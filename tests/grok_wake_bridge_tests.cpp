#include "grok_wake_transport.h"
#include "grok_json.h"
#include <logonomic_cli_addon/session_wake.h>
#include <logonomic_cli_addon/ids.h>
#include <iostream>
#include <stdexcept>

namespace json_util = logonomic::cli_addon::json;

namespace grok {
int serve(const std::string& token, const std::vector<std::string>& native_argv);
}

namespace {
namespace addon = logonomic::cli_addon;
using grok::Json;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Scenario
{
    bool timeout;
    bool notify;
    bool submitted;
    bool transition;
    int prompt_count = 0;
    std::vector<Json> replies;
};

Scenario* current_scenario = nullptr;
const std::string k_token(32, 'a');

std::string response(const Json& query, Json result)
{
    return addon::serialize_json(json_util::object({
        {"jsonrpc", Json("2.0")}, {"id", json_util::member(query, "id")}, {"result", std::move(result)}}));
}

std::string roster(const Json& query, const std::string& session)
{
    auto sessions = Json::array();
    sessions.push_back(json_util::object({{"sessionId", Json(session)}, {"cwd", Json("/work")},
        {"resident", Json(true)}, {"activity", Json("idle")},
        {"origin", json_util::object({{"kind", Json("local")}})}}));
    return response(query, json_util::object({{"result", json_util::object({{"sessions", std::move(sessions)}})}}));
}

class Scripted_runtime : public grok::Wake_runtime
{
public:
    explicit Scripted_runtime(Scenario& scenario) : m_scenario(scenario) {}

    bool write_control(std::string_view bytes) override
    {
        const auto query = json_util::parse(bytes);
        require(query.has_value(), "bridge wrote invalid native JSON");
        m_query = *query;
        if (json_util::text(m_query, "method") == "session/prompt") {
            ++m_scenario.prompt_count;
        }
        return true;
    }

    void reply(std::string_view bytes) override
    {
        const auto value = json_util::parse(bytes);
        require(value.has_value(), "bridge replied with invalid JSON");
        m_scenario.replies.push_back(*value);
        if (json_util::text(*value, "reason") == "native_control_timeout") {
            m_phase = 6;
        }
    }

    grok::Wake_poll poll() override
    {
        grok::Wake_poll event;
        switch (m_phase++) {
        case 0:
            event.control_lines.push_back(response(m_query, json_util::object({
                {"protocolVersion", Json(1)}, {"_meta", json_util::object({{"agentInstanceId", Json("leader")}})}})));
            event.request = observation();
            break;
        case 1:
            event.request = host_request(m_scenario.notify);
            break;
        case 2:
            require(json_util::text(m_query, "method") == "_x.ai/sessions/list", "fixture request never reached native inspection");
            if (m_scenario.notify) {
                event.control_lines.push_back(roster(m_query, m_session));
            }
            else {
                m_phase = 4;
            }
            break;
        case 3:
            if (m_scenario.submitted) {
                event.control_lines.push_back(roster(m_query, m_session));
            }
            break;
        case 4:
            m_abandoned_query = m_query;
            if (m_scenario.timeout) {
                m_phase = 4;
            }
            else {
                event.request_disconnected = true;
                m_phase = 6;
            }
            break;
        case 6:
            if (m_scenario.transition) {
                m_session = "next-session";
                m_nonce.assign(32, 'c');
                event.request = observation();
            }
            break;
        case 7:
            event.request = host_request(false);
            break;
        case 8:
            m_fresh_query = m_query;
            if (!m_scenario.submitted) {
                event.control_lines.push_back(roster(m_abandoned_query, "original-session"));
            }
            break;
        case 9:
            if (!m_scenario.submitted) {
                require(m_scenario.replies.back().find("available") == nullptr ||
                    json_util::member(m_scenario.replies.back(), "available") != Json(true),
                    "abandoned roster reply completed the fresh inspection");
                event.control_lines.push_back(roster(m_fresh_query, m_session));
            }
            break;
        default:
            event.exit_code = 0;
            break;
        }
        return event;
    }

private:
    std::string observation() const
    {
        return addon::serialize_json(json_util::object({
            {"operation", Json("observe")}, {"token", Json(k_token)}, {"transition_nonce", Json(m_nonce)},
            {"event", json_util::object({{"hook_event_name", Json("SessionStart")}, {"sessionId", Json(m_session)}})}}));
    }

    std::string host_request(bool notify) const
    {
        addon::Session_wake_target target;
        target.provider_session_id = m_session;
        target.working_directory   = "/work";
        target.launcher_argv       = {"bridge", "--serve", k_token, "grok"};
        target.correlation         = {m_nonce};
        target.launch_environment  = {addon::host_platform_id(), Json::object(), std::nullopt};
        target.binding = json_util::object({{"transport", Json("grok-private-leader-v1")}, {"token", Json(k_token)}});
        const auto payload = notify
            ? addon::encode_session_wake_notify_request({target, "mailbox-17", "Read your mailbox."})
            : addon::encode_session_wake_inspect_request({target});
        addon::Contract_failure failure;
        require(notify ? addon::decode_session_wake_notify_request(payload, &failure).has_value()
                       : addon::decode_session_wake_inspect_request(payload, &failure).has_value(),
            "fixture host request violates the SDK contract");
        return addon::serialize_json(json_util::object({{"operation", Json(notify ? "notify" : "inspect")}, {"payload", payload}}));
    }

    Scenario& m_scenario;
    int m_phase = 0;
    Json m_query;
    Json m_abandoned_query;
    Json m_fresh_query;
    std::string m_session = "original-session";
    std::string m_nonce = std::string(32, 'b');
};
} // namespace

namespace grok {
std::unique_ptr<Wake_runtime> start_wake_runtime(const std::string&, const std::vector<std::string>&)
{
    return std::make_unique<Scripted_runtime>(*current_scenario);
}

int run_standalone_native(const std::vector<std::string>&)
{
    throw std::runtime_error("scripted runtime unexpectedly fell back to standalone");
}
} // namespace grok

int main()
{
    try {
        for (const bool timeout : {false, true}) {
            for (const bool notify : {false, true}) {
                for (const bool transition : {false, true}) {
                    Scenario scenario{timeout, notify, false, transition, 0, {}};
                    current_scenario = &scenario;
                    require(grok::serve(k_token, {"grok"}) == 0, "bridge failed");
                    require(scenario.prompt_count == 0, "abandoned read-only request submitted a prompt");
                    require(!scenario.replies.empty() && json_util::member(scenario.replies.back(), "available") == Json(true),
                        "read-only request interruption permanently disabled the live native connection");
                }
            }
            Scenario submitted{timeout, true, true, false, 0, {}};
            current_scenario = &submitted;
            require(grok::serve(k_token, {"grok"}) == 0, "submitted bridge failed");
            require(submitted.prompt_count == 1, "uncertain prompt was submitted again");
            require(submitted.replies.size() >= 3 && json_util::text(submitted.replies[1], "disposition") == "uncertain",
                "interruption after native write lost its uncertain outcome");
            require(json_util::member(submitted.replies.back(), "available") == Json(false),
                "ambiguous native submission silently reopened the connection");
        }
        std::cout << "Grok bridge request interruption tests passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
