#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/helper_ingress.h>

#include <optional>
#include <string>

namespace grok {

std::optional<std::string> interaction_request_identity(
    const logonomic::cli_addon::Json_value& request_id);
std::optional<logonomic::cli_addon::Classify_event_result> classify_hook(
    const logonomic::cli_addon::Classify_event_request& request,
    logonomic::cli_addon::Contract_failure* failure = nullptr);
void handle_lifecycle(logonomic::cli_addon::Invocation_context& context);

} // namespace grok
