#include "grok_configuration.h"
#include "grok_accounts.h"
#include "grok_preparation.h"
#include "grok_launcher.h"
#include "grok_launcher_approval.h"
#include "grok_lifecycle.h"
#include "grok_wake.h"
#include <logonomic_cli_addon/json.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/managed_configuration.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
namespace addon = logonomic::cli_addon;
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
void launcher_approval_contract()
{
    const auto& rules = grok::launcher_approval_rules();
    const auto yolo = addon::Launcher_approval_action::YOLOFY;
    const auto undo = addon::Launcher_approval_action::UNYOLOFY;
    const auto changed = addon::Launcher_approval_disposition::CHANGED;
    const auto windows = addon::transform_launcher_approval({
        {"grok", "powershell", ""}, "windows-x86_64", yolo}, rules);
    require(windows.disposition == changed && windows.script_body == "grok --always-approve",
        "discovered Windows Grok launcher supports Yolofy");
    for (const auto* alias : {"--yolo", "--dangerously-skip-permissions"}) {
        const auto result = addon::transform_launcher_approval({
            {std::string("grok ") + alias, "direct", ""}, "linux-x86_64", yolo}, rules);
        require(result.disposition == changed && result.script_body == "grok --always-approve",
            "Grok aliases converge on its canonical approval argument");
    }
    const std::string mode_options =
        R"args( --deny "Bash(rm *)" --rules "keep --permission-mode bypassPermissions")args";
    const std::string configured = "grok" + mode_options;
    const auto comparison = addon::launcher_approval_comparison_key(
        {{configured, "direct", "PROFILE=work"}, "linux-x86_64"}, rules);
    for (const auto* mode : {"--permission-mode bypassPermissions", "--permission-mode=bypassPermissions"}) {
        const std::string source = std::string("grok ") + mode + mode_options;
        const auto enabled = addon::transform_launcher_approval(
            {{source, "direct", "PROFILE=work"}, "linux-x86_64", yolo}, rules);
        require(enabled.disposition == changed && enabled.script_body == "grok --always-approve" + mode_options,
            "Grok bypass permission mode normalizes while deny rules and instructions survive");
        const auto repeated = addon::transform_launcher_approval(
            {{enabled.script_body, "direct", "PROFILE=work"}, "linux-x86_64", yolo}, rules);
        require(repeated.disposition == addon::Launcher_approval_disposition::UNCHANGED &&
            repeated.script_body == enabled.script_body, "Grok permission-mode normalization is idempotent");
        const auto disabled = addon::transform_launcher_approval(
            {{source, "direct", "PROFILE=work"}, "linux-x86_64", undo}, rules);
        require(disabled.disposition == changed && disabled.script_body == configured,
            "Unyolofy removes the Grok bypass permission mode without replacing policy");
        const auto removed_again = addon::transform_launcher_approval(
            {{disabled.script_body, "direct", "PROFILE=work"}, "linux-x86_64", undo}, rules);
        require(removed_again.disposition == addon::Launcher_approval_disposition::UNCHANGED &&
            removed_again.script_body == configured, "Grok permission-mode removal is idempotent");
        const auto mode_comparison = addon::launcher_approval_comparison_key(
            {{source, "direct", "PROFILE=work"}, "linux-x86_64"}, rules);
        require(comparison.supported && mode_comparison.supported && comparison.key == mode_comparison.key,
            "Grok bypass permission modes retain the same rescan identity");
    }
    const std::string ordinary = "grok --permission-mode auto" + mode_options;
    const auto preserve_mode = addon::transform_launcher_approval(
        {{ordinary, "direct", ""}, "linux-x86_64", undo}, rules);
    require(preserve_mode.disposition == addon::Launcher_approval_disposition::UNCHANGED &&
        preserve_mode.script_body == ordinary, "Unyolofy preserves the user's non-bypass Grok permission mode");
    const auto replace_mode = addon::transform_launcher_approval(
        {{ordinary, "direct", ""}, "linux-x86_64", yolo}, rules);
    require(replace_mode.disposition == changed && replace_mode.script_body == "grok --always-approve" + mode_options,
        "Yolofy replaces an understood Grok permission mode without changing rules");
    const std::string policy = R"args(--deny "Bash(rm *)" --rules "keep --yolo as text")args";
    const auto removed = addon::transform_launcher_approval({
        {"grok --always-approve --yolo --dangerously-skip-permissions " + policy, "direct", ""},
        "linux-x86_64", undo}, rules);
    require(removed.disposition == changed && removed.script_body == "grok " + policy,
        "Grok deny rules and instructions survive removal of every bypass alias");
    const std::string prompt = R"(grok -p "--always-approve")";
    const auto literal = addon::transform_launcher_approval({
        {prompt, "direct", ""}, "linux-x86_64", undo}, rules);
    require(literal.disposition == addon::Launcher_approval_disposition::UNCHANGED &&
        literal.script_body == prompt, "Grok headless prompt values are not approval flags");
    const auto management = addon::transform_launcher_approval({
        {"grok login", "direct", ""}, "linux-x86_64", yolo}, rules);
    require(management.disposition == addon::Launcher_approval_disposition::UNSUPPORTED,
        "Grok account management remains unchanged");
}

void native_launcher_options_contract()
{
    // PagerArgs in xai-org/grok-build at 2bdd1d6a6369de0e8c68132ea4539e9abd9e14a8
    // declares these options and aliases. Approval edits must retain their text.
    const auto& rules = grok::launcher_approval_rules();
    for (const std::string options : {
        "--no-leader",
        "--reasoning-effort high",
        "--reasoning-effort=high",
        "--worktree feature --worktree-ref main",
        "--append-system-prompt \"keep --always-approve as text\"",
        "--system-prompt=\"keep --always-approve as text\"",
        "--allowedTools \"Read,Edit\"",
        "--disallowedTools=\"Bash(rm *)\""})
    {
        const std::string source = "grok " + options;
        const auto enabled = addon::transform_launcher_approval(
            {{source, "direct", "PROFILE=work"}, "linux-x86_64", addon::Launcher_approval_action::YOLOFY}, rules);
        require(enabled.disposition == addon::Launcher_approval_disposition::CHANGED &&
            enabled.script_body == "grok --always-approve " + options,
            "Yolofy must recognize native options and preserve their values");
        const auto repeated = addon::transform_launcher_approval(
            {{enabled.script_body, "direct", "PROFILE=work"}, "linux-x86_64", addon::Launcher_approval_action::YOLOFY}, rules);
        require(repeated.disposition == addon::Launcher_approval_disposition::UNCHANGED &&
            repeated.script_body == enabled.script_body, "Native option approval editing must be idempotent");
        const auto disabled = addon::transform_launcher_approval(
            {{enabled.script_body, "direct", "PROFILE=work"}, "linux-x86_64", addon::Launcher_approval_action::UNYOLOFY}, rules);
        require(disabled.disposition == addon::Launcher_approval_disposition::CHANGED && disabled.script_body == source,
            "Unyolofy must retain native options and approval-looking text inside their values");
        const auto plain_key = addon::launcher_approval_comparison_key(
            {{source, "direct", "PROFILE=work"}, "linux-x86_64"}, rules);
        const auto approval_key = addon::launcher_approval_comparison_key(
            {{enabled.script_body, "direct", "PROFILE=work"}, "linux-x86_64"}, rules);
        require(plain_key.supported && approval_key.supported && plain_key.key == approval_key.key,
            "Native option spelling must retain approval-independent launcher identity");
    }
    const auto unknown = addon::transform_launcher_approval(
        {{"grok --unknown-future-option value", "direct", ""}, "linux-x86_64",
            addon::Launcher_approval_action::YOLOFY}, rules);
    require(unknown.disposition == addon::Launcher_approval_disposition::UNSUPPORTED,
        "Unrecognized native options must still refuse approval editing");
}

addon::Classify_event_request event(std::string name)
{
    addon::Classify_event_request request;
    request.event.name = name;
    request.event.body.set("hook_event_name", addon::Json_value(name));
    request.event.body.set("sessionId", addon::Json_value("8e18215e-f612-4131-95dd-7890139a41ae"));
    request.event.body.set("promptId", addon::Json_value("prompt-1"));
    return request;
}
class Context final : public addon::Invocation_context
{
public:
    addon::Invoke_request input;
    addon::Json_value output;
    bool succeeded = false;
    bool done = false;
    addon::Result_status status = addon::Result_status::FAILED;
    std::string detail;
    const addon::Invoke_request& request() const override { return input; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int timeout_ms) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return false;
    }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value value) override { succeeded = true; done = true; output = std::move(value); }
    void fail(addon::Result_status result, std::string reason, std::string text) override
    {
        done = true;
        status = result;
        output = addon::Json_value(std::move(reason));
        detail = std::move(text);
    }
    bool finished() const override { return done; }
};

void standalone_launch_contract(const addon::Prepare_request& base)
{
    // Native PagerArgs::no_leader is a clap SetTrue flag with no self override.
    // Add one selector when absent; preserve authored duplicates for native rejection.
    const std::vector<std::pair<std::vector<std::string>, std::size_t>> cases{
        {{}, 1},
        {{"--no-leader"}, 1},
        {{"--no-leader", "--no-leader"}, 2},
        {{"--rules=--no-leader"}, 1},
        {{"--model", "grok-4.6", "--no-leader", "--effort", "high"}, 1},
    };
    for (const auto& [arguments, expected_count] : cases) {
        auto request = base;
        const auto program = base.launcher_argv.front();
        request.launcher.script_body = "\"" + program + "\"";
        request.launcher_argv = {program};
        for (const auto& argument : arguments) {
            request.launcher.script_body += " \"" + argument + "\"";
            request.launcher_argv.push_back(argument);
        }
        Context prepared;
        prepared.input = {"standalone", {"launch-preparation", 3}, "prepare",
            addon::encode_prepare_request(request, 3)};
        grok::handle_preparation(prepared);
        const auto patch = addon::decode_launch_patch(prepared.output, nullptr, 3);
        require(prepared.succeeded && patch && patch->argv && patch->apply_managed_configuration,
            "Standalone fixture must produce a managed native launch");
        require(static_cast<std::size_t>(std::count(patch->argv->begin(), patch->argv->end(), "--no-leader")) == expected_count,
            "Ordinary preparation must add --no-leader only when absent as an option");
        require(patch->argv->size() >= arguments.size() + 1 &&
            std::equal(arguments.rbegin(), arguments.rend(), patch->argv->rbegin()),
            "Ordinary preparation must preserve the authored native arguments");

        addon::Resume_prepare_request resume;
        resume.context.launcher = request.launcher;
        resume.context.launcher_argv = request.launcher_argv;
        resume.context.working_directory = request.working_directory;
        resume.context.launch_environment = request.launch_environment;
        resume.context.account_context = request.account_context;
        const std::string session_id = "8e18215e-f612-4131-95dd-7890139a41ae";
        resume.resume_record.set("provider_session_id", addon::Json_value(session_id));
        Context resumed;
        resumed.input = {"standalone-resume", {"interactive-resume", 2}, "prepare_resume",
            addon::encode_resume_prepare_request(resume, 2)};
        grok::handle_preparation(resumed);
        const auto result = addon::decode_resume_prepare_result(resumed.output, nullptr, 2);
        require(resumed.succeeded && result, "Standalone fixture must produce an exact native resume");
        const auto& resumed_argv = result->process.argv;
        require(static_cast<std::size_t>(std::count(resumed_argv.begin(), resumed_argv.end(), "--no-leader")) == expected_count,
            "Resume preparation must add --no-leader only when absent as an option");
        require(resumed_argv.size() >= arguments.size() + 3 &&
            std::equal(arguments.begin(), arguments.end(), resumed_argv.begin() + 1) &&
            resumed_argv[resumed_argv.size() - 2] == "--resume" && resumed_argv.back() == session_id,
            "Resume preparation must preserve authored arguments and the exact session identity");
    }
}

void launcher_argument_semantics_contract(const addon::Prepare_request& base)
{
    struct Launcher_case
    {
        std::string runner;
        std::string options;
        std::vector<std::string> arguments;
    };
    const std::vector<Launcher_case> cases{
        {addon::k_runner_direct, R"( --append-system-prompt """quoted""")",
            {"--append-system-prompt", "\"quoted\""}},
        {addon::k_runner_bash, R"( --append-system-prompt "one\\two")",
            {"--append-system-prompt", R"(one\two)"}},
        {addon::k_runner_powershell, R"( --append-system-prompt 'Don''t guess')",
            {"--append-system-prompt", "Don't guess"}},
    };
    for (const auto& item : cases) {
        auto request = base;
        request.launcher.runner_key = item.runner;
        const auto& program = base.launcher_argv.front();
        const auto script_program = item.runner == addon::k_runner_bash
            ? std::filesystem::path(program).generic_string() : program;
        request.launcher.script_body =
            (item.runner == addon::k_runner_powershell ? "& \"" : "\"") + script_program + "\"" + item.options;
        if (item.runner == addon::k_runner_direct) {
            request.launcher_argv = {program};
            request.launcher_argv.insert(request.launcher_argv.end(), item.arguments.begin(), item.arguments.end());
        }
        else
        if (item.runner == addon::k_runner_bash) {
            request.launcher_argv = {"bash",
                request.launch_environment->platform_id.starts_with("linux-") ? "-c" : "-lc",
                request.launcher.script_body};
        }
        else {
            // Pure Windows runner projection; this fixture never starts PowerShell.
            request.launch_environment->platform_id = "windows-x86_64";
            request.launcher_argv = {"powershell.exe", "-NoLogo", "-Command", request.launcher.script_body};
        }
        Context prepared;
        prepared.input = {"literal", {"launch-preparation", 3}, "prepare",
            addon::encode_prepare_request(request, 3)};
        grok::handle_preparation(prepared);
        const auto patch = addon::decode_launch_patch(prepared.output, nullptr, 3);
        require(prepared.succeeded && patch && patch->argv && patch->apply_managed_configuration &&
            patch->argv->size() == item.arguments.size() + 2 &&
            std::equal(item.arguments.begin(), item.arguments.end(), patch->argv->begin() + 2),
            "Managed preparation must preserve the selected runner's native argument bytes");

        addon::Resume_prepare_request resume;
        resume.context.launcher = request.launcher;
        resume.context.launcher_argv = request.launcher_argv;
        resume.context.working_directory = request.working_directory;
        resume.context.launch_environment = request.launch_environment;
        resume.context.account_context = request.account_context;
        resume.resume_record.set("provider_session_id", addon::Json_value("8e18215e-f612-4131-95dd-7890139a41ae"));
        Context resumed;
        resumed.input = {"literal-resume", {"interactive-resume", 2}, "prepare_resume",
            addon::encode_resume_prepare_request(resume, 2)};
        grok::handle_preparation(resumed);
        const auto result = addon::decode_resume_prepare_result(resumed.output, nullptr, 2);
        require(resumed.succeeded && result && result->process.argv.size() == item.arguments.size() + 4 &&
            std::equal(item.arguments.begin(), item.arguments.end(), result->process.argv.begin() + 1),
            "Resume must use the same runner-aware recognition and preserve native values");

#if defined(__linux__)
        if (item.runner == addon::k_runner_bash) {
            auto environment = addon::environment_pairs(request.launch_environment->effective_environment);
            addon::set_environment(environment, "GROK_ARGUMENT_CAPTURE", "1");
            auto shell_argv = request.launcher_argv;
            const auto bash = addon::resolve_program("bash", environment, request.working_directory);
            require(bash.has_value(), "The Linux native-word oracle requires bash");
            shell_argv.front() = addon::path_to_utf8(*bash);
            const auto native = grok::run_probe(shell_argv, environment, request.working_directory, 1000, prepared);
            const auto words = native ? addon::parse_json(native->output, {}, nullptr) : std::nullopt;
            auto expected = addon::Json_value::array();
            for (const auto& argument : item.arguments) {
                expected.push_back(addon::Json_value(argument));
            }
            require(words && *words == expected, "The parser's expected Bash words must agree with native Bash");
        }
#endif
        auto mismatch = request;
        mismatch.launcher_argv.back() += " changed";
        Context unmanaged;
        unmanaged.input = {"mismatched-host", {"launch-preparation", 3}, "prepare",
            addon::encode_prepare_request(mismatch, 3)};
        grok::handle_preparation(unmanaged);
        const auto retained = addon::decode_launch_patch(unmanaged.output, nullptr, 3);
        require(unmanaged.succeeded && retained && !retained->argv && !retained->apply_managed_configuration &&
            retained->disposition && !retained->disposition->prepared,
            "Contradictory host evidence must preserve the original unmanaged invocation");
        resume.context.launcher_argv = mismatch.launcher_argv;
        Context refused;
        refused.input = {"mismatched-resume", {"interactive-resume", 2}, "prepare_resume",
            addon::encode_resume_prepare_request(resume, 2)};
        grok::handle_preparation(refused);
        require(!refused.succeeded && refused.status == addon::Result_status::UNAVAILABLE,
            "Resume must refuse contradictory host invocation evidence");
    }
}

// The public structured and delegated entry points share agent preparation.
// Equivalent native spellings must keep values, order and approval unchanged.
void agent_option_forms_contract(const Context& base, const std::string& script_body)
{
    const auto prepare_with = [&](const std::string& options) {
        Context prepared;
        prepared.input = base.input;
        const std::string script = script_body + options;
        prepared.input.payload.find("launcher")->set("script_body", addon::Json_value(script));
        prepared.input.payload.find("launcher_argv")->as_array().back() = addon::Json_value(script);
        grok::handle_preparation(prepared);
        return prepared;
    };
    const auto baseline = addon::decode_process_proposal(*base.output.find("process"), nullptr);
    require(baseline && !baseline->argv.empty() && baseline->argv.back() == "stdio",
        "agent option fixture must start with the existing stdio proposal");
    const auto check = [&](const std::string& options, const std::vector<std::string>& forwarded) {
        const auto prepared = prepare_with(options);
        require(prepared.succeeded, "supported agent option spelling was refused");
        const auto* value = prepared.output.find("process");
        const auto process = value ? addon::decode_process_proposal(*value, nullptr) : std::nullopt;
        auto expected = baseline->argv;
        expected.pop_back();
        expected.insert(expected.end(), forwarded.begin(), forwarded.end());
        expected.push_back("stdio");
        require(process && process->argv == expected && process->environment == baseline->environment,
            "agent option normalization changed values, ordering, approval or environment");
    };
    for (const std::string option : {"--model", "-m", "--reasoning-effort", "--effort"}) {
        const std::string value = option == "--model" || option == "-m" ? "grok-4.6" : "high";
        for (const bool approving : {false, true}) {
            const std::string approval = approving ? " --always-approve" : "";
            std::vector<std::string> forwarded{option, value};
            if (approving) forwarded.push_back("--always-approve");
            check(" " + option + " " + value + approval, forwarded);
            check(" " + option + "=" + value + approval, forwarded);
        }
    }
    check(" --model=grok-4.6 --effort high", {"--model", "grok-4.6", "--effort", "high"});
    check(" --model grok-4.6 --effort=high", {"--model", "grok-4.6", "--effort", "high"});
    check(" --effort=low --effort high", {"--effort", "low", "--effort", "high"});
    check(" --effort high --effort=low", {"--effort", "high", "--effort", "low"});
    check(" --model=custom=model", {"--model", "custom=model"});
    for (const std::string options : {
        " --model", " --model=", " --effort", " --effort=", " --effort ''",
        " --model --always-approve", " --model= --effort high"})
    {
        const auto refused = prepare_with(options);
        require(!refused.succeeded && refused.output == addon::Json_value("invalid_launcher_option"),
            "an absent agent option value must not consume another option");
    }
    for (const std::string options : {
        " --permission-mode=acceptEdits", " --deny=Read", " --unknown=value", " --always-approve=false"})
    {
        const auto refused = prepare_with(options);
        require(!refused.succeeded &&
            refused.output == addon::Json_value("delegated_launcher_options_unavailable"),
            "normalizing equals syntax must not admit unsupported agent options");
    }
}

void delegated_model_selection_contract(const Context& base)
{
    const auto saved = addon::decode_delegated_prepare_request(base.input.payload, nullptr);
    const auto baseline = addon::decode_delegated_prepare_result(base.output, nullptr);
    require(saved && baseline && !baseline->process.argv.empty(), "delegated selection fixture is prepared");
    const std::string options =
        " --model=first-model -m second-model --effort low --reasoning-effort=medium --always-approve";
    for (const bool model : {false, true}) {
        for (const bool effort : {false, true}) {
            auto request = *saved;
            request.context.launcher.script_body += options;
            request.context.launcher_argv.back() = request.context.launcher.script_body;
            addon::Delegated_model_selection selection;
            if (model) {
                selection.model = "provider/next model";
            }
            if (effort) {
                selection.reasoning_effort = "adaptive-next";
            }
            if (model || effort) {
                request.model_selection = selection;
            }
            Context prepared;
            prepared.input = base.input;
            prepared.input.payload = addon::encode_delegated_prepare_request(request);
            const auto input = prepared.input.payload;
            grok::handle_preparation(prepared);
            const auto result = prepared.succeeded
                ? addon::decode_delegated_prepare_result(prepared.output, nullptr) : std::nullopt;
            require(result.has_value(), "delegated model selection reaches native agent preparation");
            auto expected = baseline->process.argv;
            expected.pop_back();
            if (!model) {
                expected.insert(expected.end(), {"--model", "first-model", "-m", "second-model"});
            }
            if (!effort) {
                expected.insert(expected.end(), {"--effort", "low", "--reasoning-effort", "medium"});
            }
            expected.push_back("--always-approve");
            if (model) {
                expected.push_back("--model=" + *selection.model);
            }
            if (effort) {
                expected.push_back("--reasoning-effort=" + *selection.reasoning_effort);
            }
            expected.push_back("stdio");
            require(result->process.argv == expected && result->model_selection == request.model_selection,
                "per-task selection replaces every native alias and acknowledges the exact opaque values");
            require(result->process.environment == baseline->process.environment &&
                result->account_context && baseline->account_context &&
                addon::encode_account_context(*result->account_context) ==
                    addon::encode_account_context(*baseline->account_context) && prepared.input.payload == input,
                "selection preserves the chosen account, environment, approval and saved launcher");
        }
    }
}

void managed_configuration_contract()
{
#ifdef _WIN32
    const auto fixture_name = "grok_legacy_hooks_windows.json";
#else
    const auto fixture_name = "grok_legacy_hooks_posix.json";
#endif
    std::ifstream fixture(addon::path_from_utf8(GROK_TEST_FIXTURE_DIR) / fixture_name, std::ios::binary);
    require(fixture.is_open(), "The historical hook fixture must be available");
    const std::string legacy((std::istreambuf_iterator<char>(fixture)), std::istreambuf_iterator<char>());
    require(!fixture.bad() && !legacy.empty(), "The historical hook fixture must be readable");
    const auto desired = grok::hook_configuration();
    const auto transform = [](bool present, const std::string& content) {
        Context context;
        context.input.capability = {"managed-configuration", 2};
        context.input.operation = "transform";
        context.input.payload = addon::encode_managed_configuration_transform_request(
            {addon::random_nonce_hex(), present, content});
        const auto original = context.input.payload;
        grok::transform_configuration(context);
        require(context.input.payload == original, "A hook transform must leave the supplied snapshot unchanged");
        return context;
    };
    for (const auto& input : std::vector<std::pair<bool, std::string>>{
        {false, {}}, {true, desired}, {true, legacy}})
    {
        const auto context = transform(input.first, input.second);
        const auto request = addon::decode_managed_configuration_transform_request(context.input.payload, nullptr);
        const auto result = addon::decode_managed_configuration_transform_result(context.output, nullptr);
        require(context.succeeded && request && result && result->grant == request->grant && result->content == desired,
            "Absent, current and historical owned hooks must return the current content with the original grant");
    }
    const auto parsed = addon::parse_json(legacy, {}, nullptr);
    require(parsed && parsed->find("hooks"), "The historical fixture must contain its native hooks");
    auto modified = *parsed;
    modified.find("hooks")->find("SessionStart")->as_array().front().find("hooks")->as_array().front().set(
        "command", addon::Json_value("user-owned"));
    auto mixed = *parsed;
    auto user_hook = addon::Json_value::object();
    user_hook.set("type", addon::Json_value("command"));
    user_hook.set("command", addon::Json_value("user-owned"));
    mixed.find("hooks")->find("StopFailure")->as_array().front().find("hooks")->push_back(std::move(user_hook));
    for (const auto& content : {addon::serialize_json(modified) + '\n', addon::serialize_json(mixed) + '\n',
        std::string("{\"hooks\":{\"Stop\":[{\"hooks\":[{\"type\":\"command\",\"command\":\"user-owned\"}]}]}}")})
    {
        const auto context = transform(true, content);
        require(!context.succeeded && context.status == addon::Result_status::UNAVAILABLE &&
            context.output == addon::Json_value("managed_hook_file_conflict"),
            "Modified, mixed and foreign hooks must refuse without returning replacement content");
    }
}
int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "Grok Build --session-id --resume\n"; return 0;
    }
    if (std::getenv("GROK_ARGUMENT_CAPTURE")) {
        auto words = addon::Json_value::array();
        for (int index = 1; index < argc; ++index) {
            words.push_back(addon::Json_value(argv[index]));
        }
        std::cout << addon::serialize_json(words) << '\n';
        return 0;
    }
    try {
        launcher_approval_contract();
        native_launcher_options_contract();
        managed_configuration_contract();
        require(grok::wake_compatible_arguments({"--model", "grok-4.6", "--effort=high"}),
            "native model options rejected by wake launch");
        require(grok::wake_compatible_arguments({"--resume", "8e18215e-f612-4131-95dd-7890139a41ae"}),
            "exact native resume rejected by wake launch");
        for (const auto& arguments : std::vector<std::vector<std::string>>{
            {"--allow", "bash"}, {"--deny", "write"}, {"--no-subagents"}, {"--disable-web-search"},
            {"--storage-mode", "memory"}, {"--no-leader"}, {"--leader-socket", "other"},
            {"--permission-mode", "default"}, {"--unknown-future-option"}, {"--model"}}) {
            require(!grok::wake_compatible_arguments(arguments), "standalone policy override lost through leader launch");
            addon::Launch_patch retained;
            retained.argv = std::vector<std::string>{"grok", "--no-leader"};
            retained.argv->insert(retained.argv->end(), arguments.begin(), arguments.end());
            const auto original = retained.argv;
            grok::prepare_wake(retained, arguments, "--leader-socket");
            require(retained.session_wake && !retained.session_wake->available && retained.argv == original,
                "unavailable wake changed original standalone invocation");
        }
        const auto launcher = grok::recognize_launcher(
            {"GROK_HOME='profiles/../selected' grok --model grok-4.6", addon::k_runner_bash, {}}, "linux-x86_64");
        require(launcher && launcher->environment_names == std::vector<std::string>{"GROK_HOME"}, "authored assignment name lost");
        require(launcher->prefix_arguments == std::vector<std::string>{"--model", "grok-4.6"}, "model arguments lost");
        for (const auto* script : {"grok; echo hidden", "grok $(hidden)", "grok '\""}) {
            require(!grok::recognize_launcher({script, addon::k_runner_bash, {}}, "linux-x86_64"),
                "Unsupported Bash syntax must remain a shell invocation");
        }
        require(!grok::recognize_launcher({"'grok'", addon::k_runner_direct, {}}, "linux-x86_64"),
            "Direct single quotes are program bytes, not shell quoting");
        const auto config = addon::parse_json(grok::hook_configuration(), {}, nullptr);
        require(config && config->find("hooks"), "native hooks object missing");
        const auto* pretool = config->find("hooks")->find("PreToolUse");
        require(pretool && pretool->as_array().size() == 1, "model-context hook missing");
        const auto start = grok::classify_hook(event("SessionStart"));
        require(start && start->classification == addon::Classification::TRANSITION, "session start not admitted");
        auto child_prompt = event("UserPromptSubmit");
        child_prompt.event.body.set("subagentType", addon::Json_value("researcher"));
        require(grok::classify_hook(child_prompt)->observations.empty(), "child prompt altered root state");
        const auto failed = grok::classify_hook(event("StopFailure"));
        require(failed && failed->observations.front().outcome_transition == addon::Outcome_transition::FAILED,
            "native failure lost");
        namespace fs = std::filesystem;
        const auto root = fs::canonical(fs::temp_directory_path()) / ("grok-prepare-" + addon::random_nonce_hex());
        fs::create_directories(root / "profiles" / "native");
        struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{root};
#ifdef _WIN32
        const auto fake = root / "grok.exe";
#else
        const auto fake = root / "grok";
#endif
        fs::copy_file(fs::absolute(argv[0]), fake);
#ifndef _WIN32
        fs::permissions(fake, fs::perms::owner_exec, fs::perm_options::add);
#endif
        addon::Prepare_request preparation;
        preparation.launcher.script_body = "\"" + fake.string() + "\" --model grok-4.6";
        preparation.launcher.runner_key = addon::k_runner_direct;
        preparation.launcher_argv = {fake.string(), "--model", "grok-4.6"};
        preparation.working_directory = root.string();
        preparation.launch_environment.emplace();
#ifdef _WIN32
        preparation.launch_environment->platform_id = "windows-x86_64";
#else
        preparation.launch_environment->platform_id = "linux-x86_64";
#endif
        preparation.launch_environment->effective_environment.set("GROK_HOME", addon::Json_value("profiles/./native"));
        for (const auto* key : {"SystemRoot", "WINDIR", "PATH"}) {
            if (const auto* value = std::getenv(key)) preparation.launch_environment->effective_environment.set(key, addon::Json_value(value));
        }
        const auto locator = (root / "profiles" / "." / "native").string();
        grok::Environment probe_environment;
        for (const auto& member : preparation.launch_environment->effective_environment.as_object()) {
            probe_environment.emplace_back(member.name, member.value.as_string());
        }
        // Exceed the custody table's capacity across failed spawns and reaped
        // children to verify that both paths release their reservations.
        for (int attempt = 0; attempt < 64; ++attempt) {
            Context probe;
            require(!grok::run_probe({(root / "absent-cli").string()}, probe_environment, root.string(), 1000, probe),
                "Missing probe executable unexpectedly spawned");
            const auto result = grok::run_probe({fake.string(), "--help"}, probe_environment, root.string(), 1000, probe);
            require(result && (result->output == "Grok Build --session-id --resume\n" ||
                result->output == "Grok Build --session-id --resume\r\n"),
                "Probe custody admission leaked after failed spawn or child reaping");
        }
        preparation.account_context = addon::Account_context{2, grok::profile_account_key(locator), locator};
        Context context;
        context.input.capability = {"launch-preparation", 3};
        context.input.operation = "prepare";
        context.input.payload = addon::encode_prepare_request(preparation, 3);
        grok::handle_preparation(context);
        require(context.succeeded, "public preparation failed");
        const auto patch = addon::decode_launch_patch(context.output, nullptr, 3);
        require(patch && patch->apply_managed_configuration && patch->argv &&
            std::find(patch->argv->begin(), patch->argv->end(), "grok-4.6") != patch->argv->end(), "prepared native model/options lost");
        require(std::find(patch->set.begin(), patch->set.end(), std::pair<std::string, std::string>{"GROK_HOME", "profiles/./native"}) != patch->set.end(),
            "native profile spelling changed during preparation");
        standalone_launch_contract(preparation);
        launcher_argument_semantics_contract(preparation);
        for (const std::string raw : {"~/selected", "'~/selected'", "\"~/selected\""}) {
            const std::string frozen = raw.front() == '~' ? (root / "expanded" / "selected").string() : "~/selected";
            preparation.launcher.runner_key = addon::k_runner_bash;
            preparation.launcher.script_body = "GROK_HOME=" + raw + " CUSTOM_MODEL_KEY=~/key '" + fake.generic_string() + "'";
            preparation.launcher_argv = {"bash",
                preparation.launch_environment->platform_id.starts_with("linux-") ? "-c" : "-lc",
                preparation.launcher.script_body};
            preparation.launch_environment->effective_environment.set("GROK_HOME", addon::Json_value(frozen));
            preparation.launch_environment->effective_environment.set("CUSTOM_MODEL_KEY", addon::Json_value("/frozen/key"));
            const auto profile = fs::path(frozen).is_absolute() ? fs::path(frozen) : root / frozen;
            preparation.account_context = addon::Account_context{2, grok::profile_account_key(profile.string()), profile.string()};
            Context prepared;
            prepared.input.capability = {"launch-preparation", 3};
            prepared.input.operation = "prepare";
            prepared.input.payload = addon::encode_prepare_request(preparation, 3);
            grok::handle_preparation(prepared);
            require(prepared.succeeded, "assignment preparation failed");
            const auto assigned = addon::decode_launch_patch(prepared.output, nullptr, 3);
            require(assigned && assigned->apply_managed_configuration && assigned->argv &&
                assigned->argv->front() == fake.string(), "shell assignment did not prepare native launch");
            require(std::find(assigned->set.begin(), assigned->set.end(), std::pair<std::string, std::string>{"GROK_HOME", frozen}) != assigned->set.end(),
                "prepared profile overwrote frozen tilde semantics");
            require(std::find(assigned->set.begin(), assigned->set.end(), std::pair<std::string, std::string>{"CUSTOM_MODEL_KEY", "/frozen/key"}) != assigned->set.end(),
                "prepared custom assignment lost host expansion");
            addon::Session_launch_context launch;
            launch.launcher           = preparation.launcher;
            launch.launcher_argv      = preparation.launcher_argv;
            launch.working_directory = preparation.working_directory;
            launch.launch_environment = preparation.launch_environment;
            launch.account_context    = preparation.account_context;
            addon::Resume_prepare_request resume;
            resume.context = launch;
            resume.resume_record.set("provider_session_id", addon::Json_value("8e18215e-f612-4131-95dd-7890139a41ae"));
            Context resumed;
            resumed.input = {"resume", {"interactive-resume", 2}, "prepare_resume", addon::encode_resume_prepare_request(resume, 2)};
            grok::handle_preparation(resumed);
            addon::Contract_failure failure;
            const auto exact = addon::decode_resume_prepare_result(resumed.output, &failure, 2);
            require(resumed.succeeded && exact && addon::validate_resume_process_proposal(exact->process, &failure) &&
                addon::validate_launch_patch(exact->patch, {}, {}, &failure), "resume producer violated terminal policy");
            require(exact->patch.set == assigned->set, "resume lost frozen assignment values or profile spelling");
            for (const bool delegated : {false, true}) {
                addon::Delegated_prepare_request request;
                request.context = launch;
                request.prompt = "Implement the selected change.";
                request.control.task_id = "task-control";
                request.control.private_artifact_root = root.string();
                Context task;
                task.input.request_id = delegated ? "delegated" : "structured";
                task.input.capability = {delegated ? "delegated-session" : "structured-session", 3};
                task.input.operation = "prepare";
                if (delegated) {
                    addon::Delegated_task_server server;
                    server.server_id = "control";
                    server.program = fake.string();
                    server.actions = {"task_status"};
                    server.environment = {
                        {"LOGONOMIC_SESSION_CAPABILITY", std::string(64, 'b')},
                        {"VNM_CONTROL_ENDPOINT", "tcp://127.0.0.1:32970"},
                        {"VNM_CONTROL_TOKEN", std::string(64, 'c')},
                        {"LOGONOMIC_VNMCTL_PATH", fake.string()},
                    };
                    request.control.servers.push_back(server);
                    request.control.connection = addon::Delegated_connection{
                        (root / "host relay").string(), (root / ".logonomic/task-servers.json").string()};
                    task.input.payload = addon::encode_delegated_prepare_request(request);
                }
                else {
                    addon::Structured_prepare_request structured;
                    structured.purpose = addon::Structured_purpose::IMPLEMENTATION;
                    structured.context = launch;
                    structured.prompt = request.prompt;
                    task.input.payload = addon::encode_structured_prepare_request(structured);
                }
                grok::handle_preparation(task);
                require(task.succeeded && addon::validate_session_exchange_payload(task.output, &failure),
                    "session producer duplicated inherited and frozen assignments");
                agent_option_forms_contract(task, launch.launcher.script_body);
                if (delegated) {
                    delegated_model_selection_contract(task);
                    const auto delegated_result = addon::decode_delegated_prepare_result(task.output, &failure);
                    require(delegated_result &&
                        delegated_result->acknowledgment.connection_version == addon::k_delegated_connection_version &&
                        addon::validate_delegated_acknowledgment(request.control, delegated_result->acknowledgment, &failure),
                        "delegation must acknowledge the configured host connection");
                    require(delegated_result->inspection.protocol_state.find(std::string(64, 'c')) == std::string::npos,
                        "host credentials cannot enter delegated continuation");
                    request.control.connection.reset();
                    Context refused;
                    refused.input = task.input;
                    refused.input.payload = addon::encode_delegated_prepare_request(request);
                    grok::handle_preparation(refused);
                    require(!refused.succeeded, "missing connection must refuse delegation");

                    // Oracle: the owner's rule that a delegated run keeps its
                    // launcher's permission posture (2026-09-28), and
                    // `grok agent --help` (1.0.41): agent mode takes
                    // --always-approve but no permission mode or rule option.
                    auto posture = request;
                    posture.control.connection = addon::Delegated_connection{
                        (root / "host relay").string(), (root / ".logonomic/task-servers.json").string()};
                    const auto prepare_with = [&](const std::string& options) {
                        posture.context.launcher.script_body = launch.launcher.script_body + options;
                        posture.context.launcher_argv = launch.launcher_argv;
                        posture.context.launcher_argv.back() = posture.context.launcher.script_body;
                        Context prepared_posture;
                        prepared_posture.input = task.input;
                        prepared_posture.input.payload = addon::encode_delegated_prepare_request(posture);
                        grok::handle_preparation(prepared_posture);
                        return prepared_posture;
                    };
                    const auto approving = prepare_with(" --always-approve");
                    const auto approving_result = addon::decode_delegated_prepare_result(approving.output, &failure);
                    require(approving.succeeded && approving_result, "a launcher's --always-approve is prepared");
                    const auto& agent_argv = approving_result->process.argv;
                    const auto approve = std::find(agent_argv.begin(), agent_argv.end(), "--always-approve");
                    require(approve != agent_argv.end() &&
                        std::find(approve, agent_argv.end(), "stdio") != agent_argv.end(),
                        "the launcher's --always-approve reaches grok agent");
                    const auto plain_argv = addon::decode_delegated_prepare_result(task.output, &failure)->process.argv;
                    require(std::find(plain_argv.begin(), plain_argv.end(), "--always-approve") == plain_argv.end(),
                        "a launcher without it gets no approval the package added");
                    for (const std::string option : {"--permission-mode acceptEdits", "--allow Bash", "--deny Read"}) {
                        const auto refusing = prepare_with(" " + option);
                        require(!refusing.succeeded &&
                            refusing.output == addon::Json_value("delegated_launcher_options_unavailable") &&
                            refusing.detail.find(option.substr(0, option.find(' '))) != std::string::npos,
                            "a permission option grok agent cannot carry is refused, naming it");
                    }
                }
                const auto process = addon::decode_process_proposal(*task.output.find("process"), &failure);
                require(process && addon::validate_structured_process_proposal(*process, &failure) &&
                    process->environment == assigned->set, "session lost explicit frozen environment");
            }
        }
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
