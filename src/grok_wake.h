#pragma once

#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/adapter_runtime.h>

namespace grok {

bool wake_compatible_arguments(const std::vector<std::string>& arguments);
void prepare_wake(logonomic::cli_addon::Launch_patch& patch,
    const std::vector<std::string>& arguments, std::string_view native_help);
void handle_wake(logonomic::cli_addon::Invocation_context& context);
inline constexpr const char* k_wake_token_environment = "GROK_LOGONOMIC_WAKE_TOKEN";

} // namespace grok
