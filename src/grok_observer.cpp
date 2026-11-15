#include "grok_observer.h"
#include "grok_json.h"
#include "grok_lifecycle.h"

#include <logonomic_cli_addon/lifecycle_correlation_lifetime.h>
#include <logonomic_cli_addon/sha256.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace grok {
namespace {

namespace addon       = logonomic::cli_addon;
namespace json        = addon::json;
namespace observation = vnm::filesystem_observation;
namespace path_name   = vnm::path_name;

using Clock = std::chrono::steady_clock;

constexpr std::size_t k_session_bound  = 256;
constexpr std::size_t k_retained_bound = 4096;
constexpr std::size_t k_active_bound   = 128;
constexpr qint64 k_read_bound = 4 * 1024 * 1024;
constexpr qsizetype k_line_bound = 16 * 1024 * 1024;
constexpr qsizetype k_pending_bytes_bound = 64 * 1024 * 1024;

class Observation_failure final : public std::runtime_error
{
public:
    explicit Observation_failure(const char* reason)
    :
        std::runtime_error(reason)
    {}
};

struct Cursor
{
    std::string key;
    std::uint64_t sequence = 0;
};

Cursor cursor_for(const addon::Session_correlation& correlation, std::string_view value)
{
    if (value.empty()) {
        return {correlation.transition_nonce, 0};
    }
    const auto separator = value.find(':');
    if (separator == std::string_view::npos || value.substr(0, separator) != correlation.transition_nonce) {
        throw Observation_failure("invalid_observation_cursor");
    }
    Cursor cursor{correlation.transition_nonce, 0};
    const auto suffix = value.substr(separator + 1);
    const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), cursor.sequence);
    if (suffix.empty() || parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() ||
        std::to_string(cursor.sequence) != suffix)
    {
        throw Observation_failure("invalid_observation_cursor");
    }
    return cursor;
}

std::string cursor_text(const Cursor& cursor)
{
    return cursor.key + ':' + std::to_string(cursor.sequence);
}

bool identity(const Json& value, std::size_t maximum = 256)
{
    return json::bounded_string(value, maximum) &&
        value.as_string().find_first_of(std::string_view("\0\r\n", 3)) == std::string::npos;
}

QString journal_path(const Json& body, const std::string& session)
{
    const auto& value = json::member(body, "transcriptPath");
    if (value.is_null()) {
        return {};
    }
    if (!identity(value, 8192)) {
        throw Observation_failure("subagent_journal_path_invalid");
    }
    const auto path = path_name::normalized(QString::fromStdString(value.as_string()));
    const QFileInfo info(path);
    if (!info.isAbsolute() || info.fileName() != QStringLiteral("updates.jsonl") ||
        QFileInfo(info.absolutePath()).fileName() != QString::fromStdString(session))
    {
        throw Observation_failure("subagent_journal_path_invalid");
    }
    return path;
}

struct Admission
{
    std::string child;
    std::string label;
    qint64 frontier = 0;
};

struct Journal
{
    std::string session;
    QString path;
    qint64 offset = 0;
    QByteArray checkpoint;
    QByteArray pending;
    std::deque<Admission> admissions;
    bool discard_prefix = false;
};

QByteArray checkpoint(QFile& input, qint64 offset)
{
    const auto count = std::min<qint64>(offset, 128);
    if (!input.seek(offset - count)) {
        throw Observation_failure("subagent_journal_unavailable");
    }
    const auto bytes = input.read(count);
    if (bytes.size() != count) {
        throw Observation_failure("subagent_journal_unavailable");
    }
    return bytes;
}

qint64 journal_size(const QString& path)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) {
        if (!QFileInfo::exists(path)) {
            return 0;
        }
        throw Observation_failure("subagent_journal_unavailable");
    }
    return input.size();
}

Journal open_journal(std::string session, QString path)
{
    Journal result;
    result.session = std::move(session);
    result.path    = std::move(path);
    QFile input(result.path);
    if (!input.open(QIODevice::ReadOnly)) {
        if (!QFileInfo::exists(result.path)) {
            return result;
        }
        throw Observation_failure("subagent_journal_unavailable");
    }
    result.offset = input.size();
    result.checkpoint = checkpoint(input, result.offset);
    // Existing partial records belong to history too. A later newline must
    // not turn their suffix into a newly admitted record.
    result.discard_prefix = !result.checkpoint.isEmpty() && !result.checkpoint.endsWith('\n');
    return result;
}

struct Child
{
    std::string native_id;
    std::string parent;
    std::string attempt;
    std::string run_id;
    std::string label;
    bool active = true;
};

struct Binding
{
    std::string session;
    std::string nonce;
    std::map<std::string, Journal> journals;
    // Session ancestry is immutable, including across repeated execution attempts.
    std::map<std::string, std::string> parents;
    std::map<std::pair<std::string, std::string>, Child> children;
    std::set<std::string> starts;
    std::deque<addon::Lifecycle_observation> queue;
    std::uint64_t first_sequence = 0;
    std::uint64_t next_sequence  = 0;
    std::size_t active_children = 0;
    observation::Subscription subscription;
    std::shared_ptr<observation::Wait_channel> channel;
    QStringList watched_files;
    QStringList watched_directories;
    std::string failure;
    std::optional<std::string> final_cursor;
    std::optional<addon::Lifecycle_observation> ending;
    bool observing = false;
    bool watch_dirty = true;
    bool closed = false;
    bool retired = false;
};

void stop_observation(Binding& binding)
{
    binding.subscription.reset();
    if (binding.channel) {
        binding.channel->cancel();
    }
    binding.channel.reset();
    binding.watched_files.clear();
    binding.watched_directories.clear();
}

void enqueue(Binding& binding, addon::Lifecycle_observation event)
{
    if (binding.queue.size() == k_retained_bound) {
        binding.failure = "subagent_observation_queue_capacity_exhausted";
        throw Observation_failure("subagent_observation_queue_capacity_exhausted");
    }
    ++binding.next_sequence;
    binding.queue.push_back(std::move(event));
}

void child_event(Binding& binding, Child& child, bool started)
{
    addon::Lifecycle_observation event;
    event.provider_session_id = binding.session;
    event.provider_event_name = started ? "subagent_spawned" : "subagent_finished";
    event.provider_event_id   = child.run_id + (started ? ":start" : ":stop");
    event.agent_id            = child.run_id;
    event.agent_label         = child.label;
    event.subagent_transition = started ? addon::Subagent_transition::STARTED
                                       : addon::Subagent_transition::STOPPED;
    // An attempt is not a session-wide sequence, and a child outcome never
    // changes the root's phase, outcome, attention or session transition.
    enqueue(binding, std::move(event));
    child.active = started;
    if (started) {
        ++binding.active_children;
    }
    else {
        --binding.active_children;
    }
}

bool admitted_parent(const Binding& binding, const std::string& parent)
{
    return parent == binding.session || binding.parents.contains(parent);
}

void accept_record(Binding& binding, Journal& journal, qint64 position, const QByteArray& bytes)
{
    addon::Json_parse_limits limits;
    limits.max_bytes          = (std::size_t)k_line_bound;
    limits.max_string_bytes   = (std::size_t)k_line_bound;
    const auto value = addon::parse_json(std::string_view(bytes.constData(), (std::size_t)bytes.size()), limits, nullptr);
    if (!value || !value->is_object()) {
        throw Observation_failure("subagent_journal_record_invalid");
    }
    // Native storage wraps extension notifications; ordinary ACP rows do not
    // provide child lifecycle evidence.
    if (json::text(*value, "method") != "_x.ai/session/update") {
        return;
    }
    const auto& params = json::member(*value, "params");
    const auto& update = json::member(params, "update");
    const auto kind = json::text(update, "sessionUpdate");
    if (kind != "subagent_spawned" && kind != "subagent_finished") {
        return;
    }
    const auto child = json::text(update, "child_session_id");
    if (json::text(params, "sessionId") != journal.session ||
        !identity(json::member(update, "child_session_id")) ||
        json::text(update, "subagent_id") != child ||
        !identity(json::member(update, "attempt_id")))
    {
        throw Observation_failure("subagent_journal_identity_invalid");
    }
    const auto attempt = json::text(update, "attempt_id");
    const auto key = std::make_pair(child, attempt);
    auto previous = binding.children.find(key);
    if (kind == "subagent_finished") {
        const auto status = json::text(update, "status");
        if (status != "completed" && status != "failed" && status != "cancelled") {
            throw Observation_failure("subagent_journal_status_invalid");
        }
        if (previous == binding.children.end()) {
            return;
        }
        if (previous->second.parent != journal.session) {
            throw Observation_failure("subagent_journal_lineage_changed");
        }
        if (previous->second.active) {
            child_event(binding, previous->second, false);
        }
        return;
    }
    if (json::text(update, "parent_session_id") != journal.session || child == binding.session ||
        child == journal.session)
    {
        throw Observation_failure("subagent_journal_lineage_invalid");
    }
    const auto owner = binding.parents.find(child);
    if (owner != binding.parents.end() && owner->second != journal.session) {
        throw Observation_failure("subagent_journal_lineage_changed");
    }
    // Only a current launch's authenticated hook can admit this native row.
    // In particular, a recorded spawn in a resumed transcript is insufficient.
    const auto admission = std::find_if(journal.admissions.begin(), journal.admissions.end(),
        [&](const Admission& candidate) { return candidate.child == child && position >= candidate.frontier; });
    if (admission == journal.admissions.end()) {
        return;
    }
    if (json::text(update, "subagent_type") != admission->label) {
        throw Observation_failure("subagent_journal_type_changed");
    }
    if (previous != binding.children.end()) {
        // A duplicated durable record cannot consume a later START admission.
        return;
    }
    if (binding.children.size() == k_retained_bound || binding.active_children == k_active_bound) {
        throw Observation_failure("subagent_identity_capacity_exhausted");
    }
    Child run;
    run.native_id = child;
    run.parent    = journal.session;
    run.attempt   = attempt;
    run.run_id = "grok:" + addon::to_hex(addon::Sha256::digest(
        binding.nonce + ':' + run.parent + ':' + child + ':' + attempt));
    run.label = admission->label;
    journal.admissions.erase(admission);
    binding.parents.emplace(child, journal.session);
    auto inserted = binding.children.emplace(key, std::move(run)).first;
    child_event(binding, inserted->second, true);
    // A nested START can arrive before its parent's journal append. That
    // journal becomes observable only after this immutable ancestry join.
    binding.watch_dirty = true;
}

bool scan_journal(Binding& binding, Journal& journal, qint64& budget)
{
    QFile input(journal.path);
    if (!input.open(QIODevice::ReadOnly)) {
        if (journal.offset == 0 && !QFileInfo::exists(journal.path)) {
            return false;
        }
        throw Observation_failure("subagent_journal_unavailable");
    }
    const auto size = input.size();
    if (size < journal.offset || checkpoint(input, journal.offset) != journal.checkpoint) {
        // Rewind/replacement cannot silently reuse an old byte cursor or turn
        // missing records into terminal evidence.
        throw Observation_failure("subagent_journal_rewritten");
    }
    if (!input.seek(journal.offset)) {
        throw Observation_failure("subagent_journal_unavailable");
    }
    while (journal.offset < size && budget > 0) {
        const auto count = std::min({size - journal.offset, budget, qint64{64 * 1024}});
        const auto bytes = input.read(count);
        if (bytes.size() != count) {
            throw Observation_failure("subagent_journal_unavailable");
        }
        journal.offset += count;
        budget -= count;
        journal.pending += bytes;
        qsizetype consumed = 0;
        for (;;) {
            const auto newline = journal.pending.indexOf('\n', consumed);
            if (newline < 0) {
                break;
            }
            const auto length = newline - consumed;
            if (length > k_line_bound) {
                throw Observation_failure("subagent_journal_record_capacity_exhausted");
            }
            if (!journal.discard_prefix && length != 0) {
                const auto position = journal.offset - journal.pending.size() + consumed;
                accept_record(binding, journal, position, journal.pending.mid(consumed, length));
            }
            journal.discard_prefix = false;
            consumed = newline + 1;
        }
        journal.pending.remove(0, consumed);
        if (journal.pending.size() > k_line_bound) {
            throw Observation_failure("subagent_journal_record_capacity_exhausted");
        }
    }
    journal.checkpoint = checkpoint(input, journal.offset);
    return journal.offset < size;
}

bool scan(Binding& binding)
{
    qint64 budget = k_read_bound;
    bool more = false;
    qsizetype pending_bytes = 0;
    std::size_t ancestry_size = 0;
    do {
        ancestry_size = binding.parents.size();
        pending_bytes = 0;
        // A parent append can unlock a descendant journal visited earlier in
        // this map. Finish that join within the same bounded read budget.
        for (auto& [session, journal] : binding.journals) {
            if (admitted_parent(binding, session)) {
                more = scan_journal(binding, journal, budget) || more;
                pending_bytes += journal.pending.size();
                if (pending_bytes > k_pending_bytes_bound) {
                    throw Observation_failure("subagent_journal_buffer_capacity_exhausted");
                }
            }
        }
    } while (budget > 0 && ancestry_size != binding.parents.size());
    if (!more && binding.ending) {
        enqueue(binding, std::move(*binding.ending));
        binding.ending.reset();
        binding.closed = true;
        stop_observation(binding);
    }
    return more;
}

bool update_observation(
    Binding& binding, observation::Service& service, const observation::Configuration& configuration)
{
    QStringList files;
    QStringList directories;
    for (const auto& [session, journal] : binding.journals) {
        if (!admitted_parent(binding, session)) {
            continue;
        }
        auto frontier = journal.path;
        auto parent = QFileInfo(frontier).absolutePath();
        while (!QFileInfo(parent).isDir() && parent != frontier) {
            frontier = parent;
            parent = QFileInfo(frontier).absolutePath();
        }
        files.append(frontier);
        if (QFileInfo(journal.path).absolutePath() == parent) {
            directories.append(parent);
        }
    }
    files.removeDuplicates();
    directories.removeDuplicates();
    files.sort();
    directories.sort();
    if (files.isEmpty()) {
        binding.watch_dirty = false;
        return false;
    }
    if (binding.channel && files == binding.watched_files && directories == binding.watched_directories) {
        binding.watch_dirty = false;
        return false;
    }
    observation::Subscription_options options;
    options.mode         = observation::Watch_mode::DIRECT;
    options.interval     = configuration.general_interval;
    options.policy_epoch = std::make_shared<path_name::Policy_epoch>();
    options.exact_files  = files;
    QList<path_name::Key> keys;
    for (const auto& path : files) {
        keys.append(options.policy_epoch->key(path));
    }
    for (const auto& directory : directories) {
        options.directories.append({directory, 0});
    }
    options.classifier = [keys](const observation::Event& event) {
        return std::any_of(keys.cbegin(), keys.cend(), [&](const auto& key) {
            return path_name::matches_key(event.path, key) || path_name::matches_key(event.old_path, key);
        });
    };
    auto channel = std::make_shared<observation::Wait_channel>();
    auto subscription = service.subscribe(std::move(options), channel);
    stop_observation(binding);
    binding.subscription        = std::move(subscription);
    binding.channel             = std::move(channel);
    binding.watched_files       = std::move(files);
    binding.watched_directories = std::move(directories);
    binding.watch_dirty = false;
    return true;
}

bool observation_covered(const Binding& binding)
{
    for (const auto& file : binding.watched_files) {
        if (!binding.subscription.covers_directory(QFileInfo(file).absolutePath())) {
            return false;
        }
    }
    for (const auto& directory : binding.watched_directories) {
        if (!binding.subscription.covers_directory(directory)) {
            return false;
        }
    }
    return true;
}

} // namespace

struct Lifecycle_observer::State
{
    std::shared_ptr<observation::Service> service;
    observation::Configuration configuration;
    std::mutex mutex;
    // An empty value reserves a nonce. Only authenticated SessionStart binds it.
    std::map<std::string, std::shared_ptr<Binding>> sessions;
};

Lifecycle_observer::Lifecycle_observer()
:
    Lifecycle_observer(std::make_shared<observation::Service>(), {})
{}

Lifecycle_observer::Lifecycle_observer(
    std::shared_ptr<observation::Service> service,
    observation::Configuration configuration)
:
    m_state(std::make_unique<State>())
{
    if (!service) {
        throw std::invalid_argument("Grok observer requires a process observation service");
    }
    configuration.validate();
    m_state->service       = std::move(service);
    m_state->configuration = configuration;
}

Lifecycle_observer::~Lifecycle_observer() = default;

std::optional<addon::Classify_event_result> Lifecycle_observer::classify(
    const addon::Classify_event_request& request, addon::Contract_failure* failure)
{
    auto result = grok::classify_hook(request, failure);
    if (!result) {
        return std::nullopt;
    }
    const auto& body = request.event.body;
    const auto session = json::text(body, "sessionId");
    const auto& name = request.event.name;
    std::lock_guard lock(m_state->mutex);
    if (result->classification == addon::Classification::TRANSITION && name == "SessionStart") {
        const auto found = m_state->sessions.find(request.transition_nonce);
        if (found == m_state->sessions.end()) {
            throw Observation_failure("correlation_not_reserved");
        }
        const auto path = journal_path(body, session);
        if (found->second) {
            const auto journal = found->second->journals.find(session);
            if (found->second->session != session ||
                (journal == found->second->journals.end() ? !path.isEmpty() : journal->second.path != path))
            {
                throw Observation_failure("subagent_session_already_bound");
            }
            return result;
        }
        auto binding = std::make_shared<Binding>();
        binding->session = session;
        binding->nonce   = request.transition_nonce;
        if (!path.isEmpty()) {
            binding->journals.emplace(session, open_journal(session, path));
        }
        found->second = std::move(binding);
        return result;
    }
    const auto& current = request.ledger.current_transition;
    if (!current || request.ledger.current_epoch == 0) {
        return result;
    }
    const auto found = m_state->sessions.find(current->nonce);
    if (found == m_state->sessions.end() || !found->second ||
        found->second->session != current->provider_session_id)
    {
        return result;
    }
    auto& binding = *found->second;
    if (binding.closed || binding.retired || binding.ending) {
        return addon::Classify_event_result{};
    }
    if (name == "SessionEnd" && session == binding.session && result->observations.size() == 1 &&
        result->observations.front().session_transition == addon::Session_transition::ENDED)
    {
        // Drain already durable rows before ordering root END last. Large tails
        // continue through the filesystem service, without blocking this hook.
        binding.ending = std::move(result->observations.front());
        result->observations.clear();
        result->classification = addon::Classification::IGNORED;
        if (scan(binding) && binding.channel) {
            binding.subscription.request_rescan(QStringLiteral("Grok terminal journal drain"));
        }
        return result;
    }
    if (name != "SubagentStart") {
        // Root-shaped child hooks, including Stop/SessionEnd, cannot escape
        // the bound root even when native envelopes omit a subagent type.
        return session == binding.session ? result : std::optional(addon::Classify_event_result{});
    }
    if (!binding.failure.empty()) {
        throw Observation_failure("subagent_observation_unavailable");
    }
    if (!identity(json::member(body, "sessionId")) ||
        !identity(json::member(body, "subagentId")) ||
        !identity(json::member(body, "subagentType"), 160) ||
        request.transition_nonce.empty())
    {
        throw Observation_failure("subagent_hook_identity_invalid");
    }
    if (binding.starts.contains(request.transition_nonce)) {
        return addon::Classify_event_result{};
    }
    if (binding.starts.size() == k_retained_bound || binding.journals.size() == k_retained_bound) {
        throw Observation_failure("subagent_identity_capacity_exhausted");
    }
    const auto child = json::text(body, "subagentId");
    if (child == session || child == binding.session) {
        throw Observation_failure("subagent_hook_lineage_invalid");
    }
    const auto path = journal_path(body, session);
    if (path.isEmpty()) {
        throw Observation_failure("subagent_journal_unavailable");
    }
    auto journal = binding.journals.find(session);
    if (journal == binding.journals.end()) {
        path_name::Policy_epoch policy;
        for (const auto& [nonce, other] : m_state->sessions) {
            (void)nonce;
            if (!other || other.get() == &binding || other->closed || other->retired) {
                continue;
            }
            for (const auto& [parent, existing] : other->journals) {
                (void)parent;
                if (policy.same(path, existing.path)) {
                    throw Observation_failure("subagent_session_already_bound");
                }
            }
        }
        journal = binding.journals.emplace(session, open_journal(session, path)).first;
    }
    else if (journal->second.path != path) {
        throw Observation_failure("subagent_journal_path_changed");
    }
    const auto frontier = journal_size(path);
    if (frontier < journal->second.offset) {
        throw Observation_failure("subagent_journal_rewritten");
    }
    // Grok awaits this observe hook before persisting its SubagentSpawned row.
    // The byte frontier therefore distinguishes this attempt from history.
    journal->second.admissions.push_back({child, json::text(body, "subagentType"), frontier});
    binding.starts.insert(request.transition_nonce);
    binding.watch_dirty = true;
    if (binding.channel) {
        binding.subscription.request_rescan(QStringLiteral("Grok child admission"));
    }
    return addon::Classify_event_result{};
}

void Lifecycle_observer::handle(addon::Invocation_context& context)
{
    try {
        addon::Contract_failure failure;
        const auto& invocation = context.request();
        if (invocation.capability.capability_id == "lifecycle-correlation-lifetime") {
            if (invocation.operation == addon::k_lifecycle_correlation_lifetime_reserve_operation) {
                const auto request = addon::decode_lifecycle_correlation_reserve_request(invocation.payload, &failure);
                if (!request) {
                    context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                    return;
                }
                std::lock_guard lock(m_state->mutex);
                if (context.cancel_requested()) {
                    context.fail(addon::Result_status::CANCELLED, "cancelled");
                    return;
                }
                if (!m_state->sessions.contains(request->correlation.transition_nonce)) {
                    if (m_state->sessions.size() == k_session_bound) {
                        throw Observation_failure("subagent_session_capacity_exhausted");
                    }
                    m_state->sessions.emplace(request->correlation.transition_nonce, nullptr);
                }
                context.succeed(addon::encode_lifecycle_correlation_lifetime_ack());
            }
            else
            if (invocation.operation == addon::k_lifecycle_correlation_lifetime_retire_operation) {
                const auto request = addon::decode_lifecycle_correlation_retire_request(invocation.payload, &failure);
                if (!request) {
                    context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                    return;
                }
                std::lock_guard lock(m_state->mutex);
                const auto found = m_state->sessions.find(request->correlation.transition_nonce);
                if (found != m_state->sessions.end()) {
                    const auto binding = found->second;
                    if (request->disposition == addon::Lifecycle_correlation_retire_disposition::TERMINAL_CONSUMED &&
                        (!binding || !binding->closed || !binding->final_cursor ||
                            binding->final_cursor != request->consumed_cursor))
                    {
                        throw Observation_failure("terminal_cursor_not_consumed");
                    }
                    if (binding) {
                        binding->retired = true;
                        stop_observation(*binding);
                        binding->children.clear();
                        binding->starts.clear();
                        binding->queue.clear();
                        binding->journals.clear();
                        binding->parents.clear();
                        binding->ending.reset();
                    }
                    m_state->sessions.erase(found);
                }
                context.succeed(addon::encode_lifecycle_correlation_lifetime_ack());
            }
            else {
                context.reject_unknown_operation();
            }
            return;
        }
        if (invocation.operation == "classify_event") {
            grok::handle_lifecycle(context, this);
            return;
        }
        if (invocation.operation == "poll_observations") {
            const auto request = addon::decode_poll_observations_request(invocation.payload, &failure);
            if (!request) {
                context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                return;
            }
            if (!request->correlation) {
                context.fail(addon::Result_status::UNAVAILABLE, "session_start_required");
                return;
            }
            const auto cursor = cursor_for(*request->correlation, request->resume_cursor);
            std::lock_guard lock(m_state->mutex);
            const auto found = m_state->sessions.find(cursor.key);
            if (found == m_state->sessions.end() || !found->second) {
                throw Observation_failure("unknown_observation_cursor");
            }
            auto& binding = *found->second;
            if (!binding.failure.empty()) {
                context.fail(addon::Result_status::FAILED, binding.failure);
                return;
            }
            if (cursor.sequence < binding.first_sequence || cursor.sequence > binding.next_sequence) {
                throw Observation_failure("invalid_observation_cursor");
            }
            while (binding.first_sequence < cursor.sequence) {
                binding.queue.pop_front();
                ++binding.first_sequence;
            }
            addon::Poll_observations_result result;
            const auto count = std::min(request->limit, binding.queue.size());
            result.observations.assign(binding.queue.begin(), binding.queue.begin() + (std::ptrdiff_t)count);
            result.resume_cursor = cursor_text({cursor.key, cursor.sequence + count});
            result.more = count < binding.queue.size();
            if (binding.closed && !result.more) {
                binding.final_cursor = result.resume_cursor;
            }
            context.succeed(addon::encode_poll_observations_result(result));
            return;
        }
        if (invocation.operation != "observe") {
            context.reject_unknown_operation();
            return;
        }
        const auto request = addon::decode_observe_request(invocation.payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        if (!request->correlation) {
            context.fail(addon::Result_status::UNAVAILABLE, "session_start_required");
            return;
        }
        const auto cursor = cursor_for(*request->correlation, request->resume_cursor);
        std::shared_ptr<Binding> binding;
        std::uint64_t window_sequence = 0;
        {
            std::lock_guard lock(m_state->mutex);
            const auto found = m_state->sessions.find(cursor.key);
            if (found == m_state->sessions.end() || !found->second ||
                found->second->session != request->session.provider_session_id)
            {
                context.fail(addon::Result_status::UNAVAILABLE, "session_start_required");
                return;
            }
            binding = found->second;
            if (binding->observing || cursor.sequence > binding->next_sequence) {
                throw Observation_failure("invalid_observation_cursor");
            }
            // Classification reserves the new epoch before the host retires
            // the old one, so ownership conflicts are tested when observation
            // begins, after that transition has committed.
            if (!binding->closed) {
                path_name::Policy_epoch policy;
                for (const auto& [nonce, other] : m_state->sessions) {
                    (void)nonce;
                    if (!other || other == binding || other->closed || other->retired) {
                        continue;
                    }
                    if (other->session == binding->session) {
                        throw Observation_failure("subagent_session_already_bound");
                    }
                    for (const auto& [parent, journal] : binding->journals) {
                        (void)parent;
                        for (const auto& [other_parent, existing] : other->journals) {
                            (void)other_parent;
                            if (policy.same(journal.path, existing.path)) {
                                throw Observation_failure("subagent_session_already_bound");
                            }
                        }
                    }
                }
            }
            // Polling alone acknowledges queue entries. The final drain of a
            // completed watch may advance consumption beyond its watch cursor.
            window_sequence = std::max(cursor.sequence, binding->first_sequence);
            binding->observing = true;
        }
        std::string error;
        const auto deadline = Clock::now() + std::chrono::milliseconds(request->window_ms);
        auto reported_sequence = window_sequence;
        try {
            while (!context.cancel_requested() && Clock::now() < deadline) {
                std::shared_ptr<observation::Wait_channel> channel;
                {
                    std::lock_guard lock(m_state->mutex);
                    if (binding->retired) {
                        break;
                    }
                    if (!binding->failure.empty()) {
                        error = binding->failure;
                        break;
                    }
                    if (binding->next_sequence > reported_sequence) {
                        reported_sequence = binding->next_sequence;
                        const auto available = reported_sequence - window_sequence;
                        context.report_progress(available, std::nullopt, addon::observe_progress_note(available));
                    }
                    if (binding->closed) {
                        break;
                    }
                    if (!binding->channel || binding->watch_dirty) {
                        update_observation(*binding, *m_state->service, m_state->configuration);
                    }
                    channel = binding->channel;
                }
                if (!channel) {
                    // Recording can be disabled without disabling root hooks.
                    // This wait bounds cancellation only; it performs no scans.
                    context.wait_for_cancel(100);
                    continue;
                }
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
                auto delivery = channel->wait(std::min(remaining, std::chrono::milliseconds(100)));
                if (!delivery) {
                    continue;
                }
                {
                    std::lock_guard lock(m_state->mutex);
                    if (delivery->valid() && !binding->retired && !binding->closed && binding->channel == channel) {
                        const bool replaced = update_observation(
                            *binding, *m_state->service, m_state->configuration);
                        if (!replaced) {
                            const bool more = scan(*binding);
                            if (!more && !binding->watch_dirty && observation_covered(*binding)) {
                                binding->subscription.acknowledge(delivery->batch.capture);
                            }
                            else {
                                binding->subscription.request_rescan(QStringLiteral("Grok journal reconciliation pending"));
                            }
                        }
                    }
                }
                delivery->completion.complete();
            }
        }
        catch (const Observation_failure& exception) {
            error = exception.what();
        }
        catch (const std::exception&) {
            error = "subagent_observation_failed";
        }
        std::lock_guard lock(m_state->mutex);
        binding->observing = false;
        if (binding->retired || context.cancel_requested()) {
            context.fail(addon::Result_status::CANCELLED, binding->retired ? "correlation_retired" : "cancelled");
        }
        else
        if (!error.empty()) {
            binding->failure = error;
            stop_observation(*binding);
            context.fail(addon::Result_status::FAILED, error);
        }
        else {
            context.succeed(addon::encode_observe_result({cursor_text({cursor.key, binding->next_sequence})}));
        }
    }
    catch (const Observation_failure& exception) {
        context.fail(addon::Result_status::FAILED, exception.what());
    }
    catch (const std::exception&) {
        context.fail(addon::Result_status::FAILED, "subagent_observation_failed");
    }
}

} // namespace grok
