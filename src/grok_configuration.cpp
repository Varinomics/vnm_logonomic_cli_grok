#include "grok_configuration.h"
#include <logonomic_cli_addon/managed_configuration.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <initializer_list>
namespace grok {
namespace addon = logonomic::cli_addon;
namespace {
std::string hooks_for_events(std::initializer_list<const char*> events)
{
#ifdef _WIN32
    // EncodedCommand prevents the vendor's POSIX expansion pass and whichever
    // Windows shell it selected from interpreting this PowerShell program.
    const std::string script =
        "$d=[Environment]::GetEnvironmentVariable('LOGONOMIC_PROVIDER_HELPERS_DIR');"
        "$a=[Environment]::GetEnvironmentVariable('LOGONOMIC_SESSION_CAPABILITY');"
        "if($d -and $a){& (Join-Path $d 'grok_helper.exe')};exit 0";
    std::string utf16;
    for (char ch : script) { utf16 += ch; utf16 += '\0'; }
    const std::string command = "powershell.exe -NoLogo -NoProfile -NonInteractive -EncodedCommand " +
        addon::encode_base64(utf16);
#else
    // Grok deliberately preserves modifier expressions for the runtime shell.
    // An ordinary native launch has no authority and cleanly does nothing.
    const std::string command =
        "if [ -n \"${LOGONOMIC_PROVIDER_HELPERS_DIR:-}\" ] && "
        "[ -n \"${LOGONOMIC_SESSION_CAPABILITY:-}\" ]; then "
        "exec \"${LOGONOMIC_PROVIDER_HELPERS_DIR:-}/grok_helper\"; fi";
#endif
    auto hooks = addon::Json_value::object();
    for (const auto* event : events) {
        auto handler = addon::Json_value::object();
        handler.set("type", addon::Json_value("command"));
        handler.set("command", addon::Json_value(command));
        handler.set("timeout", addon::Json_value(3));
        auto handlers = addon::Json_value::array();
        handlers.as_array().push_back(std::move(handler));
        auto group = addon::Json_value::object();
        group.set("hooks", std::move(handlers));
        auto groups = addon::Json_value::array();
        groups.as_array().push_back(std::move(group));
        hooks.set(event, std::move(groups));
    }
    auto root = addon::Json_value::object();
    root.set("hooks", std::move(hooks));
    return addon::serialize_json(root) + '\n';
}
} // namespace
std::string hook_configuration()
{
    return hooks_for_events({"SessionStart", "SessionEnd", "UserPromptSubmit",
        "PreToolUse", "PostToolUse", "Stop", "StopFailure", "StopCancelled", "PostCompact", "SubagentStart"});
}
void transform_configuration(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_managed_configuration_transform_operation) {
        context.reject_unknown_operation(); return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_managed_configuration_transform_request(context.request().payload, &failure);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
    const auto desired = hook_configuration();
    // This declaration owns one dedicated file, never the surrounding hook
    // directory. Only the exact previous generated output is also ours;
    // preserve refusal for a user-modified or otherwise foreign file.
    if (request->present && request->content != desired &&
        request->content != hooks_for_events({"SessionStart", "SessionEnd", "UserPromptSubmit",
            "PreToolUse", "PostToolUse", "Stop", "StopFailure", "StopCancelled", "PostCompact"}) &&
        request->content != hooks_for_events({"SessionStart", "SessionEnd", "UserPromptSubmit",
            "PreToolUse", "StopFailure", "StopCancelled"}))
    {
        context.fail(addon::Result_status::UNAVAILABLE, "managed_hook_file_conflict"); return;
    }
    context.succeed(addon::encode_managed_configuration_transform_result({request->grant, desired}));
}
} // namespace grok
