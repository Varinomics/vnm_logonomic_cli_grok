#include "grok_billing_http.h"

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <string>

namespace grok {
namespace {

constexpr std::size_t k_max_response_bytes = 64 * 1024;

bool header_value_safe(std::string_view value, std::size_t limit)
{
    return !value.empty() && value.size() <= limit &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x21 && byte <= 0x7e && byte != ':' && byte != '\\';
        });
}

#if defined(_WIN32)

struct Internet_handle
{
    HINTERNET value = nullptr;
    ~Internet_handle() { if (value) WinHttpCloseHandle(value); }
};

std::wstring widen_ascii(std::string_view text)
{
    return std::wstring(text.begin(), text.end());
}

#else

std::size_t append_body(char* bytes, std::size_t size, std::size_t count, void* context)
{
    auto& body = *static_cast<std::string*>(context);
    if (size && count > k_max_response_bytes / size) return 0;
    const auto length = size * count;
    if (length > k_max_response_bytes - body.size()) return 0;
    body.append(bytes, length);
    return length;
}

#endif

} // namespace

std::optional<Billing_http_response> get_billing_resource(
    std::string_view resource, std::string_view bearer,
    std::string_view user_id, std::string_view client_version)
{
    if (resource != "/v1/billing?format=credits" && resource != "/v1/settings") return std::nullopt;
    if (!header_value_safe(bearer, 4096) || !header_value_safe(user_id, 256) ||
        (!client_version.empty() && !header_value_safe(client_version, 64))) return std::nullopt;

    const std::string authorization = "Authorization: Bearer " + std::string(bearer);
    const std::string user_header = "x-userid: " + std::string(user_id);
    const std::string version_header = "x-grok-client-version: " + std::string(client_version);
#if defined(_WIN32)
    Internet_handle session{WinHttpOpen(L"Logonomic/GrokQuota", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value || !WinHttpSetTimeouts(session.value, 2000, 2000, 2000, 2000)) return std::nullopt;
    Internet_handle connection{WinHttpConnect(session.value, L"cli-chat-proxy.grok.com",
        INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection.value) return std::nullopt;
    const auto path = widen_ascii(resource);
    Internet_handle request{WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request.value) return std::nullopt;
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY,
            &redirect_policy, sizeof(redirect_policy))) return std::nullopt;
    std::wstring headers = widen_ascii(authorization) + L"\r\n" +
        L"X-XAI-Token-Auth: xai-grok-cli\r\n" + widen_ascii(user_header) + L"\r\n";
    if (!client_version.empty()) headers += widen_ascii(version_header) + L"\r\n";
    headers += L"x-grok-client-mode: headless\r\nCache-Control: no-cache\r\n";
    if (!WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(headers.size()),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.value, nullptr))
        return std::nullopt;
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX))
        return std::nullopt;
    Billing_http_response result;
    result.status = status;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
        char buffer[4096];
        DWORD read = 0;
        if (!WinHttpReadData(request.value, buffer, sizeof(buffer), &read)) return std::nullopt;
        if (read == 0) return result;
        if (read > k_max_response_bytes - result.body.size()) return std::nullopt;
        result.body.append(buffer, read);
    }
#else
    static const bool curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!curl_ready) return std::nullopt;
    CURL* curl = curl_easy_init();
    if (!curl) return std::nullopt;
    curl_slist* headers = nullptr;
    const auto append_header = [&](const char* value) {
        auto* updated = curl_slist_append(headers, value);
        if (!updated) return false;
        headers = updated;
        return true;
    };
    if (!append_header(authorization.c_str()) ||
        !append_header("X-XAI-Token-Auth: xai-grok-cli") ||
        !append_header(user_header.c_str()) ||
        (!client_version.empty() && !append_header(version_header.c_str())) ||
        !append_header("x-grok-client-mode: headless") ||
        !append_header("Cache-Control: no-cache"))
    {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return std::nullopt;
    }
    Billing_http_response result;
    const auto url = "https://cli-chat-proxy.grok.com" + std::string(resource);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    const auto code = curl_easy_perform(curl);
    if (code == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) return std::nullopt;
    return result;
#endif
}

} // namespace grok
