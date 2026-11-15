#include "grok_lifecycle.h"
#include "grok_context.h"
#include "grok_json.h"
#include "grok_observer.h"

#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/provider_event_diagnostic.h>

#include <array>
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace grok {
namespace {

namespace addon = logonomic::cli_addon;
namespace json = addon::json;

std::optional<std::string> metadata(
    const addon::Json_value& body,
    std::string_view key,
    std::size_t bound = addon::limits::k_max_lifecycle_identity_bytes,
    addon::Contract_failure* failure = nullptr,
    std::string_view event_name = {})
{
    const auto* member = body.find(key);
    const auto reject = [&](addon::Provider_event_problem problem) {
        return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
            std::string(event_name), std::string(key), problem, member, bound, "nonempty_single_line_identity"));
    };
    if (!member) {
        return reject(addon::Provider_event_problem::MISSING);
    }
    if (!member->is_string()) {
        return reject(addon::Provider_event_problem::WRONG_TYPE);
    }
    const auto& value = member->as_string();
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return reject(addon::Provider_event_problem::EMPTY);
    }
    const auto result = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    if (result.size() > bound) {
        auto diagnostic = addon::make_provider_event_diagnostic(
            std::string(event_name), std::string(key), addon::Provider_event_problem::TOO_LARGE,
            member, bound, "nonempty_single_line_identity");
        diagnostic.actual_size = result.size();
        return addon::reject_provider_event(failure, std::move(diagnostic));
    }
    if (result.find_first_of(std::string_view("\0\r\n", 3)) != std::string::npos) {
        return reject(addon::Provider_event_problem::CONTROL_CHARACTER);
    }
    return result;
}

std::optional<std::string> interaction_kind(const addon::Json_value& body)
{
    const auto kind = json::text(json::member(body, "update"), "kind");
    if (kind == "permission" || kind == "plan_approval") return "permission";
    if (kind == "question" || kind == "mcp_elicitation") return "interactive_question";
    return std::nullopt;
}

std::optional<addon::Classify_event_result> classify_interaction(
    const addon::Classify_event_request& request, addon::Contract_failure* failure)
{
    const auto& body = request.event.body;
    const auto& update = json::member(body, "update");
    const auto session = json::text(body, "sessionId");
    const auto call_id = json::text(update, "tool_call_id");
    const auto* request_id = body.find("request_id");
    const auto id = interaction_request_identity(json::member(body, "request_id"));
    const auto pending = request.event.name == "pending_interaction";
    const auto valid_identity = [](const std::string& value) {
        return !value.empty() && value.size() <= addon::limits::k_max_lifecycle_identity_bytes &&
            value.find_first_of(std::string_view("\0\r\n", 3)) == std::string::npos;
    };
    const auto reject_identity = [
            &request,
            failure
        ](
            std::string field,
            const addon::Json_value* actual)
        {
            const auto problem = !actual ? addon::Provider_event_problem::MISSING :
                !actual->is_string() ? addon::Provider_event_problem::WRONG_TYPE :
                actual->as_string().empty() ? addon::Provider_event_problem::EMPTY :
                actual->as_string().size() > addon::limits::k_max_lifecycle_identity_bytes
                    ? addon::Provider_event_problem::TOO_LARGE : addon::Provider_event_problem::CONTROL_CHARACTER;
            return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
                request.event.name, std::move(field), problem, actual,
                addon::limits::k_max_lifecycle_identity_bytes, "native_interaction_identity"));
        };
    if (!valid_identity(session)) {
        return reject_identity("sessionId", body.find("sessionId"));
    }
    if (!valid_identity(call_id)) {
        return reject_identity("update.tool_call_id", update.find("tool_call_id"));
    }
    if (!id) {
        const auto problem = !request_id ? addon::Provider_event_problem::MISSING :
            !request_id->is_integer() && !request_id->is_string() ? addon::Provider_event_problem::WRONG_TYPE :
            request_id->is_string() && request_id->as_string().empty() ? addon::Provider_event_problem::EMPTY :
            request_id->is_string() && request_id->as_string().find('\0') != std::string::npos
                ? addon::Provider_event_problem::CONTROL_CHARACTER : addon::Provider_event_problem::TOO_LARGE;
        auto diagnostic = addon::make_provider_event_diagnostic(
            request.event.name, "request_id", problem, request_id,
            addon::limits::k_max_lifecycle_identity_bytes, "serialized_rpc_identity");
        if (request_id && request_id->is_string() && problem == addon::Provider_event_problem::TOO_LARGE) {
            diagnostic.actual_size = addon::serialize_json(*request_id).size();
        }
        return addon::reject_provider_event(failure, std::move(diagnostic));
    }
    if (json::text(update, "sessionUpdate") != request.event.name) {
        return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
            request.event.name, "update.sessionUpdate", addon::Provider_event_problem::MISMATCH,
            update.find("sessionUpdate"), {}, "matches_event_name"));
    }
    if (pending && !interaction_kind(body)) {
        const auto* kind = update.find("kind");
        return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
            request.event.name, "update.kind", !kind ? addon::Provider_event_problem::MISSING :
                !kind->is_string() ? addon::Provider_event_problem::WRONG_TYPE : addon::Provider_event_problem::INVALID_VALUE,
            kind, {}, "supported_interaction_kind"));
    }
    addon::Classify_event_result result;
    if (!request.ledger.current_transition || request.ledger.current_transition->provider_session_id != session ||
        request.ledger.current_transition->nonce != request.transition_nonce) return result;
    addon::Lifecycle_observation observation;
    observation.provider_event_name = request.event.name;
    observation.provider_session_id = session;
    observation.attention_request_id = *id;
    observation.attention_transition = pending ? addon::Attention_transition::REQUESTED : addon::Attention_transition::RESOLVED;
    observation.attention_evidence = addon::Evidence::CONFIRMED;
    result.classification = addon::Classification::EVENT;
    result.observations.push_back(std::move(observation));
    return result;
}

} // namespace

std::optional<std::string> interaction_request_identity(const addon::Json_value& request_id)
{
    if (!request_id.is_integer() && !json::bounded_string(request_id, addon::limits::k_max_lifecycle_identity_bytes)) {
        return std::nullopt;
    }
    // JSON encoding preserves the native RPC identity's string/integer type.
    // A tool-call ID alone can name several successive human waits.
    const auto identity = addon::serialize_json(request_id);
    if (identity.size() > addon::limits::k_max_lifecycle_identity_bytes) {
        return std::nullopt;
    }
    return identity;
}

std::optional<addon::Classify_event_result> classify_hook(
    const addon::Classify_event_request& request, addon::Contract_failure* failure)
{
    if (request.event.name == "pending_interaction" || request.event.name == "interaction_resolved") {
        return classify_interaction(request, failure);
    }
    const auto& body = request.event.body;
    const auto name = metadata(body, "hook_event_name", addon::limits::k_max_lifecycle_identity_bytes,
        failure, request.event.name);
    if (!name) {
        return std::nullopt;
    }
    if (*name != request.event.name) {
        return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
            request.event.name, "hook_event_name", addon::Provider_event_problem::MISMATCH,
            body.find("hook_event_name"), {}, "matches_event_name"));
    }
    addon::Classify_event_result result;
    constexpr std::array<std::string_view, 8> supported{
        "SessionStart", "SessionEnd", "UserPromptSubmit", "StopFailure", "StopCancelled",
        "Stop", "PostToolUse", "PostCompact"};
    if (std::find(supported.begin(), supported.end(), *name) == supported.end()) return result;
    if (metadata(body, "subagentType")) return result;
    const auto session = metadata(body, "sessionId", addon::limits::k_max_lifecycle_identity_bytes,
        failure, request.event.name);
    if (!session) return std::nullopt;
    if (request.context_failure) {
        result.context_failure = addon::Context_failure{*session, *request.context_failure};
    }
    result.execution_failure = request.execution_failure;
    add_hook_context(request, *session, result);
    if (*name == "Stop" || *name == "PostToolUse" || *name == "PostCompact") {
        return result;
    }
    addon::Lifecycle_observation observation;
    observation.provider_event_name = *name;
    observation.provider_session_id = *session;
    result.classification = addon::Classification::EVENT;
    if (*name == "SessionStart") {
        observation.session_transition = addon::Session_transition::STARTED;
        observation.session_source = metadata(body, "source").value_or("");
        result.classification = addon::Classification::TRANSITION;
        result.transition = observation.session_source == "load"
            ? addon::Transition_kind::RESUME : addon::Transition_kind::START;
        auto record = addon::Json_value::object();
        record.set("provider_session_id", addon::Json_value(*session));
        observation.resume_record = std::move(record);
        const auto* cwd = body.find("cwd");
        if (request.execution_context_version == 1 && !request.execution_failure &&
            !body.find("subagentType") && !body.find("subagentId") && cwd && cwd->is_string())
        {
            // The native envelope uses this actor's session_info.cwd. Capture
            // it with this SessionStart nonce, never the ledger's prior nonce.
            addon::Execution_context_update update;
            update.provider_session_id = *session;
            update.transition_nonce    = request.transition_nonce;
            update.status              = addon::Execution_context_status::CONFIRMED;
            update.environment         = "local";
            update.cwd                 = cwd->as_string();
            update.source              = "SessionStart.cwd";
            if (addon::is_valid_execution_context_update(update)) {
                result.execution_updates.push_back(std::move(update));
            }
        }
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

void handle_lifecycle(addon::Invocation_context& context, Lifecycle_observer* observer)
{
    if (context.request().operation != addon::k_classify_event_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const bool typed = context.request().capability.capability_id == addon::k_attention_kind_capability_id;
    const auto attention_request = typed
        ? addon::decode_attention_classification_request(context.request().payload, &failure) : std::nullopt;
    const auto request = typed
        ? (attention_request ? std::optional(attention_request->base) : std::nullopt)
        : addon::decode_classify_event_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto result = observer ? observer->classify(*request, &failure) : classify_hook(*request, &failure);
    if (!result) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    if (typed) {
        addon::Attention_classification_result attention{*result, std::nullopt};
        if (request->event.name == "pending_interaction" && result->observations.size() == 1) {
            attention.attention_kind = addon::Attention_kind{0, *interaction_kind(request->event.body)};
        }
        context.succeed(addon::encode_attention_classification_result(attention));
    }
    else context.succeed(addon::encode_classify_event_result(*result));
}

} // namespace grok
