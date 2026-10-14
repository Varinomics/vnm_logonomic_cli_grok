#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grok {

enum class Wake_outcome
{
    PENDING,
    ACCEPTED,
    UNAVAILABLE,
    DEFERRED,
    UNCERTAIN,
};

struct Wake_result
{
    std::string  notification_id;
    Wake_outcome outcome = Wake_outcome::PENDING;
    std::string  reason;
};

struct Wake_exchange
{
    std::string              stdin_bytes;
    std::vector<Wake_result> results;
    bool                     writes_prompt = false;
};

// One instance belongs to one persistent native ACP connection. The caller
// serializes operations and retains the original TUI as the permission owner.
class Wake_protocol
{
public:
    std::string   initialize();
    Wake_exchange inspect(std::string session_id, std::string cwd);
    Wake_exchange notify(std::string notification_id, std::string prompt);
    Wake_exchange receive(std::string_view complete_json_line);

    // Only an authenticated session observation from this managed launch may
    // confirm the native identity. These methods produce no native RPC.
    void          confirm_session(std::string session_id);
    Wake_exchange forget_session();

    // Call immediately before attempting to write an exchange with
    // writes_prompt=true: a partial write can already have submitted a turn.
    void          prompt_write_started();
    Wake_exchange abandon_request(std::string reason);
    Wake_exchange transport_closed(std::string reason);

    bool               initialized()       const { return m_initialized; }
    bool               ready()             const { return m_ready; }
    const std::string& failure_reason()    const { return m_failure_reason; }
    const std::string& agent_instance_id() const { return m_agent_instance_id; }
    const std::string& session_id()        const { return m_session_id; }

private:
    struct Pending_notification
    {
        std::string notification_id;
        std::string prompt;
        std::string request_id;
        bool        write_started = false;
    };

    using Notification_key = std::pair<std::string, std::string>;

    std::string   query_session();
    Wake_exchange finish_notification(Wake_outcome outcome, std::string reason);
    Wake_exchange fail(std::string reason);

    bool        m_initialization_started = false;
    bool        m_initialized            = false;
    bool        m_ready                  = false;
    bool        m_closed                 = false;
    std::string m_agent_instance_id;
    std::string m_failure_reason;
    std::string m_session_id;
    std::string m_observed_session_id;
    std::string m_cwd;
    std::string m_info_request_id;
    uint64_t    m_next_request = 1;

    std::optional<Pending_notification>       m_pending;
    std::map<Notification_key, Wake_result>   m_completed;
    std::map<std::string, Notification_key>   m_prompt_requests;
};

} // namespace grok
