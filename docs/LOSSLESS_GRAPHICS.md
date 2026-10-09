# Lossless desktop graphics

`--encoder lossless` selects byte-exact GFX updates without advertising an H.264 encoder. It requires `--gfx auto`, but not FFmpeg. A client that does not negotiate the implemented GFX versions still receives the existing lossless bitmap fallback. `auto`, `software`, `vaapi`, `nvenc` and `raw` retain their previous behavior.

## Encoding decisions

The encoder compares actual BGRA bytes against one retained reference framebuffer. It divides the image into 128 x 32 tiles, skips equal rows/tiles, and bounds each changed tile to the smallest rectangle containing its changed pixels. It never uses a hash as proof that pixels match.

Uniform rectangles become SolidFill commands, grouped by the complete BGRA value into batches of at most 256 non-overlapping rectangles. Nonuniform opaque rectangles use the ClearCodec residual layer only when its entire payload is strictly smaller than uncompressed BGRA. Other rectangles use raw BGRA, including nonuniform alpha content. There is no lossy color conversion. The output is top-down and rectangle right/bottom coordinates are exclusive.

ClearCodec run lengths use the specified byte/16-bit/32-bit sentinel forms. The per-connection sequence counter advances only for transmitted ClearCodec commands, wraps after 255, and is not reset by framebuffer invalidation, suppression/resume or display resizing. Glyph/band caches and lossy subcodecs are not used.

The planner validates dimensions, stride, source extent and output budget before replacing its reference. Frame acknowledgement backpressure is checked before planning. An unchanged capture emits no framebuffer commands and consumes no frame credit. Statistics count covered update-rectangle pixels and unsegmented command bytes, not TCP/TLS bandwidth or capture time. In the single-pixel edit fixture, one SolidFill command occupies 24 bytes; StartFrame, EndFrame and transport headers are additional overhead.

This reduces transmitted data for suitable desktop content; it does not eliminate CPU framebuffer capture or scanning. This is not a zero-copy DMA-BUF path, a general-purpose ClearCodec decoder, or a full implementation of every GFX codec/cache.

## Validation

Revision `2d0daa1f8e44ce9ca6369fc10392b28c8b940aaa` passed [CI run 37927109397](https://github.com/wieslawsoltes/LRDP/actions/runs/37927109397): Debug ASan/UBSan, optimized Release and protocol-only builds/tests. Downloaded JUnit records 44 suites: 43 passed, one explicit skip, zero failures. The skipped AVC420 client test is unrelated to this codec and remains an unverified interoperability case.

`lossless` runs 259,307 checks with a separately written pixel decoder, run-width boundaries, sequence wrap, repeated refreshes, padded stride, alpha fallback, randomized frame mutations, exact damage and ACK credit. `lossless_session` decodes actual TCP/TLS/DVC traffic independently through input, clipboard and resize/reactivation. `client_lossless` checks sampled pixels from the installed FreeRDP client's actual X11 presentation with zero tolerance and rejects codec/update errors.

All 32 locally available sanitizer suites also passed sequentially as an ordinary user. A previous four-way local run hit the existing rich-clipboard fixture's timeout; five isolated repetitions and the complete sequential rerun passed. The timeout's cause was not established.

Windows/mobile clients, physical GPU execution, and real compositor capture remain unverified; these tests do not certify full RDP parity.

## Public protocol sources

- [ClearCodec stream and sequence counter, MS-RDPEGFX 2.2.4.1](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/f6c8a114-eaba-489f-9626-f41ad27a19b1)
- [Composite payload lengths](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/23253264-20f7-4a85-b6b4-023ca955cb3f)
- [Residual run encoding](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/c47e61d7-97db-4b35-93b9-3e41077f3ad0)
- [WireToSurface and ClearCodec identifier](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/6fa49bae-192f-4e25-888a-7cacfae303cf)
- [SolidFill](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/d696ab07-fd47-42f6-a601-c8b6fae26577)

Original implementation from public specifications. Independent clients are used as black boxes; their implementation source is not imported.
