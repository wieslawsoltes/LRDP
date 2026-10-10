# Modern graphics negotiation and telemetry

This continuation accepts the explicitly documented RDP graphics capability sets
10.3 through 10.7 in addition to existing 8.1, 10, 10.1 and 10.2. Unknown version
numbers remain unselected. No new codec capability is implied by this parser.
10.3's small cache is implicit; 10.7 explicitly disables scaled mapping because
LRDP currently maps to an unscaled output. Mutually exclusive AVC flags are
validated. Without an encoder, AVC_DISABLED is confirmed; 10.1 (which mandates
AVC444v2) is not selected. Other supported 10.x sets use the existing AVC444v1
path conservatively.

The merged PR #9 QoE API (`GraphicsQoe`, `latest_qoe`, and
`capability_version`) and its additional decoder/oracle tests are retained.
This extends that implementation rather than replacing its validation evidence.

## QoE does not acknowledge a frame

The 20-byte QoE Frame Acknowledge PDU is accepted for versions which define it.
Its frame identifier must refer to a sent frame. Exactly one latest sample and
two saturating counters are retained: there is no peer-sized telemetry queue.
Duplicate, out-of-order and old-generation samples cannot rewind the latest
measurement. The unsigned client timestamp is unwrapped relative to the first
sample; a backwards or ambiguous half-wrap interval invalidates the derived
elapsed time rather than fabricating an accurate measurement. The raw timing
fields remain advisory (zero can denote an unavailable/overflowed measurement).

Only an ordinary Frame Acknowledge PDU releases flow-control credits. QoE traffic
cannot bypass the two-frame window, allocation quota or acknowledgement deadline.

## In-channel capability reset

A previously negotiated 10.3+ client can re-advertise capabilities. The complete
advertisement and confirmation are validated/allocated before replacing graphics
state. Surfaces, pending frame credits, telemetry and unsent graphics commands
are discarded. The Session retires the old video worker and invalidates its
completion generation before configuring the new surface and encoder.

Frame IDs are not reused. Old-generation acknowledgements cannot release a new
frame or disable its acknowledgement flow control. Identifier exhaustion requires
a reconnect instead of ambiguous wraparound. A normal desktop resize does not
constitute this capability reset.

## Evidence and boundaries

`graphics_compatibility` exercises capability ordering, unavailable encoders,
invalid flags, timestamp wrap, stale/future frame identifiers, no-credit QoE,
transactional malformed advertisements, and 10,000 deterministic malformed PDU
probes. `gfx_compatibility_session` uses an independent Python client over actual
certificate-verified TCP/TLS, re-advertises three versions on the same graphics
channel, checks decoded pixels, and continues through clipboard/input/resize.
The companion lossless TLS fixture uses the independent ClearCodec/SolidFill
decoder and verifies that capability resets restart the codec sequence whereas
ordinary desktop resize does not. The existing tile damage encoder, solid-fill
batching, printer/drive traffic, and persistence/network hooks are retained.
These are project conformance fixtures, not Windows-client or physical-GPU
interoperability certification.

## Public specification provenance

- MS-RDPEGFX 2.2.2.21, QoE Frame Acknowledge:
  https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/17aaf205-23fe-467f-a629-447f428fdda0
- MS-RDPEGFX 3.2.5.21, advisory QoE processing:
  https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/06a92ec4-1048-4e5f-b2f9-bc114da00d88
- MS-RDPEGFX 3.3.5.19, client capability re-advertisement:
  https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/a720af02-0a47-44e2-a934-dc8534062b44
- Capability sets 10.3 and 10.7:
  https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/fef125c5-60be-43af-8ad1-2158761f4b32
  https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/ba94595b-04de-4fbd-8ee4-89d8ff8f5cf1
