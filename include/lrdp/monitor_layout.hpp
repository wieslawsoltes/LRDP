#pragma once
#include "display.hpp"
namespace lrdp {
// MS-RDPBCGR 2.2.1.3.6 and 2.2.1.3.10, excluding their four-byte GCC header.
Layout decode_initial_monitors(View definitions, View attributes = {});
// MS-RDPBCGR 2.2.12.1: Share Data PDU, source zero, inclusive monitor edges.
Bytes monitor_layout_pdu(const Layout& layout);
}
