#include "grok_accounts.h"
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
namespace {
void check(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: grok [OPTIONS] [PROMPT] [COMMAND]\nGrok --session-id --resume --leader-socket\n";
        return 0;
    }
    if (argc != 2) return 2;
    try {
        const auto directory = fs::canonical(fs::temp_directory_path()) / ("grok-wake-package-" + addon::random_nonce_hex());
        fs::create_directories(directory / "native");
        struct Cleanup {
            fs::path path;
            ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
        } cleanup{directory};
#ifdef _WIN32
        const auto native = directory / "grok.exe";
#else
        const auto native = directory / "grok";
#endif
        fs::copy_file(fs::absolute(argv[0]), native);
        addon::Simulator_options options;
        options.package_directory = addon::path_from_utf8(argv[1]);
        options.validation.platform_id = addon::host_platform_id();
        addon::Host_simulator host(options, nullptr);
        addon::Contract_failure failure;
        check(host.start(&failure), "package start: " + failure.reason_code + " " + failure.detail);
        const auto invoke = [&](const addon::Capability_pair& capability, std::string operation, addon::Json_value payload) {
            const auto label = capability.capability_id + "/" + operation;
            const auto id = host.invoke(capability, std::move(operation), std::move(payload), 5000);
            const auto result = host.wait_outcome(id, 6000);
            check(result && result->status == addon::Host_status::SUCCESS,
                "packaged operation " + label + " failed: " + (result ? result->reason_code : "no outcome"));
            return result->payload;
        };
        addon::Prepare_request prepare;
        prepare.launcher = {"'" + addon::path_to_utf8(native) + "' --model grok-4.6", addon::k_runner_direct, {}};
        prepare.launcher_argv = {addon::path_to_utf8(native), "--model", "grok-4.6"};
        prepare.working_directory = addon::path_to_utf8(directory);
        prepare.launch_environment = addon::Launch_environment{addon::host_platform_id(), addon::Json_value::object(), std::nullopt};
        for (const auto& [name, value] : addon::current_process_environment()) {
            if (name == "SystemRoot" || name == "WINDIR" || name == "PATH" || name == "HOME" || name == "USERPROFILE")
                prepare.launch_environment->effective_environment.set(name, addon::Json_value(value));
        }
        const auto profile = addon::path_to_utf8(directory / "native");
        prepare.launch_environment->effective_environment.set("GROK_HOME", addon::Json_value(profile));
        prepare.account_context = addon::Account_context{2, grok::profile_account_key(profile), profile};
        prepare.session_wake.emplace();
        const auto patch = addon::decode_launch_patch(invoke({"launch-preparation", 3}, "prepare",
            addon::encode_prepare_request(prepare, 3)), &failure, 3);
        check(patch && patch->session_wake && patch->session_wake->available && patch->argv && patch->argv->size() == 6,
            "packaged wake preparation did not bind its indexed helper");
        check(fs::equivalent(addon::path_from_utf8(patch->argv->front()),
            options.package_directory / "helpers" / fs::path(patch->argv->front()).filename()), "helper escaped package");
        check((*patch->argv)[1] == "--serve" && (*patch->argv)[3] == addon::path_to_utf8(native) &&
            (*patch->argv)[4] == "--model" && (*patch->argv)[5] == "grok-4.6", "native argument/profile preparation changed");
        check(patch->account_context && addon::encode_account_context(*patch->account_context) ==
            addon::encode_account_context(*prepare.account_context), "selected account echo changed");

        auto continuation = prepare;
        continuation.initial_prompt = addon::Initial_prompt_request{
            "Logonomic continuation: read the earlier session's brief before proceeding.",
            addon::path_to_utf8(directory / "brief.md")};
        const auto continued = addon::decode_launch_patch(invoke({"launch-preparation", 3}, "prepare",
            addon::encode_prepare_request(continuation, 3)), &failure, 3);
        check(continued && continued->session_wake && continued->session_wake->available && continued->argv &&
            continued->argv->size() == patch->argv->size() + 2 &&
            addon::validate_initial_prompt_acknowledgment(continuation.initial_prompt, *continued, &failure),
            "packaged initial-prompt preparation lost the wake bridge or its acknowledgment");
        check((*continued->argv)[continued->argv->size() - 2] == "--" &&
            continued->argv->back() == continuation.initial_prompt->text &&
            std::count(continued->argv->begin(), continued->argv->end(), continuation.initial_prompt->text) == 1,
            "packaged wake bridge did not retain the exact single native first message");

        addon::Session_wake_target target;
        target.provider_session_id = "8e18215e-f612-4131-95dd-7890139a41ae";
        target.working_directory = prepare.working_directory;
        target.launcher_argv = *patch->argv;
        target.launch_environment = *prepare.launch_environment;
        target.correlation = {addon::random_nonce_hex()};
        target.binding = patch->session_wake->binding;
        const auto readiness = addon::decode_session_wake_readiness(invoke({"session-wake", 1}, "inspect",
            addon::encode_session_wake_inspect_request({target})), &failure);
        check(readiness && !readiness->available, "preparation alone claimed a live native session");
        const auto notification = addon::decode_session_wake_notify_result(invoke({"session-wake", 1}, "notify",
            addon::encode_session_wake_notify_request({target, "test-notification", "Read the mailbox."})), &failure);
        check(notification && notification->disposition == addon::Session_wake_disposition::UNAVAILABLE,
            "absent private runtime claimed native acceptance or ambiguous submission");

        addon::Resume_prepare_request resume;
        resume.context.launcher = prepare.launcher;
        resume.context.launcher_argv = prepare.launcher_argv;
        resume.context.working_directory = prepare.working_directory;
        resume.context.account_context = prepare.account_context;
        resume.context.launch_environment = prepare.launch_environment;
        resume.context.session_wake.emplace();
        resume.resume_record.set("provider_session_id", addon::Json_value(target.provider_session_id));
        const auto resumed = addon::decode_resume_prepare_result(invoke({"interactive-resume", 2}, addon::k_prepare_resume_operation,
            addon::encode_resume_prepare_request(resume, 2)), &failure, 2);
        check(resumed && resumed->patch.session_wake && resumed->patch.session_wake->available &&
            resumed->process.argv.size() == 7 && resumed->process.argv[5] == "--resume" &&
            resumed->process.argv[6] == target.provider_session_id, "exact resume lost prepared wake or native identity");
        check(resumed->patch.session_wake->binding != patch->session_wake->binding,
            "separate native launches reused a private endpoint");
        check(std::find(resumed->patch.set.begin(), resumed->patch.set.end(),
            std::pair<std::string, std::string>{"GROK_LOGONOMIC_WAKE_TOKEN", resumed->process.argv[1]}) != resumed->patch.set.end(),
            "resumed helper did not inherit its exact fresh private endpoint");

        const auto resumed_helper = options.package_directory / resumed->process.executable.path;
        const auto helper_bytes   = resumed_helper.generic_u8string();
        target.launcher_argv = {std::string(helper_bytes.begin(), helper_bytes.end())};
        target.launcher_argv.insert(target.launcher_argv.end(), resumed->process.argv.begin(), resumed->process.argv.end());
        target.binding = resumed->patch.session_wake->binding;
        const auto resumed_readiness = addon::decode_session_wake_readiness(invoke({"session-wake", 1}, "inspect",
            addon::encode_session_wake_inspect_request({target})), &failure);
        check(resumed_readiness && resumed_readiness->reason == "native_control_unavailable",
            "host-resolved resumed helper spelling failed the launch binding");
        const auto resumed_notification = addon::decode_session_wake_notify_result(invoke({"session-wake", 1}, "notify",
            addon::encode_session_wake_notify_request({target, "resumed-notification", "Read the mailbox."})), &failure);
        check(resumed_notification && resumed_notification->reason == "native_control_unavailable",
            "resumed notification failed the launch binding");

        target.launcher_argv.front() = addon::path_to_utf8(native);
        const auto wrong_helper = addon::decode_session_wake_readiness(invoke({"session-wake", 1}, "inspect",
            addon::encode_session_wake_inspect_request({target})), &failure);
        check(wrong_helper && wrong_helper->reason == "native_launch_binding_unavailable",
            "a different executable passed the launch binding");

        prepare.launcher.script_body += " --deny write";
        const auto restricted = addon::decode_launch_patch(invoke({"launch-preparation", 3}, "prepare",
            addon::encode_prepare_request(prepare, 3)), &failure, 3);
        check(restricted && restricted->session_wake && !restricted->session_wake->available && restricted->argv &&
            restricted->argv->front() == addon::path_to_utf8(native) &&
            std::find(restricted->argv->begin(), restricted->argv->end(), "--no-leader") != restricted->argv->end() &&
            restricted->argv->back() == "write", "native policy restriction changed to enable wake");
        int exit_code = -1;
        check(host.shutdown(5000, &exit_code) && exit_code == 0, "package did not shut down cleanly");
        std::cout << "packaged wake preparation and unavailable runtime passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
