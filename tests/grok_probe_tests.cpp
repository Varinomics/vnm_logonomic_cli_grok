#include "grok_accounts.h"
#include "grok_probes.h"
#include "grok_launcher.h"

#include <logonomic_cli_addon/package_source.h>

#include <iostream>
#include <filesystem>
#include <chrono>
#include <stdexcept>
#include <string>

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

    request = {};
    request.inspection.protocol_state = "await_initialize";
    request.inspection.stdout_bytes = R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":1}})" "\n";
    const auto step = grok::inspect_quota(request);
    check(!step.complete && step.stdin_bytes && step.stdin_bytes->find("_x.ai/billing") != std::string::npos,
        "quota issues only the native read-only billing RPC after initialization");
    request.inspection.protocol_state = step.protocol_state;
    request.inspection.stdout_bytes = R"({"id":2,"result":{"config":{"creditUsagePercent":63.5,"monthlyLimit":{"val":10000},"used":{"val":9000},"currentPeriod":{"type":"USAGE_PERIOD_TYPE_WEEKLY","end":"2026-09-20T12:00:00Z"},"prepaidBalance":{"val":1234}},"subscription_tier":"SuperGrok"}})" "\n";
    const auto quota = grok::inspect_quota(request);
    check(quota.complete && quota.observation->state == addon::Quota_status::READY &&
        quota.observation->primary->used_percent == 63.5 && quota.observation->credits_balance == "12.34",
        "native percentage wins over deprecated cents and prepaid cents stay exact");
    addon::Contract_failure failure;
    check(addon::decode_quota_inspect_result(addon::encode_quota_inspect_result(quota, 2), request.inspection.final, &failure, 2).has_value(),
        "native quota observation satisfies the host decimal credit contract");
    for (const double percent : {-7.5, 125.0}) {
        request.inspection.stdout_bytes = "{\"id\":2,\"result\":{\"config\":{\"creditUsagePercent\":" +
            std::to_string(percent) + "}}}\n";
        const auto observation = grok::inspect_quota(request);
        check(observation.observation->primary->used_percent == percent &&
            addon::decode_quota_inspect_result(addon::encode_quota_inspect_result(observation, 2), request.inspection.final, &failure, 2).has_value(),
            "quota-scan preserves every finite signed percentage without saturation");
    }
    request.inspection.stdout_bytes = R"({"id":2,"result":{"config":{"monthlyLimit":{"val":10000},"used":{"val":15000},"onDemandCap":{"val":2000},"onDemandUsed":{"val":3000},"prepaidBalance":{"val":-125}}}})" "\n";
    const auto overage = grok::inspect_quota(request);
    check(overage.observation->primary->used_percent == 150.0 &&
        overage.observation->secondary->used_percent == 150.0 && overage.observation->credits_balance == "-1.25" &&
        addon::decode_quota_inspect_result(addon::encode_quota_inspect_result(overage, 2), request.inspection.final, &failure, 2).has_value(),
        "cents-derived primary and on-demand overages and signed balances survive host decoding");
    request.inspection.stdout_bytes = R"({"id":2,"result":{"config":{"onDemandCap":{"val":0}},"subscription_tier":"SuperGrok Lite"}})" "\n";
    const auto unmetered = grok::inspect_quota(request);
    check(unmetered.complete && unmetered.observation->state == addon::Quota_status::UNAVAILABLE &&
        addon::decode_quota_inspect_result(addon::encode_quota_inspect_result(unmetered, 2), request.inspection.final, &failure, 2).has_value(),
        "a plan whose billing reply meters nothing reports quota unavailable, not ready without values");
    request.inspection.stdout_bytes = R"({"id":2,"error":{"code":-32603,"message":"secret diagnostic"}})" "\n";
    const auto failed = grok::inspect_quota(request);
    check(failed.complete && failed.observation->state == addon::Quota_status::FAILED,
        "upstream failure is explicit and raw diagnostics are not propagated");
}
} // namespace
int main()
{
    try { exercise(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
