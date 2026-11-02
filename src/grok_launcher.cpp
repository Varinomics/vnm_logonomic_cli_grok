#include "grok_launcher.h"
#include "grok_launcher_approval.h"
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <algorithm>
namespace grok {
namespace addon = logonomic::cli_addon;
namespace {

using addon::lower;
using addon::set_environment;
using addon::resolve_program;
using addon::native_image;
using addon::path_to_utf8;

} // namespace

std::optional<addon::Probe_output> run_probe(const std::vector<std::string>& argv,
    const Environment& environment, std::string_view working_directory, int timeout_ms,
    addon::Invocation_context& context)
{
    return addon::probe(argv, environment, working_directory, timeout_ms, context);
}

std::optional<Launcher_command> recognize_launcher(
    const addon::Launcher_request& launcher, std::string_view platform_id,
    std::optional<std::span<const std::string>> host_argv)
{
    const auto invocation = addon::literal_launcher_invocation(launcher, platform_id, host_argv);
    if (!invocation) {
        return std::nullopt;
    }
    const bool windows = platform_id.starts_with("windows-");
    const auto& program = invocation->argv.front();
    const auto separator = program.find_last_of(windows ? "/\\" : "/");
    auto name = program.substr(separator == std::string::npos ? 0 : separator + 1);
    if (windows) {
        name = lower(std::move(name));
    }
    const auto& names = launcher_approval_rules().program_names;
    if (std::find(names.begin(), names.end(), name) == names.end()) {
        return std::nullopt;
    }
    Launcher_command result;
    result.program = program;
    result.prefix_arguments.assign(invocation->argv.begin() + 1, invocation->argv.end());
    result.environment_names = invocation->environment_names;
    return result;
}

std::optional<Environment> launcher_environment(const Launcher_command& command,
    const addon::Json_value& effective_environment)
{
    if (!effective_environment.is_object()) return std::nullopt;
    Environment result;
    for (const auto& name : command.environment_names) {
        const addon::Json_value* value = nullptr;
        for (const auto& member : effective_environment.as_object()) {
#ifdef _WIN32
            const bool matches = lower(member.name) == lower(name);
#else
            const bool matches = member.name == name;
#endif
            if (!matches) continue;
            if (value || !member.value.is_string()) return std::nullopt;
            value = &member.value;
        }
        // The host owns runner-specific assignment expansion. Missing evidence
        // cannot be reconstructed from a quote-stripped launcher token.
        if (!value) return std::nullopt;
        set_environment(result, name, value->as_string());
    }
    return result;
}

std::optional<std::vector<std::string>> resolve_probe_launcher(
    const Launcher_command& command, const std::vector<std::string>& operation_arguments,
    const addon::Json_value& effective_environment, std::string_view working_directory)
{
    Environment environment;
    if (!effective_environment.is_object()) return std::nullopt;
    for (const auto& member : effective_environment.as_object()) {
        if (!member.value.is_string()) return std::nullopt;
        environment.emplace_back(member.name, member.value.as_string());
    }
    if (!launcher_environment(command, effective_environment)) return std::nullopt;
    const auto program = resolve_program(command.program, environment, working_directory);
    if (!program || !native_image(*program)) return std::nullopt;
    std::vector<std::string> result{path_to_utf8(*program)};
    result.insert(result.end(), operation_arguments.begin(), operation_arguments.end());
    return result;
}

std::optional<addon::Launcher_candidate> discover_launcher(const addon::Discover_request& request)
{
    Launcher_command command{"grok", {}, {}};
    if (!resolve_probe_launcher(command, {}, request.ambient_environment, {})) return std::nullopt;
    return addon::Launcher_candidate{
        "interactive", "Grok Build", "grok",
        request.platform_id == "windows-x86_64" ? addon::k_runner_powershell : addon::k_runner_direct,
        "", true, addon::Json_value::object(),
    };
}
} // namespace grok
