#include "grok_billing.h"

#include <logonomic_cli_addon/json.h>

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace {
namespace addon = logonomic::cli_addon;

std::optional<std::string> selected_profile()
{
#ifdef _WIN32
    const DWORD length = GetEnvironmentVariableW(L"GROK_HOME", nullptr, 0);
    if (length == 0 || length > 4096) return std::nullopt;
    std::wstring wide(length, L'\0');
    if (GetEnvironmentVariableW(L"GROK_HOME", wide.data(), length) != length - 1)
        return std::nullopt;
    wide.resize(length - 1);
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return std::nullopt;
    std::string profile(bytes, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
            static_cast<int>(wide.size()), profile.data(), bytes, nullptr, nullptr) != bytes)
        return std::nullopt;
    return profile;
#else
    const char* value = std::getenv("GROK_HOME");
    if (!value || !*value) return std::nullopt;
    const std::string profile(value);
    return profile.size() <= 4096 ? std::optional(profile) : std::nullopt;
#endif
}

int run()
{
    const auto profile = selected_profile();
    if (!profile) return 1;
    const auto result = grok::fetch_billing_quota(*profile);
    const auto output = addon::serialize_json(addon::encode_quota_inspect_result(result, 2, true)) + '\n';
    return std::fwrite(output.data(), 1, output.size(), stdout) == output.size() &&
        std::fflush(stdout) == 0 ? 0 : 1;
}
} // namespace

int main()
{
#ifdef _WIN32
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 1;
#endif
    return run();
}
