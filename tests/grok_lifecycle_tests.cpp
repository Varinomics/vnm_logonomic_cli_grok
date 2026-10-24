#include "grok_lifecycle.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sidecar.h>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

namespace {

namespace addon = logonomic::cli_addon;

bool check(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

std::optional<addon::Lifecycle_declaration> package_lifecycle_declaration(const std::filesystem::path& package)
{
    const addon::Directory_package_source source(package);
    const auto path = addon::sidecar_relative_path(
        addon::k_lifecycle_events_capability_id, addon::k_lifecycle_events_version);
    const auto bytes = source.read(path, addon::limits::k_max_sidecar_bytes, nullptr);
    const auto sidecar = bytes ? addon::parse_sidecar(*bytes, nullptr) : std::nullopt;
    return sidecar ? addon::decode_lifecycle_declaration(sidecar->payload, nullptr) : std::nullopt;
}

addon::Classify_event_request hook(const std::string& name)
{
    addon::Classify_event_request request;
    request.event.name = name;
    request.event.body.set("hook_event_name", addon::Json_value(name));
    request.event.body.set("sessionId", addon::Json_value("8e18215e-f612-4131-95dd-7890139a41ae"));
    request.event.body.set("promptId", addon::Json_value("prompt-1"));
    return request;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: grok_lifecycle_tests <package directory>\n";
        return 2;
    }
    // Oracle: the host's feature gate. It rejects an observation carrying a
    // fact the package's own declaration leaves unsupported, phase and all.
    const auto declaration = package_lifecycle_declaration(addon::path_from_utf8(argv[1]));
    if (!check(declaration.has_value(), "package lifecycle declaration is unreadable")) {
        return 1;
    }
    bool ok = true;
    for (const std::string name : {"SessionStart", "UserPromptSubmit", "StopFailure", "StopCancelled", "SessionEnd"}) {
        auto request = hook(name);
        if (name == "SessionStart") {
            request.event.body.set("source", addon::Json_value("resume"));
        }
        const auto result = grok::classify_hook(request);
        if (!check(result && result->observations.size() == 1, name + " left no observation")) {
            ok = false;
            continue;
        }
        ok &= check(addon::lifecycle_profile_admits(declaration->features, result->observations.front()),
            "the declaration refuses what " + name + " reports");
    }
    return ok ? 0 : 1;
}
