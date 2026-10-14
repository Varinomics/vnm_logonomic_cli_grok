#include "grok_wake_identity.h"
#include "grok_json.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
grok::Json event(const std::string& name, const std::string& session)
{
    return grok::object({{"hook_event_name", grok::Json(name)}, {"sessionId", grok::Json(session)}});
}
}

int main()
{
    try {
        const std::string first(32, 'a');
        const std::string second(32, 'b');
        grok::Wake_session_identity identity;
        identity.observe(event("UserPromptSubmit", "session-one"), first);
        require(identity.session_id().empty(), "ordinary event supplied an unauthenticated session transition");
        identity.observe(event("SessionStart", "session-one"), first);
        require(identity.session_id() == "session-one" && identity.transition_nonce() == first,
            "accepted start did not retain exact current identity");
        identity.observe(event("StopCancelled", "session-one"), second);
        require(identity.transition_nonce() == first, "turn state changed session correlation");
        auto child = event("SessionStart", "child");
        child.set("subagentType", grok::Json("researcher"));
        identity.observe(child, second);
        require(identity.session_id() == "session-one", "child session replaced root identity");
        identity.observe(event("SessionEnd", "other-session"), first);
        require(identity.session_id() == "session-one", "unrelated session ended current identity");
        identity.observe(event("SessionStart", "session-one"), second);
        require(identity.session_id() == "session-one" && identity.transition_nonce() == second,
            "resume of same native UUID retained previous epoch correlation");
        identity.observe(event("SessionStart", "session-two"), first);
        require(identity.session_id() == "session-two" && identity.transition_nonce() == first,
            "native session transition retained previous root identity");
        identity.observe(event("SessionEnd", "session-two"), second);
        require(identity.session_id().empty() && identity.transition_nonce().empty(), "ended session retained identity");
        identity.observe(event("SessionStart", "session-one"), "invalid");
        require(identity.session_id().empty(), "invalid authenticated transition nonce accepted");
        std::cout << "authenticated native session identity passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
