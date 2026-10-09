#include "lrdp/reconnect.hpp"
#include <algorithm>

namespace lrdp {
namespace {
void unicode(View bytes, bool terminated) {
    Reader in(bytes);
    require(!(bytes.size() & 1), "odd Client Info Unicode length");
    if (terminated) {
        require(bytes.size() >= 2 && bytes[bytes.size()-2] == 0 && bytes.back() == 0,
                "unterminated Client Info string");
        in = Reader(bytes.first(bytes.size()-2));
    }
    while (!in.empty()) {
        const auto unit = in.le16(); require(unit != 0, "embedded Client Info NUL");
        if (unit >= 0xd800 && unit <= 0xdbff) {
            const auto low = in.le16(); require(low >= 0xdc00 && low <= 0xdfff, "invalid Client Info surrogate pair");
        } else require(unit < 0xdc00 || unit > 0xdfff, "orphan Client Info surrogate");
    }
}
void sized_unicode(Reader& in, unsigned maximum, bool terminated) {
    const auto count = in.le16(); require(count <= maximum, "Client Info string exceeds limit");
    unicode(in.take(count), terminated);
}
}
ReconnectCookie decode_reconnect_cookie(View bytes) {
    Reader in(bytes); require(bytes.size() == 28 && in.le32() == 28 && in.le32() == 1, "invalid reconnect cookie framing/version");
    ReconnectCookie value; value.logon_id = in.le32();
    const auto key = in.take(16); std::copy(key.begin(), key.end(), value.bytes.begin()); in.end(); return value;
}
Bytes encode_reconnect_cookie(const ReconnectCookie& cookie) {
    Writer out(28); out.le32(28).le32(1).le32(cookie.logon_id).raw(cookie.bytes); return std::move(out).finish();
}
std::optional<ReconnectCookie> client_info_reconnect(View payload) {
    Reader in(payload); require(in.le16() == 0x40 && in.le16() == 0, "expected protected Client Info");
    in.skip(4); require(in.le32() & 0x10, "Unicode Client Info required");
    unsigned lengths[5];
    for (auto& n : lengths) { n = in.le16(); require(n <= 4096 && !(n & 1), "Client Info string exceeds policy"); }
    for (const auto n : lengths) unicode(in.take(n+2), true);
    require(in.remaining() <= 8192, "extended Client Info exceeds policy");
    if (in.empty()) return {};
    const auto family = in.le16(); require(family == 2 || family == 23, "invalid Client Info address family");
    sized_unicode(in, 80, true); sized_unicode(in, 512, true);
    if (in.empty()) return {};
    in.skip(172); // Informational timezone: never applied as an authentication identity.
    if (in.empty()) return {};
    in.skip(4); // Ignored clientSessionId is NOT the reconnect LogonId.
    if (in.empty()) return {};
    in.skip(4); // performanceFlags.
    if (in.empty()) return {};
    const auto length = in.le16(); require(length == 0 || length == 28, "invalid reconnect cookie length");
    std::optional<ReconnectCookie> cookie;
    if (length) cookie = decode_reconnect_cookie(in.take(length));
    if (in.empty()) return cookie;
    in.skip(2); require(in.le16() == 0, "invalid Client Info reserved2");
    if (in.empty()) return cookie;
    sized_unicode(in, 254, false); require(in.le16() <= 1, "invalid dynamic DST flag"); in.end(); return cookie;
}
Bytes reconnect_logon_info(const ReconnectCookie& cookie) {
    // TS_LOGON_INFO_EXTENDED.Length includes its 570-byte Pad and LogonFields.
    Writer out; out.le32(3).le16(608).le32(1).le32(28).raw(encode_reconnect_cookie(cookie)).zeros(570);
    return std::move(out).finish();
}
} // namespace lrdp
