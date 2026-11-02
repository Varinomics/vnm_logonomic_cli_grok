#pragma once
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/launcher_discovery.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace grok {
using Environment = std::vector<std::pair<std::string, std::string>>;
struct Launcher_command
{
    std::string program;
    std::vector<std::string> prefix_arguments;
    std::vector<std::string> environment_names;
};
std::optional<logonomic::cli_addon::Probe_output> run_probe(const std::vector<std::string>& argv,
    const Environment& environment, std::string_view working_directory, int timeout_ms,
    logonomic::cli_addon::Invocation_context& context);
std::optional<Launcher_command> recognize_launcher(
    const logonomic::cli_addon::Launcher_request& launcher, std::string_view platform_id,
    std::optional<std::span<const std::string>> host_argv = std::nullopt);
std::optional<Environment> launcher_environment(const Launcher_command& command,
    const logonomic::cli_addon::Json_value& effective_environment);
std::optional<logonomic::cli_addon::Launcher_candidate> discover_launcher(
    const logonomic::cli_addon::Discover_request& request);
std::optional<std::vector<std::string>> resolve_probe_launcher(
    const Launcher_command& command, const std::vector<std::string>& operation_arguments,
    const logonomic::cli_addon::Json_value& environment, std::string_view working_directory);
} // namespace grok
