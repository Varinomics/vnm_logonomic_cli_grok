#include "grok_launcher.h"
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <mutex>
#include <thread>
#include <logonomic_cli_addon/ids.h>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace grok {
namespace addon = logonomic::cli_addon;
namespace {

using addon::lower;
using addon::set_environment;
using addon::resolve_program;
using addon::native_image;
using addon::path_to_utf8;
using addon::base_name;

#ifdef _WIN32
using addon::wide;
using addon::quote_argument;

struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
#else
// Reserve custody before spawning. Unconfirmed children remain in a bounded
// owner table and are reaped on subsequent admissions; saturation fails closed.
class Child_custody
{
public:
    Child_custody()
    {
        std::lock_guard lock(s_mutex);
        for (std::size_t i = 0; i < s_children.size(); ++i) {
            int status = 0;
            if (s_children[i] > 0) {
                const auto waited = waitpid(s_children[i], &status, WNOHANG);
                if (waited == s_children[i] || (waited < 0 && errno == ECHILD)) s_children[i] = 0;
            }
            if (!m_slot && s_children[i] == 0) { m_slot = &s_children[i]; *m_slot = -1; }
        }
    }
    ~Child_custody()
    {
        if (m_slot && !m_retained) { std::lock_guard lock(s_mutex); *m_slot = 0; }
    }
    bool admitted() const { return m_slot != nullptr; }
    void retain(pid_t child)
    {
        std::lock_guard lock(s_mutex);
        *m_slot = child;
        m_retained = true;
    }
private:
    inline static std::mutex s_mutex;
    inline static std::array<pid_t, 32> s_children{};
    pid_t* m_slot = nullptr;
    bool m_retained = false;
};

class Pipe_signal_guard
{
public:
    Pipe_signal_guard()
    {
        sigemptyset(&m_signals);
        sigaddset(&m_signals, SIGPIPE);
        sigset_t pending{};
        sigpending(&pending);
        m_preexisting = sigismember(&pending, SIGPIPE) == 1;
        m_active = pthread_sigmask(SIG_BLOCK, &m_signals, &m_previous) == 0;
    }
    ~Pipe_signal_guard()
    {
        if (!m_active) return;
        if (!m_preexisting) {
            timespec zero{};
            while (sigtimedwait(&m_signals, nullptr, &zero) >= 0) {}
        }
        pthread_sigmask(SIG_SETMASK, &m_previous, nullptr);
    }
    bool active() const { return m_active; }
private:
    sigset_t m_signals{};
    sigset_t m_previous{};
    bool m_preexisting = false;
    bool m_active = false;
};
#endif

std::optional<Probe_output> probe(
    const std::vector<std::string>& argv, const Environment& environment,
    std::string_view working_directory, int timeout_ms, addon::Invocation_context& context, Probe_exchange* exchange = nullptr)
{
    if (context.cancel_requested()) return std::nullopt;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    Probe_output result;
    bool valid = true;
    std::string pending_input = exchange ? exchange->initial_input : std::string{};
    std::size_t observed_bytes = 0;
    auto settle_deadline = deadline;
    const auto advance = [&] {
        if (exchange && exchange->tick) {
            const bool complete = exchange->complete;
            exchange->tick();
            if (!complete && exchange->complete) settle_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        }
        if (!exchange || exchange->complete || observed_bytes == result.output.size()) return;
        observed_bytes = result.output.size();
        const auto reply = exchange->on_output(result.output);
        if (!reply) { valid = false; return; }
        pending_input += *reply;
        if (exchange->complete) settle_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    };
    const std::size_t output_limit = exchange ? 2 * 1024 * 1024 : 65536;
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle output_read, output_write, errors_read, errors_write;
    if (!CreatePipe(&output_read.value, &output_write.value, &security, 0) ||
        !CreatePipe(&errors_read.value, &errors_write.value, &security, 0)) return std::nullopt;
    SetHandleInformation(output_read.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errors_read.value, HANDLE_FLAG_INHERIT, 0);
    Handle input, input_write;
    if (exchange) {
        const auto pipe_name = wide("\\\\.\\pipe\\logonomic-grok-probe-" + addon::random_nonce_hex());
        input_write.value = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 65536, 65536, 0, nullptr);
        if (input_write.value == INVALID_HANDLE_VALUE) return std::nullopt;
        input.value = CreateFileW(pipe_name.c_str(), GENERIC_READ, 0, &security, OPEN_EXISTING, 0, nullptr);
        if (input.value == INVALID_HANDLE_VALUE) return std::nullopt;
        if (!ConnectNamedPipe(input_write.value, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return std::nullopt;
    }
    else {
        input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (input.value == INVALID_HANDLE_VALUE) return std::nullopt;
    }
    std::wstring command;
    for (const auto& argument : argv) {
        if (!command.empty()) command += L' ';
        command += quote_argument(wide(argument));
    }
    std::vector<std::wstring> variables;
    for (const auto& [name, value] : environment) variables.push_back(wide(name + "=" + value));
    std::sort(variables.begin(), variables.end(), [](const auto& left, const auto& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });
    std::wstring environment_block;
    for (const auto& variable : variables) { environment_block += variable; environment_block += L'\0'; }
    environment_block += L'\0';
    if (variables.empty()) environment_block += L'\0';
    // Restrict handle inheritance: concurrent adapter invokes must not retain
    // each other's pipe handles and keep a completed probe alive.
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<unsigned char> attributes(attribute_bytes);
    auto* attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_bytes)) return std::nullopt;
    HANDLE handles[]{input.value, output_write.value, errors_write.value};
    const bool updated = UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        handles, sizeof(handles), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput  = input.value;
    startup.StartupInfo.hStdOutput = output_write.value;
    startup.StartupInfo.hStdError  = errors_write.value;
    startup.lpAttributeList = attribute_list;
    PROCESS_INFORMATION information{};
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    const auto directory = wide(working_directory);
    const bool created = updated && job.value &&
        SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
        CreateProcessW(wide(argv.front()).c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
            environment_block.data(), directory.empty() ? nullptr : directory.c_str(),
            &startup.StartupInfo, &information);
    DeleteProcThreadAttributeList(attribute_list);
    if (!created) return std::nullopt;
    Handle process{information.hProcess}, thread{information.hThread};
    CloseHandle(input.value); input.value = nullptr;
    CloseHandle(output_write.value); output_write.value = nullptr;
    CloseHandle(errors_write.value); errors_write.value = nullptr;
    if (!AssignProcessToJobObject(job.value, process.value) || ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        TerminateProcess(process.value, 1);
        WaitForSingleObject(process.value, 1000);
        return std::nullopt;
    }
    const auto drain = [&](HANDLE pipe, std::string& output) {
        DWORD available = 0;
        while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available) {
            std::array<char, 4096> buffer{};
            DWORD count = 0;
            if (!ReadFile(pipe, buffer.data(), std::min(available, static_cast<DWORD>(buffer.size())), &count, nullptr)) break;
            if (output.size() + count > output_limit) { valid = false; break; }
            output.append(buffer.data(), count);
        }
    };
    for (;;) {
        drain(output_read.value, result.output);
        drain(errors_read.value, result.errors);
        advance();
        if (input_write.value && !pending_input.empty()) {
            DWORD written = 0;
            if (!WriteFile(input_write.value, pending_input.data(), static_cast<DWORD>(pending_input.size()), &written, nullptr)) valid = false;
            pending_input.erase(0, written);
        }
        if (input_write.value && exchange->complete) { CloseHandle(input_write.value); input_write.value = nullptr; }
        if (exchange && exchange->complete && std::chrono::steady_clock::now() >= settle_deadline) break;
        if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) break;
        if (!valid || context.cancel_requested() || std::chrono::steady_clock::now() >= deadline) {
            valid = false;
            break;
        }
        context.wait_for_cancel(10);
    }
    TerminateJobObject(job.value, 1);
    const auto custody_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (;;) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) { valid = false; break; }
        if (accounting.ActiveProcesses == 0 && WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) break;
        if (std::chrono::steady_clock::now() >= custody_deadline) { valid = false; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    drain(output_read.value, result.output);
    drain(errors_read.value, result.errors);
    DWORD exit_code = 1;
    valid = GetExitCodeProcess(process.value, &exit_code) && (exit_code == 0 || (exchange && exchange->complete)) && valid;
#else
    Child_custody custody;
    if (!custody.admitted()) return std::nullopt;
    Pipe_signal_guard signals;
    if (!signals.active()) return std::nullopt;
    int output_pipe[2], errors_pipe[2];
    if (pipe2(output_pipe, O_CLOEXEC) != 0) return std::nullopt;
    if (pipe2(errors_pipe, O_CLOEXEC) != 0) {
        close(output_pipe[0]); close(output_pipe[1]); return std::nullopt;
    }
    int input_pair[2]{-1, -1};
    if (exchange && pipe2(input_pair, O_CLOEXEC) != 0) {
        for (int descriptor : {output_pipe[0], output_pipe[1], errors_pipe[0], errors_pipe[1]}) close(descriptor);
        return std::nullopt;
    }
    const int input = exchange ? input_pair[0] : open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        for (int descriptor : {output_pipe[0], output_pipe[1], errors_pipe[0], errors_pipe[1]}) close(descriptor);
        return std::nullopt;
    }
    std::vector<std::string> environment_strings;
    for (const auto& [name, value] : environment) environment_strings.push_back(name + "=" + value);
    std::vector<char*> arguments, variables;
    for (const auto& argument : argv) arguments.push_back(const_cast<char*>(argument.c_str()));
    for (auto& variable : environment_strings) variables.push_back(variable.data());
    arguments.push_back(nullptr);
    variables.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    posix_spawn_file_actions_adddup2(&actions, input, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errors_pipe[1], STDERR_FILENO);
    const std::string directory(working_directory);
    if (!directory.empty()) posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
    // Inherit the generation custodian's session and process group. A private
    // probe group would escape the public SDK's descendant custody contract.
    pid_t process = 0;
    const int spawned = posix_spawn(&process, argv.front().c_str(), &actions, &attributes,
        arguments.data(), variables.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(input); close(output_pipe[1]); close(errors_pipe[1]);
    if (spawned != 0) { close(output_pipe[0]); close(errors_pipe[0]); if (input_pair[1] >= 0) close(input_pair[1]); return std::nullopt; }
    if (input_pair[1] >= 0) fcntl(input_pair[1], F_SETFL, O_NONBLOCK);
    fcntl(output_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(errors_pipe[0], F_SETFL, O_NONBLOCK);
    const auto drain = [&](int descriptor, std::string& output) {
        std::array<char, 4096> buffer{};
        for (;;) {
            const auto count = read(descriptor, buffer.data(), buffer.size());
            if (count <= 0) break;
            if (output.size() + static_cast<std::size_t>(count) > output_limit) { valid = false; break; }
            output.append(buffer.data(), static_cast<std::size_t>(count));
        }
    };
    int status = 0;
    bool reaped = false;
    bool child_owned = true;
    for (;;) {
        drain(output_pipe[0], result.output);
        drain(errors_pipe[0], result.errors);
        advance();
        if (input_pair[1] >= 0 && !pending_input.empty()) {
            const auto written = write(input_pair[1], pending_input.data(), pending_input.size());
            if (written > 0) pending_input.erase(0, static_cast<std::size_t>(written));
            else if (written < 0 && errno != EINTR && errno != EAGAIN) valid = false;
        }
        if (input_pair[1] >= 0 && exchange->complete) { close(input_pair[1]); input_pair[1] = -1; }
        if (exchange && exchange->complete && std::chrono::steady_clock::now() >= settle_deadline) {
            break;
        }
        const auto waited = waitpid(process, &status, WNOHANG);
        if (waited == process) { reaped = true; break; }
        if (waited < 0 && errno != EINTR) {
            if (errno == ECHILD) child_owned = false;
            valid = false;
            break;
        }
        if (!valid || context.cancel_requested() || std::chrono::steady_clock::now() >= deadline) {
            valid = false;
            break;
        }
        pollfd descriptors[]{{output_pipe[0], POLLIN, 0}, {errors_pipe[0], POLLIN, 0}};
        poll(descriptors, 2, 10);
    }
    // Signal only our unreaped leader: once waitpid has consumed its status,
    // the numeric PID may be recycled. Descendants remain generation-owned.
    if (!reaped && child_owned) kill(process, SIGKILL);
    const auto custody_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!reaped && child_owned && std::chrono::steady_clock::now() < custody_deadline) {
        const auto waited = waitpid(process, &status, WNOHANG);
        if (waited == process) { reaped = true; break; }
        if (waited < 0 && errno != EINTR) {
            if (errno == ECHILD) child_owned = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!reaped) {
        valid = false;
        if (child_owned) custody.retain(process);
    }
    // This confirms the leader only. The custodian owns descendant settlement
    // at generation retirement; this probe does not claim subtree emptiness.
    drain(output_pipe[0], result.output);
    drain(errors_pipe[0], result.errors);
    close(output_pipe[0]); close(errors_pipe[0]);
    if (input_pair[1] >= 0) close(input_pair[1]);
    valid = valid && ((WIFEXITED(status) && WEXITSTATUS(status) == 0) || (exchange && exchange->complete));
#endif
    return valid && !context.cancel_requested() ? std::optional(std::move(result)) : std::nullopt;
}



} // namespace

std::optional<Probe_output> run_probe(const std::vector<std::string>& argv,
    const Environment& environment, std::string_view working_directory, int timeout_ms,
    addon::Invocation_context& context, Probe_exchange* exchange)
{
    return grok::probe(argv, environment, working_directory, timeout_ms, context, exchange);
}

std::vector<std::string> literal_launcher_argv(std::string_view script)
{
    std::vector<std::string> result;
    std::string word;
    char quote = 0;
    bool started = false;
    for (char ch : script) {
        if (ch == '\0' || ch == '\r' || ch == '\n') return {};
        if (quote != '\'' && std::string_view("$`;&|<>").find(ch) != std::string_view::npos) return {};
        if (ch == '\'' || ch == '"') {
            if (quote == ch) quote = 0;
            else if (quote == 0) quote = ch;
            else word += ch;
            started = true;
        }
        else if (!quote && std::isspace(static_cast<unsigned char>(ch))) {
            if (started) { result.push_back(std::move(word)); word.clear(); started = false; }
        }
        else { word += ch; started = true; }
    }
    if (quote) return {};
    if (started) result.push_back(std::move(word));
    return result;
}

std::optional<Launcher_command> recognize_launcher(std::string_view script)
{
    auto argv = literal_launcher_argv(script);
    Launcher_command result;
    std::size_t index = 0;
    while (index < argv.size()) {
        const auto equal = argv[index].find('=');
        if (equal == std::string::npos) break;
        const auto name = argv[index].substr(0, equal);
        if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_') ||
            !std::all_of(name.begin(), name.end(), [](unsigned char ch) { return std::isalnum(ch) || ch == '_'; })) return std::nullopt;
        result.environment_names.push_back(name);
        ++index;
    }
    if (index == argv.size() || base_name(argv[index]) != "grok") return std::nullopt;
    result.program = argv[index++];
    result.prefix_arguments.assign(argv.begin() + index, argv.end());
    return result;
}

std::optional<Environment> launcher_environment(const Launcher_command& command,
    const addon::Json_value& effective_environment)
{
    if (!effective_environment.is_object()) return std::nullopt;
    Environment result;
    for (const auto& name : command.environment_names) {
        const addon::Json_value* value = nullptr;
        for (const auto& member : effective_environment.as_object()) {
#ifdef _WIN32
            const bool matches = lower(member.name) == lower(name);
#else
            const bool matches = member.name == name;
#endif
            if (!matches) continue;
            if (value || !member.value.is_string()) return std::nullopt;
            value = &member.value;
        }
        // The host owns runner-specific assignment expansion. Missing evidence
        // cannot be reconstructed from a quote-stripped launcher token.
        if (!value) return std::nullopt;
        set_environment(result, name, value->as_string());
    }
    return result;
}

std::optional<std::vector<std::string>> resolve_probe_launcher(
    const Launcher_command& command, const std::vector<std::string>& operation_arguments,
    const addon::Json_value& effective_environment, std::string_view working_directory)
{
    Environment environment;
    if (!effective_environment.is_object()) return std::nullopt;
    for (const auto& member : effective_environment.as_object()) {
        if (!member.value.is_string()) return std::nullopt;
        environment.emplace_back(member.name, member.value.as_string());
    }
    if (!launcher_environment(command, effective_environment)) return std::nullopt;
    const auto program = resolve_program(command.program, environment, working_directory);
    if (!program || !native_image(*program)) return std::nullopt;
    std::vector<std::string> result{path_to_utf8(*program)};
    result.insert(result.end(), operation_arguments.begin(), operation_arguments.end());
    return result;
}

std::optional<addon::Launcher_candidate> discover_launcher(const addon::Discover_request& request)
{
    Launcher_command command{"grok", {}, {}};
    if (!resolve_probe_launcher(command, {}, request.ambient_environment, {})) return std::nullopt;
    return addon::Launcher_candidate{
        "interactive", "Grok Build", "grok",
        request.platform_id == "windows-x86_64" ? addon::k_runner_powershell : addon::k_runner_direct,
        "", true, addon::Json_value::object(),
    };
}
} // namespace grok
