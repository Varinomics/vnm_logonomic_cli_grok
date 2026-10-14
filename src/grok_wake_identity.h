#pragma once

#include <logonomic_cli_addon/json.h>
#include <string>
#include <string_view>

namespace grok {

// Current native identity admitted by the host for this exact original launch.
// This tracks session ownership, independently of busy, pause or approval state.
class Wake_session_identity
{
public:
    void observe(const logonomic::cli_addon::Json_value& event, std::string_view transition_nonce);
    const std::string& session_id() const { return m_session_id; }
    const std::string& transition_nonce() const { return m_transition_nonce; }

private:
    std::string m_session_id;
    std::string m_transition_nonce;
};

} // namespace grok
