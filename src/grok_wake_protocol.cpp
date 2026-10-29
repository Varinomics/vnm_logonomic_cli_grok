#include "grok_wake_protocol.h"
#include "grok_json.h"
#include "grok_lifecycle.h"

#include <logonomic_cli_addon/limits.h>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <system_error>
#include <utility>

namespace json_util = logonomic::cli_addon::json;

namespace grok {
namespace addon = logonomic::cli_addon;
namespace {

constexpr std::size_t k_record_limit = 1024 * 1024;
constexpr std::size_t k_notification_limit = 4096;
constexpr std::size_t k_interaction_limit = 4096;
constexpr const char* k_initialize_id = "logonomic-wake-initialize";

std::string rpc(std::string id, const char* method, Json params)
{
    return addon::serialize_json(json_util::object({
        {"jsonrpc", Json("2.0")}, {"id", Json(std::move(id))},
        {"method", Json(method)}, {"params", std::move(params)}})) + '\n';
}

bool valid_identity(const std::string& value)
{
    return !value.empty() && value.find('\0') == std::string::npos;
}

bool valid_attention_identity(const std::string& value)
{
    return !value.empty() && value.size() <= addon::limits::k_max_lifecycle_identity_bytes &&
        value.find_first_of(std::string_view("\0\r\n", 3)) == std::string::npos;
}

Wake_exchange interaction_exchange(const std::string& session, const std::string& call_id,
    const Json& request_id, const std::string& kind = {})
{
    const auto name = kind.empty() ? "interaction_resolved" : "pending_interaction";
    auto update = json_util::object({{"sessionUpdate", Json(name)}, {"tool_call_id", Json(call_id)}});
    if (!kind.empty()) {
        update.set("kind", Json(kind));
    }
    Wake_exchange exchange;
    exchange.interactions.push_back({name, json_util::object({
        {"sessionId", Json(session)}, {"update", std::move(update)}, {"request_id", request_id}})});
    return exchange;
}

bool same_directory(const std::string& expected, const std::string& observed)
{
    if (expected == observed) {
        return true;
    }
    std::error_code error;
    return std::filesystem::equivalent(
        std::filesystem::path(std::u8string(expected.begin(), expected.end())),
        std::filesystem::path(std::u8string(observed.begin(), observed.end())), error) && !error;
}

Wake_exchange result_exchange(std::string id, Wake_outcome outcome, std::string reason)
{
    Wake_exchange exchange;
    exchange.results.push_back({std::move(id), outcome, std::move(reason)});
    return exchange;
}

} // namespace

std::string Wake_protocol::initialize()
{
    assert(!m_initialization_started);
    m_initialization_started = true;
    return rpc(k_initialize_id, "initialize", json_util::object({
        {"protocolVersion", Json(1)}, {"clientCapabilities", Json::object()},
        {"clientInfo", json_util::object({{"name", Json("logonomic-grok-wake")}, {"version", Json("1.0.0")}})}}));
}

std::string Wake_protocol::query_session()
{
    m_ready           = false;
    m_info_request_id = "logonomic-wake-info-" + std::to_string(m_next_request++);
    // A roster read identifies the resident session without subscribing this
    // control client through a request carrying params.sessionId.
    return rpc(m_info_request_id, "_x.ai/sessions/list", Json::object());
}

void Wake_protocol::confirm_session(std::string session_id)
{
    if (m_observed_session_id == session_id) {
        return;
    }
    m_observed_session_id = std::move(session_id);
    m_attention_session_id.clear();
    m_pending_interactions.clear();
    m_ready = false;
}

Wake_exchange Wake_protocol::forget_session()
{
    m_observed_session_id.clear();
    m_attention_session_id.clear();
    m_pending_interactions.clear();
    m_info_request_id.clear();
    m_ready = false;
    m_failure_reason = "native_session_unconfirmed";
    return m_pending ? finish_notification(
        m_pending->write_started ? Wake_outcome::UNCERTAIN : Wake_outcome::UNAVAILABLE,
        m_failure_reason) : Wake_exchange{};
}

Wake_exchange Wake_protocol::inspect(std::string session_id, std::string cwd)
{
    assert(!m_pending && m_info_request_id.empty());
    m_ready = false;
    m_failure_reason.clear();
    m_session_id = std::move(session_id);
    m_cwd        = std::move(cwd);
    if (!m_initialized || m_closed) {
        m_failure_reason = "native_connection_unavailable";
        return {};
    }
    if (!valid_identity(m_session_id) || !valid_identity(m_cwd)) {
        m_failure_reason = "native_session_identity_invalid";
        return {};
    }
    Wake_exchange exchange;
    exchange.stdin_bytes = query_session();
    return exchange;
}

Wake_exchange Wake_protocol::notify(std::string notification_id, std::string prompt)
{
    const auto completed = m_completed.find({m_session_id, notification_id});
    if (completed != m_completed.end()) {
        return result_exchange(notification_id, completed->second.outcome, completed->second.reason);
    }
    if (m_pending) {
        return result_exchange(std::move(notification_id), Wake_outcome::DEFERRED,
            "native_notification_pending");
    }
    // Evicting a submitted ID would make a later retry start another turn.
    // At capacity keep replaying known outcomes and refuse new notifications.
    if (m_completed.size() >= k_notification_limit) {
        return result_exchange(std::move(notification_id), Wake_outcome::UNAVAILABLE,
            "native_notification_capacity_reached");
    }
    if (!m_ready || m_closed || m_observed_session_id != m_session_id) {
        return result_exchange(std::move(notification_id), Wake_outcome::UNAVAILABLE,
            m_failure_reason.empty() ? "native_session_not_inspected" : m_failure_reason);
    }
    if (!valid_identity(notification_id) || !valid_identity(prompt)) {
        return result_exchange(std::move(notification_id), Wake_outcome::UNAVAILABLE,
            "native_notification_invalid");
    }
    m_pending = Pending_notification{std::move(notification_id), std::move(prompt), {}, false};
    Wake_exchange exchange;
    exchange.stdin_bytes = query_session();
    return exchange;
}

void Wake_protocol::prompt_write_started()
{
    assert(m_pending && !m_pending->request_id.empty());
    m_pending->write_started = true;
    // The existing legitimate session/prompt subscribes this native connection.
    // Observation must never issue a separate session-scoped request: a native
    // subscriber can become the driver when the original TUI disconnects.
    m_attention_session_id = m_observed_session_id;
}

Wake_exchange Wake_protocol::finish_notification(Wake_outcome outcome, std::string reason)
{
    assert(m_pending);
    auto exchange = result_exchange(m_pending->notification_id, outcome, std::move(reason));
    // Only submitted outcomes suppress retries. A failed resident lookup has
    // made no turn and can be retried with the same mailbox notification.
    if (outcome == Wake_outcome::ACCEPTED || outcome == Wake_outcome::UNCERTAIN) {
        m_completed.insert_or_assign(
            Notification_key{m_session_id, m_pending->notification_id}, exchange.results.front());
    }
    m_pending.reset();
    return exchange;
}

Wake_exchange Wake_protocol::fail(std::string reason)
{
    m_failure_reason = std::move(reason);
    m_initialized    = false;
    m_ready          = false;
    m_closed         = true;
    m_info_request_id.clear();
    m_attention_session_id.clear();
    m_pending_interactions.clear();
    if (!m_pending) {
        return {};
    }
    return finish_notification(
        m_pending->write_started ? Wake_outcome::UNCERTAIN : Wake_outcome::UNAVAILABLE,
        m_failure_reason);
}

Wake_exchange Wake_protocol::transport_closed(std::string reason)
{
    return fail(std::move(reason));
}

Wake_exchange Wake_protocol::abandon_request(std::string reason)
{
    if (m_pending && m_pending->write_started) {
        return fail(std::move(reason));
    }
    // A caller can cancel a roster read while the persistent ACP connection
    // remains healthy. Retire its RPC ID so a late reply cannot submit a turn.
    m_info_request_id.clear();
    m_ready = false;
    m_failure_reason = std::move(reason);
    if (!m_pending) {
        return {};
    }
    m_prompt_requests.erase(m_pending->request_id);
    return finish_notification(Wake_outcome::UNAVAILABLE, m_failure_reason);
}

Wake_exchange Wake_protocol::receive_interaction(const Json& params)
{
    const auto session = json_util::text(params, "sessionId");
    if (!m_initialized || m_attention_session_id.empty() || session != m_attention_session_id ||
        session != m_observed_session_id || !valid_attention_identity(session)) return {};
    const auto& update = json_util::member(params, "update");
    const auto name = json_util::text(update, "sessionUpdate");
    const auto id = json_util::text(update, "tool_call_id");
    if (!valid_attention_identity(id)) return {};
    if (name == "pending_interaction") {
        const auto kind = json_util::text(update, "kind");
        if (kind != "permission" && kind != "question" && kind != "plan_approval" && kind != "mcp_elicitation") return {};
        if (m_pending_interactions.contains(id)) return {};
        if (m_pending_interactions.size() >= k_interaction_limit) return fail("native_interaction_capacity_reached");
        // Permission guards also cover automatic policy decisions. Only the
        // subsequent reverse RPC establishes a wait for the original driver.
        m_pending_interactions.emplace(id, Pending_interaction{kind, {}});
        return {};
    }
    else if (name == "interaction_resolved") {
        const auto found = m_pending_interactions.find(id);
        if (found == m_pending_interactions.end()) {
            return {};
        }
        const auto request_id = found->second.request_id;
        m_pending_interactions.erase(found);
        return request_id.is_null() ? Wake_exchange{} : interaction_exchange(session, id, request_id);
    }
    return {};
}

Wake_exchange Wake_protocol::receive_interaction_request(const Json& request)
{
    auto method = json_util::text(request, "method");
    const Json* params = &json_util::member(request, "params");
    if (method.starts_with('_')) {
        method.erase(0, 1);
        if (json_util::text(*params, "method") != method) {
            return {};
        }
        params = &json_util::member(*params, "params");
    }
    std::string kind;
    if (method == "session/request_permission") { kind = "permission";      }
    if (method == "x.ai/ask_user_question")     { kind = "question";        }
    if (method == "x.ai/exit_plan_mode")        { kind = "plan_approval";   }
    if (method == "x.ai/mcp/elicit")            { kind = "mcp_elicitation"; }
    if (kind.empty()) {
        return {};
    }

    auto session = json_util::text(*params, "sessionId");
    if (session.empty()) {
        session = json_util::text(*params, "session_id");
    }
    if (!m_initialized || m_attention_session_id.empty() || session != m_attention_session_id ||
        session != m_observed_session_id)
    {
        return {};
    }
    auto call_id = json_util::text(*params, "tool_call_id");
    if (call_id.empty()) {
        call_id = json_util::text(*params, "toolCallId");
    }
    if (call_id.empty()) {
        const auto& call = json_util::member(*params, "toolCall");
        call_id = json_util::text(call, "toolCallId");
    }
    const auto found = m_pending_interactions.find(call_id);
    const auto& request_id = json_util::member(request, "id");
    if (found == m_pending_interactions.end() || found->second.kind != kind ||
        !found->second.request_id.is_null() || !interaction_request_identity(request_id))
    {
        return {};
    }
    found->second.request_id = request_id;
    return interaction_exchange(session, call_id, request_id, kind);
}

Wake_exchange Wake_protocol::receive(std::string_view complete_json_line)
{
    if (m_closed) {
        return {};
    }
    if (complete_json_line.size() > k_record_limit) {
        return fail("native_record_limit");
    }
    const auto value = json_util::parse(complete_json_line);
    if (!value || json_util::text(*value, "jsonrpc") != "2.0") {
        return fail("native_record_invalid");
    }
    if (value->find("method")) {
        // Approval, questions and elicitation belong to the original TUI.
        // This control client never answers an interactive callback.
        if (value->find("id")) {
            return receive_interaction_request(*value);
        }
        const auto method = json_util::text(*value, "method");
        const auto& params = json_util::member(*value, "params");
        if (method == "_x.ai/session_notification" &&
            json_util::text(params, "method") == "x.ai/session_notification") {
            // Gateway ExtNotification adds both the '_' prefix and a nested
            // {method, params} envelope; these are actual native wire fields.
            return receive_interaction(json_util::member(params, "params"));
        }
        if (method == "x.ai/session_notification") return receive_interaction(params);
        if (json_util::text(*value, "method") != "_x.ai/queue/changed" ||
            !m_pending || !m_pending->write_started)
        {
            return {};
        }
        if (json_util::text(params, "sessionId") != m_session_id) {
            return {};
        }
        bool accepted = json_util::text(params, "runningPromptId") == m_pending->notification_id;
        const auto& entries = json_util::member(params, "entries");
        if (entries.is_array()) {
            accepted |= std::any_of(entries.as_array().begin(), entries.as_array().end(),
                [&](const Json& entry) { return json_util::text(entry, "id") == m_pending->notification_id; });
        }
        return accepted ? finish_notification(Wake_outcome::ACCEPTED, "native_queue_accepted")
                        : Wake_exchange{};
    }

    const auto id      = json_util::text(*value, "id");
    const auto& result = json_util::member(*value, "result");
    if (id == k_initialize_id) {
        if (!m_initialization_started || m_initialized || value->find("error") ||
            json_util::member(result, "protocolVersion") != Json(1))
        {
            return fail("native_initialize_invalid");
        }
        m_agent_instance_id = json_util::text(json_util::member(result, "_meta"), "agentInstanceId");
        if (!valid_identity(m_agent_instance_id)) {
            return fail("native_instance_identity_missing");
        }
        m_initialized = true;
        return {};
    }
    if (!m_info_request_id.empty() && id == m_info_request_id) {
        m_info_request_id.clear();
        const auto& sessions = json_util::member(json_util::member(result, "result"), "sessions");
        const Json* resident = nullptr;
        bool ambiguous = false;
        if (sessions.is_array()) {
            for (const auto& session : sessions.as_array()) {
                if (json_util::text(session, "sessionId") == m_session_id) {
                    ambiguous |= resident != nullptr;
                    resident = &session;
                }
            }
        }
        if (value->find("error") || !resident || ambiguous ||
            json_util::member(*resident, "resident") != Json(true) ||
            json_util::text(json_util::member(*resident, "origin"), "kind") != "local" ||
            json_util::text(*resident, "activity") == "dormant" || json_util::text(*resident, "activity") == "dead" ||
            !same_directory(m_cwd, json_util::text(*resident, "cwd")))
        {
            m_failure_reason = "native_resident_session_unavailable";
            m_ready          = false;
            return m_pending
                ? finish_notification(Wake_outcome::UNAVAILABLE, m_failure_reason)
                : Wake_exchange{};
        }
        if (m_observed_session_id != m_session_id) {
            m_failure_reason = "native_session_unconfirmed";
            m_ready          = false;
            return m_pending
                ? finish_notification(Wake_outcome::UNAVAILABLE, m_failure_reason)
                : Wake_exchange{};
        }
        m_ready = true;
        m_failure_reason.clear();
        if (!m_pending) {
            return {};
        }
        m_pending->request_id = "logonomic-wake-prompt-" + std::to_string(m_next_request++);
        m_prompt_requests.emplace(m_pending->request_id,
            Notification_key{m_session_id, m_pending->notification_id});
        auto prompt = Json::array();
        prompt.push_back(json_util::object({{"type", Json("text")}, {"text", Json(m_pending->prompt)}}));
        Wake_exchange exchange;
        exchange.stdin_bytes = rpc(m_pending->request_id, "session/prompt", json_util::object({
            {"sessionId", Json(m_session_id)}, {"prompt", std::move(prompt)},
            {"_meta", json_util::object({{"promptId", Json(m_pending->notification_id)}, {"verbatim", Json(true)}})}}));
        exchange.writes_prompt = true;
        return exchange;
    }
    const auto prompt_request = m_prompt_requests.find(id);
    if (prompt_request != m_prompt_requests.end()) {
        m_prompt_requests.erase(prompt_request);
        if (m_pending && m_pending->request_id == id) {
            // Completion without the correlated queue event cannot establish
            // the queue admission contract. Never submit this ID a second time.
            return finish_notification(
                m_pending->write_started ? Wake_outcome::UNCERTAIN : Wake_outcome::UNAVAILABLE,
                "native_queue_acknowledgement_missing");
        }
    }
    return {};
}

} // namespace grok
