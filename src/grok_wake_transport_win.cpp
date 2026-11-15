#include "grok_wake_transport.h"

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <set>
#include <thread>

namespace grok {
namespace {
constexpr std::size_t k_request_limit = 65536;
using Clock = std::chrono::steady_clock;

struct Handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.value) { other.value = INVALID_HANDLE_VALUE; }
    Handle& operator=(Handle&& other) noexcept
    {
        reset(); value = other.value; other.value = INVALID_HANDLE_VALUE; return *this;
    }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
    void reset(HANDLE next = INVALID_HANDLE_VALUE)
    {
        if (*this) CloseHandle(value);
        value = next;
    }
};

struct io_result_t
{
    DWORD error;
    DWORD count;
};

class Pipe_operation
{
public:
    Pipe_operation() { m_overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~Pipe_operation()
    {
        cancel();
        if (m_overlapped.hEvent) {
            CloseHandle(m_overlapped.hEvent);
        }
    }
    Pipe_operation(const Pipe_operation&) = delete;
    Pipe_operation& operator=(const Pipe_operation&) = delete;

    bool valid() const { return m_overlapped.hEvent != nullptr; }
    bool active() const { return m_file != INVALID_HANDLE_VALUE; }
    HANDLE event() const { return m_overlapped.hEvent; }
    const char* data() const { return m_buffer.data(); }

    void connect(HANDLE file)
    {
        begin(file);
        complete(ConnectNamedPipe(file, &m_overlapped));
        if (m_result.error == ERROR_PIPE_CONNECTED) {
            m_result.error = ERROR_SUCCESS;
        }
    }

    void read(HANDLE file, std::size_t size)
    {
        m_buffer.resize(size);
        begin(file);
        complete(ReadFile(file, m_buffer.data(), (DWORD)m_buffer.size(), &m_result.count, &m_overlapped));
    }

    void write(HANDLE file, std::string_view bytes)
    {
        m_buffer.assign(bytes.begin(), bytes.end());
        begin(file);
        complete(WriteFile(file, m_buffer.data(), (DWORD)m_buffer.size(), &m_result.count, &m_overlapped));
    }

    std::optional<io_result_t> take()
    {
        if (!active()) {
            return std::nullopt;
        }
        if (m_result.error == ERROR_IO_PENDING) {
            if (!GetOverlappedResult(m_file, &m_overlapped, &m_result.count, FALSE)) {
                const auto error = GetLastError();
                if (error == ERROR_IO_INCOMPLETE) {
                    return std::nullopt;
                }
                m_result.error = error;
            }
            else {
                m_result.error = ERROR_SUCCESS;
            }
        }
        m_file = INVALID_HANDLE_VALUE;
        return m_result;
    }

    void cancel()
    {
        if (active() && m_result.error == ERROR_IO_PENDING) {
            // CancelIoEx requests cancellation; only completion releases the
            // kernel's access to this OVERLAPPED, event and buffer.
            CancelIoEx(m_file, &m_overlapped);
            DWORD count = 0;
            GetOverlappedResult(m_file, &m_overlapped, &count, TRUE);
        }
        m_file = INVALID_HANDLE_VALUE;
    }

private:
    void begin(HANDLE file)
    {
        ResetEvent(m_overlapped.hEvent);
        const auto event = m_overlapped.hEvent;
        m_overlapped = {};
        m_overlapped.hEvent = event;
        m_result = {};
        m_file = file;
    }

    void complete(BOOL success)
    {
        m_result.error = success ? ERROR_SUCCESS : GetLastError();
        if (m_result.error != ERROR_IO_PENDING) {
            SetEvent(m_overlapped.hEvent);
        }
    }

    OVERLAPPED m_overlapped{};
    HANDLE m_file = INVALID_HANDLE_VALUE;
    io_result_t m_result{};
    std::vector<char> m_buffer;
};

std::wstring wide(std::string_view value)
{
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!length) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::string utf8(std::wstring_view value)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring quote(std::wstring_view value)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += ch;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

std::wstring pipe_name(std::string_view token)
{
    if (token.size() != 32 || token.find_first_not_of("0123456789abcdef") != std::string_view::npos) return {};
    return L"\\\\.\\pipe\\Logonomic.Grok." + wide(token);
}

Handle duplicate(HANDLE original)
{
    HANDLE result = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &result, 0, TRUE, DUPLICATE_SAME_ACCESS)) return {};
    return Handle(result);
}

Handle spawn(const std::vector<std::string>& argv, HANDLE job, const std::array<HANDLE, 3>& streams, bool interactive)
{
    std::wstring command;
    for (const auto& argument : argv) {
        if (!command.empty()) command += L' ';
        command += quote(wide(argument));
    }
    std::array<Handle, 3> inherited{duplicate(streams[0]), duplicate(streams[1]), duplicate(streams[2])};
    if (!inherited[0] || !inherited[1] || !inherited[2]) return {};
    std::array<HANDLE, 3> handles{inherited[0].value, inherited[1].value, inherited[2].value};
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) return {};
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb         = sizeof(startup);
    startup.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput  = handles[0];
    startup.StartupInfo.hStdOutput = handles[1];
    startup.StartupInfo.hStdError  = handles[2];
    startup.lpAttributeList       = attributes;
    PROCESS_INFORMATION process{};
    const DWORD hidden = interactive && GetConsoleCP() ? 0 : CREATE_NO_WINDOW;
    const bool started = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        handles.data(), sizeof(handles), nullptr, nullptr) &&
        CreateProcessW(wide(argv.front()).c_str(), command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | hidden,
            nullptr, nullptr, &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributes);
    if (!started) return {};
    Handle child(process.hProcess);
    Handle thread(process.hThread);
    // Assign before the first instruction: even native reconnect/spawn paths
    // cannot escape this launch's nested job or outlive the original TUI.
    if ((job && !AssignProcessToJobObject(job, child.value)) || ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        TerminateProcess(child.value, 3);
        WaitForSingleObject(child.value, INFINITE);
        return {};
    }
    return child;
}

std::set<std::wstring> leader_pipes()
{
    std::set<std::wstring> result;
    WIN32_FIND_DATAW data{};
    const HANDLE search = FindFirstFileW(L"\\\\.\\pipe\\grok-leader-*", &data);
    if (search == INVALID_HANDLE_VALUE) return result;
    do { result.emplace(data.cFileName); } while (FindNextFileW(search, &data));
    FindClose(search);
    return result;
}

bool wait_for_leader(HANDLE leader, const std::set<std::wstring>& preceding)
{
    const DWORD leader_pid = GetProcessId(leader);
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < deadline && WaitForSingleObject(leader, 0) == WAIT_TIMEOUT) {
        for (const auto& name : leader_pipes()) {
            if (preceding.contains(name)) continue;
            Handle pipe(CreateFileW((L"\\\\.\\pipe\\" + name).c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, 0, nullptr));
            ULONG server = 0;
            if (pipe && GetNamedPipeServerProcessId(pipe.value, &server) && server == leader_pid) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

Handle create_owned_pipe(const std::wstring& name, DWORD access, DWORD mode)
{
    if (name.empty()) return {};
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) return {};
    Handle process_token(raw_token);
    DWORD size = 0;
    GetTokenInformation(process_token.value, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> user(size);
    if (!GetTokenInformation(process_token.value, TokenUser, user.data(), size, &size)) return {};
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) return {};
    const std::wstring descriptor = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR security = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1, &security, nullptr)) return {};
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), security, FALSE};
    Handle pipe(CreateNamedPipeW(name.c_str(), access | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        mode | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, k_request_limit, k_request_limit, 0, &attributes));
    LocalFree(security);
    return pipe;
}

bool create_control_pipe(const std::wstring& name, bool parent_reads, Handle& parent, Handle& child)
{
    parent = create_owned_pipe(name, parent_reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE);
    Pipe_operation connection;
    if (!parent || !connection.valid()) {
        return false;
    }
    connection.connect(parent.value);
    child.reset(CreateFileW(name.c_str(), parent_reads ? GENERIC_WRITE : GENERIC_READ,
        0, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!child || WaitForSingleObject(connection.event(), 5000) != WAIT_OBJECT_0) {
        return false;
    }
    const auto connected = connection.take();
    return connected && connected->error == ERROR_SUCCESS;
}

class Windows_wake_runtime final : public Wake_runtime
{
public:
    Handle job, leader, tui, control, input, output, server;

    bool initialize_io()
    {
        if (!m_connect.valid() || !m_client_read.valid() || !m_client_write.valid() ||
            !m_control_read.valid() || !m_control_write.valid())
        {
            return false;
        }
        m_connect.connect(server.value);
        m_control_read.read(output.value, 16384);
        return true;
    }

    ~Windows_wake_runtime() override
    {
        m_connect.cancel();
        m_client_read.cancel();
        m_client_write.cancel();
        m_control_read.cancel();
        m_control_write.cancel();
        if (job) TerminateJobObject(job.value, 0);
        for (HANDLE process : {tui.value, control.value, leader.value}) {
            if (process != INVALID_HANDLE_VALUE) WaitForSingleObject(process, 5000);
        }
    }

    bool write_control(std::string_view bytes) override
    {
        if (m_closed || bytes.size() > k_request_limit || m_queued_bytes + bytes.size() > k_request_limit) {
            return false;
        }
        m_control_queue.emplace_back(bytes);
        m_queued_bytes += bytes.size();
        if (!m_control_write.active()) {
            start_control_write();
        }
        return true;
    }

    void wait(std::optional<Clock::time_point> deadline) override
    {
        const auto include_deadline = [&](Clock::time_point value) {
            if (!deadline || value < *deadline) {
                deadline = value;
            }
        };
        if (m_connected) {
            include_deadline(m_client_deadline);
        }
        if (m_control_write.active()) {
            include_deadline(m_write_deadline);
        }
        std::vector<HANDLE> handles{tui.value, leader.value};
        if (!m_closed) {
            handles.push_back(control.value);
        }
        for (const auto* operation : {&m_connect, &m_client_read, &m_client_write, &m_control_read, &m_control_write}) {
            if (operation->active()) {
                handles.push_back(operation->event());
            }
        }
        DWORD timeout = INFINITE;
        if (deadline) {
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(*deadline - Clock::now()).count();
            timeout = (DWORD)std::clamp<long long>(remaining, 0, INFINITE - 1);
        }
        if (WaitForMultipleObjects((DWORD)handles.size(), handles.data(), FALSE, timeout) == WAIT_FAILED) {
            m_wait_failed = true;
        }
    }

    Wake_poll poll() override
    {
        Wake_poll result;
        if (m_wait_failed) {
            result.exit_code = 3;
            return result;
        }
        DWORD exit_code = 0;
        if (WaitForSingleObject(tui.value, 0) != WAIT_TIMEOUT) {
            GetExitCodeProcess(tui.value, &exit_code);
            result.exit_code = static_cast<int>(exit_code);
        }
        else if (WaitForSingleObject(leader.value, 0) != WAIT_TIMEOUT) result.exit_code = 3;
        if (result.exit_code) {
            return result;
        }

        if (const auto written = m_control_write.take()) {
            if (written->error != ERROR_SUCCESS || written->count != m_control_queue.front().size()) {
                close_control(result);
            }
            else {
                m_queued_bytes -= m_control_queue.front().size();
                m_control_queue.pop_front();
                if (!m_control_queue.empty()) {
                    start_control_write();
                }
            }
        }
        poll_client(result);
        if (result.request_disconnected && m_control_write.active()) {
            close_control(result);
        }
        if (m_control_write.active() && Clock::now() >= m_write_deadline) {
            close_control(result);
        }
        for (int read = 0; read < 8; ++read) {
            const auto completed = m_control_read.take();
            if (!completed) {
                break;
            }
            if (completed->error != ERROR_SUCCESS) {
                close_control(result);
                break;
            }
            for (DWORD i = 0; i < completed->count; ++i) {
                const auto ch = m_control_read.data()[i];
                if (ch == '\n') {
                    if (!m_dropping_line) {
                        result.control_lines.push_back(std::move(m_pending_output));
                    }
                    m_pending_output.clear();
                    m_dropping_line = false;
                }
                else if (!m_dropping_line) {
                    m_pending_output += ch;
                    if (m_pending_output.size() > 1024 * 1024) {
                        m_pending_output.clear();
                        m_dropping_line = true;
                    }
                }
            }
            m_control_read.read(output.value, 16384);
        }
        if (!m_closed && WaitForSingleObject(control.value, 0) != WAIT_TIMEOUT) {
            close_control(result);
        }
        return result;
    }

    void reply(std::string_view bytes) override
    {
        if (!m_connected || m_client_write.active()) {
            return;
        }
        m_reply_size = bytes.size();
        m_client_write.write(server.value, bytes);
        // A completed write can still leave unread pipe bytes. The outstanding
        // read observes client closure; only that or expiry reclaims the pipe.
    }

private:
    void start_control_write()
    {
        m_control_write.write(input.value, m_control_queue.front());
        m_write_deadline = Clock::now() + std::chrono::milliseconds(250);
    }

    void close_control(Wake_poll& result)
    {
        if (m_closed) {
            return;
        }
        m_control_read.cancel();
        m_control_write.cancel();
        m_control_queue.clear();
        m_queued_bytes = 0;
        m_closed = result.control_closed = true;
    }

    void disconnect_client(Wake_poll& result)
    {
        m_client_read.cancel();
        m_client_write.cancel();
        DisconnectNamedPipe(server.value);
        m_connected = m_received = false;
        result.request_disconnected = true;
        m_connect.connect(server.value);
    }

    void poll_client(Wake_poll& result)
    {
        if (const auto connected = m_connect.take()) {
            if (connected->error != ERROR_SUCCESS) {
                m_wait_failed = true;
                result.exit_code = 3;
                return;
            }
            m_connected = true;
            m_client_deadline = Clock::now() + std::chrono::seconds(8);
            m_client_read.read(server.value, k_request_limit);
        }
        if (!m_connected) {
            return;
        }
        if (Clock::now() >= m_client_deadline) {
            disconnect_client(result);
            return;
        }
        if (const auto received = m_client_read.take()) {
            if (received->error != ERROR_SUCCESS || m_received) {
                disconnect_client(result);
                return;
            }
            result.request = std::string(m_client_read.data(), received->count);
            m_received = true;
            m_client_read.read(server.value, 1);
        }
        if (const auto written = m_client_write.take()) {
            if (written->error != ERROR_SUCCESS || written->count != m_reply_size) {
                disconnect_client(result);
            }
        }
    }

    Pipe_operation m_connect, m_client_read, m_client_write, m_control_read, m_control_write;
    bool m_connected = false;
    bool m_received = false;
    bool m_closed = false;
    bool m_dropping_line = false;
    bool m_wait_failed = false;
    std::size_t m_reply_size = 0;
    std::size_t m_queued_bytes = 0;
    std::string m_pending_output;
    std::deque<std::string> m_control_queue;
    Clock::time_point m_client_deadline;
    Clock::time_point m_write_deadline;
};
} // namespace

std::unique_ptr<Wake_runtime> start_wake_runtime(const std::string& token, const std::vector<std::string>& native_argv)
{
    if (native_argv.empty() || pipe_name(token).empty()) return {};
    auto runtime = std::make_unique<Windows_wake_runtime>();
    runtime->server = create_owned_pipe(pipe_name(token), PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE);
    runtime->job.reset(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!runtime->server || !runtime->job || !SetInformationJobObject(runtime->job.value,
        JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return {};
    Handle null(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr));
    if (!null) return {};
    const auto endpoint = utf8((std::filesystem::temp_directory_path() / ("logonomic-grok-" + token + ".sock")).wstring());
    const auto preceding = leader_pipes();
    // The attachability probe is a short connection. Native last-client exit
    // must not race the real clients; the job, rather than client count, owns
    // this private leader's lifetime, including supervisor crash cleanup.
    runtime->leader = spawn({native_argv[0], "agent", "--leader-socket", endpoint, "leader",
        "--no-exit-on-disconnect", "--relay-on-demand", "--no-auto-update"},
        runtime->job.value, {null.value, null.value, null.value}, false);
    if (!runtime->leader || !wait_for_leader(runtime->leader.value, preceding)) return {};
    Handle control_input, control_output;
    if (!create_control_pipe(pipe_name(token) + L".Input", false, runtime->input, control_input) ||
        !create_control_pipe(pipe_name(token) + L".Output", true, runtime->output, control_output))
    {
        return {};
    }
    runtime->control = spawn({native_argv[0], "agent", "--leader", "--leader-socket", endpoint, "stdio"},
        runtime->job.value, {control_input.value, control_output.value, null.value}, false);
    if (!runtime->control) return {};
    auto tui = native_argv;
    tui.insert(tui.begin() + 1, {"--leader", "--leader-socket", endpoint});
    runtime->tui = spawn(tui, runtime->job.value,
        {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)}, true);
    if (!runtime->tui) return {};
    if (!runtime->initialize_io()) return {};
    return runtime;
}

Wake_channel_result exchange_wake_request(std::string_view token, std::string_view request,
    const std::function<bool(int)>& wait_for_cancel)
{
    Wake_channel_result result;
    const auto name = pipe_name(token);
    if (name.empty() || request.size() > k_request_limit) return result;
    Handle pipe;
    const auto deadline = Clock::now() + std::chrono::seconds(6);
    while (Clock::now() < deadline && !wait_for_cancel(0)) {
        pipe.reset(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        if (pipe) break;
        if (GetLastError() != ERROR_PIPE_BUSY) return result;
        wait_for_cancel(10);
    }
    if (!pipe || wait_for_cancel(0)) return result;
    DWORD mode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
    if (!SetNamedPipeHandleState(pipe.value, &mode, nullptr, nullptr)) return result;
    DWORD count = 0;
    result.request_written = true;
    if (!WriteFile(pipe.value, request.data(), static_cast<DWORD>(request.size()), &count, nullptr) || count != request.size()) return result;
    while (Clock::now() < deadline && !wait_for_cancel(0)) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe.value, nullptr, 0, nullptr, &available, nullptr)) return result;
        if (available) {
            std::string response(k_request_limit, '\0');
            if (!ReadFile(pipe.value, response.data(), static_cast<DWORD>(response.size()), &count, nullptr)) return result;
            response.resize(count);
            result.response = std::move(response);
            return result;
        }
        wait_for_cancel(10);
    }
    return result;
}

int run_standalone_native(const std::vector<std::string>& native_argv)
{
    if (native_argv.empty()) return 3;
    auto argv = native_argv;
    argv.insert(argv.begin() + 1, "--no-leader");
    auto child = spawn(argv, nullptr,
        {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)}, true);
    if (!child) return 3;
    if (WaitForSingleObject(child.value, INFINITE) != WAIT_OBJECT_0) return 3;
    DWORD exit_code = 3;
    GetExitCodeProcess(child.value, &exit_code);
    return static_cast<int>(exit_code);
}

std::string session_bridge_path()
{
    std::wstring executable(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!count || count >= executable.size()) return {};
    executable.resize(count);
    return utf8((std::filesystem::path(executable).parent_path() / "grok_session_bridge.exe").wstring());
}
} // namespace grok
#endif
