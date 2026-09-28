#include "grok_accounts.h"
#include "grok_probes.h"
#include "grok_launcher.h"

#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/probe_contracts.h>
#include <logonomic_cli_addon/quota.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <iostream>
#include <optional>
#include <filesystem>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace addon = logonomic::cli_addon;
void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void exercise()
{
    addon::Launch_environment environment;
#if defined(_WIN32)
    environment.platform_id = "windows-x86_64";
#else
    environment.platform_id = "linux-x86_64";
#endif
    environment.effective_environment = addon::Json_value::object();
    environment.effective_environment.set("GROK_HOME", addon::Json_value("profiles/grok"));
    addon::Launcher_request launcher;
    launcher.script_body = "grok";
    const auto base = std::filesystem::temp_directory_path().generic_string();
    std::string profile_error;
    const auto selected = grok::profile_selection(environment, launcher, base, base, &profile_error);
    check(selected && addon::path_from_utf8(selected->profile_path) ==
        std::filesystem::path(base) / "profiles" / "grok",
        "relative GROK_HOME uses frozen host normalization directory");
    launcher.runner_key = addon::k_runner_bash;
    for (const std::string prefix : {"~/profiles/grok", "'~/profiles/grok'", "\"~/profiles/grok\""}) {
        launcher.script_body = "GROK_HOME=" + prefix + " grok";
        const auto native_value = prefix.front() == '~' ? base + "/expanded/grok" : "~/profiles/grok";
        environment.effective_environment.set("GROK_HOME", addon::Json_value(native_value));
        const auto projected = grok::profile_selection(environment, launcher, base, base, &profile_error);
        const auto expected = std::filesystem::path(native_value).is_absolute()
            ? std::filesystem::path(native_value) : std::filesystem::path(base) / native_value;
        check(projected && projected->adapter_account_key == grok::profile_account_key(expected.string()),
            "account must use frozen shell expansion, preserving quoted literal tilde");
        const auto command = grok::recognize_launcher(launcher.script_body);
        const auto assignments = grok::launcher_environment(*command, environment.effective_environment);
        check(assignments && *assignments == grok::Environment{{"GROK_HOME", native_value}},
            "process assignment must use frozen value rather than reparsed text");
    }
    const auto custom = grok::recognize_launcher("CUSTOM_MODEL_KEY=~/key grok");
    check(!grok::launcher_environment(*custom, environment.effective_environment),
        "missing authored assignment evidence must not fall back to raw text");
    environment.effective_environment.set("CUSTOM_MODEL_KEY", addon::Json_value("/frozen/model-key"));
    check(grok::launcher_environment(*custom, environment.effective_environment)->front().second == "/frozen/model-key",
        "explicit custom model assignment retains host-expanded value");
    launcher.script_body = "grok";
    environment.effective_environment.set("GROK_HOME", addon::Json_value(""));
    const auto fallback = grok::profile_selection(environment, launcher, base, base, &profile_error);
    check(fallback && std::filesystem::path(fallback->profile_path).filename() == ".grok",
        "empty GROK_HOME uses native default profile behavior");
    environment.effective_environment.set("GROK_HOME", addon::Json_value("profiles/link/../grok"));
    const auto literal = grok::profile_selection(environment, launcher, base, base, &profile_error);
    check(literal && literal->profile_path.find("..") != std::string::npos,
        "profile locator retains native dot traversal until filesystem resolution");
#if !defined(_WIN32)
    const auto path_root = std::filesystem::temp_directory_path() / ("grok-profile-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_root / "outside" / "child");
    std::filesystem::create_directories(path_root / "outside" / "grok");
    std::filesystem::create_directories(path_root / "profiles");
    std::filesystem::create_directory_symlink(path_root / "outside" / "child", path_root / "profiles" / "link");
    const auto through_link = path_root / "profiles" / "link" / ".." / "grok";
    check(grok::profile_account_key(through_link.string()) == grok::profile_account_key((path_root / "outside" / "grok").string()),
        "account identity follows symlink before parent traversal");
    const auto absent = path_root / "profiles" / "link" / ".." / "new-profile";
    const auto before_creation = grok::profile_account_key(absent.string());
    std::filesystem::create_directories(absent);
    check(before_creation == grok::profile_account_key(absent.string()) &&
        before_creation == grok::profile_account_key((path_root / "outside" / "new-profile").string()),
        "account identity stays stable when native creation resolves an existing symlink ancestor");
    std::filesystem::remove_all(path_root);
#endif
    addon::Probe_inspect_request request;
    request.inspection.final = true;
    request.inspection.exit_code = 0;
    for (const std::string banner : {"You are using XAI_API_KEY.", "You are logged in with grok.com.",
            "Model 'custom-model' is using its own API key.", "You are authenticated via deployment key."}) {
        request.inspection.stdout_bytes = banner + "\n\nAvailable models:\n";
        check(grok::inspect_auth(request).observation->state == addon::Auth_status::AUTHENTICATED,
            "native credential modes recognized without reading secrets");
    }
    request.inspection.stdout_bytes = "You are not authenticated.\n";
    check(grok::inspect_auth(request).observation->state == addon::Auth_status::NOT_LOGGED_IN, "native negative status recognized");
    request.inspection.timed_out = true;
    check(grok::inspect_auth(request).observation->state == addon::Auth_status::UNAVAILABLE, "timeout never means logged out");
}
class Context final : public addon::Invocation_context
{
public:
    addon::Invoke_request input;
    addon::Json_value output;
    bool succeeded = false;
    bool done = false;
    addon::Result_status failed_status = addon::Result_status::FAILED;
    std::string failed_reason;
    const addon::Invoke_request& request() const override { return input; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value value) override { succeeded = true; done = true; output = std::move(value); }
    void fail(addon::Result_status status, std::string reason, std::string) override
    {
        done = true;
        failed_status = status;
        failed_reason = std::move(reason);
    }
    bool finished() const override { return done; }
};
void quota_is_read_by_the_packaged_helper()
{
#if defined(_WIN32)
    const std::string platform = "windows-x86_64";
    const std::string profile  = "C:/Users/Jane Doe/.grok-work";
    const std::string helper   = "helpers/grok_quota_bridge.exe";
#else
    const std::string platform = "linux-x86_64";
    const std::string profile  = "/home/jane/.grok-work";
    const std::string helper   = "helpers/grok_quota_bridge";
#endif
    addon::Probe_prepare_request prepare;
    prepare.context.launcher.script_body = "grok";
    prepare.context.launcher.runner_key  = addon::k_runner_direct;
    prepare.context.launcher_argv        = {"grok"};
    prepare.context.working_directory    = std::filesystem::path(profile).parent_path().generic_string();
    prepare.context.account_context      = addon::Account_context{};
    prepare.context.account_context->version             = 2;
    prepare.context.account_context->adapter_account_key = grok::profile_account_key(profile);
    prepare.context.account_context->profile_path        = profile;
    prepare.context.launch_environment = addon::Launch_environment{};
    prepare.context.launch_environment->platform_id = platform;
    prepare.context.launch_environment->effective_environment = addon::Json_value::object();
    prepare.context.launch_environment->effective_environment.set("GROK_HOME", addon::Json_value(profile));
    const auto prepare_quota = [&](Context& context) {
        context.input.capability.capability_id = "quota";
        context.input.operation = addon::k_prepare_operation;
        context.input.payload = addon::encode_probe_prepare_request(prepare, 2);
        grok::handle_probes(context);
    };
    Context prepared;
    prepare_quota(prepared);
    addon::Contract_failure failure;
    const auto proposal = addon::decode_structured_prepare_result(prepared.output, &failure, 2);
    check(prepared.succeeded && proposal && proposal->process.executable == addon::Process_executable(helper) &&
        proposal->process.argv.empty() && proposal->inspection.protocol_state == "await_bridge" &&
        proposal->process.environment == std::vector<std::pair<std::string, std::string>>{{"GROK_HOME", profile}},
        "quota runs the packaged helper with the selected profile and nothing else");
    check(addon::validate_structured_process_proposal(proposal->process, &failure),
        "the host admits the helper proposal");

    prepare.context.launch_environment->effective_environment.set(
        "GROK_CLI_CHAT_PROXY_BASE_URL", addon::Json_value("https://proxy.example.com/v1"));
    Context rerouted;
    prepare_quota(rerouted);
    check(!rerouted.succeeded && rerouted.done && rerouted.failed_status == addon::Result_status::UNAVAILABLE &&
        rerouted.failed_reason == "quota_route_overridden",
        "a launcher that routes Grok elsewhere has no quota the helper can read");

    addon::Probe_inspect_request request;
    request.account_context = prepare.context.account_context;
    request.inspection.protocol_state = "await_bridge";
    request.inspection.final = true;
    request.inspection.exit_code = 0;
    addon::Quota_inspect_result result;
    result.complete = true;
    result.observation = addon::Quota_observation{};
    result.observation->state = addon::Quota_status::READY;
    result.observation->primary = addon::Quota_window{};
    result.observation->primary->used_percent   = 125.0;
    result.observation->primary->window_minutes = 7 * 24 * 60;
    result.observation->primary->resets_at_ms   = 1'790'000'000'000;
    result.observation->plan = "SuperGrok";
    const auto inspect = [&](const std::string& output) {
        request.inspection.stdout_bytes = output;
        Context context;
        context.input.capability.capability_id = "quota";
        context.input.operation = addon::k_inspect_operation;
        context.input.payload = addon::encode_probe_inspect_request(request, 2);
        grok::handle_probes(context);
        check(context.succeeded, "the adapter completes every helper outcome as an observation");
        addon::Contract_failure inspect_failure;
        const auto decoded = addon::decode_quota_inspect_result(context.output, true, &inspect_failure, 2);
        check(decoded && decoded->complete && decoded->observation &&
            decoded->observation->scope.source_account_key == prepare.context.account_context->adapter_account_key &&
            decoded->observation->scope.observed_at_ms > 0 && decoded->observation->scope.fresh_for_ms == 0,
            "the adapter stamps the selected account and the observation time");
        return *decoded->observation;
    };
    const auto ready = inspect(addon::serialize_json(addon::encode_quota_inspect_result(result, 2)) + '\n');
    check(ready.state == addon::Quota_status::READY && ready.primary && ready.primary->used_percent == 125.0 &&
        ready.primary->window_minutes == result.observation->primary->window_minutes &&
        ready.primary->resets_at_ms == result.observation->primary->resets_at_ms && ready.plan == "SuperGrok",
        "the helper's figures reach the host unchanged");
    check(inspect("").state == addon::Quota_status::FAILED, "a helper that wrote no result failed the attempt");
    request.inspection.exit_code = 1;
    check(inspect(addon::serialize_json(addon::encode_quota_inspect_result(result, 2))).state ==
        addon::Quota_status::FAILED, "a helper that exited with an error failed the attempt");
    request.inspection.exit_code = 0;
    request.inspection.timed_out = true;
    check(inspect("").state == addon::Quota_status::FAILED, "a helper that timed out failed the attempt");
}
void accounts_are_named_after_their_profile_folders()
{
    // Oracle: accounts@1 names an account with a filesystem profile after that
    // profile's folder, which tells it apart from the provider's other accounts.
#if defined(_WIN32)
    const std::string platform = "windows-x86_64";
    const std::string home     = "C:/Users/Jane Doe";
    const std::string home_env = "USERPROFILE";
#else
    const std::string platform = "linux-x86_64";
    const std::string home     = "/home/jane";
    const std::string home_env = "HOME";
#endif
    const auto discovered_name = [&](const char* grok_home) {
        addon::Accounts_launcher_request request;
        request.launcher.script_body = "grok";
        request.launcher.runner_key  = addon::k_runner_direct;
        request.launch_environment.platform_id = platform;
        request.launch_environment.effective_environment = addon::Json_value::object();
        request.launch_environment.effective_environment.set(home_env, addon::Json_value(home));
        if (grok_home) request.launch_environment.effective_environment.set("GROK_HOME", addon::Json_value(grok_home));
        request.default_home_fallback   = home;
        request.normalization_directory = home;
        Context context;
        context.input.operation = addon::k_accounts_discover_operation;
        context.input.payload   = addon::encode_accounts_launcher_request(request);
        grok::handle_accounts(context);
        addon::Contract_failure failure;
        const auto accounts = addon::decode_accounts_result(context.output, &failure);
        check(context.succeeded && accounts && accounts->size() == 1, "discovery returns the selected account");
        return accounts->front().display_name;
    };
    check(discovered_name(nullptr) == ".grok", "the default profile is named .grok");
    check(discovered_name((home + "/.grok-work").c_str()) == ".grok-work", "a GROK_HOME profile is named after its folder");
    check(discovered_name((home + "/.grok-work/").c_str()) == ".grok-work", "a trailing separator does not hide the folder name");
    check(discovered_name((home + "/profiles/link/../.grok-work").c_str()) == ".grok-work",
        "parent traversal inside GROK_HOME does not change the folder name");
    check(discovered_name((home + "/.grok-review").c_str()) != discovered_name((home + "/.grok-work").c_str()),
        "two profiles of one provider are named apart");
    const std::string long_home = home + "/.grok-account-for-the-varinomics-nightly-release-builds";
    check(discovered_name(long_home.c_str()) == addon::account_display_name(long_home),
        "an over-long folder name is fitted by the SDK's account display-name rule");
}
} // namespace
int main()
{
    try { exercise(); quota_is_read_by_the_packaged_helper(); accounts_are_named_after_their_profile_folders(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
