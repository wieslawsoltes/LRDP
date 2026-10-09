# Negotiated large cursors

The server advertises fast-path output, a 608299-byte multifragment bound and both large-pointer flags. Client capability order is immaterial. 96- and 384-pixel output requires the matching flag, sufficient reassembly size, fast-path output, and a New Pointer cache. Otherwise shapes are fitted to the supported limit.

96-pixel shapes use fragmented Fast-Path New Pointer updates. Larger shapes use Fast-Path Large Pointer updates with 32-bit mask lengths. Every transport PDU is at most 16383 bytes. No bulk compression or legacy RDP encryption is advertised by this TLS-only encoder. Hotspots and exact-match LRU semantics survive size changes; activation clears negotiated cache state.

Sources: Microsoft MS-RDPBCGR sections 2.2.7.1.1, 2.2.7.2.6, 2.2.7.2.7, 2.2.9.1.2.1 and 2.2.9.1.2.1.11.
https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/41323437-c753-460e-8108-495a6fdd68a8
https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/037f4d6c-5753-4627-ba5e-ce1b8e9bc0cd

Tests independently reassemble packets, verify fragment order and wide lengths, exact pixels, edge hotspots, malformed quotas and downgrade behavior. Windows and physical-display interoperability still needs testing.
