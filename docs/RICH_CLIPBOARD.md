# HTML and image clipboard

`--clipboard-rich` enables HTML and image clipboard on `x11` and `headless` desktops. It is off by default. It can be combined with `--clipboard-files DIRECTORY`; file selections take precedence over rich selections. Wayland rich content is not included in this branch.

## Wire and native formats

The RDP clipboard advertises Unicode text, registered `HTML Format`, `CF_DIBV5` (17), and `CF_DIB` (8) only when the current snapshot contains those types. Registered format IDs are mapped by name; the peer's numeric HTML format ID is not assumed to equal the server's ID. Text, HTML and an image are retrieved sequentially as one generation-tagged snapshot. Only a complete current snapshot is published natively; a failed optional format does not discard successful formats. An outstanding format-list offer retains its immutable data until the next offer is sent.

CF_HTML uses UTF-8 byte offsets, not Unicode character counts. The reader validates context/fragment/optional selection bounds, optional absent context, line endings, duplicate offsets and Unicode validity. The native `text/html` selection receives the fragment; external source URLs and surrounding context are not fetched or rendered. This is transport, **not HTML sanitization**: applications decide how to handle rich content when the user pastes it.

DIB conversion supports uncompressed 1/4/8-bit palettes, 16/24/32-bit RGB, and 16/32-bit contiguous disjoint bitfields; headers 40/108/124, top-down and bottom-up images, DWORD scanline padding, sRGB and straight alpha. DIBV5 output carries full straight-alpha pixels; legacy 24-bit DIB output is opaque. Compressed DIB, linked/embedded ICC profiles and nontrivial calibrated color spaces are rejected rather than interpreted incorrectly. Native `image/bmp` uses a validated packed BMP container. `image/png` is also available when libpng development support was found at build time; the server uses the system libpng library, not a private PNG implementation.

## Bounds and lifecycle

A rich snapshot is limited to 8 MiB including a reserved metadata allowance; image dimensions are at most 8192 in either direction and are additionally bounded by the decoded-byte quota. Both static-channel reassembly and native incremental transfers have compatible explicit bounds. UTF-8 validation does not allocate a full UTF-16 copy of HTML. Pixel geometry, row strides, palettes, masks and payload lengths are validated before image allocation. Ownership changes invalidate incomplete snapshots; changing to plain text removes stale image/HTML formats. Disabling rich mode retains the original text-only policy.

X11 TARGETS discovery, independent per-format conversion, INCR streaming in both directions, and snapshot-specific outgoing data preserve native clipboard ownership without application injection. Native transfers remain deadline-bounded. The optional flag does not enable file access or execute remote content.

## Verification

- Core codec/state tests: Unicode byte offsets; malformed/truncated headers; image pixels, alpha, rows and palette order; decompression/allocation-bomb bounds; advertised-ID mapping; offer coalescing; stale response cancellation; optional-format failure; 20,000 deterministic malformed-codec probes.
- Native Xlib fixture: multi-format HTML/BMP imports and exports, optional PNG roundtrips, 180 KiB incremental HTML, complete snapshots, ownership echo prevention and format revocation.
- Independent Python TLS client plus independent Xlib application: 1.2 MB HTML export, DIBV5 pixel oracle, fragmented HTML and alpha-image import, application paste verification while graphics are suppressed. Neither independent fixture links LRDP's clipboard codecs.

These tests do not establish Microsoft Office, Windows Explorer, real Wayland compositor, or ICC-managed clipboard interoperability.

## Public specifications

- Microsoft HTML Clipboard Format: https://learn.microsoft.com/en-us/windows/win32/dataxchg/html-clipboard-format
- Standard Clipboard Formats: https://learn.microsoft.com/en-us/windows/win32/dataxchg/standard-clipboard-formats
- BITMAPINFOHEADER: https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapinfoheader
- BITMAPV5HEADER: https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapv5header
- MS-RDPECLIP sections 2.2 and 3.1: format lists, data requests/responses, and long-format-name negotiation.
