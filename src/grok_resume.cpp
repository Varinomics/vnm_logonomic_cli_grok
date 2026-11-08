#include "grok_resume.h"
#include "grok_json.h"

#include <logonomic_cli_addon/execution_context.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/usage_temporal.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace grok {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs    = std::filesystem;
namespace json  = addon::json;

constexpr std::size_t k_max_buckets       = 4096;
constexpr std::size_t k_max_summary_bytes = 1024 * 1024;
constexpr std::size_t k_max_history_bytes = 16 * 1024 * 1024;
constexpr std::size_t k_max_history_lines = 65536;

struct file_stamp_t
{
    bool missing = false;
    std::uintmax_t bytes = 0;
    fs::file_time_type modified{};

    bool operator==(const file_stamp_t&) const = default;
};

std::optional<file_stamp_t> file_stamp(const fs::path& path)
{
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found) {
        return file_stamp_t{true};
    }
    if (error || !fs::is_regular_file(status)) {
        return std::nullopt;
    }
    const auto bytes = fs::file_size(path, error);
    if (error) {
        return std::nullopt;
    }
    const auto modified = fs::last_write_time(path, error);
    if (error) {
        return std::nullopt;
    }
    return file_stamp_t{false, bytes, modified};
}

std::optional<std::string> read_file(
    const fs::path& path,
    const file_stamp_t& stamp,
    std::size_t limit,
    addon::Invocation_context& context)
{
    if (stamp.missing || stamp.bytes > limit || context.cancel_requested()) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string bytes;
    std::array<char, 16384> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
        const auto count = (std::size_t)input.gcount();
        if (context.cancel_requested() || count > limit - bytes.size()) {
            return std::nullopt;
        }
        bytes.append(buffer.data(), count);
    }
    if (input.bad() || bytes.size() != stamp.bytes || file_stamp(path) != stamp) {
        return std::nullopt;
    }
    return bytes;
}

std::optional<Json> parse_document(std::string_view bytes, std::size_t limit)
{
    addon::Json_parse_limits limits;
    limits.max_bytes        = limit;
    limits.max_string_bytes = limit;
    return addon::parse_json(bytes, limits, nullptr);
}

int hex_digit(char byte)
{
    if (byte >= '0' && byte <= '9') { return byte - '0';      }
    if (byte >= 'a' && byte <= 'f') { return byte - 'a' + 10; }
    if (byte >= 'A' && byte <= 'F') { return byte - 'A' + 10; }
    return -1;
}

std::optional<std::string> bucket_cwd(const fs::path& bucket)
{
    const auto name = bucket.filename().u8string();
    std::string decoded;
    for (std::size_t index = 0; index < name.size(); ++index) {
        if (name[index] != '%') {
            decoded += (char)name[index];
            continue;
        }
        if (index + 2 >= name.size()) {
            return std::nullopt;
        }
        const int high = hex_digit((char)name[index + 1]);
        const int low  = hex_digit((char)name[index + 2]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        decoded += (char)((high << 4) | low);
        index += 2;
    }
    if (!addon::is_execution_local_path(decoded)) {
        return std::nullopt;
    }
    constexpr char digits[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char byte : decoded) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '~')
        {
            encoded += (char)byte;
        }
        else {
            encoded += '%';
            encoded += digits[byte >> 4];
            encoded += digits[byte & 15];
        }
    }
    // Native load recomputes this location from cwd. A decoded marker alone
    // cannot prove that a slug/hash bucket is the path the loader will open.
    if (encoded.size() > 255) {
        return std::nullopt;
    }
    std::error_code error;
    if (!fs::equivalent(bucket, bucket.parent_path() / addon::path_from_utf8(encoded), error) || error) {
        return std::nullopt;
    }
    return decoded;
}

std::optional<fs::path> exact_session_directory(
    const fs::path& profile,
    std::string_view session_id,
    addon::Invocation_context& context)
{
    std::error_code error;
    fs::directory_iterator entry(profile / "sessions", error);
    if (error) {
        return std::nullopt;
    }
    std::optional<fs::path> selected;
    std::size_t count = 0;
    for (; entry != fs::directory_iterator(); entry.increment(error)) {
        if (error || ++count > k_max_buckets || context.cancel_requested()) {
            return std::nullopt;
        }
        const auto status = entry->symlink_status(error);
        if (error) {
            return std::nullopt;
        }
        if (!fs::is_directory(status)) {
            continue;
        }
        const auto candidate = entry->path() / addon::path_from_utf8(session_id);
        const auto type      = fs::symlink_status(candidate, error);
        if (error == std::errc::no_such_file_or_directory || type.type() == fs::file_type::not_found) {
            error.clear();
            continue;
        }
        if (error || !fs::is_directory(type)) {
            return std::nullopt;
        }
        const auto summary = file_stamp(candidate / "summary.json");
        if (!summary) {
            return std::nullopt;
        }
        if (summary->missing) {
            continue;
        }
        if (selected) {
            return std::nullopt;
        }
        selected = candidate;
    }
    return error ? std::nullopt : selected;
}

bool nonnegative_integer(const Json& value)
{
    return value.is_integer() && value.as_integer() >= 0;
}

bool native_timestamp(std::string_view text)
{
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':')
    {
        return false;
    }
    for (std::size_t index = 0; index < 19; ++index) {
        if (index == 4 || index == 7 || index == 10 || index == 13 || index == 16) {
            continue;
        }
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
    }
    if (text.back() != 'Z') {
        const auto zone = text.substr(text.size() - 6);
        if ((zone[0] != '+' && zone[0] != '-') || zone[3] != ':' ||
            zone[1] < '0' || zone[1] > '9' || zone[2] < '0' || zone[2] > '9' ||
            zone[4] < '0' || zone[4] > '9' || zone[5] < '0' || zone[5] > '9')
        {
            return false;
        }
    }
    return addon::parse_native_usage_timestamp(text).has_value();
}

bool optional_string(const Json& object, std::string_view name)
{
    const auto* value = object.find(name);
    return !value || value->is_null() || value->is_string();
}

bool valid_cwd_reminder(const Json& value)
{
    if (value.is_null()) {
        return true;
    }
    const auto* destination = value.find("destination_cwd");
    const auto* alias       = value.find("cwd");
    return json::only_keys(value, {"cwd_generation", "previous_cwd", "destination_cwd", "cwd",
               "content", "destination_project_instructions"}) &&
        nonnegative_integer(json::member(value, "cwd_generation")) &&
        json::member(value, "previous_cwd").is_string() && json::member(value, "content").is_string() &&
        ((destination && !alias && destination->is_string()) || (alias && !destination && alias->is_string())) &&
        optional_string(value, "destination_project_instructions");
}

bool valid_summary(const Json& summary, std::string_view session_id, std::string_view cwd)
{
    const auto& info = json::member(summary, "info");
    // This is Summary/Info's persisted schema, not just a directory index.
    // Native serde rejects malformed typed optional fields before any restore.
    if (!json::only_keys(summary, {"info", "agent_id", "attempt_id", "cwd_generation", "previous_cwd",
            "pending_cwd_switch_reminder", "cwd_switch_bookkeeping_generation", "session_summary",
            "created_at", "updated_at", "num_messages", "num_chat_messages", "current_model_id",
            "parent_session_id", "forked_at", "collection_id", "next_trace_turn", "chat_format_version",
            "prompt_display_cwd", "session_kind", "fork_context_source", "fork_parent_prompt_id",
            "inherited_prefix_len", "hidden", "source_workspace_dir", "git_root_dir", "git_remotes",
            "head_commit", "head_branch", "request_id", "grok_home", "last_active_at", "generated_title",
            "title_is_manual", "worktree_label", "agent_name", "agent_profile", "sandbox_profile",
            "reasoning_effort", "context_window", "last_turn_summary", "last_turn_summary_prompt_id", "last_recap"}) ||
        !json::exact_keys(info, {"id", "cwd"}) ||
        json::text(info, "id") != session_id || json::text(info, "cwd") != cwd ||
        !json::member(summary, "session_summary").is_string() ||
        !json::member(summary, "current_model_id").is_string() ||
        !nonnegative_integer(json::member(summary, "num_messages")) ||
        !native_timestamp(json::text(summary, "created_at")) ||
        !native_timestamp(json::text(summary, "updated_at")))
    {
        return false;
    }
    for (const auto name : {"agent_id", "attempt_id", "previous_cwd", "parent_session_id", "collection_id",
             "prompt_display_cwd", "session_kind", "fork_context_source", "fork_parent_prompt_id",
             "source_workspace_dir", "git_root_dir", "head_commit", "head_branch", "request_id", "grok_home",
             "generated_title", "worktree_label", "agent_name", "sandbox_profile", "last_turn_summary",
             "last_turn_summary_prompt_id", "last_recap"})
    {
        if (!optional_string(summary, name)) {
            return false;
        }
    }
    for (const auto name : {"cwd_generation", "cwd_switch_bookkeeping_generation", "num_chat_messages",
             "next_trace_turn", "chat_format_version"})
    {
        const auto* value = summary.find(name);
        if (value && !nonnegative_integer(*value)) {
            return false;
        }
    }
    if (const auto* format = summary.find("chat_format_version"); format && format->as_integer() > 1) {
        return false;
    }
    for (const auto name : {"forked_at", "last_active_at"}) {
        const auto* value = summary.find(name);
        if (value && !value->is_null() && (!value->is_string() || !native_timestamp(value->as_string()))) {
            return false;
        }
    }
    for (const auto name : {"context_window", "inherited_prefix_len"}) {
        const auto* value = summary.find(name);
        if (value && !value->is_null() &&
            (!nonnegative_integer(*value) || (std::string_view(name) == "context_window" && value->as_integer() == 0)))
        {
            return false;
        }
    }
    const auto* hidden = summary.find("hidden");
    const auto* manual = summary.find("title_is_manual");
    if ((hidden && !hidden->is_null() && !hidden->is_boolean()) || (manual && !manual->is_boolean())) {
        return false;
    }
    if (const auto* remotes = summary.find("git_remotes")) {
        if (!remotes->is_array() || std::any_of(remotes->as_array().begin(), remotes->as_array().end(),
                [](const Json& item) { return !item.is_string(); }))
        {
            return false;
        }
    }
    if (const auto* effort = summary.find("reasoning_effort"); effort && !effort->is_null()) {
        constexpr std::array<std::string_view, 7> values{"none", "minimal", "low", "medium", "high", "xhigh", "max"};
        if (!effort->is_string() || std::find(values.begin(), values.end(), effort->as_string()) == values.end()) {
            return false;
        }
    }
    const auto* reminder = summary.find("pending_cwd_switch_reminder");
    // PersistedAgentSelection deliberately accepts any agent_profile JSON,
    // discarding invalid inline definitions and retaining its optional name.
    return !reminder || valid_cwd_reminder(*reminder);
}

bool content_parts(const Json& parts, bool text_only = false)
{
    if (!parts.is_array()) {
        return false;
    }
    for (const auto& part : parts.as_array()) {
        const auto type = json::text(part, "type");
        if (type == "text" && json::exact_keys(part, {"type", "text"}) &&
            json::member(part, "text").is_string())
        {
            continue;
        }
        if (!text_only && type == "image" && json::exact_keys(part, {"type", "url"}) &&
            json::member(part, "url").is_string())
        {
            continue;
        }
        return false;
    }
    return true;
}

enum class History_item
{
    UNKNOWN,
    PRIMARY,
    REMINDER,
    CONTENT,
};

History_item chat_item(const Json& item)
{
    const auto type   = json::text(item, "type");
    const auto reason = json::text(item, "synthetic_reason");
    if (type == "system" && json::only_keys(item, {"type", "content", "synthetic_reason"}) &&
        json::member(item, "content").is_string())
    {
        if (!item.find("synthetic_reason") || reason == "primary") {
            return History_item::PRIMARY;
        }
        return reason == "compaction_meta" ? History_item::CONTENT : History_item::UNKNOWN;
    }
    if (type == "user" && content_parts(json::member(item, "content"))) {
        if (reason == "system_reminder") {
            return json::only_keys(item, {"type", "content", "synthetic_reason"}) &&
                content_parts(json::member(item, "content"), true)
                ? History_item::REMINDER : History_item::UNKNOWN;
        }
        constexpr std::array<std::string_view, 21> origins{
            "human", "compaction_meta", "length_continue", "project_instructions", "auto_continue",
            "auto_recovery", "interjection", "agent_message", "parent_agent_message", "task_completed",
            "subagent_completed", "notification_drain", "goal_summary", "goal_classifier_nudge",
            "scheduler_fired", "stop_hook_feedback", "working_directory_switch", "parent_human_message",
            "session_prefix", "direct_bash", "goal_setup"};
        if (!item.find("synthetic_reason") || std::find(origins.begin(), origins.end(), reason) != origins.end()) {
            return History_item::CONTENT;
        }
    }
    if (type == "assistant" && json::member(item, "content").is_string() &&
        (!item.find("tool_calls") || item.find("tool_calls")->is_array()))
    {
        return History_item::CONTENT;
    }
    if (type == "tool_result" && json::member(item, "content").is_string() &&
        json::member(item, "tool_call_id").is_string())
    {
        return History_item::CONTENT;
    }
    return History_item::UNKNOWN;
}

template<typename Classify>
addon::Resume_content read_lines(
    std::string_view bytes,
    Classify classify,
    addon::Invocation_context& context,
    bool retain_recognized_content = false)
{
    if (bytes.empty() || bytes.back() != '\n') {
        return addon::Resume_content::UNKNOWN;
    }
    std::size_t lines = 0;
    bool content = false;
    bool unknown = false;
    while (!bytes.empty()) {
        if (++lines > k_max_history_lines || context.cancel_requested()) {
            return addon::Resume_content::UNKNOWN;
        }
        const auto end   = bytes.find('\n');
        const auto value = parse_document(bytes.substr(0, end), k_max_history_bytes);
        const auto item  = value ? classify(*value, lines) : History_item::UNKNOWN;
        unknown = unknown || item == History_item::UNKNOWN;
        if (unknown && !retain_recognized_content) {
            return addon::Resume_content::UNKNOWN;
        }
        content = content || item == History_item::CONTENT;
        bytes.remove_prefix(end + 1);
    }
    return content ? addon::Resume_content::NONEMPTY :
        unknown ? addon::Resume_content::UNKNOWN : addon::Resume_content::EMPTY;
}

History_item update_item(const Json& envelope, std::string_view session_id)
{
    const auto& params = json::member(envelope, "params");
    const auto& update = json::member(params, "update");
    if (!json::only_keys(envelope, {"timestamp", "method", "params"}) ||
        !nonnegative_integer(json::member(envelope, "timestamp")) ||
        !json::only_keys(params, {"sessionId", "update", "_meta"}) ||
        (params.find("_meta") && !params.find("_meta")->is_object()) ||
        json::text(params, "sessionId") != session_id)
    {
        return History_item::UNKNOWN;
    }
    const auto method = json::text(envelope, "method");
    const auto kind   = json::text(update, "sessionUpdate");
    if (method == "_x.ai/session/update" && kind == "session_summary_generated" &&
        json::only_keys(update, {"sessionUpdate", "session_summary"}) &&
        json::member(update, "session_summary").is_string())
    {
        return History_item::REMINDER;
    }
    if (method == "_x.ai/session/update" && kind == "hook_execution" &&
        json::exact_keys(update, {"sessionUpdate", "event_name", "runs"}) &&
        json::text(update, "event_name") == "session_start" && json::member(update, "runs").is_array())
    {
        // A successful observer hook is startup metadata. Hook output, other
        // events, failures and unknown statuses are not an empty-history proof.
        for (const auto& run : json::member(update, "runs").as_array()) {
            const auto& status = json::member(run, "status");
            if (!json::exact_keys(run, {"name", "status"}) || !json::member(run, "name").is_string() ||
                !json::exact_keys(status, {"status", "elapsed_ms"}) || json::text(status, "status") != "success" ||
                !nonnegative_integer(json::member(status, "elapsed_ms")))
            {
                return History_item::UNKNOWN;
            }
        }
        return History_item::REMINDER;
    }
    if (method == "_x.ai/session/update" && kind == "background_tasks" &&
        json::only_keys(update, {"sessionUpdate", "tasks", "truncated"}) &&
        json::member(update, "tasks").is_array() && json::member(update, "tasks").as_array().empty() &&
        (!update.find("truncated") || (update.find("truncated")->is_boolean() && !update.find("truncated")->as_boolean())))
    {
        return History_item::REMINDER;
    }
    if (method == "session/update" &&
        (kind == "user_message_chunk" || kind == "agent_message_chunk" || kind == "agent_thought_chunk"))
    {
        const auto& content = json::member(update, "content");
        if (json::text(content, "type") == "text" && json::member(content, "text").is_string()) {
            return History_item::CONTENT;
        }
    }
    return History_item::UNKNOWN;
}

History_item btw_item(const Json& item, std::string_view session_id)
{
    if (!json::only_keys(item, {"btwSessionId", "parentSessionId", "askedAt", "question", "answer",
            "model", "success", "error", "attempts"}) ||
        !json::member(item, "btwSessionId").is_string() || json::text(item, "parentSessionId") != session_id ||
        !native_timestamp(json::text(item, "askedAt")) || !json::member(item, "question").is_string() ||
        !json::member(item, "answer").is_string() || !json::member(item, "model").is_string() ||
        !json::member(item, "success").is_boolean() || !optional_string(item, "error"))
    {
        return History_item::UNKNOWN;
    }
    const auto* attempts = item.find("attempts");
    if (attempts && (!nonnegative_integer(*attempts) || (std::uint64_t)attempts->as_integer() > UINT32_MAX)) {
        return History_item::UNKNOWN;
    }
    return History_item::CONTENT;
}

addon::Resume_content side_content(
    const fs::path& directory, std::string_view session_id, addon::Invocation_context& context)
{
    std::error_code error;
    const auto modified = fs::last_write_time(directory, error);
    if (error) {
        return addon::Resume_content::UNKNOWN;
    }
    fs::directory_iterator entry(directory, error);
    if (error) {
        return addon::Resume_content::UNKNOWN;
    }
    std::vector<std::pair<fs::path, file_stamp_t>> stamps;
    auto result = addon::Resume_content::EMPTY;
    for (; entry != fs::directory_iterator(); entry.increment(error)) {
        if (error || stamps.size() >= k_max_buckets || context.cancel_requested()) {
            return addon::Resume_content::UNKNOWN;
        }
        const auto stamp = file_stamp(entry->path());
        if (!stamp || stamp->missing) {
            return addon::Resume_content::UNKNOWN;
        }
        stamps.emplace_back(entry->path(), *stamp);
        const auto name = addon::path_to_utf8(entry->path().filename());
        if (name == "summary.json" || name == "chat_history.jsonl" || name == "updates.jsonl" ||
            name == "system_prompt.txt" || name == "prompt_context.json" || name == "title_refresh_idx" ||
            name == "summary.json.lock" || name == "updates.jsonl.lock")
        {
            continue;
        }
        if (name == "events.jsonl" && stamp->bytes == 0) {
            continue;
        }
        if (name != "btw_history.jsonl") {
            return addon::Resume_content::UNKNOWN;
        }
        const auto bytes = read_file(entry->path(), *stamp, k_max_history_bytes, context);
        if (!bytes) {
            return addon::Resume_content::UNKNOWN;
        }
        result = read_lines(*bytes, [&](const Json& item, std::size_t) {
            return btw_item(item, session_id);
        }, context, true);
        if (result == addon::Resume_content::UNKNOWN) {
            return result;
        }
    }
    if (error || fs::last_write_time(directory, error) != modified || error) {
        return addon::Resume_content::UNKNOWN;
    }
    for (const auto& [path, stamp] : stamps) {
        if (file_stamp(path) != stamp) {
            return addon::Resume_content::UNKNOWN;
        }
    }
    return result;
}

struct history_file_t
{
    fs::path path;
    file_stamp_t stamp;
    std::string bytes;
};

std::optional<history_file_t> read_history(const fs::path& path, addon::Invocation_context& context)
{
    const auto stamp = file_stamp(path);
    if (!stamp) {
        return std::nullopt;
    }
    if (stamp->missing) {
        return history_file_t{path, *stamp, {}};
    }
    const auto bytes = read_file(path, *stamp, k_max_history_bytes, context);
    if (!bytes) {
        return std::nullopt;
    }
    return history_file_t{path, *stamp, *bytes};
}

bool readable_workflows(const fs::path& directory)
{
    std::error_code error;
    const auto status = fs::symlink_status(directory / "workflows", error);
    if (error == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found) {
        return true;
    }
    if (error) {
        return false;
    }
    if (fs::is_directory(status)) {
        // Native restore skips invalid entries, but opening its workflow
        // directory can still fail the whole load.
        const fs::directory_iterator entries(directory / "workflows", error);
        return !error;
    }
    return true;
}

addon::Resume_content session_content(
    const fs::path& directory,
    std::string_view session_id,
    const Json& summary,
    const history_file_t& chat_file,
    const history_file_t& updates_file,
    addon::Invocation_context& context)
{
    auto chat = addon::Resume_content::UNKNOWN;
    std::size_t chat_lines = 0;
    if (!chat_file.stamp.missing) {
        chat_lines = (std::size_t)std::count(chat_file.bytes.begin(), chat_file.bytes.end(), '\n');
        // Native chat loading skips unrecognized rows but retains understood
        // conversation items. Unknown rows still prevent an empty result.
        chat = read_lines(chat_file.bytes, [](const Json& item, std::size_t line) {
            const auto kind = chat_item(item);
            // initialize() atomically persists [primary, skill reminder?].
            // Repeated or displaced initialization records are not that proof.
            if ((kind == History_item::PRIMARY && line != 1) ||
                (kind == History_item::REMINDER && line != 2))
            {
                return History_item::UNKNOWN;
            }
            return kind;
        }, context, true);
    }
    auto updates = addon::Resume_content::EMPTY;
    std::size_t update_lines = 0;
    if (!updates_file.bytes.empty()) {
        update_lines = (std::size_t)std::count(updates_file.bytes.begin(), updates_file.bytes.end(), '\n');
        updates = read_lines(updates_file.bytes, [&](const Json& item, std::size_t) {
            return update_item(item, session_id);
        }, context);
    }
    if (chat == addon::Resume_content::NONEMPTY || updates == addon::Resume_content::NONEMPTY) {
        return addon::Resume_content::NONEMPTY;
    }
    const auto side = side_content(directory, session_id, context);
    if (side == addon::Resume_content::NONEMPTY) {
        return side;
    }
    for (const auto name : {"pending_cwd_switch_reminder", "last_turn_summary", "last_turn_summary_prompt_id", "last_recap"}) {
        const auto* value = summary.find(name);
        if (value && !value->is_null()) {
            return addon::Resume_content::UNKNOWN;
        }
    }
    const auto* recorded_chat = summary.find("num_chat_messages");
    if ((std::uint64_t)json::member(summary, "num_messages").as_integer() > update_lines ||
        (recorded_chat && (std::uint64_t)recorded_chat->as_integer() > chat_lines))
    {
        return addon::Resume_content::UNKNOWN;
    }
    // A missing/empty chat cache may be rebuilt from updates. It is never an
    // initialization proof, even when summary counters and updates are empty.
    return chat == addon::Resume_content::EMPTY && updates == addon::Resume_content::EMPTY &&
        side == addon::Resume_content::EMPTY
        ? addon::Resume_content::EMPTY : addon::Resume_content::UNKNOWN;
}

} // namespace

addon::Resume_inspect_result inspect_resume_history(
    const fs::path& profile,
    std::string_view session_id,
    bool inspect_content,
    addon::Invocation_context& context)
{
    addon::Resume_inspect_result result;
    const auto directory = exact_session_directory(profile, session_id, context);
    if (!directory) {
        return result;
    }
    const auto cwd          = bucket_cwd(directory->parent_path());
    const auto summary_path = *directory / "summary.json";
    const auto stamp        = file_stamp(summary_path);
    const auto bytes        = stamp ? read_file(summary_path, *stamp, k_max_summary_bytes, context) : std::nullopt;
    const auto summary      = bytes ? parse_document(*bytes, k_max_summary_bytes) : std::nullopt;
    if (!cwd || !summary || !valid_summary(*summary, session_id, *cwd)) {
        return result;
    }
    const auto chat = read_history(*directory / "chat_history.jsonl", context);
    const auto updates = read_history(*directory / "updates.jsonl", context);
    if (!chat || !updates || !readable_workflows(*directory)) {
        return result;
    }
    const auto* format = summary->find("chat_format_version");
    result.availability = addon::Resume_availability::AVAILABLE;
    if (inspect_content && format && format->as_integer() == 1) {
        result.content = session_content(*directory, session_id, *summary, *chat, *updates, context);
    }
    if (file_stamp(summary_path) != stamp || file_stamp(chat->path) != chat->stamp ||
        file_stamp(updates->path) != updates->stamp || context.cancel_requested())
    {
        return {};
    }
    return result;
}

} // namespace grok
