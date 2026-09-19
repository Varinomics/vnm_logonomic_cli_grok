#include "grok_accounts.h"
#include "grok_configuration.h"
#include "grok_launcher.h"
#include "grok_lifecycle.h"
#include "grok_preparation.h"
#include "grok_probes.h"
#include "grok_usage.h"
#include <logonomic_cli_addon/profile_storage.h>
#include <exception>
namespace {
namespace addon = logonomic::cli_addon;
void discovery(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    if (context.request().operation == "discover") {
        const auto request = addon::decode_discover_request(context.request().payload, &failure);
        if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
        const auto candidate = grok::discover_launcher(*request);
        context.succeed(addon::encode_candidates(candidate ? std::vector<addon::Launcher_candidate>{*candidate} : std::vector<addon::Launcher_candidate>{}));
    }
    else if (context.request().operation == "recognize") {
        const auto request = addon::decode_recognize_request(context.request().payload, &failure);
        if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
        addon::Launcher_recognition result;
        result.recognized = grok::recognize_launcher(request->launcher.script_body).has_value();
        if (result.recognized) result.display_name = "Grok Build";
        context.succeed(addon::encode_recognition(result));
    }
    else context.reject_unknown_operation();
}
void invoke(addon::Invocation_context& context)
{
    try {
        const auto& capability = context.request().capability.capability_id;
        if (context.cancel_requested()) context.fail(addon::Result_status::CANCELLED, "cancelled");
        else if (capability == "launcher-discovery") discovery(context);
        else if (capability == "accounts") grok::handle_accounts(context);
        else if (capability == "auth-status" || capability == "quota") grok::handle_probes(context);
        else if (capability == "usage-scan") grok::handle_usage(context);
        else if (capability == "managed-configuration") grok::transform_configuration(context);
        else if (capability == "helper-ingress") grok::handle_lifecycle(context);
        else if (capability == "profile-storage" && context.request().operation == "acknowledge") {
            const auto* value = context.request().payload.find("profile_storage");
            const auto storage = value ? addon::decode_profile_storage_context(*value, nullptr) : std::nullopt;
            if (!storage || context.request().payload.as_object().size() != 1) {
                context.fail(addon::Result_status::FAILED, "profile_storage_mismatch"); return;
            }
            auto result = addon::Json_value::object();
            result.set("profile_storage", addon::profile_storage_member(*storage));
            context.succeed(std::move(result));
        }
        else grok::handle_preparation(context);
    }
    catch (const std::exception&) {
        context.fail(addon::Result_status::FAILED, "grok_operation_failed");
    }
}
}
int main()
{
    addon::Adapter_runtime_options options;
    options.adapter_name = "grok-addon";
    options.adapter_version = "1.0.0";
    options.capabilities = {
        {"presentation", 1}, {"launcher-discovery", 1}, {"launch-preparation", 3},
        {"managed-configuration", 2}, {"profile-storage", 1}, {"accounts", 2},
        {"auth-status", 2}, {"quota", 2}, {"usage-scan", 2},
        {"lifecycle-events", 2}, {"helper-ingress", 2}, {"coordination-delivery", 1},
        {"model-context-delivery", 1}, {"interactive-resume", 2}, {"structured-session", 2}, {"delegated-session", 2},
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler(invoke);
    return runtime.run();
}
