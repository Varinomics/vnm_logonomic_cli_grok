#include "grok_lifecycle.h"

#include <logonomic_cli_addon/limits.h>

#include <array>
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace grok {
namespace {

namespace addon = logonomic::cli_addon;

std::optional<std::string> metadata(const addon::Json_value& body, std::string_view key,
    std::size_t bound = addon::limits::k_max_lifecycle_identity_bytes)
{
    const auto* member = body.find(key);
    if (!member || !member->is_string()) {
        return std::nullopt;
    }
    const auto& value = member->as_string();
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::nullopt;
    }
    const auto result = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    if (result.size() > bound || result.find_first_of(std::string_view("\0\r\n", 3)) != std::string::npos) {
        return std::nullopt;
    }
    return result;
}

} // namespace

std::optional<addon::Classify_event_result> classify_hook(const addon::Classify_event_request& request)
{
    const auto& body = request.event.body;
    const auto name = metadata(body, "hook_event_name");
    if (!name || *name != request.event.name) {
        return std::nullopt;
    }
    addon::Classify_event_result result;
    constexpr std::array<std::string_view, 5> supported{
        "SessionStart", "SessionEnd", "UserPromptSubmit", "StopFailure", "StopCancelled"};
    if (std::find(supported.begin(), supported.end(), *name) == supported.end()) return result;
    if (metadata(body, "subagentType")) return result;
    const auto session = metadata(body, "sessionId");
    if (!session) return std::nullopt;
    addon::Lifecycle_observation observation;
    observation.provider_event_name = *name;
    observation.provider_session_id = *session;
    result.classification = addon::Classification::EVENT;
    if (*name == "SessionStart") {
        observation.session_transition = addon::Session_transition::STARTED;
        observation.session_source = metadata(body, "source").value_or("");
        result.classification = addon::Classification::TRANSITION;
        result.transition = observation.session_source == "resume"
            ? addon::Transition_kind::RESUME : addon::Transition_kind::START;
        auto record = addon::Json_value::object();
        record.set("provider_session_id", addon::Json_value(*session));
        observation.resume_record = std::move(record);
    }
    else if (*name == "SessionEnd") observation.session_transition = addon::Session_transition::ENDED;
    else {
        observation.provider_turn_id = metadata(body, "promptId").value_or("");
        if (*name == "UserPromptSubmit" || *name == "StopFailure" || *name == "StopCancelled") {
            observation.phase_transition = *name == "UserPromptSubmit"
                ? addon::Phase_transition::WORKING : addon::Phase_transition::AWAITING_INPUT;
            // Submit is a gate; another hook may still block it. Stop emits
            // no idle transition because a later gate may keep working.
            observation.phase_evidence = addon::Evidence::INFERRED;
            if (*name == "StopFailure" || *name == "StopCancelled") {
                observation.outcome_transition = *name == "StopFailure"
                    ? addon::Outcome_transition::FAILED : addon::Outcome_transition::INTERRUPTED;
                observation.outcome_evidence = addon::Evidence::CONFIRMED;
            }
        }
    }
    result.observations.push_back(std::move(observation));
    return result;
}

void handle_lifecycle(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_classify_event_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_classify_event_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto result = classify_hook(*request);
    if (!result) {
        context.fail(addon::Result_status::FAILED, "invalid_provider_event");
        return;
    }
    context.succeed(addon::encode_classify_event_result(*result));
}

} // namespace grok
