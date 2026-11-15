#include "grok_observer.h"
#include "grok_json.h"

#include <logonomic_cli_addon/lifecycle_correlation_lifetime.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace addon       = logonomic::cli_addon;
namespace json        = addon::json;
namespace observation = vnm::filesystem_observation;
namespace fs          = std::filesystem;

using Json = addon::Json_value;
constexpr const char* k_root = "root-session";

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class Context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return m_cancel.load(); }
    bool wait_for_cancel(int timeout_ms) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return cancel_requested();
    }
    void report_progress(std::uint64_t completed, std::optional<std::uint64_t>, std::string) override
    {
        m_available = completed;
    }
    void succeed(Json result) override { m_result = std::move(result); }
    void fail(addon::Result_status status, std::string reason, std::string) override
    {
        m_status  = status;
        m_failure = std::move(reason);
    }
    bool finished() const override { return m_result.has_value() || !m_failure.empty(); }

    addon::Invoke_request m_request;
    std::optional<Json> m_result;
    addon::Result_status m_status = addon::Result_status::SUCCESS;
    std::string m_failure;
    std::atomic<bool> m_cancel = false;
    std::atomic<std::uint64_t> m_available = 0;
};

bool reserve(grok::Lifecycle_observer& observer, const std::string& nonce)
{
    Context context;
    context.m_request.capability = {"lifecycle-correlation-lifetime", 1};
    context.m_request.operation = "reserve";
    context.m_request.payload = addon::encode_lifecycle_correlation_reserve_request({{nonce}});
    observer.handle(context);
    return context.m_result.has_value();
}

bool retire(grok::Lifecycle_observer& observer, const std::string& nonce,
    std::optional<std::string> cursor = std::nullopt)
{
    addon::Lifecycle_correlation_retire_request request;
    request.correlation = {nonce};
    request.disposition = cursor ? addon::Lifecycle_correlation_retire_disposition::TERMINAL_CONSUMED
                                : addon::Lifecycle_correlation_retire_disposition::ABANDONED;
    request.consumed_cursor = std::move(cursor);
    Context context;
    context.m_request.capability = {"lifecycle-correlation-lifetime", 1};
    context.m_request.operation = "retire";
    context.m_request.payload = addon::encode_lifecycle_correlation_retire_request(request);
    observer.handle(context);
    return context.m_result.has_value();
}

std::optional<addon::Poll_observations_result> poll(grok::Lifecycle_observer& observer,
    const std::string& nonce, const std::string& cursor = {}, std::size_t limit = 16)
{
    addon::Poll_observations_request request;
    request.correlation = addon::Session_correlation{nonce};
    request.resume_cursor = cursor;
    request.limit = limit;
    Context context;
    context.m_request.capability = {"lifecycle-events", 2};
    context.m_request.operation = "poll_observations";
    context.m_request.payload = addon::encode_poll_observations_request(request);
    observer.handle(context);
    return context.m_result ? addon::decode_poll_observations_result(*context.m_result, limit, nullptr) : std::nullopt;
}

bool wait_until(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// Oracle: grok-build 2bdd1d6, session/storage/mod.rs SessionUpdateEnvelope and
// extensions/notification.rs SubagentSpawned/SubagentFinished. Inner native
// fields remain snake_case even though the outer notification uses sessionId.
std::string record(const std::string& parent, const std::string& child, const std::string& attempt,
    bool started, const std::string& status = "completed")
{
    auto update = json::object({
        {"sessionUpdate", Json(started ? "subagent_spawned" : "subagent_finished")},
        {"subagent_id", Json(child)}, {"attempt_id", Json(attempt)}, {"child_session_id", Json(child)}});
    if (started) {
        update.set("parent_session_id", Json(parent));
        update.set("subagent_type", Json("code-reviewer"));
        update.set("description", Json("Native task text is not a lifecycle label"));
    }
    else {
        update.set("status", Json(status));
        update.set("tool_calls", Json(1));
        update.set("turns", Json(1));
        update.set("duration_ms", Json(1));
    }
    return addon::serialize_json(json::object({
        {"timestamp", Json(1794744000)}, {"method", Json("_x.ai/session/update")},
        {"params", json::object({{"sessionId", Json(parent)}, {"update", std::move(update)}})}})) + '\n';
}

observation::Configuration test_configuration()
{
    observation::Configuration result;
    result.general_interval = std::chrono::milliseconds(25);
    return result;
}

class Fixture final
{
public:
    explicit Fixture(const std::string& historical = {},
        const std::function<void(Fixture&, addon::Classify_event_request&)>& before_start = {})
    :
        m_root(fs::temp_directory_path() / ("grok-observer-" + addon::random_nonce_hex())),
        m_service(std::make_shared<observation::Service>()),
        m_observer(m_service, test_configuration()),
        m_nonce(addon::random_nonce_hex())
    {
        append(k_root, historical);
        require(reserve(m_observer, m_nonce), "reserve root epoch before authenticated SessionStart");
        auto start = hook("SessionStart");
        start.ledger = {};
        start.transition_nonce = m_nonce;
        start.event.body.set("source", Json("new"));
        if (before_start) {
            before_start(*this, start);
        }
        const auto result = m_observer.classify(start);
        require(result && result->classification == addon::Classification::TRANSITION,
            "root SessionStart must retain its native transition");
    }

    ~Fixture()
    {
        retire(m_observer, m_nonce);
        std::error_code error;
        fs::remove_all(m_root, error);
    }

    QString path(const std::string& parent) const
    {
        return QString::fromStdString(addon::path_to_utf8(m_root / parent / "updates.jsonl"));
    }

    void append(const std::string& parent, const std::string& bytes, bool truncate = false) const
    {
        require(QDir().mkpath(QFileInfo(path(parent)).absolutePath()), "create journal fixture directory");
        QFile output(path(parent));
        require(output.open(QIODevice::WriteOnly | (truncate ? QIODevice::Truncate : QIODevice::Append)),
            "open native journal fixture");
        require(output.write(bytes.data(), (qint64)bytes.size()) == (qint64)bytes.size(), "append complete journal bytes");
    }

    addon::Classify_event_request hook(const std::string& name, const std::string& child = {},
        const std::string& parent = k_root) const
    {
        addon::Classify_event_request request;
        request.transition_nonce = addon::random_nonce_hex();
        request.event.name = name;
        request.event.body = json::object({
            {"hook_event_name", Json(name)}, {"sessionId", Json(parent)},
            {"transcriptPath", Json(path(parent).toStdString())}});
        request.ledger.current_epoch = 1;
        request.ledger.current_transition = addon::Ledger_transition{addon::Transition_kind::START, k_root, m_nonce};
        if (!child.empty()) {
            request.event.body.set("subagentId", Json(child));
            request.event.body.set("subagentType", Json("code-reviewer"));
        }
        return request;
    }

    void admit(const std::string& child, const std::string& parent = k_root)
    {
        const auto result = m_observer.classify(hook("SubagentStart", child, parent));
        require(result && result->observations.empty(), "hook admission awaits the actual native attempt record");
    }

    addon::Poll_observations_result page(std::size_t count, const std::string& cursor = {})
    {
        std::optional<addon::Poll_observations_result> result;
        require(wait_until([&] {
            result = poll(m_observer, m_nonce, cursor);
            return result && result->observations.size() == count;
        }), "native journal rows must reach the bounded observation queue");
        return *result;
    }

    std::uint64_t capture(std::uint64_t after = 0)
    {
        std::uint64_t dispatches = 0;
        require(wait_until([&] {
            const auto stats = m_service->statistics();
            dispatches = stats.dispatches;
            return dispatches > after && stats.outstanding == 0 && stats.pending_registrations == 0 && stats.retained_events == 0;
        }), "native acquisition capture must finish before checking an unchanged queue");
        return dispatches;
    }

    fs::path m_root;
    std::shared_ptr<observation::Service> m_service;
    grok::Lifecycle_observer m_observer;
    std::string m_nonce;
};

class Observe_window final
{
public:
    explicit Observe_window(Fixture& fixture, std::string cursor = {})
    {
        addon::Observe_request request;
        request.session = {addon::path_to_utf8(fixture.m_root), {"grok"}, k_root};
        request.correlation = addon::Session_correlation{fixture.m_nonce};
        request.resume_cursor = std::move(cursor);
        request.window_ms = addon::limits::k_observe_window_ms;
        m_context.m_request.capability = {"lifecycle-events", 2};
        m_context.m_request.operation = "observe";
        m_context.m_request.payload = addon::encode_observe_request(request);
        m_worker = std::async(std::launch::async, [&fixture, this] { fixture.m_observer.handle(m_context); });
    }

    ~Observe_window()
    {
        m_context.m_cancel = true;
        m_worker.wait();
    }

    Context m_context;
    std::future<void> m_worker;
};

void authenticated_attempts_and_torn_records()
{
    Fixture fixture(record(k_root, "child", "old", true) + record(k_root, "child", "old", false));
    Observe_window window(fixture);
    auto foreign = fixture.hook("SubagentStart", "foreign");
    foreign.ledger = {};
    fixture.m_observer.classify(foreign);
    fixture.append(k_root, record(k_root, "foreign", "unowned", true) + record(k_root, "foreign", "unowned", false));
    const auto start = fixture.hook("SubagentStart", "child");
    fixture.m_observer.classify(start);
    const auto finish = record(k_root, "child", "attempt-a", false);
    fixture.append(k_root, record(k_root, "child", "attempt-a", true) + finish.substr(0, finish.size() / 2));
    const auto started = fixture.page(1);
    const auto& first = started.observations.front();
    require(first.subagent_transition == addon::Subagent_transition::STARTED && first.provider_session_id == k_root &&
        first.session_transition == addon::Session_transition::NONE && first.phase_transition == addon::Phase_transition::NONE &&
        first.outcome_transition == addon::Outcome_transition::NONE && first.provider_sequence.empty(),
        "the native child span must remain isolated from all root lifecycle fields");
    fixture.m_observer.classify(start);
    const auto child_stop = fixture.m_observer.classify(fixture.hook("SubagentStop", "child"));
    require(child_stop && child_stop->observations.empty(), "blockable SubagentStop cannot close a child");
    const auto root_stop = fixture.m_observer.classify(fixture.hook("Stop"));
    require(root_stop && root_stop->observations.empty(), "root stop gate cannot imply child quiescence");
    const auto child_end = fixture.m_observer.classify(fixture.hook("SessionEnd", {}, "child"));
    require(child_end && child_end->observations.empty(), "a child SessionEnd cannot become a root END");
    fixture.capture();
    const auto partial = poll(fixture.m_observer, fixture.m_nonce, started.resume_cursor);
    require(partial && partial->observations.empty(), "a torn terminal record supplies no STOP evidence");
    fixture.append(k_root, finish.substr(finish.size() / 2));
    const auto stopped = fixture.page(1, started.resume_cursor);
    require(stopped.observations.front().subagent_transition == addon::Subagent_transition::STOPPED &&
        stopped.observations.front().agent_id == first.agent_id, "the completed durable row closes exactly its attempt");
    const auto replay = poll(fixture.m_observer, fixture.m_nonce, started.resume_cursor);
    require(replay && replay->observations.front().provider_event_id == stopped.observations.front().provider_event_id,
        "poll retries replay stable native semantic identities");
}

void descendants_can_arrive_before_their_ancestry()
{
    Fixture fixture;
    fixture.admit("a-child");
    fixture.append("a-child", record("a-child", "b-grandchild", "historical", true) +
        record("a-child", "b-grandchild", "historical", false));
    fixture.admit("b-grandchild", "a-child");
    fixture.admit("c-descendant", "b-grandchild");
    fixture.append("b-grandchild", record("b-grandchild", "c-descendant", "third", true) +
        record("b-grandchild", "c-descendant", "third", false));
    fixture.append("a-child", record("a-child", "b-grandchild", "second", true) +
        record("a-child", "b-grandchild", "second", false));
    fixture.append(k_root, record(k_root, "a-child", "first", true) + record(k_root, "a-child", "first", false));
    Observe_window window(fixture);
    const auto result = fixture.page(6);
    std::map<std::string, int> balance;
    for (const auto& event : result.observations) {
        require(event.provider_session_id == k_root && event.provider_sequence.empty(),
            "every descendant retains the authenticated root routing identity");
        balance[event.agent_id] += event.subagent_transition == addon::Subagent_transition::STARTED ? 1 : -1;
        require(balance[event.agent_id] >= 0, "a descendant STOP must follow its own START");
    }
    require(balance.size() == 3 && std::all_of(balance.begin(), balance.end(), [](const auto& item) { return item.second == 0; }),
        "all three native attempts retain separately paired intervals");
}

void repeated_native_children_keep_distinct_attempts()
{
    Fixture fixture;
    fixture.admit("child");
    fixture.append(k_root, record(k_root, "child", "first", true) + record(k_root, "child", "first", false));
    Observe_window window(fixture);
    const auto first = fixture.page(2);
    fixture.admit("child");
    fixture.append(k_root, record(k_root, "child", "first", true) + record(k_root, "child", "first", false) +
        record(k_root, "child", "second", true));
    const auto second = fixture.page(1, first.resume_cursor);
    require(second.observations.front().agent_id != first.observations.front().agent_id,
        "a native attempt_id change opens a distinct interval for the same child session");
    fixture.append(k_root, record(k_root, "child", "first", false) + record(k_root, "child", "second", false, "cancelled"));
    const auto stopped = fixture.page(1, second.resume_cursor);
    require(stopped.observations.front().agent_id == second.observations.front().agent_id,
        "a stale finish cannot close a later attempt of the same native child");
}

void optional_child_evidence_cannot_reject_root_lifecycle()
{
    for (const bool unreadable : {false, true}) {
        Fixture fixture({}, [unreadable](Fixture& value, addon::Classify_event_request& start) {
            if (unreadable) {
                require(QFile::remove(value.path(k_root)), "remove the root journal fixture");
                require(QDir().mkpath(value.path(k_root)), "make the optional journal unreadable as a file");
            }
            else {
                start.event.body.set("transcriptPath", Json(17));
            }
        });
        Observe_window window(fixture);
        const auto ended = fixture.m_observer.classify(fixture.hook("SessionEnd"));
        require(ended && ended->observations.empty(), "native root END remains queued without optional child storage");
        const auto final = fixture.page(1);
        require(final.observations.front().session_transition == addon::Session_transition::ENDED,
            "malformed or unreadable optional storage cannot reject root END");
        require(window.m_worker.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
            window.m_context.m_result.has_value(), "root-only observation still completes successfully");
    }
    Fixture fixture;
    fixture.append(k_root, "malformed complete row\n");
    const auto ended = fixture.m_observer.classify(fixture.hook("SessionEnd"));
    require(ended && fixture.page(1).observations.front().session_transition == addon::Session_transition::ENDED,
        "a malformed optional row cannot reject an authenticated SessionEnd");
}

void rewritten_journal_rebases_without_inventing_a_stop()
{
    Fixture fixture;
    fixture.admit("child");
    fixture.append(k_root, record(k_root, "child", "attempt", true));
    Observe_window window(fixture);
    const auto started = fixture.page(1);
    const auto capture = fixture.capture();
    fixture.append(k_root, "", true);
    fixture.capture(capture);
    const auto missing = poll(fixture.m_observer, fixture.m_nonce, started.resume_cursor);
    require(missing && missing->observations.empty(), "missing bytes cannot fabricate a child STOP");
    fixture.admit("later");
    fixture.append(k_root, record(k_root, "later", "new", true) + record(k_root, "later", "new", false));
    const auto later = fixture.page(2, started.resume_cursor);
    require(later.observations.front().agent_id != started.observations.front().agent_id &&
        later.observations.back().subagent_transition == addon::Subagent_transition::STOPPED,
        "fresh authenticated evidence remains observable after a journal-local rebase");
    fixture.m_observer.classify(fixture.hook("SessionEnd"));
    require(fixture.page(1, later.resume_cursor).observations.front().session_transition == addon::Session_transition::ENDED,
        "the host owns closure of the unresolved child at root END");
}

void native_child_parent_cannot_change()
{
    Fixture fixture;
    fixture.admit("child");
    fixture.admit("other-parent");
    fixture.append(k_root, record(k_root, "child", "first", true) +
        record(k_root, "other-parent", "parent-attempt", true));
    Observe_window window(fixture);
    const auto started = fixture.page(2);
    fixture.admit("child", "other-parent");
    fixture.append("other-parent", record("other-parent", "child", "second", true));
    fixture.capture();
    const auto invalid = poll(fixture.m_observer, fixture.m_nonce, started.resume_cursor);
    require(invalid && invalid->observations.empty(),
        "an existing native child cannot move to another parent even with a new attempt_id");
    fixture.append(k_root, record(k_root, "child", "first", false) +
        record(k_root, "other-parent", "parent-attempt", false));
    const auto stopped = fixture.page(2, started.resume_cursor);
    require(stopped.observations.front().agent_id == started.observations.front().agent_id &&
        stopped.observations.back().agent_id == started.observations.back().agent_id,
        "rejected lineage evidence does not harm the already admitted children");
}

void malformed_and_unavailable_evidence_remains_local()
{
    Fixture fixture;
    fixture.admit("child");
    fixture.admit("other");
    fixture.append(k_root, record(k_root, "child", "attempt", true) + record(k_root, "other", "attempt", true));
    Observe_window window(fixture);
    const auto started = fixture.page(2);
    fixture.append(k_root, "{bad json}\n" + record(k_root, "child", "attempt", false, "future-status") +
        record(k_root, "other", "attempt", false));
    const auto other = fixture.page(1, started.resume_cursor);
    require(other.observations.front().agent_id == started.observations.back().agent_id,
        "invalid rows and unknown outcomes supply no fact and do not hide another child's finish");
    const auto dispatches = fixture.capture();
    const auto backup = fixture.path(k_root) + QStringLiteral(".saved");
    require(QFile::rename(fixture.path(k_root), backup), "hold the admitted journal bytes");
    require(QDir().mkpath(fixture.path(k_root)), "make the admitted journal temporarily unreadable");
    require(wait_until([&] { return fixture.m_service->statistics().dispatches > dispatches; }),
        "the service must attempt the unavailable capture");
    require(poll(fixture.m_observer, fixture.m_nonce, other.resume_cursor).has_value(),
        "a transient read failure leaves the observation channel available");
    require(QDir().rmdir(fixture.path(k_root)) && QFile::rename(backup, fixture.path(k_root)),
        "restore the exact admitted journal bytes");
    fixture.append(k_root, record(k_root, "child", "attempt", false));
    const auto recovered = fixture.page(1, other.resume_cursor);
    require(recovered.observations.front().agent_id == started.observations.front().agent_id,
        "recovered durable evidence closes only the remaining admitted attempt");
}

void capacity_refuses_new_children_and_preserves_terminal_facts()
{
    Fixture fixture;
    std::string records;
    for (int index = 0; index < 129; ++index) {
        const auto child = "child-" + std::to_string(index);
        fixture.admit(child);
        records += record(k_root, child, "attempt", true);
    }
    fixture.append(k_root, records);
    Observe_window window(fixture);
    fixture.capture();
    std::string cursor;
    std::vector<addon::Lifecycle_observation> admitted;
    for (;;) {
        const auto page = poll(fixture.m_observer, fixture.m_nonce, cursor);
        require(page.has_value(), "the 129th active attempt cannot fail the channel");
        admitted.insert(admitted.end(), page->observations.begin(), page->observations.end());
        cursor = page->resume_cursor;
        if (!page->more) break;
    }
    require(admitted.size() == 128, "local admission respects the actual host active-child bound");
    fixture.append(k_root, record(k_root, "child-128", "attempt", false) +
        record(k_root, "child-0", "attempt", false));
    const auto stopped = fixture.page(1, cursor);
    require(stopped.observations.front().agent_id == admitted.front().agent_id,
        "an unadmitted child has no STOP and an admitted child's STOP remains deliverable");
    fixture.admit("replacement");
    fixture.append(k_root, record(k_root, "replacement", "new", true));
    const auto replacement = fixture.page(1, stopped.resume_cursor);
    require(replacement.observations.front().subagent_transition == addon::Subagent_transition::STARTED,
        "a released active slot can admit a new native attempt");
    fixture.m_observer.classify(fixture.hook("SessionEnd"));
    require(fixture.page(1, replacement.resume_cursor).observations.front().session_transition == addon::Session_transition::ENDED,
        "active-child saturation cannot suppress root END");

    Fixture full;
    full.admit("held");
    full.append(k_root, record(k_root, "held", "attempt", true));
    for (int index = 0; index < 2050; ++index) {
        const auto child = "short-" + std::to_string(index);
        full.admit(child);
        full.append(k_root, record(k_root, child, "attempt", true) + record(k_root, child, "attempt", false));
    }
    full.append(k_root, record(k_root, "held", "attempt", false));
    full.m_observer.classify(full.hook("SessionEnd"));
    cursor.clear();
    std::vector<addon::Lifecycle_observation> saturated;
    for (;;) {
        const auto page = poll(full.m_observer, full.m_nonce, cursor);
        require(page.has_value(), "bounded queue saturation cannot fail polling");
        saturated.insert(saturated.end(), page->observations.begin(), page->observations.end());
        cursor = page->resume_cursor;
        if (!page->more) break;
    }
    require(saturated.size() <= 4096 && saturated.size() < 2 * 2050 + 3 && saturated.size() > 3,
        "the queue refuses new optional spans within its retained bound");
    require(saturated.back().session_transition == addon::Session_transition::ENDED &&
        saturated[saturated.size() - 2].subagent_transition == addon::Subagent_transition::STOPPED &&
        saturated[saturated.size() - 2].agent_id == saturated.front().agent_id,
        "capacity is reserved for every admitted STOP and the final root END");
    require(retire(full.m_observer, full.m_nonce, cursor), "a saturated final page retains normal terminal retirement");
}

void native_root_resume_uses_a_new_epoch()
{
    Fixture fixture;
    fixture.admit("child");
    fixture.append(k_root, record(k_root, "child", "first", true) + record(k_root, "child", "first", false));
    std::string first_agent;
    {
        Observe_window window(fixture);
        first_agent = fixture.page(2).observations.front().agent_id;
    }
    const auto old_nonce = fixture.m_nonce;
    const auto stale = fixture.hook("SubagentStart", "stale");
    auto resumed = fixture.hook("SessionStart");
    resumed.event.body.set("source", Json("load"));
    require(reserve(fixture.m_observer, resumed.transition_nonce), "reserve the new epoch before classification");
    const auto transition = fixture.m_observer.classify(resumed);
    require(transition && transition->transition == addon::Transition_kind::RESUME,
        "resume classification precedes retirement of the previous epoch for the same native session");
    require(retire(fixture.m_observer, old_nonce), "the host retires the previous epoch after its replacement commits");
    fixture.m_nonce = resumed.transition_nonce;
    fixture.m_observer.classify(stale);
    fixture.admit("child");
    fixture.append(k_root, record(k_root, "child", "second", true) + record(k_root, "child", "second", false));
    Observe_window window(fixture);
    const auto result = fixture.page(2);
    require(result.observations.front().agent_id != first_agent && !poll(fixture.m_observer, old_nonce),
        "resumed root and child identities remain isolated from retired cursors and historical records");
}

void terminal_order_and_retirement()
{
    Fixture fixture;
    for (int index = 0; index < 10; ++index) {
        const auto child = "child-" + std::to_string(index);
        fixture.admit(child);
        fixture.append(k_root, record(k_root, child, "attempt", true) + record(k_root, child, "attempt", false));
    }
    const auto ended = fixture.m_observer.classify(fixture.hook("SessionEnd"));
    require(ended && ended->observations.empty(), "root END is ordered through the retained queue");
    const auto first = poll(fixture.m_observer, fixture.m_nonce);
    require(first && first->more && first->observations.size() == 16, "poll obeys the host's sixteen-event page bound");
    const auto final = poll(fixture.m_observer, fixture.m_nonce, first->resume_cursor);
    require(final && !final->more && final->observations.size() == 5 &&
        final->observations.back().session_transition == addon::Session_transition::ENDED,
        "root END belongs to the final page after admitted durable child facts");
    require(!retire(fixture.m_observer, fixture.m_nonce, first->resume_cursor), "retirement requires the exact terminal cursor");
    const auto replay = poll(fixture.m_observer, fixture.m_nonce, first->resume_cursor);
    require(replay && replay->resume_cursor == final->resume_cursor, "terminal page remains replayable until consumed");
    require(retire(fixture.m_observer, fixture.m_nonce, final->resume_cursor), "terminal consumption retires the correlation");
    fixture.m_observer.classify(fixture.hook("SubagentStart", "late"));
    require(!poll(fixture.m_observer, fixture.m_nonce), "late hooks cannot mint state after retirement");
    require(retire(fixture.m_observer, fixture.m_nonce), "retirement is idempotent");
}

void cancellation_and_cursor_ownership()
{
    Fixture fixture;
    Observe_window window(fixture);
    fixture.admit("child");
    fixture.append(k_root, record(k_root, "child", "attempt", true));
    const auto started = fixture.page(1);
    require(!poll(fixture.m_observer, addon::random_nonce_hex(), started.resume_cursor), "cursor cannot cross root epochs");
    require(retire(fixture.m_observer, fixture.m_nonce), "abandonment retires an active watch");
    require(window.m_worker.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
        window.m_context.m_status == addon::Result_status::CANCELLED && window.m_context.m_failure == "correlation_retired",
        "retirement wakes the observe window and suppresses successful completion");
    require(wait_until([&] { return fixture.m_service->statistics().subscriptions == 0; }),
        "retirement releases the shared service subscription");
}

} // namespace

int main()
{
    try {
        authenticated_attempts_and_torn_records();
        descendants_can_arrive_before_their_ancestry();
        repeated_native_children_keep_distinct_attempts();
        optional_child_evidence_cannot_reject_root_lifecycle();
        rewritten_journal_rebases_without_inventing_a_stop();
        native_child_parent_cannot_change();
        malformed_and_unavailable_evidence_remains_local();
        capacity_refuses_new_children_and_preserves_terminal_facts();
        native_root_resume_uses_a_new_epoch();
        terminal_order_and_retirement();
        cancellation_and_cursor_ownership();
        std::cout << "Grok native journal and observer contracts passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
