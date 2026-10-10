# Local-work reconciliation — October 10, 2026

This closeout audits the source archives and patch series retained in the ChatGPT
workspace. It does not inspect unconnected checkouts on the operator's computer.
There was no pre-existing live Git worktree in this workspace, only retained
source/evidence archives.

## Published baseline

`LRDP-source-c381bb0.zip` reconstructed all 232 tracked files of tree
`f3038ec62dc77ce1c724367d4f7bb63a59a0e747`, exactly matching PR #15 head
`c381bb08a69b4be0af3892ff78b06ef7936d2f5b`. That final head passed CI run
37997598448 and was merged at `dad0cb269119c35207911327ead1be3b7a6b064b`.
No unpublished delta was present in that latest source archive.

## Older local-only delivery

The retained `LRDP-patches-and-evidence.zip` manifest describes delivery
`486c8e1c0305ed530c8118038817311b6d3735c4`, based on merged AVC444 PR #9.
Its three workstreams were reconciled individually, not applied over newer code:

- **Graphics compatibility:** the distinct 10.3–10.7 negotiation, bounded QoE
  telemetry and capability-reset changes were recovered onto the published
  baseline. Existing H.264, lossless, printing, drives, broker and network hooks
  are preserved. New tests cover ClearCodec sequence restart at capability reset
  and continued sequence across ordinary resize, using the newer merged encoder.
- **ClearCodec:** the older alternative band encoder and fallback-selection patch
  are superseded by merged PR #10's explicit lossless mode, exact tiled damage,
  SolidFill batching, residual codec and independent-client presentation tests.
  Replacing that implementation with an older alternative would regress the
  integrated product. No duplicate encoder or conflicting default was imported.
- **Drive capacity:** the old Class 7/Class 3 capacity and unsigned statvfs work
  is superseded by merged PR #13's fuller capacity/metadata/readonly-annotation
  implementation and its kernel-FUSE/FreeRDP validation. It is not re-applied.

Other retained source archives identify earlier revisions already represented by
merged PRs #1–#15; their evidence and binaries are historical outputs, not pending
source changes. Archives are retained rather than deleted.

## Scope

Finishing retained local work and merging PRs is not a declaration of full Windows
RDP parity. The unimplemented and unverified areas in
[IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md) still apply. New graphics
behavior and public specification references are documented in
[GRAPHICS_COMPATIBILITY.md](GRAPHICS_COMPATIBILITY.md).
