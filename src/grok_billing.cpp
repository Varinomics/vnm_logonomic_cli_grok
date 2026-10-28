#include "grok_billing.h"
#include "grok_billing_http.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace grok {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

using addon::json::member;

// Only the established weekly paid-plan case implies zero when usage is absent.
constexpr std::string_view k_weekly_period = "USAGE_PERIOD_TYPE_WEEKLY";

struct Credential
{
    std::string              scope_key;
    std::string              bearer;
    std::string              user_id;
    std::string              principal_id;
    std::chrono::sys_time<std::chrono::milliseconds> expires_at;
};

std::optional<std::string> read_bounded(const fs::path& path, std::size_t maximum)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return std::nullopt;
    const auto size = file.tellg();
    if (size < 0 || static_cast<std::uintmax_t>(size) > maximum) return std::nullopt;
    std::string data(static_cast<std::size_t>(size), '\0');
    file.seekg(0);
    if (!file.read(data.data(), static_cast<std::streamsize>(size))) return std::nullopt;
    return data;
}

std::optional<addon::Json_value> read_json(const fs::path& path, std::size_t maximum)
{
    const auto bytes = read_bounded(path, maximum);
    if (!bytes) return std::nullopt;
    addon::Json_parse_limits limits;
    limits.max_bytes = maximum;
    return addon::parse_json(*bytes, limits, nullptr);
}

bool custom_auth_or_route(const fs::path& profile)
{
#if defined(_WIN32)
    constexpr bool windows = true;
#else
    constexpr bool windows = false;
#endif
    for (const auto& variable : addon::current_process_environment()) {
        if (is_quota_route_override(variable.first, windows)) return true;
    }
    // Configuration through which a profile names its own identity provider
    // or service route (GrokComConfig and the proxy settings of Grok's client).
    constexpr std::string_view settings[] = {
        "cli_chat_proxy_base_url", "oidc", "oauth2", "force_login_team",
        "preferred_method", "auth_provider_command", "models_base_url",
    };
    for (const char* name : {"config.toml", "managed_config.toml", "requirements.toml"}) {
        const auto path = profile / name;
        std::error_code error;
        const bool exists = fs::exists(path, error);
        if (error) return true;
        if (!exists) continue;
        const auto content = read_bounded(path, 64 * 1024);
        if (!content) return true;
        for (const auto setting : settings) {
            if (content->find(setting) != std::string::npos) return true;
        }
    }
    return false;
}

std::optional<std::chrono::sys_time<std::chrono::milliseconds>> timestamp(
    std::string_view text, std::string* submillisecond_fraction = nullptr)
{
    using namespace std::chrono;
    if (submillisecond_fraction) submillisecond_fraction->clear();
    if (text.size() < 20 || text.size() > 40 || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':') return std::nullopt;
    const auto digits = [&](std::size_t offset, std::size_t count) -> std::optional<int> {
        int value = 0;
        for (std::size_t i = offset; i < offset + count; ++i) {
            if (i >= text.size() || text[i] < '0' || text[i] > '9') return std::nullopt;
            value = value * 10 + text[i] - '0';
        }
        return value;
    };
    const auto year_value = digits(0, 4), month_value = digits(5, 2), day_value = digits(8, 2);
    const auto hour_value = digits(11, 2), minute_value = digits(14, 2), second_value = digits(17, 2);
    if (!year_value || !month_value || !day_value || !hour_value || !minute_value || !second_value ||
        *hour_value > 23 || *minute_value > 59 || *second_value > 59) return std::nullopt;
    const year_month_day day{year(*year_value), month(static_cast<unsigned>(*month_value)),
        std::chrono::day(static_cast<unsigned>(*day_value))};
    if (!day.ok()) return std::nullopt;
    std::size_t position = 19;
    int fraction_ms = 0;
    if (position < text.size() && text[position] == '.') {
        ++position;
        const auto fraction_start = position;
        int place = 100;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
            const int digit = text[position] - '0';
            if (place == 0 && submillisecond_fraction) {
                submillisecond_fraction->push_back(text[position]);
            }
            fraction_ms += digit * place;
            place /= 10;
            ++position;
        }
        if (position == fraction_start) return std::nullopt;
    }
    if (submillisecond_fraction) {
        while (!submillisecond_fraction->empty() && submillisecond_fraction->back() == '0') {
            submillisecond_fraction->pop_back();
        }
    }
    int offset_minutes = 0;
    if (position + 1 == text.size() && text[position] == 'Z') {}
    else
    if (position + 6 == text.size() && (text[position] == '+' || text[position] == '-') &&
        text[position + 3] == ':')
    {
        const auto hours = digits(position + 1, 2), minutes = digits(position + 4, 2);
        if (!hours || !minutes || *hours > 23 || *minutes > 59) return std::nullopt;
        offset_minutes = (*hours * 60 + *minutes) * (text[position] == '+' ? 1 : -1);
    }
    else return std::nullopt;
    return sys_days(day) + hours(*hour_value) + minutes(*minute_value) + seconds(*second_value) -
        minutes(offset_minutes) + milliseconds(fraction_ms);
}

std::optional<Credential> parse_credential(const addon::Json_value& auth)
{
    if (!auth.is_object()) return std::nullopt;
    constexpr std::string_view scope = "https://auth.x.ai::b1a00492-073a-47ea-816f-4c329264a828";
    const auto* entry = auth.find(scope);
    if (!entry || !entry->is_object()) return std::nullopt;
    const auto& bearer = member(*entry, "key");
    const auto& user_id = member(*entry, "user_id");
    const auto& principal_type = member(*entry, "principal_type");
    const auto& principal_id = member(*entry, "principal_id");
    const auto& refresh = member(*entry, "refresh_token");
    const auto& issuer = member(*entry, "oidc_issuer");
    const auto& client = member(*entry, "oidc_client_id");
    const auto& mode = member(*entry, "auth_mode");
    const auto& expiry = member(*entry, "expires_at");
    if (!bearer.is_string() || bearer.as_string().empty() ||
        !user_id.is_string() || user_id.as_string().empty() ||
        !principal_type.is_string() || principal_type.as_string() != "User" ||
        !principal_id.is_string() || principal_id.as_string().empty() ||
        !refresh.is_string() || refresh.as_string().empty() ||
        !issuer.is_string() || issuer.as_string() != "https://auth.x.ai" ||
        !client.is_string() || client.as_string() != "b1a00492-073a-47ea-816f-4c329264a828" ||
        !mode.is_string() || mode.as_string() != "oidc" || !expiry.is_string())
        return std::nullopt;
    const auto expires_at = timestamp(expiry.as_string());
    if (!expires_at) return std::nullopt;
    return Credential{std::string(scope), bearer.as_string(), user_id.as_string(),
        principal_id.as_string(), *expires_at};
}

std::string read_version(const fs::path& profile)
{
    const auto value = read_json(profile / "version.json", 4096);
    if (!value || !value->is_object()) return {};
    const auto& version = member(*value, "version");
    if (!version.is_string() || version.as_string().size() > 64) return {};
    const auto& text = version.as_string();
    if (!std::all_of(text.begin(), text.end(), [](unsigned char byte) {
            return (byte >= '0' && byte <= '9') || byte == '.' || byte == '-';
        })) return {};
    return text;
}

addon::Quota_inspect_result status(addon::Quota_status state)
{
    addon::Quota_inspect_result result;
    result.complete = true;
    addon::Quota_observation observation;
    observation.state = state;
    result.observation = std::move(observation);
    return result;
}

std::optional<addon::Json_value> response_json(
    const std::optional<Billing_http_response>& response, addon::Quota_inspect_result& failure)
{
    if (!response) {
        failure = addon::make_quota_failure(addon::Quota_failure::REQUEST_FAILED);
        return std::nullopt;
    }
    const addon::Quota_diagnostic diagnostic{addon::Quota_failure_stage::HTTP_RESPONSE, response->status};
    if (response->status != 200) {
        const auto reason = response->status == 401
            ? addon::Quota_failure::AUTHENTICATION_REQUIRED
            : addon::Quota_failure::REQUEST_REJECTED;
        failure = addon::make_quota_failure(reason, {}, diagnostic);
        return std::nullopt;
    }
    addon::Json_parse_limits limits;
    limits.max_bytes = 64 * 1024;
    auto value = addon::parse_json(response->body, limits, nullptr);
    if (!value || !value->is_object()) {
        failure = addon::make_quota_failure(addon::Quota_failure::INVALID_RESPONSE, {}, diagnostic);
        return std::nullopt;
    }
    return value;
}

bool paid_supergrok(std::string_view tier)
{
    constexpr std::string_view prefix = "SuperGrok";
    if (tier.size() < prefix.size()) return false;
    const bool matches = std::equal(prefix.begin(), prefix.end(), tier.begin(),
        [](char left, char right) {
            return std::tolower(static_cast<unsigned char>(left)) ==
                std::tolower(static_cast<unsigned char>(right));
        });
    return matches && (tier.size() == prefix.size() || tier[prefix.size()] == ' ');
}

bool no_monthly_cap(const addon::Json_value& config)
{
    const auto* limit = config.find("monthlyLimit");
    if (!limit) return true;
    if (!limit->is_object()) return false;
    const auto* value = limit->find("val");
    return !value || (value->is_number() && value->as_number() == 0.0);
}

} // namespace

addon::Quota_inspect_result fetch_billing_quota(std::string_view profile_path)
{
    using addon::Quota_status;
    using addon::Quota_failure;
    // UNAVAILABLE means this profile has no quota the helper can read; FAILED
    // means this attempt could not read it. Only a failure keeps the host's
    // last figure on display, greyed as out of date.
    const auto profile = addon::path_from_utf8(profile_path);
    if (!profile.is_absolute() || custom_auth_or_route(profile)) return status(Quota_status::UNAVAILABLE);
    std::error_code file_error;
    if (!fs::exists(profile / "auth.json", file_error))
        return status(file_error ? Quota_status::FAILED : Quota_status::NOT_LOGGED_IN);
    const auto auth = read_json(profile / "auth.json", 64 * 1024);
    if (!auth) {
        return status(Quota_status::FAILED);
    }
    const auto credential = parse_credential(*auth);
    if (!credential) return status(Quota_status::UNAVAILABLE);
    // An expired access token is not refreshed here. Grok rotates its refresh
    // token under its own auth.json lock, and a second refresher would spend a
    // token Grok still holds. Grok refreshes it the next time it runs.
    if (std::chrono::system_clock::now() >= credential->expires_at) {
        return addon::make_quota_failure(Quota_failure::CREDENTIALS_EXPIRED);
    }
    const auto version = read_version(profile);
    addon::Quota_inspect_result failure;
    const auto billing = response_json(get_billing_resource("/v1/billing?format=credits",
        credential->bearer, credential->user_id, version), failure);
    if (!billing) {
        return failure;
    }
    const addon::Quota_diagnostic billing_diagnostic{addon::Quota_failure_stage::HTTP_RESPONSE, 200};
    const auto& config = member(*billing, "config");
    if (config.is_null()) return status(Quota_status::UNAVAILABLE);
    if (!config.is_object()) {
        return addon::make_quota_failure(Quota_failure::INVALID_RESPONSE, {}, billing_diagnostic);
    }
    // Grok's client reads an explicit null percentage as an omitted one.
    const auto* reported_percent = config.find("creditUsagePercent");
    if (reported_percent && reported_percent->is_null()) reported_percent = nullptr;
    if (reported_percent && (!reported_percent->is_number() || !std::isfinite(reported_percent->as_number()))) {
        return addon::make_quota_failure(Quota_failure::INVALID_RESPONSE, {}, billing_diagnostic);
    }

    const auto settings = response_json(get_billing_resource("/v1/settings",
        credential->bearer, credential->user_id, version), failure);
    std::optional<std::string> tier;
    if (settings) {
        const auto& value = member(*settings, "subscription_tier_display");
        if (value.is_string() && !value.as_string().empty() && value.as_string().size() <= 128)
            tier = value.as_string();
    }

    const auto& period = member(config, "currentPeriod");
    if (!period.is_object()) {
        return addon::make_quota_failure(Quota_failure::INVALID_RESPONSE, {}, billing_diagnostic);
    }
    const auto& type = member(period, "type");
    const auto& start_text = member(period, "start");
    const auto& end_text = member(period, "end");
    if (!start_text.is_string() || !end_text.is_string()) {
        return addon::make_quota_failure(Quota_failure::INVALID_RESPONSE, {}, billing_diagnostic);
    }
    std::string start_fraction;
    std::string end_fraction;
    const auto start = timestamp(start_text.as_string(), &start_fraction);
    const auto end   = timestamp(end_text.as_string(), &end_fraction);
    const auto now   = std::chrono::system_clock::now();
    if (!start || !end || *start >= *end || now < *start || now >= *end) {
        return addon::make_quota_failure(Quota_failure::INVALID_RESPONSE, {}, billing_diagnostic);
    }
    // Equal submillisecond fractions cancel when measuring a period. Keep
    // them through validation so flooring the SDK reset never rounds an
    // unrepresentable duration into a whole-minute window.
    const auto duration       = *end - *start;
    const auto window_minutes = std::chrono::duration_cast<std::chrono::minutes>(duration);
    if (window_minutes.count() <= 0 || duration != window_minutes || start_fraction != end_fraction) {
        return addon::make_quota_failure(Quota_failure::INVALID_RESPONSE, {}, billing_diagnostic);
    }
    // quota@2 carries the native percentage, so usage past the allowance stays
    // above 100 and a negative value is preserved rather than saturated.
    std::optional<double> percent;
    if (reported_percent) {
        percent = reported_percent->as_number();
    }
    else {
        if (!type.is_string() || type.as_string() != k_weekly_period) {
            return status(Quota_status::UNAVAILABLE);
        }
        // The billing service writes proto3 JSON, which omits zero-valued
        // fields (Grok's own client documents this for its Cent type), and
        // Grok's pager shows an omitted percentage with no monthly limit as
        // 0% used: credit_balance_from_config in
        // crates/codegen/xai-grok-pager/src/app/effects/helpers.rs of
        // github.com/xai-org/grok-build. The helper applies that rule only
        // where zero cannot stand for an unmetered plan: a paid SuperGrok tier
        // on unified billing inside its current weekly period. Without the
        // tier the attempt could not decide.
        if (!settings) {
            return failure;
        }
        const auto& unified = member(config, "isUnifiedBillingUser");
        if (tier && paid_supergrok(*tier) && no_monthly_cap(config) &&
            unified.is_boolean() && unified.as_boolean() &&
            duration >= std::chrono::days(6) && duration <= std::chrono::days(8))
        {
            percent = 0.0;
        }
    }
    if (!percent) return status(Quota_status::UNAVAILABLE);
    // The figures belong to the credential that fetched them. If Grok replaced
    // it meanwhile, the next refresh reads the current one.
    const auto current_auth = read_json(profile / "auth.json", 64 * 1024);
    const auto current_credential = current_auth ? parse_credential(*current_auth) : std::nullopt;
    if (!current_credential || current_credential->scope_key != credential->scope_key ||
        current_credential->bearer != credential->bearer ||
        current_credential->user_id != credential->user_id ||
        current_credential->principal_id != credential->principal_id ||
        std::chrono::system_clock::now() >= current_credential->expires_at)
        return status(Quota_status::FAILED);

    auto result = status(Quota_status::READY);
    addon::Quota_window window;
    window.used_percent   = *percent;
    window.window_minutes = static_cast<std::uint64_t>(window_minutes.count());
    window.resets_at_ms   = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end->time_since_epoch()).count());
    result.observation->primary = std::move(window);
    result.observation->plan = std::move(tier);
    return result;
}

} // namespace grok
