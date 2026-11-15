#include "grok_acp.h"
#include "grok_json.h"
#include <algorithm>
#include <limits>
namespace json_util = logonomic::cli_addon::json;

namespace grok {
namespace addon = logonomic::cli_addon;
namespace {
constexpr std::size_t k_record_limit = 1024 * 1024;
constexpr std::size_t k_result_limit = 1024 * 1024;
std::string rpc(int id, const char* method, Json params)
{
    return addon::serialize_json(json_util::object({{"jsonrpc", Json("2.0")}, {"id", Json(id)},
        {"method", Json(method)}, {"params", std::move(params)}})) + '\n';
}
addon::Structured_inspect_result failure(const char* reason)
{
    addon::Structured_inspect_result result;
    result.complete = true;
    result.failure_reason = reason;
    return result;
}
bool response(const Json& value, int id)
{
    return json_util::text(value, "jsonrpc") == "2.0" && json_util::member(value, "id").is_integer() &&
        json_util::member(value, "id").as_integer() == id && json_util::member(value, "result").is_object() && !value.find("error");
}
bool enrolled_session(const Json& state, const std::string& session)
{
    return !session.empty() && (session == json_util::text(state, "session") ||
        json_util::member(state, "children").find(session));
}
bool enroll_child(Json& state, const Json& params)
{
    const auto& update = json_util::member(params, "update");
    const auto& parent = json_util::member(update, "parent_session_id");
    const auto& child = json_util::member(update, "child_session_id");
    const auto& attempt = json_util::member(update, "attempt_id");
    if (!json_util::bounded_string(parent, 512) || !json_util::bounded_string(child, 512) ||
        (update.find("attempt_id") && !json_util::bounded_string(attempt, 512)) ||
        json_util::member(update, "subagent_id") != child || parent == child ||
        child == json_util::member(state, "session") || !enrolled_session(state, parent.as_string()) ||
        !enrolled_session(state, json_util::text(params, "sessionId")))
    {
        return false;
    }
    auto children = json_util::member(state, "children");
    if (const auto* previous = children.find(child.as_string())) {
        if (json_util::member(*previous, "parent") != parent) {
            return false;
        }
        if (attempt.is_string() && json_util::member(*previous, "attempt") == attempt) {
            return true;
        }
        // A wake is a new attempt on the same session. Its parent continuation
        // has no native close barrier, and an older tool result cannot settle
        // it. Repeated announcements without attempt IDs are also ambiguous.
        state.set("unconfirmed_subagent_work", Json(true));
    }
    // Native SubagentSpawned precedes child Prompt dispatch. Its envelope can
    // name the spawner instead of the actual parent. Keep ancestry across
    // finished attempts: a wake reuses this child session and its routing.
    children.set(child.as_string(), json_util::object({{"parent", parent}, {"attempt", attempt},
        {"foreground_complete", Json(false)}}));
    state.set("children", std::move(children));
    return true;
}
bool record_subagent_tool(Json& state, const std::string& session, const Json& update)
{
    const auto kind = json_util::text(update, "sessionUpdate");
    if (kind != "tool_call" && kind != "tool_call_update") {
        return true;
    }
    const auto input_type = json_util::text(json_util::member(update, "rawInput"), "variant");
    const auto& output = json_util::member(update, "rawOutput");
    const auto output_type = json_util::text(output, "type");
    for (const char* orchestration : {"SendSubagentMessage", "Workflow", "UpdateGoal"}) {
        if (input_type == orchestration || output_type == orchestration) {
            state.set("unconfirmed_subagent_work", Json(true));
        }
    }
    // The native pending Task call has subagentBackground metadata; its
    // refined input has variant=Task. Neither declared background=false nor a
    // completed Text result proves foreground completion: Task can fall back
    // to background execution before the child is even announced.
    const bool task = input_type == "Task" ||
        json_util::member(json_util::member(update, "_meta"), "subagentBackground").is_boolean();
    const bool completed = output_type == "SubagentCompleted";
    if (!task && !completed) {
        return true;
    }
    const auto& call_id = json_util::member(update, "toolCallId");
    if (!json_util::bounded_string(call_id, 512)) {
        return false;
    }
    auto all_calls = json_util::member(state, "spawn_calls");
    auto calls = json_util::member(all_calls, session);
    if (!calls.is_object()) {
        calls = Json::object();
    }
    if (task && (kind == "tool_call" || !calls.find(call_id.as_string()))) {
        calls.set(call_id.as_string(), Json());
    }
    if (completed) {
        const auto* invocation = calls.find(call_id.as_string());
        auto children = json_util::member(state, "children");
        const auto& child_id = json_util::member(output, "subagent_id");
        if (!invocation || json_util::text(update, "status") != "completed" ||
            !json_util::bounded_string(child_id, 512))
        {
            return false;
        }
        const auto* child = children.find(child_id.as_string());
        if (!child || json_util::text(*child, "parent") != session) {
            return false;
        }
        if (invocation->is_object()) {
            return json_util::member(*invocation, "child") == child_id;
        }
        calls.set(call_id.as_string(), json_util::object({{"child", child_id},
            {"attempt", json_util::member(*child, "attempt")}}));
        auto finished = *child;
        finished.set("foreground_complete", Json(true));
        children.set(child_id.as_string(), std::move(finished));
        state.set("children", std::move(children));
    }
    all_calls.set(session, std::move(calls));
    state.set("spawn_calls", std::move(all_calls));
    return true;
}
bool subagents_complete(const Json& state)
{
    if (json_util::member(state, "unconfirmed_subagent_work") == Json(true)) {
        return false;
    }
    for (const auto& child : json_util::member(state, "children").as_object()) {
        if (json_util::member(child.value, "foreground_complete") != Json(true)) {
            return false;
        }
    }
    for (const auto& session : json_util::member(state, "spawn_calls").as_object()) {
        for (const auto& call : session.value.as_object()) {
            if (!call.value.is_object()) {
                return false;
            }
        }
    }
    return true;
}
// A request whose `toolCall` invokes one of the task's own Task tools: Grok
// asks with rawInput {variant: "UseTool", tool_name: "<server>__<action>"}
// and the same title.
bool task_tool_request(const Json& tool_call, const Json& expected)
{
    const auto& input = json_util::member(tool_call, "rawInput");
    const auto name   = json_util::text(input, "tool_name");
    if (json_util::text(input, "variant") != "UseTool" || name.empty() || json_util::text(tool_call, "title") != name) return false;
    for (const auto& server : expected.as_array()) {
        for (const auto& action : json_util::member(server, "actions").as_array()) {
            if (name == json_util::text(server, "name") + "__" + action.as_string()) return true;
        }
    }
    return false;
}
// The reply to a native permission request: its allow-once option for the
// task's own Task tools, its reject-once option for anything else, which the
// launcher's posture has not approved and nobody can approve headlessly.
// Empty when the request is not one Grok writes.
std::string permission_reply(const Json& request, const Json& state, bool* rejected)
{
    const auto& params  = json_util::member(request, "params");
    const auto& options = json_util::member(params, "options");
    if (!enrolled_session(state, json_util::text(params, "sessionId")) || !json_util::member(params, "toolCall").is_object() ||
        !options.is_array()) return {};
    const bool allowed = task_tool_request(json_util::member(params, "toolCall"), json_util::member(state, "servers"));
    const char* option = allowed ? "allow-once" : "reject-once";
    if (!std::any_of(options.as_array().begin(), options.as_array().end(), [&](const auto& offered) {
            return json_util::text(offered, "optionId") == option &&
                json_util::text(offered, "kind") == (allowed ? "allow_once" : "reject_once");
        })) return {};
    *rejected = !allowed && json_util::text(params, "sessionId") == json_util::text(state, "session");
    return addon::serialize_json(json_util::object({{"jsonrpc", Json("2.0")}, {"id", json_util::member(request, "id")},
        {"result", json_util::object({{"outcome", json_util::object({{"outcome", Json("selected")}, {"optionId", Json(option)}})}})}})) + '\n';
}
bool admitted_servers(const Json& result, const Json& expected)
{
    const auto& servers = json_util::member(result, "servers");
    if (!servers.is_array() || !expected.is_array()) return false;
    for (const auto& wanted : expected.as_array()) {
        const Json* found = nullptr;
        for (const auto& server : servers.as_array()) {
            if (json_util::text(server, "name") == json_util::text(wanted, "name")) {
                if (found) return false;
                found = &server;
            }
        }
        if (!found) return false;
        const auto& session = json_util::member(*found, "session");
        if (json_util::member(session, "enabled") != Json(true) || json_util::text(session, "status") != "ready" ||
            json_util::member(session, "authRequired") == Json(true) || json_util::member(session, "setupRequired") == Json(true)) return false;
        const auto& tools = json_util::member(session, "tools");
        if (!tools.is_array()) return false;
        for (const auto& action : json_util::member(wanted, "actions").as_array()) {
            if (!std::any_of(tools.as_array().begin(), tools.as_array().end(), [&](const auto& tool) {
                    return json_util::text(tool, "name") == action.as_string() && json_util::member(tool, "enabled") != Json(false);
                })) return false;
        }
    }
    return true;
}
}
std::string acp_initialize()
{
    return rpc(1, "initialize", json_util::object({{"protocolVersion", Json(1)}, {"clientCapabilities", Json::object()},
        {"clientInfo", json_util::object({{"name", Json("logonomic-grok")}, {"version", Json("1.0.0")}})}}));
}
std::string acp_initial_state(const addon::Delegated_prepare_request& request)
{
    auto servers = Json::array();
    auto expected = Json::array();
    for (const auto& server : request.control.servers) {
        const auto name = "logonomic-" + request.control.task_id + "-" + server.server_id;
        const auto& connection = request.control.connection.value();
        auto args = Json::array();
        for (const auto& argument : {std::string("--descriptor"), connection.descriptor_path,
                 std::string("--server"), server.server_id})
        {
            args.push_back(Json(argument));
        }
        servers.push_back(json_util::object({{"name", Json(name)}, {"command", Json(connection.relay_program)},
            {"args", std::move(args)}, {"env", Json::array()}}));
        auto actions = Json::array();
        for (const auto& action : server.actions) actions.push_back(Json(action));
        expected.push_back(json_util::object({{"name", Json(name)}, {"actions", std::move(actions)}}));
    }
    return addon::serialize_json(json_util::object({{"stage", Json(1)}, {"pending_base64", Json("")},
        {"prompt", Json(request.prompt)}, {"session", Json("")}, {"output", Json("")}, {"children", Json::object()},
        {"spawn_calls", Json::object()}, {"unconfirmed_subagent_work", Json(false)},
        {"new", json_util::object({{"cwd", Json(request.context.working_directory)}, {"mcpServers", std::move(servers)}})},
        {"servers", std::move(expected)}, {"catalog_dirty", Json(false)},
        {"catalog_admitted", Json(false)}, {"catalog_id", Json(3)}, {"rejected", Json(false)}}));
}
addon::Structured_inspect_result inspect_acp(const addon::Structured_inspection& inspection)
{
    auto state = addon::parse_exchange_protocol_state(inspection.protocol_state);
    if (!state || !json_util::exact_keys(*state, {"stage", "pending_base64", "prompt", "session", "output", "children", "new", "servers",
            "spawn_calls", "unconfirmed_subagent_work", "catalog_dirty", "catalog_admitted", "catalog_id", "rejected"}) ||
        !json_util::member(*state, "stage").is_integer() || !json_util::member(*state, "catalog_dirty").is_boolean() || !json_util::member(*state, "servers").is_array() ||
        !json_util::member(*state, "new").is_object() || !json_util::member(*state, "rejected").is_boolean() ||
        !json_util::member(*state, "children").is_object() || !json_util::member(*state, "spawn_calls").is_object() ||
        !json_util::member(*state, "unconfirmed_subagent_work").is_boolean()) return failure("malformed_acp_state");
    for (const auto key : {"pending_base64", "prompt", "session", "output"})
        if (!json_util::member(*state, key).is_string()) return failure("malformed_acp_state");
    if (!json_util::member(*state, "catalog_admitted").is_boolean() ||
        !json_util::member(*state, "catalog_id").is_integer()      ||
        json_util::member(*state, "catalog_id").as_integer() < 3  ||
        json_util::member(*state, "catalog_id").as_integer() > std::numeric_limits<int>::max())
    {
        return failure("malformed_acp_state");
    }
    auto catalog_id = (int)json_util::member(*state, "catalog_id").as_integer();
    auto retained = addon::decode_base64(json_util::text(*state, "pending_base64"));
    if (!retained) return failure("malformed_acp_state");
    // Only a complete newline-delimited record is required to be valid UTF-8.
    auto pending = std::move(*retained);
    pending += inspection.stdout_bytes;
    auto output = json_util::text(*state, "output");
    auto stage = json_util::member(*state, "stage").as_integer();
    std::string input;
    std::size_t offset = 0;
    for (;;) {
        const auto end = pending.find('\n', offset);
        if (end == std::string::npos) break;
        if (end - offset > k_record_limit || end == offset) return failure("acp_record_limit");
        const auto value = json_util::parse(std::string_view(pending).substr(offset, end - offset));
        offset = end + 1;
        if (!value || json_util::text(*value, "jsonrpc") != "2.0") return failure("invalid_acp_record");
        if (value->find("method")) {
            if (value->find("id")) {
                if (json_util::text(*value, "method") != "session/request_permission") return failure("unsupported_acp_client_request");
                bool rejected = false;
                const auto reply = stage == 4 ? permission_reply(*value, *state, &rejected) : std::string{};
                if (reply.empty()) return failure("acp_permission_request_unsupported");
                if (rejected) state->set("rejected", Json(true));
                input += reply;
                continue;
            }
            const auto method = json_util::text(*value, "method");
            if (method == "session/update") {
                const auto& params = json_util::member(*value, "params");
                const auto session = json_util::text(*state, "session");
                const auto source_session = json_util::text(params, "sessionId");
                if (!session.empty() && !enrolled_session(*state, source_session)) {
                    return failure("acp_session_mismatch");
                }
                const auto& update = json_util::member(params, "update");
                if (!record_subagent_tool(*state, source_session, update)) {
                    return failure("invalid_acp_subagent_completion");
                }
                if (json_util::text(update, "sessionUpdate") == "agent_message_chunk" && source_session == session) {
                    if (stage != 4 || json_util::text(json_util::member(update, "content"), "type") != "text") return failure("unexpected_acp_text");
                    const auto& text = json_util::member(json_util::member(update, "content"), "text");
                    if (text.is_string()) {
                        output += text.as_string();
                    }
                    if (output.size() > k_result_limit) return failure("acp_result_limit");
                }
            }
            else
            if (method == "_x.ai/session_notification" &&
                json_util::text(json_util::member(json_util::member(*value, "params"), "update"),
                    "sessionUpdate") == "subagent_spawned")
            {
                if (stage != 4 || !enroll_child(*state, json_util::member(*value, "params"))) {
                    return failure("invalid_acp_child_lineage");
                }
            }
            else
            if (method == "_x.ai/session_notification" &&
                json_util::text(json_util::member(json_util::member(*value, "params"), "update"),
                    "sessionUpdate") == "subagent_finished")
            {
                const auto& params = json_util::member(*value, "params");
                if (enrolled_session(*state, json_util::text(params, "sessionId")) &&
                    json_util::member(json_util::member(params, "update"), "will_wake") == Json(true))
                {
                    state->set("unconfirmed_subagent_work", Json(true));
                }
            }
            else if (!method.starts_with("_x.ai/")) return failure("unsupported_acp_notification");
            if (method == "_x.ai/mcp/server_status" || method == "_x.ai/mcp/tools_changed" ||
                method == "_x.ai/mcp/servers_updated" || method == "_x.ai/mcp_initialized")
            {
                if (stage == 3 || stage == 6) {
                    state->set("catalog_dirty", Json(true));
                }
            }
            continue;
        }
        if (stage == 6 || !input.empty() || !response(*value, stage == 3 ? catalog_id : (int)stage)) {
            return failure("out_of_order_acp_response");
        }
        const auto& result = json_util::member(*value, "result");
        if (stage == 1) {
            if (json_util::member(result, "protocolVersion") != Json(1)) return failure("invalid_acp_initialize");
            input = rpc(2, "session/new", json_util::member(*state, "new"));
            state->set("new", Json::object());
            stage = 2;
        }
        else if (stage == 2) {
            if (!json_util::bounded_string(json_util::member(result, "sessionId"), 512)) return failure("invalid_acp_session");
            state->set("session", json_util::member(result, "sessionId"));
            input = rpc(3, "_x.ai/mcp/list", json_util::object({{"sessionId", json_util::member(*state, "session")}, {"cache", Json(true)}}));
            stage = 3;
        }
        else
        if (stage == 3) {
            // Grok nests the catalog: {"result": {"servers": [...], "sessionMcpResolved": ...}}.
            const auto& catalog = json_util::member(result, "result");
            if (!json_util::member(catalog, "servers").is_array()) {
                return failure("invalid_mcp_catalog");
            }
            // Consume the reply before processing further frames. Admission waits
            // for trailing invalidations, including an incomplete trailing frame.
            state->set("catalog_admitted", Json(admitted_servers(catalog, json_util::member(*state, "servers"))));
            stage = 6;
        }
        else if (stage == 4) {
            // Grok cancels the turn when a tool's permission is rejected.
            if (json_util::text(result, "stopReason") == "cancelled" && json_util::member(*state, "rejected") == Json(true) &&
                json_util::text(json_util::member(result, "_meta"), "cancellationCategory") == "PermissionRejected")
            {
                return failure("tool_permission_denied");
            }
            if (json_util::text(result, "stopReason") != "end_turn" || output.empty()) return failure("acp_completion_failed");
            if (!subagents_complete(*state)) {
                return failure("acp_subagent_completion_unconfirmed");
            }
            input = rpc(5, "session/close", json_util::object({{"sessionId", json_util::member(*state, "session")}}));
            stage = 5;
        }
        else if (stage == 5) {
            const auto outcome = json_util::text(json_util::member(result, "_meta"), "x.ai/closeOutcome");
            if (outcome != "closed" || offset != pending.size() ||
                (inspection.final && (inspection.timed_out || inspection.exit_code != 0))) return failure("acp_close_unconfirmed");
            if (!subagents_complete(*state)) {
                return failure("acp_subagent_completion_unconfirmed");
            }
            addon::Structured_inspect_result completed;
            completed.complete = true;
            completed.result_bytes = std::move(output);
            return completed;
        }
        else return failure("invalid_acp_stage");
    }
    pending.erase(0, offset);
    if (pending.size() > k_record_limit) return failure("acp_record_limit");
    if (inspection.final) return failure(inspection.timed_out ? "acp_timeout" : "acp_incomplete");
    if (stage == 6 && pending.empty()) {
        if (json_util::member(*state, "catalog_dirty") == Json(true)) {
            if (catalog_id == std::numeric_limits<int>::max()) {
                return failure("acp_request_id_limit");
            }
            // Distinct IDs prevent a duplicate old snapshot from satisfying the
            // refresh. The host's absolute operation deadline bounds this wait.
            catalog_id = catalog_id == 3 ? 7 : catalog_id + 1;
            input = rpc(catalog_id, "_x.ai/mcp/list",
                json_util::object({{"sessionId", json_util::member(*state, "session")}, {"cache", Json(true)}}));
            state->set("catalog_dirty", Json(false));
            state->set("catalog_admitted", Json(false));
            stage = 3;
        }
        else
        if (json_util::member(*state, "catalog_admitted") == Json(true)) {
            auto prompt = Json::array();
            prompt.push_back(json_util::object({{"type", Json("text")}, {"text", json_util::member(*state, "prompt")}}));
            input = rpc(4, "session/prompt",
                json_util::object({{"sessionId", json_util::member(*state, "session")}, {"prompt", std::move(prompt)}}));
            state->set("prompt", Json(""));
            stage = 4;
        }
    }
    state->set("catalog_id", Json(catalog_id));
    state->set("stage", Json(stage));
    state->set("pending_base64", Json(addon::encode_base64(pending)));
    state->set("output", Json(std::move(output)));
    addon::Structured_inspect_result result;
    result.protocol_state = addon::serialize_json(*state);
    if (!addon::exchange_protocol_state_valid(result.protocol_state)) return failure("acp_state_limit");
    if (!input.empty()) result.stdin_bytes = std::move(input);
    return result;
}
} // namespace grok
