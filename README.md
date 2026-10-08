# LRDP

A clean-room Linux Remote Desktop Protocol implementation, derived from Microsoft's public Open Specifications and public Linux platform interfaces. No Microsoft implementation source is used.

The repository is being bootstrapped. It does not yet provide a production-ready RDP service or claim complete Windows feature parity. Capability support and interoperability evidence will be tracked explicitly.

## Design priorities

- Clipboard, dynamic monitor layouts, correct input and session lifecycle are core features, not streaming add-ons.
- Native Linux integration without application injection or `LD_PRELOAD`.
- Bounded protocol parsing, encrypted transport, least privilege and explicit consent.
- Hardware-accelerated encoding with validated fallback; never advertise an unimplemented wire capability.
- Public-specification provenance, conformance tests and observable performance.
