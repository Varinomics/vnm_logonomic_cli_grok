#include <logonomic_cli_addon/package_source.h>
#include "grok_accounts.h"
#include "grok_launcher.h"

#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/sha256.h>

#if defined(_WIN32)
#include <windows.h>
#endif

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

} // namespace

std::string profile_account_key(std::string_view absolute_path)
{
    // Resolve the existing ancestor before normalizing an absent suffix. The
    // account key must not change when the CLI creates that profile. This is
    // identity only: the native locator and launch value remain verbatim.
    std::error_code error;
    fs::path path = fs::weakly_canonical(logonomic::cli_addon::path_from_utf8(absolute_path), error);
    if (error) path = logonomic::cli_addon::path_from_utf8(absolute_path);
    path = logonomic::cli_addon::path_from_utf8(path_text(path));
#if defined(_WIN32)
    std::wstring wide = path.generic_wstring();
    const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr, 0);
    if (size == 0) return {};
    std::wstring folded(static_cast<std::size_t>(size), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(),
            static_cast<int>(wide.size()), folded.data(), size, nullptr, nullptr, 0))
    {
        return {};
    }
    path = fs::path(folded);
#endif
    const auto bytes = path.generic_u8string();
    return "profile-" + addon::to_hex(addon::Sha256::digest(std::string(bytes.begin(), bytes.end())));
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
    auto actual = logonomic::cli_addon::path_from_utf8(*raw);
    if (!actual.is_absolute()) actual = logonomic::cli_addon::path_from_utf8(request.working_directory) / actual;
    if (profile_account_key(path_text(actual)) != selected->adapter_account_key) {
        context.fail(addon::Result_status::UNAVAILABLE, "account_context_mismatch");
        return {};
    }
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
    if (selected.adapter_account_key != profile_account_key(selected.profile_path)) {
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
