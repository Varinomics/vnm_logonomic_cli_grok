#include "grok_wake_transport.h"

#if defined(__linux__) || defined(__APPLE__)

#include <logonomic_cli_addon/ids.h>
#include <fcntl.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
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
    return addon::random_nonce_hex();
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
        std::ofstream(directory / "fixture-control.pid") << ::getpid();
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == "close-control") {
                if (::close(STDIN_FILENO) != 0 || ::close(STDOUT_FILENO) != 0) return 4;
                std::ofstream(directory / "fixture-control-closed") << "closed\n";
                while (true) ::pause();
            }
            if (line == "oversized-line") std::cout << std::string(1024 * 1024 + 1, 'a') << '\n' << std::flush;
            else std::cout << line << '\n' << std::flush;
        }
        return 0;
    }
    const bool leader = std::find(arguments.begin(), arguments.end(), "leader") != arguments.end();
    std::ofstream(directory / (leader ? "fixture-leader.pid" : "fixture-tui.pid")) << ::getpid();
    if (leader && std::getenv("GROK_TEST_HOLD_LEADER")) {
        while (true) {
            ::pause();
        }
    }
    int listener = -1;
    if (leader) {
        listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
        if (listener < 0 || ::fcntl(listener, F_SETFD, FD_CLOEXEC) != 0 ||
            ::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
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
        for (const auto* name : {
            "fixture-leader.pid", "fixture-control.pid", "fixture-tui.pid", "fixture-control-closed"})
        {
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
    // Fixed sleeps throttle this oversized fixture across small socket buffers.
    while (Clock::now() < large_deadline && !preserved) {
        const auto events = fixture.m_runtime->poll();
        require(!events.control_closed, "oversized native event does not disconnect original observer");
        for (const auto& line : events.control_lines) {
            require(line.size() <= 1024 * 1024, "native event memory bound");
            if (line == "stream-preserved") preserved = true;
        }
        std::this_thread::yield();
    }
    require(preserved, "native stream remains framed after oversized event");

    require(fixture.m_runtime->write_control("close-control\n"), "request native peer closure");
    const auto close_deadline = Clock::now() + std::chrono::seconds(2);
    const auto closed_marker = fs::path(fixture.m_directory) / "fixture-control-closed";
    // Observe closure without letting runtime polling retire the sending socket.
    while (Clock::now() < close_deadline && !fs::exists(closed_marker)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(fs::exists(closed_marker), "native peer closes both socket descriptors before the write");
    require(!fixture.m_runtime->write_control("unaccepted-after-close\n"),
        "closed native peer fails without SIGPIPE terminating the bridge");
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

class Bridge_process
{
public:
    Bridge_process(
        const std::string& bridge, const std::string& native, bool hold_leader, bool ignore_interrupt = false)

    :
        m_token(unique_token()),
        m_directory(directory_for(m_token))
    {
        m_pid = ::fork();
        require(m_pid >= 0, "fork isolated bridge fixture");
        if (m_pid == 0) {
            // An isolated fixture group lets failure cleanup settle its children
            // without addressing any process in the surrounding test runner.
            if (::setpgid(0, 0) != 0) {
                ::_exit(126);
            }
            sigset_t signals;
            ::sigemptyset(&signals);
            for (const int signal : {SIGHUP, SIGINT, SIGTERM}) {
                struct sigaction action{};
                action.sa_handler = ignore_interrupt && signal == SIGINT ? SIG_IGN : SIG_DFL;
                ::sigemptyset(&action.sa_mask);
                if (::sigaction(signal, &action, nullptr) != 0) {
                    ::_exit(126);
                }
                ::sigaddset(&signals, signal);
            }
            if (::sigprocmask(SIG_UNBLOCK, &signals, nullptr) != 0) {
                ::_exit(126);
            }
            if (hold_leader) {
                ::setenv("GROK_TEST_HOLD_LEADER", "1", 1);
            }
            else {
                ::unsetenv("GROK_TEST_HOLD_LEADER");
            }
            ::execl(bridge.c_str(), bridge.c_str(), "--serve", m_token.c_str(), native.c_str(), nullptr);
            ::_exit(127);
        }
    }

    ~Bridge_process()
    {
        // await_exit deliberately leaves the bridge waitable, so its PID and
        // group identity cannot be reused before this fixture-owned cleanup.
        (void)::kill(-m_pid, SIGKILL);
        (void)::kill(m_pid, SIGKILL);
        int status = 0;
        while (::waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {}
        while (::waitpid(-m_pid, &status, 0) > 0 || errno == EINTR) {}
        for (const auto* name : {
            "control.sock", "leader.sock", "leader.lock", "fixture-leader.pid",
            "fixture-control.pid", "fixture-tui.pid"})
        {
            (void)::unlink((m_directory + "/" + name).c_str());
        }
        (void)::rmdir(m_directory.c_str());
    }

    std::vector<pid_t> await_native(bool hold_leader) const
    {
        std::vector<pid_t> children;
        const auto deadline = Clock::now() + std::chrono::seconds(4);
        for (const auto* role : {"leader", "control", "tui"}) {
            const auto path = m_directory + "/fixture-" + role + ".pid";
            pid_t pid = -1;
            while (Clock::now() < deadline && pid <= 1) {
                std::ifstream(path) >> pid;
                if (pid <= 1) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            require(pid > 1, "bridge admits expected inert native child");
            children.push_back(pid);
            require(::unlink(path.c_str()) == 0, "remove fixture marker before runtime cleanup");
            if (hold_leader) {
                break;
            }
        }
        return children;
    }

    siginfo_t await_exit() const
    {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < deadline) {
            siginfo_t status{};
            require(::waitid(P_PID, m_pid, &status, WEXITED | WNOHANG | WNOWAIT) == 0,
                "observe bridge exit without releasing its process identity");
            if (status.si_pid == m_pid) {
                return status;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        throw std::runtime_error("signal did not promptly stop the bridge");
    }

    const std::string m_token;
    const std::string m_directory;
    pid_t m_pid = -1;
};

void bridge_signal_contract(const std::string& executable, const std::string& bridge)
{
#if defined(__linux__)
    // Reap the fixture's orphaned descendants even when testing a broken bridge.
    require(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "fixture can reap its orphaned native children");
#endif
    Fixture peer(executable);
    for (const bool hold_leader : {false, true}) {
        for (const int signal : {SIGHUP, SIGINT, SIGTERM}) {
            Bridge_process process(bridge, executable, hold_leader);
            const auto children = process.await_native(hold_leader);
            require(::kill(process.m_pid, signal) == 0, "signal only the owned bridge root");
            const auto status = process.await_exit();
            require(status.si_code == CLD_KILLED && status.si_status == signal,
                "cleanup preserves the original terminating signal");
            require(!fs::exists(process.m_directory), "signal removes the owned wake runtime directory");
            for (const auto child : children) {
                require(::kill(child, 0) < 0 && errno == ESRCH,
                    "bridge settles and reaps every admitted direct child before exiting");
            }
            require(fs::exists(peer.m_directory + "/control.sock") &&
                !peer.m_runtime->poll().exit_code.has_value(),
                "signal cleanup preserves the separate live wake runtime");
        }
    }

    Bridge_process ignored(bridge, executable, false, true);
    (void)ignored.await_native(false);
    require(::kill(ignored.m_pid, SIGINT) == 0, "send an inherited ignored signal");
    Test_context context;
    const auto response = grok::exchange_wake_request(
        ignored.m_token, R"({"operation":"inspect","payload":{}})", context);
    require(response.response.has_value(), "inherited SIG_IGN remains ignored while serving requests");
    require(::kill(ignored.m_pid, SIGTERM) == 0, "terminate the ignored-interrupt fixture");
    const auto status = ignored.await_exit();
    require(status.si_code == CLD_KILLED && status.si_status == SIGTERM &&
        !fs::exists(ignored.m_directory), "other signals still clean up an ignored-interrupt launch");
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments(argv, argv + argc);
    const bool bridge_contract = argc == 3 && arguments[1] == "--session-bridge";
    if (argc > 1 && !bridge_contract) return native_fixture(arguments);
    try {
        require(std::signal(SIGPIPE, SIG_DFL) != SIG_ERR, "SIGPIPE has its default disposition");
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGPIPE);
        require(::sigprocmask(SIG_UNBLOCK, &signals, nullptr) == 0, "SIGPIPE is unblocked");
        const auto executable = fs::canonical(argv[0]).string();
        require(grok::session_bridge_path() ==
            (fs::path(executable).parent_path().parent_path() / "helpers" / "grok_session_bridge").string(),
            "session bridge resolves beside the original package executable");
        transport_contract(executable);
        uncertain_delivery_contract(executable);
        unavailable_and_exit_contract(executable);
        if (bridge_contract) {
            bridge_signal_contract(executable, arguments[2]);
        }
        std::cout << "POSIX wake transport contracts passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#endif
