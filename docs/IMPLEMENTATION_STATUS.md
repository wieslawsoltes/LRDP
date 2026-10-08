# LRDP implementation status — October 8, 2026

LRDP is an experimental specification-derived native Linux RDP server. Windows feature parity is the goal, not the current implementation status. No proprietary or other RDP implementation source is incorporated in the wire stack. System OpenSSL, GSSAPI, X11, GLib/GIO, PipeWire and FFmpeg remain legitimate dependencies. This is a provenance description, not a legal clean-room certification.

## Verified revisions

Code revision `b3c4cea5023c7cd96fe787a8b6daf53161afb533` passed [GitHub Actions run 37832552839](https://github.com/wieslawsoltes/LRDP/actions/runs/37832552839), job `113501303408`. Its archived JUnit report contains 25 registered suites: 24 passed and one was explicitly skipped. Debug ASan/UBSan, optimized Release and protocol-only builds succeeded. All eight protocol-only suites passed. The matching local sanitizer build passed all 17 suites available with the locally installed dependencies, including private Xorg and native file paste.

The skipped suite is `client_avc420`: the installed Ubuntu FreeRDP executable lacks AVC420 support. A skip is not a passing interoperability result. The native FFmpeg software encoder/decoder and color oracle passed separately. Windows file-clipboard interoperability and physical GPU execution remain unverified.

Earlier baseline revision `c96bce01757a91dc0d97b55a9005145dcf5abe5e` passed [run 37810505403](https://github.com/wieslawsoltes/LRDP/actions/runs/37810505403) with 16 passing suites and one codec-client skip. PR #1 has been merged; subsequent headless, monitor, cursor and file work is tracked in PR #2.

Evidence applies to the recorded code revision, not automatically to later changes. CI archives the exact source revision, JUnit results, test log and Release executable.

## Implemented in this development iteration

### Authentication and process isolation

CredSSP v5/v6 NLA uses system GSSAPI, nonce-bound TLS public-key hashes, message protection, strict DER parsing, state sequencing and exact authenticated-principal authorization. Kerberos is permitted by default. System GSS-NTLMSSP is an explicit `--allow-ntlm` opt-in; LRDP requires NTLMv2 session security and verifies protected-message sequence numbers in addition to system GSS signature verification. Cryptography is not reimplemented in LRDP.

The independent FreeRDP/system-GSS test verifies a rendered desktop after authorized authentication, rejection of a wrong password, and rejection of a valid but unauthorized principal. Principal case and domain/realm are preserved. A terminal C NUL supplied by the system GSS-NTLMSSP display-name buffer is excluded; embedded NULs and control characters remain invalid.

Each ordinary connection runs in its own child process, created before native resources and worker threads. The listener bounds concurrent sessions and owns child shutdown. Desktop resources are not opened before successful authentication in NLA mode.

This is authenticated sharing of the server account's desktop, not a PAM login broker. No Unix impersonation or PAM login-session creation is implemented. The headless backend creates a separate X server/application process tree under the existing server account. Actual Kerberos-domain and Windows-client interoperability are not yet recorded.

### Wayland and clipboard

A native RemoteDesktop/ScreenCast portal path creates a consent-scoped session, requests one monitor plus keyboard/pointer control, opens the granted PipeWire descriptor, captures mapped raw video and injects input through the portal. The implementation requests an embedded cursor when available and processes permission revocation even when RDP graphics are suppressed.

Clipboard redirection is enabled only when the portal grants Clipboard access. File descriptors, transfer sizes, deadlines, generation changes, exact session paths and unique D-Bus sender identity are checked. Incoming and outgoing Unicode text transfers are bounded; the implementation does not advertise clipboard files, HTML or images.

The private D-Bus portal test exercises actual public API calls and Unix descriptor passing, early response ordering, input methods, bidirectional 180 KiB text, permission denial, revocation and cleanup. Buffer-layout tests validate pitch, crop, color conversion and allocation bounds. Native PipeWire capture compiles, but no real GNOME/Plasma compositor consent/capture session has been verified.

Existing physical X11 and portal backends do not create virtual monitors. The private headless backend now applies DisplayControl resizing to real Xorg outputs, as described below.

### Private desktops, initial topology and native cursor

`--backend headless` starts rootless Xorg with the dummy driver, a random private Xauthority cookie, no TCP listener, a dedicated runtime directory, and an explicitly selected application/session command. Direct argument-vector execution, close-on-exec descriptors, process-group ownership and startup/shutdown deadlines bound the subprocess lifecycle. The native desktop shares the server's Unix identity and filesystem privileges: it is not a container or login broker.

RandR changes are validated and applied transactionally, with rollback on failure and retirement of old custom modes. Initial client monitor topology and attributes are parsed; active layouts are sent during activation. Tests cover negative monitor origins, framebuffer dimensions, 20 mode changes, rejection of unauthenticated X clients and the same application surviving three RDP resize/reactivation cycles with working input and clipboard.

Native XFixes cursor shapes now reach the client through negotiated alpha/legacy pointer updates and a bounded exact LRU. Hidden cursors, masks, scanline order and hotspots have pixel-level tests. Large Pointer is not advertised; native shapes exceeding 32 x 32 are scaled.

### Opt-in file clipboard

`--clipboard-files DIRECTORY` enables file and directory transfer on X11/headless backends. A confined native store pins selected source inodes, rejects special files/symlinks and observed source mutation, and writes received data to private non-executable staging. Native `text/uri-list` and GNOME copied-files selections bridge file-manager copy/paste to FileGroupDescriptorW and FileContents messages.

The protocol engine supports format-ID mapping, locks, unknown sizes, bounded parallel ranges, short/out-of-order responses, cancellation and inactivity deadlines. The real TLS/private-Xorg integration test exports 180,003 bytes and imports 200,007 bytes that an independent native application pastes and checks. It also verifies Unicode filenames, empty files/directories, graphics suppression, traversal rejection, cursor hotspots and staging cleanup. Windows-client file interoperability has not been recorded.

Received staging is session-scoped and removed on orderly cleanup. Forced termination may leave private staging. Defaults are 128 descriptors, 256 MiB retained receive data, four 64 KiB reads and a 30-second inactivity timeout; the native store additionally caps retained receive transactions at 16. See [file clipboard details](FILE_CLIPBOARD.md).

### Native audio

`--audio` publishes a per-session virtual PipeWire sink, LRDP Remote Speakers. Linux applications deliberately routed to it are sent to the client through RDPSND. `--microphone` publishes LRDP Remote Microphone, a virtual source supplied by the client's AUDIO_INPUT channel. The server does not capture the host's physical microphone, change defaults or automatically reroute unrelated applications.

Playback supports PCM 48 kHz stereo signed 16-bit little-endian samples and RDPSND v2/v6/v8 framing, quality negotiation, training, Wave/Wave2 and bounded rolling-block acknowledgements. Microphone input supports PCM 48 kHz mono, 960 frames per packet, format/Open sequencing, DVC fragmentation, exact packet bounds and a real-time rate budget.

PipeWire callbacks do not allocate, block or throw. Lock-free single-producer/single-consumer rings preserve cursor ownership, drop new whole sample frames on overflow and bound playout latency. Microphone underrun produces initialized silence. Media has a bounded priority queue but can preempt graphics only between complete packets; active OpenSSL write buffers remain stable across partial writes and retry conditions.

Two native tests passed: production virtual microphone-to-speaker sample transfer through an isolated real PipeWire graph; and a full production RDP server roundtrip from certificate-verified TLS microphone packets, through PipeWire, back to RDPSND Wave2 samples. The latter requires audio to continue after graphics suppression. Physical audio hardware and independently implemented RDP microphone clients remain unverified.

## Existing graphics and X11 paths

Original GCC/MCS, activation, fast/slow keyboard/pointer input, static and dynamic channels, Unicode clipboard, dirty-tile bitmaps and RDP 8.1 GFX surfaces remain available. Independent FreeRDP tests inspect actual rendered bitmap and raw-GFX pixels. Xvfb tests exercise native X11 capture, tracked input release and large ICCCM clipboard transfers.

The FFmpeg software H.264 encoder/decoder path passes its Annex-B, forced-keyframe, edge-padding and BT.709 full-range pixel oracle. VA-API and NVENC encoder paths compile and have explicit fallback/reporting, but no physical GPU test or benchmark has been recorded. CPU capture, conversion and upload remain in the hardware path: it is not zero-copy DMA-BUF rendering.

## Build and test

Ubuntu/Debian development dependencies:

```sh
sudo apt-get install \
  cmake ninja-build g++ pkg-config libssl-dev libkrb5-dev \
  libx11-dev libxtst-dev libxfixes-dev libxrandr-dev \
  libglib2.0-dev libpipewire-0.3-dev \
  libavcodec-dev libavutil-dev libswscale-dev \
  python3 xvfb xserver-xorg-core xserver-xorg-video-dummy xterm \
  freerdp2-x11 gss-ntlmssp dbus-daemon pipewire-bin

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLRDP_SANITIZE=ON \
  -DLRDP_TEST_NTLM=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`LRDP_TEST_NTLM=ON` explicitly requires the independent NLA test rather than silently omitting it because a dynamically loaded authentication provider is not found by CMake. Its randomized credentials live only in a temporary mode-0600 file. No production accounts, PAM or keytabs are modified.

For a dependency-minimal core build:

```sh
cmake -S . -B build-core -G Ninja -DLRDP_BUILD_SERVER=OFF
cmake --build build-core --parallel
ctest --test-dir build-core --output-on-failure
```

Run native headless tests as a non-root user. Optional native integrations are controlled by `LRDP_HEADLESS`, `LRDP_X11`, `LRDP_PORTAL`, `LRDP_AUDIO`, `LRDP_GSSAPI` and `LRDP_FFMPEG`.

## Local laboratory

```sh
umask 077
openssl req -x509 -newkey rsa:3072 -nodes -days 7 \
  -subj '/CN=localhost' \
  -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' \
  -keyout private-key.pem -out certificate.pem

./build/lrdpd --lab-no-auth \
  --cert certificate.pem --key private-key.pem \
  --backend demo --encoder raw
```

`--lab-no-auth` permits only 127.0.0.1 or ::1 and is mutually exclusive with NLA. TLS encrypts this profile but does not authenticate users. Do not send real credentials or forward this listener to untrusted peers. The demo desktop is a protocol diagnostic, not a Linux login session.

Use `--backend x11 --display "$DISPLAY"` for an authorized existing X11 desktop, or `--backend portal` within a compatible graphical user's session. Append `--audio --microphone` to request the virtual audio endpoints. Route applications explicitly through desktop sound settings; no automatic host routing occurs.

## Authenticated sharing

Provision a dedicated GSS acceptor identity and certificate through the administrator. Paths, realm and service below are deployment-specific placeholders:

```sh
export KRB5_KTNAME=/secure/path/to/lrdp-service.keytab

./build/lrdpd \
  --auth nla \
  --service TERMSRV@desktop.example.org \
  --allow-principal alice@EXAMPLE.ORG \
  --cert /secure/path/to/desktop-chain.pem \
  --key /secure/path/to/desktop-key.pem \
  --listen 127.0.0.1 \
  --backend portal \
  --audio --microphone \
  --encoder auto
```

Run as the desktop's Unix user, not root. `--allow-principal` is repeatable and exact. `--allow-ntlm` additionally permits the separately configured system NTLMv2 provider. The example deliberately retains loopback while setting up authentication. `--max-sessions` bounds child processes; `--once` serves one connection for controlled tests.

`--encoder auto` tries VA-API, NVENC and software H.264 if AVC420 is negotiated; `software`, `vaapi`, `nvenc` and `raw` select explicit policies. `--device` selects a VA-API render node. Logs distinguish hardware encoding with CPU upload from software encoding. Hardware initialization failure does not become a false hardware-success report.

## Remaining scope — not implemented

- PAM/Unix user-session broker, native Wayland virtual/headless compositor outputs, full compositor-backed multimonitor management and backend/hardware conformance.
- Wayland portal file clipboard, clipboard images/HTML, Windows file-client validation; RDPDR drives, printers, serial/parallel, USB, smart cards and cameras.
- AVC444, progressive/RemoteFX codecs, advanced caches and zero-copy DMA-BUF/GPU conversion.
- RemoteApp/RAIL, UDP multitransport, gateway transport, network autodetection, persistent reconnection and session brokerage.
- Full touch/pen/gesture, keyboard-layout/IME parity and compressed audio/UDP-audio profiles.
- Real Windows/mobile client, Kerberos-domain, GNOME/Plasma, GPU, physical audio and long-duration/WAN conformance evidence.

These remain open requirements. Unit tests, project socket fixtures, independent clients and native/hardware tests must continue to be reported separately. No production security audit or full Microsoft Windows RDP compatibility certification is claimed.
