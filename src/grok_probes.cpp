#include "grok_probes.h"
#include "grok_accounts.h"
#include "grok_billing.h"
#include "grok_launcher.h"

#include <logonomic_cli_addon/quota.h>

#include <chrono>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grok {
namespace {

namespace addon = logonomic::cli_addon;

std::string_view trimmed(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return value;
}

addon::Observation_scope scope(const addon::Probe_inspect_request& request)
{
    addon::Observation_scope result;
    if (request.account_context) result.source_account_key = request.account_context->adapter_account_key;
    result.observed_at_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    return result;
}

void assign_environment(std::vector<std::pair<std::string, std::string>>* environment,
    std::string name, std::string value, bool windows)
{
    for (auto& assignment : *environment) {
        if (windows ? addon::ascii_lowercase(assignment.first) == addon::ascii_lowercase(name) : assignment.first == name) {
            assignment = {std::move(name), std::move(value)};
            return;
        }
    }
    environment->emplace_back(std::move(name), std::move(value));
}

} // namespace

addon::Auth_inspect_result inspect_auth(const addon::Probe_inspect_request& request)
{
    addon::Auth_inspect_result result;
    result.complete = request.inspection.final;
    if (!result.complete) { result.protocol_state = request.inspection.protocol_state; return result; }
    addon::Auth_observation observation;
    observation.scope = scope(request);
    observation.state = addon::Auth_status::UNAVAILABLE;
    if (!request.inspection.timed_out && request.inspection.exit_code == 0) {
        const auto output = trimmed(request.inspection.stdout_bytes);
        const auto line = trimmed(output.substr(0, output.find('\n')));
        if (line == "You are not authenticated.") observation.state = addon::Auth_status::NOT_LOGGED_IN;
        else
        if (line == "You are using XAI_API_KEY." || line == "You are authenticated via deployment key." ||
            (line.starts_with("You are logged in with ") && line.ends_with('.')) ||
            (line.starts_with("Model '") && line.ends_with("' is using its own API key.")))
            observation.state = addon::Auth_status::AUTHENTICATED;
    }
    result.observation = observation;
    return result;
}

void handle_probes(addon::Invocation_context& context)
{
    if (context.cancel_requested()) { context.fail(addon::Result_status::CANCELLED, "cancelled"); return; }
    const bool quota = context.request().capability.capability_id == "quota";
    addon::Contract_failure failure;
    if (context.request().operation == addon::k_prepare_operation) {
        const auto request = addon::decode_probe_prepare_request(context.request().payload, &failure, 2);
        if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
        const auto native_home = launch_profile_home(request->context, context);
        if (!native_home) return;
        const auto& environment = *request->context.launch_environment;
        addon::Structured_prepare_result result;
        if (quota) {
            const bool windows = environment.platform_id.starts_with("windows-");
            for (const auto& item : environment.effective_environment.as_object()) {
                if (is_quota_route_override(item.name, windows)) {
                    context.fail(addon::Result_status::UNAVAILABLE, "quota_route_overridden");
                    return;
                }
            }
            const auto selected = selected_profile(request->context, context);
            if (!selected) return;
#if defined(_WIN32)
            result.process.executable = "helpers/grok_quota_bridge.exe";
#else
            result.process.executable = "helpers/grok_quota_bridge";
#endif
            assign_environment(&result.process.environment, "GROK_HOME", selected->profile_path, windows);
        }
        else {
            const auto command = recognize_launcher(request->context.launcher.script_body);
            if (!command) { context.fail(addon::Result_status::UNAVAILABLE, "launcher_unrecognized"); return; }
            const auto argv = resolve_probe_launcher(*command, {"models"},
                environment.effective_environment, request->context.working_directory);
            if (!argv) { context.fail(addon::Result_status::UNAVAILABLE, "launcher_program_unavailable"); return; }
#if defined(_WIN32)
            result.process.executable = "helpers/grok_launch_bridge.exe";
#else
            result.process.executable = "helpers/grok_launch_bridge";
#endif
            result.process.argv = *argv;
            const bool windows = environment.platform_id.starts_with("windows-");
            for (const auto& item : environment.effective_environment.as_object()) {
                assign_environment(&result.process.environment, item.name, item.value.as_string(), windows);
            }
            assign_environment(&result.process.environment, "GROK_HOME", *native_home, windows);
        }
        result.profile_storage = request->context.profile_storage;
        result.account_context = request->context.account_context;
        result.inspection.protocol_state = quota ? "await_bridge" : "await_exit";
        context.succeed(addon::encode_structured_prepare_result(result, 2));
        return;
    }
    if (context.request().operation != addon::k_inspect_operation) { context.reject_unknown_operation(); return; }
    const auto request = addon::decode_probe_inspect_request(context.request().payload, &failure, 2);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
    if (!quota) { context.succeed(addon::encode_auth_inspect_result(inspect_auth(*request))); return; }
    if (request->inspection.protocol_state != "await_bridge" || !request->inspection.final) {
        context.fail(addon::Result_status::FAILED, "invalid_quota_bridge_state");
        return;
    }
    // A helper that timed out, failed or wrote no result is a failed
    // observation; the host adds its own timeout diagnostic.
    const auto wire = addon::parse_json(trimmed(request->inspection.stdout_bytes), {}, nullptr);
    auto decoded = wire && !request->inspection.timed_out && request->inspection.exit_code == 0
        ? addon::decode_quota_inspect_result(*wire, true, &failure, 2) : std::nullopt;
    if (!decoded || !decoded->complete || !decoded->observation) {
        decoded = addon::Quota_inspect_result{};
        decoded->complete = true;
        decoded->observation = addon::Quota_observation{};
        decoded->observation->state = addon::Quota_status::FAILED;
    }
    decoded->observation->scope = scope(*request);
    context.succeed(addon::encode_quota_inspect_result(*decoded, 2));
}

} // namespace grok
