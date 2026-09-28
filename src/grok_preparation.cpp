#include "grok_preparation.h"
#include "grok_accounts.h"
#include "grok_acp.h"
#include "grok_launcher.h"
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <algorithm>
#include <regex>
namespace grok {
namespace addon = logonomic::cli_addon;
namespace {
bool environment_name_equal(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
#ifdef _WIN32
        const auto fold = [](char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + 'a' - 'A') : ch; };
        if (fold(left[i]) != fold(right[i])) return false;
#else
        if (left[i] != right[i]) return false;
#endif
    }
    return true;
}
bool interactive_arguments(const std::vector<std::string>& arguments)
{
    // A subcommand or single-shot request has a different lifetime and output
    // contract. Preserve all native interactive option values verbatim.
    const std::vector<std::string> values{
        "--model", "-m", "--cwd", "--rules", "--append-system-prompt", "--system-prompt",
        "--resume", "-r", "--load", "--session-id", "-s", "--agent", "--agents",
        "--allow", "--deny", "--allowedTools", "--disallowedTools", "--permission-mode",
        "--reasoning-effort", "--effort", "--profile", "--debug-file", "--leader-socket"};
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& argument = arguments[i];
        const auto option = argument.substr(0, argument.find('='));
        if (option == "--single" || option == "--print" || option == "-p" ||
            option == "--prompt-file" || option == "--prompt-json" || option == "--version" ||
            option == "--help" || option == "-h" || option == "-v" || option == "-V" ||
            option == "--leader" || argument.empty() || argument[0] != '-') return false;
        if (std::find(values.begin(), values.end(), option) != values.end() && argument.find('=') == std::string::npos) {
            if (i + 1 == arguments.size() || arguments[i + 1].starts_with('-')) {
                if (option == "--resume" || option == "-r") continue;
                return false;
            }
            ++i;
        }
    }
    return true;
}
}
void prepare_delegated(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const bool structured = context.request().capability.capability_id == "structured-session";
    std::optional<addon::Delegated_prepare_request> request;
    if (structured) {
        const auto input = addon::decode_structured_prepare_request(context.request().payload, &failure, 2);
        if (input) {
            request.emplace();
            request->purpose = input->purpose;
            request->context = input->context;
            request->prompt = input->prompt;
        }
    }
    else request = addon::decode_delegated_prepare_request(context.request().payload, &failure, 2);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
    if (!structured && !request->control.connection) {
        context.fail(addon::Result_status::UNAVAILABLE, "task_server_connection_unavailable");
        return;
    }
    if (request->purpose != addon::Structured_purpose::IMPLEMENTATION) {
        context.fail(addon::Result_status::UNAVAILABLE, "purpose_unavailable"); return;
    }
    const auto command = recognize_launcher(request->context.launcher.script_body);
    const auto home = launch_profile_home(request->context, context);
    if (!home) return;
    if (!command) { context.fail(addon::Result_status::UNAVAILABLE, "launcher_unavailable"); return; }
    const auto assignments = launcher_environment(*command, request->context.launch_environment->effective_environment);
    if (!assignments) { context.fail(addon::Result_status::UNAVAILABLE, "launcher_environment_unavailable"); return; }
    std::vector<std::string> arguments{"agent", "--no-leader"};
    for (std::size_t i = 0; i < command->prefix_arguments.size(); ++i) {
        const auto& option = command->prefix_arguments[i];
        // The launcher's own approval posture carries over; grok agent has no
        // equivalent of the interactive permission mode or rule options.
        if (option == "--always-approve") {
            arguments.push_back(option);
            continue;
        }
        if (option != "--model" && option != "-m" && option != "--reasoning-effort" && option != "--effort") {
            context.fail(addon::Result_status::UNAVAILABLE, "delegated_launcher_options_unavailable",
                "Grok runs a delegated task in its agent mode, which takes only --model, --reasoning-effort and "
                "--always-approve from a launcher; the launcher's " + option.substr(0, option.find('=')) +
                " has no agent-mode equivalent.");
            return;
        }
        if (++i == command->prefix_arguments.size()) {
            context.fail(addon::Result_status::FAILED, "invalid_launcher_option"); return;
        }
        arguments.push_back(option);
        arguments.push_back(command->prefix_arguments[i]);
    }
    arguments.push_back("stdio");
    const auto argv = resolve_probe_launcher(*command, arguments,
        request->context.launch_environment->effective_environment, request->context.working_directory);
    if (!argv) { context.fail(addon::Result_status::UNAVAILABLE, "launcher_program_unavailable"); return; }
    addon::Delegated_prepare_result result;
#ifdef _WIN32
    result.process.executable = "helpers/grok_launch_bridge.exe";
#else
    result.process.executable = "helpers/grok_launch_bridge";
#endif
    result.process.argv = *argv;
    result.process.stdin_behavior = addon::Stdin_behavior::PAYLOAD;
    result.process.stdin_payload = acp_initialize();
    result.process.environment = *assignments;
    std::erase_if(result.process.environment, [](const auto& item) { return environment_name_equal(item.first, "GROK_HOME"); });
    result.process.environment.emplace_back("GROK_HOME", *home);
    for (const auto& member : request->context.launch_environment->effective_environment.as_object()) {
        const bool assigned = std::any_of(result.process.environment.begin(), result.process.environment.end(),
            [&](const auto& item) { return environment_name_equal(item.first, member.name); });
        if (!assigned) {
            result.process.environment_names.push_back(member.name);
        }
    }
    result.inspection.incremental = true;
    result.inspection.keep_stdin_open = true;
    result.inspection.protocol_state = acp_initial_state(*request);
    if (!structured) {
        result.acknowledgment.connection_version = addon::k_delegated_connection_version;
    }
    result.profile_storage = request->context.profile_storage;
    result.account_context = request->context.account_context;
    for (const auto& server : request->control.servers) result.acknowledgment.server_ids.push_back(server.server_id);
    addon::Json_value payload;
    if (structured) {
        addon::Structured_prepare_result prepared;
        prepared.process = std::move(result.process);
        prepared.inspection = std::move(result.inspection);
        prepared.profile_storage = result.profile_storage;
        prepared.account_context = result.account_context;
        payload = addon::encode_structured_prepare_result(prepared, 2);
    }
    else payload = addon::encode_delegated_prepare_result(result, 2);
    if (!addon::validate_session_exchange_payload(payload, &failure)) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return;
    }
    context.succeed(std::move(payload));
}
void prepare_resume(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_resume_prepare_request(context.request().payload, &failure, 2);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
    const auto* id = request->resume_record.find("provider_session_id");
    static const std::regex uuid("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}");
    if (!request->resume_record.is_object() || request->resume_record.as_object().size() != 1 ||
        !id || !id->is_string() || !std::regex_match(id->as_string(), uuid)) {
        context.fail(addon::Result_status::FAILED, "invalid_resume_record"); return;
    }
    const auto command = recognize_launcher(request->context.launcher.script_body);
    if (!command || !interactive_arguments(command->prefix_arguments)) {
        context.fail(addon::Result_status::UNAVAILABLE, "resume_launcher_unavailable"); return;
    }
    for (const auto& argument : command->prefix_arguments) {
        const auto option = argument.substr(0, argument.find('='));
        if (option == "--cwd" || option == "--resume" || option == "-r" || option == "--load" ||
            option == "--continue" || option == "-c" || option == "--session-id" || option == "-s" || option == "--fork-session") {
            context.fail(addon::Result_status::UNAVAILABLE, "resume_identity_option_conflict"); return;
        }
    }
    const auto home = launch_profile_home(request->context, context);
    if (!home) return;
    const auto assignments = launcher_environment(*command, request->context.launch_environment->effective_environment);
    if (!assignments) { context.fail(addon::Result_status::UNAVAILABLE, "launcher_environment_unavailable"); return; }
    auto arguments = command->prefix_arguments;
    arguments.insert(arguments.end(), {"--no-leader", "--resume", id->as_string()});
    const auto argv = resolve_probe_launcher(*command, arguments,
        request->context.launch_environment->effective_environment, request->context.working_directory);
    if (!argv) { context.fail(addon::Result_status::UNAVAILABLE, "launcher_program_unavailable"); return; }
    addon::Resume_prepare_result result;
#ifdef _WIN32
    result.process.executable = "helpers/grok_launch_bridge.exe";
#else
    result.process.executable = "helpers/grok_launch_bridge";
#endif
    result.process.argv = *argv;
    result.process.stdin_behavior = addon::Stdin_behavior::INHERIT;
    result.patch.set = *assignments;
    std::erase_if(result.patch.set, [](const auto& item) { return environment_name_equal(item.first, "GROK_HOME"); });
    result.patch.set.emplace_back("GROK_HOME", *home);
    result.patch.profile_storage = request->context.profile_storage;
    result.patch.account_context = request->context.account_context;
    result.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(request->resume_record);
    context.succeed(addon::encode_resume_prepare_result(result, 2));
}
void handle_preparation(addon::Invocation_context& context)
{
    if (context.request().capability.capability_id == "interactive-resume" &&
        context.request().operation == addon::k_prepare_resume_operation) { prepare_resume(context); return; }

    if ((context.request().capability.capability_id == "delegated-session" ||
         context.request().capability.capability_id == "structured-session") &&
        context.request().operation == addon::k_prepare_operation) { prepare_delegated(context); return; }
    if (context.request().capability.capability_id == "structured-session" &&
        context.request().operation == addon::k_inspect_operation) {
        addon::Contract_failure failure;
        const auto request = addon::decode_structured_inspect_request(context.request().payload, &failure, 2);
        if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
        if (request->purpose != addon::Structured_purpose::IMPLEMENTATION) {
            context.fail(addon::Result_status::UNAVAILABLE, "purpose_unavailable"); return;
        }
        context.succeed(addon::encode_structured_inspect_result(inspect_acp(request->inspection), 2)); return;
    }

    if (context.request().capability.capability_id != "launch-preparation" ||
        context.request().operation != addon::k_prepare_operation) {
        context.reject_unknown_operation(); return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_prepare_request(context.request().payload, &failure, 3);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
    const auto command = recognize_launcher(request->launcher.script_body);
    addon::Launch_patch patch;
    patch.account_context = request->account_context;
    patch.profile_storage = request->profile_storage;
    if (!command || !interactive_arguments(command->prefix_arguments)) {
        patch.disposition = addon::Launch_disposition{false, false, false};
        context.succeed(addon::encode_launch_patch(patch, 3)); return;
    }
    addon::Session_launch_context account;
    account.launcher = request->launcher;
    account.launcher_argv = request->launcher_argv;
    account.working_directory = request->working_directory;
    account.account_context = request->account_context;
    account.launch_environment = request->launch_environment;
    const auto profile = launch_profile_home(account, context);
    if (!profile) return;
    const auto assignments = launcher_environment(*command, request->launch_environment->effective_environment);
    if (!assignments) { context.fail(addon::Result_status::UNAVAILABLE, "launcher_environment_unavailable"); return; }
    const auto program = resolve_probe_launcher(*command, {},
        request->launch_environment->effective_environment, request->working_directory);
    if (!program) { context.fail(addon::Result_status::UNAVAILABLE, "launcher_program_unavailable"); return; }
    Environment environment;
    for (const auto& member : request->launch_environment->effective_environment.as_object())
        environment.emplace_back(member.name, member.value.as_string());
    auto probe = *program;
    probe.push_back("--help");
    const auto help = run_probe(probe, environment, request->working_directory, 3000, context);
    if (!help || help->output.find("--session-id") == std::string::npos || help->output.find("--resume") == std::string::npos) {
        context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            "interactive_cli_contract_unavailable"); return;
    }
    auto argv = *program;
    // Standalone execution keeps this launch out of an unrelated persistent
    // leader and makes its environment visible to its own native hooks.
    argv.push_back("--no-leader");
    argv.insert(argv.end(), command->prefix_arguments.begin(), command->prefix_arguments.end());
    patch.argv = std::move(argv);
    patch.set = *assignments;
    std::erase_if(patch.set, [](const auto& item) { return environment_name_equal(item.first, "GROK_HOME"); });
    patch.set.emplace_back("GROK_HOME", *profile);
    patch.apply_managed_configuration = true;
    patch.disposition = addon::Launch_disposition{true, true, true};
    context.succeed(addon::encode_launch_patch(patch, 3));
}
} // namespace grok
