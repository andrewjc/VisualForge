# Phase 3 — Packet schema, observation trace, and replay

Status: complete

## Implemented slice

- Versioned little-endian trace protocol with a 64-byte file header and
  32-byte record envelope.
- Pointer-free fixed-layout payloads for capture, frame, view, writer, resize,
  frame-end, and capture-end events.
- Separate CPU envelope correlation and GPU/work writer correlation IDs.
- Stable FNV-1a IDs derived from semantic domain, canonical key, and generation;
  process addresses are not part of identity.
- IEEE CRC-32 at file-payload and record-payload boundaries.
- Bounded writer arena with no partial publication when capacity is exhausted.
- Zero-allocation record inspector with schema-major negotiation, forward minor
  acceptance, unknown-record skipping, padding checks, and explicit corruption
  errors.
- FrameCapture state and ownership validation.
- Standalone vf_packet_replay --inspect tool with a pre-allocation 64 MiB input
  ceiling.
- Opt-in one-shot live observation at the existing Present/Resize boundary.
  Tracing is enabled only by VISUALFORGE_TRACE_ONCE; publication uses a
  temporary file, FlushFileBuffers, self-inspection, and atomic replacement.

No vanilla render call is suppressed or replaced in this phase.

## TDD/RGR evidence

The red suite was built around an independently computed 440-byte golden trace,
including all record CRCs and the header/payload CRCs. Ten of eleven Phase 3
cases failed on 18 intended assertions against inert codec stubs; the fixed
layout contract was already green.

Final focused result:

- 8,927 assertions across 13 Phase 3 cases.

Covered contracts include deterministic byte identity, the golden trace,
layout/alignment, standard CRC vectors, stable IDs across different string
storage addresses, schema/endian handling, file and record CRC distinction,
truncation, caller-configured limits, pointer-flag rejection, unknown record
skipping, nonzero padding, bounded-arena failure, and frame ownership.

Accumulated results:

- Debug: 40/40 CTest tests passed.
- Release: 40/40 CTest tests passed.
- Plugin export contract remains unchanged.

## Live Observe gate

Observe artifact:

artifacts/phase-03/observe-20260815-105835

Observed records:

    renderer-observe: armed state=Observing
    renderer-health schema=1 mode=Observe backend=absent suppression=off
    renderer-observe: trace complete frames=1 views=1 writers=2

The external Release inspector reported:

    trace schema=1.0 capture=2379525771543518596 records=7 frames=1 views=1 writers=2 resizes=0 unknown=0 first-frame=8227775763898471227 last-frame=8227775763898471227 thread=67120

The live trace is:

artifacts/phase-03/observe-20260815-105835/live-frame.vftrace

## Disabled-trace regression gate

Off artifact:

artifacts/phase-03/off-20260815-105915

The clean-environment run logged the exact Phase 2 health record, did not arm
Observe, and did not publish a trace. Both smokes passed without early process
exit and restored the installed plugin/log/INI set.

Restored installed DLL SHA-256:

FCBB0032169C05BF992F77F96A6F3B5EA6C8AD7A5840DB8F516FCE90278FB29F

## Promotion decision

Phase 3 is promoted. Phase 4 may negotiate a backend and report Vulkan
capabilities, while Off and Observe remain inherited fallback oracles.
