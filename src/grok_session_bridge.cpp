#include "grok_wake_protocol.h"
#include "grok_wake_transport.h"
#include "grok_wake_identity.h"
#include "grok_json.h"
#include <logonomic_cli_addon/session_wake.h>
#include <chrono>
#include <cstdio>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace grok {
namespace addon = logonomic::cli_addon;
namespace {
using Clock = std::chrono::steady_clock;

struct Request
{
    bool notify = false;
    bool inspected = false;
    addon::Session_wake_target target;
    std::string notification_id;
    std::string instruction;
    Clock::time_point deadline;
};

addon::Session_wake_notify_result native_result(const Wake_result& result)
{
    switch (result.outcome) {
    case Wake_outcome::ACCEPTED:
        return {addon::Session_wake_disposition::ACCEPTED, {}, result.notification_id};
    case Wake_outcome::DEFERRED:
        return {addon::Session_wake_disposition::DEFERRED, result.reason, std::nullopt};
    case Wake_outcome::UNCERTAIN:
        return {addon::Session_wake_disposition::UNCERTAIN, result.reason, std::nullopt};
    default:
        return {addon::Session_wake_disposition::UNAVAILABLE, result.reason, std::nullopt};
    }
}
} // namespace

int serve(const std::string& token, const std::vector<std::string>& native_argv)
{
    auto runtime = start_wake_runtime(token, native_argv);
    if (!runtime) {
        // Unsupported native configuration (including a sandbox which cannot
        // use a leader) preserves the launch's original standalone semantics.
        // No private endpoint remains, so the host observes wake unavailable.
        return run_standalone_native(native_argv);
    }
    Wake_protocol protocol;
    Wake_session_identity identity;
    std::optional<Request> request;
    const auto reply = [&](const Json& value) { runtime->reply(addon::serialize_json(value)); };
    const auto unavailable = [&](bool notify, const std::string& reason) {
        if (notify) reply(addon::encode_session_wake_notify_result(
            {addon::Session_wake_disposition::UNAVAILABLE, reason, std::nullopt}));
        else reply(addon::encode_session_wake_readiness({false, reason}));
    };
    const auto consume_results = [&](const Wake_exchange& exchange) {
        for (const auto& result : exchange.results) {
            if (request && request->notify && result.notification_id == request->notification_id) {
                reply(addon::encode_session_wake_notify_result(native_result(result)));
                request.reset();
            }
        }
    };
    const auto send = [&](Wake_exchange exchange) {
        consume_results(exchange);
        if (!exchange.stdin_bytes.empty()) {
            if (exchange.writes_prompt) protocol.prompt_write_started();
            if (!runtime->write_control(exchange.stdin_bytes))
                consume_results(protocol.transport_closed("native_control_write_failed"));
        }
    };
    if (!runtime->write_control(protocol.initialize())) protocol.transport_closed("native_initialize_write_failed");
    for (;;) {
        const auto events = runtime->poll();
        if (events.exit_code) return *events.exit_code;
        if (events.request_disconnected && request) {
            // Once a caller leaves, do not turn a late resident lookup into a
            // fresh submission. A partially written prompt remains uncertain;
            // this connection is never reconnected or silently retried.
            consume_results(protocol.abandon_request("native_request_cancelled"));
            request.reset();
        }
        for (const auto& line : events.control_lines) send(protocol.receive(line));
        if (events.control_closed) consume_results(protocol.transport_closed("native_control_closed"));
        if (events.request) {
            const auto envelope = parse(*events.request);
            const auto operation = envelope ? text(*envelope, "operation") : std::string{};
            const bool notify = operation == "notify";
            if (envelope && operation == "observe" && text(*envelope, "token") == token) {
                const auto before = identity.session_id();
                const auto previous_nonce = identity.transition_nonce();
                identity.observe(member(*envelope, "event"), text(*envelope, "transition_nonce"));
                if (before != identity.session_id() || previous_nonce != identity.transition_nonce()) {
                    send(protocol.forget_session());
                    if (!identity.session_id().empty()) protocol.confirm_session(identity.session_id());
                }
                reply(object({{"observed", Json(true)}}));
            }
            else if (request) {
                if (notify) reply(addon::encode_session_wake_notify_result(
                    {addon::Session_wake_disposition::DEFERRED, "native_notification_pending", std::nullopt}));
                else unavailable(false, "native_control_busy");
            }
            else {
                Request next;
                next.notify = notify;
                next.deadline = Clock::now() + std::chrono::seconds(4);
                bool valid = false;
                if (envelope && notify) {
                    const auto decoded = addon::decode_session_wake_notify_request(member(*envelope, "payload"), nullptr);
                    if (decoded) {
                        next.target = decoded->target;
                        next.notification_id = decoded->notification_id;
                        next.instruction = decoded->instruction;
                        valid = true;
                    }
                }
                else if (envelope && operation == "inspect") {
                    const auto decoded = addon::decode_session_wake_inspect_request(member(*envelope, "payload"), nullptr);
                    if (decoded) { next.target = decoded->target; valid = true; }
                }
                if (!valid || text(next.target.binding, "token") != token ||
                    text(next.target.binding, "transport") != "grok-private-leader-v1") {
                    unavailable(notify, "native_launch_binding_unavailable");
                }
                else if (identity.transition_nonce().empty() ||
                    next.target.correlation.transition_nonce != identity.transition_nonce())
                    unavailable(notify, "native_session_correlation_unavailable");
                else if (!protocol.initialized()) unavailable(notify, "native_connection_unavailable");
                else {
                    request = std::move(next);
                    send(protocol.inspect(request->target.provider_session_id, request->target.working_directory));
                }
            }
        }
        if (request) {
            if (Clock::now() >= request->deadline) {
                consume_results(protocol.abandon_request("native_control_timeout"));
                if (request) { unavailable(request->notify, "native_control_timeout"); request.reset(); }
            }
            else if (!protocol.failure_reason().empty()) {
                unavailable(request->notify, protocol.failure_reason());
                request.reset();
            }
            else if (!request->inspected && protocol.ready()) {
                if (request->notify) {
                    request->inspected = true;
                    send(protocol.notify(request->notification_id, request->instruction));
                }
                else {
                    reply(addon::encode_session_wake_readiness({true, {}}));
                    request.reset();
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
} // namespace grok

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> arguments;
    for (int i = 0; i < argc; ++i) {
        const int length = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (length <= 0) return 2;
        std::string argument(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, argument.data(), length, nullptr, nullptr);
        argument.pop_back();
        arguments.push_back(std::move(argument));
    }
    if (arguments.size() < 4 || arguments[1] != "--serve") return 2;
    try { return grok::serve(arguments[2], std::vector<std::string>(arguments.begin() + 3, arguments.end())); }
    catch (...) { return 3; }
}
#else
int main(int argc, char** argv)
{
    if (argc < 4 || std::string_view(argv[1]) != "--serve") return 2;
    try { return grok::serve(argv[2], std::vector<std::string>(argv + 3, argv + argc)); }
    catch (...) { return 3; }
}
#endif
