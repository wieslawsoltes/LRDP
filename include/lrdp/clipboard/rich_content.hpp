#pragma once
#include "lrdp/wire.hpp"
#include <optional>
namespace lrdp {
inline constexpr std::size_t rich_clipboard_limit = 8 * 1024 * 1024;
// Straight-alpha sRGB BGRA, top-down; not the premultiplied cursor representation.
struct ClipboardImage {
    std::uint32_t width = 0, height = 0;
    Bytes bgra;
    void validate() const;
    bool operator==(const ClipboardImage&) const = default;
};
struct RichClipboard {
    std::optional<std::string> text, html;
    std::optional<ClipboardImage> image;
    void validate() const;
    bool empty() const { return !text && !html && !image; }
};
void validate_clipboard_utf8(std::string_view text);
Bytes encode_clipboard_html(std::string_view fragment);
std::string decode_clipboard_html(View data);
Bytes encode_clipboard_dib(const ClipboardImage& image, bool version5 = true);
ClipboardImage decode_clipboard_dib(View data);
Bytes encode_clipboard_bmp(const ClipboardImage& image);
ClipboardImage decode_clipboard_bmp(View data);
// Optional native libpng adapter; no PNG dependency in the wire library.
Bytes encode_clipboard_png(const ClipboardImage& image);
ClipboardImage decode_clipboard_png(View data);
}
