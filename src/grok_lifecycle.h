#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/helper_ingress.h>

#include <optional>

namespace grok {

std::optional<logonomic::cli_addon::Classify_event_result> classify_hook(
    const logonomic::cli_addon::Classify_event_request& request);
void handle_lifecycle(logonomic::cli_addon::Invocation_context& context);

} // namespace grok
