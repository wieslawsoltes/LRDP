# Continuous network measurements

`--network-metrics` enables optional diagnostics on the existing authenticated or
loopback-laboratory TLS/TCP connection. It adds no new listening socket, native
library dependency, worker thread, authentication bypass, UDP transport or gateway.
Without the switch the server does not initiate network measurements. A client
must also advertise `RNS_UD_CS_SUPPORT_NETCHAR_AUTODETECT` (0x0080) and explicitly
request the optional MCS message channel.

Add the flag to the existing server command, for example for local diagnostics:

```sh
./build/lrdpd --lab-no-auth --cert certificate.pem --key private-key.pem \
  --backend demo --encoder raw --network-metrics
```

Laboratory mode is unauthenticated and restricted to loopback. Do not forward it
to untrusted peers. The flag works with the existing NLA profile without changing
its authorization policy. For an independent FreeRDP client, `/network:auto`
requests automatic network characteristics; the installed client must implement
that capability. Windows client interoperability is not yet recorded here.

## What the numbers mean

Every five seconds the server reports `Network:` diagnostics. Before a valid
sample exists an optional value is `unknown`, not a fabricated zero.

- `rtt_us`: server-monotonic elapsed time from the completed TLS plaintext write
  of the RTT request to parsing its matched response. It includes scheduling,
  TLS/network buffering after that boundary, and peer response processing; it is
  not a raw IP ping or a precise physical-wire timestamp.
- `smoothed_rtt_us`: 7/8 previous value plus 1/8 latest sample. `jitter_us` is a
  3/4-smoothed absolute deviation from the previous smoothed RTT. Neither claims
  to be an RTP jitter estimator or a percentile.
- `minimum_rtt_us`: minimum of the last 32 accepted RTT samples, not a lifetime
  minimum. `rtt_age_ms` and `bandwidth_age_ms` identify stale observations.
- `peer_receive_kbps`: peer-reported received bytes times eight divided by its
  elapsed milliseconds, using 64-bit arithmetic. This is an **application-limited
  receive-throughput observation**, not available link capacity. An idle desktop,
  lossless desktop, saturated link and slow encoder can all yield different values.
- Sample, ignored-response, invalid-measurement and timeout counters make missing
  data explicit. Disabled flags and sequence exhaustion are logged separately.

The peer can misreport its timer or byte count. The server checks that accepted
bytes do not exceed observed transmitted plaintext bytes in the matching window,
rejects zero/impossible durations and insufficient samples, and retains a larger
wire-byte budget for inspection through the C++ API. These checks do not make the
peer's bandwidth claim trustworthy. **No measurement changes authentication,
encoder quality, frame credit, codec negotiation or resource quotas.**

## Protocol and ordering

The existing security negotiation already advertises
`EXTENDED_CLIENT_DATA_SUPPORTED`, permitting extended client data blocks.
Server policy, Client Core support and an explicit `CS_MCS_MSGCHANNEL` request
must all be present before allocating a separate MCS message channel. It is not
a `drdynvc` channel or an entry in the static-channel array. The client's reserved
field is checked. Requested but declined channels receive ID zero; when the client
omits the optional request block, no Server Message Channel Data block is emitted
(MS-RDPBCGR 2.2.1.4). Client Core support alone is insufficient. The dedicated
channel is joined and checked like other MCS channels, including when all 31
static channels are present.

Continuous request types are RTT `0x0001`, bandwidth START `0x0014`, and bandwidth
STOP `0x0429`, carried with `SEC_AUTODETECT_REQ`. Responses use
`SEC_AUTODETECT_RSP`, RTT type `0x0000` and bandwidth result type `0x000B`.
Connect-time variants and unimplemented message types are rejected rather than
misinterpreted as continuous results. Required ignored security-header fields
remain ignored. Response parsing is allocation-free and validates the complete
10- or 18-byte payload before consuming a pending request.

No probe precedes the first **actually transmitted Font Map**, even if the
Session has already reached its active state while activation packets are queued.
The transport exposes a borrowed complete-packet view once its final partial
`SSL_write` succeeds. The observer does not move, copy, retain or reorder the
active buffer, including across `SSL_ERROR_WANT_READ/WRITE` retries.

START precedes the current normal application batch. STOP follows that batch and
all previously queued normal packets, so an expensive render completing after a
sampling deadline cannot be overtaken by its stop marker. Existing packet-boundary
media priority is preserved; the byte budget uses actual completed transmission
order. There are no synthetic bulk payloads and no capacity-saturating probe flood.

## Fixed limits and lifecycle

Default policy: one RTT request at most every 2 seconds; one passive bandwidth
window at most every 5 seconds when application data is ready; a nominal 250 ms
window; 5-second response timeout measured after transmission; at least 1 KiB for
a usable bandwidth sample. Rendering and queued traffic can extend the actual
window. New operations require a fully active session and an empty transport
queue; an existing STOP and timeout continue during congestion or reactivation.

Only one RTT and one bandwidth operation can be outstanding. Three missing
responses disable only that class of monitoring, with exponential retry backoff;
they do not close an otherwise functioning desktop. Malformed protocol messages
still follow the ordinary session error path. Normal socket/write deadlines are
not relaxed.

Request IDs monotonically consume the 16-bit space and are **never reused within
a connection**. Once all 65,535 IDs have been issued, new monitoring stops and
`sequence_exhausted=1` is reported. An outstanding bandwidth STOP still completes.
This deliberately trades indefinitely long monitoring for unambiguous stale-reply
handling without unbounded memory. Reconnecting creates a fresh measurement state.

Valid, duplicate and stale diagnostic replies do **not** refresh the existing
30-minute idle-receive deadline. Ordinary input/control traffic retains its
previous behavior. Diagnostics continue while graphics are suppressed, without
forcing desktop capture or defeating suppression.

## Tests and evidence boundaries

`network_autodetect` uses a deterministic clock and explicit completed-write
notifications: queue residence, partial ordering, rolling minimum, EWMA, 64-bit
throughput, malformed payloads, duplicate/stale replies, independent timeout
backoff and exhaustive sequence exhaustion. `network_session_state` exercises
real Session activation, message-channel join checks, the Font Map egress gate,
security parsing and idle-activity classification.

`network_session` is a separate Python TLS wire peer. It verifies capability and
CLI and explicit-channel gating, arbitrary join/block order, 31 static channels, real pixel output,
matched RTT and passive bandwidth, early-response refusal, stale responses and
graphics suppression/resume. Its ordinary Refresh Rect requests create real
framebuffer traffic rather than synthetic bandwidth-test payloads.

`network_client`, when FreeRDP and Xvfb are available, runs the installed
independent client with `/network:auto`, requires repeated RTT samples, and checks
its actual native presentation pixels. This does not imply Windows, mobile,
physical-GPU, real-WAN, UDP, gateway or connect-time autodetection validation.
Consult the exact-revision CI report; unavailable optional targets are not passes.

## Public specification provenance

Original implementation from Microsoft Open Specifications, not another RDP
implementation's source. System TLS remains OpenSSL.

- [MS-RDPBCGR 1.3.9: continuous versus connect-time detection](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/dc672839-4f4e-40b1-a71c-cd6a959baa38)
- [Client Message Channel Data, 2.2.1.3.7](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/f50e791c-de03-4b25-b17e-e914c9020bc3)
- [Server MCS Connect Response, 2.2.1.4](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/927de44c-7fe8-4206-a14f-e5517dc24b1c)
- [Server Message Channel Data, 2.2.1.4.5](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/9269d58a-3d85-48a2-942a-bb0bbe5a55aa)
- [Basic Security Header, 2.2.8.1.1.2.1](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/e13405c5-668b-4716-94b2-1c2654ca1ad4)
- [RTT request, 2.2.14.1.1](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/33b5dd38-a7c3-43d5-a717-ded2391ed599)
- [Bandwidth START, 2.2.14.1.2](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/1429c9e6-3e33-462b-b0d9-7dbff7faf979)
- [Bandwidth STOP, 2.2.14.1.4](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/515150db-4e7a-4c9b-88d8-63f9fe79981f)
- [Bandwidth results, 2.2.14.2.2](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/6999bd6a-7eb2-4fba-9e5a-c932596056bf)
- [Client processing, 3.2.5.14](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/16ffa852-8aa7-481c-99a0-36c1a9a198f6)
