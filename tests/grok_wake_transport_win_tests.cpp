#include "grok_wake_transport.h"
#include "grok_json.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_wake.h>
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {
namespace addon = logonomic::cli_addon;
namespace json  = addon::json;
using grok::Json;
using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message) + " (Windows " + std::to_string(GetLastError()) + ")");
    }
}

class Handle
{
public:
    explicit Handle(HANDLE value = nullptr)
    :
        m_value(value)
    {}
    ~Handle() { if (*this) { CloseHandle(m_value); } }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept
    :
        m_value(std::exchange(other.m_value, nullptr))
    {}
    HANDLE get() const { return m_value; }
    explicit operator bool() const { return m_value && m_value != INVALID_HANDLE_VALUE; }

private:
    HANDLE m_value;
};

std::wstring wide(const std::string& value)
{
    return addon::path_from_utf8(value).wstring();
}

std::wstring event_name(const std::string& token, const char* action)
{
    return L"Local\\Logonomic.Grok.Test." + wide(token) + L"." + wide(action);
}

Handle event(const std::string& token, const char* action, bool signaled = false)
{
    Handle result(CreateEventW(nullptr, TRUE, signaled, event_name(token, action).c_str()));
    require(bool(result), "fixture event creation failed");
    return result;
}

void emit(const Json& value)
{
    const auto line = addon::serialize_json(value) + '\n';
    DWORD written = 0;
    require(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), (DWORD)line.size(), &written, nullptr) &&
        written == line.size(), "native fixture output failed");
}

int native_peer(const std::vector<std::string>& args)
{
    const auto socket = std::find(args.begin(), args.end(), "--leader-socket");
    require(socket != args.end() && socket + 1 != args.end(), "native fixture endpoint missing");
    const auto stem = std::filesystem::path(*(socket + 1)).stem().string();
    const auto token = stem.substr(std::string("logonomic-grok-").size());
    const auto has = [&](const char* value) { return std::find(args.begin(), args.end(), value) != args.end(); };
    if (has("leader")) {
        const auto name = L"\\\\.\\pipe\\grok-leader-test-" + wide(token);
        Handle listener(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr));
        require(bool(listener), "native fixture leader pipe failed");
        auto stop = event(token, "leader-exit");
        WaitForSingleObject(stop.get(), INFINITE);
        return 23;
    }
    if (!has("stdio")) {
        auto stop = event(token, "tui-exit");
        auto ready = event(token, "tui-ready");
        SetEvent(ready.get());
        WaitForSingleObject(stop.get(), INFINITE);
        return 17;
    }
    auto stop = event(token, "control-exit");
    std::thread([handle = stop.get()] {
        WaitForSingleObject(handle, INFINITE);
        ExitProcess(19);
    }).detach();
    auto read_allowed = event(token, "read-allowed", true);
    auto read_paused  = event(token, "read-paused");
    auto response_allowed = event(token, "response-allowed", true);
    auto prompt_seen = event(token, "prompt-seen");
    for (std::string line; std::getline(std::cin, line);) {
        const auto query = json::parse(line);
        require(query.has_value(), "native fixture input was not JSON");
        const auto method = json::text(*query, "method");
        if (method == "test/pause") {
            ResetEvent(read_allowed.get());
            SetEvent(read_paused.get());
            WaitForSingleObject(read_allowed.get(), INFINITE);
            continue;
        }
        if (method == "test/eof") {
            CloseHandle(GetStdHandle(STD_OUTPUT_HANDLE));
            WaitForSingleObject(stop.get(), INFINITE);
            return 19;
        }
        if (method == "test/burst") {
            std::string bytes(1024 * 1024, 'x');
            bytes += "\nlast\n";
            DWORD written = 0;
            require(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.data(), (DWORD)bytes.size(), &written, nullptr),
                "native burst write failed");
            continue;
        }
        Json result = Json::object();
        if (method == "initialize") {
            result = json::object({{"protocolVersion", Json(1)},
                {"_meta", json::object({{"agentInstanceId", Json("fixture-leader")}})}});
        }
        else if (method == "_x.ai/sessions/list") {
            auto sessions = Json::array();
            sessions.push_back(json::object({{"sessionId", Json("native-session")}, {"cwd", Json("/work")},
                {"resident", Json(true)}, {"activity", Json("idle")},
                {"origin", json::object({{"kind", Json("local")}})}}));
            result = json::object({{"result", json::object({{"sessions", std::move(sessions)}})}});
        }
        else if (method == "session/prompt") {
            SetEvent(prompt_seen.get());
            WaitForSingleObject(response_allowed.get(), INFINITE);
            const auto& params = json::member(*query, "params");
            emit(json::object({{"jsonrpc", Json("2.0")}, {"method", Json("_x.ai/queue/changed")},
                {"params", json::object({{"sessionId", Json("native-session")},
                    {"runningPromptId", json::member(json::member(params, "_meta"), "promptId")}})}}));
            result = json::object({{"stopReason", Json("end_turn")}});
        }
        emit(json::object({{"jsonrpc", Json("2.0")}, {"id", json::member(*query, "id")},
            {"result", std::move(result)}}));
    }
    return 0;
}

std::wstring quote(const std::string& value)
{
    // These arguments are executable paths and hexadecimal fixture tokens.
    return L"\"" + wide(value) + L"\"";
}

struct Peers
{
    std::string token = addon::random_nonce_hex();
    Handle tui_exit = event(token, "tui-exit");
    Handle leader_exit = event(token, "leader-exit");
    Handle control_exit = event(token, "control-exit");
    Handle tui_ready = event(token, "tui-ready");
    Handle read_allowed = event(token, "read-allowed", true);
    Handle read_paused = event(token, "read-paused");
    Handle response_allowed = event(token, "response-allowed", true);
    Handle prompt_seen = event(token, "prompt-seen");
};

class Session : public Peers
{
public:
    Session(const std::string& bridge, const std::string& peer)
    {
        std::wstring command = quote(bridge) + L" --serve " + wide(token) + L" " + quote(peer);
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        Handle null(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, 0, nullptr));
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = startup.hStdOutput = startup.hStdError = null.get();
        PROCESS_INFORMATION process{};
        require(CreateProcessW(wide(bridge).c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process), "bridge process creation failed");
        m_process = process.hProcess;
        CloseHandle(process.hThread);
        require(WaitForSingleObject(tui_ready.get(), 12000) == WAIT_OBJECT_0, "native TUI did not start");
    }

    ~Session()
    {
        if (m_process) {
            SetEvent(tui_exit.get());
            if (WaitForSingleObject(m_process, 6000) != WAIT_OBJECT_0) {
                TerminateProcess(m_process, 3);
                WaitForSingleObject(m_process, 6000);
            }
            CloseHandle(m_process);
        }
    }

    HANDLE process() const { return m_process; }

    Json exchange(const Json& request)
    {
        const auto response = grok::exchange_wake_request(token, addon::serialize_json(request), [](int delay) {
            if (delay) { Sleep((DWORD)delay); }
            return false;
        });
        require(response.response.has_value(), "bridge exchange lost its response");
        const auto parsed = json::parse(*response.response);
        require(parsed.has_value(), "bridge response was not JSON");
        return *parsed;
    }

    Json request(bool notify, const std::string& id = "mailbox-17") const
    {
        addon::Session_wake_target target;
        target.provider_session_id = "native-session";
        target.working_directory = "/work";
        target.launcher_argv = {"bridge", "--serve", token, "grok"};
        target.correlation = {std::string(32, 'b')};
        target.launch_environment = {addon::host_platform_id(), Json::object(), std::nullopt};
        target.binding = json::object({{"transport", Json("grok-private-leader-v1")}, {"token", Json(token)}});
        const auto payload = notify
            ? addon::encode_session_wake_notify_request({target, id, "Read your mailbox."})
            : addon::encode_session_wake_inspect_request({target});
        return json::object({{"operation", Json(notify ? "notify" : "inspect")}, {"payload", payload}});
    }

    void initialize()
    {
        const auto observed = exchange(json::object({{"operation", Json("observe")}, {"token", Json(token)},
            {"transition_nonce", Json(std::string(32, 'b'))},
            {"event", json::object({{"hook_event_name", Json("SessionStart")}, {"sessionId", Json("native-session")}})}}));
        require(json::member(observed, "observed") == Json(true), "session observation failed");
        const auto ready = exchange(request(false));
        require(json::member(ready, "available") == Json(true), "native session was unavailable");
    }

private:
    HANDLE m_process = nullptr;
};

Handle connect_client(const std::string& token)
{
    const auto name = L"\\\\.\\pipe\\Logonomic.Grok." + wide(token);
    Handle client(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr));
    require(bool(client), "private client connect failed");
    DWORD mode = PIPE_READMODE_MESSAGE;
    require(SetNamedPipeHandleState(client.get(), &mode, nullptr, nullptr), "private client message mode failed");
    return client;
}

void write(HANDLE pipe, std::string_view bytes)
{
    DWORD count = 0;
    require(WriteFile(pipe, bytes.data(), (DWORD)bytes.size(), &count, nullptr) && count == bytes.size(),
        "private client write failed");
}

std::vector<Handle> original_children()
{
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    require(bool(snapshot), "child snapshot failed");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    require(Process32FirstW(snapshot.get(), &entry), "child enumeration failed");
    std::vector<Handle> children;
    do {
        if (entry.th32ParentProcessID == GetCurrentProcessId()) {
            children.emplace_back(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE, entry.th32ProcessID));
            require(bool(children.back()), "original child handle could not be retained");
        }
    } while (Process32NextW(snapshot.get(), &entry));
    require(children.size() == 3, "fixture did not retain exactly its three native children");
    return children;
}

grok::Wake_poll until(grok::Wake_runtime& runtime, const std::function<bool(const grok::Wake_poll&)>& done,
    std::chrono::milliseconds timeout = std::chrono::seconds(3))
{
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        auto result = runtime.poll();
        if (done(result)) {
            return result;
        }
        require(!result.exit_code, "runtime exited before expected event");
        require(Clock::now() < deadline, "runtime event deadline expired");
        runtime.wait(deadline);
    }
}

void native_contracts(const std::string& bridge, const std::string& peer)
{
    // The oracles are the owned launch/protocol contract and Win32 overlapped
    // completion, pipe message framing and process-handle wait semantics.
    {
        Peers peers;
        auto runtime = grok::start_wake_runtime(peers.token, {peer});
        require(bool(runtime), "native runtime did not start");
        const auto begin = Clock::now();
        runtime->wait(begin + std::chrono::milliseconds(150));
        require(Clock::now() - begin >= std::chrono::milliseconds(140), "quiet wait returned without an event/deadline");
        {
            auto client = connect_client(peers.token);
            write(client.get(), "request");
            // Both connection and request can complete before entering wait.
            runtime->wait(Clock::now() + std::chrono::seconds(1));
            const auto request = until(*runtime, [](const auto& value) { return value.request.has_value(); });
            require(request.request == "request", "pre-wait request was lost");
            runtime->reply("retained response");
            (void)runtime->poll();
            Sleep(200);
            char buffer[128]{};
            DWORD count = 0;
            require(ReadFile(client.get(), buffer, sizeof(buffer), &count, nullptr) &&
                std::string_view(buffer, count) == "retained response", "reply was discarded before client read");
        }
        until(*runtime, [](const auto& value) { return value.request_disconnected; });
        auto replacement = connect_client(peers.token);
        const std::string maximum_request(65536, 'r');
        write(replacement.get(), maximum_request);
        require(until(*runtime, [](const auto& value) { return value.request.has_value(); }).request == maximum_request,
            "closed client listener did not rearm or maximum private message changed");
    }
    std::cout << "pre-wait connect/read, reply retention, disconnect and rearm passed\n";
    {
        Peers peers;
        auto runtime = grok::start_wake_runtime(peers.token, {peer});
        require(bool(runtime), "expiry runtime did not start");
        auto client = connect_client(peers.token);
        const auto begin = Clock::now();
        until(*runtime, [](const auto& value) { return value.request_disconnected; }, std::chrono::seconds(10));
        require(Clock::now() - begin >= std::chrono::seconds(7), "connected client expired before its bound");
    }
    std::cout << "incomplete client eight-second deadline passed\n";
    {
        Peers peers;
        auto runtime = grok::start_wake_runtime(peers.token, {peer});
        require(bool(runtime), "framing runtime did not start");
        require(runtime->write_control("{\"method\":\"test/burst\"}\n"), "burst request write failed");
        std::vector<std::string> lines;
        until(*runtime, [&](const auto& value) {
            lines.insert(lines.end(), value.control_lines.begin(), value.control_lines.end());
            return lines.size() == 2;
        });
        require(lines[0].size() == 1024 * 1024 && lines[1] == "last", "maximum native line or trailing frame lost");
        require(runtime->write_control("{\"method\":\"test/eof\"}\n"), "EOF request write failed");
        until(*runtime, [](const auto& value) { return value.control_closed; });
        // A settled EOF/control handle must leave the idle wait set.
        const auto begin = Clock::now();
        runtime->wait(begin + std::chrono::milliseconds(150));
        require(Clock::now() - begin >= std::chrono::milliseconds(140), "settled native EOF spins the wait loop");
    }
    std::cout << "maximum-line burst, native EOF and settled wait source passed\n";
    for (const bool disconnect : {false, true}) {
        Peers peers;
        auto runtime = grok::start_wake_runtime(peers.token, {peer});
        require(bool(runtime), "backpressure runtime did not start");
        require(runtime->write_control("{\"method\":\"test/pause\"}\n"), "pause request write failed");
        require(WaitForSingleObject(peers.read_paused.get(), 3000) == WAIT_OBJECT_0, "native reader did not pause");
        (void)runtime->poll();
        {
            auto client = connect_client(peers.token);
            write(client.get(), "request");
            until(*runtime, [](const auto& value) { return value.request.has_value(); });
            require(runtime->write_control(std::string(65536, 'x')), "first full native buffer rejected");
            (void)runtime->poll();
            require(runtime->write_control(std::string(65536, 'y')), "pending native write rejected");
            if (!disconnect) {
                until(*runtime, [](const auto& value) { return value.control_closed; });
            }
        }
        if (disconnect) {
            const auto cancelled = until(*runtime, [](const auto& value) { return value.request_disconnected; });
            require(cancelled.control_closed, "client disconnect did not cancel outstanding native write");
        }
        require(!runtime->write_control("replay"), "cancelled native write reopened for replay");
    }
    std::cout << "pending native write deadline and disconnect cancellation passed\n";
    for (int source = 0; source != 3; ++source) {
        Peers peers;
        auto runtime = grok::start_wake_runtime(peers.token, {peer});
        require(bool(runtime), "process exit runtime did not start");
        const HANDLE stop = source == 0 ? peers.tui_exit.get() :
            source == 1 ? peers.leader_exit.get() : peers.control_exit.get();
        SetEvent(stop);
        const auto exited = until(*runtime, [source](const auto& value) {
            return source == 2 ? value.control_closed : value.exit_code.has_value();
        });
        if (source != 2) {
            require(exited.exit_code == (source == 0 ? 17 : 3), "original child exit status changed");
        }
        else {
            const auto begin = Clock::now();
            runtime->wait(begin + std::chrono::milliseconds(150));
            require(Clock::now() - begin >= std::chrono::milliseconds(140), "settled child spins the wait loop");
            SetEvent(peers.tui_exit.get());
            require(until(*runtime, [](const auto& value) { return value.exit_code.has_value(); }).exit_code == 17,
                "TUI exit was lost after control exit");
        }
    }
    std::cout << "original TUI/leader/control exits and settled handles passed\n";
    {
        Session session(bridge, peer);
        session.initialize();
        ResetEvent(session.response_allowed.get());
        const auto begin = Clock::now();
        const auto receipt = session.exchange(session.request(true));
        require(json::text(receipt, "disposition") == "uncertain", "timed-out native prompt lost uncertainty");
        require(Clock::now() - begin >= std::chrono::seconds(3) &&
            Clock::now() - begin < std::chrono::seconds(6), "protocol request deadline was not observed");
        ResetEvent(session.prompt_seen.get());
        SetEvent(session.response_allowed.get());
        const auto repeat = session.exchange(session.request(true));
        require(json::text(repeat, "disposition") != "accepted" &&
            WaitForSingleObject(session.prompt_seen.get(), 0) == WAIT_TIMEOUT, "uncertain prompt was replayed");
    }
    {
        Session session(bridge, peer);
        session.initialize();
        ResetEvent(session.response_allowed.get());
        const auto result = grok::exchange_wake_request(session.token,
            addon::serialize_json(session.request(true)), [&](int delay) {
                return WaitForSingleObject(session.prompt_seen.get(), (DWORD)delay) == WAIT_OBJECT_0;
            });
        require(result.request_written && !result.response, "request cancellation did not disconnect pending caller");
        ResetEvent(session.prompt_seen.get());
        SetEvent(session.response_allowed.get());
        require(json::text(session.exchange(session.request(true)), "disposition") != "accepted" &&
            WaitForSingleObject(session.prompt_seen.get(), 0) == WAIT_TIMEOUT, "disconnected prompt was replayed");
    }
    std::cout << "actual bridge request deadline, cancelled receipt and uncertain no-replay passed\n";
    DWORD before = 0, after = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &before), "initial handle sample failed");
    for (int trial = 0; trial != 8; ++trial) {
        Peers peers;
        auto runtime = grok::start_wake_runtime(peers.token, {peer});
        require(bool(runtime), "teardown runtime did not start");
        auto children = original_children();
        if (trial % 2) {
            auto client = connect_client(peers.token);
            write(client.get(), "request");
            until(*runtime, [](const auto& value) { return value.request.has_value(); });
            runtime->reply(std::string(128 * 1024, 'z'));
            // The reader stays alive without draining the oversized response,
            // so destruction must cancel and reap both outstanding directions.
            runtime.reset();
        }
        else {
            runtime.reset();
        }
        for (const auto& child : children) {
            require(WaitForSingleObject(child.get(), 0) == WAIT_OBJECT_0,
                "runtime teardown left an original child running");
        }
    }
    require(GetProcessHandleCount(GetCurrentProcess(), &after), "final handle sample failed");
    require(after == before, "pending operation teardown leaked handles");
    std::cout << "repeated pending connect/read/write destruction settled without handle leaks\n";
}

unsigned long long cpu_ticks(HANDLE process)
{
    FILETIME created{}, exited{}, kernel{}, user{};
    require(GetProcessTimes(process, &created, &exited, &kernel, &user), "process CPU sample failed");
    const auto ticks = [](FILETIME value) {
        return ((unsigned long long)value.dwHighDateTime << 32) | value.dwLowDateTime;
    };
    return ticks(kernel) + ticks(user);
}

void measure(const std::string& bridge, const std::string& peer)
{
    for (int trial = 0; trial != 3; ++trial) {
        Session session(bridge, peer);
        session.initialize();
        Sleep(150);
        unsigned long long before_cycles = 0, after_cycles = 0;
        QueryProcessCycleTime(session.process(), &before_cycles);
        const auto before_cpu = cpu_ticks(session.process());
        const auto before = Clock::now();
        Sleep(3000);
        const auto elapsed = std::chrono::duration<double>(Clock::now() - before).count();
        const auto cpu = cpu_ticks(session.process()) - before_cpu;
        QueryProcessCycleTime(session.process(), &after_cycles);
        std::vector<double> latency;
        for (int i = 0; i != 24; ++i) {
            Sleep((DWORD)((i * 7 + trial * 3) % 18));
            const auto begin = Clock::now();
            const auto receipt = session.exchange(session.request(true, "mailbox-" + std::to_string(i)));
            latency.push_back(std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
            require(json::text(receipt, "disposition") == "accepted", "native notification not accepted");
        }
        std::sort(latency.begin(), latency.end());
        std::cout << "trial=" << trial << " idle_seconds=" << elapsed << " cpu_ms=" << cpu / 10000.0
            << " cycles=" << after_cycles - before_cycles << " latency_ms_min=" << latency.front()
            << " p50=" << latency[12] << " p95=" << latency[22] << " max=" << latency.back() << '\n';
    }
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::string> args(argv + 1, argv + argc);
        if (std::find(args.begin(), args.end(), "--leader-socket") != args.end()) {
            return native_peer(args);
        }
        require(args.size() == 2, "usage: --measure|--idle|--session-bridge <bridge>");
        const auto peer = addon::path_to_utf8(std::filesystem::absolute(addon::path_from_utf8(argv[0])));
        if (args[0] == "--measure") {
            measure(args[1], peer);
        }
        else if (args[0] == "--idle") {
            Session session(args[1], peer);
            session.initialize();
            std::cout << GetProcessId(session.process()) << std::endl;
            Sleep(12000);
        }
        else {
            require(args[0] == "--session-bridge", "unknown test mode");
            native_contracts(args[1], peer);
        }
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
