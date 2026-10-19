#include <logonomic_cli_addon/helper_client.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json.h>
#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include "grok_wake.h"
#include "grok_wake_transport.h"
#include "grok_json.h"
#include <cstdlib>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
namespace json_util = logonomic::cli_addon::json;

int main()
{
    namespace addon = logonomic::cli_addon;
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) return 1;
#endif
    addon::Contract_failure failure;
    const auto environment = addon::read_helper_environment(&failure);
    if (!environment) return 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1800);
    const auto cancelled = [&] { return std::chrono::steady_clock::now() >= deadline; };
    std::string input;
    for (int ch; (ch = std::fgetc(stdin)) != EOF;) {
        if (input.size() >= 128u * 1024u || cancelled()) return 1;
        input += static_cast<char>(ch);
    }
    addon::Json_parse_limits limits;
    limits.max_bytes = 128u * 1024u;
    limits.max_string_bytes = 128u * 1024u;
    const auto event = addon::parse_json(input, limits, nullptr);
    if (std::ferror(stdin) || !event || !event->is_object()) return 1;
    const auto* wire = event->find("hookEventName");
    const auto* alias = event->find("hook_event_name");
    if (!wire || !wire->is_string() || !alias || !alias->is_string()) return 1;
    std::string name;
    for (const auto& pair : {std::pair{"session_start", "SessionStart"}, {"session_end", "SessionEnd"},
             {"user_prompt_submit", "UserPromptSubmit"}, {"pre_tool_use", "PreToolUse"},
             {"stop", "Stop"}, {"stop_failure", "StopFailure"}, {"stop_cancelled", "StopCancelled"},
             {"permission_denied", "PermissionDenied"}, {"subagent_start", "SubagentStart"},
             {"subagent_stop", "SubagentStop"}}) {
        if (wire->as_string() == pair.first && alias->as_string() == pair.second) name = pair.second;
    }
    if (name.empty()) return 1;
    addon::Helper_client client(*environment, cancelled);
    auto body = addon::Json_value::object();
    for (const auto field : {"hook_event_name", "sessionId", "promptId", "source",
             "subagentType", "subagentId", "phase", "reason"}) {
        if (const auto* value = event->find(field); value && value->is_string() && value->as_string().size() <= 768)
            body.set(field, *value);
    }
    // Prompt gates cannot supply model context in this native CLI. PreToolUse
    // can, and remains its real event name throughout authenticated admission.
    bool title_ok = true;
    if (name == "PreToolUse" && !event->find("subagentType")) {
        const auto instruction = client.request_title_instruction("grok", name, false, addon::Model_context_delivery{});
        title_ok = instruction.completed && instruction.accepted;
        if (title_ok && !instruction.instruction.empty()) {
            auto hook = addon::Json_value::object();
            hook.set("additionalContext", addon::Json_value(instruction.instruction));
            auto response = addon::Json_value::object();
            response.set("hookSpecificOutput", std::move(hook));
            const auto output = addon::serialize_json(response) + '\n';
            if (std::fwrite(output.data(), 1, output.size(), stdout) != output.size() || std::fflush(stdout) != 0) return 1;
        }
    }
    if (name == "PreToolUse") return title_ok ? 0 : 1;
    const auto key = addon::random_nonce_hex();
    const auto nonce = addon::random_nonce_hex();
    auto outcome = client.deliver_provider_event(name, body, key, nonce);
    if (!outcome.committed && !cancelled() &&
        (outcome.exit_code == addon::k_helper_exit_transport_failure || outcome.exit_code == addon::k_helper_exit_pending))
        outcome = client.deliver_provider_event(name, body, key, nonce);
    if (outcome.committed && outcome.accepted && (name == "SessionStart" || name == "SessionEnd")) {
        if (const auto* token = std::getenv(grok::k_wake_token_environment); token && !cancelled()) {
            const auto observation = addon::serialize_json(json_util::object({
                {"operation", addon::Json_value("observe")}, {"token", addon::Json_value(token)},
                {"transition_nonce", addon::Json_value(nonce)}, {"event", body}}));
            // Identity follows authenticated admission and precedes journal
            // acknowledgement. It carries no prompt text or launch authority.
            (void)grok::exchange_wake_request(token, observation, [&](int delay) {
                if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                return cancelled();
            });
        }
    }
    if (outcome.committed) (void)client.acknowledge_provider_event(key);
    return outcome.committed && outcome.accepted ? 0 : 1;
}
