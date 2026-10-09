# Deployment preflight and build capabilities

`lrdpd --version` returns the CMake project version. `lrdpd --capabilities` returns schema-versioned JSON describing integrations compiled into that executable. It needs no certificate or service configuration, does not connect to any desktop or bus, and never probes a GPU. A compiled FFmpeg/VA-API/NVENC path is not a guarantee that a codec or physical encoder can initialize. The report explicitly distinguishes unimplemented zero-copy capture and PAM login brokerage from the available per-account headless broker.

`lrdpd --check-config` accepts the same ordinary options as server startup, with one shared parser. It loads and checks the certificate/private-key pair, checks the leaf certificate's validity interval against the local clock, validates the numeric listen address, and inspects configured endpoint directories and broker socket metadata. It checks required headless executables only for non-broker headless sessions. No TCP listener, desktop, portal dialog, mount, print directory, audio device, child process, or broker connection is created.

```sh
./build/lrdpd --capabilities
./build/lrdpd --check-config \
  --auth nla \
  --service TERMSRV@desktop.example.org \
  --allow-principal user@EXAMPLE.ORG \
  --cert /secure/desktop-chain.pem \
  --key /secure/desktop-key.pem \
  --backend headless \
  --desktop-command /usr/bin/xterm \
  --encoder lossless \
  --network-metrics
```

The identities and paths above are placeholders for an administrator-provisioned deployment. Run preflight as the same Unix user and environment that will run the server. The command is suitable as a service `ExecStartPre` check, but no service is installed or modified automatically.

Success produces JSON with `kind=configuration-preflight`, `valid=true`, and an explicit `unchecked` list. Errors go to stderr with a nonzero exit status and no success object. Reports omit credential paths, key material, service names and principal names; they include only the allowed-principal count, selected backend/encoder, and compiled capability report.

Preflight intentionally does not claim certificate trust-chain/hostname verification, NLA credential/KDC validation, listener address availability, a responsive broker, compositor consent or capture, physical printer/audio/GPU access, or successful FUSE mounting. For example, a configured TCP port may already be occupied: preflight still succeeds because it never binds. Native permission grants and per-connection NLA authorization remain runtime requirements. This is point-in-time configuration validation, not a persistent lease or a TOCTOU-proof authorization boundary.

TLS file loading is now explicitly noninteractive in both ordinary startup and preflight. Local credential paths must resolve to nonempty regular files no larger than 4 MiB. Certificate-manager symlinks remain permitted. A password-encrypted PEM key fails immediately rather than invoking OpenSSL's terminal callback; LRDP does not yet provide a secure passphrase provider. Do not make a key world-readable to work around this limitation. Keep service key files protected by the administrator's access policy. Preflight does not inspect every ACL or independently certify key-file access policy.

The automated fixture holds stdin open while testing encrypted keys, supplies FIFO/directory/oversized/mismatched credentials, generates expired and future certificates with explicit validity dates, and tests optional build-feature gates. It holds the configured TCP port open, supplies a non-listening broker socket and an executable marker script, and verifies that preflight neither connects nor executes nor creates deployment artifacts. Dependency-minimal and native builds are tested separately so unavailable integrations cannot be accidentally reported as compiled.

Primary API references: OpenSSL `SSL_CTX_set_default_passwd_cb` and `X509_cmp_time`; the latter returns zero for parse errors, which are rejected rather than interpreted as an equal timestamp.

https://docs.openssl.org/3.0/man3/SSL_CTX_set_default_passwd_cb/
https://docs.openssl.org/3.0/man3/X509_cmp_time/
