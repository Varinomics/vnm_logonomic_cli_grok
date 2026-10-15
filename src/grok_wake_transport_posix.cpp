#include "grok_wake_transport.h"

#if defined(__linux__) || defined(__APPLE__)

#include <fcntl.h>
#include <poll.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <sys/prctl.h>
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace grok {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr std::size_t k_max_message_bytes = 65536;
constexpr std::size_t k_max_native_line_bytes = 1024 * 1024;

class Descriptor
{
public:
    explicit Descriptor(int value = -1) : m_value(value) {}
    ~Descriptor() { reset(); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return m_value; }
    void reset(int value = -1)
    {
        if (m_value >= 0) ::close(m_value);
        m_value = value;
    }

private:
    int m_value;
};

bool valid_token(std::string_view token)
{
    return token.size() == 32 && std::all_of(token.begin(), token.end(), [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
    });
}

std::string runtime_directory(std::string_view token)
{
    return "/tmp/logonomic-grok-" + std::to_string(::geteuid()) + "-" + std::string(token);
}

sockaddr_un socket_address(const std::string& path)
{
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
#if defined(__APPLE__)
    address.sun_len = sizeof(address);
#endif
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return address;
}

bool same_user(int descriptor)
{
#if defined(__APPLE__)
    uid_t user = 0;
    gid_t group = 0;
    return ::getpeereid(descriptor, &user, &group) == 0 && user == ::geteuid();
#else
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    return ::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
        size == sizeof(credentials) && credentials.uid == ::geteuid();
#endif
}

bool configure_socket(int descriptor, bool nonblocking)
{
    if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0) {
        return false;
    }
#if defined(__APPLE__)
    const int enabled = 1;
    if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        return false;
    }
#endif
    if (nonblocking) {
        const int flags = ::fcntl(descriptor, F_GETFL);
        if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
            return false;
        }
    }
    return true;
}

int open_socket()
{
#if defined(__APPLE__)
    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor >= 0 && !configure_socket(descriptor, true)) {
        ::close(descriptor);
        return -1;
    }
    return descriptor;
#else
    return ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#endif
}

int accept_socket(int listener)
{
#if defined(__APPLE__)
    const int descriptor = ::accept(listener, nullptr, nullptr);
    if (descriptor >= 0 && !configure_socket(descriptor, true)) {
        ::close(descriptor);
        return -1;
    }
    return descriptor;
#else
    return ::accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
#endif
}

ssize_t send_socket(int descriptor, const char* bytes, std::size_t size)
{
#if defined(__APPLE__)
    return ::send(descriptor, bytes, size, 0);
#else
    return ::send(descriptor, bytes, size, MSG_NOSIGNAL);
#endif
}

bool private_path(const std::string& path, bool directory)
{
    struct stat status{};
    return ::lstat(path.c_str(), &status) == 0 && status.st_uid == ::geteuid() &&
        (directory ? S_ISDIR(status.st_mode) : S_ISSOCK(status.st_mode)) &&
        (status.st_mode & 0777) == (directory ? 0700 : 0600);
}

pid_t spawn_native(const std::vector<std::string>& arguments, int input, int output, int error)
{
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
#if defined(__linux__)
    const pid_t parent = ::getpid();
#endif
    const pid_t child = ::fork();
    if (child != 0) return child;

    // The original terminal's PTY owner retains cleanup authority. Linux also
    // ends each direct child when this bridge dies; Darwin's retained PTY session
    // owns its original groups, including ordinary foreground/background jobs.
#if defined(__linux__)
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != parent) ::_exit(126);
#endif
    if ((input >= 0 && ::dup2(input, STDIN_FILENO) < 0) ||
        (output >= 0 && ::dup2(output, STDOUT_FILENO) < 0) ||
        (error >= 0 && ::dup2(error, STDERR_FILENO) < 0))
    {
        ::_exit(126);
    }
    ::execv(argv.front(), argv.data());
    ::_exit(127);
}

std::optional<int> collect_exit(pid_t& child)
{
    if (child <= 0) return std::nullopt;
    int status = 0;
    const pid_t result = ::waitpid(child, &status, WNOHANG);
    if (result != child) return std::nullopt;
    child = -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

bool write_bounded(int descriptor, std::string_view bytes, Clock::time_point deadline)
{
    std::size_t offset = 0;
    while (offset < bytes.size() && Clock::now() < deadline) {
        const ssize_t count = send_socket(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        }
        else
        if (count < 0 && errno == EINTR) {
            continue;
        }
        else
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pending{descriptor, POLLOUT, 0};
            (void)::poll(&pending, 1, 10);
        }
        else {
            return false;
        }
    }
    return offset == bytes.size();
}

class Posix_wake_runtime final : public Wake_runtime
{
public:
    explicit Posix_wake_runtime(std::string directory)

    :
        m_directory(std::move(directory))
    {}

    ~Posix_wake_runtime() override
    {
        for (const pid_t child : {m_tui, m_control, m_leader}) {
            if (child > 0) (void)::kill(child, SIGKILL);
        }
        const auto deadline = Clock::now() + std::chrono::milliseconds(500);
        while ((m_tui > 0 || m_control > 0 || m_leader > 0) && Clock::now() < deadline) {
            (void)collect_exit(m_tui);
            (void)collect_exit(m_control);
            (void)collect_exit(m_leader);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // No recursive deletion: only this runtime's two endpoints and native
        // leader lock can exist here. The outer PTY owner settles descendants.
        for (const auto* name : {"/control.sock", "/leader.sock", "/leader.lock"}) {
            (void)::unlink((m_directory + name).c_str());
        }
        (void)::rmdir(m_directory.c_str());
    }

    bool start(const std::vector<std::string>& native_argv)
    {
        m_listener.reset(open_socket());
        const std::string channel_path = m_directory + "/control.sock";
        const auto address = socket_address(channel_path);
        if (m_listener.get() < 0 ||
            ::bind(m_listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::chmod(channel_path.c_str(), 0600) != 0 || ::listen(m_listener.get(), 4) != 0)
        {
            return false;
        }
        Descriptor null_stream(::open("/dev/null", O_RDWR | O_CLOEXEC));
        if (null_stream.get() < 0) return false;
        const std::string leader_path = m_directory + "/leader.sock";
        m_leader = spawn_native(
            {native_argv.front(), "agent", "--leader-socket", leader_path,
                "leader", "--relay-on-demand", "--no-auto-update"},
            null_stream.get(), null_stream.get(), null_stream.get());
        if (m_leader < 0) return false;
        m_native_admitted = true;

        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (true) {
            if (collect_exit(m_leader).has_value()) return false;
            struct stat endpoint{};
            if (::lstat(leader_path.c_str(), &endpoint) == 0 &&
                S_ISSOCK(endpoint.st_mode) && endpoint.st_uid == ::geteuid()) break;
            if (Clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        int endpoints[2];
#if defined(__APPLE__)
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, endpoints) != 0) return false;
#else
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, endpoints) != 0) return false;
#endif
        m_control_channel.reset(endpoints[0]);
        Descriptor native_channel(endpoints[1]);
        if (!configure_socket(m_control_channel.get(), true) ||
            !configure_socket(native_channel.get(), false))
        {
            return false;
        }
        m_control = spawn_native(
            {native_argv.front(), "agent", "--leader", "--leader-socket", leader_path, "stdio"},
            native_channel.get(), native_channel.get(), null_stream.get());
        if (m_control < 0) return false;
        std::vector<std::string> tui{native_argv.front(), "--leader", "--leader-socket", leader_path};
        tui.insert(tui.end(), native_argv.begin() + 1, native_argv.end());
        m_tui = spawn_native(tui, -1, -1, -1);
        return m_tui > 0;
    }

    bool native_admitted() const { return m_native_admitted; }
    void fail_start() { m_start_failed = true; }

    bool write_control(std::string_view bytes) override
    {
        if (m_start_failed || m_control_channel.get() < 0 || bytes.size() > k_max_message_bytes) return false;
        const bool written = write_bounded(
            m_control_channel.get(), bytes, Clock::now() + std::chrono::milliseconds(250));
        if (!written) m_control_channel.reset();
        return written;
    }

    Wake_poll poll() override
    {
        Wake_poll result;
        if (m_start_failed) {
            result.exit_code = 125;
            return result;
        }
        if (const auto exit = collect_exit(m_tui)) result.exit_code = *exit;
        if (collect_exit(m_leader).has_value()) result.exit_code = 125;
        if (result.exit_code.has_value()) return result;
        if (collect_exit(m_control).has_value()) m_control_channel.reset();

        read_control(result);
        if (m_client.get() < 0) {
            const int client = accept_socket(m_listener.get());
            if (client >= 0) {
                m_client.reset(client);
                m_client_deadline = Clock::now() + std::chrono::seconds(6);
                if (!same_user(client)) disconnect_request(result);
            }
        }
        read_request(result);
        return result;
    }

    void reply(std::string_view bytes) override
    {
        if (m_client.get() >= 0 && bytes.size() <= k_max_message_bytes) {
            std::string response(bytes);
            if (response.empty() || response.back() != '\n') response += '\n';
            (void)write_bounded(m_client.get(), response, Clock::now() + std::chrono::milliseconds(250));
        }
        m_client.reset();
        m_request.clear();
        m_request_delivered = false;
    }

private:
    void read_control(Wake_poll& result)
    {
        std::array<char, 4096> buffer{};
        std::size_t read_bytes = 0;
        while (m_control_channel.get() >= 0 && read_bytes < k_max_message_bytes) {
            const ssize_t count = ::recv(m_control_channel.get(), buffer.data(), buffer.size(), 0);
            if (count > 0) {
                read_bytes += static_cast<std::size_t>(count);
                std::string_view incoming(buffer.data(), static_cast<std::size_t>(count));
                while (!incoming.empty()) {
                    const auto newline = incoming.find('\n');
                    const auto fragment = incoming.substr(0, newline);
                    if (!m_discard_control_line) {
                        if (m_control_buffer.size() + fragment.size() > k_max_native_line_bytes) {
                            m_control_buffer.clear();
                            m_discard_control_line = true;
                        }
                        else {
                            m_control_buffer.append(fragment);
                        }
                    }
                    if (newline == std::string_view::npos) break;
                    if (!m_discard_control_line) result.control_lines.push_back(std::move(m_control_buffer));
                    m_control_buffer.clear();
                    m_discard_control_line = false;
                    incoming.remove_prefix(newline + 1);
                }
            }
            else
            if (count < 0 && errno == EINTR) {
                continue;
            }
            else {
                if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) m_control_channel.reset();
                break;
            }
        }
        result.control_closed = m_control_channel.get() < 0;
    }

    void disconnect_request(Wake_poll& result)
    {
        m_client.reset();
        m_request.clear();
        m_request_delivered = false;
        result.request_disconnected = true;
    }

    void read_request(Wake_poll& result)
    {
        if (m_client.get() < 0) return;
        if (Clock::now() >= m_client_deadline) {
            disconnect_request(result);
            return;
        }
        std::array<char, 4096> buffer{};
        while (m_client.get() >= 0) {
            const ssize_t count = ::recv(m_client.get(), buffer.data(), buffer.size(), 0);
            if (count > 0) {
                if (m_request_delivered) {
                    disconnect_request(result);
                    return;
                }
                m_request.append(buffer.data(), static_cast<std::size_t>(count));
                if (m_request.size() > k_max_message_bytes) {
                    disconnect_request(result);
                    return;
                }
                const auto newline = m_request.find('\n');
                if (newline != std::string::npos) {
                    if (newline + 1 != m_request.size()) {
                        disconnect_request(result);
                        return;
                    }
                    result.request = m_request.substr(0, newline);
                    m_request_delivered = true;
                    return;
                }
            }
            else
            if (count < 0 && errno == EINTR) {
                continue;
            }
            else {
                if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) disconnect_request(result);
                return;
            }
        }
    }

    std::string m_directory;
    Descriptor m_listener;
    Descriptor m_control_channel;
    Descriptor m_client;
    pid_t m_leader = -1;
    pid_t m_control = -1;
    pid_t m_tui = -1;
    bool m_native_admitted = false;
    bool m_start_failed = false;
    std::string m_control_buffer;
    bool m_discard_control_line = false;
    std::string m_request;
    bool m_request_delivered = false;
    Clock::time_point m_client_deadline;
};

} // namespace

std::unique_ptr<Wake_runtime> start_wake_runtime(
    const std::string& token, const std::vector<std::string>& native_argv)
{
    if (!valid_token(token) || native_argv.empty() || native_argv.front().empty() ||
        native_argv.front().front() != '/') return {};
    const auto directory = runtime_directory(token);
    if (::mkdir(directory.c_str(), 0700) != 0) return {};
    auto runtime = std::make_unique<Posix_wake_runtime>(directory);
    if (!runtime->start(native_argv)) {
        if (!runtime->native_admitted()) return {};
        // The original terminal owner must settle all admitted descendants
        // before another native execution can start. An exec fallback here
        // would keep that owner's root alive and postpone its settlement.
        std::fputs("Grok's isolated session could not start; closing its admitted native processes.\n", stderr);
        runtime->fail_start();
    }
    return runtime;
}

int run_standalone_native(const std::vector<std::string>& native_argv)
{
    if (native_argv.empty()) return 127;
    std::vector<std::string> arguments{native_argv.front(), "--no-leader"};
    arguments.insert(arguments.end(), native_argv.begin() + 1, native_argv.end());
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    ::execv(argv.front(), argv.data());
    return 127;
}

Wake_channel_result exchange_wake_request(
    std::string_view token, std::string_view request,
    const std::function<bool(int)>& wait_for_cancel)
{
    Wake_channel_result result;
    if (!valid_token(token) || request.size() >= k_max_message_bytes || wait_for_cancel(0)) return result;
    const auto directory = runtime_directory(token);
    const auto path = directory + "/control.sock";
    if (!private_path(directory, true) || !private_path(path, false)) return result;
    Descriptor channel(open_socket());
    if (channel.get() < 0) return result;
    const auto address = socket_address(path);
    const int connected = ::connect(channel.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (connected != 0) return result;
    if (!same_user(channel.get())) return result;

    std::string message(request);
    if (message.empty() || message.back() != '\n') message += '\n';
    std::size_t offset = 0;
    std::string response;
    const auto deadline = Clock::now() + std::chrono::seconds(6);
    std::array<char, 4096> buffer{};
    while (Clock::now() < deadline && !wait_for_cancel(0)) {
        if (offset < message.size()) {
            const ssize_t count = send_socket(
                channel.get(), message.data() + offset, message.size() - offset);
            if (count > 0) {
                result.request_written = true;
                offset += static_cast<std::size_t>(count);
            }
            else
            if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                return result;
            }
        }
        else {
            const ssize_t count = ::recv(channel.get(), buffer.data(), buffer.size(), 0);
            if (count > 0) {
                response.append(buffer.data(), static_cast<std::size_t>(count));
                if (response.size() > k_max_message_bytes) return result;
                const auto newline = response.find('\n');
                if (newline != std::string::npos) {
                    if (newline + 1 == response.size()) result.response = response.substr(0, newline);
                    return result;
                }
            }
            else
            if (count == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
                return result;
            }
        }
        if (wait_for_cancel(10)) return result;
    }
    return result;
}

std::string session_bridge_path()
{
    std::error_code error;
#if defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (size == 0 || _NSGetExecutablePath(path.data(), &size) != 0) {
        return {};
    }
    const auto executable = fs::canonical(path.data(), error);
#else
    const auto executable = fs::read_symlink("/proc/self/exe", error);
#endif
    if (error) return {};
    return (executable.parent_path().parent_path() / "helpers" / "grok_session_bridge").string();
}

} // namespace grok

#endif
