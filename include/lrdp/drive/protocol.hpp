#pragma once
#include "lrdp/wire.hpp"
#include <chrono>
#include <compare>
#include <deque>
#include <map>
#include <optional>
#include <set>

namespace lrdp::drive {
using Clock = std::chrono::steady_clock;
inline constexpr std::uint32_t success = 0, denied = 0xc0000022, invalid_handle = 0xc0000008,
    removed = 0xc00002b6, unsupported = 0xc00000bb, no_more_files = 0x80000006, end_of_file = 0xc0000011;
struct DeviceKey {
    std::uint32_t id = 0;
    std::uint64_t generation = 0;
    auto operator<=>(const DeviceKey&) const = default;
};
struct PrinterInfo {
    std::uint32_t flags = 0;
    std::string driver, name;
    bool operator==(const PrinterInfo&) const = default;
};
struct Device {
    DeviceKey key;
    std::string name;
    std::optional<PrinterInfo> printer;
    bool operator==(const Device&) const = default;
};
// Local dispatch domain; never populated from peer-controlled packet contents.
enum class Purpose { filesystem, printer };
enum class Operation { open, close, read, write, query_information, query_directory, query_volume, set_information };
struct Request {
    std::uint64_t ticket = 0;
    DeviceKey device;
    Operation operation = Operation::open;
    std::uint64_t handle = 0, offset = 0;
    std::uint32_t length = 0, access = 0x80000000, disposition = 1, options = 0x00200000, information = 4;
    bool initial = true;
    std::string path;
    Bytes data;
    Purpose purpose = Purpose::filesystem;
};
struct Reply {
    std::uint64_t ticket = 0;
    std::uint32_t status = success;
    std::uint64_t handle = 0;
    std::uint32_t transferred = 0;
    Bytes data;
    Purpose purpose = Purpose::filesystem;
};
struct Limits {
    unsigned devices = 64, handles = 1024, outstanding = 64;
    std::uint32_t transfer = 65536;
    std::chrono::seconds timeout{30};
    bool writable = false;
    bool files = true, printers = false;
};
Bytes pdu(unsigned packet, View body = {});
std::uint64_t read_u64(Reader& in);
void write_u64(Writer& out, std::uint64_t value);
// POSIX-relative names become a drive-rooted RDP path. No ADS, parent traversal,
// remote UNC prefixes, embedded NUL, or alternate separator is accepted.
std::string wire_path(std::string_view path, bool wildcard = false);
PrinterInfo printer_information(View data);
Bytes request_body(const Request& request, std::uint32_t limit);
Bytes rename_information(std::string_view path, bool replace);

struct FileInfo {
    std::uint64_t created = 0, accessed = 0, modified = 0, changed = 0;
    std::uint64_t size = 0, allocated = 0;
    std::uint32_t attributes = 0, links = 1;
    bool directory = false, delete_pending = false;
};
struct DirectoryEntry { std::string name; FileInfo info; };
FileInfo basic_information(View data);
void standard_information(FileInfo& info, View data);
std::vector<DirectoryEntry> directory_information(View data);

// Owned by the RDP session event loop. No native filesystem calls or locks here.
// All public results are bounded; completion IDs are reused only after a reply.
class Protocol final {
    enum class Phase { idle, announce, name, capabilities, active };
    struct RemoteHandle { DeviceKey device; std::uint32_t file, access; bool directory; };
    struct Pending { Request request; Clock::time_point deadline; bool abandoned = false; };
    Limits limits_;
    Phase phase_ = Phase::idle;
    std::uint16_t minor_ = 13;
    std::uint32_t client_id_ = 0, extended_ = 0;
    bool client_drives_ = false, client_printers_ = false;
    std::uint64_t next_generation_ = 1, next_handle_ = 1;
    std::map<std::uint32_t, Device> devices_;
    std::map<std::uint64_t, RemoteHandle> handles_;
    std::map<std::uint32_t, Pending> pending_;
    std::set<std::pair<Purpose, std::uint64_t>> tickets_;
    std::deque<std::uint32_t> free_ids_;
    std::vector<Bytes> outbound_;
    std::vector<Reply> replies_;
    void capabilities(Reader& in);
    void announce_devices(Reader& in);
    void remove_devices(Reader& in);
    void complete(Reader& in);
    void emit(unsigned kind, View data = {});
public:
    explicit Protocol(Limits limits = {});
    void start(std::uint32_t client_id = 0x4c524450);
    void receive(View message);
    // false = caller retains the request and retries after backpressure clears.
    bool submit(const Request& request, Clock::time_point now = Clock::now());
    void tick(Clock::time_point now = Clock::now()) const;
    [[nodiscard]] bool ready() const { return phase_ == Phase::active; }
    [[nodiscard]] const Limits& limits() const { return limits_; }
    [[nodiscard]] std::vector<Device> devices() const;
    [[nodiscard]] std::size_t in_flight() const { return pending_.size(); }
    std::vector<Bytes> drain();
    std::vector<Reply> take_replies();
};
} // namespace lrdp::drive
