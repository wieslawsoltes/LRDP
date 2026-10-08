#include "lrdp/clipboard/file_uri.hpp"

namespace lrdp {
namespace {
unsigned hex(char c) {
    if (c >= '0' && c <= '9') return unsigned(c - '0');
    if (c >= 'a' && c <= 'f') return unsigned(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return unsigned(c - 'A') + 10;
    throw ProtocolError("invalid file URI percent escape");
}
void absolute_path(std::string_view path) {
    require(!path.empty() && path[0] == '/' && path.size() <= 4096, "file URI must contain an absolute local path");
    for (const unsigned char c : path) require(c >= 32 && c != 127, "file URI contains a control character");
    (void)utf16le(path);
}
}
std::vector<std::string> decode_file_uris(std::string_view text, bool gnome) {
    require(text.size() <= 1024*1024, "file URI list exceeds policy");
    std::vector<std::string> paths; bool first = true;
    for (std::size_t start = 0; start < text.size();) {
        auto end = text.find('\n', start); if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(start, end-start); start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (gnome && first) { require(line == "copy" || line == "cut", "invalid copied-files action"); first = false; continue; }
        first = false; if (line.empty() || line[0] == '#') continue;
        require(paths.size() < 128 && line.starts_with("file://"), "non-file URI or too many clipboard paths"); line.remove_prefix(7);
        if (line.starts_with("localhost/")) line.remove_prefix(9);
        require(!line.empty() && line[0] == '/', "remote file URI authority is not permitted");
        std::string path;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '%') {
                require(i+2 < line.size(), "truncated file URI escape");
                path.push_back(char((hex(line[i+1]) << 4) | hex(line[i+2]))); i += 2;
            } else {
                require(line[i] != '?' && line[i] != '#', "file URI query or fragment is not permitted"); path.push_back(line[i]);
            }
        }
        absolute_path(path); paths.push_back(std::move(path));
    }
    require(!paths.empty(), "empty file URI clipboard"); return paths;
}
std::string encode_file_uris(const std::vector<std::string>& paths, bool gnome) {
    require(!paths.empty() && paths.size() <= 128, "invalid file URI path count");
    constexpr char digits[] = "0123456789ABCDEF"; std::string text = gnome ? "copy\n" : "";
    for (const auto& path : paths) {
        absolute_path(path); text += "file://";
        for (const unsigned char c : path) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') text += char(c);
            else { text += '%'; text += digits[c >> 4]; text += digits[c & 15]; }
        }
        text += gnome ? "\n" : "\r\n";
    }
    require(text.size() <= 1024*1024, "file URI list exceeds policy"); return text;
}
}
