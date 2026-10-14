#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <memory>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grok {

// One private runtime belongs to one original terminal launch. Its native ACP
// client stays connected for that lifetime; a notification never starts or
// reconnects a provider process.
struct Wake_poll
{
    std::vector<std::string> control_lines;
    std::optional<std::string> request;
    bool control_closed = false;
    bool request_disconnected = false;
    std::optional<int> exit_code;
};

class Wake_runtime
{
public:
    virtual ~Wake_runtime() = default;
    virtual bool write_control(std::string_view bytes) = 0;
    virtual Wake_poll poll() = 0;
    virtual void reply(std::string_view bytes) = 0;
};

std::unique_ptr<Wake_runtime> start_wake_runtime(
    const std::string& token, const std::vector<std::string>& native_argv);
int run_standalone_native(const std::vector<std::string>& native_argv);

struct Wake_channel_result
{
    std::optional<std::string> response;
    bool request_written = false;
};

Wake_channel_result exchange_wake_request(
    std::string_view token, std::string_view request,
    const std::function<bool(int)>& wait_for_cancel);

inline Wake_channel_result exchange_wake_request(std::string_view token, std::string_view request,
    logonomic::cli_addon::Invocation_context& context)
{
    return exchange_wake_request(token, request, [&](int delay) {
        return delay ? context.wait_for_cancel(delay) : context.cancel_requested();
    });
}

// Resolves the indexed helper alongside the currently executing adapter. The
// host retains that package generation for the prepared terminal's lifetime.
std::string session_bridge_path();

} // namespace grok
