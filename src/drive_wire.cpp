#include "lrdp/drive/protocol.hpp"
#include <algorithm>
#include <limits>
namespace lrdp::drive {
Bytes pdu(unsigned packet, View body) {
    Writer out; out.le16(0x4472).le16(packet).raw(body); return std::move(out).finish();
}
std::uint64_t read_u64(Reader& in) { const auto low = in.le32(); return std::uint64_t(in.le32()) << 32 | low; }
void write_u64(Writer& out, std::uint64_t value) { out.le32(std::uint32_t(value)).le32(std::uint32_t(value >> 32)); }
namespace {
void component(std::string_view name) {
    require(!name.empty() && name != "." && name != ".." && name.back() != '.' && name.back() != ' ', "invalid redirected path component");
    for (unsigned char c : name) require(c >= 32 && c != 127 && std::string_view("\\/:*?\"<>|").find(char(c)) == std::string_view::npos,
                                      "unsafe redirected filename character");
    require(utf16le(name, false).size() <= 510, "redirected filename exceeds UTF-16 component limit");
}
}
std::string wire_path(std::string_view path, bool wildcard) {
    require(path.size() <= 4096 && (path.empty() || path.front() != '/'), "redirected path must be relative");
    std::string result = "\\";
    std::size_t position = 0;
    while (position < path.size()) {
        auto slash = path.find('/', position); if (slash == std::string_view::npos) slash = path.size();
        component(path.substr(position, slash - position));
        if (position) result += '\\';
        result += path.substr(position, slash - position); position = slash + 1;
    }
    require(path.empty() || path.back() != '/', "trailing redirected path separator");
    if (wildcard) { if (result.back() != '\\') result += '\\'; result += '*'; }
    require(utf16le(result).size() <= 8192, "redirected UTF-16 path exceeds limit");
    return result;
}
Bytes rename_information(std::string_view path, bool replace) {
    const auto name = utf16le(wire_path(path), false);
    Writer out; out.u8(replace ? 1 : 0).u8(0).le32(std::uint32_t(name.size())).raw(name);
    return std::move(out).finish();
}
Bytes request_body(const Request& r, std::uint32_t limit) {
    Writer out;
    switch (r.operation) {
    case Operation::open: {
        require(r.disposition <= 5 && (r.options & ~0x00200061U) == 0 && (r.options & 0x41) != 0x41, "unsupported redirected create options");
        const auto path = utf16le(wire_path(r.path));
        out.le32(r.access); write_u64(out,0);
        out.le32(r.options & 1 ? 0x10 : 0x80).le32(7).le32(r.disposition).le32(r.options)
            .le32(std::uint32_t(path.size())).raw(path); break;
    }
    case Operation::close: out.zeros(32); break;
    case Operation::read: case Operation::write:
        require(r.length <= limit && r.offset <= std::uint64_t(std::numeric_limits<std::int64_t>::max()) - r.length, "redirected I/O range exceeds policy");
        require(r.operation != Operation::write || r.data.size() == r.length, "redirected write length mismatch");
        out.le32(r.length); write_u64(out,r.offset); out.zeros(20);
        if (r.operation == Operation::write) out.raw(r.data);
        break;
    case Operation::query_information:
        require(r.information == 4 || r.information == 5 || r.information == 35, "unsupported file information class");
        out.le32(r.information).le32(0).zeros(24); break;
    case Operation::query_volume:
        require(r.information == 3 || r.information == 7, "unsupported volume information class");
        out.le32(r.information).le32(0).zeros(24); break;
    case Operation::query_directory: {
        require(r.information == 1, "unsupported directory information class");
        const auto path = r.initial ? utf16le(wire_path(r.path,true)) : Bytes{};
        out.le32(1).u8(r.initial ? 1 : 0).le32(std::uint32_t(path.size())).zeros(23).raw(path); break;
    }
    case Operation::set_information:
        if (r.information == 13) require(r.data.empty(), "disposition uses an empty implied-delete buffer");
        else if (r.information == 20 || r.information == 19) {
            require(r.data.size() == 8, "invalid EOF/allocation information");
            Reader data(r.data); require(read_u64(data) <= std::uint64_t(std::numeric_limits<std::int64_t>::max()), "negative EOF/allocation");
        } else if (r.information == 4) require(r.data.size() == 40, "invalid FileBasicInformation set size");
        else if (r.information == 10) {
            Reader data(r.data); require(data.u8() <= 1 && data.u8() == 0, "invalid redirected rename flags");
            const auto size = data.le32(); require(size <= 8192, "rename path exceeds quota");
            auto name = from_utf16le(data.take(size),false); data.end();
            require(!name.empty() && name.front() == '\\', "rename path is not drive-rooted");
            name.erase(0,1); std::replace(name.begin(),name.end(),'\\','/'); (void)wire_path(name);
        } else throw ProtocolError("unsupported set information class");
        out.le32(r.information).le32(std::uint32_t(r.data.size())).zeros(24).raw(r.data); break;
    }
    return std::move(out).finish();
}
FileInfo basic_information(View data) {
    Reader in(data); FileInfo info;
    info.created=read_u64(in); info.accessed=read_u64(in); info.modified=read_u64(in); info.changed=read_u64(in);
    info.attributes=in.le32(); info.directory=(info.attributes & 0x10) != 0; in.end(); return info;
}
void standard_information(FileInfo& info, View data) {
    Reader in(data); info.allocated=read_u64(in); info.size=read_u64(in); info.links=in.le32();
    const auto deleted=in.u8(), directory=in.u8(); in.end();
    require(deleted <= 1 && directory <= 1 && info.size <= std::uint64_t(std::numeric_limits<std::int64_t>::max()) &&
            info.allocated <= std::uint64_t(std::numeric_limits<std::int64_t>::max()), "invalid standard file information");
    info.delete_pending=deleted != 0; info.directory=directory != 0;
}
std::vector<DirectoryEntry> directory_information(View data) {
    std::vector<DirectoryEntry> entries;
    std::size_t offset=0;
    while (offset < data.size()) {
        require(entries.size() < 4096, "directory batch exceeds entry quota");
        Reader in(data.subspan(offset)); const auto next=in.le32(); (void)in.le32();
        DirectoryEntry entry; auto& info=entry.info;
        info.created=read_u64(in); info.accessed=read_u64(in); info.modified=read_u64(in); info.changed=read_u64(in);
        info.size=read_u64(in); info.allocated=read_u64(in); info.attributes=in.le32();
        info.directory=(info.attributes & 0x10) != 0;
        require(info.size <= std::uint64_t(std::numeric_limits<std::int64_t>::max()) && info.allocated <= std::uint64_t(std::numeric_limits<std::int64_t>::max()), "invalid directory file sizes");
        const auto bytes=in.le32(); require(bytes && bytes <= 510, "invalid directory name length");
        entry.name=from_utf16le(in.take(bytes),false);
        if (entry.name != "." && entry.name != "..") component(entry.name);
        entries.push_back(std::move(entry));
        if (!next) { require(in.remaining() <= 7, "unexpected directory tail"); break; }
        require(next >= in.position() && next <= data.size()-offset && next % 8 == 0 && next-in.position() <= 7,
                "invalid directory next-entry offset");
        offset += next;
        require(offset < data.size(), "directory next-entry points beyond data");
    }
    return entries;
}
} // namespace lrdp::drive
