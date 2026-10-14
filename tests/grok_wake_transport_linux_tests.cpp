#include "grok_wake_transport.h"

#if defined(__linux__)

#include <fcntl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class Test_context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return m_cancelled.load(); }
    bool wait_for_cancel(int timeout_ms) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return cancel_requested();
    }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value) override {}
    void fail(addon::Result_status, std::string, std::string) override {}
    bool finished() const override { return false; }
    void cancel() { m_cancelled = true; }

private:
    addon::Invoke_request m_request;
    std::atomic_bool m_cancelled = false;
};

std::string unique_token()
{
    std::array<unsigned char, 16> bytes{};
    require(::getrandom(bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size()), "random token");
    std::string result;
    constexpr char k_digits[] = "0123456789abcdef";
    for (const auto value : bytes) {
        result += k_digits[value >> 4];
        result += k_digits[value & 15];
    }
    return result;
}

std::string directory_for(std::string_view token)
{
    return "/tmp/logonomic-grok-" + std::to_string(::geteuid()) + "-" + std::string(token);
}

std::string option(const std::vector<std::string>& arguments, const std::string& name)
{
    const auto found = std::find(arguments.begin(), arguments.end(), name);
    return found != arguments.end() && found + 1 != arguments.end() ? *(found + 1) : std::string{};
}

int native_fixture(const std::vector<std::string>& arguments)
{
    const auto socket_path = option(arguments, "--leader-socket");
    if (socket_path.empty()) return 2;
    const auto directory = fs::path(socket_path).parent_path();
    if (arguments.back() == "stdio") {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == "oversized-line") std::cout << std::string(1024 * 1024 + 1, 'a') << '\n' << std::flush;
            else std::cout << line << '\n' << std::flush;
        }
        return 0;
    }
    const bool leader = std::find(arguments.begin(), arguments.end(), "leader") != arguments.end();
    std::ofstream(directory / (leader ? "fixture-leader.pid" : "fixture-tui.pid")) << ::getpid();
    int listener = -1;
    if (leader) {
        listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
        if (listener < 0 || ::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::chmod(socket_path.c_str(), 0600) != 0 || ::listen(listener, 4) != 0) return 3;
    }
    while (true) ::pause();
}

class Fixture
{
public:
    explicit Fixture(const std::string& executable)

    :
        m_token(unique_token()),
        m_directory(directory_for(m_token)),
        m_runtime(grok::start_wake_runtime(m_token, {executable}))
    {
        require(m_runtime != nullptr, "runtime starts with explicit native leader");
    }

    ~Fixture()
    {
        m_runtime.reset();
        for (const auto* name : {"fixture-leader.pid", "fixture-tui.pid"}) {
            (void)::unlink((m_directory + "/" + name).c_str());
        }
        (void)::rmdir(m_directory.c_str());
    }

    std::string m_token;
    std::string m_directory;
    std::unique_ptr<grok::Wake_runtime> m_runtime;
};

grok::Wake_poll await_request(Fixture& fixture)
{
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < deadline) {
        auto value = fixture.m_runtime->poll();
        if (value.request.has_value()) return value;
        require(!value.exit_code.has_value(), "native fixture remains alive");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("private request timeout");
}

void transport_contract(const std::string& executable)
{
    Fixture fixture(executable);
    struct stat directory_status{};
    struct stat socket_status{};
    require(::stat(fixture.m_directory.c_str(), &directory_status) == 0 &&
        (directory_status.st_mode & 0777) == 0700, "runtime directory is private");
    require(::stat((fixture.m_directory + "/control.sock").c_str(), &socket_status) == 0 &&
        (socket_status.st_mode & 0777) == 0600, "runtime socket is private");

    Test_context context;
    auto result = std::async(std::launch::async, [&]() {
        return grok::exchange_wake_request(fixture.m_token, "{\"id\":1}", context);
    });
    const auto request = await_request(fixture);
    require(request.request == "{\"id\":1}", "one complete JSON request");
    fixture.m_runtime->reply("{\"accepted\":true}");
    const auto response = result.get();
    require(response.request_written && response.response == "{\"accepted\":true}", "private response delivered");

    require(fixture.m_runtime->write_control("resident-session-marker\n"), "retained ACP client write");
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    bool echoed = false;
    while (Clock::now() < deadline && !echoed) {
        const auto events = fixture.m_runtime->poll();
        echoed = std::find(events.control_lines.begin(), events.control_lines.end(),
            "resident-session-marker") != events.control_lines.end();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(echoed, "retained ACP client newline framing");
    require(!fixture.m_runtime->write_control(std::string(65537, 'a')), "outbound ACP bound");
    require(fixture.m_runtime->write_control("oversized-line\nstream-preserved\n"), "large native event fixture");
    const auto large_deadline = Clock::now() + std::chrono::seconds(2);
    bool preserved = false;
    while (Clock::now() < large_deadline && !preserved) {
        const auto events = fixture.m_runtime->poll();
        require(!events.control_closed, "oversized native event does not disconnect original observer");
        for (const auto& line : events.control_lines) {
            require(line.size() <= 1024 * 1024, "native event memory bound");
            if (line == "stream-preserved") preserved = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(preserved, "native stream remains framed after oversized event");
}

void uncertain_delivery_contract(const std::string& executable)
{
    Fixture fixture(executable);
    Test_context context;
    auto result = std::async(std::launch::async, [&]() {
        return grok::exchange_wake_request(fixture.m_token, "{\"notify\":true}", context);
    });
    (void)await_request(fixture);
    context.cancel();
    const auto response = result.get();
    require(response.request_written && !response.response.has_value(), "cancel after send preserves uncertainty");
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    bool disconnected = false;
    while (Clock::now() < deadline && !disconnected) {
        disconnected = fixture.m_runtime->poll().request_disconnected;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(disconnected, "server sees cancelled client departure");
}

void unavailable_and_exit_contract(const std::string& executable)
{
    Test_context context;
    const auto missing = grok::exchange_wake_request(unique_token(), "{}", context);
    require(!missing.request_written && !missing.response.has_value(), "missing runtime cannot submit work");
    require(!grok::start_wake_runtime("../escape", {executable}), "invalid token cannot name an endpoint");
    auto failed = grok::start_wake_runtime(unique_token(), {"/nonexistent/logonomic-grok-native-fixture"});
    require(failed && failed->poll().exit_code == 125,
        "failure after native admission requires root exit before another native launch");
    failed.reset();

    Fixture fixture(executable);
    context.cancel();
    const auto cancelled = grok::exchange_wake_request(fixture.m_token, "{}", context);
    require(!cancelled.request_written, "cancel before send is determinate");
    pid_t leader = -1;
    std::ifstream(fixture.m_directory + "/fixture-leader.pid") >> leader;
    require(leader > 1 && ::kill(leader, SIGKILL) == 0, "terminate owned leader fixture");
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    std::optional<int> exited;
    while (Clock::now() < deadline && !exited.has_value()) {
        exited = fixture.m_runtime->poll().exit_code;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(exited == 125, "leader loss terminates original runtime instead of reconnecting");
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments(argv, argv + argc);
    if (argc > 1) return native_fixture(arguments);
    try {
        const auto executable = fs::canonical("/proc/self/exe").string();
        transport_contract(executable);
        uncertain_delivery_contract(executable);
        unavailable_and_exit_contract(executable);
        std::cout << "Linux wake transport contracts passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#endif
