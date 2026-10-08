#include "lrdp/clipboard/file_store.hpp"
#include <algorithm>
#include <map>

namespace lrdp {
namespace {
std::string folded(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
    return value;
}
std::uint64_t read64(Reader& reader) {
    const auto low = reader.le32(); return std::uint64_t(reader.le32()) << 32 | low;
}
}
std::string validate_clipboard_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    require(!path.empty() && path.size() <= 1024 && utf16le(path).size() <= 520, "clipboard file name exceeds policy");
    unsigned depth = 0;
    for (std::size_t start = 0; start <= path.size();) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end == std::string::npos ? end : end - start);
        require(!part.empty() && part != "." && part != ".." && ++depth <= 16, "unsafe clipboard file path");
        require(part.back() != '.' && part.back() != ' ', "ambiguous clipboard file name");
        for (const unsigned char c : part)
            require(c >= 32 && c != 127 && c != ':' && c != '*' && c != '?' && c != '"' && c != '<' && c != '>' && c != '|',
                    "unsupported clipboard file name character");
        const auto base = folded(part.substr(0, part.find('.')));
        require(base != "con" && base != "prn" && base != "aux" && base != "nul" && base != "conin$" && base != "conout$" &&
                !(base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '1' && base[3] <= '9'),
                "reserved clipboard file name");
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return path;
}
std::uint64_t validate_clipboard_files(const std::vector<ClipboardFile>& files, FileClipboardLimits limits, bool sizes_required) {
    require(limits.entries > 0 && limits.entries <= 1024 && limits.bytes <= 1024ULL*1024*1024,
            "invalid file clipboard limits");
    require(!files.empty() && files.size() <= limits.entries, "clipboard file count exceeds policy");
    std::map<std::string, bool> names;
    std::uint64_t total = 0;
    for (const auto& file : files) {
        const auto name = folded(validate_clipboard_path(file.name));
        require(names.emplace(name, file.directory).second, "duplicate clipboard file name");
        if (!file.directory) {
            require(!sizes_required || file.size.has_value(), "missing clipboard file size");
            if (file.size) {
                require(*file.size <= limits.bytes - total, "clipboard file allocation exceeds policy");
                total += *file.size;
            }
        }
    }
    for (const auto& [name, directory] : names) {
        (void)directory;
        for (auto slash = name.find('/'); slash != std::string::npos; slash = name.find('/', slash + 1)) {
            auto parent = names.find(name.substr(0, slash));
            require(parent != names.end() && parent->second, "clipboard file parent is absent or not a directory");
        }
    }
    return total;
}
Bytes encode_clipboard_files(const std::vector<ClipboardFile>& files, FileClipboardLimits limits) {
    (void)validate_clipboard_files(files, limits);
    Writer out; out.le32(std::uint32_t(files.size()));
    for (const auto& file : files) {
        auto name = validate_clipboard_path(file.name); std::replace(name.begin(), name.end(), '/', '\\');
        auto encoded = utf16le(name);
        const auto size = file.directory ? 0 : file.size.value_or(0);
        const auto flags = 4U | 0x4000U | (file.size && !file.directory ? 0x40U : 0U) | (file.modified ? 0x20U : 0U);
        out.le32(flags).zeros(32).le32(file.directory ? 0x10 : 0x80).zeros(16)
            .le32(std::uint32_t(file.modified.value_or(0))).le32(std::uint32_t(file.modified.value_or(0) >> 32))
            .le32(std::uint32_t(size >> 32)).le32(std::uint32_t(size)).raw(encoded).zeros(520 - encoded.size());
    }
    return std::move(out).finish();
}
std::vector<ClipboardFile> decode_clipboard_files(View payload, FileClipboardLimits limits) {
    Reader in(payload); const auto count = in.le32();
    require(count > 0 && count <= limits.entries && count <= 1024 && in.remaining() == std::size_t(count)*592,
            "invalid clipboard file descriptor count or length");
    std::vector<ClipboardFile> files; files.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        ClipboardFile file;
        const auto flags = in.le32(); in.skip(32); const auto attributes = in.le32(); in.skip(16);
        const auto modified = read64(in); const auto high = in.le32(), low = in.le32();
        file.directory = (flags & 4) && (attributes & 0x10);
        if (flags & 0x20) file.modified = modified;
        if (file.directory) file.size = 0;
        else if (flags & 0x40) file.size = std::uint64_t(high) << 32 | low;
        const auto name = in.take(520); std::size_t length = 0;
        while (length < name.size() && (name[length] || name[length + 1])) length += 2;
        require(length < name.size(), "unterminated clipboard file name");
        file.name = validate_clipboard_path(from_utf16le(name.first(length + 2)));
        files.push_back(std::move(file));
    }
    (void)validate_clipboard_files(files, limits); return files;
}
} // namespace lrdp
