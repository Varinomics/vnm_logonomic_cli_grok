#include "grok_accounts.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs    = std::filesystem;
namespace json  = addon::json;

constexpr std::string_view k_session_id = "8e18215e-f612-4131-95dd-7890139a41ae";
constexpr std::string_view k_primary = "{\"type\":\"system\",\"content\":\"You are Grok Build.\"}\n";
constexpr std::string_view k_reminder =
    "{\"type\":\"user\",\"synthetic_reason\":\"system_reminder\","
    "\"content\":[{\"type\":\"text\",\"text\":\"Available skills: review\"}]}\n";

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class Temporary_directory
{
public:
    Temporary_directory()
    :
        m_path(fs::canonical(fs::temp_directory_path()) / ("grok-resume-" + addon::random_nonce_hex()))
    {
        fs::create_directories(m_path);
    }
    ~Temporary_directory()
    {
        std::error_code error;
        fs::remove_all(m_path, error);
    }
    const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

void write(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), (std::streamsize)bytes.size());
    output.close();
    require(bool(output), "could not write native history fixture");
}

std::map<std::string, std::string> native_files(const fs::path& root)
{
    std::map<std::string, std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream input(entry.path(), std::ios::binary);
        result.emplace(addon::path_to_utf8(fs::relative(entry.path(), root)),
            std::string(std::istreambuf_iterator<char>(input), {}));
    }
    return result;
}

std::string cwd_bucket(std::string_view cwd)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char byte : cwd) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '~')
        {
            encoded += (char)byte;
        }
        else {
            encoded += '%';
            encoded += digits[byte >> 4];
            encoded += digits[byte & 15];
        }
    }
    return encoded;
}

addon::Json_value native_summary(std::string_view cwd)
{
    // Oracle: native Summary, including the summary-only skeleton produced
    // by official Grok 1.0.77 before an auth-required session/new rejection.
    return json::object({
        {"info", json::object({{"id", addon::Json_value(k_session_id)}, {"cwd", addon::Json_value(cwd)}})},
        {"session_summary", addon::Json_value("")},
        {"created_at", addon::Json_value("2026-11-08T09:33:54.045371105Z")},
        {"updated_at", addon::Json_value("2026-11-08T09:33:54.048726075Z")},
        {"num_messages", addon::Json_value(0)},
        {"num_chat_messages", addon::Json_value(0)},
        {"current_model_id", addon::Json_value("grok-4.6")},
        {"chat_format_version", addon::Json_value(1)}});
}

addon::Resume_inspect_request inspect_request(const fs::path& native, const fs::path& profile, const fs::path& cwd)
{
    addon::Resume_inspect_request request;
    request.context.launcher = {"\"" + addon::path_to_utf8(native) + "\"", addon::k_runner_direct, {}};
    request.context.launcher_argv = {addon::path_to_utf8(native)};
    request.context.working_directory = addon::path_to_utf8(cwd);
    request.context.launch_environment = addon::Launch_environment{
        addon::host_platform_id(), addon::Json_value::object(), std::nullopt};
    for (const auto& [name, value] : addon::current_process_environment()) {
        if (name == "SystemRoot" || name == "WINDIR" || name == "PATH") {
            request.context.launch_environment->effective_environment.set(name, addon::Json_value(value));
        }
    }
    const auto home = addon::path_to_utf8(profile);
    request.context.launch_environment->effective_environment.set("GROK_HOME", addon::Json_value(home));
    request.context.account_context = addon::Account_context{2, grok::profile_account_key(home), home};
    request.resume_record.set("provider_session_id", addon::Json_value(k_session_id));
    return request;
}

addon::Json_value invoke(
    addon::Host_simulator& host,
    const addon::Capability_pair& capability,
    const std::string& operation,
    addon::Json_value payload)
{
    const auto id = host.invoke(capability, operation, std::move(payload), 5000);
    const auto result = host.wait_outcome(id, 6000);
    require(result && result->status == addon::Host_status::SUCCESS,
        operation + " failed: " + (result ? result->reason_code : "no outcome"));
    return result->payload;
}

addon::Resume_inspect_result inspect(
    addon::Host_simulator& host,
    const addon::Resume_inspect_request& request,
    std::uint16_t version = 4)
{
    const auto payload = invoke(host, {"interactive-resume", version}, addon::k_inspect_resume_operation,
        addon::encode_resume_inspect_request(request));
    const auto result = addon::decode_resume_inspect_result(payload, nullptr, version);
    require(result && addon::validate_resume_inspection(request, *result, nullptr),
        "packaged inspection did not acknowledge its complete exact request");
    require(version != 3 || !payload.find("content"), "version 3 acquired version 4 response grammar");
    return *result;
}

void require_content(
    addon::Host_simulator& host,
    const addon::Resume_inspect_request& request,
    addon::Resume_content expected,
    const std::string& label)
{
    const auto result = inspect(host, request);
    require(result.availability == addon::Resume_availability::AVAILABLE && result.content == expected, label);
}

void prepare_exact(addon::Host_simulator& host, const addon::Resume_inspect_request& request)
{
    addon::Resume_prepare_request prepare;
    prepare.context       = request.context;
    prepare.resume_record = request.resume_record;
    const auto payload = invoke(host, {"interactive-resume", 2}, addon::k_prepare_resume_operation,
        addon::encode_resume_prepare_request(prepare, 2));
    const auto result = addon::decode_resume_prepare_result(payload, nullptr, 2);
    require(result && result->acknowledgment.record_digest_hex == addon::resume_record_digest_hex(request.resume_record),
        "preparation did not retain the exact opaque resume record");
    require(result->process.argv.size() >= 3 && result->process.argv.back() == json::text(request.resume_record, "provider_session_id") &&
            result->process.argv[result->process.argv.size() - 2] == "--resume",
        "preparation retargeted the exact native UUID");
    require(std::find(result->patch.set.begin(), result->patch.set.end(),
            std::pair<std::string, std::string>{"GROK_HOME", json::text(
                request.context.launch_environment->effective_environment, "GROK_HOME")}) != result->patch.set.end(),
        "preparation and inspection selected different native homes");
}

void require_shared_refusal(
    addon::Host_simulator& host,
    const addon::Resume_inspect_request& request,
    std::string_view reason)
{
    addon::Resume_prepare_request prepare;
    prepare.context       = request.context;
    prepare.resume_record = request.resume_record;
    for (const bool inspection : {true, false}) {
        const auto operation = inspection ? addon::k_inspect_resume_operation : addon::k_prepare_resume_operation;
        const auto invocation = host.invoke({"interactive-resume", (std::uint16_t)(inspection ? 4 : 2)}, operation,
            inspection ? addon::encode_resume_inspect_request(request) : addon::encode_resume_prepare_request(prepare, 2), 5000);
        const auto outcome = host.wait_outcome(invocation, 6000);
        require(outcome && outcome->status != addon::Host_status::SUCCESS && outcome->reason_code == reason,
            "inspection/preparation did not share exact scope refusal: " + std::string(reason));
    }
}

addon::Json_value native_update(addon::Json_value update)
{
    return json::object({
        {"timestamp", addon::Json_value(1794131439)},
        {"method", addon::Json_value("_x.ai/session/update")},
        {"params", json::object({
            {"sessionId", addon::Json_value(k_session_id)},
            {"update", std::move(update)},
            {"_meta", json::object({
                {"eventId", addon::Json_value(std::string(k_session_id) + "-5")},
                {"agentTimestampMs", addon::Json_value(std::int64_t{1794131439753})}})}})}});
}

void native_summary_contract(
    addon::Host_simulator& host,
    const addon::Resume_inspect_request& request,
    const fs::path& directory)
{
    write(directory / "chat_history.jsonl", k_primary);
    const auto baseline = native_summary(request.context.working_directory);
    // Oracle: Summary's native serde types. In particular context_window is
    // Option<NonZeroU64>; treating zero as valid falsely advertised resumability.
    const std::vector<std::pair<std::string, addon::Json_value>> invalid{
        {"context_window", addon::Json_value(0)},
        {"context_window", addon::Json_value("128000")},
        {"agent_id", addon::Json_value(1)},
        {"num_chat_messages", addon::Json_value()},
        {"cwd_generation", addon::Json_value(-1)},
        {"git_remotes", addon::Json_value(addon::Json_value::Array{addon::Json_value(7)})},
        {"hidden", addon::Json_value("false")},
        {"title_is_manual", addon::Json_value()},
        {"reasoning_effort", addon::Json_value("future")},
        {"last_active_at", addon::Json_value("2026-11-31T09:33:54Z")},
        {"pending_cwd_switch_reminder", json::object({{"cwd_generation", addon::Json_value(0)}})},
        {"future_summary_content", addon::Json_value("retained")},
    };
    for (const auto& [name, value] : invalid) {
        auto summary = baseline;
        summary.set(name, value);
        write(directory / "summary.json", addon::serialize_json(summary));
        const auto result = inspect(host, request);
        require(result.availability == addon::Resume_availability::UNKNOWN &&
                result.content == addon::Resume_content::UNKNOWN,
            "invalid or unsupported native Summary field supplied positive evidence: " + name);
    }
    require(inspect(host, request, 3).availability == addon::Resume_availability::UNKNOWN,
        "version 3 ignored invalid native Summary evidence");
    prepare_exact(host, request);
    auto summary = baseline;
    summary.set("context_window", addon::Json_value(128000));
    summary.set("agent_id", addon::Json_value());
    summary.set("hidden", addon::Json_value(false));
    summary.set("git_remotes", addon::Json_value(addon::Json_value::Array{addon::Json_value("https://example.invalid/repository")}));
    summary.set("reasoning_effort", addon::Json_value("high"));
    write(directory / "summary.json", addon::serialize_json(summary));
    require_content(host, request, addon::Resume_content::EMPTY, "valid native typed summary fields were rejected");
    summary.set("last_recap", addon::Json_value("Retained side conversation recap"));
    write(directory / "summary.json", addon::serialize_json(summary));
    require_content(host, request, addon::Resume_content::UNKNOWN, "retained summary content manufactured empty history");
    write(directory / "summary.json", addon::serialize_json(baseline));
}

void native_content_contract(
    addon::Host_simulator& host,
    const addon::Resume_inspect_request& request,
    const fs::path& directory)
{
    using Content = addon::Resume_content;
    const auto summary_path = directory / "summary.json";
    const auto chat_path    = directory / "chat_history.jsonl";
    const auto updates_path = directory / "updates.jsonl";
    auto summary = native_summary(request.context.working_directory);
    write(summary_path, addon::serialize_json(summary));
    require_content(host, request, Content::UNKNOWN, "summary-only native skeleton was treated as empty");

    write(chat_path, k_primary);
    require_content(host, request, Content::EMPTY, "native primary initialization was retained as conversation content");
    write(chat_path, std::string(k_primary) + std::string(k_reminder));
    require_content(host, request, Content::EMPTY, "baseline skill reminder was retained as conversation content");
    write(updates_path, "");
    require_content(host, request, Content::EMPTY, "empty optional replay stream contradicted complete initialization");
    const auto title = native_update(json::object({
        {"sessionUpdate", addon::Json_value("session_summary_generated")},
        {"session_summary", addon::Json_value("Inspect build settings")}}));
    write(updates_path, addon::serialize_json(title) + '\n');
    require_content(host, request, Content::EMPTY, "presentation title alone made initialized history nonempty");
    // Captured official Grok 1.0.77 (50ca579fa996): session/new with a
    // successful SessionStart observer, then a cold session/load. Text and
    // identities are normalized; the persisted native field schema is intact.
    const auto hook = native_update(json::object({
        {"sessionUpdate", addon::Json_value("hook_execution")},
        {"event_name", addon::Json_value("session_start")},
        {"runs", addon::Json_value(addon::Json_value::Array{json::object({
            {"name", addon::Json_value("global/capture-start:session_start[0].hooks[0]")},
            {"status", json::object({
                {"status", addon::Json_value("success")}, {"elapsed_ms", addon::Json_value(4)}})}})})}}));
    const auto background = native_update(json::object({
        {"sessionUpdate", addon::Json_value("background_tasks")}, {"tasks", addon::Json_value::array()}}));
    summary.set("num_messages", addon::Json_value(1));
    summary.set("num_chat_messages", addon::Json_value(2));
    summary.set("agent_id", addon::Json_value("ag1.c7ba47b9f11147dcad49ccb961de7273"));
    summary.set("attempt_id", addon::Json_value("at1.845c52fc8548018a5c79c57d343db7"));
    summary.set("next_trace_turn", addon::Json_value(0));
    summary.set("last_active_at", addon::Json_value("2026-11-08T09:50:39.753445942Z"));
    summary.set("agent_name", addon::Json_value("grok-build-plan"));
    summary.set("sandbox_profile", addon::Json_value("off"));
    write(summary_path, addon::serialize_json(summary));
    write(updates_path, addon::serialize_json(hook) + '\n');
    require_content(host, request, Content::EMPTY, "ordinary successful SessionStart metadata hid empty initialization");
    summary.set("num_messages", addon::Json_value(3));
    write(summary_path, addon::serialize_json(summary));
    write(updates_path, addon::serialize_json(hook) + '\n' + addon::serialize_json(background) + '\n' +
        addon::serialize_json(hook) + '\n');
    require_content(host, request, Content::EMPTY, "cold-load startup metadata hid empty initialization");
    summary.set("num_messages", addon::Json_value(1));
    write(summary_path, addon::serialize_json(summary));
    auto truncated = background;
    truncated.find("params")->find("update")->set("truncated", addon::Json_value(true));
    write(updates_path, addon::serialize_json(truncated) + '\n');
    require_content(host, request, Content::UNKNOWN, "truncated task snapshot manufactured empty history");
    auto output = hook;
    output.find("params")->find("update")->find("runs")->as_array().front().set("output", addon::Json_value("retained hook output"));
    write(updates_path, addon::serialize_json(output) + '\n');
    require_content(host, request, Content::UNKNOWN, "hook output was discarded as startup metadata");
    auto busy = background;
    busy.find("params")->find("update")->find("tasks")->push_back(json::object({
        {"task_id", addon::Json_value("retained-task")}}));
    write(updates_path, addon::serialize_json(busy) + '\n');
    require_content(host, request, Content::UNKNOWN, "unrecognized background work manufactured empty history");
    summary = native_summary(request.context.working_directory);
    write(summary_path, addon::serialize_json(summary));
    fs::remove(updates_path);

    const std::vector<std::pair<std::string, std::string>> unknown{
        {"zero-byte cache", ""},
        {"unterminated record", "{\"type\":\"system\",\"content\":\"initialization\"}"},
        {"malformed JSON", std::string(k_primary) + "{\"type\":\n"},
        {"unknown record", std::string(k_primary) + "{\"type\":\"future_record\"}\n"},
        {"invalid reason", "{\"type\":\"system\",\"content\":\"prompt\",\"synthetic_reason\":7}\n"},
        {"repeated primary", std::string(k_primary) + std::string(k_primary)},
        {"reminder without primary", std::string(k_reminder)},
        {"future primary field", "{\"type\":\"system\",\"content\":\"prompt\",\"future_content\":\"retained\"}\n"},
    };
    for (const auto& [label, bytes] : unknown) {
        write(chat_path, bytes);
        require_content(host, request, Content::UNKNOWN, label + " manufactured empty content");
    }

    const std::vector<std::string> meaningful{
        "{\"type\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Review the build settings.\"}]}",
        "{\"type\":\"user\",\"content\":[{\"type\":\"image\",\"url\":\"data:image/png;base64,aW1hZ2U=\"}]}",
        "{\"type\":\"assistant\",\"content\":\"The build settings are consistent.\"}",
        "{\"type\":\"tool_result\",\"tool_call_id\":\"read-config\",\"content\":\"configuration\"}",
        "{\"type\":\"user\",\"synthetic_reason\":\"compaction_meta\",\"content\":[{\"type\":\"text\",\"text\":\"Retained work summary\"}]}",
    };
    for (const auto& item : meaningful) {
        write(chat_path, std::string(k_primary) + item + '\n');
        require_content(host, request, Content::NONEMPTY, "retained native conversation content was hidden");
    }
    write(chat_path, std::string(k_primary) + std::string(k_reminder) + std::string(k_reminder) + meaningful.front() + '\n');
    require_content(host, request, Content::NONEMPTY,
        "additional native reminders concealed retained human content");
    write(chat_path, k_primary);
    write(updates_path, "{\"timestamp\":1794123000,\"method\":\"session/update\",\"params\":{\"sessionId\":\"" +
        std::string(k_session_id) + "\",\"update\":{\"sessionUpdate\":\"user_message_chunk\","
        "\"content\":{\"type\":\"text\",\"text\":\"Retain this user input.\"}}}}\n");
    require_content(host, request, Content::NONEMPTY, "companion updates lost retained user input");
    fs::remove(chat_path);
    require_content(host, request, Content::NONEMPTY, "native rebuildable replay content was treated as empty");
    write(chat_path, k_primary);
    write(updates_path, "{\"timestamp\":1794123000,\"method\":\"_x.ai/session/update\",\"params\":{\"sessionId\":\"" +
        std::string(k_session_id) + "\",\"update\":{\"sessionUpdate\":\"compaction_checkpoint\"}}}\n");
    require_content(host, request, Content::UNKNOWN, "uncertain compaction replay manufactured empty history");
    write(updates_path, "{\"timestamp\":1794123000");
    require_content(host, request, Content::UNKNOWN, "incomplete replay manufactured empty history");
    fs::remove(updates_path);
    summary.set("num_messages", addon::Json_value(1));
    write(summary_path, addon::serialize_json(summary));
    require_content(host, request, Content::UNKNOWN, "missing retained replay contradicted by summary count became empty");
    summary.set("num_messages", addon::Json_value(0));
    summary.set("num_chat_messages", addon::Json_value(3));
    write(summary_path, addon::serialize_json(summary));
    require_content(host, request, Content::UNKNOWN, "missing chat records contradicted by summary count became empty");
    summary.set("num_chat_messages", addon::Json_value(0));
    summary.set("chat_format_version", addon::Json_value(0));
    write(summary_path, addon::serialize_json(summary));
    require_content(host, request, Content::UNKNOWN, "unsupported content format manufactured empty history");
    summary.set("chat_format_version", addon::Json_value(7));
    write(summary_path, addon::serialize_json(summary));
    require(inspect(host, request).availability == addon::Resume_availability::UNKNOWN,
        "unknown native persistence format claimed availability");
    write(summary_path, addon::serialize_json(native_summary(request.context.working_directory)));
    write(chat_path, std::string(16 * 1024 * 1024 + 1, 'x'));
    const auto oversized = inspect(host, request);
    require(oversized.availability == addon::Resume_availability::UNKNOWN && oversized.content == Content::UNKNOWN,
        "bounded read supplied positive evidence for oversized history");
    fs::remove(chat_path);
    fs::create_directory(chat_path);
    require(inspect(host, request).availability == addon::Resume_availability::UNKNOWN,
        "nonregular native chat history was advertised as resumable");
    prepare_exact(host, request);
    fs::remove(chat_path);
    write(chat_path, k_primary);

    // /btw is an independently persisted conversation stream; it does not
    // increment the chat/update counters used above.
    const auto btw_path = directory / "btw_history.jsonl";
    write(btw_path, "{\"btwSessionId\":\"btw-1\",\"parentSessionId\":\"" + std::string(k_session_id) +
        "\",\"askedAt\":\"2026-11-08T09:50:39.753445942Z\",\"question\":\"What does this option do?\","
        "\"answer\":\"It selects the native model.\",\"model\":\"native-local-fixture\",\"success\":true,\"attempts\":1}\n");
    require_content(host, request, Content::NONEMPTY, "retained /btw conversation was hidden as empty");
    write(btw_path, "{\"question\":\"incomplete side history\"}\n");
    require_content(host, request, Content::UNKNOWN, "unsupported /btw history manufactured emptiness");
    fs::remove(btw_path);
    write(directory / "feedback.jsonl", "{\"type\":\"user_feedback\",\"submission\":{\"text\":\"retain this feedback\"}}\n");
    require_content(host, request, Content::UNKNOWN, "uninspected side-history stream manufactured emptiness");
    fs::remove(directory / "feedback.jsonl");
    write(directory / "events.jsonl", "");
    require_content(host, request, Content::EMPTY, "empty native telemetry file contradicted complete initialization");
    write(directory / "events.jsonl", "{\"type\":\"turn_started\"}\n");
    require_content(host, request, Content::UNKNOWN, "uninspected native event content manufactured emptiness");
    fs::remove(directory / "events.jsonl");
    fs::create_directories(directory / "compaction_checkpoints");
    require_content(host, request, Content::UNKNOWN, "uninspected compaction checkpoint directory manufactured emptiness");
    fs::remove(directory / "compaction_checkpoints");
}

void native_fixture_contract(
    const fs::path& self,
    const fs::path& package,
    const fs::path& fixture,
    std::string_view expected)
{
    const auto original = native_files(fixture);
    const auto found = original.find("summary.json");
    require(found != original.end(), "native fixture has no summary.json");
    const auto summary = addon::parse_json(found->second, {}, nullptr);
    require(summary.has_value(), "native fixture summary is not JSON");
    const auto& info = json::member(*summary, "info");
    const auto id   = json::text(info, "id");
    const auto cwd  = json::text(info, "cwd");
    require(!id.empty() && !cwd.empty(), "native fixture summary has no exact identity/cwd");
    Temporary_directory temporary;
    const auto profile = temporary.path() / "profile";
    const auto directory = profile / "sessions" / cwd_bucket(cwd) / addon::path_from_utf8(id);
    fs::create_directories(directory);
    for (const auto& entry : fs::directory_iterator(fixture)) {
        // This is the capture helper's annotation, not a native session file.
        if (entry.path().filename() != "provenance.json") {
            fs::copy(entry.path(), directory / entry.path().filename(), fs::copy_options::recursive);
        }
    }
#ifdef _WIN32
    const auto native = temporary.path() / "grok.exe";
#else
    const auto native = temporary.path() / "grok";
#endif
    fs::copy_file(self, native);
    auto request = inspect_request(native, profile, addon::path_from_utf8(cwd));
    request.resume_record.set("provider_session_id", addon::Json_value(id));
    addon::Simulator_options options;
    options.package_directory = package;
    options.validation.platform_id = addon::host_platform_id();
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    require(host.start(&failure), "native fixture package start: " + failure.reason_code);
    const auto copied = native_files(profile);
    const auto result = inspect(host, request);
    const auto actual = result.content == addon::Resume_content::EMPTY ? "empty" :
        result.content == addon::Resume_content::NONEMPTY ? "nonempty" : "unknown";
    require(result.availability == addon::Resume_availability::AVAILABLE && actual == expected,
        "captured native fixture expected available/" + std::string(expected) + ", observed " +
            addon::serialize_json(addon::encode_resume_inspect_result(result, 4)));
    require(inspect(host, request, 3).availability == addon::Resume_availability::AVAILABLE,
        "native fixture version 3 availability differs");
    prepare_exact(host, request);
    require(copied == native_files(profile) && original == native_files(fixture),
        "native fixture inspection/preparation changed captured bytes");
    int exit_code = -1;
    require(host.shutdown(1000, &exit_code) && exit_code == 0, "native fixture adapter shutdown failed");
    std::cout << "native fixture " << id << ": available/" << actual << "; captured bytes unchanged\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 && !(argc == 5 && std::string_view(argv[2]) == "--native-fixture")) {
        std::cerr << "usage: grok_resume_tests <package directory> [--native-fixture <session directory> <empty|nonempty|unknown>]\n";
        return 2;
    }
    try {
        if (argc == 5) {
            native_fixture_contract(fs::absolute(argv[0]), addon::path_from_utf8(argv[1]),
                addon::path_from_utf8(argv[3]), argv[4]);
            return 0;
        }
        Temporary_directory temporary;
        const auto& root = temporary.path();
        const auto profile = root / "selected profile";
        const auto cwd     = root / "repo" / "nested folder";
        fs::create_directories(cwd);
#ifdef _WIN32
        const auto native = root / "grok.exe";
#else
        const auto native = root / "grok";
#endif
        fs::copy_file(fs::absolute(argv[0]), native);
        auto request = inspect_request(native, profile, cwd);
        const auto directory = profile / "sessions" / cwd_bucket(addon::path_to_utf8(cwd)) /
            addon::path_from_utf8(k_session_id);
        addon::Simulator_options options;
        options.package_directory = addon::path_from_utf8(argv[1]);
        options.validation.platform_id = addon::host_platform_id();
        addon::Host_simulator host(options, nullptr);
        addon::Contract_failure failure;
        require(host.start(&failure), "package start: " + failure.reason_code + " " + failure.detail);
        const auto capabilities = host.live_capabilities();
        for (const std::uint16_t version : {2, 3, 4}) {
            require(std::find(capabilities.begin(), capabilities.end(),
                    addon::Capability_pair{"interactive-resume", version}) != capabilities.end(),
                "packaged adapter did not negotiate its declared resume capability");
        }
        require(inspect(host, request).availability == addon::Resume_availability::UNKNOWN,
            "missing local profile was mislabeled absent");
        prepare_exact(host, request);
        native_content_contract(host, request, directory);
        native_summary_contract(host, request, directory);

        const auto before = native_files(profile);
        const auto current = inspect(host, request);
        require(current.content == addon::Resume_content::EMPTY, "known initialized content not recognized");
        const auto older = inspect(host, request, 3);
        require(older.availability == addon::Resume_availability::AVAILABLE &&
                older.content == addon::Resume_content::UNKNOWN,
            "version 3 did not retain its availability-only contract");
        prepare_exact(host, request);
        require(before == native_files(profile), "inspection or preparation modified native history");

        auto invalid = request;
        invalid.resume_record.set("unexpected", addon::Json_value("another conversation"));
        require_shared_refusal(host, invalid, "invalid_resume_record");
        invalid = request;
        invalid.context.account_context->adapter_account_key = grok::profile_account_key(addon::path_to_utf8(root / "wrong account"));
        require_shared_refusal(host, invalid, "account_context_mismatch");
        invalid = request;
        invalid.context.launcher.script_body += " --resume another-session";
        invalid.context.launcher_argv.insert(invalid.context.launcher_argv.end(), {"--resume", "another-session"});
        require_shared_refusal(host, invalid, "resume_identity_option_conflict");

        auto relative = request;
        relative.context.launch_environment->effective_environment.set("GROK_HOME",
            addon::Json_value(addon::path_to_utf8(fs::relative(profile, cwd))));
        require(inspect(host, relative).content == addon::Resume_content::EMPTY,
            "relative GROK_HOME was not resolved against the requested launch cwd");
        prepare_exact(host, relative);

        auto moved = request;
        moved.context.working_directory = addon::path_to_utf8(root);
        require(inspect(host, moved).content == addon::Resume_content::EMPTY,
            "exact native lookup incorrectly required launch cwd to equal stored cwd");
        require(!addon::validate_resume_inspection(moved, current, nullptr),
            "inspection acknowledgment did not bind the complete resolved context");

        auto other = request;
        const auto other_home = addon::path_to_utf8(root / "other profile");
        other.context.account_context = addon::Account_context{2, grok::profile_account_key(other_home), other_home};
        other.context.launch_environment->effective_environment.set("GROK_HOME", addon::Json_value(other_home));
        require(inspect(host, other).availability == addon::Resume_availability::UNKNOWN,
            "inspection crossed the selected account's native storage scope");
        prepare_exact(host, other);

        auto alternative = request;
        alternative.context.launcher.script_body += " --chat";
        alternative.context.launcher_argv.push_back("--chat");
        require(inspect(host, alternative).availability == addon::Resume_availability::UNKNOWN,
            "alternate native lookup mode used local storage as authoritative evidence");
        prepare_exact(host, alternative);

        const auto alias_bucket = profile / "sessions" / "unverified-cwd-marker";
        fs::rename(directory.parent_path(), alias_bucket);
        write(alias_bucket / ".cwd", request.context.working_directory);
        require(inspect(host, request).availability == addon::Resume_availability::UNKNOWN,
            "a cwd marker advertised a directory native load would not recompute");
        prepare_exact(host, request);
        fs::remove(alias_bucket / ".cwd");
        fs::rename(alias_bucket, directory.parent_path());

        const auto duplicate = profile / "sessions" / cwd_bucket(addon::path_to_utf8(root)) /
            addon::path_from_utf8(k_session_id);
        write(duplicate / "summary.json", addon::serialize_json(native_summary(addon::path_to_utf8(root))));
        require(inspect(host, request).availability == addon::Resume_availability::UNKNOWN,
            "duplicate native identity was selected by directory order");
        fs::remove_all(duplicate.parent_path());
        auto wrong = native_summary(request.context.working_directory);
        wrong.find("info")->set("id", addon::Json_value("87d4155d-df9c-4ccc-a69b-900d555916e2"));
        write(directory / "summary.json", addon::serialize_json(wrong));
        require(inspect(host, request).availability == addon::Resume_availability::UNKNOWN,
            "mismatched persisted identity claimed availability");
        fs::remove_all(directory);
        require(inspect(host, request).availability == addon::Resume_availability::UNKNOWN,
            "deleted local evidence incorrectly ruled out native nonlocal resolution");
        prepare_exact(host, request);

        int exit_code = -1;
        require(host.shutdown(1000, &exit_code) && exit_code == 0, "packaged adapter did not shut down cleanly");
        // A host adopting only @2/@3 must not receive a @4-only result member.
        options.adopted_capabilities = std::vector<addon::Capability_pair>{{"interactive-resume", 2}, {"interactive-resume", 3}};
        addon::Host_simulator older_host(options, nullptr);
        require(older_host.start(&failure), "version 3 host did not negotiate");
        require(inspect(older_host, request, 3).availability == addon::Resume_availability::UNKNOWN,
            "version 3 host did not retain conservative unknown evidence");
        require(older_host.shutdown(1000, &exit_code) && exit_code == 0, "version 3 adapter did not shut down cleanly");
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
