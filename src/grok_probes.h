#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/auth_status.h>
#include <logonomic_cli_addon/quota.h>

namespace grok {

logonomic::cli_addon::Auth_inspect_result inspect_auth(
    const logonomic::cli_addon::Probe_inspect_request& request);
logonomic::cli_addon::Quota_inspect_result inspect_quota(
    const logonomic::cli_addon::Probe_inspect_request& request);
void handle_probes(logonomic::cli_addon::Invocation_context& context);

} // namespace grok
