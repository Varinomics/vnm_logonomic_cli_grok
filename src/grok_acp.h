#pragma once
#include <logonomic_cli_addon/session_contracts.h>
namespace grok {
std::string acp_initialize();
std::string acp_initial_state(const logonomic::cli_addon::Delegated_prepare_request& request);
logonomic::cli_addon::Structured_inspect_result inspect_acp(const logonomic::cli_addon::Structured_inspection& inspection);
} // namespace grok
