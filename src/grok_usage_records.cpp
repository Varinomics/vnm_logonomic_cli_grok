#include <logonomic_cli_addon/package_source.h>
#include "grok_usage_records.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <fstream>
#include <set>
#include <utility>

namespace grok {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
constexpr std::uint64_t k_max_exact_count = 9007199254740991;

std::string path_text(const fs::path& path)
{
    const auto bytes = path.u8string();
    return std::string(bytes.begin(), bytes.end());
}

const addon::Json_value& member(const addon::Json_value& object, std::string_view name)
{
    static const addon::Json_value absent;
    const auto* value = object.find(name);
    return value ? *value : absent;
}

std::string text(const addon::Json_value& object, std::string_view name)
{
    const auto& value = member(object, name);
    return value.is_string() ? value.as_string() : std::string{};
}

std::optional<std::uint64_t> count(const addon::Json_value& value)
{
    if (!value.is_number()) {
        return std::nullopt;
    }
    const double number = value.as_number();
    if (!std::isfinite(number) || number < 0 || number > k_max_exact_count || std::floor(number) != number) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(number);
}

std::optional<std::int64_t> timestamp(std::string_view iso)
{
    // Grok persists RFC 3339; explicit offsets do not depend on local DST.
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (iso.size() < 19 || std::sscanf(std::string(iso.substr(0, 19)).c_str(),
            "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6)
    {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{year},
        std::chrono::month{static_cast<unsigned>(month)}, std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok() || year < 1 || year > 9999 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59)
    {
        return std::nullopt;
    }
    std::size_t offset = 19;
    std::int64_t milliseconds = 0;
    if (offset < iso.size() && iso[offset] == '.') {
        ++offset;
        const auto begin = offset;
        while (offset < iso.size() && iso[offset] >= '0' && iso[offset] <= '9') {
            if (offset - begin < 3) {
                milliseconds = milliseconds * 10 + (iso[offset] - '0');
            }
            ++offset;
        }
        if (offset == begin) {
            return std::nullopt;
        }
        for (auto digits = offset - begin; digits < 3; ++digits) milliseconds *= 10;
    }
    if (offset == iso.size()) {
        std::tm local{};
        local.tm_year = year - 1900;
        local.tm_mon = month - 1;
        local.tm_mday = day;
        local.tm_hour = hour;
        local.tm_min = minute;
        local.tm_sec = second;
        local.tm_isdst = -1;
        const auto seconds = std::mktime(&local);
        if (seconds == static_cast<std::time_t>(-1)) return std::nullopt;
        return static_cast<std::int64_t>(seconds) * 1000 + milliseconds;
    }
    int zone_minutes = 0;
    if (offset < iso.size() && iso[offset] == 'Z' && offset + 1 == iso.size()) {
        ++offset;
    }
    else
    if (offset + 6 == iso.size() && (iso[offset] == '+' || iso[offset] == '-') && iso[offset + 3] == ':') {
        int zone_hour = 0, zone_minute = 0;
        if (std::sscanf(std::string(iso.substr(offset + 1)).c_str(), "%2d:%2d", &zone_hour, &zone_minute) != 2 ||
            zone_hour > 23 || zone_minute > 59)
        {
            return std::nullopt;
        }
        zone_minutes = (zone_hour * 60 + zone_minute) * (iso[offset] == '-' ? -1 : 1);
        offset = iso.size();
    }
    else {
        return std::nullopt;
    }
    if (offset != iso.size()) {
        return std::nullopt;
    }
    const std::int64_t days = std::chrono::sys_days(date).time_since_epoch().count();
    return ((days * 24 + hour) * 3600 + minute * 60 + second - zone_minutes * 60) * 1000 + milliseconds;
}


} // namespace

std::optional<std::string> repository_for_cwd(std::string_view cwd)
{
    fs::path candidate = logonomic::cli_addon::path_from_utf8(cwd).lexically_normal();
    std::error_code error;
    if (!candidate.is_absolute() || !fs::is_directory(candidate, error)) return std::nullopt;
    while (!candidate.empty()) {
        const auto marker = candidate / ".git";
        if (fs::is_directory(marker, error) || fs::is_regular_file(marker, error)) return path_text(candidate);
        const auto parent = candidate.parent_path();
        if (parent == candidate) break;
        candidate = parent;
    }
    return std::nullopt;
}

void refresh_repository(Session_usage_record* record)
{
    const auto path = repository_for_cwd(record->recorded_cwd);
    record->usage.repository_path.reset();
    addon::Usage_repository repository;
    if (path) repository.allocations.push_back({*path, record->usage.total_tokens});
    else repository.unattributed_tokens = record->usage.total_tokens;
    record->usage.repository_usage = std::move(repository);
}

bool read_session_usage(std::istream& input, const fs::path& path,
    const std::function<bool(const Session_usage_record&)>& emit,
    const std::function<bool()>& cancelled, std::string* error)
{
    const auto fail = [error](std::string reason) { *error = std::move(reason); return false; };
    const auto read_document = [&](std::istream& stream) -> std::optional<addon::Json_value> {
        constexpr std::size_t k_document_bytes = 16 * 1024 * 1024;
        std::string bytes;
        std::array<char, 65536> chunk{};
        while (stream.read(chunk.data(), chunk.size()) || stream.gcount() > 0) {
            if (cancelled()) { fail("cancelled"); return {}; }
            if (static_cast<std::size_t>(stream.gcount()) > k_document_bytes - bytes.size()) {
                fail("usage_document_too_large"); return {};
            }
            bytes.append(chunk.data(), static_cast<std::size_t>(stream.gcount()));
        }
        if (stream.bad()) { fail("usage_read_failed"); return {}; }
        addon::Json_parse_limits limits;
        limits.max_bytes = k_document_bytes;
        const auto value = addon::parse_json(bytes, limits, nullptr);
        if (!value || !value->is_object()) { fail("usage_document_invalid"); return {}; }
        return value;
    };
    const auto document = read_document(input);
    if (!document) return false;
    std::ifstream summary_input(path.parent_path() / "summary.json", std::ios::binary);
    if (!summary_input) return fail("usage_summary_unavailable");
    const auto summary = read_document(summary_input);
    if (!summary) return false;
    const auto kind = text(*summary, "session_kind");
    // The parent billing ledger already includes these child calls.
    if (kind == "subagent" || kind == "subagent_fork" || kind == "subagent_resume") return true;
    const auto& turns = member(*document, "turns");
    if (!turns.is_array()) return fail("usage_turns_invalid");
    // Fork copies restamp sessionId but retain billed rows without origin IDs.
    // Neither equal values nor wall-clock timestamps prove ownership.
    if (!turns.as_array().empty() && (!member(*summary, "parent_session_id").is_null() ||
            !member(*summary, "forked_at").is_null())) return fail("usage_fork_attribution_unavailable");
    const auto session = text(*document, "sessionId");
    const auto& info = member(*summary, "info");
    if (session.empty() || session != text(info, "id") || session != path.parent_path().filename().string()) return fail("usage_session_identity_invalid");
    const auto cwd = text(info, "cwd");
    std::set<std::uint64_t> turn_numbers;
    for (const auto& row : turns.as_array()) {
        if (cancelled()) return fail("cancelled");
        const auto turn = count(member(row, "turnNumber"));
        if (!turn || *turn > UINT32_MAX || !turn_numbers.insert(*turn).second) return fail("usage_turn_identity_invalid");
        const auto instant = timestamp(text(row, "endedAt"));
        if (!instant) return fail("usage_timestamp_invalid");
        const auto seconds = static_cast<std::time_t>(*instant / 1000);
        std::tm local{};
#if defined(_WIN32)
        if (localtime_s(&local, &seconds)) return fail("usage_timestamp_invalid");
#else
        if (!localtime_r(&seconds, &local)) return fail("usage_timestamp_invalid");
#endif
        std::array<char, 11> date{};
        if (!std::strftime(date.data(), date.size(), "%Y-%m-%d", &local)) return fail("usage_timestamp_invalid");
        Session_usage_record record;
        record.usage.record_id = "grok:" + session + ":" + std::to_string(*turn);
        record.usage.session_key = session;
        record.usage.completed_date = date.data();
        record.usage.source_path = path_text(path);
        record.usage.counter_field = "turns.totalTokens";
        record.usage.source_counter_semantics = "per_event_sum";
        const auto total = count(member(row, "totalTokens"));
        if (!total) return fail("usage_total_invalid");
        record.usage.total_tokens = *total;
        const auto incomplete = member(row, "usageIsIncomplete");
        if (!incomplete.is_null() && !incomplete.is_boolean()) return fail("usage_completeness_invalid");
        const bool covered = incomplete.is_null() || !incomplete.as_boolean();
        const auto category = [&](std::string_view name, addon::Usage_amount* amount) {
            const auto& value = member(row, name);
            const auto tokens = value.is_null() ? std::optional<std::uint64_t>(0) : count(value);
            if (!tokens) return false;
            *amount = {true, *tokens, addon::Usage_field_coverage{covered ? 1u : 0u, 1, false,
                covered ? addon::Usage_support::SUPPORTED : addon::Usage_support::UNKNOWN}};
            return true;
        };
        if (!category("inputTokens", &record.usage.input) || !category("outputTokens", &record.usage.output) ||
            !category("cachedReadTokens", &record.usage.cache_read) || !category("cacheCreationTokens", &record.usage.cache_creation))
            return fail("usage_category_invalid");
        if (*record.usage.input.value > k_max_exact_count - *record.usage.output.value ||
            *record.usage.input.value + *record.usage.output.value != *total) return fail("usage_total_mismatch");
        const auto model = text(row, "primaryModelId");
        if (!model.empty()) record.usage.model = model;
        record.recorded_cwd = cwd;
        refresh_repository(&record);
        const auto raw = text(row, "endedAt");
        if (raw.size() <= addon::k_max_usage_temporal_bytes) {
            addon::Usage_temporal proof;
            proof.parent = {"native-proof", 1, "native-proof", std::nullopt};
            proof.portions.push_back({"instant", raw, {}, {}, 1});
            if (addon::decode_usage_temporal(addon::encode_usage_temporal(proof), nullptr)) record.instant = raw;
        }
        if (!emit(record)) return false;
    }
    return true;
}

} // namespace grok
