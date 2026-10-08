# File clipboard and cursor remoting

## File transfer setup and semantics

`--clipboard-files DIRECTORY` is an explicit per-server opt-in for the `x11` and `headless` backends. The directory must already exist, be owned by the server's Unix user, and not be writable by other users. Directory components must not be symlinks. The server rejects `/` as a root. Use a dedicated directory rather than exposing a home directory containing unrelated data.

A native file-manager copy must expose `text/uri-list` or `x-special/gnome-copied-files`. LRDP recognizes local `file:///...` and `file://localhost/...` URIs, percent-decodes UTF-8, and rejects remote authorities, query/fragment syntax and control characters. GNOME cut offers are treated as copies: LRDP never deletes an exported source.

Local exports are restricted to selected regular files and directories beneath the configured root. The source inode is pinned with `O_PATH`, inspected before opening, and read by descriptor. Symlinks, devices, FIFOs and sockets cannot become source files. Reads check inode, size and modification/change timestamps before and after access; an observed mutation fails the transfer rather than silently producing mixed content. This is not an immutable filesystem snapshot against a malicious process with the same Unix identity.

On reception, LRDP validates all relative descriptors before creating files. It rejects absolute/traversing paths, missing directory parents, ambiguous ASCII-case duplicate names, reserved DOS names, trailing dots/spaces and alternate data streams. Empty files/directories and UTF-16 names are supported. Filename validation is deliberately restrictive; full Windows Unicode collation and all filesystem-specific naming rules are not implemented.

Received directories have mode 0700 and files mode 0600. Files are written into `.lrdp-clipboard-*` staging under the configured root. The clipboard offer is published only after range coverage is complete and file writes have been synchronized; this is a clipboard transaction, not a filesystem rename transaction. No remote executable is launched and no executable mode is propagated. File timestamps are represented in descriptors but are not restored onto received files.

## Lifetime and trust boundaries

Received data is materialized when a remote file offer is selected, not lazily on each later application paste. Complete files remain available to native applications during the RDP session. Paste/copy them into a durable destination before disconnecting. Cancelled transfers remove unpublished staging. Orderly session teardown removes recorded staging with the expected directory identity; original source files are preserved.

Forced termination, crashes or machine failure can leave private staging directories. Automatic cross-process crash recovery is not implemented. Inspect stale directories after confirming their sessions have ended. Do not indiscriminately delete active staging.

The configured root confines remote file selection and publication, but is not an OS sandbox. The private desktop runs as the same Unix account as LRDP, and local applications with that identity have that account's filesystem permissions. Authentication, rootless Xorg isolation and file redirection are separate controls. Laboratory mode must remain loopback-only.

## Protocol and allocation bounds

| Policy | Default/current bound |
| --- | --- |
| Descriptors per offer | 128, including directories |
| File tree depth | 16 components |
| Encoded relative name | 260 UTF-16 code units including terminator |
| Source offer bytes | 256 MiB; `--clipboard-max-mib` permits 1–1024 MiB |
| Received bytes retained per session | Same byte budget, cumulative across completed and active transfers |
| Active/completed receive transactions per session | 16 |
| Concurrent range/size requests | Four |
| Requested range size | Up to 64 KiB |
| Retained remote clipboard locks | Eight |
| Receive inactivity deadline | 30 seconds |

`FileGroupDescriptorW` is identified by its format name and mapped to the peer's registered numeric ID. Both long names and the legacy ASCII short-name format are handled. FileContents SIZE and RANGE requests use their stream identifiers and optional clipboard lock IDs. Locks retain the exact published file-source snapshot across subsequent clipboard offers. Unknown sizes are requested before issuing ranges. Out-of-order and legal short RANGE responses are handled without publishing holes; stale/cancelled stream completions cannot finish a new transfer.

The wire engine is independent of the native filesystem implementation through `ClipboardFileStore`, `ClipboardFileSource` and `ClipboardFileSink`. Native filesystem work is bounded synchronous I/O on the session thread; it is not an asynchronous or zero-copy disk pipeline. More than 4 GiB files, clipboard images/HTML, direct remote file-path access and Wayland portal file integration are not advertised or implemented.

## Cursor remoting

XFixes supplies the native cursor shape and hotspot. LRDP converts host `unsigned long` ARGB pixels into canonical premultiplied BGRA without retaining native pointers. RDP New Pointer updates carry 32-bit alpha when negotiated; Color Pointer updates provide a 24-bit fallback. Scanlines, masks and hotspots are encoded explicitly rather than through packed host structs.

A bounded 32-entry exact-match LRU emits Cached Pointer updates for known shapes, suppresses redundant selection messages, and supports hidden cursors. The current implementation does not advertise Large Pointer support. Shapes larger than 32 x 32 are scaled with their aspect ratio and hotspot preserved. The portal backend continues to use an embedded cursor when granted, rather than this XFixes path.

## Tests and public inputs

`clipboard_files` covers descriptor sizes and endian order, unsafe names, 10,000 seeded descriptor mutations, source locks, unknown sizes, out-of-order/short ranges, cancellation, deadlines and registered-format ID mapping. `clipboard_file_store` exercises real descriptors, symlink/FIFO rejection, outside-root paths, source mutation, private file modes, quota enforcement and unpublished/complete lifetime. `x11_desktop` verifies large text and file-URI incremental transfers, GNOME copy payloads and native cursor pixels.

`clipboard_file_session` uses an independent Python TLS client and an Xlib application that does not link LRDP. It verifies 180,003 exported bytes, 200,007 imported bytes delivered out of order and pasted by the application, Unicode names, empty files/directories, cursor hotspots, clipboard progress during graphics suppression, malicious-path rejection and teardown cleanup. These are strong project integration tests, not Windows file-manager interoperability certification.

Normative inputs:

- [MS-RDPECLIP File Descriptor, 2.2.5.2.3.1](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeclip/a765d784-2b39-4b88-9faa-88f8666f9c35)
- [File Contents Request, 2.2.5.3](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeclip/cbc851d3-4e68-45f4-9292-26872a9209f2)
- [General Capability Set](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeclip/7718c8c9-798d-4788-bb75-64afdc913869)
- [MS-RDPBCGR Color Pointer Update](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/71fad4fc-6ad4-4c7f-8103-a442bebaf7d2)
- [New Pointer Update](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/584e4438-574c-45f4-947f-b0edcd9ae32c)
- [Pointer Capability Set](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/925e2c05-c13f-44b1-aa20-23082051fef9)
- [RFC 8089: file URI scheme](https://www.rfc-editor.org/rfc/rfc8089.html)
