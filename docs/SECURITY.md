# Security model

LRDP is experimental. Passing tests is not an independent security audit or full Windows interoperability certification.

## Authenticated desktop sharing

`--auth nla --service TERMSRV@host.example.org --allow-principal user@REALM` requires RDP HYBRID security and CredSSP v5 or newer. System GSSAPI performs authentication and message protection. Kerberos is permitted by default. `--allow-ntlm` explicitly permits the installed GSS-NTLMSSP mechanism; LRDP does not implement NTLM cryptography or maintain a password database.

Provision the service's acceptor credentials through the operating system's GSS/Kerberos configuration. For example, use an administrator-provisioned, user-readable dedicated service keytab via `KRB5_KTNAME`. Do not run the server as root merely to read a system keytab.

The authenticated principal must exactly match an explicitly supplied `--allow-principal` entry. Principals are not stripped of their realm, case-folded, or interpreted as Unix usernames. Allowed principals access the desktop of the Unix account running LRDP. This is authenticated desktop sharing, not a multi-user login broker. No Unix impersonation, PAM session creation, or automatic home-directory mounting is performed.

CredSSP's nonce-bound client and server SHA-256 hashes bind GSS message protection to the TLS certificate SubjectPublicKey. The public-key hash includes the specified direction string's NUL terminator. Legacy versions without nonce binding, changing protocol versions, duplicate DER fields, unprotected delegation, reordered or replayed GSS messages, and unauthorized principals are rejected. Delegated password structures are validated and immediately wiped; they are not used as an alternate authorization identity.

## Laboratory profile

`--lab-no-auth` remains deliberately restricted to `127.0.0.1` or `::1`, requires TLS, and is mutually exclusive with NLA. Client Info usernames/passwords are not authentication. Never expose a laboratory port through an unauthenticated proxy or port forwarding.

## Process and resource boundaries

Each connection has its own child process, created before opening platform or encoder resources. `--max-sessions` bounds concurrent children. Negotiation, TLS, CredSSP, activation, partial records and stalled writes have deadlines. Protocol messages, framebuffers and clipboard transfers have allocation quotas. Only complete validated input batches are injected. Keyboard and pointer state is released on orderly session teardown.

The parent owns its child process list and terminates only those children at shutdown. A stalled driver can still require forced termination; no claim of hard real-time driver cancellation is made.

## Private Xorg and file redirection

The headless backend creates a separate rootless X server with a per-session authentication cookie and no TCP listener. Its application/session process still has the server account's Unix permissions; this is not a filesystem sandbox, separate Unix login or PAM broker.

File clipboard is off by default. `--clipboard-files DIRECTORY` restricts exported selections and received staging to an existing user-owned directory without group/other write permission. Source paths are resolved by directory descriptors without following symlinks; only pinned regular files/directories are read. Incoming paths are validated before filesystem creation, received files are mode 0600, and clipboard publication requires complete range coverage. Source files are never deleted and remote files are never executed.

Ordinary session cleanup removes received staging, not user source files. Crashes/forced termination can leave private staging and require inspection after sessions have ended. A process with the same Unix identity is outside this confinement threat model. See [FILE_CLIPBOARD.md](FILE_CLIPBOARD.md) for exact quotas, synchronous-I/O limitations and lifetime.

## Normative sources

- MS-CSSP 2.2 and 3.1.1: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-cssp/385a7489-d46b-464c-b224-f7340e308a5c
- TSRequest: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-cssp/6aac4dea-08ef-47a6-8747-22ea7f6d8685
- MIT GSSAPI: https://web.mit.edu/kerberos/krb5-1.22/doc/appdev/gssapi.html
- System GSS-NTLMSSP configuration: https://github.com/gssapi/gss-ntlmssp/blob/main/TESTING.txt

Tests separate state-machine fixtures (not cryptographic evidence) from independent FreeRDP/GSS-NTLMSSP socket interoperability. Kerberos domain, smart-card delegation, Remote Credential Guard and Windows client coverage must be reported separately.
