#include <logonomic_cli_addon/package_source.h>
#include "grok_accounts.h"
#include "grok_launcher.h"

#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace grok {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

std::string path_text(const fs::path& path)
{
    const auto bytes = path.u8string();
    std::string result(bytes.begin(), bytes.end());
#if defined(_WIN32)
    if (result.starts_with("\\\\?\\UNC\\")) result = "\\\\" + result.substr(8);
    else
    if (result.starts_with("\\\\?\\")) result.erase(0, 4);
#endif
    return result;
}

std::string ascii_lower(std::string text)
{
    for (char& byte : text) {
        if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
    }
    return text;
}

std::optional<std::string> environment_value(const addon::Json_value& environment, std::string_view name, bool windows)
{
    for (const auto& item : environment.as_object()) {
        if ((windows ? ascii_lower(item.name) == ascii_lower(std::string(name)) : item.name == name) &&
            item.value.is_string())
        {
            return item.value.as_string();
        }
    }
    return {};
}

std::string key_from_identity(std::optional<std::string> identity)
{
    if (!identity) return {};
#if defined(_WIN32)
    // Grok historically hashed generic separators. A swap preserves every
    // distinction in the SDK's opaque identity, including its binary domains.
    for (char& byte : *identity) {
        if (byte == '\\') byte = '/';
        else if (byte == '/') byte = '\\';
    }
#endif
    return "profile-" + addon::to_hex(addon::Sha256::digest(*identity));
}

} // namespace

fs::path canonical_native_path(std::string_view absolute_path)
{
    const auto original = addon::path_from_utf8(absolute_path);
    if (!original.is_absolute()) return {};
    std::error_code error;
    const auto canonical = fs::weakly_canonical(original, error);
    return addon::path_from_utf8(path_text(error ? original : canonical));
}

std::string profile_account_key(std::string_view absolute_path)
{
    return key_from_identity(addon::native_account_profile_identity(canonical_native_path(absolute_path)));
}

std::optional<Profile_selection> profile_selection(const addon::Launch_environment& environment,
    const addon::Launcher_request& launcher, std::string_view fallback,
    std::string_view normalization_directory, std::string* error)
{
    const auto command = recognize_launcher(launcher, environment.platform_id);
    if (!command) { *error = "launcher_unrecognized"; return std::nullopt; }
    if (!command->environment_names.empty() && launcher.runner_key != addon::k_runner_bash) {
        *error = "launcher_assignment_dialect_unavailable"; return std::nullopt;
    }
    if (!launcher_environment(*command, environment.effective_environment)) {
        *error = "launcher_environment_unavailable"; return std::nullopt;
    }
    const bool windows = environment.platform_id.starts_with("windows-");
    const auto resolved_value = [&](std::string_view name) {
        return environment_value(environment.effective_environment, name, windows);
    };
    std::string profile = resolved_value("GROK_HOME").value_or("");
    if (profile.empty()) {
        std::string home = resolved_value(windows ? "USERPROFILE" : "HOME").value_or(std::string(fallback));
        std::error_code canonical_error;
        auto home_path = fs::canonical(logonomic::cli_addon::path_from_utf8(home), canonical_error);
        if (canonical_error) home_path = logonomic::cli_addon::path_from_utf8(home);
        profile = path_text(home_path / ".grok");
    }
    fs::path path = logonomic::cli_addon::path_from_utf8(profile);
    if (!path.is_absolute()) path = logonomic::cli_addon::path_from_utf8(normalization_directory) / path;
    if (!path.is_absolute()) { *error = "profile_path_invalid"; return std::nullopt; }
    Profile_selection selected{profile_account_key(path_text(path)), path_text(path)};
    if (selected.adapter_account_key.empty()) { *error = "profile_path_invalid"; return std::nullopt; }
    return selected;
}

std::optional<std::string> launch_profile_home(const addon::Session_launch_context& request,
    addon::Invocation_context& context)
{
    const auto selected = selected_profile(request, context);
    if (!selected) return {};
    const auto& environment = *request.launch_environment;
    const bool windows = environment.platform_id.starts_with("windows-");
    auto raw = environment_value(environment.effective_environment, "GROK_HOME", windows);
    const auto command = recognize_launcher(request.launcher, environment.platform_id, request.launcher_argv);
    if (!command) { context.fail(addon::Result_status::UNAVAILABLE, "launcher_unrecognized"); return {}; }
    if (!command->environment_names.empty() && request.launcher.runner_key != addon::k_runner_bash) {
        context.fail(addon::Result_status::UNAVAILABLE, "launcher_assignment_dialect_unavailable"); return {};
    }
    if (!launcher_environment(*command, environment.effective_environment)) {
        context.fail(addon::Result_status::UNAVAILABLE, "launcher_environment_unavailable"); return {};
    }
    if (!raw || raw->empty()) return selected->profile_path;
    return raw;
}

std::optional<Profile_selection> selected_profile(const addon::Session_launch_context& request,
    addon::Invocation_context& context)
{
    if (!request.account_context || !request.account_context->profile_path) {
        context.fail(addon::Result_status::UNAVAILABLE, "account_profile_unavailable");
        return std::nullopt;
    }
    Profile_selection selected{request.account_context->adapter_account_key, *request.account_context->profile_path};
    const auto path = canonical_native_path(selected.profile_path);
    if (request.launch_environment) {
        const auto& environment = *request.launch_environment;
        const auto raw = environment_value(environment.effective_environment, "GROK_HOME",
            environment.platform_id.starts_with("windows-"));
        if (raw && !raw->empty()) {
            const auto actual = addon::path_from_utf8(request.working_directory) / addon::path_from_utf8(*raw);
            if (!addon::same_native_account_profile(path, canonical_native_path(path_text(actual)))) {
                context.fail(addon::Result_status::UNAVAILABLE, "account_context_mismatch");
                return std::nullopt;
            }
        }
    }
    if (selected.adapter_account_key != key_from_identity(addon::native_account_profile_identity(path)) &&
        selected.adapter_account_key != key_from_identity(addon::historical_account_profile_identity(path, path)))
    {
        context.fail(addon::Result_status::UNAVAILABLE, "account_context_mismatch");
        return std::nullopt;
    }
    return selected;
}

void handle_accounts(addon::Invocation_context& context)
{
    if (context.cancel_requested()) { context.fail(addon::Result_status::CANCELLED, "cancelled"); return; }
    addon::Contract_failure failure;
    const auto request = addon::decode_accounts_launcher_request(context.request().payload, &failure);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
    std::string error;
    const auto selected = profile_selection(request->launch_environment, request->launcher,
        request->default_home_fallback, request->normalization_directory, &error);
    if (!selected) { context.fail(addon::Result_status::FAILED, error); return; }
    if (context.request().operation == addon::k_accounts_discover_operation) {
        std::error_code failure;
        const bool exists = fs::is_directory(logonomic::cli_addon::path_from_utf8(selected->profile_path), failure);
        addon::Account_record account;
        account.adapter_account_key = selected->adapter_account_key;
        account.profile_path = selected->profile_path;
        account.display_name = addon::account_display_name(selected->profile_path);
        account.status = exists ? addon::Account_status::AVAILABLE : addon::Account_status::UNAVAILABLE;
        context.succeed(addon::encode_accounts({account}));
    }
    else
    if (context.request().operation == addon::k_accounts_resolve_operation) {
        context.succeed(addon::encode_account_resolution({selected->adapter_account_key}));
    }
    else context.reject_unknown_operation();
}

} // namespace grok
