#include "grok_usage.h"
#include "grok_accounts.h"
#include "grok_usage_records.h"

#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/stored_usage_accounting.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/capability_ids.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <mutex>
#include <random>
#include <stdexcept>

namespace grok {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

using addon::json::member;

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
    if (usage.bytes == 0) return usage;
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
        require(try_reserve(usage), "usage_disk_budget_exceeded");
    }
    bool try_reserve(const Disk_usage& usage)
    {
        if (!(usage.bytes <= m_limits.disk_bytes - std::min(m_usage.bytes, m_limits.disk_bytes) &&
            usage.entries <= m_limits.filesystem_entries - std::min(m_usage.entries, m_limits.filesystem_entries) &&
            usage.lines <= m_limits.persisted_lines - std::min(m_usage.lines, m_limits.persisted_lines))) return false;
        m_usage.bytes += usage.bytes; m_usage.entries += usage.entries; m_usage.lines += usage.lines;
        return true;
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

std::string path_text(const fs::path& path)
{
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
}

std::string digest(std::string_view value)
{
    return addon::to_hex(addon::Sha256::digest(value));
}

// Previous cache inventories named sources with the old account-key hash.
// Use that format only to verify an already sealed file during migration.
std::string legacy_cache_key(const fs::path& path)
{
    auto generic = path.generic_u8string();
#ifdef _WIN32
    const auto wide = path.generic_wstring();
    const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr, 0);
    require(count > 0, "usage_cache_identity_mismatch");
    std::wstring folded(static_cast<std::size_t>(count), L'\0');
    require(LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(),
        static_cast<int>(wide.size()), folded.data(), count, nullptr, nullptr, 0) == count, "usage_cache_identity_mismatch");
    generic = fs::path(folded).generic_u8string();
#endif
    return digest("profile-" + digest(std::string(generic.begin(), generic.end())));
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

void replace_file(const fs::path& source, const fs::path& destination)
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
    if (active_budget) active_budget->release(previous_usage);
    sync_directory(destination.parent_path());
    if (source.parent_path() != destination.parent_path()) {
        sync_directory(source.parent_path());
    }
}

void write_json(const fs::path& path, const addon::Json_value& value)
{
    const auto temporary = path.parent_path() / (path.filename().string() + ".pending");
    require(!fs::is_symlink(fs::symlink_status(temporary)), "usage_storage_link");
    Budget_output output(temporary, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "usage_storage_write_failed");
    write_line(output, addon::serialize_json(value));
    output.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
    replace_file(temporary, path);
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

std::string string_member(const addon::Json_value& object, std::string_view name)
{
    const auto& value = member(object, name);
    require(value.is_string(), "usage_cache_invalid");
    return value.as_string();
}

// Current stored accounting shares category and repository validation with
// public usage records without adding page-level evidence to cache bytes.
std::string encode_record(const Session_usage_record& record)
{
    auto envelope = addon::Json_value::object();
    envelope.set("usage", addon::encode_stored_usage_accounting(record.usage));
    envelope.set("recorded_cwd", addon::Json_value(record.recorded_cwd));
    return addon::serialize_json(envelope);
}

Session_usage_record decode_record(std::string_view line)
{
    auto value = addon::parse_json(line, {}, nullptr);
    require(value && value->is_object(), "usage_cache_invalid");
    auto* usage = value->find("usage");
    require(usage && usage->is_object(), "usage_cache_invalid");
    addon::Contract_failure failure;
    auto record = addon::decode_stored_usage_accounting(*usage, &failure);
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
    if (member(metadata, "modified") == addon::Json_value(addon::file_time_ticks_text(value.modified))) return true;
    return false;
}

std::string retained_source_key(const addon::Json_value& header)
{
    if (header.find("legacy_source_key")) {
        const auto key = string_member(header, "legacy_source_key");
        require(!header.find("canonical_source") && digest_name(key), "usage_cache_identity_mismatch");
        return key;
    }
    const auto canonical = string_member(header, "canonical_source");
    require(addon::path_from_utf8(canonical).is_absolute(), "usage_cache_identity_mismatch");
    return digest(canonical);
}

std::string migrate_source_key(addon::Json_value& header, const std::string& recorded_key)
{
    if (header.find("canonical_source") || header.find("legacy_source_key")) {
        require(retained_source_key(header) == recorded_key, "usage_cache_identity_mismatch");
#if defined(_WIN32)
        if (header.find("canonical_source")) {
            const auto captured = string_member(header, "canonical_source");
            const auto current = canonical_native_path(captured);
            if (path_text(current) != captured && addon::same_native_account_profile(addon::path_from_utf8(captured), current)) {
                header.set("canonical_source", addon::Json_value(path_text(current)));
                return retained_source_key(header);
            }
        }
#endif
        return recorded_key;
    }
    const auto locator = addon::path_from_utf8(string_member(header, "source"));
    require(locator.is_absolute(), "usage_cache_identity_mismatch");
    const auto canonical = canonical_native_path(path_text(locator));
    const bool matches = legacy_cache_key(canonical) == recorded_key;
    const auto observed = addon::historical_account_profile_identity(canonical, canonical);
    if (matches && (addon::same_native_account_profile(locator.lexically_normal(), canonical) ||
        (observed && observed == addon::native_account_profile_identity(canonical))))
    {
        header.set("canonical_source", addon::Json_value(path_text(canonical)));
    }
    else {
        // The sealed inventory still owns these records when a former link
        // target disappears. Keep their original key outside live-file lookup.
        header.set("legacy_source_key", addon::Json_value(recorded_key));
    }
    return retained_source_key(header);
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
    require(retained_source_key(*header) == path.stem().string(), "usage_cache_identity_mismatch");
    return std::move(*header);
}

void refresh_file(const fs::path& path, const fs::path& work,
    bool force, bool* incomplete, const std::function<bool()>& cancelled)
{
    check_cancel(cancelled);
    const auto source = path_text(fs::absolute(path).lexically_normal());
    const auto canonical = path_text(canonical_native_path(source));
    const auto key = digest(canonical);
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
        member(*metadata, "summary_modified") == addon::Json_value(addon::file_time_ticks_text(summary_before->modified))) return;
    std::ifstream input(path, std::ios::binary);
    if (!input) { *incomplete = true; return; }
    const auto pending = work / (key + ".pending-cache");
    Budget_output output(pending, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "usage_storage_write_failed");
    auto next = addon::Json_value::object();
    next.set("revision", addon::Json_value(k_parser_revision));
    next.set("source", addon::Json_value(source));
    next.set("canonical_source", addon::Json_value(canonical));
    next.set("size", addon::Json_value(std::to_string(before->size)));
    next.set("modified", addon::Json_value(addon::file_time_ticks_text(before->modified)));
    next.set("summary_size", addon::Json_value(std::to_string(summary_before->size)));
    next.set("summary_modified", addon::Json_value(addon::file_time_ticks_text(summary_before->modified)));
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
        require(fs::is_regular_file(fs::symlink_status(accepted / name)), "required_usage_cache_unavailable");
        std::ifstream source(accepted / name, std::ios::binary);
        std::string record;
        require(source && read_line(source, &record), "required_usage_cache_unavailable");
        auto metadata = addon::parse_json(record, {}, nullptr);
        require(metadata && metadata->is_object(), "usage_cache_invalid");
        const auto key = migrate_source_key(*metadata, line.substr(0, 64));
        const auto destination = candidate / (key + ".cache");
        require(!fs::exists(fs::symlink_status(destination)), "required_usage_cache_unavailable");
        Budget_output output(destination, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "usage_storage_open_failed");
        addon::Sha256 contents;
        contents.update(record); contents.update("\n");
        write_line(output, addon::serialize_json(*metadata));
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
            require(record.usage.source_path == source, "usage_cache_identity_mismatch");
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

constexpr const char* k_temporal_file = "usage-temporal";
constexpr const char* k_temporal_index = "usage-temporal-index";

std::string accounting_digest(std::string_view line)
{
    const auto value = addon::parse_json(line, {}, nullptr);
    require(value.has_value(), "usage_cache_invalid");
    return addon::to_hex(addon::Sha256::digest(addon::serialize_json_canonical(*value)));
}

struct Temporal_row
{
    std::string id;
    std::uint64_t total = 0;
    std::string cache_digest;
    std::string accounting_digest;
    std::optional<std::string> instant;
    std::string reason = "missing-evidence";
};

addon::Json_value encode_temporal_row(const Temporal_row& row)
{
    auto value = addon::Json_value::object();
    value.set("record_id", addon::Json_value(row.id));
    value.set("total_tokens", addon::Json_value(row.total));
    value.set("cache_accounting_sha256_hex", addon::Json_value(row.cache_digest));
    value.set("accounting_sha256_hex", addon::Json_value(row.accounting_digest));
    value.set("instant", row.instant ? addon::Json_value(*row.instant) : addon::Json_value{});
    value.set("reason", addon::Json_value(row.reason));
    return value;
}

Temporal_row decode_temporal_row(std::string_view line)
{
    const auto value = addon::parse_json(line, {}, nullptr);
    require(value && value->is_object() && value->as_object().size() == 6, "usage_temporal_unavailable");
    Temporal_row row;
    row.id = string_member(*value, "record_id");
    const auto& total = member(*value, "total_tokens");
    require(total.is_integer() && total.as_integer() >= 0 && total.as_integer() <= 9007199254740991LL,
        "usage_temporal_unavailable");
    row.total = static_cast<std::uint64_t>(total.as_integer());
    row.cache_digest = string_member(*value, "cache_accounting_sha256_hex");
    row.accounting_digest = string_member(*value, "accounting_sha256_hex");
    require(digest_name(row.cache_digest) && digest_name(row.accounting_digest), "usage_temporal_unavailable");
    row.reason = string_member(*value, "reason");
    const auto& instant = member(*value, "instant");
    if (instant.is_string()) {
        require(instant.as_string().size() <= addon::k_max_usage_temporal_bytes && row.reason == "complete",
            "usage_temporal_unavailable");
        addon::Usage_temporal proof;
        proof.parent = {"native-proof", 1, "native-proof", std::nullopt};
        proof.portions.push_back({"instant", instant.as_string(), {}, {}, 1});
        require(addon::decode_usage_temporal(addon::encode_usage_temporal(proof), nullptr).has_value(),
            "usage_temporal_unavailable");
        row.instant = instant.as_string();
    }
    else {
        require(instant.is_null() && (row.reason == "missing-evidence" || row.reason == "capacity" ||
            row.reason == "unsupported" || (row.reason == "complete" && row.total == 0)), "usage_temporal_unavailable");
    }
    return row;
}

std::array<char, 16> temporal_index_entry(std::uint64_t record_offset, std::uint64_t proof_offset)
{
    std::array<char, 16> bytes{};
    for (unsigned index = 0; index < 8; ++index) {
        bytes[index] = static_cast<char>((record_offset >> (index * 8)) & 255);
        bytes[index + 8] = static_cast<char>((proof_offset >> (index * 8)) & 255);
    }
    return bytes;
}

std::pair<std::uint64_t, std::uint64_t> decode_temporal_index(const std::array<char, 16>& bytes)
{
    std::pair<std::uint64_t, std::uint64_t> offsets{};
    for (unsigned index = 0; index < 8; ++index) {
        offsets.first |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[index])) << (index * 8);
        offsets.second |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[index + 8])) << (index * 8);
    }
    return offsets;
}

std::uint64_t cache_manifest_bytes(const fs::path& root,
    const std::optional<addon::Account_context>& account, const std::string& snapshot_id)
{
    addon::Usage_cache_manifest manifest{account, snapshot_id, "grok-turn-cache@1", {}};
    std::uint64_t bytes = addon::serialize_json(addon::encode_usage_cache_manifest(manifest)).size();
    std::uint64_t count = 0;
    for (const auto& entry : fs::directory_iterator(root)) {
        require(fs::is_regular_file(entry.symlink_status()), "usage_storage_invalid");
        manifest.files = {{path_text(entry.path().filename()), fs::file_size(entry.path()), std::string(64, '0')}};
        const auto encoded = addon::encode_usage_cache_manifest(manifest);
        bytes += addon::serialize_json(member(encoded, "files").as_array().front()).size();
        if (count++ != 0) ++bytes;
    }
    return bytes;
}

std::uint64_t temporal_manifest_bytes(const std::optional<addon::Account_context>& account,
    const std::string& snapshot_id)
{
    addon::Usage_cache_manifest manifest{account, snapshot_id, "grok-turn-cache@1", {}};
    std::uint64_t bytes = 0;
    for (const auto* name : {k_temporal_file, k_temporal_index}) {
        manifest.files = {{name, active_budget->limits().candidate_bytes, std::string(64, '0')}};
        const auto encoded = addon::encode_usage_cache_manifest(manifest);
        bytes += addon::serialize_json(member(encoded, "files").as_array().front()).size() + 1;
    }
    return bytes;
}

struct Native_times
{
    std::vector<std::pair<std::string, std::optional<std::string>>> rows;
    bool matched = false;
    bool capacity = false;
};

Native_times matching_native_times(const fs::path& cache_path,
    const std::function<bool()>& cancelled)
{
    Native_times times;
    std::ifstream cached;
    const auto header = read_cache_header(cache_path, &cached);
    if (member(header, "revision") != addon::Json_value(k_parser_revision) || header.find("legacy_source_key")) return times;
    const auto source = addon::path_from_utf8(string_member(header, "source"));
    if (!addon::same_native_account_profile(addon::path_from_utf8(string_member(header, "canonical_source")),
        canonical_native_path(path_text(source)))) return times;
    const auto before = fingerprint(source);
    const auto summary_before = fingerprint(source.parent_path() / "summary.json");
    if (!before || !summary_before || !fingerprint_matches(header, *before) ||
        member(header, "summary_size") != addon::Json_value(std::to_string(summary_before->size)) ||
        member(header, "summary_modified") != addon::Json_value(addon::file_time_ticks_text(summary_before->modified)))
    {
        return times;
    }
    std::ifstream native(source, std::ios::binary);
    if (!native) return times;
    std::string error;
    std::size_t bytes = 0;
    std::string line;
    const bool parsed = read_session_usage(native, source, [&](const Session_usage_record& record) {
        if (!read_line(cached, &line) || encode_record(decode_record(line)) != encode_record(record)) return false;
        if (record.usage.total_tokens != 0 && !times.capacity) {
            const auto size = record.usage.record_id.size() + record.instant.value_or("").size();
            if (size > k_sort_bytes - std::min(bytes, k_sort_bytes)) {
                times.rows.clear();
                times.capacity = true;
            }
            else {
                bytes += size;
                times.rows.emplace_back(record.usage.record_id, record.instant);
            }
        }
        return true;
    }, cancelled, &error);
    check_cancel(cancelled);
    const auto after = fingerprint(source);
    const auto summary_after = fingerprint(source.parent_path() / "summary.json");
    times.matched = parsed && !read_line(cached, &line) && after && summary_after &&
        *after == *before && *summary_after == *summary_before;
    if (!times.matched) times.rows.clear();
    std::sort(times.rows.begin(), times.rows.end(), [](const auto& a, const auto& b) { return id_less(a.first, b.first); });
    return times;
}

bool valid_temporal_sidecar(const fs::path& root, const std::optional<addon::Account_context>& account,
    const std::string& snapshot_id, const std::function<bool()>& cancelled)
{
    try {
        if (!fs::is_regular_file(fs::symlink_status(root / k_temporal_file)) ||
            !fs::is_regular_file(fs::symlink_status(root / k_temporal_index))) return false;
        const auto records_bytes = fs::file_size(root / "records");
        const auto proof_bytes = fs::file_size(root / k_temporal_file);
        const auto index_bytes = fs::file_size(root / k_temporal_index);
        const auto remaining = active_budget->limits().candidate_bytes -
            std::min<std::uintmax_t>(records_bytes, active_budget->limits().candidate_bytes);
        if (proof_bytes > remaining || index_bytes > remaining - proof_bytes || index_bytes % 16 != 0 ||
            index_bytes / 16 > active_budget->limits().candidate_records) return false;
        std::ifstream proof(root / k_temporal_file, std::ios::binary);
        std::ifstream index(root / k_temporal_index, std::ios::binary);
        std::ifstream records(root / "records", std::ios::binary);
        if (!proof || !index || !records) return false;
        std::string line;
        if (!read_line(proof, &line)) return false;
        const auto header = addon::parse_json(line, {}, nullptr);
        if (!header || !header->is_object() || header->as_object().size() != 4 ||
            member(*header, "version") != addon::Json_value(1) || member(*header, "account") != account_value(account) ||
            member(*header, "snapshot_id") != addon::Json_value(snapshot_id) ||
            member(*header, "accounting_ledger_sha256_hex") != addon::Json_value(file_digest(root / "records", cancelled)))
        {
            return false;
        }
        addon::Sha256 digest;
        digest.update(line); digest.update("\n");
        std::uint64_t count = 0, record_offset = 0, proof_offset = line.size() + 1;
        while (read_line(proof, &line)) {
            check_cancel(cancelled);
            const auto value = addon::parse_json(line, {}, nullptr);
            if (value && value->find("end")) {
                return value->is_object() && value->as_object().size() == 3 &&
                    member(*value, "end") == addon::Json_value(1) &&
                    member(*value, "count") == addon::Json_value(count) &&
                    member(*value, "sha256_hex") == addon::Json_value(addon::to_hex(digest.finish())) &&
                    !read_line(records, &line) && !read_line(proof, &line) && index.peek() == std::char_traits<char>::eof();
            }
            const auto row = decode_temporal_row(line);
            std::string accounting;
            if (!read_line(records, &accounting)) return false;
            const auto record = decode_record(accounting);
            std::array<char, 16> offsets{};
            if (!index.read(offsets.data(), offsets.size()) ||
                decode_temporal_index(offsets) != std::make_pair(record_offset, proof_offset) ||
                row.id != record.usage.record_id || row.total != record.usage.total_tokens ||
                row.accounting_digest != accounting_digest(accounting)) return false;
            ++count;
            record_offset += accounting.size() + 1;
            proof_offset += line.size() + 1;
            digest.update(line); digest.update("\n");
        }
    }
    catch (const Scan_failure& failure) { if (failure.reason == "cancelled") throw; }
    catch (const std::exception&) { check_cancel(cancelled); }
    return false;
}

class Temporal_budget final
{
public:
    explicit Temporal_budget(const fs::path& root)
    : m_remaining(active_budget->limits().candidate_bytes - fs::file_size(root / "records")) {}

    void write(std::ofstream& output, std::string_view bytes)
    {
        require(bytes.size() <= m_remaining - std::min(m_bytes, m_remaining), "usage_temporal_capacity");
        active_budget->reserve({bytes.size(), 0,
            static_cast<std::uint64_t>(std::count(bytes.begin(), bytes.end(), '\n'))});
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        require(static_cast<bool>(output), "usage_storage_write_failed");
        m_bytes += bytes.size();
    }
    void line(std::ofstream& output, std::string_view bytes)
    {
        write(output, bytes);
        write(output, "\n");
    }
    void remove(const fs::path& path)
    {
        const auto bytes = fs::file_size(path);
        require(bytes <= m_bytes, "usage_budget_accounting_invalid");
        remove_owned_path(path);
        m_bytes -= bytes;
    }
private:
    std::uint64_t m_remaining = 0;
    std::uint64_t m_bytes = 0;
};

bool copy_unknown_temporal(const fs::path& root,
    const std::optional<addon::Usage_cache_reference>& accepted,
    const std::optional<addon::Account_context>& account,
    Temporal_budget& budget, const std::function<bool()>& cancelled)
{
    if (!accepted) return false;
    const auto source = cache_io_path(addon::path_from_utf8(accepted->root));
    if (!fs::is_regular_file(fs::symlink_status(source / k_temporal_file)) ||
        !fs::is_regular_file(fs::symlink_status(source / k_temporal_index))) return false;
    std::ifstream proof(source / k_temporal_file, std::ios::binary);
    std::string line;
    if (!proof || !read_line(proof, &line)) return false;
    const auto header = addon::parse_json(line, {}, nullptr);
    const auto* version = header ? header->find("version") : nullptr;
    const auto* custody = header ? header->find("snapshot_id") : nullptr;
    if (!header || !header->is_object() || !version || !version->is_integer() ||
        version->as_integer() <= 0 || version->as_integer() == 1 || !custody ||
        !custody->is_string() || custody->as_string().empty() ||
        member(*header, "account") != account_value(account)) return false;
    const auto ledger = file_digest(source / "records", cancelled);
    if (member(*header, "accounting_ledger_sha256_hex") != addon::Json_value(ledger) ||
        file_digest(root / "records", cancelled) != ledger ||
        file_digest(root / "cache-inventory", cancelled) != file_digest(source / "cache-inventory", cancelled))
    {
        return false;
    }
    proof.close();
    // The accepted SDK seal already authenticates these opaque bytes. Keep
    // their original custody; neither future rows nor their index are decoded.
    for (const auto* name : {k_temporal_file, k_temporal_index}) {
        check_cancel(cancelled);
        const auto original = source / name;
        const auto destination = root / name;
        std::ifstream input(original, std::ios::binary);
        Budget_output output(destination, std::ios::binary | std::ios::trunc);
        require(input && output, "usage_temporal_unavailable");
        std::array<char, 65536> bytes{};
        while (input.read(bytes.data(), bytes.size()) || input.gcount() > 0) {
            check_cancel(cancelled);
            budget.write(output, std::string_view(bytes.data(), static_cast<std::size_t>(input.gcount())));
        }
        require(!input.bad(), "usage_temporal_unavailable");
        output.close(); input.close();
        require(static_cast<bool>(output) &&
            file_digest(destination, cancelled) == file_digest(original, cancelled), "usage_temporal_unavailable");
        sync_file(destination);
    }
    return true;
}

fs::path temporal_run(const fs::path& root, std::uint64_t level, std::uint64_t index)
{
    return root / ("temporal-run-" + std::to_string(level) + "-" + std::to_string(index));
}

void merge_temporal_runs(const fs::path& left_path, const fs::path& right_path,
    const fs::path& destination, Temporal_budget& budget, const std::function<bool()>& cancelled)
{
    std::ifstream left(left_path, std::ios::binary), right(right_path, std::ios::binary);
    Budget_output output(destination, std::ios::binary | std::ios::trunc);
    require(left && right && output, "usage_temporal_unavailable");
    std::string a, b;
    std::string a_id, b_id;
    const auto next = [](std::ifstream& input, std::string& line, std::string& id) {
        if (!read_line(input, &line)) return false;
        id = decode_temporal_row(line).id;
        return true;
    };
    bool have_a = next(left, a, a_id), have_b = next(right, b, b_id);
    while (have_a || have_b) {
        check_cancel(cancelled);
        require(!have_a || !have_b || a_id != b_id, "usage_temporal_unavailable");
        if (have_a && (!have_b || id_less(a_id, b_id))) {
            budget.line(output, a);
            have_a = next(left, a, a_id);
        }
        else {
            budget.line(output, b);
            have_b = next(right, b, b_id);
        }
    }
    output.close(); left.close(); right.close();
    require(static_cast<bool>(output), "usage_storage_write_failed");
    budget.remove(left_path); budget.remove(right_path);
}

fs::path sorted_temporal_bindings(const fs::path& root, Temporal_budget& budget,
    const std::function<bool()>& cancelled)
{
    std::vector<Temporal_row> chunk;
    std::size_t bytes = 0;
    std::uint64_t runs = 0;
    const auto flush = [&] {
        if (chunk.empty()) return;
        std::sort(chunk.begin(), chunk.end(), [](const auto& a, const auto& b) { return id_less(a.id, b.id); });
        Budget_output output(temporal_run(root, 0, runs++), std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "usage_storage_write_failed");
        for (const auto& row : chunk) budget.line(output, addon::serialize_json(encode_temporal_row(row)));
        output.close();
        require(static_cast<bool>(output), "usage_storage_write_failed");
        chunk.clear(); bytes = 0;
    };
    for (const auto& entry : fs::directory_iterator(root)) {
        check_cancel(cancelled);
        const auto name = entry.path().filename().string();
        if (!name.ends_with(".cache") || !digest_name(std::string_view(name).substr(0, name.size() - 6))) continue;
        const auto native = matching_native_times(entry.path(), cancelled);
        std::ifstream input;
        read_cache_header(entry.path(), &input);
        std::string line;
        while (read_line(input, &line)) {
            check_cancel(cancelled);
            auto record = decode_record(line);
            Temporal_row row;
            row.id = record.usage.record_id;
            row.total = record.usage.total_tokens;
            row.cache_digest = accounting_digest(line);
            refresh_repository(&record);
            row.accounting_digest = accounting_digest(encode_record(record));
            if (native.matched) {
                if (row.total == 0) row.reason = "complete";
                else if (native.capacity) row.reason = "capacity";
                else {
                    const auto found = std::lower_bound(native.rows.begin(), native.rows.end(), row.id,
                        [](const auto& observation, const auto& id) { return id_less(observation.first, id); });
                    require(found != native.rows.end() && found->first == row.id, "usage_temporal_unavailable");
                    row.instant = found->second;
                    row.reason = row.instant ? "complete" : "unsupported";
                }
            }
            const auto size = addon::serialize_json(encode_temporal_row(row)).size();
            if (!chunk.empty() && (chunk.size() >= k_sort_records || size > k_sort_bytes - std::min(bytes, k_sort_bytes))) flush();
            bytes += size;
            chunk.push_back(std::move(row));
        }
    }
    flush();
    std::uint64_t level = 0;
    while (runs > 1) {
        const auto next = (runs + 1) / 2;
        for (std::uint64_t index = 0; index < next; ++index) {
            check_cancel(cancelled);
            const auto destination = temporal_run(root, level + 1, index);
            if (index * 2 + 1 == runs) fs::rename(temporal_run(root, level, index * 2), destination);
            else merge_temporal_runs(temporal_run(root, level, index * 2),
                temporal_run(root, level, index * 2 + 1), destination, budget, cancelled);
        }
        ++level; runs = next;
    }
    if (runs == 0) {
        Budget_output output(temporal_run(root, 0, 0), std::ios::binary);
        require(static_cast<bool>(output), "usage_storage_write_failed");
    }
    return temporal_run(root, level, 0);
}

void write_temporal_sidecar(const fs::path& root, const fs::path& sorted,
    const std::optional<addon::Usage_cache_reference>& accepted,
    const std::optional<addon::Account_context>& account, const std::string& snapshot_id,
    Temporal_budget& budget, const std::function<bool()>& cancelled)
{
    std::ifstream prior, prior_records;
    std::string prior_line;
    std::string prior_record_digest;
    std::optional<Temporal_row> prior_row;
    const auto next_prior = [&] {
        prior_row.reset();
        prior_record_digest.clear();
        if (prior && read_line(prior, &prior_line)) {
            const auto value = addon::parse_json(prior_line, {}, nullptr);
            if (value && value->find("end")) return;
            prior_row = decode_temporal_row(prior_line);
            std::string accounting;
            require(read_line(prior_records, &accounting), "usage_temporal_unavailable");
            prior_record_digest = accounting_digest(encode_record(decode_record(accounting)));
        }
    };
    if (accepted) {
        const auto source = cache_io_path(addon::path_from_utf8(accepted->root));
        if (valid_temporal_sidecar(source, account, accepted->snapshot_id, cancelled)) {
            prior.open(source / k_temporal_file, std::ios::binary);
            prior_records.open(source / "records", std::ios::binary);
            require(read_line(prior, &prior_line), "usage_temporal_unavailable");
            next_prior();
        }
    }
    std::ifstream bindings(sorted, std::ios::binary), records(root / "records", std::ios::binary);
    Budget_output proof(root / k_temporal_file, std::ios::binary | std::ios::trunc);
    Budget_output index(root / k_temporal_index, std::ios::binary | std::ios::trunc);
    require(bindings && records && proof && index, "usage_temporal_unavailable");
    auto header = addon::Json_value::object();
    header.set("version", addon::Json_value(1));
    header.set("account", account_value(account));
    header.set("snapshot_id", addon::Json_value(snapshot_id));
    header.set("accounting_ledger_sha256_hex", addon::Json_value(file_digest(root / "records", cancelled)));
    addon::Sha256 digest;
    std::uint64_t proof_offset = 0;
    const auto append = [&](std::string_view line) {
        budget.line(proof, line);
        digest.update(line); digest.update("\n");
        proof_offset += line.size() + 1;
    };
    append(addon::serialize_json(header));
    std::uint64_t record_offset = 0, count = 0;
    std::string line, accounting;
    while (read_line(bindings, &line)) {
        check_cancel(cancelled);
        auto row = decode_temporal_row(line);
        require(read_line(records, &accounting), "usage_temporal_unavailable");
        const auto record = decode_record(accounting);
        require(row.id == record.usage.record_id && row.total == record.usage.total_tokens &&
            row.accounting_digest == accounting_digest(accounting), "usage_temporal_unavailable");
        while (prior_row && id_less(prior_row->id, row.id)) next_prior();
        if (prior_row && prior_row->id == row.id && prior_row->total == row.total &&
            (prior_row->cache_digest == row.cache_digest || prior_record_digest == row.accounting_digest))
        {
            if (row.reason == "missing-evidence") {
                row.instant = prior_row->instant;
                row.reason = prior_row->reason;
            }
            else if (row.instant && prior_row->instant && row.instant != prior_row->instant) {
                row.instant.reset();
                row.reason = "unsupported";
            }
        }
        const auto offsets = temporal_index_entry(record_offset, proof_offset);
        budget.write(index, std::string_view(offsets.data(), offsets.size()));
        append(addon::serialize_json(encode_temporal_row(row)));
        record_offset += accounting.size() + 1;
        ++count;
    }
    require(!read_line(records, &accounting), "usage_temporal_unavailable");
    auto footer = addon::Json_value::object();
    footer.set("end", addon::Json_value(1));
    footer.set("count", addon::Json_value(count));
    footer.set("sha256_hex", addon::Json_value(addon::to_hex(digest.finish())));
    budget.line(proof, addon::serialize_json(footer));
    proof.close(); index.close(); bindings.close(); records.close();
    require(proof && index, "usage_storage_write_failed");
    budget.remove(sorted);
    sync_file(root / k_temporal_file);
    sync_file(root / k_temporal_index);
}

void prepare_temporal(const fs::path& root, const std::optional<addon::Usage_cache_reference>& accepted,
    const std::optional<addon::Account_context>& account, const std::string& snapshot_id,
    const std::function<bool()>& cancelled)
{
    const Disk_usage manifest_delta{temporal_manifest_bytes(account, snapshot_id), 0, 0};
    if (!active_budget->try_reserve(manifest_delta)) return;
    try {
        Temporal_budget budget(root);
        if (copy_unknown_temporal(root, accepted, account, budget, cancelled)) return;
        const auto sorted = sorted_temporal_bindings(root, budget, cancelled);
        write_temporal_sidecar(root, sorted, accepted, account, snapshot_id, budget, cancelled);
        return;
    }
    catch (const Scan_failure& failure) { if (failure.reason == "cancelled") throw; }
    catch (const std::exception&) { check_cancel(cancelled); }
    // This phase exclusively owns these names. Original caches, records and
    // reserved accounting seal metadata remain intact after local refusal.
    for (const auto& entry : fs::directory_iterator(root)) {
        const auto name = entry.path().filename().string();
        if (name == k_temporal_file || name == k_temporal_index || name.starts_with("temporal-run-")) {
            remove_owned_path(entry.path());
        }
    }
    active_budget->release(manifest_delta);
}

bool valid_temporal_record(const addon::Usage_record& record)
{
    if (!record.temporal || !addon::decode_usage_temporal(addon::encode_usage_temporal(*record.temporal), nullptr)) return false;
    const auto encoded = addon::encode_usage_record(record, 3);
    return addon::decode_usage_record(encoded, nullptr, 3).has_value() &&
        addon::parse_json(addon::serialize_json(encoded), {}, nullptr).has_value();
}

class Temporal_lookup final
{
public:
    Temporal_lookup(const fs::path& root, const std::optional<addon::Account_context>& account,
        const std::string& snapshot_id)
    {
        try {
            if (!fs::is_regular_file(fs::symlink_status(root / k_temporal_file)) ||
                !fs::is_regular_file(fs::symlink_status(root / k_temporal_index))) return;
            const auto bytes = fs::file_size(root / k_temporal_index);
            if (bytes % 16 != 0 || bytes / 16 > Usage_scan_limits{}.candidate_records ||
                fs::file_size(root / k_temporal_file) > Usage_scan_limits{}.candidate_bytes) return;
            m_proof.open(root / k_temporal_file, std::ios::binary);
            m_index.open(root / k_temporal_index, std::ios::binary);
            m_proof_bytes = fs::file_size(root / k_temporal_file);
            std::string line;
            if (!m_proof || !m_index || !read_line(m_proof, &line)) return;
            const auto header = addon::parse_json(line, {}, nullptr);
            if (!header || !header->is_object() || header->as_object().size() != 4 ||
                member(*header, "version") != addon::Json_value(1) || member(*header, "account") != account_value(account) ||
                member(*header, "snapshot_id") != addon::Json_value(snapshot_id) ||
                !digest_name(string_member(*header, "accounting_ledger_sha256_hex"))) return;
            m_count = bytes / 16;
        }
        catch (const Scan_failure&) {}
        catch (const std::exception&) {}
    }

    void apply(addon::Usage_record& record, std::string_view accounting, std::uint64_t offset,
        const std::string& snapshot_id, const std::optional<std::string>& account)
    {
        try {
            std::uint64_t first = 0, last = m_count;
            std::array<char, 16> bytes{};
            while (first < last) {
                const auto middle = first + (last - first) / 2;
                m_index.clear();
                m_index.seekg(static_cast<std::streamoff>(middle * bytes.size()));
                require(static_cast<bool>(m_index.read(bytes.data(), bytes.size())), "usage_temporal_unavailable");
                if (decode_temporal_index(bytes).first < offset) first = middle + 1;
                else last = middle;
            }
            if (first == m_count) return;
            m_index.clear();
            m_index.seekg(static_cast<std::streamoff>(first * bytes.size()));
            require(static_cast<bool>(m_index.read(bytes.data(), bytes.size())), "usage_temporal_unavailable");
            const auto offsets = decode_temporal_index(bytes);
            require(offsets.first == offset && offsets.second < m_proof_bytes, "usage_temporal_unavailable");
            m_proof.clear();
            m_proof.seekg(static_cast<std::streamoff>(offsets.second));
            std::string line;
            require(read_line(m_proof, &line), "usage_temporal_unavailable");
            const auto row = decode_temporal_row(line);
            require(row.id == record.record_id && row.total == record.total_tokens &&
                row.accounting_digest == accounting_digest(accounting), "usage_temporal_unavailable");
            addon::Usage_temporal proof;
            proof.parent = {record.record_id, record.total_tokens, snapshot_id, account};
            proof.coverage_reason = row.reason;
            if (row.instant && row.total != 0) proof.portions.push_back({"instant", *row.instant, {}, {}, row.total});
            else proof.unavailable_tokens = row.total;
            record.temporal = std::move(proof);
            if (!valid_temporal_record(record)) record.temporal.reset();
        }
        catch (const Scan_failure&) { record.temporal.reset(); }
        catch (const std::exception&) { record.temporal.reset(); }
    }
private:
    std::ifstream m_proof;
    std::ifstream m_index;
    std::uint64_t m_count = 0;
    std::uint64_t m_proof_bytes = 0;
};

std::uint64_t decimal(std::string_view text)
{
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    require(!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
        std::to_string(value) == text, "usage_cursor_invalid");
    return value;
}

addon::Usage_scan_page page_at(const fs::path& snapshot, const std::string& identifier,
    std::uint64_t offset, const std::optional<addon::Account_context>& account, const std::function<bool()>& cancelled,
    const addon::Usage_scan_request& request)
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
    Temporal_lookup temporal(snapshot, account, identifier);
    page.account_context = request.context.account_context;
    page.profile_storage = request.context.profile_storage;
    // The final seal replaces only this fixed-width digest. All other
    // context and prospective cursor bytes participate in admission.
    page.cache_candidate = addon::Usage_cache_reference{
        request.cache_selection->candidate_root, identifier, std::string(64, '0')};
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
    std::size_t encoded_records_bytes = 0;
    while (page.records.size() < addon::k_max_usage_page_records && read_line(input, &line)) {
        check_cancel(cancelled);
        if (!page.records.empty() && bytes + line.size() > k_line_limit / 2) break;
        auto record = decode_record(line).usage;
        record.aggregate_evidence = "partial";
        temporal.apply(record, line, offset, identifier, page.scope.source_account_key);
        const auto next = offset + line.size() + 1;
        page.complete = next == size;
        page.next_cursor = page.complete ? std::nullopt : std::optional(std::to_string(next));
        if (!addon::append_usage_page_record(page, record, encoded_records_bytes, 3)) {
            if (!page.records.empty()) break;
            if (record.temporal) {
                record.temporal->portions.clear();
                record.temporal->unavailable_tokens = record.total_tokens;
                record.temporal->coverage_reason = "capacity";
                if (!valid_temporal_record(record)) record.temporal.reset();
            }
            if (!addon::append_usage_page_record(page, record, encoded_records_bytes, 3)) {
                record.temporal.reset();
                require(addon::append_usage_page_record(page, record, encoded_records_bytes, 3),
                    "usage_record_frame_capacity");
            }
        }
        bytes += line.size();
        offset += line.size() + 1;
    }
    page.complete = offset == size;
    if (!page.complete) page.next_cursor = std::to_string(offset);
    else page.next_cursor.reset();
    return page;
}

} // namespace

std::optional<addon::Usage_scan_page> scan_usage(
    const addon::Usage_scan_request& request,
    const fs::path& vendor_profile,
    const fs::path& storage,
    const std::function<bool()>& cancelled,
    std::string* error,
    const Usage_scan_limits& limits,
    const std::function<void()>& started)
{
    try {
        std::unique_lock lock(scan_mutex, std::defer_lock);
        while (!lock.try_lock_for(std::chrono::milliseconds(50))) check_cancel(cancelled);
        check_cancel(cancelled);
        if (!request.snapshot_id && started) {
            started();
        }
        addon::Contract_failure failure;
        require(addon::validate_usage_cache_selection(request, &failure), "usage_cache_selection_invalid");
        const auto& selection = *request.cache_selection;
        require(addon::validate_usage_cache_root(selection.candidate_root, addon::path_to_utf8(storage), &failure),
            "usage_cache_unavailable");
        const auto root = cache_io_path(addon::path_from_utf8(selection.candidate_root));
        auto identity = request;
        identity.snapshot_id.reset();
        identity.cursor.reset();
        const auto request_hash = addon::to_hex(addon::Sha256::digest(
            addon::serialize_json_canonical(addon::encode_usage_scan_request(identity, 3))));
        if (request.snapshot_id) {
            const auto state = read_json(root / "cache-state");
            require(state && string_member(*state, "request_sha256") == request_hash &&
                string_member(*state, "snapshot_id") == *request.snapshot_id, "usage_snapshot_mismatch");
            auto page = page_at(root, *request.snapshot_id, decimal(*request.cursor), request.context.account_context, cancelled, request);
            page.cache_candidate = addon::Usage_cache_reference{
                selection.candidate_root, page.snapshot_id, file_digest(root / "cache-manifest.json", cancelled)};
            page.account_context = request.context.account_context;
            page.profile_storage = request.context.profile_storage;
            for (auto& record : page.records) record.aggregate_evidence = "partial";
            return page;
        }
        require(addon::validate_usage_cache_allocation(selection.candidate_root, addon::path_to_utf8(storage), &failure),
            "usage_cache_unavailable");
        if (selection.accepted) {
            const auto manifest = addon::validate_usage_cache_candidate(*selection.accepted,
                addon::path_to_utf8(storage), request.context.account_context, cancelled, &failure);
            check_cancel(cancelled);
            require(manifest && manifest->format == "grok-turn-cache@1", "usage_cache_unavailable");
        }
        require(!request.cursor, "usage_cursor_invalid");
        Disk_budget budget(root, limits, cancelled);
        Budget_scope budget_scope(budget);
        require(fs::is_directory(vendor_profile), "usage_sessions_unavailable");
        const auto sessions = vendor_profile / "sessions";
        const bool have_sessions = fs::exists(sessions);
        require(!have_sessions || fs::is_directory(sessions), "usage_sessions_not_directory");
        const auto identifier = nonce();
        const auto& work = root;
        if (selection.accepted) {
            copy_accepted_cache(cache_io_path(addon::path_from_utf8(selection.accepted->root)), work,
                request.context.account_context, cancelled);
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
        check_cancel(cancelled);
        auto state = addon::Json_value::object();
        state.set("request_sha256", addon::Json_value(request_hash));
        state.set("snapshot_id", addon::Json_value(identifier));
        write_json(work / "cache-state", state);
        // The SDK writes its manifest outside Budget_output. Reserve the
        // complete original seal before optional scratch can consume space.
        budget.reserve({cache_manifest_bytes(work, request.context.account_context, identifier), 1, 0});
        prepare_temporal(work, selection.accepted, request.context.account_context, identifier, cancelled);
        auto page = page_at(work, identifier, 0, request.context.account_context, cancelled, request);
        // A surviving native session file does not prove complete aggregate history.
        for (auto& record : page.records) record.aggregate_evidence = "partial";
        page.cache_candidate = addon::seal_usage_cache_candidate(
            selection.candidate_root, addon::path_to_utf8(storage), request.context.account_context,
            identifier, "grok-turn-cache@1", cancelled, &failure);
        if (!page.cache_candidate) throw Scan_failure{failure.reason_code};
        page.account_context = request.context.account_context;
        page.profile_storage = request.context.profile_storage;
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
    const auto request = addon::decode_usage_scan_request(context.request().payload, &failure, 3);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
    const auto profile = selected_profile(request->context, context);
    if (!profile) return;
    if (!request->context.profile_storage) { context.fail(addon::Result_status::UNAVAILABLE, "profile_storage_unavailable"); return; }
    std::string error;
    const auto page = scan_usage(
        *request,
        addon::path_from_utf8(profile->profile_path),
        addon::path_from_utf8(request->context.profile_storage->root),
        [&context] { return context.cancel_requested(); }, &error, {},
        [&context] { context.report_progress(0, std::nullopt); });
    if (!page) { context.fail(error == "cancelled" ? addon::Result_status::CANCELLED : addon::Result_status::FAILED, error); return; }
    context.succeed(addon::encode_usage_scan_result(*page, 3));
}

} // namespace grok
