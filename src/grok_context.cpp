#include "grok_context.h"

#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <optional>
#include <string_view>
#include <utility>

namespace grok {
namespace {

namespace addon = logonomic::cli_addon;
namespace json = addon::json;

constexpr std::size_t k_transcript_tail_bytes = 1024 * 1024;
constexpr double k_max_exact_count = 9007199254740991;

std::optional<std::uint64_t> token_count(const addon::Json_value& value)
{
    if (!value.is_number()) {
        return std::nullopt;
    }
    const auto number = value.as_number();
    if (!std::isfinite(number) || number < 0 || number > k_max_exact_count || std::floor(number) != number) {
        return std::nullopt;
    }
    return (std::uint64_t)number;
}

bool valid_identity(const addon::Json_value& value)
{
    return json::bounded_string(value, addon::limits::k_max_lifecycle_identity_bytes) &&
        std::none_of(value.as_string().begin(), value.as_string().end(),
            [](unsigned char byte) { return byte < 0x20; });
}

std::optional<addon::Context_update> transcript_context(
    const addon::Json_value& body, const std::string& session)
{
    const auto& pointer = json::member(body, "transcriptPath");
    if (!json::bounded_string(pointer, 8192)) {
        return std::nullopt;
    }
    const auto path = addon::path_from_utf8(pointer.as_string());
    if (!path.is_absolute() || path.filename() != "updates.jsonl" || path.parent_path().filename() != session) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto end = input.tellg();
    if (end <= 0) {
        return std::nullopt;
    }
    const auto length = std::min<std::streamoff>((std::streamoff)end, k_transcript_tail_bytes);
    input.seekg((std::streamoff)end - length);
    std::string bytes((std::size_t)length, '\0');
    if (!input.read(bytes.data(), length)) {
        return std::nullopt;
    }
    // A tail may begin inside a record; only complete native records are evidence.
    std::size_t offset = (std::streamoff)end > length ? bytes.find('\n') : 0;
    if (offset == std::string::npos) {
        return std::nullopt;
    }
    if ((std::streamoff)end > length) {
        ++offset;
    }
    std::optional<addon::Context_update> latest;
    addon::Json_parse_limits limits;
    limits.max_bytes        = k_transcript_tail_bytes;
    limits.max_string_bytes = k_transcript_tail_bytes;
    while (offset < bytes.size()) {
        const auto newline = bytes.find('\n', offset);
        if (newline == std::string::npos) {
            break;
        }
        const auto line = std::string_view(bytes).substr(offset, newline - offset);
        offset = newline + 1;
        if (line.find_first_not_of(" \t\r") == std::string_view::npos) {
            continue;
        }
        const auto record = addon::parse_json(line, limits, nullptr);
        if (!record || !record->is_object()) {
            latest.reset();
            continue;
        }
        const auto method = json::text(*record, "method");
        const auto& params = json::member(*record, "params");
        if ((method != "session/update" && method != "_x.ai/session/update") ||
            json::text(params, "sessionId") != session)
        {
            return std::nullopt;
        }
        const auto& update = json::member(params, "update");
        const auto kind = json::text(update, "sessionUpdate");
        if (kind == "rewind_marker" || kind == "compaction_checkpoint" || kind == "model_changed") {
            latest.reset();
            continue;
        }
        const auto& meta = json::member(params, "_meta");
        const auto* tokens = meta.find("totalTokens");
        if (method == "_x.ai/session/update" && kind == "auto_compact_completed") {
            tokens = update.find("tokens_after");
        }
        if (!tokens) {
            continue;
        }
        latest.reset();
        const auto count = token_count(*tokens);
        const auto& event_id = json::member(meta, "eventId");
        if (count && valid_identity(event_id)) {
            addon::Context_update observation;
            observation.provider_session_id = session;
            observation.event_id            = event_id.as_string();
            observation.used_tokens         = *count;
            latest = std::move(observation);
        }
    }
    return latest;
}

} // namespace

void add_hook_context(
    const addon::Classify_event_request& request,
    const std::string& provider_session_id,
    addon::Classify_event_result& result)
{
    if (request.context_state_version != 2) {
        return;
    }
    addon::Context_update update;
    update.provider_session_id = provider_session_id;
    update.event_id            = "grok-hook:" + addon::provider_event_evidence_digest_hex(request.event);
    update.usage_unavailable   = true;
    if (request.event.name == "PostCompact") {
        // Grok stamps the envelope once, after installing the replacement.
        // Its native timestamp participates in identity, never history order.
        update.compacted = valid_identity(json::member(request.event.body, "timestamp"));
    }
    else {
        try {
            if (auto snapshot = transcript_context(request.event.body, provider_session_id)) {
                update = std::move(*snapshot);
            }
        }
        catch (const std::exception&) {
            // Optional transcript I/O must preserve the hook's lifecycle facts.
        }
    }
    result.context_updates.push_back(std::move(update));
}

} // namespace grok
