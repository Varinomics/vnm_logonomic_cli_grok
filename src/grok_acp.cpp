#include "grok_acp.h"
#include "grok_json.h"
#include <algorithm>
#include <limits>
namespace grok {
namespace addon = logonomic::cli_addon;
namespace {
constexpr std::size_t k_record_limit = 1024 * 1024;
constexpr std::size_t k_result_limit = 1024 * 1024;
std::string rpc(int id, const char* method, Json params)
{
    return addon::serialize_json(object({{"jsonrpc", Json("2.0")}, {"id", Json(id)},
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
    return text(value, "jsonrpc") == "2.0" && member(value, "id").is_integer() &&
        member(value, "id").as_integer() == id && member(value, "result").is_object() && !value.find("error");
}
// A request whose `toolCall` invokes one of the task's own Task tools: Grok
// asks with rawInput {variant: "UseTool", tool_name: "<server>__<action>"}
// and the same title.
bool task_tool_request(const Json& tool_call, const Json& expected)
{
    const auto& input = member(tool_call, "rawInput");
    const auto name   = text(input, "tool_name");
    if (text(input, "variant") != "UseTool" || name.empty() || text(tool_call, "title") != name) return false;
    for (const auto& server : expected.as_array()) {
        for (const auto& action : member(server, "actions").as_array()) {
            if (name == text(server, "name") + "__" + action.as_string()) return true;
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
    const auto& params  = member(request, "params");
    const auto& options = member(params, "options");
    if (text(params, "sessionId") != text(state, "session") || !member(params, "toolCall").is_object() ||
        !options.is_array()) return {};
    const bool allowed = task_tool_request(member(params, "toolCall"), member(state, "servers"));
    const char* option = allowed ? "allow-once" : "reject-once";
    if (!std::any_of(options.as_array().begin(), options.as_array().end(), [&](const auto& offered) {
            return text(offered, "optionId") == option &&
                text(offered, "kind") == (allowed ? "allow_once" : "reject_once");
        })) return {};
    *rejected = !allowed;
    return addon::serialize_json(object({{"jsonrpc", Json("2.0")}, {"id", member(request, "id")},
        {"result", object({{"outcome", object({{"outcome", Json("selected")}, {"optionId", Json(option)}})}})}})) + '\n';
}
bool admitted_servers(const Json& result, const Json& expected)
{
    const auto& servers = member(result, "servers");
    if (!servers.is_array() || !expected.is_array()) return false;
    for (const auto& wanted : expected.as_array()) {
        const Json* found = nullptr;
        for (const auto& server : servers.as_array()) {
            if (text(server, "name") == text(wanted, "name")) {
                if (found) return false;
                found = &server;
            }
        }
        if (!found) return false;
        const auto& session = member(*found, "session");
        if (member(session, "enabled") != Json(true) || text(session, "status") != "ready" ||
            member(session, "authRequired") == Json(true) || member(session, "setupRequired") == Json(true)) return false;
        const auto& tools = member(session, "tools");
        if (!tools.is_array()) return false;
        for (const auto& action : member(wanted, "actions").as_array()) {
            if (!std::any_of(tools.as_array().begin(), tools.as_array().end(), [&](const auto& tool) {
                    return text(tool, "name") == action.as_string() && member(tool, "enabled") != Json(false);
                })) return false;
        }
    }
    return true;
}
}
std::string acp_initialize()
{
    return rpc(1, "initialize", object({{"protocolVersion", Json(1)}, {"clientCapabilities", Json::object()},
        {"clientInfo", object({{"name", Json("logonomic-grok")}, {"version", Json("1.0.0")}})}}));
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
        servers.push_back(object({{"name", Json(name)}, {"command", Json(connection.relay_program)},
            {"args", std::move(args)}, {"env", Json::array()}}));
        auto actions = Json::array();
        for (const auto& action : server.actions) actions.push_back(Json(action));
        expected.push_back(object({{"name", Json(name)}, {"actions", std::move(actions)}}));
    }
    return addon::serialize_json(object({{"stage", Json(1)}, {"pending", Json("")},
        {"prompt", Json(request.prompt)}, {"session", Json("")}, {"output", Json("")},
        {"new", object({{"cwd", Json(request.context.working_directory)}, {"mcpServers", std::move(servers)}})},
        {"servers", std::move(expected)}, {"catalog_dirty", Json(false)},
        {"catalog_admitted", Json(false)}, {"catalog_id", Json(3)}, {"rejected", Json(false)}}));
}
addon::Structured_inspect_result inspect_acp(const addon::Structured_inspection& inspection)
{
    auto state = parse(inspection.protocol_state);
    if (!state || !exact_keys(*state, {"stage", "pending", "prompt", "session", "output", "new", "servers",
            "catalog_dirty", "catalog_admitted", "catalog_id", "rejected"}) ||
        !member(*state, "stage").is_integer() || !member(*state, "catalog_dirty").is_boolean() || !member(*state, "servers").is_array() ||
        !member(*state, "new").is_object() || !member(*state, "rejected").is_boolean()) return failure("malformed_acp_state");
    for (const auto key : {"pending", "prompt", "session", "output"})
        if (!member(*state, key).is_string()) return failure("malformed_acp_state");
    if (!member(*state, "catalog_admitted").is_boolean() ||
        !member(*state, "catalog_id").is_integer()      ||
        member(*state, "catalog_id").as_integer() < 3  ||
        member(*state, "catalog_id").as_integer() > std::numeric_limits<int>::max())
    {
        return failure("malformed_acp_state");
    }
    auto catalog_id = (int)member(*state, "catalog_id").as_integer();
    auto pending = text(*state, "pending") + inspection.stdout_bytes;
    auto output = text(*state, "output");
    auto stage = member(*state, "stage").as_integer();
    std::string input;
    std::size_t offset = 0;
    for (;;) {
        const auto end = pending.find('\n', offset);
        if (end == std::string::npos) break;
        if (end - offset > k_record_limit || end == offset) return failure("acp_record_limit");
        const auto value = parse(std::string_view(pending).substr(offset, end - offset));
        offset = end + 1;
        if (!value || text(*value, "jsonrpc") != "2.0") return failure("invalid_acp_record");
        if (value->find("method")) {
            if (value->find("id")) {
                if (text(*value, "method") != "session/request_permission") return failure("unsupported_acp_client_request");
                bool rejected = false;
                const auto reply = stage == 4 ? permission_reply(*value, *state, &rejected) : std::string{};
                if (reply.empty()) return failure("acp_permission_request_unsupported");
                if (rejected) state->set("rejected", Json(true));
                input += reply;
                continue;
            }
            const auto method = text(*value, "method");
            if (method == "session/update") {
                const auto& params = member(*value, "params");
                const auto session = text(*state, "session");
                if (!session.empty() && text(params, "sessionId") != session) return failure("acp_session_mismatch");
                const auto& update = member(params, "update");
                if (text(update, "sessionUpdate") == "agent_message_chunk") {
                    if (stage != 4 || text(member(update, "content"), "type") != "text") return failure("unexpected_acp_text");
                    output += text(member(update, "content"), "text");
                    if (output.size() > k_result_limit) return failure("acp_result_limit");
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
        const auto& result = member(*value, "result");
        if (stage == 1) {
            if (member(result, "protocolVersion") != Json(1)) return failure("invalid_acp_initialize");
            input = rpc(2, "session/new", member(*state, "new"));
            state->set("new", Json::object());
            stage = 2;
        }
        else if (stage == 2) {
            if (!bounded_string(member(result, "sessionId"), 512)) return failure("invalid_acp_session");
            state->set("session", member(result, "sessionId"));
            input = rpc(3, "_x.ai/mcp/list", object({{"sessionId", member(*state, "session")}, {"cache", Json(true)}}));
            stage = 3;
        }
        else
        if (stage == 3) {
            // Grok nests the catalog: {"result": {"servers": [...], "sessionMcpResolved": ...}}.
            const auto& catalog = member(result, "result");
            if (!member(catalog, "servers").is_array()) {
                return failure("invalid_mcp_catalog");
            }
            // Consume the reply before processing further frames. Admission waits
            // for trailing invalidations, including an incomplete trailing frame.
            state->set("catalog_admitted", Json(admitted_servers(catalog, member(*state, "servers"))));
            stage = 6;
        }
        else if (stage == 4) {
            // Grok cancels the turn when a tool's permission is rejected.
            if (text(result, "stopReason") == "cancelled" && member(*state, "rejected") == Json(true) &&
                text(member(result, "_meta"), "cancellationCategory") == "PermissionRejected")
            {
                return failure("tool_permission_denied");
            }
            if (text(result, "stopReason") != "end_turn" || output.empty()) return failure("acp_completion_failed");
            input = rpc(5, "session/close", object({{"sessionId", member(*state, "session")}}));
            stage = 5;
        }
        else if (stage == 5) {
            const auto outcome = text(member(result, "_meta"), "x.ai/closeOutcome");
            if (outcome != "closed" || offset != pending.size() ||
                (inspection.final && (inspection.timed_out || inspection.exit_code != 0))) return failure("acp_close_unconfirmed");
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
        if (member(*state, "catalog_dirty") == Json(true)) {
            if (catalog_id == std::numeric_limits<int>::max()) {
                return failure("acp_request_id_limit");
            }
            // Distinct IDs prevent a duplicate old snapshot from satisfying the
            // refresh. The host's absolute operation deadline bounds this wait.
            catalog_id = catalog_id == 3 ? 7 : catalog_id + 1;
            input = rpc(catalog_id, "_x.ai/mcp/list",
                object({{"sessionId", member(*state, "session")}, {"cache", Json(true)}}));
            state->set("catalog_dirty", Json(false));
            state->set("catalog_admitted", Json(false));
            stage = 3;
        }
        else
        if (member(*state, "catalog_admitted") == Json(true)) {
            auto prompt = Json::array();
            prompt.push_back(object({{"type", Json("text")}, {"text", member(*state, "prompt")}}));
            input = rpc(4, "session/prompt",
                object({{"sessionId", member(*state, "session")}, {"prompt", std::move(prompt)}}));
            state->set("prompt", Json(""));
            stage = 4;
        }
    }
    state->set("catalog_id", Json(catalog_id));
    state->set("stage", Json(stage));
    state->set("pending", Json(std::move(pending)));
    state->set("output", Json(std::move(output)));
    addon::Structured_inspect_result result;
    result.protocol_state = addon::serialize_json(*state);
    if (!addon::exchange_protocol_state_valid(result.protocol_state)) return failure("acp_state_limit");
    if (!input.empty()) result.stdin_bytes = std::move(input);
    return result;
}
} // namespace grok
