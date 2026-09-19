#include <logonomic_cli_addon/package_source.h>
#include "grok_usage.h"
#include "grok_accounts.h"
#include "grok_usage_records.h"

#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/ids.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

namespace grok {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
constexpr std::size_t k_line_limit = 4 * 1024 * 1024;
constexpr std::size_t k_sort_bytes = 4 * 1024 * 1024;
constexpr std::size_t k_sort_records = 128;
constexpr int k_parser_revision = 1;
std::timed_mutex scan_mutex;

fs::path cache_io_path(const fs::path& path)
{
#ifdef _WIN32
    // Cache descendants can exceed MAX_PATH in account-scoped storage.
    // Keep the extended spelling local to I/O, never account or wire identity.
    auto native = fs::absolute(path).lexically_normal().make_preferred().native();
    if (native.starts_with(L"\\\\?\\")) return fs::path(native);
    if (native.starts_with(L"\\\\")) return fs::path(L"\\\\?\\UNC\\" + native.substr(2));
    return fs::path(L"\\\\?\\" + native);
#else
    return path;
#endif
}

struct Scan_failure { std::string reason; };

void require(bool condition, std::string reason)
{
    if (!condition) throw Scan_failure{std::move(reason)};
}

void check_cancel(const std::function<bool()>& cancelled)
{
    require(!cancelled(), "cancelled");
}

struct Disk_usage
{
    std::uint64_t bytes = 0;
    std::uint64_t entries = 0;
    std::uint64_t lines = 0;
};

Disk_usage path_usage(const fs::path& path, const std::function<bool()>& cancelled, const Usage_scan_limits* limits = nullptr)
{
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (!fs::exists(status)) return {};
    require(!error && !fs::is_symlink(status), "usage_storage_invalid");
    Disk_usage usage{0, 1, 0};
    if (fs::is_directory(status)) return usage;
    require(fs::is_regular_file(status), "usage_storage_invalid");
    usage.bytes = fs::file_size(path);
    if (limits) require(usage.bytes <= limits->disk_bytes, "usage_disk_budget_exceeded");
    if (usage.bytes == 0) return usage; // Includes the exclusively held storage lock.
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "usage_storage_read_failed");
    std::array<char, 65536> bytes{};
    while (input.read(bytes.data(), bytes.size()) || input.gcount() > 0) {
        check_cancel(cancelled);
        usage.lines += static_cast<std::uint64_t>(std::count(bytes.begin(), bytes.begin() + input.gcount(), '\n'));
        if (limits) require(usage.lines <= limits->persisted_lines, "usage_disk_budget_exceeded");
    }
    require(!input.bad(), "usage_storage_read_failed");
    return usage;
}

class Disk_budget final
{
public:
    Disk_budget(const fs::path& root, const Usage_scan_limits& limits, const std::function<bool()>& cancelled)
    : m_limits(limits), m_cancelled(cancelled)
    {
        reserve(path_usage(root, cancelled, &limits));
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            check_cancel(cancelled);
            reserve(path_usage(entry.path(), cancelled, &limits));
        }
    }
    void reserve(const Disk_usage& usage)
    {
        require(usage.bytes <= m_limits.disk_bytes - std::min(m_usage.bytes, m_limits.disk_bytes) &&
            usage.entries <= m_limits.filesystem_entries - std::min(m_usage.entries, m_limits.filesystem_entries) &&
            usage.lines <= m_limits.persisted_lines - std::min(m_usage.lines, m_limits.persisted_lines), "usage_disk_budget_exceeded");
        m_usage.bytes += usage.bytes; m_usage.entries += usage.entries; m_usage.lines += usage.lines;
    }
    void release(const Disk_usage& usage)
    {
        require(usage.bytes <= m_usage.bytes && usage.entries <= m_usage.entries && usage.lines <= m_usage.lines, "usage_budget_accounting_invalid");
        m_usage.bytes -= usage.bytes; m_usage.entries -= usage.entries; m_usage.lines -= usage.lines;
    }
    Disk_usage inspect(const fs::path& path) const { return path_usage(path, m_cancelled); }
    void prepare_file(const fs::path& path)
    {
        const auto old = inspect(path);
        if (old.entries == 0) reserve({0, 1, 0});
        else release({old.bytes, 0, old.lines});
    }
    const Usage_scan_limits& limits() const { return m_limits; }
private:
    const Usage_scan_limits& m_limits;
    const std::function<bool()>& m_cancelled;
    Disk_usage m_usage;
};

thread_local Disk_budget* active_budget = nullptr;

class Budget_scope final
{
public:
    explicit Budget_scope(Disk_budget& budget) : m_previous(active_budget) { active_budget = &budget; }
    ~Budget_scope() { active_budget = m_previous; }
private:
    Disk_budget* m_previous;
};

class Budget_output final : public std::ofstream
{
public:
    Budget_output(const fs::path& path, std::ios::openmode mode)
    {
        if (active_budget) active_budget->prepare_file(path);
        open(path, mode);
    }
};

void remove_owned_path(const fs::path& path)
{
    const auto usage = active_budget ? active_budget->inspect(path) : Disk_usage{};
    if (fs::remove(path) && active_budget) active_budget->release(usage);
}

// A retired generation may still be unwinding a scan while its replacement
// starts. The storage lock therefore covers process generations as well as
// concurrent calls in this process; no cache mutation relies on RPC ordering.
class Storage_lock final
{
public:
    Storage_lock(const fs::path& path, const std::function<bool()>& cancelled)
    {
        require(!fs::is_symlink(fs::symlink_status(path)), "usage_storage_link");
#if defined(_WIN32)
        while (m_handle == INVALID_HANDLE_VALUE) {
            check_cancel(cancelled);
            m_handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m_handle != INVALID_HANDLE_VALUE) break;
            require(GetLastError() == ERROR_SHARING_VIOLATION, "usage_storage_lock_failed");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
#else
        m_descriptor = open(path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        require(m_descriptor >= 0, "usage_storage_lock_failed");
        while (flock(m_descriptor, LOCK_EX | LOCK_NB) != 0) {
            const int failure = errno;
            if (cancelled() || (failure != EWOULDBLOCK && failure != EAGAIN && failure != EINTR)) {
                close(m_descriptor); m_descriptor = -1;
                throw Scan_failure{cancelled() ? "cancelled" : "usage_storage_lock_failed"};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
#endif
    }
    ~Storage_lock()
    {
#if defined(_WIN32)
        if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle);
#else
        if (m_descriptor >= 0) close(m_descriptor);
#endif
    }
    Storage_lock(const Storage_lock&) = delete;
    Storage_lock& operator=(const Storage_lock&) = delete;
private:
#if defined(_WIN32)
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_descriptor = -1;
#endif
};

std::string path_text(const fs::path& path)
{
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
}

std::string digest(std::string_view value)
{
    return addon::to_hex(addon::Sha256::digest(value));
}

std::string cache_key(std::string_view source)
{
    return digest(profile_account_key(source));
}

std::string nonce()
{
    static std::atomic<std::uint64_t> serial{0};
    std::random_device random;
    return digest(std::to_string(random()) + ":" + std::to_string(random()) + ":" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ":" + std::to_string(serial++));
}

bool digest_name(std::string_view value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
    });
}

void sync_file(const fs::path& path)
{
#if defined(_WIN32)
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "usage_storage_sync_failed");
    const bool synced = FlushFileBuffers(handle) != 0;
    const bool closed = CloseHandle(handle) != 0;
#else
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    require(descriptor >= 0, "usage_storage_sync_failed");
    const bool synced = fsync(descriptor) == 0;
    const bool closed = close(descriptor) == 0;
#endif
    require(synced && closed, "usage_storage_sync_failed");
}

void sync_directory(const fs::path& path)
{
#if defined(_WIN32)
    // File contents are flushed separately; replacement uses MOVEFILE_WRITE_THROUGH.
    (void)path;
#else
    const int descriptor = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    require(descriptor >= 0, "usage_storage_sync_failed");
    const bool synced = fsync(descriptor) == 0;
    const bool closed = close(descriptor) == 0;
    require(synced && closed, "usage_storage_sync_failed");
#endif
}

fs::path directory(const fs::path& parent, const std::string& name)
{
    const auto path = parent / name;
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    require(!fs::is_symlink(status), "usage_storage_link");
    if (!fs::exists(status)) {
        error.clear();
        if (active_budget) active_budget->reserve({0, 1, 0});
        fs::create_directory(path, error);
        require(!error, "usage_storage_create_failed");
        sync_directory(parent);
    }
    require(fs::is_directory(fs::symlink_status(path, error)) && !error, "usage_storage_invalid");
    return path;
}

void remove_snapshot(const fs::path& path)
{
    require(digest_name(path.filename().string()), "usage_snapshot_invalid");
    require(fs::is_directory(fs::symlink_status(path)), "usage_storage_invalid");
    // All entries written here are files. Refuse foreign directories instead
    // of following or recursively removing anything in an unexpected shape.
    for (const auto& entry : fs::directory_iterator(path)) {
        require(!fs::is_directory(entry.symlink_status()), "usage_storage_invalid");
        remove_owned_path(entry.path());
    }
    remove_owned_path(path);
}

class Pending_snapshot final
{
public:
    explicit Pending_snapshot(fs::path path) : m_path(std::move(path)) {}
    ~Pending_snapshot()
    {
        if (!m_committed) {
            const auto budget = active_budget;
            active_budget = nullptr; // Cleanup only removes files; cancellation must not inhibit it.
            try { remove_snapshot(m_path); } catch (...) { /* The next scan retries owned scratch cleanup. */ }
            active_budget = budget;
        }
    }
    void commit() { m_committed = true; }
private:
    fs::path m_path;
    bool m_committed = false;
};

struct Accepted_generations
{
    std::string current;
    std::optional<std::string> previous;
};

void prune_snapshots(const fs::path& snapshots, const Accepted_generations& accepted)
{
    for (const auto& entry : fs::directory_iterator(snapshots)) {
        const auto name = entry.path().filename().string();
        if (!digest_name(name) || name == accepted.current || name == accepted.previous) continue;
        remove_snapshot(entry.path());
    }
}

bool read_line(std::istream& input, std::string* line)
{
    line->clear();
    char character = 0;
    while (input.get(character)) {
        if (character == '\n') return true;
        require(line->size() < k_line_limit, "usage_cache_record_too_large");
        line->push_back(character);
    }
    require(!input.bad(), "usage_storage_read_failed");
    require(line->empty(), "usage_cache_record_incomplete");
    return false;
}

void write_line(std::ostream& output, std::string_view line)
{
    if (active_budget) active_budget->reserve({static_cast<std::uint64_t>(line.size()) + 1, 0, 1});
    output.write(line.data(), static_cast<std::streamsize>(line.size()));
    output.put('\n');
    require(static_cast<bool>(output), "usage_storage_write_failed");
}

void replace_file(const fs::path& source, const fs::path& destination, bool* replaced = nullptr)
{
    const auto previous_usage = active_budget ? active_budget->inspect(destination) : Disk_usage{};
    std::error_code error;
    require(!fs::is_symlink(fs::symlink_status(destination, error)), "usage_storage_link");
    sync_file(source);
#if defined(_WIN32)
    require(MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
        "usage_storage_replace_failed");
#else
    error.clear();
    fs::rename(source, destination, error);
    require(!error, "usage_storage_replace_failed");
#endif
    if (replaced) {
        *replaced = true;
    }
    if (active_budget) active_budget->release(previous_usage);
    sync_directory(destination.parent_path());
    if (source.parent_path() != destination.parent_path()) {
        sync_directory(source.parent_path());
    }
}

void write_json(const fs::path& path, const addon::Json_value& value, bool* replaced = nullptr)
{
    const auto temporary = path.parent_path() / (path.filename().string() + ".pending");
    require(!fs::is_symlink(fs::symlink_status(temporary)), "usage_storage_link");
    Budget_output output(temporary, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "usage_storage_write_failed");
    write_line(output, addon::serialize_json(value));
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
    replace_file(temporary, path, replaced);
}

std::optional<addon::Json_value> read_json(const fs::path& path)
{
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (!fs::exists(status)) return std::nullopt;
    require(fs::is_regular_file(status) && !fs::is_symlink(status), "usage_storage_invalid");
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "usage_storage_read_failed");
    std::string line;
    require(read_line(input, &line), "usage_cache_invalid");
    auto value = addon::parse_json(line, {}, nullptr);
    require(value.has_value() && value->is_object(), "usage_cache_invalid");
    require(!read_line(input, &line), "usage_cache_invalid");
    return value;
}

const addon::Json_value& member(const addon::Json_value& object, std::string_view name)
{
    static const addon::Json_value missing;
    const auto* value = object.find(name);
    return value ? *value : missing;
}

std::string string_member(const addon::Json_value& object, std::string_view name)
{
    const auto& value = member(object, name);
    require(value.is_string(), "usage_cache_invalid");
    return value.as_string();
}

// Persist through the public codec so scanner caches cannot acquire a second
// interpretation of category coverage or repository allocation records.
std::string encode_record(const Session_usage_record& record)
{
    auto envelope = addon::Json_value::object();
    envelope.set("usage", addon::encode_usage_record(record.usage, 2));
    envelope.set("recorded_cwd", addon::Json_value(record.recorded_cwd));
    return addon::serialize_json(envelope);
}

Session_usage_record decode_record(std::string_view line)
{
    const auto value = addon::parse_json(line, {}, nullptr);
    require(value && value->is_object(), "usage_cache_invalid");
    addon::Contract_failure failure;
    auto record = addon::decode_usage_record(member(*value, "usage"), &failure, 2);
    require(record.has_value(), "usage_cache_invalid");
    return {std::move(*record), string_member(*value, "recorded_cwd")};
}

struct Fingerprint
{
    std::uintmax_t size = 0;
    fs::file_time_type modified;
    bool operator==(const Fingerprint&) const = default;
};

std::optional<Fingerprint> fingerprint(const fs::path& path)
{
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error) return std::nullopt;
    const auto modified = fs::last_write_time(path, error);
    if (error) return std::nullopt;
    return Fingerprint{size, modified};
}

bool fingerprint_matches(const addon::Json_value& metadata, const Fingerprint& value)
{
    if (member(metadata, "size") != addon::Json_value(std::to_string(value.size))) return false;
    if (member(metadata, "modified") == addon::Json_value(std::to_string(value.modified.time_since_epoch().count()))) return true;
    return false;
}

addon::Json_value read_cache_header(const fs::path& path, std::ifstream* input)
{
    require(fs::is_regular_file(fs::symlink_status(path)), "usage_cache_invalid");
    input->open(path, std::ios::binary);
    require(static_cast<bool>(*input), "usage_storage_read_failed");
    std::string line;
    require(read_line(*input, &line), "usage_cache_invalid");
    auto header = addon::parse_json(line, {}, nullptr);
    require(header && header->is_object(), "usage_cache_invalid");
    require(cache_key(string_member(*header, "source")) == path.stem().string(), "usage_cache_identity_mismatch");
    return std::move(*header);
}

void refresh_file(const fs::path& path, const fs::path& work,
    bool force, bool* incomplete, const std::function<bool()>& cancelled)
{
    check_cancel(cancelled);
    const auto source = path_text(fs::absolute(path).lexically_normal());
    const auto key = cache_key(source);
    const auto cache_path = work / (key + ".cache");
    const auto before = fingerprint(path);
    const auto summary_before = fingerprint(path.parent_path() / "summary.json");
    std::optional<addon::Json_value> metadata;
    if (fs::exists(fs::symlink_status(cache_path))) {
        std::ifstream cached_input;
        metadata = read_cache_header(cache_path, &cached_input);
    }
    if (!before) { *incomplete = true; return; }
    require(summary_before.has_value(), "usage_summary_unavailable");
    // A writer may have created or truncated the file before populating it.
    // Preserve prior evidence and do not manufacture an empty cache entry.
    if (before->size == 0) { *incomplete = true; return; }
    const bool current = metadata && member(*metadata, "revision") == addon::Json_value(k_parser_revision);
    if (!force && current && fingerprint_matches(*metadata, *before) &&
        member(*metadata, "summary_size") == addon::Json_value(std::to_string(summary_before->size)) &&
        member(*metadata, "summary_modified") == addon::Json_value(std::to_string(summary_before->modified.time_since_epoch().count()))) return;
    std::ifstream input(path, std::ios::binary);
    if (!input) { *incomplete = true; return; }
    const auto pending = work / (key + ".pending-cache");
    Budget_output output(pending, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "usage_storage_write_failed");
    auto next = addon::Json_value::object();
    next.set("revision", addon::Json_value(k_parser_revision));
    next.set("source", addon::Json_value(source));
    next.set("size", addon::Json_value(std::to_string(before->size)));
    next.set("modified", addon::Json_value(std::to_string(before->modified.time_since_epoch().count())));
    next.set("summary_size", addon::Json_value(std::to_string(summary_before->size)));
    next.set("summary_modified", addon::Json_value(std::to_string(summary_before->modified.time_since_epoch().count())));
    // One replacement binds the source fingerprints to their complete parsed records.
    write_line(output, addon::serialize_json(next));
    std::string error;
    std::uint64_t file_records = 0;
    std::uint64_t file_bytes = 0;
    const bool parsed = read_session_usage(input, logonomic::cli_addon::path_from_utf8(source), [&](const Session_usage_record& record) {
        if (record.usage.input.coverage->covered_records == 0) *incomplete = true;
        const auto encoded = encode_record(record);
        require(file_records < active_budget->limits().candidate_records && encoded.size() + 1 <=
            active_budget->limits().candidate_bytes - std::min(file_bytes, active_budget->limits().candidate_bytes),
            "usage_candidate_budget_exceeded");
        ++file_records; file_bytes += encoded.size() + 1;
        write_line(output, encoded);
        return true;
    }, cancelled, &error);
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
    const auto after = fingerprint(path);
    const auto summary_after = fingerprint(path.parent_path() / "summary.json");
    if (!after || *after != *before || !summary_after || *summary_after != *summary_before || error == "usage_read_failed") { *incomplete = true; return; }
    require(parsed, error.empty() ? "usage_invalid" : error);
    check_cancel(cancelled);
    replace_file(pending, cache_path);
}

addon::Json_value account_value(const std::optional<addon::Account_context>& account)
{
    return account ? addon::Json_value(account->adapter_account_key) : addon::Json_value{};
}

Accepted_generations accepted_generations(const fs::path& root)
{
    const auto pointer = read_json(root / "accepted");
    if (!pointer) {
        return {};
    }
    Accepted_generations accepted;
    accepted.current = string_member(*pointer, "snapshot_id");
    require(digest_name(accepted.current), "required_usage_cache_unavailable");
    const auto& previous = member(*pointer, "previous_snapshot_id");
    if (!previous.is_null()) {
        require(previous.is_string() && digest_name(previous.as_string()), "required_usage_cache_unavailable");
        accepted.previous = previous.as_string();
    }
    return accepted;
}

std::string file_digest(const fs::path& path, const std::function<bool()>& cancelled)
{
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "usage_storage_read_failed");
    addon::Sha256 hash;
    std::array<char, 65536> bytes{};
    while (input.read(bytes.data(), bytes.size()) || input.gcount() > 0) {
        check_cancel(cancelled);
        hash.update(std::string_view(bytes.data(), static_cast<std::size_t>(input.gcount())));
    }
    require(!input.bad(), "usage_storage_read_failed");
    return addon::to_hex(hash.finish());
}

void write_cache_manifest(const fs::path& work,
    const std::optional<addon::Account_context>& account, const std::function<bool()>& cancelled)
{
    const auto pending = work / "cache-inventory.pending";
    require(!fs::is_symlink(fs::symlink_status(pending)), "usage_storage_link");
    Budget_output output(pending, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "usage_storage_write_failed");
    auto header = addon::Json_value::object();
    header.set("version", addon::Json_value(1));
    header.set("account", account_value(account));
    addon::Sha256 hash;
    const auto append = [&](std::string_view line) { write_line(output, line); hash.update(line); hash.update("\n"); };
    append(addon::serialize_json(header));
    std::uint64_t count = 0;
    for (const auto& entry : fs::directory_iterator(work)) {
        check_cancel(cancelled);
        const auto name = entry.path().filename().string();
        if (!name.ends_with(".cache") || !digest_name(std::string_view(name).substr(0, name.size() - 6))) continue;
        const auto key = name.substr(0, 64);
        require(fs::is_regular_file(entry.symlink_status()), "required_usage_cache_unavailable");
        append(key + ":" + file_digest(entry.path(), cancelled)); ++count;
    }
    write_line(output, "end:" + std::to_string(count) + ":" + addon::to_hex(hash.finish()));
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
    replace_file(pending, work / "cache-inventory");
}

void copy_accepted_cache(const fs::path& accepted, const fs::path& candidate,
    const std::optional<addon::Account_context>& account, const std::function<bool()>& cancelled)
{
    require(fs::is_directory(fs::symlink_status(accepted)), "required_usage_cache_unavailable");
    const auto path = accepted / "cache-inventory";
    require(fs::is_regular_file(fs::symlink_status(path)), "required_usage_cache_unavailable");
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "usage_storage_read_failed");
    std::string line;
    require(read_line(input, &line), "required_usage_cache_unavailable");
    const auto header = addon::parse_json(line, {}, nullptr);
    require(header && member(*header, "version") == addon::Json_value(1) &&
        member(*header, "account") == account_value(account), "required_usage_cache_unavailable");
    addon::Sha256 hash;
    hash.update(line); hash.update("\n");
    std::uint64_t count = 0;
    while (read_line(input, &line)) {
        check_cancel(cancelled);
        if (line.starts_with("end:")) {
            const auto expected = "end:" + std::to_string(count) + ":" + addon::to_hex(hash.finish());
            require(line == expected && !read_line(input, &line), "required_usage_cache_unavailable");
            return;
        }
        require(line.size() == 129 && line[64] == ':' && digest_name(std::string_view(line).substr(0, 64)) &&
            digest_name(std::string_view(line).substr(65)), "required_usage_cache_unavailable");
        hash.update(line); hash.update("\n"); ++count;
        const auto name = line.substr(0, 64) + ".cache";
        const auto destination = candidate / name;
        require(!fs::exists(fs::symlink_status(destination)), "required_usage_cache_unavailable");
        require(fs::is_regular_file(fs::symlink_status(accepted / name)), "required_usage_cache_unavailable");
        std::ifstream source(accepted / name, std::ios::binary);
        Budget_output output(destination, std::ios::binary | std::ios::trunc);
        require(source && output, "usage_storage_open_failed");
        addon::Sha256 contents;
        std::string record;
        while (read_line(source, &record)) {
            check_cancel(cancelled);
            contents.update(record); contents.update("\n");
            write_line(output, record);
        }
        require(addon::to_hex(contents.finish()) == line.substr(65), "required_usage_cache_unavailable");
        output.close();
        require(static_cast<bool>(output), "usage_storage_write_failed");
        sync_file(destination);
    }
    throw Scan_failure{"required_usage_cache_unavailable"};
}

struct Sort_record
{
    std::string id;
    std::string line;
};

bool id_less(std::string_view left, std::string_view right)
{
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
        [](unsigned char a, unsigned char b) { return a < b; });
}

fs::path run_path(const fs::path& work, std::uint64_t level, std::uint64_t index)
{
    return work / ("run-" + std::to_string(level) + "-" + std::to_string(index));
}

void merge_runs(const fs::path& left_path, const fs::path& right_path, const fs::path& destination,
    const std::function<bool()>& cancelled)
{
    std::ifstream left(left_path, std::ios::binary), right(right_path, std::ios::binary);
    Budget_output output(destination, std::ios::binary | std::ios::trunc);
    require(left && right && output, "usage_storage_open_failed");
    Sort_record a, b;
    const auto next = [](std::istream& input, Sort_record* record) {
        if (!read_line(input, &record->line)) return false;
        record->id = decode_record(record->line).usage.record_id;
        return true;
    };
    bool have_a = next(left, &a), have_b = next(right, &b);
    while (have_a || have_b) {
        check_cancel(cancelled);
        require(!have_a || !have_b || a.id != b.id, "duplicate_usage_record");
        if (have_a && (!have_b || id_less(a.id, b.id))) { write_line(output, a.line); have_a = next(left, &a); }
        else { write_line(output, b.line); have_b = next(right, &b); }
    }
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
    left.close(); right.close();
    remove_owned_path(left_path); remove_owned_path(right_path);
}

void sorted_snapshot(const fs::path& work, bool* incomplete, const std::function<bool()>& cancelled)
{
    std::vector<Sort_record> chunk;
    std::size_t bytes = 0;
    std::uint64_t runs = 0;
    std::uint64_t candidate_records = 0;
    std::uint64_t candidate_bytes = 0;
    const auto flush = [&] {
        if (chunk.empty()) return;
        std::sort(chunk.begin(), chunk.end(), [](const auto& a, const auto& b) { return id_less(a.id, b.id); });
        Budget_output output(run_path(work, 0, runs++), std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "usage_storage_write_failed");
        std::string previous;
        for (const auto& record : chunk) {
            require(previous.empty() || previous != record.id, "duplicate_usage_record");
            write_line(output, record.line);
            previous = record.id;
        }
        output.close();
        require(static_cast<bool>(output), "usage_storage_write_failed");
        chunk.clear(); bytes = 0;
    };
    for (const auto& entry : fs::directory_iterator(work)) {
        check_cancel(cancelled);
        const auto filename = entry.path().filename().string();
        if (!filename.ends_with(".cache") || !digest_name(std::string_view(filename).substr(0, filename.size() - 6))) continue;
        std::ifstream input;
        const auto metadata = read_cache_header(entry.path(), &input);
        const auto source = string_member(metadata, "source");
        std::string line;
        while (read_line(input, &line)) {
            check_cancel(cancelled);
            auto record = decode_record(line);
            if (record.usage.input.coverage->covered_records == 0) *incomplete = true;
            require(record.usage.source_path && cache_key(*record.usage.source_path) == cache_key(source), "usage_cache_identity_mismatch");
            refresh_repository(&record);
            auto encoded = encode_record(record);
            require(candidate_records < active_budget->limits().candidate_records && encoded.size() + 1 <=
                active_budget->limits().candidate_bytes - std::min(candidate_bytes, active_budget->limits().candidate_bytes),
                "usage_candidate_budget_exceeded");
            ++candidate_records; candidate_bytes += encoded.size() + 1;
            if (!chunk.empty() && (chunk.size() >= k_sort_records || bytes + encoded.size() > k_sort_bytes)) flush();
            bytes += encoded.size();
            chunk.push_back({record.usage.record_id, std::move(encoded)});
        }
    }
    flush();
    std::uint64_t level = 0;
    while (runs > 1) {
        const auto next_runs = (runs + 1) / 2;
        for (std::uint64_t index = 0; index < next_runs; ++index) {
            check_cancel(cancelled);
            const auto destination = run_path(work, level + 1, index);
            if (index * 2 + 1 == runs) fs::rename(run_path(work, level, index * 2), destination);
            else merge_runs(run_path(work, level, index * 2), run_path(work, level, index * 2 + 1), destination, cancelled);
        }
        ++level; runs = next_runs;
    }
    if (runs == 1) fs::rename(run_path(work, level, 0), work / "records");
    else { Budget_output empty(work / "records", std::ios::binary); require(static_cast<bool>(empty), "usage_storage_write_failed"); }
}

std::uint64_t decimal(std::string_view text)
{
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    require(!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
        std::to_string(value) == text, "usage_cursor_invalid");
    return value;
}

addon::Usage_scan_page page_at(const fs::path& snapshot, const std::string& identifier,
    std::uint64_t offset, const std::optional<addon::Account_context>& account, const std::function<bool()>& cancelled)
{
    require(fs::is_directory(fs::symlink_status(snapshot)), "usage_snapshot_expired");
    const auto metadata = read_json(snapshot / "snapshot");
    require(metadata.has_value(), "usage_snapshot_expired");
    addon::Contract_failure failure;
    const auto encoded_scope = addon::decode_observation_scope(member(*metadata, "scope"), &failure);
    require(encoded_scope.has_value(), "usage_snapshot_invalid");
    require(encoded_scope->source_account_key == (account ? std::optional(account->adapter_account_key) : std::nullopt),
        "usage_snapshot_account_mismatch");
    addon::Usage_scan_page page;
    page.scope = *encoded_scope;
    page.snapshot_id = identifier;
    const auto& incomplete = member(*metadata, "incomplete");
    require(incomplete.is_boolean(), "usage_snapshot_invalid");
    page.incomplete_historical_coverage = incomplete.as_boolean();
    const auto path = snapshot / "records";
    require(fs::is_regular_file(fs::symlink_status(path)), "usage_snapshot_invalid");
    const auto size = fs::file_size(path);
    require(offset <= size, "usage_cursor_invalid");
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "usage_storage_read_failed");
    if (offset > 0) {
        input.seekg(static_cast<std::streamoff>(offset - 1));
        require(input.get() == '\n', "usage_cursor_invalid");
    }
    input.seekg(static_cast<std::streamoff>(offset));
    std::string line;
    std::size_t bytes = 0;
    while (page.records.size() < addon::k_max_usage_page_records && read_line(input, &line)) {
        check_cancel(cancelled);
        if (!page.records.empty() && bytes + line.size() > k_line_limit / 2) break;
        bytes += line.size();
        page.records.push_back(decode_record(line).usage);
        offset += line.size() + 1;
    }
    page.complete = offset == size;
    if (!page.complete) page.next_cursor = std::to_string(offset);
    return page;
}

} // namespace

std::optional<addon::Usage_scan_page> scan_usage(const addon::Usage_scan_request& request,
    const fs::path& vendor_profile, const fs::path& storage, const std::function<bool()>& cancelled,
    std::string* error, const Usage_scan_limits& limits)
{
    try {
        std::unique_lock lock(scan_mutex, std::defer_lock);
        while (!lock.try_lock_for(std::chrono::milliseconds(50))) check_cancel(cancelled);
        check_cancel(cancelled);
        // Bootstrap creates only these three zero-byte entries before inventory.
        require(limits.filesystem_entries >= 3, "usage_disk_budget_exceeded");
        const auto* selection = request.cache_selection ? &*request.cache_selection : nullptr;
        addon::Contract_failure failure;
        std::string request_hash;
        fs::path root;
        fs::path snapshots;
        std::unique_ptr<Storage_lock> storage_lock;
        if (selection) {
            require(addon::validate_usage_cache_selection(request, &failure), "usage_cache_selection_invalid");
            const auto outside_mutable = [&](const std::string& selected) {
                const auto mutable_path = storage / "grok-usage-v1";
                for (auto path = addon::path_from_utf8(selected); path.has_relative_path(); path = path.parent_path()) {
                    std::error_code error;
                    if (fs::equivalent(path, mutable_path, error)) {
                        return false;
                    }
                }
                const auto relative = addon::path_from_utf8(selected).lexically_relative(storage);
                auto name = addon::path_to_utf8(*relative.begin());
#ifdef _WIN32
                name = addon::ascii_lowercase(name);
#endif
                return name != "grok-usage-v1";
            };
            require(outside_mutable(selection->candidate_root) &&
                (!selection->accepted || outside_mutable(selection->accepted->root)), "usage_cache_namespace_conflict");
            require(addon::validate_usage_cache_root(selection->candidate_root, addon::path_to_utf8(storage), &failure),
                "usage_cache_unavailable");
            root = cache_io_path(addon::path_from_utf8(selection->candidate_root));
            auto identity = request;
            identity.snapshot_id.reset();
            identity.cursor.reset();
            request_hash = addon::to_hex(addon::Sha256::digest(
                addon::serialize_json_canonical(addon::encode_usage_scan_request(identity, 3))));
            if (request.snapshot_id) {
                const auto state = read_json(root / "cache-state");
                require(state && string_member(*state, "request_sha256") == request_hash &&
                    string_member(*state, "snapshot_id") == *request.snapshot_id, "usage_snapshot_mismatch");
                auto page = page_at(root, *request.snapshot_id, decimal(*request.cursor), request.context.account_context, cancelled);
                page.cache_candidate = addon::Usage_cache_reference{
                    selection->candidate_root, page.snapshot_id, file_digest(root / "cache-manifest.json", cancelled)};
                page.account_context = request.context.account_context;
                page.profile_storage = request.context.profile_storage;
                for (auto& record : page.records) record.aggregate_evidence = "partial";
                return page;
            }
            require(addon::validate_usage_cache_allocation(selection->candidate_root, addon::path_to_utf8(storage), &failure),
                "usage_cache_unavailable");
            if (selection->accepted) {
                const auto manifest = addon::validate_usage_cache_candidate(*selection->accepted,
                    addon::path_to_utf8(storage), request.context.account_context, cancelled, &failure);
                check_cancel(cancelled);
                require(manifest && manifest->format == "grok-turn-cache@1", "usage_cache_unavailable");
            }
            if (request.native_file_cache) {
                const auto& seed = *request.native_file_cache;
                require(request.context.account_context &&
                    seed.source_account_key == request.context.account_context->adapter_account_key,
                    "usage_native_cache_account_mismatch");
                addon::Directory_package_source source(storage);
                addon::Source_error source_error;
                const auto relative = addon::path_from_utf8(seed.path).lexically_relative(storage);
                const auto bytes = source.read(addon::path_to_utf8(relative), 1, &source_error);
                require(bytes && bytes->empty() && addon::to_hex(addon::Sha256::digest(*bytes)) == seed.sha256_hex,
                    "usage_native_cache_unsupported");
                require(!seed.cache_required || selection->accepted.has_value(), "required_usage_cache_unavailable");
            }
        }
        else {
            root = directory(cache_io_path(storage), "grok-usage-v1");
            storage_lock = std::make_unique<Storage_lock>(root / "scan.lock", cancelled);
            snapshots = directory(root, "snapshots");
        }
        if (request.snapshot_id) {
            require(digest_name(*request.snapshot_id) && request.cursor.has_value(), "usage_cursor_invalid");
            return page_at(snapshots / *request.snapshot_id, *request.snapshot_id, decimal(*request.cursor), request.context.account_context, cancelled);
        }
        require(!request.cursor, "usage_cursor_invalid");
        Disk_budget budget(root, limits, cancelled);
        Budget_scope budget_scope(budget);
        require(fs::is_directory(vendor_profile), "usage_sessions_unavailable");
        const auto sessions = vendor_profile / "sessions";
        const bool have_sessions = fs::exists(sessions);
        require(!have_sessions || fs::is_directory(sessions), "usage_sessions_not_directory");
        const auto accepted = selection ? Accepted_generations{} : accepted_generations(root);
        if (!selection) prune_snapshots(snapshots, accepted);
        const auto identifier = nonce();
        const auto work = selection ? root : directory(snapshots, identifier);
        Pending_snapshot pending(work);
        // Host allocations, including failed writes, remain owned by the host.
        if (selection) pending.commit();
        if (selection && selection->accepted) {
            copy_accepted_cache(cache_io_path(addon::path_from_utf8(selection->accepted->root)), work,
                request.context.account_context, cancelled);
        }
        if (!accepted.current.empty()) {
            copy_accepted_cache(snapshots / accepted.current, work, request.context.account_context, cancelled);
        }
        bool incomplete = false;
        // RelocationView owns only <cwd bucket>/<session id>, never nested
        // workflow or artifact directories. Fixed native filenames preserve
        // the platform filesystem's case sensitivity.
        fs::directory_iterator buckets;
        if (have_sessions) buckets = fs::directory_iterator(sessions);
        for (const auto& bucket : buckets) {
            check_cancel(cancelled);
            if (!fs::is_directory(bucket.symlink_status())) continue;
            for (const auto& entry : fs::directory_iterator(bucket.path())) {
                check_cancel(cancelled);
                if (!fs::is_directory(entry.symlink_status()) || entry.path().filename().string().starts_with('.')) continue;
                if (!fs::is_regular_file(fs::symlink_status(entry.path() / "summary.json"))) continue;
                const auto usage = entry.path() / "usage.json";
                if (fs::is_regular_file(usage)) {
                    refresh_file(usage, work, request.mode != "incremental", &incomplete, cancelled);
                }
            }
        }
        sorted_snapshot(work, &incomplete, cancelled);
        write_cache_manifest(work, request.context.account_context, cancelled);
        check_cancel(cancelled);
        addon::Observation_scope scope;
        if (request.context.account_context) scope.source_account_key = request.context.account_context->adapter_account_key;
        scope.observed_at_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        auto metadata = addon::Json_value::object();
        metadata.set("scope", addon::encode_observation_scope(scope));
        metadata.set("incomplete", addon::Json_value(incomplete));
        sync_file(work / "records");
        write_json(work / "snapshot", metadata);
        auto page = page_at(work, identifier, 0, request.context.account_context, cancelled);
        check_cancel(cancelled);
        if (selection) {
            auto state = addon::Json_value::object();
            state.set("request_sha256", addon::Json_value(request_hash));
            state.set("snapshot_id", addon::Json_value(identifier));
            write_json(work / "cache-state", state);
            // A surviving native session file does not prove complete aggregate history.
            for (auto& record : page.records) record.aggregate_evidence = "partial";
            page.cache_candidate = addon::seal_usage_cache_candidate(
                selection->candidate_root, addon::path_to_utf8(storage), request.context.account_context,
                identifier, "grok-turn-cache@1", cancelled, &failure);
            if (!page.cache_candidate) throw Scan_failure{failure.reason_code};
            page.account_context = request.context.account_context;
            page.profile_storage = request.context.profile_storage;
            return page;
        }
        auto pointer = addon::Json_value::object();
        pointer.set("snapshot_id", addon::Json_value(identifier));
        pointer.set("previous_snapshot_id", accepted.current.empty()
            ? addon::Json_value{} : addon::Json_value(accepted.current));
        // A post-rename sync failure has an indeterminate commit outcome. Preserve
        // the candidate so cleanup can never delete a possibly referenced generation.
        pending.commit();
        bool published = false;
        try {
            write_json(root / "accepted", pointer, &published);
        }
        catch (...) {
            if (published) {
                throw Scan_failure{"usage_cache_commit_indeterminate"};
            }
            throw;
        }
        return page;
    }
    catch (const Scan_failure& failure) { *error = failure.reason; }
    catch (const fs::filesystem_error&) { *error = "usage_storage_io_failed"; }
    catch (const std::exception&) { *error = "usage_scan_failed"; }
    return std::nullopt;
}

void handle_usage(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_usage_scan_operation) { context.reject_unknown_operation(); return; }
    addon::Contract_failure failure;
    const auto version = context.request().capability.version;
    const auto request = addon::decode_usage_scan_request(context.request().payload, &failure, version);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
    const auto profile = selected_profile(request->context, context);
    if (!profile) return;
    if (!request->context.profile_storage) { context.fail(addon::Result_status::UNAVAILABLE, "profile_storage_unavailable"); return; }
    std::string error;
    const auto page = scan_usage(*request, logonomic::cli_addon::path_from_utf8(profile->profile_path), logonomic::cli_addon::path_from_utf8(request->context.profile_storage->root),
        [&context] { return context.cancel_requested(); }, &error);
    if (!page) { context.fail(error == "cancelled" ? addon::Result_status::CANCELLED : addon::Result_status::FAILED, error); return; }
    context.succeed(addon::encode_usage_scan_result(*page, version));
}

} // namespace grok
