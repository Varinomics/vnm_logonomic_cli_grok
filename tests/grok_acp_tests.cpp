#include "grok_acp.h"
#include "grok_json.h"
#include <iostream>
#include <stdexcept>
namespace addon = logonomic::cli_addon;
using namespace grok;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string reply(int id, Json result)
{
    return addon::serialize_json(object({{"jsonrpc", Json("2.0")}, {"id", Json(id)}, {"result", std::move(result)}})) + '\n';
}
// Oracle: grok 1.0.41 answers `_x.ai/mcp/list` with its catalog nested one
// level down, as recorded by the scripted delegation gate
// (`{"result":{"servers":[...],"sessionMcpResolved":false}}`).
Json catalog(Json servers)
{
    return object({{"result", object({{"servers", std::move(servers)}, {"sessionMcpResolved", Json(false)}})}});
}
namespace {
int require_catalog_refresh(const addon::Structured_inspect_result& result)
{
    require(!result.complete && !result.failure_reason && result.stdin_bytes.has_value(),
        "invalidated catalog did not request a refresh");
    const auto request = parse(*result.stdin_bytes);
    require(request && text(*request, "method") == "_x.ai/mcp/list",
        "invalidated catalog released the prompt");
    require(text(member(*request, "params"), "sessionId") == "root-session" &&
        member(member(*request, "params"), "cache") == Json(true),
        "refresh changed the native session query semantics");
    return (int)member(*request, "id").as_integer();
}
void require_prompt(const addon::Structured_inspect_result& result, const std::string& expected)
{
    require(!result.complete && !result.failure_reason && result.stdin_bytes.has_value(),
        "clean catalog did not release the retained prompt");
    const auto request = parse(*result.stdin_bytes);
    require(request && text(*request, "method") == "session/prompt",
        "clean catalog did not issue the native prompt request");
    const auto& prompt = member(member(*request, "params"), "prompt");
    require(prompt.is_array() && prompt.as_array().size() == 1 &&
        text(prompt.as_array().front(), "text") == expected,
        "catalog refresh changed the original prompt");
}
void catalog_ordering(const std::string& catalog_state, const Json& servers, const std::string& prompt)
{
    // The native MCP catalog is invalidated by each of these notifications.
    // A prompt requires a clean session snapshot with its required actions.
    const auto ready = catalog(servers);
    for (const auto& rejection : {
            object({{"enabled", Json(false)}}),
            object({{"status", Json("connecting")}}),
            object({{"authRequired", Json(true)}}),
            object({{"setupRequired", Json(true)}}),
            object({{"tools", Json::array()}})})
    {
        auto server = servers.as_array().front();
        auto session = member(server, "session");
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
        const auto notification = addon::serialize_json(object({
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
    inspection.stdout_bytes = reply(3, ready) + reply(4, object({{"stopReason", Json("end_turn")}}));
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
        inspection.stdout_bytes = reply(1, object({{"protocolVersion", Json(1)}}));
        auto result = inspect_acp(inspection);
        require(result.stdin_bytes && result.stdin_bytes->find("session/new") != std::string::npos, "new session not sequenced after initialize");
        require(result.stdin_bytes->find("session/prompt") == std::string::npos, "prompt escaped MCP admission");
        const auto native_new = parse(*result.stdin_bytes);
        require(native_new.has_value(), "session/new must remain valid native JSON");
        const auto& emitted_servers = member(member(*native_new, "params"), "mcpServers");
        require(emitted_servers.is_array() && emitted_servers.as_array().size() == 1,
            "forward exactly the required task server");
        const auto& emitted_server = emitted_servers.as_array().front();
        const auto& args = member(emitted_server, "args").as_array();
        require(text(emitted_server, "command") == request.control.connection->relay_program &&
            args.size() == 4 && args[0].as_string() == "--descriptor" &&
            args[1].as_string() == request.control.connection->descriptor_path &&
            args[2].as_string() == "--server" && args[3].as_string() == "control",
            "native MCP request preserves exact nonsecret relay arguments");
        const auto& emitted_env = member(emitted_server, "env");
        require(emitted_env.is_array() && emitted_env.as_array().empty() &&
            result.stdin_bytes->find("opaque") == std::string::npos &&
            result.protocol_state.find("opaque") == std::string::npos,
            "host authority cannot enter ACP requests or continuation");
        inspection.protocol_state = result.protocol_state;
        inspection.stdout_bytes = reply(2, object({{"sessionId", Json("root-session")}}));
        result = inspect_acp(inspection);
        require(result.stdin_bytes && result.stdin_bytes->find("_x.ai/mcp/list") != std::string::npos, "native MCP readiness not queried");
        const auto catalog_state = result.protocol_state;
        auto servers = Json::array();
        auto tools = Json::array();
        tools.push_back(object({{"name", Json("complete")}, {"enabled", Json(true)}}));
        servers.push_back(object({{"name", Json("logonomic-task-1-control")},
            {"session", object({{"enabled", Json(true)}, {"status", Json("ready")}, {"tools", std::move(tools)}})}}));
        catalog_ordering(catalog_state, servers, request.prompt);
        inspection.protocol_state = catalog_state;
        inspection.stdout_bytes = reply(3, catalog(servers));
        result = inspect_acp(inspection);
        require(result.stdin_bytes && result.stdin_bytes->find("session/prompt") != std::string::npos, "admitted control did not release prompt");
        const auto prompt_state = result.protocol_state;
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
            options.push_back(object({{"optionId", Json("always-allow")}, {"name", Json("always allow")}, {"kind", Json("allow_always")}}));
            if (allow_once) {
                options.push_back(object({{"optionId", Json("allow-once")}, {"name", Json("allow once")}, {"kind", Json("allow_once")}}));
            }
            options.push_back(object({{"optionId", Json("reject-once")}, {"name", Json("reject once")}, {"kind", Json("reject_once")}}));
            auto input = object({{"variant", Json("UseTool")}, {"tool_name", Json(tool)}, {"tool_input", Json::object()}});
            auto call = object({{"toolCallId", Json("call_7d5edeb1")}, {"kind", Json("other")}, {"title", Json(title)},
                {"rawInput", std::move(input)}});
            return addon::serialize_json(object({{"jsonrpc", Json("2.0")}, {"id", Json(0)},
                {"method", Json("session/request_permission")},
                {"params", object({{"sessionId", Json("root-session")}, {"toolCall", std::move(call)},
                    {"options", std::move(options)}})}})) + '\n';
        };
        const auto chosen = [](const addon::Structured_inspect_result& answered) {
            require(!answered.complete && !answered.failure_reason && answered.stdin_bytes.has_value(),
                "a permission request was not answered");
            const auto reply = parse(*answered.stdin_bytes);
            require(reply && member(*reply, "id") == Json(0) &&
                text(member(member(*reply, "result"), "outcome"), "outcome") == "selected",
                "the answer names the request and selects one of its options");
            return text(member(member(*reply, "result"), "outcome"), "optionId");
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
            inspection.stdout_bytes = reply(4, object({{"stopReason", Json("cancelled")},
                {"_meta", object({{"cancellationCategory", Json("PermissionRejected")}})}}));
            require(inspect_acp(inspection).failure_reason == std::optional<std::string>("tool_permission_denied"),
                "a turn Grok ended at a rejected tool fails as that denial");
            inspection.stdout_bytes = reply(4, object({{"stopReason", Json("cancelled")},
                {"_meta", object({{"cancellationCategory", Json("UserCancelled")}})}}));
            require(inspect_acp(inspection).failure_reason == std::optional<std::string>("acp_completion_failed"),
                "a cancellation for another cause after a rejection is not a permission denial");
        }
        inspection.protocol_state = prompt_state;
        inspection.stdout_bytes = reply(4, object({{"stopReason", Json("cancelled")},
            {"_meta", object({{"cancellationCategory", Json("PermissionRejected")}})}}));
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
            reply(4, object({{"stopReason", Json("end_turn")}}));
        const auto after_approval = inspect_acp(inspection);
        require(!after_approval.complete && after_approval.stdin_bytes &&
            after_approval.stdin_bytes->find("session/close") != std::string::npos,
            "a turn that used its approved Task tool completes as before");
        inspection.protocol_state = prompt_state;
        inspection.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"method\":\"session/update\",\"params\":{\"sessionId\":\"wrong-session\",\"update\":{\"sessionUpdate\":\"agent_message_chunk\",\"content\":{\"type\":\"text\",\"text\":\"fake\"}}}}\n";
        require(inspect_acp(inspection).failure_reason.has_value(), "foreign session text accepted");
        inspection.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"method\":\"session/update\",\"params\":{\"sessionId\":\"root-session\",\"update\":{\"sessionUpdate\":\"agent_message_chunk\",\"content\":{\"type\":\"text\",\"text\":\"Done.\"}}}}\n" +
            reply(4, object({{"stopReason", Json("end_turn")}}));
        result = inspect_acp(inspection);
        require(!result.complete && result.stdin_bytes && result.stdin_bytes->find("session/close") != std::string::npos, "completion skipped native close");
        inspection.protocol_state = result.protocol_state;
        inspection.stdout_bytes = reply(5, object({{"_meta", object({{"x.ai/closeOutcome", Json("superseded")}})}}));
        require(inspect_acp(inspection).failure_reason.has_value(), "superseded session counted as closed");
        inspection.stdout_bytes = reply(5, object({{"_meta", object({{"x.ai/closeOutcome", Json("closed")}})}}));
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
