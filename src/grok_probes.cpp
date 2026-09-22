#include "grok_probes.h"
#include "grok_accounts.h"
#include "grok_launcher.h"

#include <chrono>
#include <cmath>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace grok {
namespace {

namespace addon = logonomic::cli_addon;

const addon::Json_value& member(const addon::Json_value& object, std::string_view name)
{
    static const addon::Json_value missing;
    const auto* value = object.find(name);
    return value ? *value : missing;
}

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

std::optional<addon::Json_value> response_for_id(std::string_view output, int id)
{
    while (!output.empty()) {
        const auto end = output.find('\n');
        const auto line = trimmed(output.substr(0, end));
        const auto value = addon::parse_json(line, {}, nullptr);
        if (value && value->is_object()) {
            const auto& identifier = member(*value, "id");
            if ((identifier.is_number() && identifier.as_number() == id) ||
                (identifier.is_string() && identifier.as_string() == std::to_string(id))) return value;
        }
        if (end == std::string_view::npos) break;
        output.remove_prefix(end + 1);
    }
    return std::nullopt;
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

addon::Quota_inspect_result inspect_quota(const addon::Probe_inspect_request& request)
{
    addon::Quota_inspect_result result;
    const auto fail = [&] {
        result.complete = true;
        addon::Quota_observation observation;
        observation.scope = scope(request);
        observation.state = addon::Quota_status::FAILED;
        result.observation = std::move(observation);
        return result;
    };
    if (const auto response = response_for_id(request.inspection.stdout_bytes, 2)) {
        if (response->find("error")) return fail();
        const auto& payload = member(*response, "result");
        const auto& config = member(payload, "config");
        if (!config.is_object()) return fail();
        for (const auto name : {"monthlyLimit", "used", "onDemandCap", "onDemandUsed", "prepaidBalance"}) {
            const auto& item = member(config, name);
            if (!item.is_null() && (!item.is_object() ||
                (!member(item, "val").is_null() && !member(item, "val").is_integer()))) return fail();
        }
        const auto cents = [&](std::string_view name) -> std::optional<std::int64_t> {
            const auto& item = member(config, name);
            if (item.is_null()) return {};
            const auto& value = member(item, "val");
            if (item.is_object() && value.is_null()) return 0;
            if (!value.is_integer()) return {};
            return value.as_integer();
        };
        const auto limit = cents("monthlyLimit");
        const auto used = cents("used");
        const auto& percent = member(config, "creditUsagePercent");
        std::optional<double> usage;
        if (!percent.is_null()) {
            if (!percent.is_number() || !std::isfinite(percent.as_number())) return fail();
            usage = percent.as_number();
        }
        else
        if (limit && *limit > 0 && used) usage = static_cast<double>(*used) / *limit * 100;
        addon::Quota_observation observation;
        observation.scope = scope(request);
        observation.state = addon::Quota_status::READY;
        if (usage) {
            addon::Quota_window window;
            window.used_percent = *usage;
            const auto& period = member(config, "currentPeriod");
            const auto& type = member(period, "type");
            if (type.is_string()) {
                if (type.as_string() == "USAGE_PERIOD_TYPE_WEEKLY") window.label = "Weekly";
                if (type.as_string() == "USAGE_PERIOD_TYPE_MONTHLY") window.label = "Monthly";
            }
            const auto& end = member(period, "end").is_string() ? member(period, "end") : member(config, "billingPeriodEnd");
            if (end.is_string()) window.reset_text = end.as_string();
            observation.primary = window;
        }
        const auto cap = cents("onDemandCap");
        if (cap && *cap > 0) {
            auto demand = cents("onDemandUsed");
            if (!demand && used && limit && *used >= 0 && *limit >= 0) demand = *used > *limit ? *used - *limit : 0;
            if (demand) {
                addon::Quota_window window;
                window.used_percent = static_cast<double>(*demand) / *cap * 100;
                window.label = "On-demand";
                observation.secondary = window;
            }
        }
        if (const auto credit = cents("prepaidBalance")) {
            const auto magnitude = *credit < 0 ? std::uint64_t(-(*credit + 1)) + 1 : std::uint64_t(*credit);
            observation.credits_balance = (*credit < 0 ? "-" : "") + std::to_string(magnitude / 100) + "." +
                (magnitude % 100 < 10 ? "0" : "") + std::to_string(magnitude % 100);
        }
        const auto& plan = member(payload, "subscription_tier");
        if (plan.is_string() && !plan.as_string().empty()) observation.plan = plan.as_string();
        // A plan can meter nothing this reply reports: SuperGrok Lite has no
        // usage allowance and a zero on-demand cap. Its quota is unavailable,
        // and an unavailable observation carries no values, the plan included.
        if (!observation.primary && !observation.secondary && !observation.credits_balance) {
            observation.state = addon::Quota_status::UNAVAILABLE;
            observation.plan.reset();
        }
        result.complete = true;
        result.observation = observation;
        return result;
    }
    if (!request.inspection.final && request.inspection.protocol_state == "await_initialize") {
        if (const auto response = response_for_id(request.inspection.stdout_bytes, 1)) {
            if (response->find("error") || !member(*response, "result").is_object()) return fail();
            result.protocol_state = "await_quota";
            result.stdin_bytes = R"({"jsonrpc":"2.0","id":2,"method":"_x.ai/billing","params":{}})" "\n";
            return result;
        }
    }
    if (request.inspection.final) return fail();
    result.protocol_state = request.inspection.protocol_state;
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
        const auto command = recognize_launcher(request->context.launcher.script_body);
        if (!command) { context.fail(addon::Result_status::UNAVAILABLE, "launcher_unrecognized"); return; }
        const auto& environment = *request->context.launch_environment;
        const auto argv = resolve_probe_launcher(*command, quota ? std::vector<std::string>{"agent", "--no-leader", "stdio"} :
            std::vector<std::string>{"models"}, environment.effective_environment, request->context.working_directory);
        if (!argv) { context.fail(addon::Result_status::UNAVAILABLE, "launcher_program_unavailable"); return; }
        addon::Structured_prepare_result result;
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
        result.profile_storage = request->context.profile_storage;
        result.account_context = request->context.account_context;
        result.inspection.protocol_state = quota ? "await_initialize" : "await_exit";
        if (quota) {
            result.inspection.incremental = true;
            result.inspection.keep_stdin_open = true;
            result.process.stdin_behavior = addon::Stdin_behavior::PAYLOAD;
            result.process.stdin_payload = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"clientCapabilities":{},"clientInfo":{"name":"logonomic","version":"1"}}})" "\n";
        }
        context.succeed(addon::encode_structured_prepare_result(result, 2));
        return;
    }
    if (context.request().operation != addon::k_inspect_operation) { context.reject_unknown_operation(); return; }
    const auto request = addon::decode_probe_inspect_request(context.request().payload, &failure, 2);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
    if (!quota) { context.succeed(addon::encode_auth_inspect_result(inspect_auth(*request))); return; }
    auto result = inspect_quota(*request);
    context.succeed(addon::encode_quota_inspect_result(result, 2));
}

} // namespace grok
