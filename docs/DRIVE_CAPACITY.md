# Redirected drive capacity

Native FUSE statfs now calls the same bounded asynchronous RDPDR bridge used by
file I/O. File managers and statvfs consumers can query a redirected volume's
allocation geometry, total allocation units, and caller-available space without
inventing capacity from the local mount host's filesystem.

## Query policy

A query resolves the current device generation, opens the requested path with
FILE_READ_ATTRIBUTES (not write access), validates ordinary file metadata and
rejects observed reparse points, queries FileFsFullSizeInformation (class 7), and
closes the remote handle on success or failure. There is no stale capacity cache.

Only STATUS_NOT_SUPPORTED, STATUS_INVALID_INFO_CLASS or
STATUS_INVALID_DEVICE_REQUEST triggers one FileFsSizeInformation (class 3)
fallback. Access denial, disconnection, bad lengths, negative counts and overflow
are not hidden by fallback. Response validation also runs on the network-owned
protocol path before data is returned to the worker.

## Quotas and native representation

Both structures contain signed nonnegative 64-bit counts and unsigned 32-bit
sector geometry. Zero geometry and 64-bit byte-capacity overflow are rejected.
The full structure distinguishes caller-free units from actual physical free
units. Its total may already reflect a user quota, so actual free units can
legitimately exceed that total.

The portable record retains these fields exactly. The POSIX view conservatively
clamps bfree to min(actual, total) and bavail to min(caller, actual, total), avoiding
both inflated caller allowance and negative free-space displays. Native narrowing
is checked; the caller's statvfs structure is untouched when conversion fails.
Readonly combines LRDP's local mount policy with observed remote volume/device
read-only flags. Remote ACLs may still reject writes when all three permit them.

The synthetic mount root is a namespace, not a volume. It reports zero aggregate
blocks rather than double-counting exports which can alias the same client
volume. The protocol does not supply inode totals; those fields are unspecified
(zero). The name-length value is a conservative 255-byte ASCII ceiling, not a
claim that POSIX bytes and Windows UTF-16 code units are interchangeable.

## Evidence and boundaries

The capacity suite verifies all truncations, signed/zero/overflow cases, quota
normalization, native output atomicity, fallback policy, least-privilege requests,
handle cleanup, replacement generations and 20,000 seeded malformed records.
The independent Python RDPDR client and native Filesystem facade exchange real
class 7 and class 3 responses through the production TLS Session/Bridge in both
readonly and writable modes, while graphics are suppressed. The fixture includes
16 TiB capacity, caller quotas, and a full-size unsupported-class fallback.

The kernel FUSE/FreeRDP test is extended with statvfs assertions, but that extended
kernel test requires /dev/fuse, libfuse3 and an independent client. Local portable
and TLS evidence is not a claim that the extended kernel test or Windows clients
have been verified. Existing FUSE fsync/O_SYNC/atomic-append rejection remains:
capacity reporting does not add a stable-storage flush primitive or promise
write durability.

## Public specification provenance

- MS-RDPEFS 2.2.3.3.6, Query Volume Information Request:
  https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpefs/484e622d-0e2b-423c-8461-7de38878effb
- MS-FSCC 2.5.4, FileFsFullSizeInformation:
  https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/63768db7-9012-4209-8cca-00781e7322f5
- MS-FSCC 2.5.8, FileFsSizeInformation:
  https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/e13e068c-e3a7-4dd4-94fd-3892b492e6e7

## Volume metadata and native annotations

The same generation-bound handle can now query the remaining documented volume
classes: FileFsVolumeInformation (1), FileFsDeviceInformation (4), and
FileFsAttributeInformation (5). The network parser validates them before a
completion enters the native bridge, and the facade validates alternate Client
implementations too. The class-1 RDP record has a 17-byte header: unlike generic
MS-FSCC, its Reserved byte MUST NOT be present. Any nonzero Boolean denotes true.
The label permits one terminal UTF-16 NUL and retains spaces; embedded NULs,
unpaired surrogates, negative creation times and over-quota text are rejected.
Filesystem names are length-delimited nonempty Unicode, not type-dispatch keys.
Unknown flags remain opaque; incompatible compression flags and invalid component
limits are rejected. Each variable-length text field is limited to 4096 wire
bytes. Device type is restricted to the documented disk/CD-ROM values.

Native statfs reports ST_RDONLY when the local policy, remote volume attributes,
OR device characteristics require it. Missing optional metadata classes preserve
local policy; only explicit unsupported statuses mean absence. Access denial and
malformed successes remain errors and leave the caller's output unchanged.
This reports observed properties, not an atomic remote snapshot or a substitute
for the client's ACL/write checks. Metadata is not cached, so repeated queries
observe flag/label changes and do not cross device generations.

The FUSE mount exposes read-only, length-delimited UTF-8/ASCII annotations through
getxattr/listxattr. For example, Python can call
`os.getxattr(drive_path, 'user.lrdp.volume.label')`.

| Name | Representation |
| --- | --- |
| user.lrdp.volume.label | UTF-8 volume label; empty is a valid value |
| user.lrdp.volume.serial | Eight hexadecimal digits with a `0x` prefix |
| user.lrdp.volume.created-100ns | Unsigned decimal Windows epoch ticks |
| user.lrdp.volume.filesystem | Informational UTF-8 name, without inferred semantics |
| user.lrdp.volume.flags | Raw filesystem flags, eight hexadecimal digits |
| user.lrdp.volume.max-component-utf16 | Decimal peer component limit, not a POSIX byte count |
| user.lrdp.volume.device-type | Raw device type, eight hexadecimal digits |
| user.lrdp.volume.device-characteristics | Raw characteristics, eight hexadecimal digits |

Only supported annotations are listed. Unknown names or unsupported metadata
return ENODATA; two-phase size probes and ERANGE handling do not truncate data or
append unrequested NULs. List results use the native NUL-separated name format.
setxattr/removexattr fail explicitly; this is not arbitrary remote EA/ACL
redirection. No annotation is used as an authenticated identity, executable,
path, permission grant, or globally unique identifier. Values are never logged.
The synthetic namespace root has no annotations. The conservative statvfs name
budget remains separate from the explicitly labelled peer UTF-16 component limit.

New primary sources:
- MS-RDPEFS 2.2.3.3.6 (class-1 reserved-byte exception) and 2.2.3.4.6 (external optional padding).
- MS-FSCC 2.5.1: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/ebc7e6e5-4650-4e54-b17c-cf60f6fbeeaa
- MS-FSCC 2.5.9: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/bf691378-c34e-4a13-976e-404ea1a87738
- MS-FSCC 2.5.10: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/616b66d5-b335-4e1c-8f87-b4a55e8d3e4a
- MS-FSCC 2.1.8: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/8ce7b38c-d3cc-415d-ab39-944000ea77ff
