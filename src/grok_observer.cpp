#include "grok_observer.h"
#include "grok_json.h"
#include "grok_lifecycle.h"

#include <logonomic_cli_addon/filesystem_lifecycle_observer.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
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

constexpr std::size_t k_retained_bound = 4096;
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
        return {};
    }
    const auto path = path_name::normalized(QString::fromStdString(value.as_string()));
    const QFileInfo info(path);
    if (!info.isAbsolute() || info.fileName() != QStringLiteral("updates.jsonl") ||
        QFileInfo(info.absolutePath()).fileName() != QString::fromStdString(session))
    {
        return {};
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

std::optional<QByteArray> checkpoint(QFile& input, qint64 offset)
{
    const auto count = std::min<qint64>(offset, 128);
    if (!input.seek(offset - count)) {
        return std::nullopt;
    }
    const auto bytes = input.read(count);
    if (bytes.size() != count) {
        return std::nullopt;
    }
    return bytes;
}

bool absent_journal(const QString& path)
{
    std::error_code error;
    const bool exists = std::filesystem::exists(addon::path_from_utf8(path.toStdString()), error);
    return !exists && (!error || error == std::errc::no_such_file_or_directory ||
        error == std::errc::not_a_directory);
}

std::optional<qint64> journal_size(const QString& path)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) {
        return absent_journal(path) ? std::optional<qint64>(0) : std::nullopt;
    }
    const auto size = input.size();
    return size >= 0 ? std::optional(size) : std::nullopt;
}

std::optional<Journal> open_journal(std::string session, QString path)
{
    Journal result;
    result.session = std::move(session);
    result.path    = std::move(path);
    QFile input(result.path);
    if (!input.open(QIODevice::ReadOnly)) {
        return absent_journal(result.path) ? std::optional(result) : std::nullopt;
    }
    result.offset = input.size();
    if (result.offset < 0) return std::nullopt;
    const auto frontier = checkpoint(input, result.offset);
    if (!frontier) return std::nullopt;
    result.checkpoint = *frontier;
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

struct Binding final : addon::Filesystem_lifecycle_binding
{
    std::map<std::string, Journal> journals;
    std::map<std::string, std::string> parents;
    std::map<std::pair<std::string, std::string>, Child> children;
    std::set<std::string> starts;

    observation::Subscription_options interests(const addon::Filesystem_lifecycle_bindings&) override;
    bool reconcile(addon::Invocation_context*) override;
    void check_owner(const addon::Filesystem_lifecycle_bindings& bindings) override;
    void release() override
    {
        journals.clear();
        parents.clear();
        children.clear();
        starts.clear();
    }
};

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
    if (binding.publish(std::move(event))) child.active = started;
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
        return;
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
        return;
    }
    const auto attempt = json::text(update, "attempt_id");
    const auto key = std::make_pair(child, attempt);
    auto previous = binding.children.find(key);
    if (kind == "subagent_finished") {
        const auto status = json::text(update, "status");
        if (status != "completed" && status != "failed" && status != "cancelled") {
            return;
        }
        if (previous == binding.children.end()) {
            return;
        }
        if (previous->second.parent != journal.session) {
            return;
        }
        if (previous->second.active) {
            child_event(binding, previous->second, false);
        }
        return;
    }
    if (json::text(update, "parent_session_id") != journal.session || child == binding.session ||
        child == journal.session)
    {
        return;
    }
    const auto owner = binding.parents.find(child);
    if (owner != binding.parents.end() && owner->second != journal.session) {
        return;
    }
    // Only a current launch's authenticated hook can admit this native row.
    // In particular, a recorded spawn in a resumed transcript is insufficient.
    const auto admission = std::find_if(journal.admissions.begin(), journal.admissions.end(),
        [&](const Admission& candidate) { return candidate.child == child && position >= candidate.frontier; });
    if (admission == journal.admissions.end()) {
        return;
    }
    if (json::text(update, "subagent_type") != admission->label) {
        return;
    }
    if (previous != binding.children.end()) {
        // A duplicated durable record cannot consume a later START admission.
        return;
    }
    if (binding.children.size() == k_retained_bound || !binding.can_start_child()) {
        journal.admissions.erase(admission);
        return;
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
    if (!input.open(QIODevice::ReadOnly)) return absent_journal(journal.path);
    const auto size = input.size();
    if (size < 0) return false;
    const auto previous = size >= journal.offset ? checkpoint(input, journal.offset) : std::nullopt;
    if (size < journal.offset || (previous && *previous != journal.checkpoint)) {
        // Rebase only this journal. Old hook frontiers cannot admit replacement
        // history, and neither missing bytes nor a rewrite prove child STOP.
        const auto fresh = open_journal(journal.session, journal.path);
        if (!fresh) return false;
        journal = *fresh;
        return true;
    }
    if (!previous || !input.seek(journal.offset)) return false;
    while (journal.offset < size && budget > 0) {
        const auto count = std::min({size - journal.offset, budget, qint64{64 * 1024}});
        const auto bytes = input.read(count);
        if (bytes.size() != count) return false;
        journal.offset += count;
        budget -= count;
        journal.checkpoint = (journal.checkpoint + bytes).right(128);
        journal.pending += bytes;
        qsizetype consumed = 0;
        for (;;) {
            const auto newline = journal.pending.indexOf('\n', consumed);
            if (newline < 0) break;
            const auto length = newline - consumed;
            if (!journal.discard_prefix && length > 0 && length <= k_line_bound) {
                const auto position = journal.offset - journal.pending.size() + consumed;
                accept_record(binding, journal, position, journal.pending.mid(consumed, length));
            }
            journal.discard_prefix = false;
            consumed = newline + 1;
        }
        journal.pending.remove(0, consumed);
        if (journal.pending.size() > k_line_bound) {
            journal.pending.clear();
            journal.discard_prefix = true;
        }
    }
    return journal.offset == size;
}

bool scan(Binding& binding)
{
    qint64 budget = k_read_bound;
    bool complete = true;
    std::size_t ancestry_size = 0;
    do {
        ancestry_size = binding.parents.size();
        qsizetype pending_bytes = 0;
        for (auto& [session, journal] : binding.journals) {
            if (!admitted_parent(binding, session)) continue;
            complete = scan_journal(binding, journal, budget) && complete;
            if (journal.pending.size() > k_pending_bytes_bound - pending_bytes) {
                journal.pending.clear();
                journal.discard_prefix = true;
            }
            pending_bytes += journal.pending.size();
        }
    } while (budget > 0 && ancestry_size != binding.parents.size());
    return complete;
}

bool Binding::reconcile(addon::Invocation_context*)
{
    return scan(*this);
}

observation::Subscription_options Binding::interests(const addon::Filesystem_lifecycle_bindings&)
{
    QStringList files;
    QStringList directories;
    for (const auto& [parent_session, journal] : journals) {
        if (!admitted_parent(*this, parent_session)) continue;
        auto frontier = journal.path;
        auto parent = QFileInfo(frontier).absolutePath();
        while (!QFileInfo(parent).isDir() && parent != frontier) {
            frontier = parent;
            parent = QFileInfo(frontier).absolutePath();
        }
        files.append(frontier);
        if (QFileInfo(journal.path).absolutePath() == parent) directories.append(parent);
    }
    return addon::lifecycle_file_interests(std::move(files), std::move(directories));
}

void Binding::check_owner(const addon::Filesystem_lifecycle_bindings& bindings)
{
    if (closed) return;
    path_name::Policy_epoch policy;
    for (const auto& [other_nonce, value] : bindings) {
        (void)other_nonce;
        const auto other = std::static_pointer_cast<Binding>(value);
        if (!other || other.get() == this || other->closed || other->retired) continue;
        if (other->session == session) throw Observation_failure("subagent_session_already_bound");
        for (const auto& [parent, journal] : journals) {
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

} // namespace

struct Lifecycle_observer::State
{
    State(std::shared_ptr<observation::Service> service, observation::Configuration configuration)

    :
        driver(std::move(service), configuration)
    {}
    addon::Filesystem_lifecycle_observer driver;
};

Lifecycle_observer::Lifecycle_observer()

:
    Lifecycle_observer(std::make_shared<observation::Service>(), {})
{}

Lifecycle_observer::Lifecycle_observer(
    std::shared_ptr<observation::Service> service,
    observation::Configuration configuration)

:
    m_state(std::make_unique<State>(std::move(service), configuration))
{}

Lifecycle_observer::~Lifecycle_observer() = default;

std::optional<addon::Classify_event_result> Lifecycle_observer::classify(
    const addon::Classify_event_request& request, addon::Contract_failure* failure)
{
    auto result = grok::classify_hook(request, failure);
    if (!result) return std::nullopt;
    const auto& body = request.event.body;
    const auto session = json::text(body, "sessionId");
    const auto& name = request.event.name;
    auto& driver = m_state->driver;
    auto guard = driver.lock();
    if (result->classification == addon::Classification::TRANSITION && name == "SessionStart") {
        if (!driver.reserved(request.transition_nonce)) throw Observation_failure("correlation_not_reserved");
        if (const auto existing = driver.find(request.transition_nonce)) {
            if (existing->session != session) throw Observation_failure("subagent_session_already_bound");
            return result;
        }
        auto binding = std::make_shared<Binding>();
        binding->session = session;
        binding->nonce = request.transition_nonce;
        const auto path = journal_path(body, session);
        if (!path.isEmpty()) {
            if (auto journal = open_journal(session, path)) binding->journals.emplace(session, std::move(*journal));
        }
        // The authenticated root transition does not depend on optional child
        // storage. A later valid parent hook can establish a readable frontier.
        driver.bind(std::move(binding));
        return result;
    }
    const auto& current = request.ledger.current_transition;
    if (!current || request.ledger.current_epoch == 0) return result;
    const auto owner = std::static_pointer_cast<Binding>(driver.find(current->nonce));
    if (!owner || owner->session != current->provider_session_id) return result;
    auto& binding = *owner;
    if (binding.closed || binding.retired) return addon::Classify_event_result{};
    if (name == "SessionEnd" && session == binding.session && result->observations.size() == 1 &&
        result->observations.front().session_transition == addon::Session_transition::ENDED)
    {
        // One bounded best-effort read preserves available child facts before
        // END. Optional unavailable or oversized tails cannot veto native END.
        (void)scan(binding);
        binding.finish(std::move(result->observations.front()));
        result->observations.clear();
        result->classification = addon::Classification::IGNORED;
        return result;
    }
    if (name != "SubagentStart") {
        return session == binding.session ? result : std::optional(addon::Classify_event_result{});
    }
    if (!identity(json::member(body, "sessionId")) || !identity(json::member(body, "subagentId")) ||
        !identity(json::member(body, "subagentType"), 160) || request.transition_nonce.empty())
    {
        return addon::Classify_event_result{};
    }
    if (binding.starts.contains(request.transition_nonce) || binding.starts.size() == k_retained_bound ||
        binding.journals.size() == k_retained_bound)
    {
        return addon::Classify_event_result{};
    }
    const auto child = json::text(body, "subagentId");
    if (child == session || child == binding.session) return addon::Classify_event_result{};
    const auto path = journal_path(body, session);
    if (path.isEmpty()) return addon::Classify_event_result{};
    auto journal = binding.journals.find(session);
    if (journal == binding.journals.end()) {
        path_name::Policy_epoch policy;
        for (const auto& [nonce, value] : driver.bindings()) {
            (void)nonce;
            const auto other = std::static_pointer_cast<Binding>(value);
            if (!other || other.get() == &binding || other->closed || other->retired) continue;
            for (const auto& [parent, existing] : other->journals) {
                (void)parent;
                if (policy.same(path, existing.path)) return addon::Classify_event_result{};
            }
        }
        auto opened = open_journal(session, path);
        if (!opened) return addon::Classify_event_result{};
        journal = binding.journals.emplace(session, std::move(*opened)).first;
    }
    else if (journal->second.path != path) return addon::Classify_event_result{};
    const auto frontier = journal_size(path);
    if (!frontier) return addon::Classify_event_result{};
    if (*frontier < journal->second.offset) {
        auto fresh = open_journal(session, path);
        if (!fresh) return addon::Classify_event_result{};
        journal->second = std::move(*fresh);
    }
    journal->second.admissions.push_back({child, json::text(body, "subagentType"), *frontier});
    binding.starts.insert(request.transition_nonce);
    binding.request_rescan();
    return addon::Classify_event_result{};
}

void Lifecycle_observer::handle(addon::Invocation_context& context)
{
    if (m_state->driver.handle(context)) return;
    try {
        if (context.request().operation == "classify_event") grok::handle_lifecycle(context, this);
        else context.reject_unknown_operation();
    }
    catch (const std::exception& error) { context.fail(addon::Result_status::FAILED, error.what()); }
}

} // namespace grok
