#include "grok_accounts.h"
#include "grok_launcher.h"
#include "grok_preparation.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class Context final : public addon::Invocation_context
{
public:
    addon::Invoke_request input;
    addon::Json_value     output;
    bool                  succeeded = false;
    bool                  done = false;
    std::string           reason;
    addon::Result_status  status = addon::Result_status::FAILED;

    const addon::Invoke_request& request() const override { return input; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    bool finished() const override { return done; }

    void succeed(addon::Json_value value) override
    {
        succeeded = true;
        done      = true;
        output    = std::move(value);
    }

    void fail(addon::Result_status result, std::string code, std::string) override
    {
        done   = true;
        status = result;
        reason = std::move(code);
    }
};

Context prepare(const addon::Prepare_request& request)
{
    Context context;
    context.input.capability = {"launch-preparation", 3};
    context.input.operation  = "prepare";
    context.input.payload    = addon::encode_prepare_request(request, 3);
    grok::handle_preparation(context);
    return context;
}

addon::Prepare_request with_arguments(addon::Prepare_request request, const std::vector<std::string>& arguments)
{
    for (const auto& argument : arguments) {
        request.launcher.script_body += " \"";
        for (const char character : argument) {
            request.launcher.script_body += character == '"' ? "\"\"\"" : std::string(1, character);
        }
        request.launcher.script_body += '"';
        request.launcher_argv.push_back(argument);
    }
    return request;
}

void require_refusal(const addon::Prepare_request& request)
{
    const auto context = prepare(request);
    if (context.succeeded || context.status != addon::Result_status::UNAVAILABLE ||
        context.reason != addon::reason::k_initial_prompt_unavailable)
    {
        throw std::runtime_error(
            "An undeliverable first message must fail its capability: reason=" + context.reason +
            ", prompt=" + request.initial_prompt->text.substr(0, 80) +
            ", last_argument=" + request.launcher_argv.back().substr(0, 80));
    }
}

void exercise(const std::string& executable)
{
    const auto root = fs::canonical(fs::temp_directory_path()) / ("grok-prompt-" + addon::random_nonce_hex());
    fs::create_directories(root / "project");
    fs::create_directories(root / "native");
    struct Cleanup
    {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{root};
#ifdef _WIN32
    const auto native = root / "grok.exe";
#else
    const auto native = root / "grok";
#endif
    fs::copy_file(addon::path_from_utf8(executable), native);
#ifndef _WIN32
    fs::permissions(native, fs::perms::owner_exec, fs::perm_options::add);
#endif
    const auto brief_directory = root / u8"continuation briefs Zoë 概要";
    fs::create_directory(brief_directory);
    const auto brief = addon::path_to_utf8(brief_directory / "brief.md");
    std::ofstream(addon::path_from_utf8(brief)) << "The earlier session's working summary.\n";

    addon::Prepare_request direct;
    const auto program = addon::path_to_utf8(native);
    const auto profile = addon::path_to_utf8(root / "native");
    direct.launcher = {"\"" + program + "\"", addon::k_runner_direct, {}};
    direct.launcher_argv     = {program};
    direct.working_directory = addon::path_to_utf8(root / "project");
    direct.launch_environment = addon::Launch_environment{addon::host_platform_id(), addon::Json_value::object(), {}};
    for (const auto& [name, value] : addon::current_process_environment()) {
        if (name == "SystemRoot" || name == "WINDIR" || name == "PATH" || name == "HOME" || name == "USERPROFILE") {
            direct.launch_environment->effective_environment.set(name, addon::Json_value(value));
        }
    }
    direct.launch_environment->effective_environment.set("GROK_HOME", addon::Json_value(profile));
    direct.account_context = addon::Account_context{2, grok::profile_account_key(profile), profile};
    direct.profile_storage = addon::Profile_storage_context{1, profile};
    const auto configured = with_arguments(direct, {"--model", "grok-4.6", "--effort", "high"});
    const auto ordinary = prepare(configured);
    require(ordinary.succeeded && !ordinary.output.find(addon::k_initial_prompt_member),
        "An ordinary launch must not acknowledge a first message it was not given");

    auto shell = configured;
    shell.launcher.runner_key = addon::k_runner_powershell;
    shell.launcher.script_body = "& " + shell.launcher.script_body;
    shell.launch_environment->platform_id = "windows-x86_64";
    shell.launcher_argv = {"powershell.exe", "-NoLogo", "-Command", shell.launcher.script_body};
    const auto policy = with_arguments(direct,
        {"--permission-mode", "acceptEdits", "--allow", "Read", "--deny", "Bash(rm *)", "--no-subagents"});
    const auto standalone = with_arguments(direct, {"--no-leader"});
    for (const auto* launcher : std::vector<const addon::Prepare_request*>{
        &direct, &configured, &shell, &policy, &standalone})
    {
        for (const std::string& text : {
            "Logonomic continuation: read the brief at " + brief + " before anything else and follow it.",
            std::string("-- \"quoted \\\"inner\\\"\" 'single' $HOME %PATH% ^& | < > ` trailing \\\\"),
            std::string("Read /home/zoe/goal/brief.md and C:\\work\\notes\\brief.md"),
            std::string("Explainx/goal and explain\xC2\xA0/goal as ordinary text.")})
        {
            auto request = *launcher;
            request.initial_prompt = addon::Initial_prompt_request{text, brief};
            const auto prepared = prepare(request);
            addon::Contract_failure failure;
            const auto patch = addon::decode_launch_patch(prepared.output, &failure, 3);
            require(prepared.succeeded && patch && patch->argv &&
                addon::validate_launch_patch(*patch, {}, {}, &failure) &&
                addon::validate_initial_prompt_acknowledgment(request.initial_prompt, *patch, &failure),
                "The host must accept the prepared first message and its exact digest");
            require(patch->argv->at(patch->argv->size() - 2) == "--" && patch->argv->back() == text &&
                std::count(patch->argv->begin(), patch->argv->end(), text) == 1,
                "The message must be the one positional argument after the end-of-options marker");
            require(std::count(patch->argv->begin(), patch->argv->end(), "--no-leader") == 1,
                "An authored standalone selector must not become a duplicate flag");
            require(patch->account_context && addon::encode_account_context(*patch->account_context) ==
                addon::encode_account_context(*direct.account_context) && patch->profile_storage &&
                patch->profile_storage->root == profile,
                "Prompt preparation must retain the selected account and package storage");

            auto environment = addon::environment_pairs(request.launch_environment->effective_environment);
            for (const auto& [name, value] : patch->set) {
                addon::set_environment(environment, name, value);
            }
            addon::set_environment(environment, "GROK_INITIAL_PROMPT_CAPTURE", "1");
            Context capture;
            const auto delivered = grok::run_probe(*patch->argv, environment, request.working_directory, 5000, capture);
            const auto arguments = delivered ? addon::parse_json(delivered->output, {}, nullptr) : std::nullopt;
            require(arguments && arguments->is_array() && arguments->as_array().size() == patch->argv->size(),
                "The prepared native invocation must reach the recording CLI");
            for (std::size_t index = 1; index < patch->argv->size(); ++index) {
                require(arguments->as_array()[index].as_string() == patch->argv->at(index),
                    "Every native argument and the prompt must survive process quoting byte for byte");
            }
        }
    }

    auto requested = configured;
    requested.initial_prompt = addon::Initial_prompt_request{"Read the handoff brief.", brief};
    // Native argv settings select the new session's environment, persona and
    // workspace; none of these options supplies an earlier user message.
    for (const auto& arguments : std::vector<std::vector<std::string>>{
        {"--cwd", direct.working_directory}, {"--agent", "reviewer"}, {"--agents", "{}"},
        {"--worktree"}, {"-w", "handoff"}, {"--worktree=handoff", "--ref", "main"},
        {"--session-id", "9a3b3f45-a8ef-4a33-a294-dc745d698926"},
        {"--sandbox", "workspace"}, {"--sandbox", "read-only"}, {"--sandbox", "off"}})
    {
        auto request = with_arguments(direct, arguments);
        request.initial_prompt = requested.initial_prompt;
        const auto prepared = prepare(request);
        addon::Contract_failure failure;
        const auto patch = addon::decode_launch_patch(prepared.output, &failure, 3);
        require(prepared.succeeded && patch && patch->argv &&
            addon::validate_initial_prompt_acknowledgment(request.initial_prompt, *patch, &failure),
            "Native fresh-session settings must remain available during handoff");
        std::vector<std::string> expected{program, "--no-leader"};
        expected.insert(expected.end(), arguments.begin(), arguments.end());
        expected.insert(expected.end(), {"--", request.initial_prompt->text});
        require(*patch->argv == expected, "The launcher's native settings must reach the new session unchanged");
    }
    const auto check_access = [](const addon::Prepare_request& request, bool blocked, const char* name) {
        const auto prepared = prepare(request);
        if (blocked) {
            if (prepared.succeeded || prepared.status != addon::Result_status::UNAVAILABLE ||
                prepared.reason != addon::reason::k_initial_prompt_unavailable)
            {
                throw std::runtime_error(std::string(name) + ": expected a capability refusal; " + prepared.reason);
            }
        }
        else {
            addon::Contract_failure failure;
            const auto patch = addon::decode_launch_patch(prepared.output, &failure, 3);
            if (!prepared.succeeded || !patch ||
                !addon::validate_initial_prompt_acknowledgment(request.initial_prompt, *patch, &failure))
            {
                throw std::runtime_error(std::string(name) + ": expected preserved preparation; " + prepared.reason);
            }
        }
    };
    auto strict_temp = with_arguments(direct, {"--sandbox", "strict"});
    strict_temp.initial_prompt = requested.initial_prompt;
    check_access(strict_temp, false, "stock strict admits the host's normal temporary brief");
    // These separate-value options were not managed before the handoff
    // capability. Without a first message, preserve their entire old patch.
    for (const auto& arguments : std::vector<std::vector<std::string>>{
        {"--sandbox", "strict"}, {"--system-prompt-override", "instructions"},
        {"--worktree", "handoff"}, {"-w", "handoff"}, {"--worktree-ref", "main"},
        {"--ref", "main"}, {"--add-dir", direct.working_directory}})
    {
        const auto ordinary_request = with_arguments(direct, arguments);
        const auto ordinary_result = prepare(ordinary_request);
        addon::Launch_patch unchanged;
        unchanged.account_context = ordinary_request.account_context;
        unchanged.profile_storage = ordinary_request.profile_storage;
        unchanged.disposition = addon::Launch_disposition{false, false, false};
        require(ordinary_result.succeeded && addon::serialize_json(ordinary_result.output) ==
            addon::serialize_json(addon::encode_launch_patch(unchanged, 3)),
            "An ordinary launch must preserve its entire unmanaged patch");
    }
#ifndef _WIN32
    // Place the denied case outside /tmp and the project; changing the
    // request's HOME also gives the Library case an isolated native home.
    const auto user_home = addon::environment_value(
        addon::environment_pairs(direct.launch_environment->effective_environment), "HOME");
    require(!user_home.empty(), "The POSIX stock-root fixture requires a native HOME");
    const auto outside = fs::canonical(addon::path_from_utf8(user_home)) /
        ("grok-strict-" + addon::random_nonce_hex());
    fs::create_directories(outside / "home");
    fs::create_directories(outside / "workspace");
    fs::create_directories(outside / "temporary");
    fs::create_directories(outside / "profile");
    Cleanup outside_cleanup{outside};
    const auto make_brief = [](const fs::path& directory) {
        fs::create_directories(directory);
        const auto path = directory / "brief.md";
        std::ofstream(path) << "The retained working summary.\n";
        return addon::path_to_utf8(path);
    };
    auto strict = strict_temp;
    strict.initial_prompt->referenced_file = make_brief(outside / "ungranted");
    strict.launch_environment->effective_environment.set("HOME", addon::Json_value(addon::path_to_utf8(outside / "home")));
    strict.launch_environment->effective_environment.erase("TMPDIR");
    check_access(strict, true, "stock strict refuses an outside-root brief");
    auto windows_strict = strict;
    windows_strict.launch_environment->platform_id = "windows-x86_64";
    check_access(windows_strict, false, "Windows keeps its native strict-sandbox stub behavior");
    auto custom = strict;
    custom.launcher.script_body = direct.launcher.script_body;
    custom.launcher_argv = direct.launcher_argv;
    custom = with_arguments(std::move(custom), {"--sandbox", "strict-inherited"});
    check_access(custom, false, "custom sandbox configuration remains native-owned");
    auto authored = strict;
    authored.launcher.script_body = direct.launcher.script_body;
    authored.launcher_argv = direct.launcher_argv;
    authored.launcher.environment_text = "GROK_SANDBOX=strict";
    authored.launch_environment->effective_environment.set("GROK_SANDBOX", addon::Json_value("strict"));
    check_access(authored, true, "authored strict environment selection has the same native boundary");
    const auto overridden = with_arguments(authored, {"--sandbox=off"});
    check_access(overridden, false, "explicit CLI profile wins over an authored strict environment setting");
    auto cli_strict = strict;
    cli_strict.launcher.environment_text = "GROK_SANDBOX=off";
    cli_strict.launch_environment->effective_environment.set("GROK_SANDBOX", addon::Json_value("off"));
    check_access(cli_strict, true, "explicit CLI strict wins over an authored off environment setting");
    auto ambient = authored;
    ambient.launcher.environment_text.clear();
    check_access(ambient, false, "ambient sandbox defaults remain outside the invocation attestation");
    auto script_strict = ambient;
    script_strict.launcher.runner_key = addon::k_runner_bash;
    script_strict.launcher.script_body = "GROK_SANDBOX=strict " + direct.launcher.script_body;
    script_strict.launcher_argv = {"bash", "-c", script_strict.launcher.script_body};
    check_access(script_strict, true, "authored shell strict selection uses its frozen environment value");

    auto workspace_strict = strict;
    workspace_strict.initial_prompt->referenced_file = make_brief(outside / "workspace" / "continuation");
    workspace_strict = with_arguments(std::move(workspace_strict),
        {"--cwd", addon::path_to_utf8(fs::relative(outside / "workspace", addon::path_from_utf8(direct.working_directory)))});
    check_access(workspace_strict, false, "relative --cwd is resolved against the actual launch cwd");
    auto sibling = workspace_strict;
    sibling.initial_prompt->referenced_file = make_brief(outside / "workspace-sibling");
    check_access(sibling, true, "a sibling path prefix is not a stock readable root");
    auto library = strict;
    library.launch_environment->platform_id = "macos-arm64";
    library.initial_prompt->referenced_file = make_brief(outside / "home" / "Library" / "continuation");
    check_access(library, false, "stock strict admits the effective home Library root");
    auto temporary = strict;
    temporary.initial_prompt->referenced_file = make_brief(outside / "temporary");
    temporary.launch_environment->effective_environment.set("TMPDIR",
        addon::Json_value(addon::path_to_utf8(
            fs::relative(outside / "temporary", addon::path_from_utf8(direct.working_directory)))));
    temporary = with_arguments(std::move(temporary), {"--cwd", addon::path_to_utf8(outside / "workspace")});
    check_access(temporary, false, "relative TMPDIR uses the launch cwd even when --cwd selects another workspace");
    auto profile_strict = strict;
    const auto outside_profile = addon::path_to_utf8(outside / "profile");
    profile_strict.initial_prompt->referenced_file = make_brief(outside / "profile" / "continuation");
    profile_strict.launch_environment->effective_environment.set("GROK_HOME", addon::Json_value(outside_profile));
    profile_strict.account_context = addon::Account_context{2, grok::profile_account_key(outside_profile), outside_profile};
    check_access(profile_strict, false, "stock strict admits the selected native GROK_HOME");

    auto escaping_sessions = profile_strict;
    escaping_sessions.initial_prompt->referenced_file = make_brief(outside / "session-target");
    fs::create_directory_symlink(outside / "session-target", outside / "profile" / "sessions");
    check_access(escaping_sessions, true, "stock strict skips a GROK_HOME sessions link to an unrelated directory");
    fs::remove(outside / "profile" / "sessions");
    escaping_sessions.initial_prompt->referenced_file = make_brief(outside / "home" / ".grok" / "sessions");
    fs::create_directory_symlink(outside / "home" / ".grok" / "sessions", outside / "profile" / "sessions");
    check_access(escaping_sessions, false, "stock strict admits its supported default-sessions link");
#endif
    for (const auto& arguments : std::vector<std::vector<std::string>>{
        {"--resume", "9a3b3f45-a8ef-4a33-a294-dc745d698926"}, {"--continue"}, {"--fork-session"},
        {"-r"}, {"--load", "9a3b3f45-a8ef-4a33-a294-dc745d698926"}, {"-c"}, {"--leader"},
        {"--single", "another prompt"}, {"--prompt-file", "prompt.txt"}, {"login"}, {"earlier prompt"},
        {"--model"}})
    {
        require_refusal(with_arguments(requested, arguments));
    }
    for (const std::string text : {" leading space", "trailing space ", "\xC2\xA0leading", "trailing\xE3\x80\x80",
            "/help", "exit", "QUIT", ":wq!", "explain /btw this", "explain /goal", "explain /goal\xC2\xA0this"})
    {
        auto request = requested;
        request.initial_prompt->text = text;
        require_refusal(request);
    }
    auto no_prompt = requested;
    no_prompt.launch_environment->effective_environment.set("GROK_INITIAL_PROMPT_NO_POSITIONAL", addon::Json_value("1"));
    require_refusal(no_prompt);
    auto unrecognized = requested;
    unrecognized.launcher.script_body += "; echo another command";
    require_refusal(unrecognized);
    auto contradictory = requested;
    contradictory.launcher_argv.back() = "different";
    require_refusal(contradictory);

    // Short repeatable options fit the launcher's script bound while filling
    // the argv entry budget. Only adding the host's first message exceeds it.
    std::vector<std::string> options;
    while (direct.launcher_argv.size() + options.size() + 2 < addon::limits::k_max_argv_entries) {
        options.insert(options.end(), {"--allow", "Read"});
    }
    auto crowded = with_arguments(direct, options);
    const auto fitting = prepare(crowded);
    addon::Contract_failure failure;
    const auto fitting_patch = addon::decode_launch_patch(fitting.output, &failure, 3);
    require(fitting.succeeded && fitting_patch && addon::validate_launch_patch(*fitting_patch, {}, {}, &failure),
        "The crowded launcher must be valid and fit without the added first message");
    crowded.initial_prompt = requested.initial_prompt;
    require_refusal(crowded);
}

int run(const std::vector<std::string>& arguments)
{
    if (arguments.size() == 2 && arguments[1] == "--help") {
        if (!std::getenv("GROK_INITIAL_PROMPT_NO_POSITIONAL")) {
            std::cout << "Usage: grok [OPTIONS] [PROMPT] [COMMAND]\n";
        }
        std::cout << "Grok Build --session-id --resume --leader-socket\n";
        return 0;
    }
    if (std::getenv("GROK_INITIAL_PROMPT_CAPTURE")) {
        auto recorded = addon::Json_value::array();
        for (const auto& argument : arguments) {
            recorded.push_back(addon::Json_value(argument));
        }
        std::cout << addon::serialize_json(recorded) << '\n';
        return 0;
    }
    try {
        require(arguments.size() == 1, "Unexpected fixture arguments");
        exercise(arguments.front());
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> arguments;
    for (int index = 0; index < argc; ++index) {
        arguments.push_back(addon::path_to_utf8(fs::path(argv[index])));
    }
    return run(arguments);
}
#else
int main(int argc, char** argv)
{
    return run(std::vector<std::string>(argv, argv + argc));
}
#endif
