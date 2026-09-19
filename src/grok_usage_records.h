#pragma once

#include <logonomic_cli_addon/usage_scan.h>

#include <filesystem>
#include <functional>
#include <istream>
#include <optional>
#include <string>
#include <string_view>

namespace grok {

struct Session_usage_record
{
    logonomic::cli_addon::Usage_record usage;
    std::string recorded_cwd;
};

std::optional<std::string> repository_for_cwd(std::string_view cwd);
void refresh_repository(Session_usage_record* record);
bool read_session_usage(std::istream& input, const std::filesystem::path& path,
    const std::function<bool(const Session_usage_record&)>& emit,
    const std::function<bool()>& cancelled, std::string* error);

} // namespace grok
