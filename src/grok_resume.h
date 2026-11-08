#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <filesystem>
#include <string_view>

namespace grok {

// Missing local evidence stays unknown because native resume can resolve
// other stores or a restored child. This reader never edits native history.
logonomic::cli_addon::Resume_inspect_result inspect_resume_history(
    const std::filesystem::path& profile,
    std::string_view session_id,
    bool inspect_content,
    logonomic::cli_addon::Invocation_context& context);

} // namespace grok
