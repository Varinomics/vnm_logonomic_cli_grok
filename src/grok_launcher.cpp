#include "grok_launcher.h"
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <algorithm>
#include <cctype>
namespace grok {
namespace addon = logonomic::cli_addon;
namespace {

using addon::lower;
using addon::set_environment;
using addon::resolve_program;
using addon::native_image;
using addon::path_to_utf8;
using addon::base_name;

} // namespace

std::optional<addon::Probe_output> run_probe(const std::vector<std::string>& argv,
    const Environment& environment, std::string_view working_directory, int timeout_ms,
    addon::Invocation_context& context)
{
    return addon::probe(argv, environment, working_directory, timeout_ms, context);
}

std::vector<std::string> literal_launcher_argv(std::string_view script)
{
    std::vector<std::string> result;
    std::string word;
    char quote = 0;
    bool started = false;
    for (char ch : script) {
        if (ch == '\0' || ch == '\r' || ch == '\n') return {};
        if (quote != '\'' && std::string_view("$`;&|<>").find(ch) != std::string_view::npos) return {};
        if (ch == '\'' || ch == '"') {
            if (quote == ch) quote = 0;
            else if (quote == 0) quote = ch;
            else word += ch;
            started = true;
        }
        else if (!quote && std::isspace(static_cast<unsigned char>(ch))) {
            if (started) { result.push_back(std::move(word)); word.clear(); started = false; }
        }
        else { word += ch; started = true; }
    }
    if (quote) return {};
    if (started) result.push_back(std::move(word));
    return result;
}

std::optional<Launcher_command> recognize_launcher(std::string_view script)
{
    auto argv = literal_launcher_argv(script);
    Launcher_command result;
    std::size_t index = 0;
    while (index < argv.size()) {
        const auto equal = argv[index].find('=');
        if (equal == std::string::npos) break;
        const auto name = argv[index].substr(0, equal);
        if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_') ||
            !std::all_of(name.begin(), name.end(), [](unsigned char ch) { return std::isalnum(ch) || ch == '_'; })) return std::nullopt;
        result.environment_names.push_back(name);
        ++index;
    }
    if (index == argv.size() || base_name(argv[index]) != "grok") return std::nullopt;
    result.program = argv[index++];
    result.prefix_arguments.assign(argv.begin() + index, argv.end());
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
