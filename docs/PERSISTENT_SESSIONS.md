# Persistent private desktops

`lrdp-sessiond` keeps a private rootless Xorg/dummy desktop and its configured application alive after an RDP connection ends. A new `lrdpd` connection can attach to that desktop using the RDP Auto-Reconnect Cookie (ARC), subject to fresh NLA authentication and exact principal authorization in authenticated mode. The desktop application, window identity and application state are not restarted during reattachment.

This is opt-in, per-Unix-account process retention. It is **not** a PAM login broker, Unix-user impersonation, filesystem sandbox, checkpoint/restore facility, Wayland virtual compositor, or recovery after the broker or host restarts. Applications execute as the account that runs the broker. Different RDP principals under that account do not obtain different Unix security identities.

## Run

Build normally with the headless backend enabled and install the Xorg dummy driver. The build produces `lrdpd`, `lrdp-sessiond` and `lrdp-sessionctl`. Run both daemons as the same ordinary user, never root or a setuid program.

Create an existing private runtime directory:

```sh
install -d -m 700 "$XDG_RUNTIME_DIR/lrdp-persistent"
```

Start the broker in its own terminal or service:

```sh
./build/lrdp-sessiond \
  --directory "$XDG_RUNTIME_DIR/lrdp-persistent" \
  --retain-seconds 1800 \
  --max-desktops 4 \
  --desktop-command /usr/bin/xterm
```

Executable paths must be absolute. Repeat `--desktop-arg ARG` for application arguments. `--xorg-executable` changes the Xorg executable. These options belong to the **broker**, not the RDP worker, when persistence is enabled; the worker cannot choose a replacement command while resuming a desktop. Commands are executed directly, not interpreted by a shell. A desktop exits when its configured application/session leader exits.

Add these arguments to the existing authenticated `lrdpd` configuration:

```sh
--backend headless \
--session-broker "$XDG_RUNTIME_DIR/lrdp-persistent/broker.sock"
```

For example, with administrator-provisioned GSS service credentials, a certificate and an authorized principal:

```sh
export KRB5_KTNAME=/secure/path/to/lrdp.keytab
./build/lrdpd \
  --auth nla \
  --service TERMSRV@desktop.example.org \
  --allow-principal alice@EXAMPLE.ORG \
  --cert /secure/path/to/chain.pem \
  --key /secure/path/to/key.pem \
  --backend headless \
  --session-broker "$XDG_RUNTIME_DIR/lrdp-persistent/broker.sock"
```

The paths and identities above are deployment-specific examples, not automatically provisioned accounts. See [SECURITY.md](SECURITY.md) for GSS configuration. Kerberos is preferred; system GSS-NTLMSSP requires explicit `--allow-ntlm`. No security negotiation is downgraded for reconnection. The existing `--lab-no-auth` profile is supported for local development only, remains loopback-only, and has a separate fixed broker identity; never forward it to untrusted peers or use it for a production desktop.

A reconnect-capable client must retain the server cookie and send its derived verifier when reconnecting. Starting an unrelated new client connection without that cookie creates a **new desktop**; identity alone never selects an existing desktop. An invalid, expired or replayed cookie fails the connection instead of silently launching a replacement application.

## Lifecycle and limits

The broker has one exclusive control-socket lease per attached desktop. It observes socket EOF even if the RDP worker is killed. It retains only desktops whose initial activation completed and whose peer advertised `AUTORECONNECT_SUPPORTED`. Unsupported peers receive no cookie; their desktops are deleted when their connections close.

Default detached retention is 1,800 seconds; the configurable range is 1 to 86,400 seconds. The default desktop quota is four, configurable up to 64. Connected and detached desktops both count against that quota. Detached desktops keep running and consume resources until expiry or explicit termination. Connected desktops do not expire under the detached timer; ordinary RDP transport/idle policies still apply.

The registry rejects attempts to attach while another lease is live, even with a valid cookie. Resume does not steal the active session. A resume that fails before activation leaves the previous cookie and original detached expiry intact; failed attempts cannot repeatedly extend the deadline. A newly created desktop abandoned before activation is deleted.

A successful reattachment rotates the cookie, and connected sessions rotate it hourly. Ordinary resize/reactivation does not change its identity. Cookie publication is subject to the network: a connection loss after server-side rotation but before receipt can leave the client holding an obsolete cookie. There is no unsafe old-cookie grace window or username-only fallback.

Existing graphics references, clipboard transfers, channel identifiers, drive mounts and virtual audio endpoints are **connection-scoped** and are recreated. Application-owned native clipboard selections can survive with their owning application. LRDP-owned received clipboard staging and in-flight transfers are not promised to survive a disconnect. Applications must handle disconnected redirected resources rather than relying on old FUSE handles or audio links.

A resumed connection recreates the native X11 adapter and applies the requested layout transactionally. RandR mode names are checked against surviving server modes; retired private modes are detached and destroyed after successful reconfiguration, not accumulated on each resume. Input teardown runs before orderly lease release. After a worker crash, the exclusive private display's held keys/buttons are released before new input is accepted, including modifiers held at the moment of the crash.

## Administration

```sh
./build/lrdp-sessionctl --socket "$XDG_RUNTIME_DIR/lrdp-persistent/broker.sock" list
./build/lrdp-sessionctl --socket "$XDG_RUNTIME_DIR/lrdp-persistent/broker.sock" terminate SESSION_ID
```

The list prints numeric session ID, attached/detached state and broker principal. Authenticated identities have an `nla:` namespace prefix; the original domain, realm and case remain exact. No cookie/random/verifier or credential is printed. Termination revokes an active lease and stops its Xorg/application process groups, or removes an idle retained desktop.

Broker shutdown stops owned desktops and removes its socket. An existing socket is never silently removed on startup. After a crash, verify no broker owns it before removing stale runtime files. A forced broker/host failure is not persistent recovery: the in-memory registry and cookie keys are lost, and process/runtime cleanup may require operator intervention.

## Security boundary

The control endpoint is a Unix `SOCK_SEQPACKET` socket in an existing owner-controlled `0700` directory. Both endpoints check Unix peer credentials. Paths are resolved through a pinned directory descriptor, socket symlinks and loose permissions are rejected, and descriptors are close-on-exec. Protocol packets, control connections, replies, startup, activation and detached lifetimes are bounded. Malformed packets or unexpected ancillary descriptors close the offending control connection.

The broker trusts the same Unix account. Its local administrative API can terminate that account's sessions, and local processes running as that account are not isolated from its desktop secrets or files. NLA authorization is performed by `lrdpd` **before** broker acquisition. Reconnection then requires the exact namespaced authenticated principal and constant-time verifier comparison. Client Info usernames, addresses and `clientSessionId` are never used as authenticated identities or session selectors.

OpenSSL generates random session identifiers and 128-bit cookie keys. The verifier is the protocol-required HMAC-MD5 of 32 zero bytes under Enhanced RDP Security, implemented by OpenSSL, not home-grown cryptography. An OpenSSL policy that disables this primitive causes the operation to fail without selecting a substitute algorithm. Cookies are sent only within the protected RDP connection and are never persisted to disk by LRDP.

Native desktop startup and broker control operations are bounded but synchronous in the broker. This is not an unbounded asynchronous job system. Authentication and graphics/input processing continue in separate RDP workers.

## Tests and evidence boundaries

`reconnect_wire` covers exact cookie and Extended Logon Info framing, optional Client Info boundaries, Unicode validation, 850 assertions and 20,000 seeded malformed inputs. `session_registry` covers identity/proof rejection, active-lease exclusion, quotas, replay after rotation, abandoned-resume expiry, hourly rotation, cleanup, and a Python-derived independent HMAC vector.

`persistent_reconnect` runs the real TLS server, native broker, rootless Xorg and an independently linked Xlib application. It checks stable application PID/window identity and changed pixels across killing the RDP process, native clipboard preservation, release of a crashed Control key, fresh input, resized output, stale/bad cookies, wrong principals, an abandoned resume, expiry, non-reconnect clients, administration and socket cleanup. Its wire/HMAC client uses Python's standard library, not LRDP's ARC codec. The local cookie fixture intentionally uses the loopback laboratory profile.

When the existing FreeRDP/system-GSS tests are enabled, `persistent_nla` separately verifies real NLA authorization into the broker and rejection of wrong credentials/unauthorized principals **without allocating a desktop**. It does not claim that a stock client's automatic retry UX has been verified against this server. Windows clients, Kerberos-domain deployments, physical GPUs, real Wayland compositor retention and long-duration WAN disruption remain unverified.

## Specification provenance

Original implementation based on public Microsoft Open Specifications; no other RDP implementation source is imported:

- [MS-RDPBCGR 5.5: Auto-Reconnection](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/e729948a-3f4e-4568-9aef-d355e30b5389).
- [2.2.4.2: Server Auto-Reconnect Cookie](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/18f4f605-0ee3-4175-8a62-cf8775252547).
- [2.2.4.3: Client Auto-Reconnect Cookie](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/2985e8e3-db10-4a92-9fd5-d5e742d2d0f2).
- [2.2.1.11.1.1.1: Extended Info](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/05ada9e4-a468-494b-8694-eb806a0ecc89).
- [2.2.10.1.1.4: Extended Logon Info](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/d3426aa1-a8e7-4b93-b45d-b4ebe0662d50).
- [2.2.7.1.1: General Capability Set](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/41dc6845-07dc-4af6-bc14-d8281acd4877).
