#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/usage_scan.h>

#include <filesystem>
#include <functional>

namespace grok {

// One GiB is the adopted host candidate bound. Eight GiB bounds retained
// generations, candidate copies, merge inputs/output, and source replacements.
// Limits count the whole owned tree; history is never evicted to
// make space. The runtime always uses these defaults; smaller limits let pure
// filesystem fixtures exercise the same production admission checks.
struct Usage_scan_limits
{
    std::uint64_t disk_bytes = 8ull * 1024 * 1024 * 1024;
    std::uint64_t filesystem_entries = 1024 * 1024;
    std::uint64_t persisted_lines = 8ull * 1024 * 1024;
    std::uint64_t candidate_bytes = 1024ull * 1024 * 1024;
    std::uint64_t candidate_records = 1024 * 1024;
};

std::optional<logonomic::cli_addon::Usage_scan_page> scan_usage(
    const logonomic::cli_addon::Usage_scan_request& request,
    const std::filesystem::path& vendor_profile,
    const std::filesystem::path& storage,
    const std::function<bool()>& cancelled,
    std::string* error, const Usage_scan_limits& limits = {});
void handle_usage(logonomic::cli_addon::Invocation_context& context);

} // namespace grok
