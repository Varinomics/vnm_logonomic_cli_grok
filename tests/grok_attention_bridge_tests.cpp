#include "grok_json.h"
#include "grok_wake_transport.h"

#include <logonomic_cli_addon/helper_client.h>
#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_wake.h>
#include <logonomic_cli_addon/sidecar.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace grok {
int serve(const std::string& token, const std::vector<std::string>& native_argv);
}

namespace {
namespace addon = logonomic::cli_addon;
namespace json = addon::json;
using grok::Json;

const std::string k_token(32, 'a');
const std::string k_nonce(32, 'b');
std::vector<std::string> frames;
int native_writes = 0;
bool resume = false;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void environment(const char* name, const std::string& value)
{
#ifdef _WIN32
    require(_putenv_s(name, value.c_str()) == 0, "fixture environment failed");
#else
    require(setenv(name, value.c_str(), 1) == 0, "fixture environment failed");
#endif
}

std::string response(const Json& query, Json result)
{
    return addon::serialize_json(json::object({
        {"jsonrpc", Json("2.0")}, {"id", json::member(query, "id")}, {"result", std::move(result)}}));
}

std::string observation(const std::string& nonce)
{
    return addon::serialize_json(json::object({
        {"operation", Json("observe")}, {"token", Json(k_token)}, {"transition_nonce", Json(nonce)},
        {"event", json::object({{"hook_event_name", Json("SessionStart")}, {"sessionId", Json("native-session")}})}}));
}

class Scripted_runtime : public grok::Wake_runtime
{
public:
    bool write_control(std::string_view bytes) override
    {
        const auto query = json::parse(bytes);
        require(query.has_value(), "bridge wrote invalid JSON");
        m_query = *query;
        const auto method = json::text(m_query, "method");
        require(method == "initialize" || method == "_x.ai/sessions/list" || method == "session/prompt",
            "attention observation wrote a native request or reverse response");
        if (method == "_x.ai/sessions/list") {
            require(json::member(m_query, "params") == Json::object(), "roster inspection subscribed the observer");
        }
        ++native_writes;
        return true;
    }

    void reply(std::string_view bytes) override
    {
        const auto value = json::parse(bytes);
        require(value && (json::member(*value, "observed") == Json(true) ||
            json::text(*value, "disposition") == "accepted"), "existing wake/identity operation failed");
    }

    grok::Wake_poll poll() override
    {
        grok::Wake_poll event;
        switch (m_phase++) {
        case 0:
            event.control_lines.push_back(response(m_query, json::object({
                {"protocolVersion", Json(1)}, {"_meta", json::object({{"agentInstanceId", Json("native-leader")}})}})));
            event.request = observation(k_nonce);
            break;
        case 1: {
            // No native subscription exists yet. Even a well-formed interaction
            // frame must not establish attention before the legitimate wake.
            event.control_lines.push_back(frames.front());
            addon::Session_wake_target target;
            target.provider_session_id = "native-session";
            target.working_directory = "/work";
            target.launcher_argv = {"bridge", "--serve", k_token, "grok"};
            target.correlation = {k_nonce};
            target.launch_environment = {addon::host_platform_id(), Json::object(), std::nullopt};
            target.binding = json::object({{"transport", Json("grok-private-leader-v1")}, {"token", Json(k_token)}});
            event.request = addon::serialize_json(json::object({{"operation", Json("notify")},
                {"payload", addon::encode_session_wake_notify_request({target, "mailbox-17", "Read your mailbox."})}}));
            break;
        }
        case 2:
        case 3: {
            require(json::text(m_query, "method") == "_x.ai/sessions/list", "wake skipped its native inspection");
            auto sessions = Json::array();
            sessions.push_back(json::object({{"sessionId", Json("native-session")}, {"cwd", Json("/work")},
                {"resident", Json(true)}, {"activity", Json("idle")},
                {"origin", json::object({{"kind", Json("local")}})}}));
            event.control_lines.push_back(response(m_query,
                json::object({{"result", json::object({{"sessions", std::move(sessions)}})}})));
            break;
        }
        case 4: {
            require(json::text(m_query, "method") == "session/prompt", "fixture never reached the subscribed native channel");
            event.control_lines.push_back(R"({"jsonrpc":"2.0","method":"_x.ai/queue/changed","params":{"sessionId":"native-session","runningPromptId":"mailbox-17"}})");
            event.control_lines.insert(event.control_lines.end(), frames.begin(), frames.begin() + 8);
            event.control_lines.push_back(frames.front());
            event.control_lines.push_back(frames[1]); // Repeated native frames are idempotent.
            auto child = *json::parse(frames.front());
            child.find("params")->find("params")->set("sessionId", Json("child-session"));
            event.control_lines.push_back(addon::serialize_json(child));
            auto invalid = *json::parse(frames.front());
            invalid.find("params")->find("params")->find("update")->set("tool_call_id", Json("invalid\nidentity"));
            event.control_lines.push_back(addon::serialize_json(invalid));
            event.control_lines.push_back(R"({"jsonrpc":"2.0","id":"native-reverse-1","method":"_x.ai/ask_user_question","params":{"method":"x.ai/ask_user_question","params":{"sessionId":"native-session","tool_call_id":"native-reverse-1"}}})");
            break;
        }
        case 5:
            event.control_lines.insert(event.control_lines.end(), frames.begin() + 8, frames.end());
            event.control_lines.push_back(frames.back());
            break;
        case 6: {
            auto pending = *json::parse(frames[2]);
            pending.find("params")->find("params")->find("update")->set("tool_call_id", Json("left-open"));
            event.control_lines.push_back(addon::serialize_json(pending));
            auto waiting = *json::parse(frames[3]);
            waiting.set("id", Json(37));
            waiting.find("params")->find("params")->set("toolCallId", Json("left-open"));
            event.control_lines.push_back(addon::serialize_json(waiting));
            break;
        }
        case 7:
            if (resume) event.request = observation(std::string(32, 'c'));
            else event.control_closed = true;
            break;
        case 8:
            // Neither loss nor a new epoch is an exact native resolution.
            // Late old-channel traffic cannot establish a new epoch's pending set.
            event.control_lines.push_back(frames.front());
            event.control_lines.push_back(frames.back());
            break;
        default:
            event.exit_code = 0;
            break;
        }
        return event;
    }

private:
    int m_phase = 0;
    Json m_query;
};

int wrapper(int argc, char** argv)
{
    require(argc == 4 && std::string_view(argv[1]) == "provider-event", "unexpected helper operation");
    if (std::string_view(argv[2]) == "acknowledge") return 0;
    require(std::string_view(argv[2]) == "deliver" && std::string_view(argv[3]) == "--args-stdin", "unexpected delivery arguments");
    std::string input;
    for (int ch; (ch = std::fgetc(stdin)) != EOF;) {
        require(input.size() < 16384, "delivery exceeded fixture input bound");
        input += static_cast<char>(ch);
    }
    const auto delivery = json::parse(input);
    require(delivery.has_value(), "helper delivery was not JSON");
    addon::Classify_event_request request;
    request.event.name = json::text(json::member(*delivery, "event"), "name");
    request.event.body = json::member(json::member(*delivery, "event"), "body");
    request.transition_nonce = json::text(*delivery, "transition_nonce");
    request.ledger.current_epoch = 1;
    request.ledger.current_transition = addon::Ledger_transition{addon::Transition_kind::START, "native-session", k_nonce};
    const auto& update = json::member(request.event.body, "update");
    const auto pending = request.event.name == "pending_interaction";
    require((pending || request.event.name == "interaction_resolved") &&
        json::text(update, "sessionUpdate") == request.event.name &&
        json::text(request.event.body, "sessionId") == "native-session",
        "bridge changed native interaction identity");

    addon::Simulator_options options;
    options.package_directory = addon::path_from_utf8(std::getenv(addon::k_helper_env_helpers_dir)).parent_path();
    addon::Host_simulator host(options, nullptr);
    require(host.start(nullptr), "packaged adapter did not start");
    const auto invoke = [&](const char* capability, Json payload) {
        const std::uint16_t version = std::string_view(capability) == "helper-ingress" ? 2 : 1;
        const auto id = host.invoke({capability, version},
            "classify_event", std::move(payload), 4000);
        const auto outcome = host.wait_outcome(id, 4500);
        require(outcome && outcome->status == addon::Host_status::SUCCESS, "packaged native attention classification failed");
        return outcome->payload;
    };
    const auto base = addon::decode_classify_event_result(invoke("helper-ingress", addon::encode_classify_event_request(request)), nullptr);
    const auto typed = addon::decode_attention_classification_result(invoke("attention-kind",
        addon::encode_attention_classification_request({request, std::nullopt})), nullptr);
    require(base && typed && base->observations.size() == 1 &&
        addon::encode_classify_event_result(*base) == addon::encode_classify_event_result(typed->base),
        "native attention did not survive the base and optional companion decoders");
    const auto& native_request_id = json::member(request.event.body, "request_id");
    require(native_request_id.is_integer(), "bridge omitted the native reverse-request identity");
    const auto& fact = base->observations.front();
    require(fact.provider_session_id == "native-session" && fact.attention_request_id == addon::serialize_json(native_request_id) &&
        fact.provider_event_name == request.event.name && fact.attention_evidence == addon::Evidence::CONFIRMED &&
        fact.attention_transition == (pending ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED) &&
        fact.provider_turn_id.empty() && fact.provider_event_id.empty() && fact.provider_sequence.empty() && fact.agent_id.empty() &&
        fact.phase_transition == addon::Phase_transition::NONE && fact.outcome_transition == addon::Outcome_transition::NONE,
        "native attention lost identity or fabricated unrelated lifecycle evidence");
    const auto kind = json::text(update, "kind");
    require(pending ? typed->attention_kind && typed->attention_kind->observation_index == 0 &&
        typed->attention_kind->kind == (kind == "permission" || kind == "plan_approval" ? "permission" : "interactive_question")
        : !typed->attention_kind, "native interaction kind was misclassified");
    const addon::Directory_package_source source(options.package_directory);
    const auto sidecar_bytes = source.read("capabilities/lifecycle-events@2.json", 16384, nullptr);
    const auto sidecar = sidecar_bytes ? addon::parse_sidecar(*sidecar_bytes, nullptr) : std::nullopt;
    const auto declaration = sidecar ? addon::decode_lifecycle_declaration(sidecar->payload, nullptr) : std::nullopt;
    require(declaration && addon::lifecycle_profile_admits(declaration->features, fact),
        "the packaged feature declaration rejects native attention");
    if (native_request_id == Json(31) && pending) {
        request.ledger.current_transition->nonce.assign(32, 'c');
        const auto stale = addon::decode_classify_event_result(invoke("helper-ingress", addon::encode_classify_event_request(request)), nullptr);
        require(stale && stale->classification == addon::Classification::IGNORED && stale->observations.empty(),
            "a delayed delivery crossed the host's transition epoch");
    }
    std::ofstream log(addon::path_from_utf8(std::getenv("GROK_ATTENTION_TEST_LOG")), std::ios::app);
    log << addon::serialize_json(addon::encode_attention_classification_result(*typed)) << '\n';
    require(log.good(), "fixture receipt write failed");
    int exit_code = -1;
    require(host.shutdown(1000, &exit_code) && exit_code == 0, "packaged adapter did not shut down");
    std::fputs("{\"accepted\":true}\n", stdout);
    return std::fflush(stdout) == 0 ? 0 : 1;
}
} // namespace

namespace grok {
std::unique_ptr<Wake_runtime> start_wake_runtime(const std::string&, const std::vector<std::string>&)
{
    return std::make_unique<Scripted_runtime>();
}
int run_standalone_native(const std::vector<std::string>&)
{
    throw std::runtime_error("scripted attention runtime unexpectedly fell back to standalone");
}
} // namespace grok

int main(int argc, char** argv)
{
    try {
        if (argc >= 2 && std::string_view(argv[1]) == "provider-event") return wrapper(argc, argv);
        require(argc == 4, "usage: grok_attention_bridge_tests <package> <native fixture> <receipt>");
        // Source-derived wire fixture: Grok 2bdd1d6's PendingInteractionGuard,
        // SessionNotification and reverse-request serde shapes, and the leader's
        // wrapped gateway form. This is not live native qualification.
        std::ifstream fixture(addon::path_from_utf8(argv[2]));
        for (std::string line; std::getline(fixture, line);) frames.push_back(line);
        require(frames.size() == 20, "native interaction fixture is incomplete");
        const auto package = addon::path_from_utf8(argv[1]);
        environment(addon::k_helper_env_vnmctl_path, addon::path_to_utf8(std::filesystem::absolute(addon::path_from_utf8(argv[0]))));
        environment(addon::k_helper_env_session_capability, std::string(64, 'a'));
        environment(addon::k_helper_env_package_id, "logonomic.cli.grok");
        environment(addon::k_helper_env_generation_digest, std::string(64, 'b'));
        environment(addon::k_helper_env_generation_nonce, std::string(32, 'c'));
        environment(addon::k_helper_env_helpers_dir, addon::path_to_utf8(package / "helpers"));
        environment("GROK_ATTENTION_TEST_LOG", argv[3]);
        for (const bool resumed : {false, true}) {
            resume = resumed;
            native_writes = 0;
            { std::ofstream reset(addon::path_from_utf8(argv[3])); }
            require(grok::serve(k_token, {"grok"}) == 0, "production bridge failed");
            require(native_writes == 4, "attention observation changed native subscription or response behavior");
            std::ifstream receipt(addon::path_from_utf8(argv[3]));
            std::vector<addon::Attention_classification_result> observed;
            for (std::string line; std::getline(receipt, line);) {
                const auto value = json::parse(line);
                const auto result = value ? addon::decode_attention_classification_result(*value, nullptr) : std::nullopt;
                require(result.has_value(), "fixture receipt is invalid");
                observed.push_back(*result);
            }
            const std::vector<int> expected_ids{31, 32, 33, 34, 32, 31, 35, 35, 36, 36, 34, 33, 37};
            require(observed.size() == expected_ids.size(),
                "native waits and resolutions did not reach the helper lane exactly once");
            for (std::size_t i = 0; i < expected_ids.size(); ++i) {
                require(observed[i].base.observations.front().attention_request_id == std::to_string(expected_ids[i]),
                    "a native wait reused another occurrence's attention identity");
            }
            require(observed.back().base.observations.front().attention_transition == addon::Attention_transition::REQUESTED,
                "transport or epoch loss fabricated an exact prompt resolution");
        }
        std::cout << "Grok native interaction bridge tests passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
