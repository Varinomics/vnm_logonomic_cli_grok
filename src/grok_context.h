#pragma once

#include <logonomic_cli_addon/helper_ingress.h>

#include <string>

namespace grok {

void add_hook_context(
    const logonomic::cli_addon::Classify_event_request& request,
    const std::string& provider_session_id,
    logonomic::cli_addon::Classify_event_result& result);

} // namespace grok
