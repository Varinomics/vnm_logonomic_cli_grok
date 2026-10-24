#include "grok_accounts.h"
#include "grok_usage.h"

#include <logonomic_cli_addon_test/invocation_context.h>
#include <logonomic_cli_addon_test/usage_scan.h>

#include <logonomic_cli_addon/environment_bootstrap.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/usage_scan.h>
#include <logonomic_cli_addon/stored_usage_accounting.h>
#include <logonomic_cli_addon/sha256.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <utility>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace test = addon::test;
namespace fs = std::filesystem;
constexpr std::string_view k_native_instant = "2026-09-13T12:00:00.123456789+02:30";

void check(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

using Cancelled_context = test::Cancelled_scan_context;

class Workspace
{
public:
    Workspace()
    :
        m_path(fs::temp_directory_path() / ("grok-cache-contract-" + addon::random_nonce_hex()))
    {
        fs::create_directories(m_path);
        m_path = fs::canonical(m_path);
    }

    ~Workspace()
    {
        std::error_code error;
        fs::remove_all(m_path, error);
    }

    fs::path m_path;
};

void write(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    check(output.good(), "local telemetry fixture write failed");
}

std::vector<fs::path> write_sources(const fs::path& profile, unsigned tokens)
{

    std::vector<fs::path> paths;
    for (unsigned index = 0; index < 270; ++index) {
        const auto id = "session-" + std::to_string(index);
        const auto session = profile / "sessions/workspace" / id;
        write(session / "summary.json", "{\"info\":{\"id\":\"" + id + "\",\"cwd\":\"\"}}");
        const auto path = session / "usage.json";
        write(path, "{\"sessionId\":\"" + id + "\",\"turns\":[{\"turnNumber\":1,"
            "\"endedAt\":\"" + std::string(k_native_instant) + "\",\"inputTokens\":" + std::to_string(tokens) +
            ",\"outputTokens\":0,\"totalTokens\":" + std::to_string(tokens) +
            ",\"cachedReadTokens\":0,\"cacheCreationTokens\":0,\"reasoningTokens\":0,"
            "\"costUsdTicks\":0,\"costIsPartial\":false,\"usageIsIncomplete\":false}]}");
        paths.push_back(path);
    }
    return paths;
}

std::string read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "fixture cache opens");
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void rewrite_inventory(const fs::path& root)
{
    std::ifstream original(root / "cache-inventory", std::ios::binary);
    std::string header;
    check(static_cast<bool>(std::getline(original, header)), "original cache inventory header exists");
    original.close();
    addon::Sha256 digest;
    digest.update(header); digest.update("\n");
    std::string bytes = header + "\n";
    unsigned count = 0;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (entry.path().extension() != ".cache") continue;
        const auto line = entry.path().stem().string() + ":" + addon::to_hex(addon::Sha256::digest(read(entry.path())));
        bytes += line + "\n";
        digest.update(line); digest.update("\n");
        ++count;
    }
    bytes += "end:" + std::to_string(count) + ":" + addon::to_hex(digest.finish()) + "\n";
    write(root / "cache-inventory", bytes);
}

void adapter_cache_contract(const fs::path& package, const fs::path& fixture)
{
    const auto profile = fixture / "native-profile";
    auto sources = write_sources(profile, 10);
    const auto native_profile = addon::path_to_utf8(profile);
    const auto account = grok::profile_account_key(native_profile);

    addon::Simulator_options options;
    options.package_directory = package;
    options.profile_storage_root = fixture / "host-storage";
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    check(host.start(&failure), failure.detail);
    std::string error;
    const auto allocated = addon::allocate_profile_storage_root(
        host.profile_storage_root(), "logonomic.cli.grok", &error, account);
    check(allocated.has_value(), error);
    const auto storage = addon::path_from_utf8(*allocated);

    addon::Usage_scan_request request;
    request.context.launcher = {"grok", addon::k_runner_direct, {}};
    request.context.launcher_argv = {"grok"};
    request.context.profile_storage = addon::Profile_storage_context{1, *allocated};
    request.context.account_context = addon::Account_context{2, account, native_profile};
    request.context.launch_environment = addon::Launch_environment{
        addon::host_platform_id(), addon::Json_value::object(),
        addon::Environment_bootstrap_evidence{native_profile}};

    addon::Simulator_invocation_options invocation;
    invocation.profile_storage = request.context.profile_storage;
    const auto acknowledge = host.invoke({"profile-storage", 1}, "acknowledge",
        addon::Json_value::object(), 5000, invocation);
    const auto acknowledged = host.wait_outcome(acknowledge, 6000);
    check(acknowledged && acknowledged->status == addon::Host_status::SUCCESS,
        "packaged adapter acknowledges allocated storage");

    const auto select = [&](std::string name, std::optional<addon::Usage_cache_reference> accepted) {
        request.snapshot_id.reset();
        request.cursor.reset();
        request.mode = "incremental";
        const auto path = storage / name;
        check(fs::create_directory(path), "host exclusively allocates candidate directory");
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), addon::path_to_utf8(path)};
    };
    const auto validate = [&](const addon::Usage_cache_reference& reference) {
        check(addon::validate_usage_cache_candidate(reference, *allocated, request.context.account_context,
            [] { return false; }, &failure).has_value(), "sealed candidate remains intact: " + failure.detail);
    };

    for (const auto& path : sources) {
        check(fs::remove(path), "remove fixture source");
    }
    select("empty-selection", std::nullopt);
    const auto empty = test::usage_scan_pages(host, request, 3);
    check(empty.records.empty() && empty.cache_candidate,
        "a fresh selection with absent source history contains no records");
    validate(*empty.cache_candidate);

    sources = write_sources(profile, 10);
    select("accepted", std::nullopt);
    const auto first_request = request;
    const auto first = test::usage_scan_page(host, request, 3);
    check(!first.complete && first.records.size() == 256 && first.cache_candidate,
        "packaged first page supplies a sealed bounded candidate");
    validate(*first.cache_candidate);
    request.snapshot_id = first.snapshot_id;
    request.cursor = first.next_cursor;
    auto changed = request;
    changed.mode = "repair";
    check(test::usage_scan_outcome(host, changed, 3).status != addon::Host_status::SUCCESS,
        "continuation cannot change the frozen request");
    const auto last = test::usage_scan_page(host, request, 3);
    check(last.complete && last.records.size() == 14 && last.cache_candidate == first.cache_candidate,
        "packaged final page preserves candidate identity");
    for (const auto& record : first.records) {
        check(record.aggregate_evidence == "partial", "surviving native subsets make conservative evidence claims");
        check(record.temporal && record.temporal->parent.record_id == record.record_id &&
            record.temporal->parent.total_tokens == 10 && record.temporal->parent.snapshot_id == first.snapshot_id &&
            record.temporal->parent.source_account_key == std::optional(account) &&
            record.temporal->portions.size() == 1 && record.temporal->portions.front().instant == k_native_instant &&
            record.temporal->portions.front().total_tokens == 10 && record.temporal->unavailable_tokens == 0,
            "actual adapter publishes exact native turn proof with raw precision and current custody");
    }
    for (const auto& row : last.records) check(row.temporal && row.temporal->portions.size() == 1 &&
        row.temporal->portions.front().instant == k_native_instant, "continuation index preserves the same native proof");

    select("account-v1", std::nullopt);
    request.context.account_context->version = 1;
    request.context.account_context->profile_path.reset();
    // Version one has no profile locator; acknowledgment support must not
    // bypass the native provider's existing explicit-account admission.
    const auto version_one = test::usage_scan_outcome(host, request, 3);
    check(version_one.status != addon::Host_status::SUCCESS &&
        version_one.reason_code == "account_profile_unavailable",
        "version-one account without a profile locator retains its explicit refusal");
    request.context = first_request.context;
    validate(*first.cache_candidate);

    select("account-absent", std::nullopt);
    request.context.account_context.reset();
    const auto account_absent = test::usage_scan_outcome(host, request, 3);
    check(account_absent.status != addon::Host_status::SUCCESS &&
        account_absent.reason_code == "account_profile_unavailable",
        "acknowledgments do not broaden Grok's existing explicit-account admission");
    request.context = first_request.context;

    sources = write_sources(profile, 1);
    select("unaccepted", first.cache_candidate);
    request.mode = "repair";
    const auto lower = test::usage_scan_pages(host, request, 3);
    check(test::usage_token_total(lower) == 270 && lower.cache_candidate,
        "lower native observation is a separate candidate");
    validate(*first.cache_candidate);
    for (const auto& path : sources) {
        check(fs::remove(path), "remove refreshed fixture source");
    }

    // The host rejects the lower partial aggregate and continues selecting the coherent original cache.
    select("recovered", first.cache_candidate);
    const auto recovered = test::usage_scan_pages(host, request, 3);
    check(test::usage_token_total(recovered) == 2700,
        "rejected candidate cannot become retained history after source deletion");
    for (const auto& row : recovered.records) check(row.temporal && row.temporal->portions.size() == 1 &&
        row.temporal->portions.front().instant == k_native_instant &&
        row.temporal->parent.snapshot_id == recovered.snapshot_id,
        "packaged deleted-source replay retains only the exact selected original proof");
    validate(*first.cache_candidate);
    validate(*lower.cache_candidate);

    const auto clone = [&](const std::string& name) {
        const auto root = storage / name;
        check(fs::create_directory(root), "fixture allocates isolated retained copy");
        const auto io = addon::usage_cache_io_path(root);
        for (const auto& entry : fs::directory_iterator(addon::usage_cache_io_path(
                 addon::path_from_utf8(first.cache_candidate->root))))
        {
            if (entry.path().filename() == "cache-manifest.json") continue;
            fs::copy_file(entry.path(), io / entry.path().filename());
        }
        return root;
    };
    const auto reseal = [&](const fs::path& root) {
        const auto reference = addon::seal_usage_cache_candidate(addon::path_to_utf8(root), *allocated,
            request.context.account_context, first.snapshot_id, "grok-turn-cache@1", [] { return false; }, &failure);
        check(reference.has_value(), "fixture seals exact altered optional carrier or admitted accounting");
        validate(*reference);
        return *reference;
    };
    for (unsigned kind = 0; kind < 5; ++kind) {
        const auto root = clone("optional-" + std::to_string(kind));
        const auto path = addon::usage_cache_io_path(root) / "usage-temporal";
        const auto bytes = read(path);
        const auto newline = bytes.find('\n');
        auto header = *addon::parse_json(bytes.substr(0, newline), {}, nullptr);
        if (kind == 0) header.set("version", addon::Json_value(99));
        if (kind == 1) header.set("account", addon::Json_value("foreign"));
        if (kind == 2) header.set("snapshot_id", addon::Json_value("foreign-snapshot"));
        if (kind < 3) write(path, addon::serialize_json(header) + bytes.substr(newline));
        if (kind == 3) write(path, bytes.substr(0, newline + 1) + std::string(4 * 1024 * 1024 + 1, 'x') + "\n");
        if (kind == 4) write(addon::usage_cache_io_path(root) / "usage-temporal-index", std::string(16, '\0'));
        select("optional-next-" + std::to_string(kind), reseal(root));
        const auto page = test::usage_scan_pages(host, request, 3);
        check(test::usage_token_total(page) == 2700,
            "unknown, foreign, oversized or damaged optional carrier retains all accounting");
        for (const auto& row : page.records) check(!row.temporal || (row.temporal->portions.empty() &&
            row.temporal->unavailable_tokens == row.total_tokens), "invalid retained evidence never becomes a dated claim");
        if (kind == 0) {
            check(page.cache_candidate.has_value(), "unknown carrier successor remains sealed accounting");
            const auto opaque = read(path);
            const auto opaque_index = read(addon::usage_cache_io_path(root) / "usage-temporal-index");
            const auto successor = addon::usage_cache_io_path(addon::path_from_utf8(page.cache_candidate->root));
            check(read(successor / "usage-temporal") == opaque && read(successor / "usage-temporal-index") == opaque_index,
                "unknown carrier and index survive unchanged accounting with exact original custody");
            fs::remove_all(addon::usage_cache_io_path(root));
            select("optional-unknown-selected-again", page.cache_candidate);
            const auto second = test::usage_scan_pages(host, request, 3);
            check(test::usage_token_total(second) == 2700 && second.cache_candidate,
                "opaque successor remains selectable after original reclamation");
            fs::remove_all(addon::usage_cache_io_path(addon::path_from_utf8(page.cache_candidate->root)));
            const auto selected = addon::usage_cache_io_path(addon::path_from_utf8(second.cache_candidate->root));
            check(read(selected / "usage-temporal") == opaque && read(selected / "usage-temporal-index") == opaque_index,
                "opaque bytes retain original custody across repeated succession and reclamation");
            validate(*second.cache_candidate);
            for (const auto& row : second.records) check(!row.temporal,
                "unknown version remains daily unavailable after opaque copy-forward");
        }
    }
    {
        const auto root = clone("admitted-control");
        std::string target_id;
        for (const auto& entry : fs::directory_iterator(addon::usage_cache_io_path(root))) {
            if (entry.path().extension() != ".cache") continue;
            const auto bytes = read(entry.path());
            const auto newline = bytes.find('\n');
            auto envelope = *addon::parse_json(bytes.substr(newline + 1), {}, nullptr);
            auto row = *envelope.find("usage");
            target_id = "grok:accepted\tparent:1";
            row.set("record_id", addon::Json_value(target_id));
            check(addon::decode_stored_usage_accounting(row, &failure).has_value() &&
                addon::parse_json(addon::serialize_json(row), {}, nullptr).has_value(),
                "original accounting admits the exact non-NUL control ID");
            envelope.set("usage", row);
            write(entry.path(), bytes.substr(0, newline + 1) + addon::serialize_json(envelope) + "\n");
            break;
        }
        rewrite_inventory(addon::usage_cache_io_path(root));
        select("admitted-control-next", reseal(root));
        const auto page = test::usage_scan_pages(host, request, 3);
        check(test::usage_token_total(page) == 2700,
            "invalid optional parent preserves actual packaged accounting");
        bool found = false;
        for (const auto& row : page.records) {
            if (row.record_id != target_id) continue;
            found = true;
            check(row.total_tokens == 10 && !row.temporal,
                "invalid full or reduced optional envelope is omitted with original ID and H intact");
        }
        check(found, "actual packaged page retains the exact original admitted parent");
    }

    select("cancelled", first.cache_candidate);
    Cancelled_context cancelled(request);
    bool saw_cancel = false;
    try {
        grok::handle_usage(cancelled);
    }
    catch (const std::runtime_error& error) {
        saw_cancel = std::string(error.what()) == "cancelled";
    }
    check(saw_cancel, "cancelled scan cannot seal or promote a candidate");
    check(!fs::exists(addon::path_from_utf8(request.cache_selection->candidate_root) / "cache-manifest.json"),
        "cancelled candidate stays unsealed");
    validate(*first.cache_candidate);

    select("reset", std::nullopt);
    request.mode = "reset";
    const auto reset = test::usage_scan_pages(host, request, 3);
    check(reset.records.empty() && reset.cache_candidate, "reset reconstructs only current sources");
    validate(*reset.cache_candidate);
    validate(*first.cache_candidate);
    auto invalid_reset = request;
    invalid_reset.cache_selection->accepted = first.cache_candidate;
    check(test::usage_scan_outcome(host, invalid_reset, 3).status != addon::Host_status::SUCCESS,
        "reset cannot import a retained selection");

    select("occupied", first.cache_candidate);
    write(addon::path_from_utf8(request.cache_selection->candidate_root) / "foreign", "occupied");
    check(test::usage_scan_outcome(host, request, 3).status != addon::Host_status::SUCCESS,
        "provider refuses a nonempty host allocation");
    validate(*first.cache_candidate);

    select("wrong-account", first.cache_candidate);
    auto wrong_account = request;
    const auto other_profile = fixture / "other-native-profile";
    fs::create_directories(other_profile);
    wrong_account.context.account_context = addon::Account_context{
        2, grok::profile_account_key(addon::path_to_utf8(other_profile)), addon::path_to_utf8(other_profile)};
    wrong_account.context.launch_environment->bootstrap =
        addon::Environment_bootstrap_evidence{addon::path_to_utf8(other_profile)};
    check(test::usage_scan_outcome(host, wrong_account, 3).status != addon::Host_status::SUCCESS,
        "selected cache cannot cross native profile and account ownership");
    validate(*first.cache_candidate);

    select("corrupt-input", first.cache_candidate);
    write(addon::path_from_utf8(first.cache_candidate->root) / "unlisted", "unexpected");
    check(test::usage_scan_outcome(host, request, 3).status != addon::Host_status::SUCCESS,
        "corrupt selected inventory fails without fallback");
    check(host.shutdown(1000, nullptr), "packaged adapter exits without contacting a model");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        check(argc == 2, "expected built package directory");
        Workspace workspace;
        adapter_cache_contract(addon::path_from_utf8(argv[1]), workspace.m_path);
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
