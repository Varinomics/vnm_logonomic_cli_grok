#include "grok_acp.h"
#include "grok_json.h"
#include <iostream>
#include <stdexcept>
namespace json_util = logonomic::cli_addon::json;

namespace addon = logonomic::cli_addon;
using namespace grok;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string reply(int id, Json result)
{
    return addon::serialize_json(json_util::object({{"jsonrpc", Json("2.0")}, {"id", Json(id)}, {"result", std::move(result)}})) + '\n';
}
// Oracle: grok 1.0.41 answers `_x.ai/mcp/list` with its catalog nested one
// level down, as recorded by the scripted delegation gate
// (`{"result":{"servers":[...],"sessionMcpResolved":false}}`).
Json catalog(Json servers)
{
    return json_util::object({{"result", json_util::object({{"servers", std::move(servers)}, {"sessionMcpResolved", Json(false)}})}});
}
namespace {
std::string notification(const char* method, const std::string& session, Json update)
{
    return addon::serialize_json(json_util::object({{"jsonrpc", Json("2.0")}, {"method", Json(method)},
        {"params", json_util::object({{"sessionId", Json(session)}, {"update", std::move(update)}})}})) + '\n';
}
std::string child_spawn(const std::string& parent, const std::string& child, const std::string& attempt)
{
    return notification("_x.ai/session_notification", parent, json_util::object({
        {"sessionUpdate", Json("subagent_spawned")}, {"subagent_id", Json(child)}, {"attempt_id", Json(attempt)},
        {"parent_session_id", Json(parent)}, {"child_session_id", Json(child)},
        {"subagent_type", Json("general-purpose")}, {"description", Json("Inspect the requested change.")}}));
}
std::string child_finished(const std::string& parent, const std::string& child, const std::string& attempt)
{
    return notification("_x.ai/session_notification", parent, json_util::object({
        {"sessionUpdate", Json("subagent_finished")}, {"subagent_id", Json(child)}, {"attempt_id", Json(attempt)},
        {"child_session_id", Json(child)}, {"status", Json("completed")}, {"tool_calls", Json(1)},
        {"turns", Json(1)}, {"duration_ms", Json(20)}, {"tokens_used", Json(30)}, {"will_wake", Json(false)}}));
}
std::string message_chunk(const std::string& session, const std::string& text)
{
    return notification("session/update", session, json_util::object({{"sessionUpdate", Json("agent_message_chunk")},
        {"content", json_util::object({{"type", Json("text")}, {"text", Json(text)}})}}));
}
std::string child_permission(const std::string& session, const std::string& tool)
{
    auto options = Json::array();
    options.push_back(json_util::object({{"optionId", Json("allow-once")}, {"name", Json("allow once")},
        {"kind", Json("allow_once")}}));
    options.push_back(json_util::object({{"optionId", Json("always-allow")}, {"name", Json("always allow")},
        {"kind", Json("allow_always")}}));
    options.push_back(json_util::object({{"optionId", Json("reject-once")}, {"name", Json("reject once")},
        {"kind", Json("reject_once")}}));
    return addon::serialize_json(json_util::object({{"jsonrpc", Json("2.0")}, {"id", Json("child-permission")},
        {"method", Json("session/request_permission")}, {"params", json_util::object({
            {"sessionId", Json(session)}, {"toolCall", json_util::object({{"toolCallId", Json("child-tool")},
                {"kind", Json("other")}, {"title", Json(tool)}, {"rawInput", json_util::object({
                    {"variant", Json("UseTool")}, {"tool_name", Json(tool)}, {"tool_input", Json::object()}})}})},
            {"options", std::move(options)}})}})) + '\n';
}
void child_routing(const std::string& prompt_state)
{
    // Oracle: xai-org/grok-code 2bdd1d6, extensions/notification.rs declares
    // snake_case SubagentSpawned fields and publishes the parent announcement
    // before child Prompt dispatch. agent/subagent/start_artifact_publication.rs
    // can also send it to the spawner. Attempts reuse the child session ID.
    addon::Structured_inspection inspection;
    inspection.protocol_state = prompt_state;
    inspection.stdout_bytes = child_spawn("root-session", "child-a", "attempt-a1") +
        message_chunk("child-a", "Child analysis must not become the root answer.");
    auto result = inspect_acp(inspection);
    require(!result.complete && !result.failure_reason && !result.stdin_bytes,
        "a child announced by the native parent was rejected");
    const auto child_state = result.protocol_state;
    inspection.protocol_state = child_state;
    inspection.stdout_bytes = child_spawn("root-session", "child-b", "attempt-b1") +
        child_spawn("child-a", "grandchild", "attempt-g1") +
        message_chunk("root-session", "Root ") + message_chunk("child-b", "Sibling text.") +
        message_chunk("grandchild", "Nested text.") +
        child_finished("child-a", "grandchild", "attempt-g1") +
        child_finished("root-session", "child-a", "attempt-a1") +
        child_spawn("root-session", "child-a", "attempt-a2") +
        child_spawn("root-session", "child-a", "attempt-a2") +
        message_chunk("child-a", "Second attempt.") +
        child_finished("root-session", "child-a", "attempt-a2") +
        child_finished("root-session", "child-b", "attempt-b1") + message_chunk("root-session", "answer.") +
        reply(4, json_util::object({{"stopReason", Json("end_turn")}}));
    result = inspect_acp(inspection);
    require(!result.complete && !result.failure_reason && result.stdin_bytes &&
        result.stdin_bytes->find("session/close") != std::string::npos,
        "completed descendant attempts prevented the root from finishing");
    inspection.protocol_state = result.protocol_state;
    inspection.stdout_bytes = reply(5, json_util::object({
        {"_meta", json_util::object({{"x.ai/closeOutcome", Json("closed")}})}}));
    result = inspect_acp(inspection);
    require(result.complete && result.result_bytes == std::optional<std::string>("Root answer."),
        "child output contaminated the root result");

    auto spawner_announcement = json_util::parse(child_spawn("child-a", "grandchild", "attempt-g1")).value();
    auto params = json_util::member(spawner_announcement, "params");
    params.set("sessionId", Json("root-session"));
    spawner_announcement.set("params", std::move(params));
    inspection.protocol_state = child_state;
    inspection.stdout_bytes = addon::serialize_json(spawner_announcement) + '\n' + message_chunk("grandchild", "Routed.");
    result = inspect_acp(inspection);
    require(!result.complete && !result.failure_reason,
        "a native announcement copied to the enrolled spawner lost its actual parent");

    for (const auto& invalid : {
            child_spawn("foreign-parent", "unknown-child", "attempt"),
            child_spawn("child-a", "root-session", "attempt"),
            child_spawn("child-a", "child-a", "attempt"),
            child_spawn("child-a", "child-b", "attempt") + child_spawn("root-session", "child-b", "attempt"),
            message_chunk("unknown-child", "Unannounced.")})
    {
        inspection.protocol_state = child_state;
        inspection.stdout_bytes = invalid;
        require(inspect_acp(inspection).failure_reason.has_value(),
            "unannounced or conflicting child lineage was accepted");
    }
    const std::string task_tool = "logonomic-task-1-control__complete";
    for (const auto& [tool, option] : {
            std::pair<std::string, std::string>{task_tool, "allow-once"},
            {"github__create_issue", "reject-once"}, {"logonomic-task-2-control__complete", "reject-once"}})
    {
        inspection.protocol_state = child_state;
        inspection.stdout_bytes = child_permission("child-a", tool);
        result = inspect_acp(inspection);
        require(!result.complete && !result.failure_reason && result.stdin_bytes,
            "an enrolled child's permission request was not answered");
        const auto answer = json_util::parse(*result.stdin_bytes);
        require(answer && json_util::member(*answer, "id") == Json("child-permission") &&
            json_util::text(json_util::member(json_util::member(*answer, "result"), "outcome"), "optionId") == option,
            "child permissions changed the launcher's one-time task tool policy or request identity");
        if (option == "reject-once") {
            inspection.protocol_state = result.protocol_state;
            inspection.stdout_bytes = reply(4, json_util::object({{"stopReason", Json("cancelled")},
                {"_meta", json_util::object({{"cancellationCategory", Json("PermissionRejected")}})}}));
            require(inspect_acp(inspection).failure_reason == std::optional<std::string>("acp_completion_failed"),
                "a child's denial was attributed to the root turn");
        }
    }
    inspection.protocol_state = child_state;
    inspection.stdout_bytes = child_permission("unknown-child", task_tool);
    require(inspect_acp(inspection).failure_reason.has_value(), "an unannounced child gained permission routing");
}
void utf8_read_boundaries(const std::string& initial)
{
    // JSON records are UTF-8, but native stdout reads can stop at any byte.
    const auto record = reply(1, json_util::object({{"protocolVersion", Json(1)},
        {"agentInfo", json_util::object({{"name", Json("Grok café 日本 🌍")}})}}));
    addon::Structured_inspection inspection;
    inspection.protocol_state = initial;
    inspection.stdout_bytes = record;
    const auto whole = inspect_acp(inspection);
    require(!whole.complete && !whole.failure_reason, "complete UTF-8 reply failed");
    for (std::size_t split = 1; split < record.size(); ++split) {
        inspection.protocol_state = initial;
        inspection.stdout_bytes = record.substr(0, split);
        const auto partial = inspect_acp(inspection);
        if (partial.complete || partial.failure_reason || partial.stdin_bytes) {
            throw std::runtime_error("partial record at byte " + std::to_string(split) +
                " advanced or failed the exchange: " + partial.failure_reason.value_or("unexpected progress"));
        }
        require(addon::exchange_protocol_state_valid(partial.protocol_state),
            "a split UTF-8 read produced an invalid host continuation");
        inspection.protocol_state = partial.protocol_state;
        inspection.stdout_bytes = record.substr(split);
        const auto resumed = inspect_acp(inspection);
        require(resumed.complete == whole.complete && resumed.failure_reason == whole.failure_reason &&
            resumed.stdin_bytes == whole.stdin_bytes && resumed.protocol_state == whole.protocol_state,
            "split UTF-8 record changed the exchange outcome");
    }
    inspection.protocol_state = initial;
    for (const char byte : record) {
        inspection.stdout_bytes.assign(1, byte);
        const auto result = inspect_acp(inspection);
        require(!result.complete && !result.failure_reason, "byte-at-a-time UTF-8 reads failed");
        inspection.protocol_state = result.protocol_state;
    }
    require(inspection.protocol_state == whole.protocol_state, "byte-at-a-time reads changed the exchange state");

    inspection.protocol_state = initial;
    inspection.stdout_bytes = std::string(1024 * 1024, 'x');
    const auto bounded = inspect_acp(inspection);
    require(!bounded.complete && !bounded.failure_reason, "pending record byte limit was reduced");
    inspection.protocol_state = bounded.protocol_state;
    inspection.stdout_bytes.clear();
    require(!inspect_acp(inspection).failure_reason, "maximum pending record failed to round-trip");
    inspection.stdout_bytes = "x";
    require(inspect_acp(inspection).failure_reason == std::optional<std::string>("acp_record_limit"),
        "pending record byte limit was not enforced");
    auto state = json_util::parse(initial);
    state->set("pending_base64", Json("invalid!"));
    inspection.protocol_state = addon::serialize_json(*state);
    inspection.stdout_bytes.clear();
    require(inspect_acp(inspection).failure_reason == std::optional<std::string>("malformed_acp_state"),
        "malformed pending bytes were accepted");
}

int require_catalog_refresh(const addon::Structured_inspect_result& result)
{
    require(!result.complete && !result.failure_reason && result.stdin_bytes.has_value(),
        "invalidated catalog did not request a refresh");
    const auto request = json_util::parse(*result.stdin_bytes);
    require(request && json_util::text(*request, "method") == "_x.ai/mcp/list",
        "invalidated catalog released the prompt");
    require(json_util::text(json_util::member(*request, "params"), "sessionId") == "root-session" &&
        json_util::member(json_util::member(*request, "params"), "cache") == Json(true),
        "refresh changed the native session query semantics");
    return (int)json_util::member(*request, "id").as_integer();
}
void require_prompt(const addon::Structured_inspect_result& result, const std::string& expected)
{
    require(!result.complete && !result.failure_reason && result.stdin_bytes.has_value(),
        "clean catalog did not release the retained prompt");
    const auto request = json_util::parse(*result.stdin_bytes);
    require(request && json_util::text(*request, "method") == "session/prompt",
        "clean catalog did not issue the native prompt request");
    const auto& prompt = json_util::member(json_util::member(*request, "params"), "prompt");
    require(prompt.is_array() && prompt.as_array().size() == 1 &&
        json_util::text(prompt.as_array().front(), "text") == expected,
        "catalog refresh changed the original prompt");
}
void catalog_ordering(const std::string& catalog_state, const Json& servers, const std::string& prompt)
{
    // The native MCP catalog is invalidated by each of these notifications.
    // A prompt requires a clean session snapshot with its required actions.
    const auto ready = catalog(servers);
    for (const auto& rejection : {
            json_util::object({{"enabled", Json(false)}}),
            json_util::object({{"status", Json("connecting")}}),
            json_util::object({{"authRequired", Json(true)}}),
            json_util::object({{"setupRequired", Json(true)}}),
            json_util::object({{"tools", Json::array()}})})
    {
        auto server = servers.as_array().front();
        auto session = json_util::member(server, "session");
        for (const auto& field : rejection.as_object()) {
            session.set(field.name, field.value);
        }
        server.set("session", std::move(session));
        auto unavailable = Json::array();
        unavailable.push_back(std::move(server));
        addon::Structured_inspection inspection;
        inspection.protocol_state = catalog_state;
        inspection.stdout_bytes = reply(3, catalog(std::move(unavailable)));
        const auto waiting = inspect_acp(inspection);
        require(!waiting.complete && !waiting.failure_reason && !waiting.stdin_bytes,
            "catalog admission ignored native server readiness or required actions");
    }
    for (const char* method : {"_x.ai/mcp/server_status", "_x.ai/mcp/tools_changed",
            "_x.ai/mcp/servers_updated", "_x.ai/mcp_initialized"})
    {
        const auto notification = addon::serialize_json(json_util::object({
            {"jsonrpc", Json("2.0")}, {"method", Json(method)}, {"params", Json::object()}})) + '\n';
        for (const bool notification_first : {true, false}) {
            addon::Structured_inspection inspection;
            inspection.protocol_state = catalog_state;
            inspection.stdout_bytes = notification_first
                ? notification + reply(3, ready)
                : reply(3, ready) + notification;
            auto refresh = inspect_acp(inspection);
            const auto refresh_id = require_catalog_refresh(refresh);
            require(refresh_id != 3, "refresh reused the invalidated request ID");
            inspection.protocol_state = refresh.protocol_state;
            inspection.stdout_bytes = reply(3, ready);
            require(inspect_acp(inspection).failure_reason.has_value(),
                "duplicate old catalog was accepted as a fresh snapshot");
            inspection.stdout_bytes = reply(refresh_id, ready);
            require_prompt(inspect_acp(inspection), prompt);
        }
        addon::Structured_inspection inspection;
        inspection.protocol_state = catalog_state;
        inspection.stdout_bytes = notification;
        const auto dirty = inspect_acp(inspection);
        require(!dirty.complete && !dirty.failure_reason && !dirty.stdin_bytes,
            "notification replaced the outstanding catalog query");
        inspection.protocol_state = dirty.protocol_state;
        inspection.stdout_bytes = reply(3, ready);
        auto refresh = inspect_acp(inspection);
        auto refresh_id = require_catalog_refresh(refresh);
        inspection.protocol_state = refresh.protocol_state;
        inspection.stdout_bytes = reply(refresh_id, ready) + notification;
        refresh = inspect_acp(inspection);
        const auto next_id = require_catalog_refresh(refresh);
        require(next_id != refresh_id, "consecutive refreshes reused a request ID");
        inspection.protocol_state = refresh.protocol_state;
        inspection.stdout_bytes = reply(next_id, ready);
        require_prompt(inspect_acp(inspection), prompt);

        // A trailing notification split at any byte must have the same effect
        // as that complete notification following the catalog in one read.
        for (std::size_t split = 1; split < notification.size(); ++split) {
            inspection.protocol_state = catalog_state;
            inspection.stdout_bytes = reply(3, ready) + notification.substr(0, split);
            const auto partial = inspect_acp(inspection);
            require(!partial.complete && !partial.failure_reason && !partial.stdin_bytes,
                "prompt escaped before a trailing frame was complete");
            inspection.protocol_state = partial.protocol_state;
            inspection.stdout_bytes = notification.substr(split);
            require_catalog_refresh(inspect_acp(inspection));
        }
        inspection.protocol_state = catalog_state;
        inspection.stdout_bytes = reply(3, catalog(Json::array()));
        const auto waiting = inspect_acp(inspection);
        require(!waiting.complete && !waiting.failure_reason && !waiting.stdin_bytes,
            "incomplete startup catalog released the prompt");
        inspection.protocol_state = waiting.protocol_state;
        inspection.stdout_bytes = notification + reply(3, ready);
        require(inspect_acp(inspection).failure_reason.has_value(),
            "notification admitted a reply before the refresh request was published");
        inspection.stdout_bytes = notification;
        refresh = inspect_acp(inspection);
        refresh_id = require_catalog_refresh(refresh);
        inspection.protocol_state = refresh.protocol_state;
        inspection.stdout_bytes = reply(refresh_id, ready);
        require_prompt(inspect_acp(inspection), prompt);
    }
    addon::Structured_inspection inspection;
    inspection.protocol_state = catalog_state;
    inspection.stdout_bytes = reply(3, ready) + reply(3, ready);
    require(inspect_acp(inspection).failure_reason.has_value(), "duplicate catalog reply was accepted");
    inspection.stdout_bytes = reply(3, ready) + reply(4, json_util::object({{"stopReason", Json("end_turn")}}));
    require(inspect_acp(inspection).failure_reason.has_value(),
        "prompt response was accepted before publishing the prompt");
    inspection.stdout_bytes = reply(3, ready).substr(0, 17);
    const auto partial = inspect_acp(inspection);
    require(!partial.complete && !partial.failure_reason && !partial.stdin_bytes,
        "partial catalog response advanced the protocol");
    inspection.protocol_state = partial.protocol_state;
    inspection.stdout_bytes = reply(3, ready).substr(17);
    require_prompt(inspect_acp(inspection), prompt);
    inspection.protocol_state = catalog_state;
    inspection.stdout_bytes = reply(3, ready);
    inspection.final = true;
    inspection.timed_out = true;
    const auto timeout = inspect_acp(inspection);
    require(timeout.failure_reason == std::optional<std::string>("acp_timeout") && !timeout.stdin_bytes,
        "catalog readiness bypassed the host operation deadline");
}
}
int main()
{
    try {
        addon::Delegated_prepare_request request;
        request.prompt = "Make the requested change.";
        request.context.working_directory = "/workspace";
        request.control.task_id = "task-1";
        request.control.connection = addon::Delegated_connection{"/host relay", "/task/.logonomic/task-servers.json"};
        request.control.servers.push_back({"control", "/task-control", {"complete"}, {{"BOUND_CONTEXT", "opaque"}}});
        addon::Structured_inspection inspection;
        inspection.protocol_state = acp_initial_state(request);
        utf8_read_boundaries(inspection.protocol_state);
        inspection.stdout_bytes = reply(1, json_util::object({{"protocolVersion", Json(1)}}));
        auto result = inspect_acp(inspection);
        require(result.stdin_bytes && result.stdin_bytes->find("session/new") != std::string::npos, "new session not sequenced after initialize");
        require(result.stdin_bytes->find("session/prompt") == std::string::npos, "prompt escaped MCP admission");
        const auto native_new = json_util::parse(*result.stdin_bytes);
        require(native_new.has_value(), "session/new must remain valid native JSON");
        const auto& emitted_servers = json_util::member(json_util::member(*native_new, "params"), "mcpServers");
        require(emitted_servers.is_array() && emitted_servers.as_array().size() == 1,
            "forward exactly the required task server");
        const auto& emitted_server = emitted_servers.as_array().front();
        const auto& args = json_util::member(emitted_server, "args").as_array();
        require(json_util::text(emitted_server, "command") == request.control.connection->relay_program &&
            args.size() == 4 && args[0].as_string() == "--descriptor" &&
            args[1].as_string() == request.control.connection->descriptor_path &&
            args[2].as_string() == "--server" && args[3].as_string() == "control",
            "native MCP request preserves exact nonsecret relay arguments");
        const auto& emitted_env = json_util::member(emitted_server, "env");
        require(emitted_env.is_array() && emitted_env.as_array().empty() &&
            result.stdin_bytes->find("opaque") == std::string::npos &&
            result.protocol_state.find("opaque") == std::string::npos,
            "host authority cannot enter ACP requests or continuation");
        inspection.protocol_state = result.protocol_state;
        inspection.stdout_bytes = reply(2, json_util::object({{"sessionId", Json("root-session")}}));
        result = inspect_acp(inspection);
        require(result.stdin_bytes && result.stdin_bytes->find("_x.ai/mcp/list") != std::string::npos, "native MCP readiness not queried");
        const auto catalog_state = result.protocol_state;
        auto servers = Json::array();
        auto tools = Json::array();
        tools.push_back(json_util::object({{"name", Json("complete")}, {"enabled", Json(true)}}));
        servers.push_back(json_util::object({{"name", Json("logonomic-task-1-control")},
            {"session", json_util::object({{"enabled", Json(true)}, {"status", Json("ready")}, {"tools", std::move(tools)}})}}));
        catalog_ordering(catalog_state, servers, request.prompt);
        inspection.protocol_state = catalog_state;
        inspection.stdout_bytes = reply(3, catalog(servers));
        result = inspect_acp(inspection);
        require(result.stdin_bytes && result.stdin_bytes->find("session/prompt") != std::string::npos, "admitted control did not release prompt");
        const auto prompt_state = result.protocol_state;
        child_routing(prompt_state);
        // Oracle: the owner's rule that a delegated run keeps its launcher's
        // posture and the package approves only its own Task tools
        // (2026-09-28), and grok 1.0.41's permission request as recorded by the
        // scripted gate and a probe: a Task tool call arrives as rawInput
        // {variant: UseTool, tool_name: <server>__<action>} with that title and
        // options allow-once, always-allow and reject-once; after reject-once
        // Grok ends the prompt with stopReason cancelled and cancellation
        // category PermissionRejected.
        const auto permission_request = [](const std::string& tool, const std::string& title, bool allow_once) {
            auto options = Json::array();
            options.push_back(json_util::object({{"optionId", Json("always-allow")}, {"name", Json("always allow")}, {"kind", Json("allow_always")}}));
            if (allow_once) {
                options.push_back(json_util::object({{"optionId", Json("allow-once")}, {"name", Json("allow once")}, {"kind", Json("allow_once")}}));
            }
            options.push_back(json_util::object({{"optionId", Json("reject-once")}, {"name", Json("reject once")}, {"kind", Json("reject_once")}}));
            auto input = json_util::object({{"variant", Json("UseTool")}, {"tool_name", Json(tool)}, {"tool_input", Json::object()}});
            auto call = json_util::object({{"toolCallId", Json("call_7d5edeb1")}, {"kind", Json("other")}, {"title", Json(title)},
                {"rawInput", std::move(input)}});
            return addon::serialize_json(json_util::object({{"jsonrpc", Json("2.0")}, {"id", Json(0)},
                {"method", Json("session/request_permission")},
                {"params", json_util::object({{"sessionId", Json("root-session")}, {"toolCall", std::move(call)},
                    {"options", std::move(options)}})}})) + '\n';
        };
        const auto chosen = [](const addon::Structured_inspect_result& answered) {
            require(!answered.complete && !answered.failure_reason && answered.stdin_bytes.has_value(),
                "a permission request was not answered");
            const auto reply = json_util::parse(*answered.stdin_bytes);
            require(reply && json_util::member(*reply, "id") == Json(0) &&
                json_util::text(json_util::member(json_util::member(*reply, "result"), "outcome"), "outcome") == "selected",
                "the answer names the request and selects one of its options");
            return json_util::text(json_util::member(json_util::member(*reply, "result"), "outcome"), "optionId");
        };
        const std::string task_tool = "logonomic-task-1-control__complete";
        inspection.protocol_state = prompt_state;
        inspection.stdout_bytes = permission_request(task_tool, task_tool, true);
        const auto approved = inspect_acp(inspection);
        require(chosen(approved) == "allow-once", "the task's own Task tool is approved once, never always");
        for (const auto& [tool, title] : {std::pair<std::string, std::string>{"github__create_issue", "github__create_issue"},
                {"logonomic-task-1-control__other", "logonomic-task-1-control__other"},
                {"logonomic-task-2-control__complete", "logonomic-task-2-control__complete"},
                {task_tool, "run_terminal_command"}})
        {
            inspection.protocol_state = prompt_state;
            inspection.stdout_bytes = permission_request(tool, title, true);
            const auto answered = inspect_acp(inspection);
            require(chosen(answered) == "reject-once", "anything but the task's own Task tools is left to the launcher");
            inspection.protocol_state = answered.protocol_state;
            inspection.stdout_bytes = reply(4, json_util::object({{"stopReason", Json("cancelled")},
                {"_meta", json_util::object({{"cancellationCategory", Json("PermissionRejected")}})}}));
            require(inspect_acp(inspection).failure_reason == std::optional<std::string>("tool_permission_denied"),
                "a turn Grok ended at a rejected tool fails as that denial");
            inspection.stdout_bytes = reply(4, json_util::object({{"stopReason", Json("cancelled")},
                {"_meta", json_util::object({{"cancellationCategory", Json("UserCancelled")}})}}));
            require(inspect_acp(inspection).failure_reason == std::optional<std::string>("acp_completion_failed"),
                "a cancellation for another cause after a rejection is not a permission denial");
        }
        inspection.protocol_state = prompt_state;
        inspection.stdout_bytes = reply(4, json_util::object({{"stopReason", Json("cancelled")},
            {"_meta", json_util::object({{"cancellationCategory", Json("PermissionRejected")}})}}));
        require(inspect_acp(inspection).failure_reason == std::optional<std::string>("acp_completion_failed"),
            "a cancellation the package did not cause is not a permission denial");
        inspection.protocol_state = prompt_state;
        inspection.stdout_bytes = permission_request(task_tool, task_tool, false);
        require(inspect_acp(inspection).failure_reason == std::optional<std::string>("acp_permission_request_unsupported"),
            "a request without a one-time approval is not answered with a lasting one");
        inspection.protocol_state = catalog_state;
        inspection.stdout_bytes = permission_request(task_tool, task_tool, true);
        require(inspect_acp(inspection).failure_reason == std::optional<std::string>("acp_permission_request_unsupported"),
            "no permission is answered before the prompt");
        inspection.protocol_state = approved.protocol_state;
        inspection.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"method\":\"session/update\",\"params\":{\"sessionId\":\"root-session\",\"update\":{\"sessionUpdate\":\"agent_message_chunk\",\"content\":{\"type\":\"text\",\"text\":\"Submitted.\"}}}}\n" +
            reply(4, json_util::object({{"stopReason", Json("end_turn")}}));
        const auto after_approval = inspect_acp(inspection);
        require(!after_approval.complete && after_approval.stdin_bytes &&
            after_approval.stdin_bytes->find("session/close") != std::string::npos,
            "a turn that used its approved Task tool completes as before");
        inspection.protocol_state = prompt_state;
        inspection.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"method\":\"session/update\",\"params\":{\"sessionId\":\"wrong-session\",\"update\":{\"sessionUpdate\":\"agent_message_chunk\",\"content\":{\"type\":\"text\",\"text\":\"fake\"}}}}\n";
        require(inspect_acp(inspection).failure_reason.has_value(), "foreign session text accepted");
        inspection.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"method\":\"session/update\",\"params\":{\"sessionId\":\"root-session\",\"update\":{\"sessionUpdate\":\"agent_message_chunk\",\"content\":{\"type\":\"text\",\"text\":\"Done.\"}}}}\n" +
            reply(4, json_util::object({{"stopReason", Json("end_turn")}}));
        result = inspect_acp(inspection);
        require(!result.complete && result.stdin_bytes && result.stdin_bytes->find("session/close") != std::string::npos, "completion skipped native close");
        inspection.protocol_state = result.protocol_state;
        inspection.stdout_bytes = reply(5, json_util::object({{"_meta", json_util::object({{"x.ai/closeOutcome", Json("superseded")}})}}));
        require(inspect_acp(inspection).failure_reason.has_value(), "superseded session counted as closed");
        inspection.stdout_bytes = reply(5, json_util::object({{"_meta", json_util::object({{"x.ai/closeOutcome", Json("closed")}})}}));
        result = inspect_acp(inspection);
        require(result.complete && result.result_bytes == std::optional<std::string>("Done."), "confirmed completion lost");
        inspection.protocol_state = catalog_state;
        inspection.stdout_bytes = reply(3, catalog(Json::array()));
        result = inspect_acp(inspection);
        require(!result.complete && !result.stdin_bytes, "asynchronous MCP startup prompted or failed prematurely");
        inspection.protocol_state = result.protocol_state;
        inspection.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"method\":\"_x.ai/mcp_initialized\",\"params\":{}}\n";
        result = inspect_acp(inspection);
        require(result.stdin_bytes && result.stdin_bytes->find("_x.ai/mcp/list") != std::string::npos,
            "native readiness notification did not refresh pending catalog");
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
