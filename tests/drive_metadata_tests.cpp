#include "lrdp/drive/filesystem.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <random>

using namespace lrdp;
using namespace lrdp::drive;
namespace {
unsigned checks = 0;
void check(bool condition, const char* message) { ++checks; if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    ++checks; try { action(); } catch (const ProtocolError&) { return; }
    throw std::runtime_error("malformed metadata accepted");
}
template<class F> void error(F action, int code) {
    ++checks; try { action(); } catch (const IoError& e) {
        if (e.code().value() == code) return;
        throw std::runtime_error("incorrect native metadata error");
    }
    throw std::runtime_error("expected native metadata failure");
}
Bytes identity(std::string_view name = "Données 日本語 🚀 ", bool terminated = false, unsigned truth = 255) {
    const auto text = utf16le(name, terminated);
    Writer out; write_u64(out, 116444736000000001ULL);
    out.le32(0x89abcdef).le32(unsigned(text.size())).u8(truth).raw(text); return out.bytes();
}
Bytes attributes(std::uint32_t flags = 0x00080007, std::string_view name = "NTFS", unsigned maximum = 255) {
    const auto text = utf16le(name, false);
    Writer out; out.le32(flags).le32(maximum).le32(unsigned(text.size())).raw(text); return out.bytes();
}
Bytes device(std::uint32_t flags = 0, unsigned kind = 7) {
    Writer out; out.le32(kind).le32(flags); return out.bytes();
}
Bytes size_record(unsigned kind) {
    Writer out; write_u64(out, 1ULL<<32); write_u64(out, 123);
    if (kind == 7) write_u64(out, 999);
    out.le32(8).le32(512); return out.bytes();
}
class Peer final : public Client {
public:
    Limits policy;
    std::uint64_t generation = 2;
    std::map<unsigned, Bytes> data{{1, identity()}, {3, size_record(3)}, {4, device()}, {5, attributes()}, {7, size_record(7)}};
    std::map<unsigned, std::uint32_t> statuses;
    std::vector<Request> requests;
    unsigned live = 0, opens = 0, closes = 0;
    unsigned remove_on = 0;
    Reply call(Request request) override {
        requests.push_back(request);
        Reply response;
        if (request.device != DeviceKey{42, generation}) { response.status = removed; return response; }
        if (request.operation == Operation::open) {
            check(request.access == 0x80 && request.disposition == 1 && (request.options & 0x200000), "metadata must not request mutation rights");
            check(live == 0, "single metadata handle lease"); ++live; ++opens; response.handle = 19;
        } else if (request.operation == Operation::close) {
            check(live == 1 && request.handle == 19, "metadata handle closed exactly once"); --live; ++closes;
        } else if (request.operation == Operation::query_information) {
            Writer out;
            if (request.information == 4) out.zeros(32).le32(0x10);
            else out.zeros(16).le32(1).u8(0).u8(1);
            response.data = out.bytes();
        } else {
            check(request.operation == Operation::query_volume && request.handle == 19, "metadata query uses pinned handle");
            if (remove_on == request.information) { ++generation; live = 0; response.status = removed; return response; }
            response.status = statuses[request.information];
            if (!response.status) response.data = data.at(request.information);
        }
        return response;
    }
    std::vector<Device> devices() const override { return {{{42, generation}, "DATA"}}; }
    const Limits& limits() const override { return policy; }
    unsigned queries(unsigned kind) const {
        return unsigned(std::count_if(requests.begin(), requests.end(), [&](const Request& r) {
            return r.operation == Operation::query_volume && r.information == kind;
        }));
    }
};
void wire() {
    for (const bool terminated : {false, true}) for (const unsigned truth : {0U, 1U, 128U, 255U}) {
        const auto bytes = identity("Données 日本語 🚀 ", terminated, truth);
        const auto result = volume_identity(bytes);
        check(result.label == "Données 日本語 🚀 " && result.supports_objects == (truth != 0), "length-delimited label, spaces and any-nonzero Boolean");
        check(result.serial == 0x89abcdef && result.created_100ns == 116444736000000001ULL, "opaque serial and 64-bit timestamp");
        for (std::size_t n = 0; n < bytes.size(); ++n) rejects([&] { (void)volume_identity(View(bytes).first(n)); });
        auto padded = bytes; padded.insert(padded.begin()+17, 0); rejects([&] { (void)volume_identity(padded); });
    }
    check(volume_identity(identity("", false)).label.empty(), "unlabelled volume is valid");
    check(volume_identity(identity("", true)).label.empty(), "empty NUL-terminated label is valid");
    auto negative = identity(); negative[7] |= 0x80; rejects([&] { (void)volume_identity(negative); });
    rejects([&] { (void)volume_identity(identity(std::string("a\0b", 3))); });
    auto extra_nul = identity("a", true); extra_nul.push_back(0); extra_nul.push_back(0); extra_nul[12] += 2;
    rejects([&] { (void)volume_identity(extra_nul); });
    rejects([&] { (void)volume_identity(identity(std::string(2049, 'a'))); });
    const auto attr = volume_attributes(attributes(0x80080007));
    check(attr.flags == 0x80080007 && attr.read_only() && attr.filesystem == "NTFS", "unknown flags retained without enabling semantics");
    for (unsigned maximum : {0U, 256U, 0xffffffffU}) rejects([&] { (void)volume_attributes(attributes(7, "NTFS", maximum)); });
    rejects([&] { (void)volume_attributes(attributes(0x8010)); });
    rejects([&] { (void)volume_attributes(attributes(0, "")); });
    for (unsigned zeros = 0; zeros <= 8; ++zeros) {
        const auto terminated = std::string("NTFS") + std::string(zeros, '\0');
        check(volume_attributes(attributes(7, terminated, 260)).filesystem == "NTFS",
              "informational filesystem name permits only a bounded zero suffix");
        check(volume_attributes(attributes(7, terminated, 260)).max_component_utf16 == 255,
              "client component hint cannot expand native path policy");
    }
    check(volume_attributes(attributes(7, "日本語 🚀 ")).filesystem == "日本語 🚀 ",
          "filesystem Unicode and trailing spaces are preserved");
    rejects([&] { (void)volume_attributes(attributes(0, std::string("NT\0FS",5))); });
    rejects([&] { (void)volume_attributes(attributes(0, std::string("NTFS\0hidden\0",12))); });
    rejects([&] { (void)volume_attributes(attributes(0, std::string(4, '\0'))); });
    rejects([&] { (void)volume_attributes(attributes(0, std::string(2049, 'x'))); });
    auto odd_name = attributes(); odd_name[8] = 7; odd_name.pop_back();
    rejects([&] { (void)volume_attributes(odd_name); });
    auto surrogate_name = attributes(); surrogate_name[12] = 0; surrogate_name[13] = 0xd8;
    rejects([&] { (void)volume_attributes(surrogate_name); });
    for (unsigned kind : {2U, 7U}) {
        const auto value = volume_device(device(0x80000002, kind));
        check(value.read_only() && value.type == kind, "disk/CD and readonly device metadata");
    }
    rejects([&] { (void)volume_device(device(0, 0)); });
    for (const auto kind : {1U,3U,4U,5U,7U}) {
        Request request; request.operation = Operation::query_volume; request.information = kind;
        const auto bytes = request_body(request, 65536); Reader in(bytes);
        check(bytes.size() == 32 && in.le32() == kind && in.le32() == 0, "five documented query classes use no mutation or query payload");
        const auto value = kind == 1 ? identity() : kind == 5 ? attributes() : kind == 4 ? device() : size_record(kind);
        validate_volume_information(value, kind);
        for (std::size_t n = 0; n < value.size(); ++n) rejects([&] { validate_volume_information(View(value).first(n),kind); });
        auto tail = value; tail.push_back(0); rejects([&] { validate_volume_information(tail,kind); });
    }
    rejects([] { validate_volume_information({}, 6); });
    std::mt19937 random(0x4d455441);
    for (unsigned i = 0; i < 25000; ++i) {
        Bytes bytes(random()%96); for (auto& value : bytes) value = std::uint8_t(random());
        try { validate_volume_information(bytes, std::array{1U,4U,5U}[i%3]); } catch (const ProtocolError&) {}
    }
}
void native() {
    Peer peer; Filesystem fs(peer); const auto path = "/"+device_name(peer.devices()[0]);
    check(fs.volume_details("/").identity == std::nullopt && fs.listxattr("/").empty() && peer.requests.empty(), "synthetic root has no fabricated metadata");
    const auto info = fs.volume_details(path);
    check(info.identity && info.attributes && info.device && info.read_only() && peer.opens == 1 && peer.closes == 1, "one generation-bound handle for complete metadata snapshot");
    std::array<std::pair<std::string_view,std::string>,8> values{{
        {"user.lrdp.volume.label", "Données 日本語 🚀 "},
        {"user.lrdp.volume.serial", "0x89abcdef"},
        {"user.lrdp.volume.created-100ns", "116444736000000001"},
        {"user.lrdp.volume.filesystem", "NTFS"},
        {"user.lrdp.volume.flags", "0x00080007"},
        {"user.lrdp.volume.max-component-utf16", "255"},
        {"user.lrdp.volume.device-type", "0x00000007"},
        {"user.lrdp.volume.device-characteristics", "0x00000000"}
    }};
    Bytes names;
    for (const auto& [name, expected] : values) {
        const auto result = fs.getxattr(path+"/file", name);
        check(std::string(result.begin(),result.end()) == expected, "volume annotation bytes and numeric formatting");
        check(native_xattr(result,nullptr,0) == int(result.size()), "native attribute size probe");
        std::array<char,8192> buffer; buffer.fill('!'); const auto before = buffer;
        if (result.size() > 1) error([&] { (void)native_xattr(result,buffer.data(),result.size()-1); },ERANGE);
        check(buffer == before, "short native attribute buffer remains unchanged");
        check(native_xattr(result,buffer.data(),buffer.size()) == int(result.size()) && buffer[result.size()] == '!', "bounded native attribute copy, no NUL appended");
        names.insert(names.end(), name.begin(),name.end()); names.push_back(0);
    }
    check(fs.listxattr(path) == names && peer.live == 0, "exact NUL-delimited supported annotation names");
    const auto count = peer.requests.size();
    error([&] { (void)fs.getxattr(path,"security.capability"); }, ENODATA);
    error([&] { (void)fs.getxattr("/","user.lrdp.volume.label"); }, ENODATA);
    check(peer.requests.size() == count, "unknown attributes never reach the peer");
    peer.policy.writable = true; struct statvfs result{};
    fs.statfs(path,result); check(result.f_blocks == (1ULL<<32) && result.f_bavail == 123 && (result.f_flag & ST_RDONLY), "remote volume readonly overrides local write opt-in for reporting");
    peer.data[5] = attributes(7); peer.data[4] = device(2);
    fs.statfs(path,result); check(result.f_flag & ST_RDONLY, "readonly device overrides reported writability");
    peer.data[4] = device(0); fs.statfs(path,result); check(!(result.f_flag & ST_RDONLY), "fresh metadata adopts changed remote flags");
    peer.policy.writable = false; fs.statfs(path,result); check(result.f_flag & ST_RDONLY, "peer cannot remove local readonly policy");
    for (const auto status : {unsupported,0xc0000003U,0xc0000010U}) {
        peer.statuses[5] = status; peer.statuses[4] = status;
        fs.statfs(path,result); check(result.f_blocks > 0, "capacity survives explicitly unsupported optional metadata");
        const auto details = fs.volume_details(path);
        check(details.identity && !details.attributes && !details.device, "optional availability is explicit");
        const auto listed = fs.listxattr(path);
        check(std::count(listed.begin(),listed.end(),0) == 3, "unavailable annotations are not listed");
        error([&] { (void)fs.getxattr(path,"user.lrdp.volume.filesystem"); }, ENODATA);
    }
    peer.statuses.clear(); peer.statuses[5] = denied;
    const auto before = result;
    error([&] { fs.statfs(path,result); }, EACCES);
    check(std::memcmp(&before,&result,sizeof(result)) == 0 && peer.live == 0, "failed optional query cannot publish partial statvfs or leak handle");
    peer.statuses.clear(); peer.data[1].pop_back();
    rejects([&] { (void)fs.volume_details(path); }); check(peer.live == 0, "malformed metadata closes handle, never returns partial details");
    peer.data[1] = identity("new label");
    const auto changed = fs.getxattr(path,"user.lrdp.volume.label");
    check(std::string(changed.begin(),changed.end()) == "new label", "no stale metadata cache");
    peer.remove_on = 5; error([&] { fs.statfs(path,result); },ENODEV);
    check(std::memcmp(&before,&result,sizeof(result)) == 0, "device generation change cannot mix a new volume into prior snapshot");
    const auto previous = peer.requests.size(); error([&] { (void)fs.volume_details(path); },ENODEV);
    check(peer.requests.size() == previous, "stale native path cannot resolve replacement drive");
    error([] { (void)native_xattr(Bytes{1},nullptr,1); },EINVAL);
}
}
int main() {
    try { wire(); native(); std::cout << checks << " checks passed plus 25000 malformed metadata probes; volume labels, RDP header, flags, statvfs, xattrs, unsupported/error policy and generation lifecycle\n"; }
    catch (const std::exception& e) { std::cerr << "FAIL after " << checks << ": " << e.what() << '\n'; return 1; }
}
