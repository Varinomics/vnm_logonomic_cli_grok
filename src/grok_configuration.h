#pragma once
#include <logonomic_cli_addon/adapter_runtime.h>
#include <string>
namespace grok {
std::string hook_configuration();
void transform_configuration(logonomic::cli_addon::Invocation_context& context);
} // namespace grok
