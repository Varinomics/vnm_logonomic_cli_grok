#include "grok_billing.h"
#include "grok_billing_http.h"

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/quota.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Fixtures follow the observed fractional weekly period and the response
// shapes documented by Grok's own open-source client (github.com/xai-org/
// grok-build): the credits config of crates/codegen/xai-grok-shell/src/
// extensions/billing.rs (its billing_config_deserializes_credits_config_shape
// test), the settings tier of src/remote/client.rs, and the auth.json entry of
// crates/codegen/xai-grok-login/src/model.rs. Times are moved around the test
// run so that the credential and the period are current.

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Clock = std::chrono::system_clock;
using addon::Quota_status;

struct Recorded_request
{
    std::string resource;
    std::string bearer;
    std::string user_id;
    std::string client_version;
};

struct Fake_billing_service
{
    std::optional<grok::Billing_http_response> billing;
    std::optional<grok::Billing_http_response> settings;
    std::function<void()>                      while_billing;
    std::vector<Recorded_request>              requests;
};

Fake_billing_service g_service;

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string rfc3339(Clock::time_point time, std::string_view fraction = {})
{
    using namespace std::chrono;
    const auto whole = floor<seconds>(time);
    const auto day = floor<days>(whole);
    const year_month_day date{day};
    const hh_mm_ss clock{whole - day};
    char text[32];
    std::snprintf(text, sizeof(text), "%04d-%02u-%02uT%02d:%02d:%02d",
        static_cast<int>(date.year()), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
        static_cast<int>(clock.hours().count()), static_cast<int>(clock.minutes().count()),
        static_cast<int>(clock.seconds().count()));
    return std::string(text) + std::string(fraction) + "Z";
}

std::uint64_t epoch_ms(Clock::time_point time)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::floor<std::chrono::seconds>(time).time_since_epoch()).count());
}

void write_file(const fs::path& path, std::string_view text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    check(static_cast<bool>(file), "fixture file was written");
}

// An OIDC login as Grok stores it: one entry keyed by issuer and client id.
std::string auth_json(Clock::time_point expires_at, std::string_view key = "fixture-access-token",
    std::string_view principal_type = "User")
{
    return R"({"https://auth.x.ai::b1a00492-073a-47ea-816f-4c329264a828":{"key":")" + std::string(key) +
        R"(","auth_mode":"oidc","create_time":")" + rfc3339(Clock::now() - std::chrono::hours(1)) +
        R"(","user_id":"fixture-user","email":"jane@example.com","principal_type":")" +
        std::string(principal_type) + R"(","principal_id":"fixture-principal",)" +
        R"("coding_data_retention_opt_out":true,"refresh_token":"fixture-refresh-token","expires_at":")" +
        rfc3339(expires_at, ".123456789") +
        R"(","oidc_issuer":"https://auth.x.ai","oidc_client_id":"b1a00492-073a-47ea-816f-4c329264a828"}})";
}

struct Period
{
    Clock::time_point start;
    Clock::time_point end;
};

Period current_week()
{
    const auto start = std::chrono::floor<std::chrono::seconds>(Clock::now() - std::chrono::days(2));
    return {start, start + std::chrono::days(7)};
}

// The credits config of GetGrokCreditsConfig, as Grok's client documents it.
// `percent` is the creditUsagePercent member text; empty omits the member.
std::string credits_config(const Period& period, std::string_view percent,
    std::string_view period_type = "USAGE_PERIOD_TYPE_WEEKLY", std::string_view extra = {})
{
    std::string config = "{";
    if (!percent.empty()) config += R"("creditUsagePercent":)" + std::string(percent) + ",";
    config += R"("currentPeriod":{"type":")" + std::string(period_type) + R"(","start":")" +
        rfc3339(period.start) + R"(","end":")" + rfc3339(period.end) + R"("},)";
    config += R"("onDemandCap":{"val":5000},"onDemandUsed":{"val":300},"prepaidBalance":{"val":1250},)";
    config += R"("isUnifiedBillingUser":true,)";
    config += R"("productUsage":[{"product":"PRODUCT_GROK_BUILD","usagePercent":61.2}],)";
    config += R"("history":[{"period":{"type":"USAGE_PERIOD_TYPE_WEEKLY","start":")" +
        rfc3339(period.start - std::chrono::days(7)) + R"(","end":")" + rfc3339(period.start) +
        R"("},"onDemandUsed":{"val":120}}])";
    config += std::string(extra) + "}";
    return R"({"config":)" + config + "}";
}

grok::Billing_http_response ok(std::string body) { return {200, std::move(body)}; }

grok::Billing_http_response settings_tier(std::string_view tier)
{
    return ok(R"({"subscription_tier_display":")" + std::string(tier) + R"(","on_demand_enabled":true})");
}

class Profile
{
public:
    Profile()
    :
        m_path(fs::temp_directory_path() / ("grok-billing-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        fs::create_directories(m_path);
    }
    ~Profile()
    {
        std::error_code ignored;
        fs::remove_all(m_path, ignored);
    }
    const fs::path& path() const { return m_path; }
    std::string text() const { return addon::path_to_utf8(m_path); }

private:
    fs::path m_path;
};

addon::Quota_inspect_result fetch(const Profile& profile)
{
    g_service.requests.clear();
    return grok::fetch_billing_quota(profile.text());
}

Quota_status state_of(const addon::Quota_inspect_result& result)
{
    check(result.complete && result.observation, "the helper completes with one observation");
    return result.observation->state;
}

bool decodes_as_quota_2(const addon::Quota_inspect_result& result)
{
    addon::Contract_failure failure;
    return addon::decode_quota_inspect_result(addon::encode_quota_inspect_result(result, 2), true, &failure, 2)
        .has_value();
}

void failure_causes()
{
    Profile profile;
    write_file(profile.path() / "auth.json", auth_json(Clock::now() + std::chrono::hours(1)));
    g_service.settings = settings_tier("SuperGrok");
    const auto reason = [&](std::optional<grok::Billing_http_response> response) {
        g_service.billing = std::move(response);
        const auto result = fetch(profile);
        check(state_of(result) == Quota_status::FAILED && result.failure_reason.has_value(),
            "a failed billing request retains its established safe cause");
        addon::Contract_failure failure;
        const auto decoded = addon::decode_quota_inspect_result(
            addon::encode_quota_inspect_result(result, 2, true), true, &failure, 2);
        check(decoded && decoded->failure_reason == result.failure_reason,
            "the helper wire result preserves the safe cause");
        if (g_service.billing) {
            check(result.diagnostic && decoded->diagnostic &&
                decoded->diagnostic->stage == addon::Quota_failure_stage::HTTP_RESPONSE &&
                decoded->diagnostic->code == g_service.billing->status,
                "the helper retains the actual HTTP status in its developer diagnostic");
        }
        else {
            check(!decoded->diagnostic, "a transport failure does not invent an HTTP status");
        }
        return *result.failure_reason;
    };
    const auto malformed = reason(ok("<html>private response</html>"));
    const auto transport = reason(std::nullopt);
    const auto rejected  = reason(grok::Billing_http_response{503, "private service error"});
    const auto denied    = reason(grok::Billing_http_response{401, "private authentication error"});
    check(malformed != transport && malformed != rejected && malformed != denied &&
        transport != rejected && transport != denied && rejected != denied,
        "malformed data, transport failure, rejection and authentication remain distinct");
    check(malformed == addon::quota_failure_reason(addon::Quota_failure::INVALID_RESPONSE) &&
        denied == addon::quota_failure_reason(addon::Quota_failure::AUTHENTICATION_REQUIRED),
        "malformed quota data is not reported as an authentication failure");
    check(reason(grok::Billing_http_response{429, "private throttling error"}) == rejected,
        "service rejection remains distinct from authentication");
    check(malformed.find("private") == std::string::npos &&
        rejected.find("private") == std::string::npos && denied.find("private") == std::string::npos,
        "native response contents do not cross the safe cause boundary");
    write_file(profile.path() / "auth.json", auth_json(Clock::now() - std::chrono::minutes(1)));
    const auto expired = fetch(profile);
    check(expired.failure_reason && *expired.failure_reason != malformed &&
        *expired.failure_reason != transport && *expired.failure_reason != rejected && g_service.requests.empty(),
        "expired credentials retain their prerequisite failure without contacting billing");
    check(expired.failure_reason == addon::quota_failure_reason(addon::Quota_failure::CREDENTIALS_EXPIRED),
        "the caller knows that Grok must renew its expired credential");
}

void credential_parsing()
{
    Profile profile;
    g_service.billing = ok(credits_config(current_week(), "42.5"));
    g_service.settings = settings_tier("SuperGrok");

    check(state_of(fetch(profile)) == Quota_status::NOT_LOGGED_IN && g_service.requests.empty(),
        "a profile without auth.json is not logged in and sends nothing");

    write_file(profile.path() / "auth.json",
        R"({"xai::api_key":{"key":"xai-fixture","auth_mode":"api_key","create_time":"2026-01-01T00:00:00Z","user_id":"fixture-user"}})");
    check(state_of(fetch(profile)) == Quota_status::UNAVAILABLE && g_service.requests.empty(),
        "an API-key login has no first-party quota and sends nothing");

    write_file(profile.path() / "auth.json", auth_json(Clock::now() + std::chrono::hours(1), "fixture-access-token", "Team"));
    check(state_of(fetch(profile)) == Quota_status::UNAVAILABLE && g_service.requests.empty(),
        "a team principal is not read as the user's own quota");

    write_file(profile.path() / "auth.json", std::string(64 * 1024 + 1, ' '));
    check(state_of(fetch(profile)) == Quota_status::FAILED && g_service.requests.empty(),
        "an auth.json beyond its bound fails the refresh without sending a request");

    write_file(profile.path() / "auth.json", auth_json(Clock::now() - std::chrono::minutes(1)));
    check(state_of(fetch(profile)) == Quota_status::FAILED && g_service.requests.empty(),
        "an expired token fails the attempt without sending it");

    write_file(profile.path() / "auth.json", auth_json(Clock::now() + std::chrono::hours(1)));
    write_file(profile.path() / "config.toml", "cli_chat_proxy_base_url = \"https://proxy.example.com/v1\"\n");
    check(state_of(fetch(profile)) == Quota_status::UNAVAILABLE && g_service.requests.empty(),
        "a profile routed through its own service sends nothing");
    write_file(profile.path() / "config.toml",
        "[grok_com_config.oauth2]\nissuer = \"https://idp.example.com\"\nclient_id = \"example-client\"\n");
    check(state_of(fetch(profile)) == Quota_status::UNAVAILABLE && g_service.requests.empty(),
        "a profile that signs in through its own OAuth2 provider sends nothing");
    fs::remove(profile.path() / "config.toml");

    write_file(profile.path() / "version.json", R"({"version":"1.0.41"})");
    check(state_of(fetch(profile)) == Quota_status::READY && g_service.requests.size() == 2,
        "a current login fetches billing and settings");
    const auto& billing = g_service.requests.front();
    const auto& settings = g_service.requests.back();
    check(billing.resource == "/v1/billing?format=credits" && settings.resource == "/v1/settings",
        "only Grok's fixed billing and settings resources are requested");
    check(billing.bearer == "fixture-access-token" && billing.user_id == "fixture-user" &&
        billing.client_version == "1.0.41" && settings.bearer == billing.bearer &&
        settings.user_id == billing.user_id,
        "both requests carry the profile's own bearer, user and client version");
}

void response_interpretation()
{
    Profile profile;
    write_file(profile.path() / "auth.json", auth_json(Clock::now() + std::chrono::hours(1)));
    const auto week = current_week();

    g_service.billing = ok(credits_config(week, "42.5"));
    g_service.settings = settings_tier("SuperGrok");
    const auto ready = fetch(profile);
    check(state_of(ready) == Quota_status::READY && ready.observation->primary &&
        !ready.observation->secondary && ready.observation->plan == "SuperGrok",
        "the documented credits config is one weekly window with its plan");
    const auto& window = *ready.observation->primary;
    check(window.used_percent == 42.5, "the native percentage is reported");
    check(window.window_minutes == std::uint64_t{7 * 24 * 60}, "the weekly window is labelled by its length, 7d");
    check(window.resets_at_ms == epoch_ms(week.end), "the window resets at the period end");
    check(!window.label && !window.reset_text, "the host derives the window's text from its length and reset");
    check(decodes_as_quota_2(ready), "the ready observation satisfies quota@2");

    for (const auto& [text, value] : {std::pair{"-7.5", -7.5}, std::pair{"125.0", 125.0}}) {
        g_service.billing = ok(credits_config(week, text));
        const auto native = fetch(profile);
        check(state_of(native) == Quota_status::READY && native.observation->primary->used_percent == value &&
            decodes_as_quota_2(native),
            "every finite native percentage is preserved without clamping");
    }

    g_service.billing = ok(credits_config(week, ""));
    const auto omitted = fetch(profile);
    check(state_of(omitted) == Quota_status::READY && omitted.observation->primary->used_percent == 0.0 &&
        omitted.observation->primary->window_minutes == std::uint64_t{7 * 24 * 60},
        "a paid unified weekly plan without a percentage has used none of it");
    g_service.billing = ok(credits_config(week, "null"));
    check(state_of(fetch(profile)) == Quota_status::READY, "an explicit null percentage reads as omitted");

    g_service.settings = settings_tier("Free");
    check(state_of(fetch(profile)) == Quota_status::UNAVAILABLE,
        "an omitted percentage on a free plan is not read as zero");
    g_service.settings = settings_tier("SuperGrok Heavy");
    g_service.billing = ok(credits_config(week, "", "USAGE_PERIOD_TYPE_WEEKLY", R"(,"monthlyLimit":{"val":2000})"));
    check(state_of(fetch(profile)) == Quota_status::UNAVAILABLE,
        "an omitted percentage beside a monthly limit is not read as zero");
    g_service.billing = ok(credits_config(week, ""));
    g_service.settings = grok::Billing_http_response{503, "{}"};
    check(state_of(fetch(profile)) == Quota_status::FAILED,
        "an omitted percentage without the plan tier fails the attempt");
    g_service.billing = ok(credits_config(week, "42.5"));
    const auto without_plan = fetch(profile);
    check(state_of(without_plan) == Quota_status::READY && !without_plan.observation->plan,
        "a reported percentage does not depend on the plan tier");
    g_service.settings = settings_tier("SuperGrok");

    g_service.billing = std::nullopt;
    check(state_of(fetch(profile)) == Quota_status::FAILED, "an unreachable billing service fails the attempt");
    for (const long status : {401L, 429L, 503L}) {
        g_service.billing = grok::Billing_http_response{status, R"({"error":"fixture"})"};
        check(state_of(fetch(profile)) == Quota_status::FAILED, "a billing error status fails the attempt");
    }
    g_service.billing = ok("<html>maintenance</html>");
    check(state_of(fetch(profile)) == Quota_status::FAILED, "an unreadable billing reply fails the attempt");
    g_service.billing = ok(credits_config(week, R"("42.5")"));
    check(state_of(fetch(profile)) == Quota_status::FAILED, "a non-numeric percentage fails the attempt");

    g_service.billing = ok(R"({"config":null})");
    check(state_of(fetch(profile)) == Quota_status::UNAVAILABLE, "a reply without a credits config meters nothing");
    g_service.billing = ok("{}");
    check(state_of(fetch(profile)) == Quota_status::UNAVAILABLE, "an absent credits config meters nothing");
    for (const auto malformed : {R"({"config":"invalid"})", R"({"config":{}})",
             R"({"config":{"currentPeriod":"invalid"}})"})
    {
        g_service.billing = ok(malformed);
        const auto failed = fetch(profile);
        check(state_of(failed) == Quota_status::FAILED && decodes_as_quota_2(failed),
            "an uninterpretable credits period fails instead of reporting absent quotas");
    }
    const Period ended{week.start - std::chrono::days(14), week.start - std::chrono::days(7)};
    g_service.billing = ok(credits_config(ended, "42.5"));
    check(state_of(fetch(profile)) == Quota_status::FAILED, "a stale billing period fails the refresh");

    g_service.billing = ok(credits_config(week, "42.5"));
    g_service.while_billing = [&] {
        write_file(profile.path() / "auth.json",
            auth_json(Clock::now() + std::chrono::hours(2), "fixture-rotated-token"));
    };
    check(state_of(fetch(profile)) == Quota_status::FAILED,
        "figures fetched with a credential Grok replaced meanwhile are not reported");
    g_service.while_billing = nullptr;
}

void native_fractional_weekly_period()
{
    Profile profile;
    write_file(profile.path() / "auth.json", auth_json(Clock::now() + std::chrono::hours(1)));
    const auto week = current_week();
    // The observed SuperGrok Lite reply carries .193919 at both weekly
    // boundaries. Move the dates with the test clock, preserving its precision.
    auto billing = credits_config(week, "");
    for (const auto boundary : {week.start, week.end}) {
        const auto position = billing.find(rfc3339(boundary));
        billing.replace(position, rfc3339(boundary).size(), rfc3339(boundary, ".193919"));
    }
    g_service.billing = ok(billing);
    g_service.settings = settings_tier("SuperGrok Lite");
    const auto result = fetch(profile);
    check(state_of(result) == Quota_status::READY && result.observation->primary,
        "the observed microsecond-precision weekly period supplies a quota");
    check(result.observation->primary->used_percent == 0.0 &&
        result.observation->primary->window_minutes == std::uint64_t{7 * 24 * 60} &&
        result.observation->primary->resets_at_ms == epoch_ms(week.end) + 193 &&
        result.observation->plan == "SuperGrok Lite" && decodes_as_quota_2(result),
        "the weekly quota keeps its duration and percentage and floors its reset to SDK milliseconds");
    const auto end_position = billing.find(rfc3339(week.end, ".193919"));
    billing.replace(end_position, rfc3339(week.end, ".193919").size(), rfc3339(week.end, ".193919000"));
    g_service.billing = ok(billing);
    check(state_of(fetch(profile)) == Quota_status::READY,
        "equivalent fractional precision preserves the exact weekly duration");
    const auto changed_position = billing.find(rfc3339(week.end, ".193919000"));
    billing.replace(changed_position, rfc3339(week.end, ".193919000").size(), rfc3339(week.end, ".193918"));
    g_service.billing = ok(billing);
    const auto unequal = fetch(profile);
    check(state_of(unequal) == Quota_status::FAILED && decodes_as_quota_2(unequal),
        "different submillisecond fractions do not become a whole-minute period after rounding");
}

void non_weekly_periods()
{
    Profile profile;
    write_file(profile.path() / "auth.json", auth_json(Clock::now() + std::chrono::hours(1)));
    g_service.settings = settings_tier("SuperGrok");
    const auto start = current_week().start;
    unsigned failures = 0;
    // UsagePeriod in grok-build's billing.rs supplies RFC3339 boundaries;
    // credit_bar.rs tests the MONTHLY enum. A calendar month is not a fixed
    // number of minutes. Shift each fixture around now as for the weekly one.
    for (const int days : {28, 29, 30, 31}) {
        for (const auto& [text, percent] : {std::pair{"-7.5", -7.5}, std::pair{"125.0", 125.0}}) {
            const Period month{start, start + std::chrono::days(days)};
            g_service.billing = ok(credits_config(month, text, "USAGE_PERIOD_TYPE_MONTHLY"));
            const auto result = fetch(profile);
            const bool correct =
                state_of(result) == Quota_status::READY && result.observation->primary &&
                result.observation->primary->window_minutes == std::uint64_t(days * 24 * 60) &&
                result.observation->primary->resets_at_ms == epoch_ms(month.end) &&
                result.observation->primary->used_percent == percent &&
                !result.observation->primary->label && !result.observation->primary->reset_text &&
                decodes_as_quota_2(result);
            if (!correct) {
                std::cerr << "monthly fixture " << days << " days, " << percent << "% failed\n";
                ++failures;
            }
        }
    }
    const Period month{start, start + std::chrono::days(30)};
    auto fractional = credits_config(month, "42.5", "USAGE_PERIOD_TYPE_MONTHLY");
    for (const auto boundary : {month.start, month.end}) {
        const auto position = fractional.find(rfc3339(boundary));
        fractional.replace(position, rfc3339(boundary).size(), rfc3339(boundary, ".125"));
    }
    g_service.billing = ok(fractional);
    const auto precise = fetch(profile);
    if (state_of(precise) != Quota_status::READY || !precise.observation->primary ||
        precise.observation->primary->window_minutes != std::uint64_t{30 * 24 * 60} ||
        precise.observation->primary->resets_at_ms != epoch_ms(month.end) + 125)
    {
        std::cerr << "monthly millisecond reset fixture failed\n";
        ++failures;
    }
    // A type name is not needed to measure a period whose boundaries are
    // supplied. This also covers exact minute lengths that are not whole days.
    const Period other{start, start + std::chrono::minutes(43201)};
    g_service.billing = ok(credits_config(other, "42.5", "USAGE_PERIOD_TYPE_UNSPECIFIED"));
    const auto measured = fetch(profile);
    if (state_of(measured) != Quota_status::READY || !measured.observation->primary ||
        measured.observation->primary->window_minutes != std::uint64_t{43201})
    {
        std::cerr << "non-weekly exact minute fixture failed\n";
        ++failures;
    }
    // The old weekly constant also hid invalid or unrepresentable intervals.
    for (const Period invalid : {
            Period{month.end + std::chrono::days(1), month.end}, Period{month.end, month.end},
            Period{month.end, month.end + std::chrono::days(7)},
            Period{start, month.end + std::chrono::seconds(1)}})
    {
        g_service.billing = ok(credits_config(invalid, "42.5"));
        if (state_of(fetch(profile)) != Quota_status::FAILED) {
            std::cerr << "invalid or fractional-minute period fixture failed\n";
            ++failures;
        }
    }
    auto submillisecond = credits_config(month, "42.5");
    const auto end_position = submillisecond.find(rfc3339(month.end));
    submillisecond.replace(end_position, rfc3339(month.end).size(), rfc3339(month.end, ".0001"));
    g_service.billing = ok(submillisecond);
    if (state_of(fetch(profile)) != Quota_status::FAILED) {
        std::cerr << "submillisecond reset fixture failed\n";
        ++failures;
    }
    for (const auto percent : {"", "null"}) {
        g_service.billing = ok(credits_config(month, percent, "USAGE_PERIOD_TYPE_MONTHLY"));
        g_service.settings = settings_tier("SuperGrok");
        const auto with_settings = state_of(fetch(profile));
        g_service.settings = grok::Billing_http_response{503, "{}"};
        const auto without_settings = state_of(fetch(profile));
        if (with_settings != Quota_status::UNAVAILABLE || without_settings != Quota_status::UNAVAILABLE) {
            std::cerr << "monthly missing percentage '" << percent << "' depends on settings failure\n";
            ++failures;
        }
    }
    check(failures == 0, "period fixtures preserve exact length, reset and native percentage");
}

} // namespace

namespace grok {

// The HTTPS transport is replaced at link time; everything above it is the
// helper's own code.
std::optional<Billing_http_response> get_billing_resource(
    std::string_view resource, std::string_view bearer,
    std::string_view user_id, std::string_view client_version)
{
    g_service.requests.push_back(
        {std::string(resource), std::string(bearer), std::string(user_id), std::string(client_version)});
    if (resource == "/v1/billing?format=credits") {
        if (g_service.while_billing) g_service.while_billing();
        return g_service.billing;
    }
    if (resource == "/v1/settings") return g_service.settings;
    return std::nullopt;
}

} // namespace grok

int main()
{
    try {
        failure_causes();
        credential_parsing();
        response_interpretation();
        native_fractional_weekly_period();
        non_weekly_periods();
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
