#include "lrdp/mcs.hpp"
namespace lrdp {
namespace {
class Capabilities {
    Writer bytes_;
    unsigned count_ = 0;
public:
    void add(unsigned type, const Writer& body) {
        require(count_ < 64,"too many server capabilities");
        bytes_.le16(type).le16(unsigned(body.size()+4)).raw(body.bytes()); ++count_;
    }
    const Bytes& bytes() const { return bytes_.bytes(); }
    unsigned count() const { return count_; }
};
}
Bytes demand_active(std::uint16_t width, std::uint16_t height, std::uint16_t depth, bool resize, bool unicode_input) {
    Capabilities caps;
    // MS-RDPBCGR 2.2.7.1.1: exactly 20 payload bytes. Both one-byte
    // refresh/suppress support fields immediately follow compressionLevel.
    Writer general; general.le16(4).le16(0).le16(0x200).le16(0)
        .le16(0).le16(0).le16(0).le16(0).le16(0).u8(1).u8(1);
    caps.add(1,general);
    Writer bitmap; bitmap.le16(depth).le16(1).le16(1).le16(1).le16(width).le16(height).le16(0).le16(resize ? 1 : 0)
        .le16(1).u8(0).u8(0).le16(1).le16(0); caps.add(2,bitmap);
    Writer order; order.zeros(20).le16(1).le16(20).le16(0).le16(1).le16(0).le16(2).zeros(52);
    caps.add(3,order); // No drawing orders or caches advertised.
    Writer pointer; pointer.le16(1).le16(0); caps.add(8,pointer);
    Writer share; share.le16(server_user).le16(0); caps.add(9,share);
    Writer input; input.le16(unicode_input ? 0x135 : 0x125).zeros(82); caps.add(13,input);
    Writer font; font.le16(1).le16(0); caps.add(14,font);
    Writer vc; vc.le32(0).le32(1600); caps.add(20,vc);
    Writer body; body.le32(share_id).le16(5).le16(unsigned(caps.bytes().size()+4)).raw({'L','R','D','P',0})
        .le16(caps.count()).le16(0).raw(caps.bytes()).le32(0);
    return share_control(1,body.bytes());
}
}
