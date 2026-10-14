#include "grok_configuration.h"
#include "grok_accounts.h"
#include "grok_preparation.h"
#include "grok_launcher.h"
#include "grok_lifecycle.h"
#include "grok_wake.h"
#include <logonomic_cli_addon/json.h>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/managed_configuration.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <stdexcept>
namespace addon = logonomic::cli_addon;
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
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
    std::string detail;
    const addon::Invoke_request& request() const override { return input; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value value) override { succeeded = true; done = true; output = std::move(value); }
    void fail(addon::Result_status, std::string reason, std::string text) override
    {
        done = true;
        output = addon::Json_value(std::move(reason));
        detail = std::move(text);
    }
    bool finished() const override { return done; }
};
int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "Grok Build --session-id --resume\n"; return 0;
    }
    try {
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
        const auto launcher = grok::recognize_launcher("GROK_HOME='profiles/../selected' grok --model grok-4.6");
        require(launcher && launcher->environment_names == std::vector<std::string>{"GROK_HOME"}, "authored assignment name lost");
        require(launcher->prefix_arguments == std::vector<std::string>{"--model", "grok-4.6"}, "model arguments lost");
        require(!grok::recognize_launcher("grok; echo hidden"), "shell chain admitted");
        require(!grok::recognize_launcher("grok $(hidden)"), "shell substitution admitted");
        require(!grok::recognize_launcher("grok '\""), "unclosed quote admitted");
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
        Context config_context;
        config_context.input.operation = "transform";
        config_context.input.payload = addon::encode_managed_configuration_transform_request(
            {addon::random_nonce_hex(), true, "{\"hooks\":{\"Stop\":[{\"hooks\":[{\"type\":\"command\",\"command\":\"user-owned\"}]}]}}"});
        grok::transform_configuration(config_context);
        require(!config_context.succeeded && config_context.output == addon::Json_value("managed_hook_file_conflict"),
            "managed transform overwrote an existing foreign hook file");
        namespace fs = std::filesystem;
        const auto root = fs::temp_directory_path() / ("grok-prepare-" + addon::random_nonce_hex());
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
        preparation.launcher.script_body = "'" + fake.generic_string() + "' --model grok-4.6";
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
        for (const std::string raw : {"~/selected", "'~/selected'", "\"~/selected\""}) {
            const std::string frozen = raw.front() == '~' ? (root / "expanded" / "selected").string() : "~/selected";
            preparation.launcher.runner_key = addon::k_runner_bash;
            preparation.launcher.script_body = "GROK_HOME=" + raw + " CUSTOM_MODEL_KEY=~/key '" + fake.generic_string() + "'";
            preparation.launcher_argv = {"bash", "-c", preparation.launcher.script_body};
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
                task.input.capability = {delegated ? "delegated-session" : "structured-session", 2};
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
                    task.input.payload = addon::encode_delegated_prepare_request(request, 2);
                }
                else {
                    addon::Structured_prepare_request structured;
                    structured.purpose = addon::Structured_purpose::IMPLEMENTATION;
                    structured.context = launch;
                    structured.prompt = request.prompt;
                    task.input.payload = addon::encode_structured_prepare_request(structured, 2);
                }
                grok::handle_preparation(task);
                require(task.succeeded && addon::validate_session_exchange_payload(task.output, &failure),
                    "session producer duplicated inherited and frozen assignments");
                if (delegated) {
                    const auto delegated_result = addon::decode_delegated_prepare_result(task.output, &failure, 2);
                    require(delegated_result &&
                        delegated_result->acknowledgment.connection_version == addon::k_delegated_connection_version &&
                        addon::validate_delegated_acknowledgment(request.control, delegated_result->acknowledgment, &failure),
                        "delegation must acknowledge the configured host connection");
                    require(delegated_result->inspection.protocol_state.find(std::string(64, 'c')) == std::string::npos,
                        "host credentials cannot enter delegated continuation");
                    request.control.connection.reset();
                    Context refused;
                    refused.input = task.input;
                    refused.input.payload = addon::encode_delegated_prepare_request(request, 2);
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
                        prepared_posture.input.payload = addon::encode_delegated_prepare_request(posture, 2);
                        grok::handle_preparation(prepared_posture);
                        return prepared_posture;
                    };
                    const auto approving = prepare_with(" --always-approve");
                    const auto approving_result = addon::decode_delegated_prepare_result(approving.output, &failure, 2);
                    require(approving.succeeded && approving_result, "a launcher's --always-approve is prepared");
                    const auto& agent_argv = approving_result->process.argv;
                    const auto approve = std::find(agent_argv.begin(), agent_argv.end(), "--always-approve");
                    require(approve != agent_argv.end() &&
                        std::find(approve, agent_argv.end(), "stdio") != agent_argv.end(),
                        "the launcher's --always-approve reaches grok agent");
                    const auto plain_argv = addon::decode_delegated_prepare_result(task.output, &failure, 2)->process.argv;
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
