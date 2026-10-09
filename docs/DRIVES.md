# Redirected client drives

LRDP can mount drives exported by an RDP client into the Linux server user's filesystem. This is opt-in MS-RDPEFS drive redirection over the `rdpdr` static virtual channel. It is separate from clipboard file copying. Printers, USB, serial ports, smart cards and cameras are not implemented by this component.

## Enable

Build with libfuse3 development files (Ubuntu: `libfuse3-dev fuse3`). `LRDP_FUSE=ON` is the default; the mount adapter is built only when libfuse3 >= 3.10 is found. Kernel `/dev/fuse` access and the `fusermount3` helper are required at runtime. Run as an ordinary user, never root.

Create an existing private directory with no group/other permission bits:

```sh
install -d -m 700 "$HOME/LRDP-Drives"
```

Append this to the normal authenticated server configuration:

```sh
--drives-directory "$HOME/LRDP-Drives"
```

The default mount is read-only. Append `--drives-writable` only when mutations of the client's exported files are intended. The client must also explicitly export a drive. For example, a FreeRDP client can use `/drive:DATA,/absolute/client/directory`.

A session creates a random private mount directory and prints `Drives mounted: ...`. Drive children have names like `drive-1-1-DATA`, containing the peer's device ID and a server-owned generation. Device replacement changes that generation so stale paths and handles cannot address a new drive. A mount belongs to the server's Unix account. This is not a multi-user login broker or an isolated filesystem sandbox.

## Implemented operations

The native adapter supports directory enumeration, file/directory metadata, read/EOF, create/open/close, partial writes, truncate, rename, mkdir, unlink and rmdir. Unicode names and 64-bit offsets are preserved within documented limits. Cross-drive rename is rejected. Native status errors are translated from NTSTATUS. Reparse points are not exposed or followed at the final component; the client remains responsible for confining its own exported namespace and intermediate components.

FUSE uses `default_permissions,nodev,nosuid,noexec`, never `allow_other`. Files are presented without executable bits. The private mount root must be owned by the current user with mode 0700 or stricter. No application is launched in response to a peer filesystem message.

The virtual-channel protocol and request scheduling run on the RDP event loop. A separate single FUSE worker performs blocking native operations through the bounded bridge, so a slow redirected filesystem does not block keyboard, clipboard, graphics or audio protocol processing. Completion IDs and remote file IDs are distinct from native handles. Device removal quarantines old completions; IDs are not reused before the old reply is retired.

## Bounds and semantics

- Default protocol limits: 64 devices, 1,024 handles, 64 outstanding requests, 64 KiB transfers and a 30-second request deadline. Native reads/writes are capped at 1 MiB per adapter operation and split into bounded protocol requests.
- One open-directory snapshot is limited to 4,096 entries and 1 MiB; total retained FUSE directory snapshots are capped at 16 MiB. Reopen a directory to refresh its snapshot.
- Kernel content and metadata caching are disabled. The client owns actual file contents and may change them concurrently.
- Peer writes are acknowledged before native completion, but this does not promise stable-storage durability. `fsync`, `O_SYNC`, `O_DSYNC` and atomic `O_APPEND` are explicitly unsupported rather than silently emulated.
- Locks, ACLs, arbitrary remote extended attributes, hardlinks, sparse-file control, symlinks, change notifications and server-side printer/device redirection are not implemented here. Read-only volume annotations and capacity reporting are described below.
- Timeout or native mount failure terminates the session to retire potentially live remote handles. Orderly teardown disconnects waiters, unmounts and removes the private mount directory. A killed/crashed process may require `fusermount3 -u` or administrator recovery; no recursive deletion of client files occurs.
- Remote paths reject parent traversal, embedded NUL, alternate data streams, UNC prefixes and ambiguous separator/components. The informational client computer-name field alone accepts extra trailing UTF-16 NULs for observed client interoperability. Authenticated names and file paths retain strict parsing.

## Evidence

Code revision `210aa5f50eb90f989ec908993988074f1103bf61` passed [CI run 37919963494](https://github.com/wieslawsoltes/LRDP/actions/runs/37919963494). The archived JUnit report contains **37 tests: 36 passed, one explicitly skipped, zero failed**. Debug ASan/UBSan, optimized Release and protocol-only builds succeeded. The skip is independent AVC420 client presentation, unrelated to drives, because the installed client lacks that codec.

`drive_fuse_client` exercised the production TLS server, an independent FreeRDP executable, actual kernel FUSE mounts and native file I/O. Both read-only and writable scenarios passed: 180,003-byte Unicode-named reads, 64-bit EOF, a 200,007-byte create/write/readback, truncate, rename, mkdir/rmdir, deletion, read-only enforcement, non-executable metadata, rejection of unsupported append/fsync semantics and mount cleanup.

Protocol/bridge/session fixtures also validate out-of-order completions, partial writes, disconnected waiters, generation invalidation, device removal, bounded queues and malformed payloads. The local environment lacks kernel FUSE and FreeRDP, so native mount evidence comes from CI, not a local hardware claim. **Windows client drive interoperability has not been verified.**

## Public specification provenance

- [MS-RDPEFS](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpefs/34d9de58-b2b5-40b6-b970-f82d4603bdb5), sections 2.2.1-2.2.3.
- [Client Name Request](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpefs/902497f1-3b1c-4aee-95f8-1668f9b7b7d2), section 2.2.2.4.

Wire code is original and specification-derived. The independent client is used as a black-box interoperability test; no other RDP implementation source is copied or imported.

### Capacity and descriptive volume metadata

The redirected volume's `statvfs` now queries actual client allocation geometry
and quota-aware availability, with explicit unsupported-class fallback. The C++ facade
combines remote volume/device read-only flags with local mount policy. Linux FUSE
ignores the callback f_flag, so OS-level flags still describe the mount rather
than per-drive remounting; remote flags remain available in the annotations. The
synthetic mount root does not sum potentially aliased client volumes.
Read-only `user.lrdp.volume.*` xattrs expose labels, filesystem names, serials and
capabilities; these are generated annotations, not arbitrary EA/ACL redirection.
See [DRIVE_CAPACITY.md](DRIVE_CAPACITY.md) for formats, error policy and limits.
