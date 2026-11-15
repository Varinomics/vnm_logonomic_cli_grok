#include <logonomic_cli_addon/context_state.h>
#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace json  = addon::json;

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
    }
    return condition;
}

bool packaged_execution_context(addon::Host_simulator& host, const addon::Json_value& delivery)
{
    const auto& event = json::member(delivery, "event");
    addon::Classify_event_request request;
    request.execution_context_version = 1;
    request.transition_nonce = json::text(delivery, "transition_nonce");
    request.event.name = json::text(event, "name");
    request.event.body = json::member(event, "body");
    const auto transcript = addon::path_from_utf8(json::text(request.event.body, "transcriptPath"));
    // CMake supplies this fixture's exact cwd with forward separators.
    const auto cwd_bytes = (transcript.parent_path() / "nested folder").generic_u8string();
    const std::string cwd(cwd_bytes.begin(), cwd_bytes.end());
    const auto* helpers = std::getenv("LOGONOMIC_PROVIDER_HELPERS_DIR");
    if (!check(helpers && request.event.name == "SessionStart" &&
            json::text(request.event.body, "cwd") == cwd && json::text(request.event.body, "source") == "load",
            "native helper did not preserve the exact SessionStart cwd and load source"))
    {
        return false;
    }
    const auto capabilities = host.live_capabilities();
    if (!check(std::any_of(capabilities.begin(), capabilities.end(), [](const auto& capability) {
            return capability.capability_id == "execution-context" && capability.version == 1;
        }), "packaged adapter did not negotiate execution-context@1"))
    {
        return false;
    }
    auto reservation = addon::Json_value::object();
    reservation.set("transition_nonce", addon::Json_value(request.transition_nonce));
    const auto reserved = host.wait_outcome(host.invoke({"lifecycle-correlation-lifetime", 1},
        "reserve", std::move(reservation), 4000), 4500);
    if (!check(reserved && reserved->status == addon::Host_status::SUCCESS,
            "fixture host did not reserve the SessionStart delivery nonce"))
    {
        return false;
    }
    const auto id = host.invoke({"helper-ingress", 2}, "classify_event",
        addon::encode_classify_event_request(request), 4000);
    const auto outcome = host.wait_outcome(id, 4500);
    const auto result = outcome && outcome->status == addon::Host_status::SUCCESS
        ? addon::decode_classify_event_result(outcome->payload, nullptr) : std::nullopt;
    if (!check(result && result->transition == addon::Transition_kind::RESUME &&
            result->observations.size() == 1 && result->execution_updates.size() == 1 &&
            result->execution_updates.front().cwd == cwd &&
            result->execution_updates.front().transition_nonce == request.transition_nonce,
            "native helper-to-packaged-classifier execution evidence did not survive"))
    {
        return false;
    }
    return true;
}

bool packaged_context(addon::Host_simulator& host, const addon::Json_value& delivery)
{
    const auto& event = json::member(delivery, "event");
    addon::Classify_event_request request;
    request.context_state_version = 2;
    request.transition_nonce = json::text(delivery, "transition_nonce");
    request.event.name = json::text(event, "name");
    request.event.body = json::member(event, "body");
    const auto& body = request.event.body;
    const auto transcript = addon::path_from_utf8(json::text(body, "transcriptPath"));
    const auto* helpers = std::getenv("LOGONOMIC_PROVIDER_HELPERS_DIR");
    if (!check(helpers &&
            (request.event.name == "PostCompact" || request.event.name == "PostToolUse" ||
             request.event.name == "UserPromptSubmit") &&
            json::text(body, "sessionId") == "native-session" &&
            json::text(body, "timestamp") == "2026-10-25T12:11:10.123456700+00:00" &&
            transcript.is_absolute() && transcript.filename() == "updates.jsonl" &&
            transcript.parent_path().filename() == "native-session",
            "native helper did not preserve context evidence"))
    {
        return false;
    }
    const auto capabilities = host.live_capabilities();
    if (!check(std::any_of(capabilities.begin(), capabilities.end(), [](const auto& capability) {
            return capability.capability_id == "context-state" && capability.version == 2;
        }), "packaged adapter did not negotiate context-state@2"))
    {
        return false;
    }
    const auto invoke = [&](addon::Json_value payload) -> std::optional<addon::Json_value> {
        const auto id = host.invoke({"helper-ingress", 2}, "classify_event", std::move(payload), 4000);
        const auto outcome = host.wait_outcome(id, 4500);
        if (!check(outcome && outcome->status == addon::Host_status::SUCCESS,
                "packaged adapter failed the helper classification request"))
        {
            return std::nullopt;
        }
        return outcome->payload;
    };
    const auto payload = addon::encode_classify_event_request(request);
    auto encoded = invoke(payload);
    const auto result = encoded ? addon::decode_classify_event_result(*encoded, nullptr) : std::nullopt;
    if (!check(result && !result->context_failure && result->context_updates.size() == 1,
            "packaged adapter context did not survive decoding"))
    {
        return false;
    }
    const auto& update = result->context_updates.front();
    if (!check(update.provider_session_id == "native-session" &&
            (request.event.name == "PostCompact"
                ? update.compacted && update.usage_unavailable
                : update.used_tokens == 7000 && !update.compacted),
            "packaged adapter did not preserve native context meaning"))
    {
        return false;
    }
    if (request.event.name == "PostCompact") {
        const auto repeated_payload = invoke(payload);
        const auto repeated = repeated_payload
            ? addon::decode_classify_event_result(*repeated_payload, nullptr) : std::nullopt;
        if (!check(repeated && repeated->context_updates.size() == 1 &&
                repeated->context_updates.front().event_id == update.event_id,
                "packaged adapter changed native success identity on redelivery"))
        {
            return false;
        }
        addon::Context_state state;
        addon::apply_context_update(state, update);
        addon::apply_context_update(state, repeated->context_updates.front());
        if (!check(state.compaction_count == 1 && !state.compaction_count_complete,
                "packaged successful event replay changed the incomplete count"))
        {
            return false;
        }
    }
    if (request.event.name == "UserPromptSubmit") {
        // Corruption is injected at the result decoder boundary, after a real
        // packaged lifecycle result; the provider did not emit this corruption.
        encoded->find("context_updates")->as_array().front().set("used_tokens", addon::Json_value("invalid"));
        const auto malformed = addon::decode_classify_event_result(*encoded, nullptr);
        if (!check(malformed && malformed->observations.size() == 1 && malformed->context_failure &&
                malformed->context_failure->provider_session_id == "native-session" &&
                malformed->context_updates.empty(),
                "malformed scoped context discarded the packaged lifecycle result"))
        {
            return false;
        }
        auto unsupported = payload;
        unsupported.set("context_state_version", addon::Json_value("invalid"));
        const auto unsupported_payload = invoke(unsupported);
        const auto diagnostic = unsupported_payload
            ? addon::decode_classify_event_result(*unsupported_payload, nullptr) : std::nullopt;
        if (!check(diagnostic && diagnostic->observations.size() == 1 && diagnostic->context_failure &&
                diagnostic->context_failure->provider_session_id == "native-session" &&
                diagnostic->context_failure->failure.reason_code == "unsupported_version" &&
                diagnostic->context_updates.empty(),
                "unsupported context metadata lost its native lifecycle or diagnostic"))
        {
            return false;
        }
        request.context_state_version = 0;
        const auto unrequested = invoke(addon::encode_classify_event_request(request));
        if (!check(unrequested && !unrequested->find("context_updates"),
                "unrequested context changed the packaged response grammar"))
        {
            return false;
        }
    }
    return true;
}

bool verify_deliveries(const char* path)
{
    std::ifstream input(addon::path_from_utf8(path));
    std::vector<addon::Json_value> deliveries;
    for (std::string line; std::getline(input, line);) {
        const auto delivery = addon::parse_json(line, {}, nullptr);
        if (!check(delivery.has_value(), "invalid helper receipt")) return false;
        deliveries.push_back(*delivery);
    }
    if (!check(input.eof() && deliveries.size() == 4,
            "native helper deliveries did not reach the fixture exactly once"))
    {
        return false;
    }
    const auto* helpers = std::getenv("LOGONOMIC_PROVIDER_HELPERS_DIR");
    if (!check(helpers != nullptr, "missing packaged helpers directory")) return false;
    addon::Simulator_options options;
    options.package_directory = addon::path_from_utf8(helpers).parent_path();
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    if (!check(host.start(&failure), failure.detail.c_str())) return false;
    const char* expected_names[] = {"PostCompact", "PostToolUse", "UserPromptSubmit", "SessionStart"};
    for (std::size_t i = 0; i < deliveries.size(); ++i) {
        const auto& delivery = deliveries[i];
        const auto name = json::text(json::member(delivery, "event"), "name");
        if (!check(name == expected_names[i], "native helper delivery order changed") ||
            !(name == "SessionStart" ? packaged_execution_context(host, delivery) : packaged_context(host, delivery)))
        {
            return false;
        }
    }
    int exit_code = -1;
    return check(host.shutdown(1000, &exit_code) && exit_code == 0,
        "packaged adapter did not shut down cleanly");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--verify-deliveries") {
        return verify_deliveries(argv[2]) ? 0 : 1;
    }
    const bool provider_event = argc == 4 && std::string(argv[1]) == "provider-event";
    if (provider_event && std::string(argv[2]) == "acknowledge") {
        return 0;
    }
    const bool title_instruction = argc == 3 && std::string(argv[1]) == "request-title-instruction" &&
        std::string(argv[2]) == "--args-stdin";
    if (!title_instruction &&
        !(provider_event && std::string(argv[2]) == "deliver" && std::string(argv[3]) == "--args-stdin"))
    {
        return 4;
    }
    std::string input;
    for (int ch; (ch = std::fgetc(stdin)) != EOF;) {
        if (input.size() > 16384) return 2;
        input += static_cast<char>(ch);
    }
    const auto value = addon::parse_json(input, {}, nullptr);
    if (provider_event) {
        // Preserve exact helper bytes inside its production deadline. Package
        // validation and classifier assertions run once in --verify-deliveries,
        // outside the timed wrapper just as in a persistent host generation.
        const auto* receipt = std::getenv("GROK_HELPER_TEST_RECEIPTS");
        if (!value || !receipt) return 2;
        std::ofstream output(addon::path_from_utf8(receipt), std::ios::app);
        output << input << '\n';
        output.close();
        if (!check(output.good(), "helper receipt write failed")) return 2;
        std::fputs("{\"accepted\":true}\n", stdout);
        return std::fflush(stdout) == 0 ? 0 : 1;
    }
    // Lifecycle is deliberately unavailable for PreToolUse. A title-capable
    // native gate must not depend on this unrelated lane completing.
    if (!value || !value->find("source") || *value->find("source") != addon::Json_value("grok") ||
        !value->find("hook_event_name") || *value->find("hook_event_name") != addon::Json_value("PreToolUse") ||
        !value->find("delivery") || !value->find("delivery")->find("kind") ||
        *value->find("delivery")->find("kind") != addon::Json_value("model-context")) return 2;
    std::fputs("{\"accepted\":true,\"instruction_required\":true,\"instruction\":\"Set the title using the authenticated host instruction.\"}\n", stdout);
    return std::fflush(stdout) == 0 ? 0 : 1;
}
