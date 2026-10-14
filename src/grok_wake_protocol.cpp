#include "grok_wake_protocol.h"
#include "grok_json.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <system_error>
#include <utility>

namespace grok {
namespace addon = logonomic::cli_addon;
namespace {

constexpr std::size_t k_record_limit = 1024 * 1024;
constexpr std::size_t k_notification_limit = 4096;
constexpr const char* k_initialize_id = "logonomic-wake-initialize";

std::string rpc(std::string id, const char* method, Json params)
{
    return addon::serialize_json(object({
        {"jsonrpc", Json("2.0")}, {"id", Json(std::move(id))},
        {"method", Json(method)}, {"params", std::move(params)}})) + '\n';
}

bool valid_identity(const std::string& value)
{
    return !value.empty() && value.find('\0') == std::string::npos;
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
    return rpc(k_initialize_id, "initialize", object({
        {"protocolVersion", Json(1)}, {"clientCapabilities", Json::object()},
        {"clientInfo", object({{"name", Json("logonomic-grok-wake")}, {"version", Json("1.0.0")}})}}));
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
    m_ready = false;
}

Wake_exchange Wake_protocol::forget_session()
{
    m_observed_session_id.clear();
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

Wake_exchange Wake_protocol::receive(std::string_view complete_json_line)
{
    if (m_closed) {
        return {};
    }
    if (complete_json_line.size() > k_record_limit) {
        return fail("native_record_limit");
    }
    const auto value = parse(complete_json_line);
    if (!value || text(*value, "jsonrpc") != "2.0") {
        return fail("native_record_invalid");
    }
    if (value->find("method")) {
        // Approval, questions and elicitation belong to the original TUI.
        // This control client never answers an interactive callback.
        if (value->find("id")) {
            return {};
        }
        if (text(*value, "method") != "_x.ai/queue/changed" ||
            !m_pending || !m_pending->write_started)
        {
            return {};
        }
        const auto& params = member(*value, "params");
        if (text(params, "sessionId") != m_session_id) {
            return {};
        }
        bool accepted = text(params, "runningPromptId") == m_pending->notification_id;
        const auto& entries = member(params, "entries");
        if (entries.is_array()) {
            accepted |= std::any_of(entries.as_array().begin(), entries.as_array().end(),
                [&](const Json& entry) { return text(entry, "id") == m_pending->notification_id; });
        }
        return accepted ? finish_notification(Wake_outcome::ACCEPTED, "native_queue_accepted")
                        : Wake_exchange{};
    }

    const auto id      = text(*value, "id");
    const auto& result = member(*value, "result");
    if (id == k_initialize_id) {
        if (!m_initialization_started || m_initialized || value->find("error") ||
            member(result, "protocolVersion") != Json(1))
        {
            return fail("native_initialize_invalid");
        }
        m_agent_instance_id = text(member(result, "_meta"), "agentInstanceId");
        if (!valid_identity(m_agent_instance_id)) {
            return fail("native_instance_identity_missing");
        }
        m_initialized = true;
        return {};
    }
    if (!m_info_request_id.empty() && id == m_info_request_id) {
        m_info_request_id.clear();
        const auto& sessions = member(member(result, "result"), "sessions");
        const Json* resident = nullptr;
        bool ambiguous = false;
        if (sessions.is_array()) {
            for (const auto& session : sessions.as_array()) {
                if (text(session, "sessionId") == m_session_id) {
                    ambiguous |= resident != nullptr;
                    resident = &session;
                }
            }
        }
        if (value->find("error") || !resident || ambiguous ||
            member(*resident, "resident") != Json(true) ||
            text(member(*resident, "origin"), "kind") != "local" ||
            text(*resident, "activity") == "dormant" || text(*resident, "activity") == "dead" ||
            !same_directory(m_cwd, text(*resident, "cwd")))
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
        prompt.push_back(object({{"type", Json("text")}, {"text", Json(m_pending->prompt)}}));
        Wake_exchange exchange;
        exchange.stdin_bytes = rpc(m_pending->request_id, "session/prompt", object({
            {"sessionId", Json(m_session_id)}, {"prompt", std::move(prompt)},
            {"_meta", object({{"promptId", Json(m_pending->notification_id)}, {"verbatim", Json(true)}})}}));
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
