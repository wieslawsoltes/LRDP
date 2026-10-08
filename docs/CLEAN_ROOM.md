# Specification-derived implementation

LRDP wire code is original C++20 code derived from public protocol descriptions, not copied from Microsoft, FreeRDP, xrdp, or other RDP implementation source. Public platform libraries provide cryptography, display integration, and codecs; they are not replaced with home-grown cryptography. This is a provenance record, not a legal clean-room certification.

## Normative inputs

- [MS-RDPBCGR](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/5073f4ed-1e93-45e1-b039-6e30c385867c): transport, negotiation, basic settings, activation, input, bitmap updates and static channels.
- [RDP negotiation](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/902b090b-9cb3-4efc-92bf-ee13373371e3).
- [MS-RDPEDYC](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpedyc/3bd53020-9b64-4c9a-97fc-90a79e7e1e06): dynamic channels; 1600-byte PDU limit and 8/16/32-bit identifiers.
- [Display layout](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpedisp/ea2de591-9203-42cd-9908-be7a55237d1c) and [layout PDU](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpedisp/22741217-12a0-4fb8-b5a0-df43905aaf06).
- [Clipboard capability structure](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeclip/017fbfda-2f6d-449a-b9cf-396b2cc58591), MS-RDPECLIP sections 2 and 3.
- [CredSSP sequencing](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-cssp/385a7489-d46b-464c-b224-f7340e308a5c).
- [XDG RemoteDesktop portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.RemoteDesktop.html): capture/input/clipboard session lifecycle and consent.

## Contribution rules

Record the public specification and section for new wire behavior. Do not import proprietary implementation code, disassemblies, private documentation, or implementation excerpts from other RDP projects. Keep unknown/unimplemented capabilities disabled. Distinguish unit tests, local protocol integration tests, and independent client interoperability evidence. A round-trip through LRDP's own encoder and decoder is not proof of Windows compatibility. Record unavailable test environments and hardware explicitly.
