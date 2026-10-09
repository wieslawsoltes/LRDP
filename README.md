# LRDP

A specification-derived native C++20 Linux RDP server. The wire stack is original code based on Microsoft's public Open Specifications; system OpenSSL, GSSAPI, X11, PipeWire, GLib and FFmpeg provide platform services. No application injection or `LD_PRELOAD` is used.

**Experimental, not complete Windows RDP parity or a production security certification.** See [implementation status](docs/IMPLEMENTATION_STATUS.md), [security boundaries](docs/SECURITY.md) and [provenance](docs/CLEAN_ROOM.md).

## Implemented paths

| Area | Available implementation |
| --- | --- |
| Connection | TLS, CredSSP v5/v6 NLA through system GSSAPI, exact principal authorization, bounded per-connection child processes |
| Desktops | Existing X11 desktop, consent-based Wayland portal/PipeWire capture, private rootless Xorg/dummy desktop, diagnostic desktop |
| Resizing | Private desktop RandR resizing, initial monitor topology, negative monitor origins, reactivation without restarting the desktop application |
| Graphics | Dirty-tile bitmaps, acknowledged GFX surfaces, AVC420/AVC444/v2, VA-API/NVENC paths, software fallback and opt-in lossless ClearCodec/SolidFill |
| Cursor | Native XFixes shape/hotspot capture, negotiated large pointers, alpha/legacy updates, exact LRU cache and hidden cursor |
| Clipboard | Unicode text; opt-in HTML/images and confined file/directory copying on X11, headless and portal backends |
| Input | Keyboard/mouse, RDPEI touch with portal injection, bounded pen wire decoding; native pen/IME parity remains incomplete |
| Drives | Opt-in RDPDR remote files and private libfuse3 mounts, read-only by default; see [implementation status](docs/IMPLEMENTATION_STATUS.md) |
| Audio | Per-session PipeWire virtual speakers and microphone; PCM RDPSND and AUDIO_INPUT channels |

Hardware encoding retains CPU capture, color conversion and upload. A real GNOME/Plasma session, physical GPU and Windows client have not been verified. [Lossless graphics](docs/LOSSLESS_GRAPHICS.md) uses `--encoder lossless` and does not require FFmpeg; it compares exact pixels, batches solid rectangles and applies ClearCodec residual RLE only when smaller than raw BGRA.

## Build

The Ubuntu 24.04 CI configuration installs:

```sh
sudo apt-get install \
  cmake ninja-build g++ pkg-config libssl-dev libkrb5-dev \
  libx11-dev libxtst-dev libxfixes-dev libxrandr-dev \
  libglib2.0-dev libpipewire-0.3-dev \
  libavcodec-dev libavutil-dev libswscale-dev \
  python3 xvfb xserver-xorg-core xserver-xorg-video-dummy xterm \
  freerdp2-x11 gss-ntlmssp dbus-daemon pipewire-bin

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DLRDP_SANITIZE=ON -DLRDP_TEST_NTLM=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run the tests as an ordinary user: the private Xorg tests intentionally reject root. Optional integrations are enabled when their dependencies are present. `LRDP_TEST_NTLM=ON` makes the independent NLA test a requirement. The protocol core can be built separately with `-DLRDP_BUILD_SERVER=OFF`.

## Local headless laboratory

```sh
umask 077
openssl req -x509 -newkey rsa:3072 -nodes -days 7 \
  -subj '/CN=localhost' \
  -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' \
  -keyout private-key.pem -out certificate.pem

install -d -m 700 "$HOME/LRDP-Share"

./build/lrdpd \
  --lab-no-auth \
  --cert certificate.pem --key private-key.pem \
  --backend headless \
  --desktop-command /usr/bin/xterm \
  --clipboard-files "$HOME/LRDP-Share" \
  --clipboard-max-mib 256 \
  --encoder auto
```

This starts an actual private X server and terminal, not the diagnostic desktop. Use `--desktop-command` with an absolute executable and repeat `--desktop-arg` for its arguments to launch another installed application/session. Arguments are executed directly, not interpreted by a shell. Rootless Xorg/dummy must be installed.

**Laboratory mode is loopback-only and unauthenticated. Do not forward it to untrusted peers or send real credentials.** NLA setup is documented in [SECURITY.md](docs/SECURITY.md); an authenticated principal still runs as the server's Unix account, not an impersonated Unix login.

Use `--backend x11 --display "$DISPLAY"` to share an existing X11 desktop, or `--backend portal` from a compatible graphical user's session. Existing physical/portal desktops do not advertise virtual-output resizing. Append `--audio --microphone` to publish virtual audio devices; applications must be routed to them explicitly.

## File clipboard

File transfer is disabled by default. With `--clipboard-files DIRECTORY`, copying local files under that directory can publish their relative descriptors to the client. Files received from the client are downloaded into private staging beneath the same directory and offered to Linux applications as file URIs only after every byte has been received. Paste them to a durable location before ending the session.

The default policy allows 128 file/directory descriptors and 256 MiB, uses four concurrent 64 KiB reads, and aborts an inactive transfer after 30 seconds. Symlinks, devices, FIFOs, path traversal and Windows alternate streams are rejected. Source files are never deleted; cut offers are handled as copies. Received files are non-executable. Orderly session cleanup removes received staging, not original source files. See [FILE_CLIPBOARD.md](docs/FILE_CLIPBOARD.md) for lifetime, quotas and crash limitations.

## Evidence and remaining scope

[CI run 37927109397](https://github.com/wieslawsoltes/LRDP/actions/runs/37927109397), for revision `2d0daa1f8e44ce9ca6369fc10392b28c8b940aaa`, passed the sanitizer build, 43 of 44 registered suites, the Release build and the protocol-only configuration. The AVC420 independent-client test was explicitly skipped because the installed client lacks that codec; a skip is not a passing interoperability result. The lossless client test checks actual rendered pixels. AVC444/v2 are exercised through real TLS sessions and a separately linked native H.264 decoder; Windows AVC presentation and physical hardware are still unverified.

The native file-clipboard fixture uses real TLS, private Xorg and an independent Xlib application for byte-exact export/import, out-of-order chunks, clipboard paste, cursor hotspots, suppression, traversal rejection and cleanup. Further tests cover portal API/descriptor lifetimes, audio through private PipeWire devices, NLA, headless resizing, rich clipboard, RDPEI and redirected drives.

Remaining work includes printer/serial/USB/smart-card/camera redirection, zero-copy capture/GPU conversion, additional codecs/caches and adaptive video regions, RemoteApp, UDP/gateway transports, persistent reconnect/PAM session brokerage and complete native pen/IME support. Windows/mobile, physical-device, real-compositor, Kerberos-domain and WAN validation are not implied by project tests.
