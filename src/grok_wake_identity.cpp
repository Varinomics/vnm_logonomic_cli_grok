#include "grok_wake_identity.h"
#include "grok_json.h"

namespace json_util = logonomic::cli_addon::json;

namespace grok {

void Wake_session_identity::observe(const Json& event, std::string_view transition_nonce)
{
    if (!json_util::text(event, "subagentType").empty() || !json_util::text(event, "subagentId").empty()) return;
    const auto name = json_util::text(event, "hook_event_name");
    const auto session = json_util::text(event, "sessionId");
    if (session.empty()) return;
    if (name == "SessionEnd" && session == m_session_id) {
        m_session_id.clear();
        m_transition_nonce.clear();
    }
    else if (name == "SessionStart") {
        m_session_id.clear();
        m_transition_nonce.clear();
        if (transition_nonce.size() == 32 &&
            transition_nonce.find_first_not_of("0123456789abcdef") == std::string_view::npos) {
            m_session_id = session;
            m_transition_nonce = transition_nonce;
        }
    }
}

} // namespace grok
