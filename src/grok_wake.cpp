#include "grok_wake.h"
#include "grok_wake_transport.h"
#include "grok_json.h"
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <filesystem>
#include <algorithm>

namespace grok {
namespace addon = logonomic::cli_addon;

bool wake_compatible_arguments(const std::vector<std::string>& arguments)
{
    // Leader sessions inherit the selected profile, but native Grok explicitly
    // ignores several standalone CLI policy overrides. Only options whose
    // leader semantics are supported enter this launch form. Unknown/new flags
    // retain ordinary standalone execution until their behavior is established.
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& argument = arguments[i];
        const auto separator = argument.find('=');
        const auto option = argument.substr(0, separator);
        if (option == "--always-approve" && separator == std::string::npos) continue;
        if (option != "--model" && option != "-m" && option != "--effort" && option != "--reasoning-effort" &&
            option != "--resume" && option != "-r" && option != "--session-id" && option != "-s") return false;
        if (separator != std::string::npos) {
            if (separator + 1 == argument.size()) return false;
        }
        else if (++i == arguments.size() || arguments[i].empty() || arguments[i].front() == '-') return false;
    }
    return true;
}

void prepare_wake(addon::Launch_patch& patch, const std::vector<std::string>& arguments, std::string_view native_help)
{
    addon::Session_wake_preparation preparation;
    preparation.reason = "native_leader_launch_options_unavailable";
    if (!wake_compatible_arguments(arguments)) {
        patch.session_wake = std::move(preparation);
        return;
    }
    const auto bridge = session_bridge_path();
    std::error_code error;
    if (!patch.argv || patch.argv->empty() || native_help.find("--leader-socket") == std::string_view::npos ||
        bridge.empty() || !std::filesystem::is_regular_file(std::filesystem::path(std::u8string(bridge.begin(), bridge.end())), error)) {
        preparation.reason = "native_leader_launch_unavailable";
    }
    else {
        const auto token = addon::random_nonce_hex();
        std::vector<std::string> argv{bridge, "--serve", token, patch.argv->front()};
        argv.insert(argv.end(), arguments.begin(), arguments.end());
        patch.argv = std::move(argv);
        std::erase_if(patch.set, [](const auto& item) {
            auto name = item.first;
#ifdef _WIN32
            for (auto& ch : name) if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
#endif
            return name == k_wake_token_environment;
        });
        patch.set.emplace_back(k_wake_token_environment, token);
        preparation.available = true;
        preparation.reason.clear();
        preparation.binding = object({{"transport", Json("grok-private-leader-v1")}, {"token", Json(token)}});
    }
    patch.session_wake = std::move(preparation);
}

void handle_wake(addon::Invocation_context& context)
{
    const bool notify = context.request().operation == addon::k_session_wake_notify_operation;
    if (!notify && context.request().operation != addon::k_session_wake_inspect_operation) {
        context.reject_unknown_operation(); return;
    }
    addon::Contract_failure failure;
    std::optional<addon::Session_wake_target> target;
    if (notify) {
        const auto request = addon::decode_session_wake_notify_request(context.request().payload, &failure);
        if (request) target = request->target;
    }
    else {
        const auto request = addon::decode_session_wake_inspect_request(context.request().payload, &failure);
        if (request) target = request->target;
    }
    if (!target) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
    const auto unavailable = [&](std::string reason) {
        if (notify) context.succeed(addon::encode_session_wake_notify_result(
            {addon::Session_wake_disposition::UNAVAILABLE, std::move(reason), std::nullopt}));
        else context.succeed(addon::encode_session_wake_readiness({false, std::move(reason)}));
    };
    const auto token = text(target->binding, "token");
    std::error_code path_error;
    const bool same_bridge = !target->launcher_argv.empty() && std::filesystem::equivalent(
        addon::path_from_utf8(target->launcher_argv.front()),
        addon::path_from_utf8(session_bridge_path()), path_error) && !path_error;
    if (text(target->binding, "transport") != "grok-private-leader-v1" || target->binding.as_object().size() != 2 ||
        token.size() != 32 || token.find_first_not_of("0123456789abcdef") != std::string::npos ||
        target->launcher_argv.size() < 4 || !same_bridge ||
        target->launcher_argv[1] != "--serve" || target->launcher_argv[2] != token) {
        unavailable("native_launch_binding_unavailable"); return;
    }
    const auto response = exchange_wake_request(token, addon::serialize_json(object({
        {"operation", Json(context.request().operation)}, {"payload", context.request().payload}})), context);
    const auto value = response.response ? parse(*response.response) : std::nullopt;
    if (value) {
        if (notify) {
            const auto result = addon::decode_session_wake_notify_result(*value, nullptr);
            if (result) { context.succeed(addon::encode_session_wake_notify_result(*result)); return; }
        }
        else {
            const auto result = addon::decode_session_wake_readiness(*value, nullptr);
            if (result) { context.succeed(addon::encode_session_wake_readiness(*result)); return; }
        }
    }
    if (notify && response.request_written) {
        context.succeed(addon::encode_session_wake_notify_result(
            {addon::Session_wake_disposition::UNCERTAIN, "native_control_response_unavailable", std::nullopt}));
    }
    else unavailable("native_control_unavailable");
}
} // namespace grok
