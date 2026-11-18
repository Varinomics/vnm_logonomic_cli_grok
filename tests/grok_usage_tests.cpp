#include "grok_usage.h"
#include "grok_usage_records.h"

#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/package_source.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <chrono>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
namespace addon = logonomic::cli_addon;

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void write(const fs::path& path, std::string_view bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    check(static_cast<bool>(output), "fixture write failed");
}

std::string summary(std::string_view kind = {}, bool fork = false, std::string_view session_id = "session-a")
{
    auto result = addon::Json_value::object();
    auto info = addon::Json_value::object();
    info.set("id", addon::Json_value(std::string(session_id)));
    info.set("cwd", addon::Json_value(""));
    result.set("info", info);
    if (!kind.empty()) result.set("session_kind", addon::Json_value(kind));
    if (fork) result.set("parent_session_id", addon::Json_value("parent-session"));
    return addon::serialize_json(result);
}

std::string usage(int input = 20, bool incomplete = false, std::string_view session_id = "session-a")
{
    auto row = addon::Json_value::object();
    row.set("turnNumber", addon::Json_value(1));
    row.set("endedAt", addon::Json_value("2026-09-13T12:00:00Z"));
    row.set("inputTokens", addon::Json_value(input));
    row.set("outputTokens", addon::Json_value(5));
    row.set("cachedReadTokens", addon::Json_value(7));
    row.set("cacheCreationTokens", addon::Json_value(3));
    row.set("reasoningTokens", addon::Json_value(2));
    row.set("totalTokens", addon::Json_value(input + 5));
    row.set("usageIsIncomplete", addon::Json_value(incomplete));
    auto rows = addon::Json_value::array();
    rows.push_back(row);
    auto result = addon::Json_value::object();
    result.set("sessionId", addon::Json_value(std::string(session_id)));
    result.set("session", row); // The session aggregate must never be added again.
    result.set("turns", rows);
    return addon::serialize_json(result);
}

void select_cache(addon::Usage_scan_request& request, const fs::path& storage,
    const char* name, std::optional<addon::Usage_cache_reference> accepted = std::nullopt)
{
    const auto candidate = storage / name;
    check(fs::create_directory(candidate), "allocate an exclusive current cache candidate");
    request.context.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(storage)};
    request.snapshot_id.reset();
    request.cursor.reset();
    request.cache_selection = addon::Usage_cache_selection{std::move(accepted), addon::path_to_utf8(candidate)};
}

void retargeted_source(const fs::path& root)
{
    const auto profile = root / "linked-profile";
    const auto storage = root / "linked-storage";
    const auto left = root / "left-sessions";
    const auto right = root / "right-sessions";
    const auto first = left / "workspace" / "session-a";
    const auto second = right / "workspace" / "session-b";
    fs::create_directory(profile);
    fs::create_directory(storage);
    fs::create_directories(first);
    fs::create_directories(second);
    write(first / "summary.json", summary());
    write(first / "usage.json", usage(20));
    write(second / "summary.json", summary({}, false, "session-b"));
    write(second / "usage.json", usage(30, false, "session-b"));
    const auto link = profile / "sessions";
    std::error_code link_error;
    fs::create_directory_symlink(left, link, link_error);
#if defined(_WIN32)
    if (link_error.value() == ERROR_PRIVILEGE_NOT_HELD || link_error == std::errc::operation_not_permitted) return;
#endif
    check(!link_error, "create a native source link for cache retention");
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{2, "fixture", addon::path_to_utf8(profile)};
    std::string error;
    select_cache(request, storage, "before-retarget");
    const auto initial = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(initial && initial->cache_candidate && initial->records.size() == 1, error.c_str());
    fs::path retained_file;
    for (const auto& entry : fs::directory_iterator(storage / "before-retarget")) {
        if (entry.path().extension() == ".cache") retained_file = entry.path().filename();
    }
    check(!retained_file.empty(), "the accepted source has a retained cache file");
    fs::remove(link);
    fs::create_directory_symlink(right, link);
    select_cache(request, storage, "after-retarget", initial->cache_candidate);
    const auto next = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(next && next->records.size() == 2 && next->records[0].total_tokens == 25 &&
        next->records[1].total_tokens == 35 && fs::is_regular_file(storage / "after-retarget" / retained_file),
        "a retargeted directory keeps the recorded source under its original canonical identity and counts its replacement separately");
}

void exercise(const fs::path& root)
{
    const auto profile = root / "profile";
    const auto session = profile / "sessions" / "cwd" / "session-a";
    const auto storage = root / "storage";
    fs::create_directories(session);
    fs::create_directory(storage);
    write(session / "summary.json", summary());
    write(session / "usage.json", usage());
    const auto nested = session / "artifacts" / "session-a";
    fs::create_directories(nested);
    write(nested / "summary.json", summary());
    write(nested / "usage.json", usage(900));
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{1, "fixture"};
    std::string error;
    select_cache(request, storage, "first");
    const auto first_request = request;
    const auto first = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(first.has_value(), error.c_str());
    check(first->records.size() == 1, "one native turn produces one record");
    const auto& row = first->records.front();
    check(row.total_tokens == 25 && row.input.value == 20 && row.output.value == 5,
        "cache and reasoning subsets do not inflate input plus output");
    check(row.cache_read.value == 7 && row.cache_creation.value == 3, "cache categories retained");
    check(row.source_counter_semantics == "per_event_sum",
        "native turn deltas retain per-event source semantics");
    addon::Contract_failure failure;
    check(addon::decode_usage_scan_result(addon::encode_usage_scan_result(*first, 3), &failure, 3).has_value(),
        "native usage page satisfies the host usage-scan contract");

    write(session / "summary.json", summary("fork", true));
    select_cache(request, storage, "fork-refusal", first->cache_candidate);
    check(!grok::scan_usage(request, profile, storage, [] { return false; }, &error) &&
        error == "usage_fork_attribution_unavailable", "copied fork fails even when usage fingerprint is unchanged");
    auto replay = first_request;
    replay.snapshot_id = first->snapshot_id;
    replay.cursor = "0";
    const auto prior = grok::scan_usage(replay, profile, storage, [] { return false; }, &error);
    check(prior && prior->records.front().total_tokens == 25, "fork failure leaves accepted immutable snapshot readable");

    write(session / "summary.json", summary());
    write(session / "usage.json", usage(30, true));
    select_cache(request, storage, "partial", first->cache_candidate);
    const auto partial = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(partial && partial->incomplete_historical_coverage, "incomplete native usage retains its coverage");
    fs::remove(session / "usage.json");
    select_cache(request, storage, "retained", partial->cache_candidate);
    const auto retained = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(retained && retained->records.front().total_tokens == 35 && retained->incomplete_historical_coverage,
        "missing source retains last known evidence and its incomplete coverage");

    write(session / "summary.json", summary("subagent_fork", true));
    std::istringstream child(usage());
    std::size_t emitted = 0;
    check(grok::read_session_usage(child, session / "usage.json", [&](const auto&) { ++emitted; return true; },
        [] { return false; }, &error) && emitted == 0, "folded child spend is not counted separately");

    write(session / "summary.json", summary());
    write(session / "usage.json", usage(300));
    grok::Usage_scan_limits limits;
    limits.candidate_records = 0;
    select_cache(request, storage, "bounded", retained->cache_candidate);
    check(!grok::scan_usage(request, profile, storage, [] { return false; }, &error, limits) &&
        error == "usage_candidate_budget_exceeded", "candidate bound rejects source before publication");
    select_cache(request, storage, "cancelled", retained->cache_candidate);
    check(!grok::scan_usage(request, profile, storage, [] { return true; }, &error) && error == "cancelled",
        "production cancellation interrupts before writing candidate data");

    const auto case_profile = root / "case-profile";
    const auto case_session = case_profile / "sessions" / "cwd" / "session-a";
    const auto case_storage = root / "case-storage";
    fs::create_directories(case_session);
    fs::create_directory(case_storage);
    write(case_session / "summary.json", summary());
    write(case_session / "USAGE.JSON", usage());
    const bool native_lookup_exists = fs::is_regular_file(case_session / "usage.json");
    select_cache(request, case_storage, "case-sensitive");
    const auto case_page = grok::scan_usage(request, case_profile, case_storage, [] { return false; }, &error);
    check(case_page && case_page->records.size() == (native_lookup_exists ? 1u : 0u),
        "discovery uses exact native path with filesystem case semantics");
}

void interrupted_refresh(const fs::path& root)
{
    const auto profile = root / "recovery-profile";
    const auto session = profile / "sessions" / "cwd" / "session-a";
    const auto storage = root / "recovery-storage";
    fs::create_directories(session);
    fs::create_directory(storage);
    write(session / "summary.json", summary());
    write(session / "usage.json", usage());
    addon::Usage_scan_request request;
    std::string error;
    select_cache(request, storage, "accepted");
    const auto accepted = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(accepted && accepted->records.size() == 1 && accepted->records.front().total_tokens == 25,
        "recovery fixture has accepted history");

    write(session / "usage.json", usage(300));
    bool interrupted = false;
    const auto cancel_after_refresh = [
            &storage,
            &interrupted
        ]()
        {
            for (const auto& entry : fs::recursive_directory_iterator(storage)) {
                if (entry.path().extension() != ".cache") {
                    continue;
                }
                std::ifstream input(entry.path(), std::ios::binary);
                std::string header, record;
                if (!std::getline(input, header) || !std::getline(input, record)) {
                    continue;
                }
                const auto parsed = addon::parse_json(record, {}, nullptr);
                const auto* usage_record = parsed ? parsed->find("usage") : nullptr;
                const auto* tokens = usage_record ? usage_record->find("total_tokens") : nullptr;
                if (tokens && tokens->is_integer() && tokens->as_integer() == 305) {
                    interrupted = true;
                    return true;
                }
            }
            return false;
        };
    select_cache(request, storage, "interrupted", accepted->cache_candidate);
    check(!grok::scan_usage(request, profile, storage, cancel_after_refresh, &error) &&
        interrupted && error == "cancelled", "interrupt after source artifact replacement before scan acceptance");
    fs::remove(session / "usage.json");
    select_cache(request, storage, "retained", accepted->cache_candidate);
    const auto retained = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(retained && retained->records.size() == 1 && retained->records.front().total_tokens == 25,
        "source deletion after rejected refresh retains the accepted generation");

    const auto second_session = profile / "sessions" / "cwd" / "session-b";
    fs::create_directory(second_session);
    write(second_session / "summary.json", summary({}, false, "session-b"));
    write(second_session / "usage.json", usage(50, false, "session-b"));
    write(session / "usage.json", usage(300));
    grok::Usage_scan_limits limits;
    limits.candidate_records = 1;
    select_cache(request, storage, "aggregate-refusal", retained->cache_candidate);
    check(!grok::scan_usage(request, profile, storage, [] { return false; }, &error, limits) &&
        error == "usage_candidate_budget_exceeded", "aggregate rejection occurs after individually valid refreshes");
    fs::remove(session / "usage.json");
    fs::remove(second_session / "usage.json");
    select_cache(request, storage, "after-refusal", retained->cache_candidate);
    const auto after_rejection = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(after_rejection && after_rejection->records.size() == 1 && after_rejection->records.front().total_tokens == 25,
        "rejected scan cannot retain newer rows or adopt a newly discovered source");

    write(session / "usage.json", usage(300));
    select_cache(request, storage, "updated", after_rejection->cache_candidate);
    const auto updated = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(updated && updated->records.size() == 1 && updated->records.front().total_tokens == 305,
        "complete replacement publishes new records");
    fs::remove(session / "usage.json");
    select_cache(request, storage, "updated-retained", updated->cache_candidate);
    const auto updated_retained = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(updated_retained && updated_retained->records.size() == 1 && updated_retained->records.front().total_tokens == 305,
        "committed replacement survives loss of native source");

    const auto generation = addon::path_from_utf8(updated_retained->cache_candidate->root);
    fs::path retained_artifact;
    for (const auto& entry : fs::directory_iterator(generation)) {
        if (entry.path().extension() == ".cache") {
            retained_artifact = entry.path();
        }
    }
    check(!retained_artifact.empty(), "accepted generation contains retained evidence");
    std::ifstream cache_input(retained_artifact, std::ios::binary);
    std::string header;
    check(static_cast<bool>(std::getline(cache_input, header)), "retained artifact has metadata");
    auto metadata = addon::parse_json(header, {}, nullptr);
    check(metadata && metadata->is_object(), "retained metadata is an object");
    metadata->set("size", addon::Json_value("0"));
    std::ostringstream records;
    records << cache_input.rdbuf();
    cache_input.close();
    write(retained_artifact, addon::serialize_json(*metadata) + "\n" + records.str());
    select_cache(request, storage, "tampered", updated_retained->cache_candidate);
    check(!grok::scan_usage(request, profile, storage, [] { return false; }, &error) &&
        error == "usage_cache_unavailable", "accepted manifest binds exact contents as well as source names");
}

std::string read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    check(static_cast<bool>(input), "fixture read failed");
    return bytes.str();
}

void temporal_contract(const fs::path& root)
{
    const auto profile = root / "temporal-profile";
    const auto session = profile / "sessions/cwd/session-a";
    const auto storage = root / "temporal-storage";
    fs::create_directories(session);
    fs::create_directory(storage);
    const auto repository = root / "temporal-repository";
    fs::create_directories(repository / "child");
    fs::create_directory(repository / ".git");
    auto native_summary = *addon::parse_json(summary(), {}, nullptr);
    auto info = *native_summary.find("info");
    info.set("cwd", addon::Json_value(addon::path_to_utf8(repository / "child")));
    native_summary.set("info", info);
    write(session / "summary.json", addon::serialize_json(native_summary));
    auto document = *addon::parse_json(usage(), {}, nullptr);
    const auto original = document.find("turns")->as_array().front();
    auto turns = addon::Json_value::array();
    const std::array<std::string, 4> instants{
        "2026-09-13T23:45:00.123456789-02:30", "2026-09-11T01:00:00+14:00",
        "2026-09-12T12:00:00Z", "2026-09-12T12:00:00"};
    const std::array<std::uint64_t, 4> totals{25, 7, 0, 5};
    for (unsigned index = 0; index < totals.size(); ++index) {
        auto row = original;
        row.set("turnNumber", addon::Json_value(static_cast<int>(index + 1)));
        row.set("endedAt", addon::Json_value(instants[index]));
        row.set("inputTokens", addon::Json_value(index == 0 ? 20 : totals[index]));
        row.set("outputTokens", addon::Json_value(index == 0 ? 5 : 0));
        row.set("totalTokens", addon::Json_value(totals[index]));
        row.set("cachedReadTokens", addon::Json_value(0));
        row.set("cacheCreationTokens", addon::Json_value(0));
        row.set("usageIsIncomplete", addon::Json_value(index == 1));
        turns.push_back(std::move(row));
    }
    document.set("turns", std::move(turns));
    document.find("turns")->as_array().front().set("costUsdTicks", addon::Json_value(20000));
    write(session / "usage.json", addon::serialize_json(document));
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{2, "fixture", addon::path_to_utf8(profile)};
    request.context.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(storage)};
    std::string error;
    const auto candidate = storage / "selected";
    fs::create_directory(candidate);
    request.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(candidate)};
    auto page = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(page && page->records.size() == 4 && page->cache_candidate, error.c_str());
    addon::Contract_failure failure;
    check(addon::validate_usage_cache_candidate(*page->cache_candidate, addon::path_to_utf8(storage),
        request.context.account_context, [] { return false; }, &failure).has_value(), "temporal candidate is durably sealed");
    check(addon::decode_usage_scan_result(addon::encode_usage_scan_result(*page, 3), &failure, 3).has_value(),
        "temporal native page satisfies the unchanged accounting codec");

    const auto accepted_accounting = page->records;

    // Current accepted accounting may lack the optional dated evidence.
    const auto historical_root = addon::path_from_utf8(page->cache_candidate->root);
    for (const auto* name : {"usage-temporal", "usage-temporal-index", "cache-manifest.json"}) {
        check(fs::remove(historical_root / name), "fixture removes only its own dated evidence and seal");
    }
    const auto historical = addon::seal_usage_cache_candidate(page->cache_candidate->root, addon::path_to_utf8(storage),
        request.context.account_context, page->snapshot_id, "grok-turn-cache@1", [] { return false; }, &failure);
    check(historical.has_value(), "historical token cache has a valid complete seal");
    fs::create_directory(storage / "historical-successor");
    request.cache_selection = addon::Usage_cache_selection{historical, addon::path_to_utf8(storage / "historical-successor")};
    page = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(page && page->records.size() == 4 && page->cache_candidate, error.c_str());

    for (unsigned index = 0; index < totals.size(); ++index) {
        const auto& row = page->records[index];
        check(row.record_id == "grok:session-a:" + std::to_string(index + 1) && row.total_tokens == totals[index] &&
            row.completed_date == accepted_accounting[index].completed_date &&
            row.semantics == accepted_accounting[index].semantics,
            "native temporal proof preserves original identity, total, date and replay semantics");
        check(row.temporal && row.temporal->parent.record_id == row.record_id &&
            row.temporal->parent.total_tokens == row.total_tokens && row.temporal->parent.snapshot_id == page->snapshot_id &&
            row.temporal->parent.source_account_key == std::optional<std::string>("fixture"),
            "native temporal parent has exact accounting and current custody");
        const auto& proof = *row.temporal;
        if (index < 2) {
            check(proof.portions.size() == 1 && proof.portions.front().instant == instants[index] &&
                proof.portions.front().total_tokens == totals[index] && proof.unavailable_tokens == 0,
                "raw native RFC3339 offset and nanosecond spelling retains the exact admitted turn total");
        }
        else if (index == 2) {
            check(proof.portions.empty() && proof.unavailable_tokens == 0 && proof.coverage_reason == "complete",
                "zero accounting has no positive temporal portion");
        }
        else {
            check(proof.portions.empty() && proof.unavailable_tokens == 5 && proof.coverage_reason == "unsupported",
                "original time-zone-free accounting is not converted into an instant");
        }
    }
    check(page->records[1].input.coverage->covered_records == 0 &&
        page->records[1].temporal->portions.front().total_tokens == 7,
        "incomplete categories preserve the separately proved full turn total");
    auto conflicting = document;
    auto conflicting_turns = *conflicting.find("turns");
    conflicting_turns.as_array().front().set("endedAt", addon::Json_value("2026-09-14T01:45:00.123456789-02:30"));
    conflicting.set("turns", conflicting_turns);
    write(session / "usage.json", addon::serialize_json(conflicting));
    fs::create_directory(storage / "conflict");
    request.cache_selection = addon::Usage_cache_selection{page->cache_candidate, addon::path_to_utf8(storage / "conflict")};
    const auto conflict = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(conflict && conflict->records.front().total_tokens == 25 && conflict->records.front().temporal &&
        conflict->records.front().temporal->portions.empty() && conflict->records.front().temporal->unavailable_tokens == 25 &&
        conflict->records.front().temporal->coverage_reason == "unsupported", "same accounting parent with conflicting native instants withdraws only proof");
    fs::remove(session / "usage.json");
    fs::remove(repository / ".git");
    fs::create_directory(storage / "retained");
    request.cache_selection = addon::Usage_cache_selection{page->cache_candidate, addon::path_to_utf8(storage / "retained")};
    const auto retained = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(retained && retained->records.front().temporal &&
        retained->records.front().temporal->portions.front().instant == instants.front() &&
        retained->records.front().temporal->parent.snapshot_id == retained->snapshot_id &&
        retained->records.front().repository_usage->unattributed_tokens == 25,
        "deleted native source validates original cached proof before repository rebinding and new custody");
}

void temporal_capacity(const fs::path& root, bool require_native_proof = true)
{
    const auto profile = root / "budget-profile";
    const auto session = profile / "sessions/cwd/session-a";
    const auto storage = root / "budget-storage";
    fs::create_directories(session);
    fs::create_directory(storage);
    write(session / "summary.json", summary());
    write(session / "usage.json", usage());
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{2, "fixture", addon::path_to_utf8(profile)};
    request.context.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(storage)};
    const auto scan = [&](const char* name, const grok::Usage_scan_limits& limits, std::string& error) {
        const auto candidate = storage / name;
        fs::create_directory(candidate);
        request.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(candidate)};
        return grok::scan_usage(request, profile, storage, [] { return false; }, &error, limits);
    };
    std::string error;
    const auto full = scan("full", {}, error);
    check(full && full->records.size() == 1 && full->records.front().total_tokens == 25 && full->cache_candidate,
        "capacity oracle proves admitted original accounting under original defaults");
    check(!require_native_proof || (full->records.front().temporal && full->records.front().temporal->portions.size() == 1),
        "capacity oracle first proves admitted native evidence under original defaults");
    const auto full_root = addon::path_from_utf8(full->cache_candidate->root);
    addon::Usage_cache_manifest base{request.context.account_context, full->snapshot_id, "grok-turn-cache@1", {}};
    std::uint64_t core_bytes = 0, full_bytes = 0, core_lines = 0, core_entries = 2;
    for (const auto& entry : fs::directory_iterator(full_root)) {
        const auto name = addon::path_to_utf8(entry.path().filename());
        full_bytes += fs::file_size(entry.path());
        if (name == "usage-temporal" || name == "usage-temporal-index" || name == "cache-manifest.json") continue;
        const auto bytes = read(entry.path());
        core_bytes += bytes.size();
        core_lines += std::count(bytes.begin(), bytes.end(), '\n');
        ++core_entries;
        base.files.push_back({name, bytes.size(), std::string(64, '0')});
    }
    core_bytes += addon::serialize_json(addon::encode_usage_cache_manifest(base)).size();
    std::array<grok::Usage_scan_limits, 5> cases{};
    cases[0].disk_bytes = core_bytes;
    cases[1].filesystem_entries = core_entries;
    cases[2].persisted_lines = core_lines;
    cases[3].candidate_bytes = fs::file_size(full_root / "records");
    cases[4].disk_bytes = full_bytes;
    for (unsigned index = 0; index < cases.size(); ++index) {
        const auto name = "bounded-" + std::to_string(index);
        const auto page = scan(name.c_str(), cases[index], error);
        check(page && page->records.size() == 1 && page->records.front().total_tokens == 25 && page->cache_candidate,
            "optional physical capacity refusal preserves original sealed accounting");
        check(!page->records.front().temporal &&
            !fs::exists(addon::path_from_utf8(page->cache_candidate->root) / "usage-temporal") &&
            !fs::exists(addon::path_from_utf8(page->cache_candidate->root) / "usage-temporal-index"),
            "optional runs, sidecar and index are withdrawn completely after local capacity refusal");
        addon::Contract_failure failure;
        check(addon::validate_usage_cache_candidate(*page->cache_candidate, addon::path_to_utf8(storage),
            request.context.account_context, [] { return false; }, &failure).has_value(),
            "capacity fallback sealed inventory contains only intact accounting files");
        std::uint64_t bytes = 0, entries = 1, lines = 0;
        for (const auto& entry : fs::directory_iterator(addon::path_from_utf8(page->cache_candidate->root))) {
            const auto contents = read(entry.path());
            bytes += contents.size(); ++entries;
            lines += std::count(contents.begin(), contents.end(), '\n');
            check(!entry.path().filename().string().starts_with("temporal-run-"), "optional scratch never survives a sealed fallback");
        }
        check(bytes <= cases[index].disk_bytes && entries <= cases[index].filesystem_entries &&
            lines <= cases[index].persisted_lines, "actual complete sealed outputs obey every unchanged physical budget");
    }
    grok::Usage_scan_limits too_small;
    too_small.disk_bytes = core_bytes - 1;
    check(!scan("base-too-large", too_small, error) && error == "usage_disk_budget_exceeded",
        "genuinely oversized original accounting including its final SDK seal still fails");
}
void opaque_temporal_contract(const fs::path& root)
{
    const auto profile = root / "opaque-profile";
    const auto session = profile / "sessions/cwd/session-a";
    const auto storage = root / "opaque-storage";
    const auto repository = root / "opaque-repository";
    fs::create_directories(session);
    fs::create_directory(storage);
    fs::create_directories(repository / "child");
    fs::create_directory(repository / ".git");
    auto native_summary = *addon::parse_json(summary(), {}, nullptr);
    auto info = *native_summary.find("info");
    info.set("cwd", addon::Json_value(addon::path_to_utf8(repository / "child")));
    native_summary.set("info", info);
    write(session / "summary.json", addon::serialize_json(native_summary));
    write(session / "usage.json", usage());
    addon::Usage_scan_request request;
    request.context.account_context = addon::Account_context{2, "fixture", addon::path_to_utf8(profile)};
    request.context.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(storage)};
    std::string error;
    const auto scan = [&](const char* name, const std::optional<addon::Usage_cache_reference>& accepted,
                          const grok::Usage_scan_limits& limits = grok::Usage_scan_limits{}) {
        const auto candidate = storage / name;
        fs::create_directory(candidate);
        request.cache_selection = addon::Usage_cache_selection{accepted, addon::path_to_utf8(candidate)};
        const auto page = grok::scan_usage(request, profile, storage, [] { return false; }, &error, limits);
        check(page && page->records.size() == 1 && page->cache_candidate, error.c_str());
        return *page;
    };
    const auto initial = scan("original", std::nullopt);
    check(initial.records.front().total_tokens == 25 && initial.records.front().temporal,
        "opaque fixture first proves native accounting and optional proof");
    const auto original_root = addon::path_from_utf8(initial.cache_candidate->root);
    auto opaque = read(original_root / "usage-temporal");
    const auto newline = opaque.find('\n');
    auto header = *addon::parse_json(opaque.substr(0, newline), {}, nullptr);
    header.set("version", addon::Json_value(99));
    header.set("future_field", addon::Json_value("opaque future custody"));
    opaque = addon::serialize_json(header) + opaque.substr(newline);
    write(original_root / "usage-temporal", opaque);
    const auto opaque_index = read(original_root / "usage-temporal-index");
    fs::remove(original_root / "cache-manifest.json");
    addon::Contract_failure failure;
    const auto future = addon::seal_usage_cache_candidate(initial.cache_candidate->root, addon::path_to_utf8(storage),
        request.context.account_context, initial.snapshot_id, "grok-turn-cache@1", [] { return false; }, &failure);
    check(future.has_value(), "fixture seals independently unknown bounded carrier");
    fs::remove(session / "usage.json");
    const auto first = scan("successor-one", future);
    const auto first_root = addon::path_from_utf8(first.cache_candidate->root);
    check(first.records.front().total_tokens == 25 && (!first.records.front().temporal ||
        first.records.front().temporal->portions.empty()),
        "unknown version does not alter accounting or invent dated proof");
    check(read(first_root / "usage-temporal") == opaque && read(first_root / "usage-temporal-index") == opaque_index,
        "bounded unknown carrier keeps exact bytes and original custody in successor");
    fs::remove_all(original_root);
    const auto second = scan("successor-two", first.cache_candidate);
    const auto second_root = addon::path_from_utf8(second.cache_candidate->root);
    fs::remove_all(first_root);
    check(read(second_root / "usage-temporal") == opaque && read(second_root / "usage-temporal-index") == opaque_index &&
        !second.records.front().temporal && second.records.front().total_tokens == 25,
        "repeated selection and older-root reclamation conserve opaque evidence and accounting");
    check(addon::validate_usage_cache_candidate(*second.cache_candidate, addon::path_to_utf8(storage),
        request.context.account_context, [] { return false; }, &failure).has_value(), "opaque successor has exact current SDK seal");

    grok::Usage_scan_limits capacity;
    capacity.candidate_bytes = fs::file_size(second_root / "records");
    const auto bounded = scan("bounded-copy", second.cache_candidate, capacity);
    const auto bounded_root = addon::path_from_utf8(bounded.cache_candidate->root);
    check(bounded.records.front().total_tokens == 25 && !bounded.records.front().temporal &&
        !fs::exists(bounded_root / "usage-temporal") && !fs::exists(bounded_root / "usage-temporal-index"),
        "opaque copy capacity refusal is local while complete original accounting remains sealed");
    check(read(second_root / "usage-temporal") == opaque, "capacity refusal never mutates accepted opaque evidence");

    // A native timestamp spelling correction can retain the accounting instant;
    // changed original cache bytes still prohibit rebinding an unknown proof.
    auto corrected = *addon::parse_json(usage(), {}, nullptr);
    auto turns = *corrected.find("turns");
    turns.as_array().front().set("endedAt", addon::Json_value("2026-09-13T12:00:00.000Z"));
    corrected.set("turns", turns);
    write(session / "usage.json", addon::serialize_json(corrected));
    const auto correction = scan("native-correction", second.cache_candidate);
    check(correction.records.front().total_tokens == 25 &&
        read(addon::path_from_utf8(correction.cache_candidate->root) / "usage-temporal") != opaque,
        "native source correction never rebinds unknown carrier");
    write(session / "usage.json", usage(300));
    const auto counter = scan("counter-correction", second.cache_candidate);
    check(counter.records.front().total_tokens == 305 &&
        read(addon::path_from_utf8(counter.cache_candidate->root) / "usage-temporal") != opaque,
        "changed accounting does not inherit unknown carrier");
    fs::remove(session / "usage.json");
    fs::remove(repository / ".git");
    const auto rebound = scan("repository-correction", second.cache_candidate);
    check(rebound.records.front().total_tokens == 25 && rebound.records.front().repository_usage->unattributed_tokens == 25 &&
        read(addon::path_from_utf8(rebound.cache_candidate->root) / "usage-temporal") != opaque,
        "repository correction preserves accounting without rebinding unknown custody");
}
} // namespace

int main(int argc, char** argv)
{
    const auto root = fs::canonical(fs::temp_directory_path()) / ("grok-usage-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directory(root);
        if (argc == 2 && std::string_view(argv[1]) == "opaque") {
            opaque_temporal_contract(root);
        }
        else if (argc == 2 && std::string_view(argv[1]) == "seal-budget") {
            temporal_capacity(root, false);
        }
        else if (argc == 2 && std::string_view(argv[1]) == "temporal") {
            temporal_contract(root);
            temporal_capacity(root);
            opaque_temporal_contract(root);
        }
        else {
            exercise(root);
            retargeted_source(root);
            interrupted_refresh(root);
            temporal_contract(root);
            temporal_capacity(root);
            opaque_temporal_contract(root);
        }
        fs::remove_all(root);
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        fs::remove_all(root);
        return 1;
    }
}
