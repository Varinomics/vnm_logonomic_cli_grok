#include "grok_wake_transport.h"

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <chrono>
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

Handle create_private_pipe(std::string_view token)
{
    const auto name = pipe_name(token);
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
    Handle pipe(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, k_request_limit, k_request_limit, 0, &attributes));
    LocalFree(security);
    return pipe;
}

class Windows_wake_runtime final : public Wake_runtime
{
public:
    Handle job, leader, tui, control, input, output, server;
    bool connected = false;
    bool received = false;
    bool closed = false;
    bool dropping_line = false;
    std::string pending_output;
    Clock::time_point connected_at;

    ~Windows_wake_runtime() override
    {
        if (job) TerminateJobObject(job.value, 0);
        for (HANDLE process : {tui.value, control.value, leader.value}) {
            if (process != INVALID_HANDLE_VALUE) WaitForSingleObject(process, 5000);
        }
    }

    bool write_control(std::string_view bytes) override
    {
        DWORD written = 0;
        return !closed && bytes.size() <= k_request_limit &&
            WriteFile(input.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    }

    Wake_poll poll() override
    {
        Wake_poll result;
        DWORD exit_code = 0;
        if (WaitForSingleObject(tui.value, 0) != WAIT_TIMEOUT) {
            GetExitCodeProcess(tui.value, &exit_code);
            result.exit_code = static_cast<int>(exit_code);
        }
        else if (WaitForSingleObject(leader.value, 0) != WAIT_TIMEOUT) result.exit_code = 3;
        DWORD available = 0;
        std::array<char, 16384> buffer{};
        for (int read = 0; read < 8 && PeekNamedPipe(output.value, nullptr, 0, nullptr, &available, nullptr) && available; ++read) {
            DWORD count = 0;
            if (!ReadFile(output.value, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &count, nullptr)) break;
            for (DWORD i = 0; i < count; ++i) {
                if (buffer[i] == '\n') {
                    if (!dropping_line) result.control_lines.push_back(std::move(pending_output));
                    pending_output.clear();
                    dropping_line = false;
                }
                else if (!dropping_line) {
                    pending_output += buffer[i];
                    if (pending_output.size() > 1024 * 1024) { pending_output.clear(); dropping_line = true; }
                }
            }
        }
        if (!closed && WaitForSingleObject(control.value, 0) != WAIT_TIMEOUT) {
            closed = true;
            result.control_closed = true;
        }
        if (!connected) {
            const bool attached = ConnectNamedPipe(server.value, nullptr) != FALSE;
            if (attached || GetLastError() == ERROR_PIPE_CONNECTED) {
                connected = true;
                connected_at = Clock::now();
            }
        }
        if (connected) {
            if (!PeekNamedPipe(server.value, nullptr, 0, nullptr, &available, nullptr) ||
                Clock::now() - connected_at > std::chrono::seconds(8)) {
                DisconnectNamedPipe(server.value);
                connected = received = false;
                result.request_disconnected = true;
            }
            else if (!received && available) {
                std::string request(k_request_limit, '\0');
                DWORD count = 0;
                if (!ReadFile(server.value, request.data(), static_cast<DWORD>(request.size()), &count, nullptr)) {
                    DisconnectNamedPipe(server.value);
                    connected = false;
                    result.request_disconnected = true;
                }
                else {
                    request.resize(count);
                    result.request = std::move(request);
                    received = true;
                }
            }
        }
        return result;
    }

    void reply(std::string_view bytes) override
    {
        if (!connected) return;
        DWORD written = 0;
        WriteFile(server.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        // The client reads the response before closing; disconnecting here
        // would discard unread pipe bytes. poll reclaims a closed connection.
        received = true;
    }
};
} // namespace

std::unique_ptr<Wake_runtime> start_wake_runtime(const std::string& token, const std::vector<std::string>& native_argv)
{
    if (native_argv.empty() || pipe_name(token).empty()) return {};
    auto runtime = std::make_unique<Windows_wake_runtime>();
    runtime->server = create_private_pipe(token);
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
    HANDLE read = INVALID_HANDLE_VALUE, write = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&read, &write, nullptr, k_request_limit)) return {};
    Handle control_input(read);
    runtime->input.reset(write);
    if (!CreatePipe(&read, &write, nullptr, k_request_limit)) return {};
    runtime->output.reset(read);
    Handle control_output(write);
    runtime->control = spawn({native_argv[0], "agent", "--leader", "--leader-socket", endpoint, "stdio"},
        runtime->job.value, {control_input.value, control_output.value, null.value}, false);
    if (!runtime->control) return {};
    auto tui = native_argv;
    tui.insert(tui.begin() + 1, {"--leader", "--leader-socket", endpoint});
    runtime->tui = spawn(tui, runtime->job.value,
        {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)}, true);
    if (!runtime->tui) return {};
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
    return utf8((std::filesystem::path(executable).parent_path().parent_path() / "helpers" / "grok_session_bridge.exe").wstring());
}
} // namespace grok
#endif
