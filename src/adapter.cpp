#include "grok_accounts.h"
#include "grok_configuration.h"
#include "grok_launcher.h"
#include "grok_launcher_approval.h"
#include "grok_lifecycle.h"
#include "grok_preparation.h"
#include "grok_probes.h"
#include "grok_usage.h"
#include "grok_wake.h"
#include <logonomic_cli_addon/capability_ids.h>
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
        result.recognized = grok::recognize_launcher(request->launcher, request->platform_id).has_value();
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
        else
        if (capability == addon::k_launcher_approval_capability_id) {
            addon::handle_launcher_approval(context, grok::launcher_approval_rules());
        }
        else if (capability == "launcher-discovery") discovery(context);
        else if (capability == "accounts") grok::handle_accounts(context);
        else if (capability == "auth-status" || capability == "quota") grok::handle_probes(context);
        else if (capability == "usage-scan") grok::handle_usage(context);
        else if (capability == "managed-configuration") grok::transform_configuration(context);
        else if (capability == "helper-ingress" || capability == addon::k_attention_kind_capability_id) grok::handle_lifecycle(context);
        else if (capability == "session-wake") grok::handle_wake(context);
        else if (capability == "profile-storage" && context.request().operation == "acknowledge") {
            addon::acknowledge_profile_storage(context);
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
        {addon::k_launcher_approval_capability_id, addon::k_launcher_approval_version},
        {addon::k_interactive_initial_prompt_capability_id, addon::k_interactive_initial_prompt_version},
        {"presentation", 1}, {"launcher-discovery", 1}, {"launch-preparation", 3},
        {"managed-configuration", 2}, {"profile-storage", 1}, {"accounts", 2},
        {"auth-status", 2}, {"quota", 2}, {"quota-failure", 1}, {"usage-scan", 3}, {"usage-temporal", 1},
        {"lifecycle-events", 2}, {"helper-ingress", 2}, {"context-state", 2}, {"coordination-delivery", 1},
        {"model-context-delivery", 1}, {"interactive-resume", 2}, {"interactive-resume", 3}, {"interactive-resume", 4},
        {"execution-context", 1}, {"structured-session", 3}, {"delegated-session", 3},
        {"session-wake", 1},
        {addon::k_attention_kind_capability_id, addon::k_attention_kind_version},
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler(invoke);
    return runtime.run();
}
