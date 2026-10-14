#include "grok_wake_protocol.h"
#include "grok_json.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;
using grok::Json;
using grok::Wake_exchange;
using grok::Wake_outcome;
using grok::Wake_protocol;
using grok::member;
using grok::object;
using grok::text;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Json request(const std::string& bytes)
{
    const auto value = grok::parse(bytes);
    require(value.has_value(), "missing or invalid request JSON");
    require(text(*value, "jsonrpc") == "2.0", "request is not JSON-RPC");
    return *value;
}

std::string reply(const Json& query, Json result)
{
    return addon::serialize_json(object({
        {"jsonrpc", Json("2.0")}, {"id", member(query, "id")}, {"result", std::move(result)}}));
}

std::string info_reply(const Json& query, const char* session = "original-session", const char* cwd = "/work")
{
    // Observed on native Windows Grok 1.0.52. The roster includes dormant
    // persisted sessions too; residency and exact identity must both match.
    auto sessions = Json::array();
    sessions.push_back(object({{"sessionId", Json(session)}, {"cwd", Json(cwd)},
        {"resident", Json(true)}, {"activity", Json("idle")},
        {"origin", object({{"kind", Json("local")}})}}));
    return reply(query, object({{"result", object({{"sessions", std::move(sessions)}})}}));
}

std::string queued(const char* session, const char* notification, bool running = false)
{
    auto params = object({{"sessionId", Json(session)}, {"entries", Json::array()}});
    if (running) {
        params.set("runningPromptId", Json(notification));
    }
    else {
        auto entries = Json::array();
        entries.push_back(object({{"id", Json(notification)}, {"kind", Json("prompt")}}));
        params.set("entries", std::move(entries));
    }
    return addon::serialize_json(object({
        {"jsonrpc", Json("2.0")}, {"method", Json("_x.ai/queue/changed")}, {"params", std::move(params)}}));
}

void initialize(Wake_protocol& protocol)
{
    const auto query = request(protocol.initialize());
    require(text(query, "method") == "initialize", "connection did not initialize ACP");
    const auto exchange = protocol.receive(reply(query, object({
        {"protocolVersion", Json(1)}, {"_meta", object({{"agentInstanceId", Json("original-leader")}})}})));
    require(exchange.stdin_bytes.empty() && exchange.results.empty(), "initialize mutated a native session");
    require(protocol.initialized() && !protocol.ready(), "initialize falsely established session readiness");
    require(protocol.agent_instance_id() == "original-leader", "leader identity was not retained");
}

void inspect(Wake_protocol& protocol, const char* session = "original-session", const char* cwd = "/work")
{
    protocol.confirm_session(session);
    const auto query = request(protocol.inspect(session, cwd).stdin_bytes);
    require(text(query, "method") == "_x.ai/sessions/list", "inspection was not a native roster read");
    require(member(query, "params") == Json::object(), "inspection implicitly subscribed to a native session");
    const auto exchange = protocol.receive(info_reply(query, session, cwd));
    require(exchange.stdin_bytes.empty() && exchange.results.empty(), "inspection mutated the session");
    require(protocol.ready(), "matching native resident identity was not admitted");
}

Wake_exchange prepare_prompt(Wake_protocol& protocol, const char* notification = "mailbox-17")
{
    const auto check = request(protocol.notify(notification, "Read your mailbox.").stdin_bytes);
    require(text(check, "method") == "_x.ai/sessions/list", "notification skipped resident revalidation");
    require(member(check, "params") == Json::object(), "resident recheck implicitly subscribed to a native session");
    auto exchange = protocol.receive(info_reply(check, protocol.session_id().c_str()));
    require(exchange.writes_prompt && exchange.results.empty(), "notification claimed acceptance before writing");
    const auto query   = request(exchange.stdin_bytes);
    const auto& params = member(query, "params");
    require(text(query, "method") == "session/prompt", "notification did not use native ACP prompt");
    require(text(params, "sessionId") == protocol.session_id(), "prompt changed the original native session");
    require(text(member(params, "_meta"), "promptId") == notification, "prompt lost notification correlation");
    require(member(member(params, "_meta"), "verbatim") == Json(true), "prompt altered notification semantics");
    const auto& prompt = member(params, "prompt");
    require(prompt.is_array() && prompt.as_array().size() == 1, "prompt content is not one text block");
    require(text(prompt.as_array().front(), "text") == "Read your mailbox.", "notification text changed");
    return exchange;
}

void require_result(const Wake_exchange& exchange, Wake_outcome outcome, const char* id = "mailbox-17")
{
    require(exchange.stdin_bytes.empty(), "result unexpectedly wrote to the provider");
    require(exchange.results.size() == 1, "missing correlated notification result");
    require(exchange.results.front().notification_id == id, "result notification identity changed");
    require(exchange.results.front().outcome == outcome, "notification outcome is incorrect");
}

void native_queue_and_deduplication()
{
    for (const bool running : {false, true}) {
        Wake_protocol protocol;
        initialize(protocol);
        inspect(protocol);
        const auto prompt_query = request(prepare_prompt(protocol).stdin_bytes);
        require(protocol.receive(queued("original-session", "mailbox-17", running)).results.empty(),
            "queue event was credited before the prompt write");
        protocol.prompt_write_started();
        require(protocol.receive(queued("other-session", "mailbox-17", running)).results.empty(),
            "another session acknowledged the notification");
        require(protocol.receive(queued("original-session", "other-mail", running)).results.empty(),
            "another notification acknowledged this request");
        require_result(protocol.notify("mailbox-17", "Read your mailbox."), Wake_outcome::DEFERRED);
        require_result(protocol.receive(queued("original-session", "mailbox-17", running)), Wake_outcome::ACCEPTED);
        require(protocol.receive(queued("original-session", "mailbox-17", running)).results.empty(),
            "repeated queue update produced duplicate acknowledgement");
        require(protocol.receive(reply(prompt_query, object({{"stopReason", Json("end_turn")}}))).results.empty(),
            "turn completion was reported as another queue acknowledgement");
        inspect(protocol);
        require_result(protocol.notify("mailbox-17", "Read your mailbox."), Wake_outcome::ACCEPTED);

        inspect(protocol, "next-session");
        prepare_prompt(protocol);
        protocol.prompt_write_started();
        require_result(protocol.receive(queued("next-session", "mailbox-17")), Wake_outcome::ACCEPTED);
        require(protocol.agent_instance_id() == "original-leader", "session change replaced leader identity");
    }
}

void permission_requests_remain_interactive()
{
    Wake_protocol protocol;
    initialize(protocol);
    inspect(protocol);
    prepare_prompt(protocol);
    protocol.prompt_write_started();
    for (const char* method : {"session/request_permission", "_x.ai/ask_user_question", "_x.ai/approve_plan"}) {
        const auto exchange = protocol.receive(addon::serialize_json(object({
            {"jsonrpc", Json("2.0")}, {"id", Json("native-callback")},
            {"method", Json(method)}, {"params", object({{"sessionId", Json("original-session")}})}})));
        require(exchange.stdin_bytes.empty() && exchange.results.empty(),
            "control client answered or resolved an interactive callback");
    }
    require_result(protocol.receive(queued("original-session", "mailbox-17")), Wake_outcome::ACCEPTED);
}

void startup_and_session_identity()
{
    Wake_protocol protocol;
    initialize(protocol);
    auto query = request(protocol.inspect("original-session", "/work").stdin_bytes);
    require(text(query, "method") == "_x.ai/sessions/list" && member(query, "params") == Json::object(),
        "startup inspection implicitly subscribed to a native session");
    auto exchange = protocol.receive(info_reply(query));
    require(exchange.stdin_bytes.empty() && !protocol.ready(), "residency alone established the managed session identity");
    require(protocol.failure_reason() == "native_session_unconfirmed", "unconfirmed session had no explicit reason");
    require_result(protocol.notify("mailbox-17", "Read your mailbox."), Wake_outcome::UNAVAILABLE);

    protocol.confirm_session("other-session");
    query = request(protocol.inspect("original-session", "/work").stdin_bytes);
    protocol.receive(info_reply(query));
    require(!protocol.ready(), "another session's observation crossed the native session boundary");
    inspect(protocol);
    protocol.forget_session();
    require(!protocol.ready(), "forgetting the observed session retained readiness");
    require_result(protocol.notify("mailbox-17", "Read your mailbox."), Wake_outcome::UNAVAILABLE);

    inspect(protocol);
    query = request(protocol.notify("mailbox-17", "Read your mailbox.").stdin_bytes);
    require_result(protocol.forget_session(), Wake_outcome::UNAVAILABLE);
    exchange = protocol.receive(info_reply(query));
    require(exchange.stdin_bytes.empty() && exchange.results.empty(), "late roster reply revived an invalid session");
    require(!protocol.ready(), "session loss during resident recheck emitted a prompt");

    inspect(protocol);
    prepare_prompt(protocol);
    protocol.prompt_write_started();
    require_result(protocol.forget_session(), Wake_outcome::UNCERTAIN);
    require_result(protocol.notify("mailbox-17", "Read your mailbox."), Wake_outcome::UNCERTAIN);
}

void roster_rejects_nonresident_or_remote_sessions()
{
    for (const char* invalid : {"dormant", "dead", "remote", "duplicate", "nonresident"}) {
        Wake_protocol protocol;
        initialize(protocol);
        protocol.confirm_session("original-session");
        const auto query = request(protocol.inspect("original-session", "/work").stdin_bytes);
        auto response = request(info_reply(query));
        auto sessions = member(member(member(response, "result"), "result"), "sessions");
        auto& row = sessions.as_array().front();
        const std::string kind = invalid;
        if (kind == "remote") row.set("origin", object({{"kind", Json("remote")}}));
        else if (kind == "duplicate") sessions.push_back(row);
        else if (kind == "nonresident") row.set("resident", Json(false));
        else row.set("activity", Json(kind));
        response.set("result", object({{"result", object({{"sessions", std::move(sessions)}})}}));
        const auto exchange = protocol.receive(addon::serialize_json(response));
        require(exchange.stdin_bytes.empty() && !protocol.ready(), "unqualified native roster row was admitted");
    }

    Wake_protocol protocol;
    initialize(protocol);
    protocol.confirm_session("original-session");
    const auto query = request(protocol.inspect("original-session", "/work").stdin_bytes);
    auto response = request(info_reply(query));
    auto sessions = member(member(member(response, "result"), "result"), "sessions");
    sessions.as_array().insert(sessions.as_array().begin(), object({
        {"sessionId", Json("first-unrelated-session")}, {"cwd", Json("/elsewhere")},
        {"resident", Json(true)}, {"activity", Json("idle")},
        {"origin", object({{"kind", Json("local")}})}}));
    response.set("result", object({{"result", object({{"sessions", std::move(sessions)}})}}));
    protocol.receive(addon::serialize_json(response));
    require(protocol.ready(), "inspection selected the first session instead of the exact target");
}

void resident_identity_and_protocol_failures()
{
    for (const bool wrong_session : {false, true}) {
        Wake_protocol protocol;
        initialize(protocol);
        const auto query = request(protocol.inspect("original-session", "/work").stdin_bytes);
        const auto exchange = protocol.receive(info_reply(query,
            wrong_session ? "other-session" : "original-session", wrong_session ? "/work" : "/elsewhere"));
        require(exchange.stdin_bytes.empty() && !protocol.ready(), "mismatched native identity was admitted");
        require(!protocol.failure_reason().empty(), "failed native inspection had no reason");
        require_result(protocol.notify("mailbox-17", "Read your mailbox."), Wake_outcome::UNAVAILABLE);
        inspect(protocol);

        const auto recheck = request(protocol.notify("mailbox-17", "Read your mailbox.").stdin_bytes);
        require_result(protocol.receive(info_reply(recheck, "other-session")), Wake_outcome::UNAVAILABLE);
        inspect(protocol);
        prepare_prompt(protocol);
    }
    Wake_protocol protocol;
    const auto query = request(protocol.initialize());
    protocol.receive(reply(query, object({{"protocolVersion", Json(1)}})));
    require(!protocol.initialized() && !protocol.failure_reason().empty(), "missing native instance was admitted");
}

void transport_outcomes()
{
    for (const bool write_started : {false, true}) {
        Wake_protocol protocol;
        initialize(protocol);
        inspect(protocol);
        prepare_prompt(protocol);
        if (write_started) {
            protocol.prompt_write_started();
        }
        const auto expected = write_started ? Wake_outcome::UNCERTAIN : Wake_outcome::UNAVAILABLE;
        require_result(protocol.transport_closed("native_transport_closed"), expected);
        require_result(protocol.notify("mailbox-17", "Read your mailbox."), expected);
    }
    Wake_protocol protocol;
    initialize(protocol);
    inspect(protocol);
    const auto prompt_query = request(prepare_prompt(protocol).stdin_bytes);
    protocol.prompt_write_started();
    require_result(protocol.receive(reply(prompt_query, object({{"stopReason", Json("end_turn")}}))),
        Wake_outcome::UNCERTAIN);
    inspect(protocol);
    require_result(protocol.notify("mailbox-17", "Read your mailbox."), Wake_outcome::UNCERTAIN);

    Wake_protocol malformed;
    initialize(malformed);
    inspect(malformed);
    prepare_prompt(malformed);
    malformed.prompt_write_started();
    require_result(malformed.receive("not JSON"), Wake_outcome::UNCERTAIN);
}

void retained_notification_capacity()
{
    Wake_protocol protocol;
    initialize(protocol);
    inspect(protocol);
    for (int i = 0; i < 4096; ++i) {
        const auto id = "mailbox-" + std::to_string(i);
        const auto prompt_query = request(prepare_prompt(protocol, id.c_str()).stdin_bytes);
        protocol.prompt_write_started();
        require_result(protocol.receive(queued("original-session", id.c_str())), Wake_outcome::ACCEPTED, id.c_str());
        protocol.receive(reply(prompt_query, object({{"stopReason", Json("end_turn")}})));
    }
    const auto refused = protocol.notify("mailbox-capacity", "Read your mailbox.");
    require_result(refused, Wake_outcome::UNAVAILABLE, "mailbox-capacity");
    require(refused.results.front().reason == "native_notification_capacity_reached",
        "notification capacity did not explain its refusal");
    require_result(protocol.notify("mailbox-0", "Read your mailbox."), Wake_outcome::ACCEPTED, "mailbox-0");
    require_result(protocol.notify("mailbox-4095", "Read your mailbox."), Wake_outcome::ACCEPTED, "mailbox-4095");
}

void abandoned_notification_retries()
{
    for (const bool write_started : {false, true}) {
        Wake_protocol protocol;
        initialize(protocol);
        inspect(protocol);
        prepare_prompt(protocol);
        if (write_started) {
            protocol.prompt_write_started();
        }
        const auto expected = write_started ? Wake_outcome::UNCERTAIN : Wake_outcome::UNAVAILABLE;
        require_result(protocol.abandon_request("native_request_cancelled"), expected);
        if (write_started) {
            require_result(protocol.notify("mailbox-17", "Read your mailbox."), Wake_outcome::UNCERTAIN);
        }
        else {
            inspect(protocol);
            prepare_prompt(protocol);
            protocol.prompt_write_started();
            require_result(protocol.receive(queued("original-session", "mailbox-17")), Wake_outcome::ACCEPTED);
        }
    }
}

void native_directory_identity()
{
    namespace fs = std::filesystem;
    class Temporary_directory
    {
    public:
        Temporary_directory()
        {
            m_path = fs::temp_directory_path() / ("grok-wake-protocol-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
            require(fs::create_directory(m_path), "could not create native directory fixture");
        }
        ~Temporary_directory()
        {
            std::error_code error;
            fs::remove_all(m_path, error);
        }
        const fs::path& path() const { return m_path; }
    private:
        fs::path m_path;
    } directory;
    const auto path_bytes = [](const fs::path& path) {
        const auto bytes = path.u8string();
        return std::string(bytes.begin(), bytes.end());
    };
    const auto original = path_bytes(directory.path());
    auto equivalent = path_bytes(directory.path() / ".");
#ifdef _WIN32
    std::replace(equivalent.begin(), equivalent.end(), '\\', '/');
    if (equivalent.size() >= 2 && equivalent[1] == ':' && equivalent[0] >= 'A' && equivalent[0] <= 'Z') {
        equivalent[0] = (char)(equivalent[0] - 'A' + 'a');
    }
#endif
    Wake_protocol protocol;
    initialize(protocol);
    protocol.confirm_session("original-session");
    auto query = request(protocol.inspect("original-session", original).stdin_bytes);
    protocol.receive(info_reply(query, "original-session", equivalent.c_str()));
    require(protocol.ready(), "equivalent native directory spelling was rejected");

    const auto different = directory.path() / "different";
    fs::create_directory(different);
    query = request(protocol.inspect("original-session", original).stdin_bytes);
    protocol.receive(info_reply(query, "original-session", path_bytes(different).c_str()));
    require(!protocol.ready(), "a different native directory was admitted");
}

} // namespace

int main()
{
    try {
        native_queue_and_deduplication();
        permission_requests_remain_interactive();
        startup_and_session_identity();
        roster_rejects_nonresident_or_remote_sessions();
        resident_identity_and_protocol_failures();
        transport_outcomes();
        abandoned_notification_retries();
        retained_notification_capacity();
        native_directory_identity();
        std::cout << "Grok native wake protocol tests passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
