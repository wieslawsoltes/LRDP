# Redirected raw printer submission

LRDP can expose printers announced by an RDP client to programs running as the server's Unix account. This is an opt-in raw spool transport, not a document renderer, CUPS queue installer, Windows driver host, or physical printer-completion service. It is independent of drive write policy and works while graphics are suppressed.

## Enable and submit

Build the default native server with OpenSSL. `LRDP_PRINTING=OFF` removes the native endpoint and CLI without removing portable device-protocol tests.

Create an existing private directory as the desktop user:

```sh
install -d -m 700 "$HOME/LRDP-Print"
```

Append `--printers-directory "$HOME/LRDP-Print"` to the normal authenticated `lrdpd` command. No root, elevated helper, default-printer change, or global queue installation is needed. Laboratory mode stays loopback-only and unauthenticated; do not expose it to untrusted peers.

After connection, the server prints an endpoint such as:

```text
Printers endpoint: /home/user/LRDP-Print/print-0123456789abcdef01234567/print.sock
```

Use that exact endpoint and select the current device generation from the list:

```sh
lrdp-print list /home/user/LRDP-Print/print-0123456789abcdef01234567/print.sock
# Example: 9:1<TAB>Printer name<TAB>Driver hint
lrdp-print submit /home/user/LRDP-Print/print-0123456789abcdef01234567/print.sock 9:1 /path/to/printer-ready.prn
```

Input must already be in a page-description language accepted by the selected client printer and its driver. A PDF, XPS, image, or text document is **not automatically converted**. The advertised driver name is informational and never loaded or executed by LRDP. Native source files are regular, nonempty files no larger than 64 MiB; terminal symlinks, FIFOs, devices, directories, and NUL-containing names are rejected.

The CLI snapshots the source into a sealed memory file in its own process before submission. Observed source size/mtime/ctime changes abort that snapshot; this is not an atomic snapshot of a concurrently modified filesystem. Once sealed, submitted bytes cannot be written, resized, or have their seals removed. The server receives a descriptor rather than a client-selected filesystem path and reads bounded chunks from it.

## Completion and uncertainty

The engine uses the specified printer CREATE, WRITE, and CLOSE operations. It handles legal short-write acknowledgements by advancing only the acknowledged byte count. Successful submission requires the final remote CLOSE acknowledgement. Success means the remote spool was closed, **not that physical pages were printed**.

Zero-progress writes, device removal, stale generations, and remote failures never produce success. Disconnecting the submitting CLI requests cancellation; an already issued CREATE is closed after its reply, and further writes stop once cancellation is observed. Bytes already sent may have printed, and a previously queued packet cannot be recalled. Cancellation is not rollback.

Timeouts or broken transport can leave the result uncertain. LRDP never automatically retries an entire print job. The CLI reports acknowledged bytes when known and warns against automatic retries. Jobs, sources, and native endpoints are connection-scoped even when the private desktop uses a persistence broker. Resuming an RDP connection does not resume or duplicate print jobs.

## Bounds and native isolation

| Resource | Bound |
| --- | --- |
| Announced printers | 16, within the overall device quota |
| Resident jobs | 4 per RDP connection |
| Aggregate immutable source size | 64 MiB per connection |
| Printer payload per WRITE | At most 64 KiB and no larger than shared drive-transfer policy |
| Jobs active on one printer | 1; other printers can progress independently |
| Native control peers | 16 |
| Native packet | 32 KiB |
| Native command/response deadline | 5 seconds |
| Job deadline | 5 minutes; outstanding device-I/O deadlines can terminate earlier |
| CLI submission wait | 330 seconds |

The endpoint is a nonblocking Unix `SOCK_SEQPACKET` socket in a random 0700 child directory, with mode 0600. Both ends verify `SO_PEERCRED`. The root and socket path are resolved through pinned directory descriptors without following symlinks. Ancillary descriptors are acquired with close-on-exec and owned before malformed/truncated messages are rejected. The endpoint accepts exactly one fully sealed descriptor for a job and no descriptors for enumeration. Reads and writes have bounded event-loop work; copying the original disk file happens in the CLI, not the RDP loop. Sealed-memory reads may still incur kernel paging latency; this is not a hard real-time or zero-copy transport.

Printer and filesystem operations have separate trusted local dispatch domains. They share bounded wire completion IDs, but printer opt-in never turns a readonly drive into a writable drive. Filesystem requests cannot access printer devices, and printer requests cannot address files. Device generations prevent old native handles or completions from authorizing replacement devices.

Orderly shutdown immediately releases held source descriptors and removes only the endpoint's recorded socket/directory inodes. A killed process can leave a private empty directory or stale socket name; there is no automatic unlink of arbitrary prior endpoints. Same-UID local processes remain trusted. The endpoint is not a security boundary between programs belonging to the same Unix account.

## Tests and evidence boundaries

Portable protocol tests cover metadata, purpose isolation, generation changes, bounded queues, and 20,000 malformed printer metadata inputs. Native tests use actual sealed memory files and Unix descriptor passing, verify snapshot immutability, short writes, rejected source types and stale generations, and require descriptor counts to return to baseline even before endpoint destruction.

The integration fixture runs the real TLS server and `lrdp-print` against a separately encoded Python RDP peer. It verifies Unicode enumeration, a byte-exact 180,003-byte job, legal short acknowledgements, cancellation during CREATE, no-progress WRITE, failed CLOSE, extra/unsealed/truncated descriptor messages, graphics suppression, and cleanup. It does not use CUPS, a Windows client, or printer hardware; none of those interoperability results are claimed.

Not implemented: automatic CUPS queues/filters, PDF/XPS rendering, printer-cache updates, printer configuration events, device-control extensions, physical job status, durable/restartable job storage, or Windows/physical-printer validation.

## Public provenance

- Microsoft MS-RDPEPC 2.2.2.1 and 2.2.2.7–12: printer metadata and CREATE/WRITE/CLOSE request/response layouts; shared MS-RDPEFS framing and capabilities.
  https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpepc/1a9a2bdd-a5ae-4d41-befc-264038950b26
- Linux `unix(7)` and `recvmsg(2)`: `SO_PEERCRED`, `SCM_RIGHTS`, `MSG_CMSG_CLOEXEC`, truncation and descriptor ownership.
  https://man7.org/linux/man-pages/man7/unix.7.html
- Linux `memfd_create(2)` and `F_GET_SEALS(2const)`: immutable source seals.
  https://man7.org/linux/man-pages/man2/memfd_create.2.html
  https://man7.org/linux/man-pages/man2/F_GET_SEALS.2const.html

The wire code is original and specification-derived; no other RDP implementation source is imported.
