#pragma once

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/quota.h>

#include <string_view>

namespace grok {

// Environment names through which Grok takes its credential or its service
// routes from somewhere other than the selected profile's own xAI login. The
// quota helper cannot tell which account or service such a session uses, so
// quota is unavailable whenever one is set: the adapter checks the frozen
// launch environment, the helper its own process environment.
inline constexpr std::string_view k_quota_route_override_names[] = {
    "GROK_AUTH", "GROK_AUTH_PATH", "GROK_CLI_CHAT_PROXY_BASE_URL",
    "GROK_PRODUCTION_CLI_CHAT_PROXY_BASE_URL", "GROK_CONFIG", "GROK_CONFIG_PATH",
    "GROK_OIDC_ISSUER", "GROK_OIDC_CLIENT_ID", "GROK_OAUTH2_ISSUER",
    "GROK_OAUTH2_CLIENT_ID", "GROK_LOCAL_AUTH", "GROK_FORCE_LOGIN_TEAM_ID",
    "GROK_AUTH_PROVIDER_COMMAND", "GROK_DEPLOYMENT_KEY", "GROK_MODELS_BASE_URL",
    "XAI_API_KEY",
};

inline bool is_quota_route_override(std::string_view name, bool windows)
{
    for (const auto candidate : k_quota_route_override_names) {
        if (windows ? logonomic::cli_addon::ascii_lowercase(name) == logonomic::cli_addon::ascii_lowercase(candidate) :
            name == candidate) return true;
    }
    return false;
}

// Reads the selected Grok profile and fetches its current first-party quota.
// No credential or billing response is written to disk or emitted to the host.
logonomic::cli_addon::Quota_inspect_result fetch_billing_quota(std::string_view profile_path);

} // namespace grok
