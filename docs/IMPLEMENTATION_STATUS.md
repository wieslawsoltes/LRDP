# LRDP implementation status — October 9, 2026

LRDP is an experimental, specification-derived native C++20 Linux RDP server. **Merged implementation does not mean complete Microsoft Windows RDP parity or production security certification.** The wire code is original and based on public specifications; system OpenSSL, GSSAPI, X11, GLib/GIO, PipeWire, FFmpeg, libpng and libfuse3 provide optional platform services. No application injection or `LD_PRELOAD` is used. See [provenance](CLEAN_ROOM.md) and [security boundaries](SECURITY.md).

This page supersedes the October 8 status snapshot, which predated rich/Wayland clipboard, large pointers, drives, AVC444, persistence and network measurements. Capability negotiation must continue to advertise only implemented paths supported by the selected backend and policy.

## Implemented paths and evidence boundaries

| Area | Implementation | Important boundary |
| --- | --- | --- |
| Connection/security | TLS, original GCC/MCS/session state machine, CredSSP v5/v6 through system GSSAPI, exact principal authorization, bounded child processes | Kerberos is permitted by default; NTLMv2 is explicit opt-in. This is not PAM impersonation or a security audit. |
| Native desktops | Existing X11 sharing, consent-scoped Wayland portal/PipeWire capture, private rootless Xorg/dummy desktop | Portal tests use a private D-Bus fixture; no real GNOME/Plasma compositor session is verified. |
| Display control | Initial monitor topology, negative origins, transactional RandR resizing and reactivation on private headless desktops | Physical X11 and portal backends do not create arbitrary client-sized virtual outputs. |
| Graphics | Dirty-tile bitmaps, acknowledged GFX surfaces, AVC420/AVC444/v2 and opt-in lossless ClearCodec residual/SolidFill | VA-API/NVENC paths compile; physical GPU execution is unverified. CPU capture/conversion/upload remain. No zero-copy DMA-BUF path. |
| Cursor | XFixes shape/hotspot capture, negotiated 96/384-pixel cursors, alpha/legacy updates, cache and fast-path fragmentation | Windows/mobile cursor interoperability and physical hardware are unverified. |
| Clipboard | Unicode text; opt-in HTML/images and confined file/directory transfer on X11, headless and consented portal backends | HTML is transported, not sanitized/rendered. Windows/Office/file-manager interoperability is unverified. |
| Input | Keyboard/mouse, RDPEI wire decoding and permission-gated native portal touch | Pen fields are decoded, but no production pen injector is advertised. Complete pen/IME/keyboard-layout parity remains missing. |
| Audio | Per-session PipeWire virtual speakers and microphone, PCM RDPSND and AUDIO_INPUT | Applications must be explicitly routed to endpoints. No physical microphone capture, default-device changes or compressed/UDP audio claim. |
| Redirected drives | Readonly-by-default RDPDR/FUSE operations, bounded asynchronous bridge, capacity/statfs and volume annotations | No stable-storage fsync/atomic append, arbitrary remote ACLs/xattrs or change-notification support. |
| Printers | Opt-in native raw printer submission, immutable sources, bounded jobs, short-write handling and CLOSE-acknowledged completion | No automatic document conversion, CUPS queues/drivers or physical-page status. |
| Reconnection | Opt-in per-account headless broker, principal-bound ARC proof, exclusive leases, cookie rotation and desktop retention across worker loss | Broker/host restart, Unix identity isolation, persistent redirected jobs and PAM login brokerage are not implemented. |
| Network diagnostics | Opt-in continuous RTT and passive peer-receive throughput over a dedicated negotiated MCS channel | Not UDP/gateway transport or available-link-capacity estimation. Metrics do not tune codecs or release GFX credits. |
| Deployment | Compiled-capability JSON, shared-parser nonbinding preflight, noninteractive TLS credential loading | Preflight does not contact a KDC/broker/compositor, bind a listener, validate certificate trust/hostname or certify hardware. |

## Feature documentation

[Large pointers](LARGE_POINTERS.md), [rich clipboard](RICH_CLIPBOARD.md), [file clipboard](FILE_CLIPBOARD.md), [extended input](EXTENDED_INPUT.md), [redirected drives](DRIVES.md), [volume interoperability](DRIVE_VOLUME_INTEROP.md), [lossless graphics](LOSSLESS_GRAPHICS.md), [persistent desktops](PERSISTENT_SESSIONS.md), [network metrics](NETWORK_METRICS.md), [printing](PRINTING.md), and [deployment preflight](DEPLOYMENT_PREFLIGHT.md) document public sources, exact limits and feature-specific failure semantics. The [README](../README.md) provides build/setup examples.

## Integrated PR completion

PRs #11–#14 are now merged on top of the earlier graphics, clipboard, touch and drive work:

- **#13:** quota-aware capacity and volume metadata; independent-client filesystem-name padding interoperability fixed without relaxing paths or authenticated identities.
- **#12:** persistent private desktop reconnection integrated with lossless graphics and volume metadata while preserving deferred NLA/ARC attachment.
- **#14:** network measurements integrated with persistence; complete-packet TLS egress timestamps and diagnostic-versus-user idle classification preserved.
- **#11:** printer protocol connected to the actual production Session, native owner-only endpoint, sealed descriptor submission and `lrdp-print` CLI.

PR #15 adds deployment preflight and removes redundant native rich-clipboard UTF-16 allocation exposed during full CI. The existing strict UTF-8 validator remains authoritative; the five-second transfer deadline and malformed-input checks are unchanged. A deterministic native regression fails on the old adapter and passes after the fix. This does not guarantee immunity to all scheduler-induced timeouts.

## Recorded validation

The combined printer/integration revision `1b8ca66e3dbb5f362de2c0d172f5059ad80e37db`, source tree `31e671a86e556fbb0fe7972370a64f29c5781645`, passed [CI run 37991632005](https://github.com/wieslawsoltes/LRDP/actions/runs/37991632005). Downloaded JUnit records **58 suites: 57 passed, one explicitly skipped, zero failed**. Sanitized Debug, optimized Release and dependency-minimal build/test stages succeeded.

Deployment and clipboard-fix revision `03f971ca5dbfdcd00b8dfc12fc94477670a27fa8` has [its own exact-revision CI](https://github.com/wieslawsoltes/LRDP/actions/runs/37996714892); consult that run's final result rather than extrapolating the earlier result. Local ordinary-user validation at this code revision passed **45 available ASan/UBSan suites**, **45 native Release suites**, and **34 optional-integration-disabled Release suites**. The unchanged rich-clipboard TLS fixture also passed eight consecutive local sanitizer runs. Tests missing local optional dependencies are not counted as passes.

The independently installed FreeRDP client used in CI lacks AVC420 support, so `client_avc420` is explicitly skipped. That is not an interoperability pass. Separate software H.264 encoder/decoder and independent paired-AVC TLS pixel-oracle fixtures pass; stock Windows AVC presentation and physical encoders remain unverified.

Evidence categories remain distinct:

1. Portable tests validate original wire/state/codec logic, bounds, malformed input, generations, timeouts and exact pixel/byte transformations.
2. Independent Python TLS peers exercise the real server, including clipboard, graphics, device I/O, networking and reconnection. The raw-print fixture transfers exactly 180,003 bytes through the real CLI and checks partial writes, cancellation, stale generations, descriptor rejection and cleanup.
3. Native fixtures use real Xorg/Xvfb, independent Xlib applications, private PipeWire audio graphs and Unix descriptor passing. Portal D-Bus tests validate API/lifetime/permission behavior but are not physical-compositor evidence.
4. Independent FreeRDP/system-GSS and kernel-FUSE tests validate selected authentication, rendering, network and drive paths in CI. They do not establish universal Windows compatibility.

CI archives the exact source revision, JUnit output, test log and Release server executable. A successful run applies to its recorded revision; subsequent source changes require their own checks. One earlier drive CI attempt had an isolated Xvfb startup failure and passed after retry without code/test weakening. The first deployment CI exposed the native rich-clipboard delay, which was corrected rather than hidden by increasing its timeout.

## Operator safety and lifetime

Run native private desktops and endpoints as an ordinary Unix user. Each authenticated principal is still operating under that server account's privileges. Same-UID local processes remain trusted. NLA requires fresh authorization on new authenticated RDP connections; client-reported names are never substitutes for that identity.

Laboratory mode is explicitly unauthenticated, restricted to loopback, and must not be forwarded to untrusted peers. Use administrator-provisioned TLS/GSS credentials for authenticated sharing. `--check-config` validates local configuration without starting a session and explicitly lists checks deferred until runtime.

File receive staging, printer jobs, drive handles, graphics decoder references and audio endpoints are connection-scoped. The persistence broker retains private Xorg/application processes, not an entire redirected-session snapshot. Forced termination can leave private stale staging/socket names. Raw printer submission may have printed bytes before failure; there is no rollback or automatic whole-job retry.

## Remaining product requirements

The major unimplemented areas are RemoteApp/RAIL; UDP/multitransport and gateway transport; zero-copy DMA-BUF and GPU-side conversion; additional graphics codecs/caches and adaptive regions; complete native pen/IME/gesture support; serial/parallel, USB, smart-card and camera redirection; automatic CUPS/driver/document-rendering integration; PAM/Unix identity brokerage; persistent broker/host-restart recovery; and expanded filesystem durability/locking/notification semantics.

Real Windows/mobile clients, Windows file managers/Office, Kerberos-domain deployment, actual GNOME/Plasma sessions, physical GPUs/printers/audio/digitizers, long-duration reliability, WAN impairment and production security review remain validation requirements. Passing all currently registered tests or having no open PRs must not be reported as full product or Windows feature parity.
