#include "grok_configuration.h"
#include "grok_lifecycle.h"

#include <logonomic_cli_addon/context_state.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/package_source.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace addon = logonomic::cli_addon;
namespace json  = addon::json;
namespace fs    = std::filesystem;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class Transcript
{
public:
    Transcript()
    :
        m_root(fs::temp_directory_path() / ("grok-context-" + addon::random_nonce_hex())),
        m_path(m_root / "native-session" / "updates.jsonl")
    {
        fs::create_directories(m_path.parent_path());
    }

    ~Transcript()
    {
        std::error_code error;
        fs::remove_all(m_root, error);
    }

    void write(const std::string& contents) const
    {
        std::ofstream output(m_path, std::ios::binary | std::ios::trunc);
        output << contents;
        output.close();
        require(!output.fail(), "could not write native transcript fixture");
    }

    std::string path() const { return addon::path_to_utf8(m_path); }

private:
    fs::path m_root;
    fs::path m_path;
};

addon::Classify_event_request hook(const std::string& name, const Transcript& transcript)
{
    addon::Classify_event_request request;
    request.context_state_version = 2;
    request.event.name = name;
    request.event.body = json::object({
        {"hook_event_name", addon::Json_value(name)},
        {"sessionId", addon::Json_value("native-session")},
        {"source", addon::Json_value("manual")},
        {"timestamp", addon::Json_value("2026-10-25T12:11:10.123456700+00:00")},
        {"transcriptPath", addon::Json_value(transcript.path())}});
    return request;
}

addon::Classify_event_result classify(const addon::Classify_event_request& request)
{
    const auto result = grok::classify_hook(request);
    require(result.has_value(), "native hook did not classify");
    const auto decoded = addon::decode_classify_event_result(addon::encode_classify_event_result(*result), nullptr);
    require(decoded && !decoded->context_failure, "context result did not survive the SDK wire contract");
    return *decoded;
}

addon::Context_update context(const addon::Classify_event_request& request)
{
    const auto result = classify(request);
    require(result.context_updates.size() == 1, "negotiated native context update missing");
    return result.context_updates.front();
}

std::string update_record(const std::string& method, const std::string& kind,
    const std::string& event_id, const std::string& meta = "", const std::string& fields = "")
{
    return "{\"timestamp\":1792923070,\"method\":\"" + method +
        "\",\"params\":{\"sessionId\":\"native-session\",\"_meta\":{\"eventId\":\"" + event_id +
        "\"" + meta + "},\"update\":{\"sessionUpdate\":\"" + kind + "\"" + fields + "}}}\n";
}

void native_context_contract()
{
    // Oracle: xai-org/grok-build session/acp_session_impl/updates.rs stamps
    // estimated active context in totalTokens; billing usage is a separate sum.
    Transcript transcript;
    const auto used = update_record("session/update", "agent_message_chunk", "native-event-1",
        ",\"totalTokens\":7000,\"usage\":{\"total_tokens\":88000}");
    transcript.write(used);
    auto request = hook("Stop", transcript);
    const auto measured = context(request);
    require(measured.used_tokens == 7000 && !measured.usage_unavailable,
        "native context must use active tokens rather than billing totals");
    require(!measured.context_window && !measured.used_fraction,
        "tokens-only native evidence must not invent a model capacity");

    addon::Context_state state;
    addon::apply_context_update(state, measured);
    request = hook("PostCompact", transcript);
    const auto compacted = context(request);
    require(compacted.compacted && compacted.usage_unavailable && !compacted.compaction_count &&
        !compacted.compaction_count_revision, "PostCompact is one installed success, not ordered history");
    addon::apply_context_update(state, compacted);
    addon::apply_context_update(state, measured);
    addon::apply_context_update(state, context(request));
    require(state.compaction_count == 1 && state.used_tokens == 7000 && !state.compaction_count_complete,
        "redelivered successful evidence must not recount or erase later occupancy");
    request.event.body.set("timestamp", addon::Json_value("2026-10-25T12:12:11.765432100+00:00"));
    addon::apply_context_update(state, context(request));
    require(state.compaction_count == 2 && !state.used_tokens && !state.compaction_count_complete,
        "a distinct native compaction must clear stale occupancy without claiming complete history");

    request.event.body.set("sessionId", addon::Json_value("resumed-native-session"));
    addon::apply_context_update(state, context(request));
    require(state.compaction_count == 1 && state.compaction_event_ids.size() == 1,
        "a new native session must reset event identity and count");

    request = hook("PostCompact", transcript);
    request.event.body.erase("timestamp");
    require(!context(request).compacted, "an unidentified native hook cannot prove a distinct compaction");

    const auto completed = update_record("_x.ai/session/update", "auto_compact_completed", "native-event-2",
        "", ",\"tokens_before\":7000,\"tokens_after\":1600");
    transcript.write(used + completed);
    request = hook("PostToolUse", transcript);
    const auto after = context(request);
    require(after.used_tokens == 1600 && !after.compacted,
        "persisted completion refreshes occupancy without counting the hook again");

    for (const std::string marker : {"rewind_marker", "compaction_checkpoint", "model_changed"}) {
        transcript.write(used + update_record("_x.ai/session/update", marker, "native-marker"));
        require(context(request).usage_unavailable, "native history boundary must clear older occupancy");
    }
    for (const std::string tokens : {"-1", "1.5", "9007199254740992", "\"7000\""}) {
        transcript.write(update_record("session/update", "agent_message_chunk", "bad-value",
            ",\"totalTokens\":" + tokens));
        require(context(request).usage_unavailable, "unproved native token count must stay unavailable");
    }
    transcript.write(update_record("session/update", "agent_message_chunk", "bad\\u0001identity",
        ",\"totalTokens\":7000"));
    require(context(request).usage_unavailable, "malformed native identity must not poison optional telemetry");

    transcript.write(used + "{malformed record}\n");
    request = hook("UserPromptSubmit", transcript);
    const auto unavailable = classify(request);
    require(unavailable.observations.size() == 1 && unavailable.context_updates.front().usage_unavailable,
        "malformed optional context must preserve native lifecycle facts");
    request.event.body.set("transcriptPath", addon::Json_value(transcript.path() + ".missing"));
    require(classify(request).observations.size() == 1 && context(request).usage_unavailable,
        "missing optional transcript must preserve lifecycle");

    request.context_state_version = 0;
    const auto unrequested = grok::classify_hook(request);
    require(unrequested && !addon::encode_classify_event_result(*unrequested).find("context_updates"),
        "a host that did not request context must retain its response grammar");
    const auto configuration = addon::parse_json(grok::hook_configuration(), {}, nullptr);
    require(configuration.has_value(), "managed hook configuration must parse");
    for (const std::string name : {"PostToolUse", "Stop", "PostCompact"}) {
        require(json::member(*configuration, "hooks").find(name) != nullptr,
            "native context reporting hooks must be installed");
    }
}

} // namespace

int main()
{
    try {
        native_context_contract();
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
