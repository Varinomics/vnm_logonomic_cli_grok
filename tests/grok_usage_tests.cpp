#include "grok_usage.h"
#include "grok_usage_records.h"

#include <logonomic_cli_addon/sha256.h>

#include <chrono>
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

std::string usage(int input = 20, bool partial = false, bool incomplete = false,
    std::optional<std::int64_t> cost_ticks = 20000, std::string_view session_id = "session-a")
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
    if (cost_ticks) {
        row.set("costUsdTicks", addon::Json_value(*cost_ticks));
    }
    row.set("costIsPartial", addon::Json_value(partial));
    row.set("usageIsIncomplete", addon::Json_value(incomplete));
    auto rows = addon::Json_value::array();
    rows.push_back(row);
    auto result = addon::Json_value::object();
    result.set("sessionId", addon::Json_value(std::string(session_id)));
    result.set("session", row); // The session aggregate must never be added again.
    result.set("turns", rows);
    return addon::serialize_json(result);
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
    const auto first = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(first && first->records.size() == 1, "one native turn produces one record");
    const auto& row = first->records.front();
    check(row.total_tokens == 25 && row.input.value == 20 && row.output.value == 5,
        "cache and reasoning subsets do not inflate input plus output");
    check(row.cache_read.value == 7 && row.cache_creation.value == 3 && row.cost_microusd.value == 2,
        "categories retained and exact tick prices convert to whole microUSD");
    check(row.source_counter_semantics == "per_event_sum",
        "native turn deltas retain per-event source semantics");
    addon::Contract_failure failure;
    check(addon::decode_usage_scan_result(addon::encode_usage_scan_result(*first, 2), &failure, 2).has_value(),
        "native usage page satisfies the host usage-scan contract");

    // Native ticks are 1e10/USD; usage-scan requires exact integer microUSD.
    struct Cost_case
    {
        std::optional<std::int64_t> ticks;
        bool partial;
        bool incomplete;
        std::optional<std::uint64_t> microusd;
    };
    for (const auto& item : {
            Cost_case{0,            false, false, 0},
            Cost_case{20000,        false, false, 2},
            Cost_case{25000,        false, false, std::nullopt},
            Cost_case{std::nullopt, false, false, std::nullopt},
            Cost_case{20000,        true,  false, std::nullopt},
            Cost_case{20000,        false, true,  std::nullopt}})
    {
        std::istringstream input(usage(20, item.partial, item.incomplete, item.ticks));
        std::size_t count = 0;
        const auto inspect_record = [
                &count,
                &item,
                &failure
            ](
                const auto& record)
            {
                ++count;
                check(record.usage.cost_microusd.applicable == item.microusd.has_value() &&
                    record.usage.cost_microusd.value == item.microusd, "only complete exact costs are applicable");
                check(addon::decode_usage_record(addon::encode_usage_record(record.usage, 2), &failure, 2).has_value(),
                    "every native cost state remains accepted by the host codec");
                return true;
            };
        check(grok::read_session_usage(input, session / "usage.json", inspect_record,
            [] { return false; }, &error) && count == 1, "native cost fixture emits one turn");
    }

    write(session / "summary.json", summary("fork", true));
    check(!grok::scan_usage(request, profile, storage, [] { return false; }, &error) &&
        error == "usage_fork_attribution_unavailable", "copied fork fails even when usage fingerprint is unchanged");
    auto replay = request;
    replay.snapshot_id = first->snapshot_id;
    replay.cursor = "0";
    const auto prior = grok::scan_usage(replay, profile, storage, [] { return false; }, &error);
    check(prior && prior->records.front().total_tokens == 25, "fork failure leaves accepted immutable snapshot readable");

    write(session / "summary.json", summary());
    write(session / "usage.json", usage(30, true, true));
    const auto partial = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(partial && partial->incomplete_historical_coverage && !partial->records.front().cost_microusd.value,
        "incomplete or partial cost is never reported as a complete price");
    fs::remove(session / "usage.json");
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
    check(!grok::scan_usage(request, profile, storage, [] { return false; }, &error, limits) &&
        error == "usage_candidate_budget_exceeded", "candidate bound rejects source before publication");
    check(!grok::scan_usage(request, profile, storage, [] { return true; }, &error) && error == "cancelled",
        "production cancellation interrupts without starting a candidate");

    const auto case_profile = root / "case-profile";
    const auto case_session = case_profile / "sessions" / "cwd" / "session-a";
    const auto case_storage = root / "case-storage";
    fs::create_directories(case_session);
    fs::create_directory(case_storage);
    write(case_session / "summary.json", summary());
    write(case_session / "USAGE.JSON", usage());
    const bool native_lookup_exists = fs::is_regular_file(case_session / "usage.json");
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
                const auto parsed = addon::parse_json(record);
                const auto* usage_record = parsed ? parsed->find("usage") : nullptr;
                const auto* tokens = usage_record ? usage_record->find("total_tokens") : nullptr;
                if (tokens && tokens->is_integer() && tokens->as_integer() == 305) {
                    interrupted = true;
                    return true;
                }
            }
            return false;
        };
    check(!grok::scan_usage(request, profile, storage, cancel_after_refresh, &error) &&
        interrupted && error == "cancelled", "interrupt after source artifact replacement before scan acceptance");
    fs::remove(session / "usage.json");
    const auto retained = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(retained && retained->records.size() == 1 && retained->records.front().total_tokens == 25,
        "source deletion after rejected refresh retains the accepted generation");

    const auto second_session = profile / "sessions" / "cwd" / "session-b";
    fs::create_directory(second_session);
    write(second_session / "summary.json", summary({}, false, "session-b"));
    write(second_session / "usage.json", usage(50, false, false, 20000, "session-b"));
    write(session / "usage.json", usage(300));
    grok::Usage_scan_limits limits;
    limits.candidate_records = 1;
    check(!grok::scan_usage(request, profile, storage, [] { return false; }, &error, limits) &&
        error == "usage_candidate_budget_exceeded", "aggregate rejection occurs after individually valid refreshes");
    fs::remove(session / "usage.json");
    fs::remove(second_session / "usage.json");
    const auto after_rejection = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(after_rejection && after_rejection->records.size() == 1 && after_rejection->records.front().total_tokens == 25,
        "rejected scan cannot retain newer rows or adopt a newly discovered source");

    write(session / "usage.json", usage(300));
    const auto updated = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(updated && updated->records.size() == 1 && updated->records.front().total_tokens == 305,
        "complete replacement publishes new records");
    fs::remove(session / "usage.json");
    const auto updated_retained = grok::scan_usage(request, profile, storage, [] { return false; }, &error);
    check(updated_retained && updated_retained->records.size() == 1 && updated_retained->records.front().total_tokens == 305,
        "committed replacement survives loss of native source");

    const auto generation = storage / "grok-usage-v1" / "snapshots" / updated_retained->snapshot_id;
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
    auto metadata = addon::parse_json(header);
    check(metadata && metadata->is_object(), "retained metadata is an object");
    metadata->set("size", addon::Json_value("0"));
    std::ostringstream records;
    records << cache_input.rdbuf();
    cache_input.close();
    write(retained_artifact, addon::serialize_json(*metadata) + "\n" + records.str());
    check(!grok::scan_usage(request, profile, storage, [] { return false; }, &error) &&
        error == "required_usage_cache_unavailable", "accepted manifest binds exact contents as well as source names");
}
} // namespace

int main()
{
    const auto root = fs::temp_directory_path() / ("grok-usage-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directory(root);
        exercise(root);
        interrupted_refresh(root);
        fs::remove_all(root);
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        fs::remove_all(root);
        return 1;
    }
}
