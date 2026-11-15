#include "grok_lifecycle.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/lifecycle_correlation_lifetime.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/provider_event_diagnostic.h>
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

bool classification_diagnostics(const std::filesystem::path& package)
{
    const auto root = std::filesystem::temp_directory_path() / ("grok-event-diagnostics-" + addon::random_nonce_hex());
    addon::Simulator_options options;
    options.package_directory = package;
    options.profile_storage_root = root;
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    if (!check(host.start(&failure), "Packaged Grok diagnostic adapter did not start")) {
        return false;
    }
    bool ok = true;
    const auto rejected = [
            &host,
            &ok
        ](
            const char* event_name,
            const char* body,
            const char* field,
            addon::Provider_event_problem problem)
        {
            addon::Attention_classification_request request;
            request.base.event = {event_name, *addon::parse_json(body, {}, nullptr)};
            request.base.transition_nonce = std::string(32, 'a');
            const auto id = host.invoke({addon::k_attention_kind_capability_id, addon::k_attention_kind_version},
                addon::k_classify_event_operation, addon::encode_attention_classification_request(request), 5000);
            const auto outcome = host.wait_outcome(id, 6000);
            if (!check(outcome && outcome->status == addon::Host_status::FAILED &&
                outcome->reason_code == "invalid_provider_event", "Malformed Grok event did not fail explicitly"))
            {
                ok = false;
                return;
            }
            const auto diagnostic = addon::decode_provider_event_diagnostic(outcome->message);
            ok &= check(diagnostic && diagnostic->event_name == event_name && diagnostic->field == field &&
                    diagnostic->problem == problem && !diagnostic->rule.empty(),
                "Packaged Grok failure lost its exact event, malformed field or validation rule");
            ok &= check(outcome->message.find("private_value_sentinel") == std::string::npos,
                "Grok diagnostic included a native field value");
        };
    rejected("SessionStart", R"({"hook_event_name":"SessionStart"})",
        "sessionId", addon::Provider_event_problem::MISSING);
    rejected("StopFailure", R"({"hook_event_name":"StopFailure","sessionId":17})",
        "sessionId", addon::Provider_event_problem::WRONG_TYPE);
    rejected("UserPromptSubmit", R"({"hook_event_name":"UserPromptSubmit","sessionId":"private_value_sentinel\nsecond line"})",
        "sessionId", addon::Provider_event_problem::CONTROL_CHARACTER);
    rejected("pending_interaction", R"({"sessionId":"session-a","request_id":true,
        "update":{"tool_call_id":"call-a","sessionUpdate":"pending_interaction","kind":"permission"}})",
        "request_id", addon::Provider_event_problem::WRONG_TYPE);
    rejected("pending_interaction", R"({"sessionId":"session-a","request_id":1,
        "update":{"tool_call_id":"call-a","sessionUpdate":"pending_interaction"}})",
        "update.kind", addon::Provider_event_problem::MISSING);
    addon::Classify_event_request valid;
    valid.event.name = "SessionStart";
    valid.event.body = *addon::parse_json(R"({"hook_event_name":"SessionStart","sessionId":"session-a"})", {}, nullptr);
    valid.transition_nonce = std::string(32, 'a');
    const auto reserve_id = host.invoke({"lifecycle-correlation-lifetime", 1}, "reserve",
        addon::encode_lifecycle_correlation_reserve_request({{valid.transition_nonce}}), 5000);
    const auto reserved = host.wait_outcome(reserve_id, 6000);
    ok &= check(reserved && reserved->status == addon::Host_status::SUCCESS,
        "The host must reserve a lifecycle correlation before root classification");
    const auto valid_id = host.invoke({"helper-ingress", 2}, addon::k_classify_event_operation,
        addon::encode_classify_event_request(valid), 5000);
    const auto accepted = host.wait_outcome(valid_id, 6000);
    ok &= check(accepted && accepted->status == addon::Host_status::SUCCESS,
        "Optional Grok source metadata became a required field");
    ok &= check(host.shutdown(1000, nullptr), "Packaged Grok diagnostic adapter did not exit cleanly");
    std::filesystem::remove_all(root);
    return ok;
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

bool execution_context_contract()
{
    bool ok = true;
#ifdef _WIN32
    const std::string cwd = "C:\\work\\repo\\nested folder";
#else
    const std::string cwd = "/work/repo/nested folder";
#endif
    auto request = hook("SessionStart");
    request.event.body.set("source", addon::Json_value("load"));
    request.event.body.set("cwd", addon::Json_value(cwd));
    request.event.body.set("workspaceRoot", addon::Json_value("not-the-default-directory"));
    request.execution_context_version = 1;
    request.transition_nonce = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    request.ledger.current_epoch = 3;
    request.ledger.current_transition = addon::Ledger_transition{
        addon::Transition_kind::RESUME, "8e18215e-f612-4131-95dd-7890139a41ae",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    const auto resumed = grok::classify_hook(request);
    if (!check(resumed && resumed->execution_updates.size() == 1, "native root cwd observation is missing")) {
        return false;
    }
    const auto& update = resumed->execution_updates.front();
    ok &= check(update.cwd == cwd && update.sequence == 0 && update.transition_nonce == request.transition_nonce &&
            update.provider_session_id == "8e18215e-f612-4131-95dd-7890139a41ae" &&
            update.status == addon::Execution_context_status::CONFIRMED && update.environment == "local",
        "native cwd lost spelling, default-directory meaning or new activation attribution");
    const auto roundtrip = addon::decode_classify_event_result(addon::encode_classify_event_result(*resumed), nullptr);
    ok &= check(roundtrip && roundtrip->execution_updates == resumed->execution_updates &&
            roundtrip->transition == addon::Transition_kind::RESUME,
        "optional execution context did not survive beside native resume classification");

    request.event.body.set("source", addon::Json_value("new"));
    const auto created = grok::classify_hook(request);
    ok &= check(created && created->transition == addon::Transition_kind::START && created->execution_updates.size() == 1,
        "native new identity did not preserve independent execution evidence");
    request.execution_context_version = 0;
    const auto unrequested = grok::classify_hook(request);
    ok &= check(unrequested && unrequested->execution_updates.empty() && unrequested->observations.size() == 1,
        "unnegotiated execution context changed lifecycle support");
    request.execution_context_version = 1;
    for (const auto& invalid : {std::string("relative/folder"), std::string("ssh://host/work"),
             cwd + '\n', cwd + std::string(1, '\0'), cwd + std::string(4096, 'x')})
    {
        request.event.body.set("cwd", addon::Json_value(invalid));
        const auto result = grok::classify_hook(request);
        ok &= check(result && result->execution_updates.empty() && result->observations.size() == 1 &&
                result->classification == addon::Classification::TRANSITION,
            "invalid optional native cwd rejected its valid lifecycle transition");
    }
    request.event.body.set("cwd", addon::Json_value(cwd));
    request.execution_failure = addon::Contract_failure{"unsupported_version", "unsupported execution companion"};
    const auto invalid = grok::classify_hook(request);
    ok &= check(invalid && invalid->execution_failure && invalid->execution_updates.empty() &&
            invalid->observations.size() == 1,
        "malformed optional execution request discarded native lifecycle evidence");
    request.execution_failure.reset();
    request.event.body.set("subagentType", addon::Json_value("researcher"));
    const auto child = grok::classify_hook(request);
    ok &= check(child && child->execution_updates.empty() && child->observations.empty(),
        "child observation acquired root native execution state");
    for (const std::string name : {"UserPromptSubmit", "PostToolUse", "PostCompact", "SessionEnd"}) {
        auto later = hook(name);
        later.execution_context_version = 1;
        later.transition_nonce = request.transition_nonce;
        later.event.body.set("cwd", addon::Json_value(cwd));
        const auto result = grok::classify_hook(later);
        ok &= check(result && result->execution_updates.empty() && result->classification != addon::Classification::TRANSITION,
            "later hook without source activation attribution invented cwd or identity");
    }
    return ok;
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
    bool ok = classification_diagnostics(addon::path_from_utf8(argv[1])) && execution_context_contract();
    for (const std::string name : {"SessionStart", "UserPromptSubmit", "StopFailure", "StopCancelled", "SessionEnd"}) {
        auto request = hook(name);
        if (name == "SessionStart") {
            // Grok's actor registration emits "load" for restored history.
            request.event.body.set("source", addon::Json_value("load"));
        }
        const auto result = grok::classify_hook(request);
        if (!check(result && result->observations.size() == 1, name + " left no observation")) {
            ok = false;
            continue;
        }
        ok &= check(addon::lifecycle_profile_admits(declaration->features, result->observations.front()),
            "the declaration refuses what " + name + " reports");
        if (name == "SessionStart") {
            ok &= check(result->transition == addon::Transition_kind::RESUME,
                "native SessionStart source=load must identify a resume transition");
        }
    }
    return ok ? 0 : 1;
}
