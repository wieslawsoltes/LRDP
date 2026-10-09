# LRDP

A specification-derived native C++20 Linux RDP server. The wire stack is original code based on Microsoft's public Open Specifications; system OpenSSL, GSSAPI, X11, PipeWire, GLib and FFmpeg provide platform services. No application injection or `LD_PRELOAD` is used.

**Experimental, not complete Windows RDP parity or a production security certification.** See [implementation status](docs/IMPLEMENTATION_STATUS.md), [security boundaries](docs/SECURITY.md) and [provenance](docs/CLEAN_ROOM.md).

For opt-in retained Xorg/application sessions, see [persistent headless reconnection](docs/PERSISTENT_SESSIONS.md). The broker keeps applications alive across RDP-worker loss; it does not provide PAM impersonation or host-restart recovery.

## Implemented paths

| Area | Available implementation |
| --- | --- |
| Connection | TLS, CredSSP v5/v6 NLA through system GSSAPI, exact principal authorization, bounded per-connection child processes |
| Desktops | Existing X11 desktop, consent-based Wayland portal/PipeWire capture, private rootless Xorg/dummy desktop, diagnostic desktop |
| Resizing | Private desktop RandR resizing, initial monitor topology, negative monitor origins, reactivation without restarting the desktop application |
| Graphics | Dirty-tile bitmaps, acknowledged RDP GFX surfaces, AVC420/AVC444/AVC444v2 encoding, VA-API/NVENC implementations and software fallback |
| Cursor | Native XFixes shape/hotspot capture, negotiated 32/96/384-pixel updates, bounded fragmentation and exact LRU cache, hidden cursor |
| Clipboard | Unicode text; opt-in HTML/images and confined file/directory copying for X11/headless and consented Wayland portal sessions |
| Audio | Per-session PipeWire virtual speakers and microphone; PCM RDPSND and AUDIO_INPUT channels |
| Client drives | Opt-in RDPDR redirection through owner-only FUSE mounts with bounded asynchronous I/O; readonly by default |
| Reconnection | Opt-in rootless broker retaining private Xorg/applications across worker loss; exact principal and ARC verifier required |

Hardware encoding currently retains CPU capture, color conversion and upload. The portal path has not been verified against a real GNOME/Plasma session. Cursor size depends on negotiated client capabilities. Persistence retains a private Xorg desktop, not an existing physical or Wayland session; the broker does not survive its own restart.

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

[CI run 37832552839](https://github.com/wieslawsoltes/LRDP/actions/runs/37832552839), for code revision `b3c4cea5023c7cd96fe787a8b6daf53161afb533`, passed the sanitizer build, 24 of 25 registered tests, the Release build and the protocol-only configuration. The remaining test was explicitly skipped because the installed independent client lacks AVC420 support; it is not a passing interoperability result.

The native file-clipboard fixture uses a real TLS session, private Xorg and an independent Xlib application. It checks byte-exact export/import, out-of-order chunks, clipboard paste, cursor hotspots, output suppression, traversal rejection and cleanup. Windows file-clipboard/client certification, physical GPU execution, zero-copy capture, remaining peripheral redirection, RemoteApp, UDP/gateway transports, broker/host-restart recovery, a PAM login broker and complete native pen/IME coverage remain unfinished. Feature-specific guides record newer tests and exact limitations.
