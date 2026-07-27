# Phase 12 — Instancing, streaming, generations, and cell lifecycle

Status: offline implementation complete; live capture and overlay gate pending

## Implemented slice

- `SceneDatabase` mirrors the engine's instance and streaming lifecycle
  without ever dereferencing an engine address. It composes the Phase 5
  `ResourceRegistry` for geometry generations and retirement timing and the
  Phase 8 `DescriptorQuarantine` for descriptor reuse, so lifetime tickets are
  unified rather than duplicated.
- Instance identity is minted by the database as `(generation << 32) | slot`,
  is stable while the instance is attached, and changes on every reuse of a
  slot so motion history can never be inherited by a different object.
- Content-hash deduplication folds two distinct engine addresses carrying
  identical immutable content into one resident geometry, one upload, and one
  byte charge. Independently mutable resources are never folded together even
  when their current contents hash identically.
- Residency budgeting fails closed. An add that would exceed the byte budget
  leaves no slot, descriptor, geometry, or byte behind, and a shared copy
  still fits because it adds no bytes.
- Cell groups attach and detach in any order. Detaching retires every instance
  in the group on the submission timeline, quarantines its descriptor, drops
  its geometry references, and releases geometry only when the completed
  timeline value passes.
- An unload that overtakes its own upload cancels it. A completion for a
  retired generation is rejected as a stale handle instead of resurrecting
  content, and an address reused for different content while its previous
  generation is still retiring fails closed rather than aliasing.
- `ResourceRegistry` no longer wraps generations. Reaching the configured
  handle-space limit returns `GenerationExhausted`, because a wrapped
  generation would let a stale handle alias a live resource. The scene
  database applies the same rule to instance slots.
- `SceneDelta` records every lifecycle transition and converts into the new
  pointer-free `RegistryDelta` trace record. The trace carries stable
  identity, content hash, byte size, generation, group, and timeline value; it
  deliberately drops the engine address, which is meaningless outside the live
  process. `RecordType::RegistryDelta` is record type 8 and `InspectTrace`
  counts it.
- Scene packet version 1.1 adds an instance section of 160-byte records
  carrying per-instance identity, current and previous transforms, and
  per-instance material parameters. Version 1.0 is unchanged and expands to
  one implicit instance per object, so every consumer sees a single expansion
  rule and Phase 11 captures stay byte-identical.
- Instances form one contiguous run per object and every object must own at
  least one. Out-of-range object indices, reordered runs, duplicate instance
  identity, uncovered objects, singular transforms, out-of-range parameters,
  and unknown flags all fail closed.
- The Vulkan mirror uploads the instance table to reflected storage binding 9
  and issues one `vkCmdDrawIndexed` per object with `instanceCount` equal to
  that object's run, selecting the record with `gl_InstanceIndex` plus a
  per-draw `firstInstance` push constant. Repeated meshes therefore become
  instances of one draw rather than duplicated draw calls.
- Per-instance material parameters modulate only their own instance in both
  the CPU oracle and the fragment shader, and the G-buffer identity plane
  carries the instance identity with the object's material identity.

## TDD/RGR evidence

The database contract began red on a missing header:

```text
SceneDatabaseTests.cpp(1): fatal error C1083: Cannot open include file:
    'renderer_core/SceneDatabase.h': No such file or directory
```

The trace record began red on missing types:

```text
TraceCodecTests.cpp(303): error C2065: 'RegistryDelta': undeclared identifier
TraceCodecTests.cpp(303): error C2653: 'RegistryDeltaKind': is not a class or
    namespace name
```

The Vulkan vertical contract was registered before replay support existed:

```text
contract.instanced_scene_frame: Failed
vf_packet_replay: usage (unrecognized --render-instanced-scene)
```

The first end-to-end run then failed on evidence rather than usage, and the
failure was a genuine contract finding: after the cell detached, the mirrored
frame still claimed the detached cell's opaque pass, so Phase 11's accounting
correctly refused to arm it.

```text
instanced-replay: pass accounting rejected the scene
visible-after=0 instancing=fail parity=fail result=fail
```

The fixture now stops emitting a pass its cell no longer produces, which is
what the engine does. The parity bound was also corrected: with five
instanced silhouettes, an edge pixel may legitimately swing a full channel, so
the gate bounds how many pixels may disagree rather than how far one edge
pixel may move, while every interior pixel must still match within 1e-4.

The green/refactor result is:

- 825 assertions in fourteen Phase 12 unit/trace cases.
- `contract.instanced_scene_frame` passes with validation enabled.
- 152/152 Debug CTest tests pass.
- 152/152 Release CTest tests pass.
- Vulkan core validation reports zero errors.
- Phase 11's `.vfscene`, `.ppm`, and `.vfgbuf` artifacts remain byte-identical
  under the version-minimal encoding rule.

One acceptance test was re-baselined by contract change: the Phase 3 trace
summary string gained a `registry-deltas=` counter. The golden trace bytes are
unchanged; only the human-readable diagnostic line grew.

## Replay and artifact evidence

The validation-enabled Debug and Release replays both report:

```text
extent=96x64 objects=2 instances=5 draws=2
resident-geometries=2 shared-instances=3 resident-bytes=8192
visible-before=5 visible-after=3 released-geometries=1
gbuffer-identity-mismatches=1 gbuffer-max-error=0.425
gbuffer-mean-error=1.01592e-05
interior=5199 interior-mismatches=0
transition-identity-mismatches=0
registry-deltas=52 plateau-bytes=8192 submissions=2
validation-errors=0
dedup=pass release=pass instancing=pass parity=pass plateau=pass trace=pass
```

Interpretation of the numbers:

- Five instances render from two objects and two draws. Three of the five are
  deduplicated aliases of already-resident content, so two geometries and
  8,192 bytes serve all five.
- The cell transition drops one cell: three instances survive with their
  identities unchanged, the removed cell's geometry is released, and the
  transitioned frame reproduces the oracle with zero identity mismatches.
- One silhouette pixel disagrees in the five-instance frame, and it is not an
  interior pixel: all 5,199 interior pixels match within 1e-4 on all twelve
  float channels and exactly on identity.
- Four further attach/detach cycles return to the same resident-byte and
  descriptor plateau, and the database settles back to the three surviving
  instances with one resident geometry.
- Fifty-two registry deltas were written to a `.vftrace` and re-inspected;
  the reader counted exactly the same number.

Artifacts:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `artifacts/phase-12/instanced-cell.vfscene` | 1,344 | `ADD9C3FDCD3699E650D19FC953314F40EAC2C8666761F50135A8C9DCB4ABE997` |
| `artifacts/phase-12/instanced-cell-release.vfscene` | 1,344 | `ADD9C3FDCD3699E650D19FC953314F40EAC2C8666761F50135A8C9DCB4ABE997` |
| `artifacts/phase-12/instanced-cell-debug.ppm` | 18,445 | `7495F6C88884480F7A579781ED814D95D7DD5A2BAB32D066DEF40349FB16C4E1` |
| `artifacts/phase-12/instanced-cell-release.ppm` | 18,445 | `7495F6C88884480F7A579781ED814D95D7DD5A2BAB32D066DEF40349FB16C4E1` |
| `artifacts/phase-12/instanced-cell-debug.vfgbuf` | 393,216 | `A68D118E879E787CC9700F5F5CFFB308F119387A0F0BE003E28AAA663724B307` |
| `artifacts/phase-12/instanced-cell-release.vfgbuf` | 393,216 | `A68D118E879E787CC9700F5F5CFFB308F119387A0F0BE003E28AAA663724B307` |
| `artifacts/phase-12/registry-deltas.vftrace` | 4,872 | `D754774CB67E1F2E847E4F0F1A9C492A27A9FFED9CF30AD18927847F1A940D07` |
| `artifacts/phase-12/registry-deltas-release.vftrace` | 4,872 | `D754774CB67E1F2E847E4F0F1A9C492A27A9FFED9CF30AD18927847F1A940D07` |
| `artifacts/phase-12/instanced-cell-debug.png` | 2,142 | rendered from the Debug PPM |
| `artifacts/phase-12/instanced-cell-gbuffer-debug.png` | 14,394 | four-quadrant G-buffer view |

Debug and Release produce byte-identical scene packets, color output, G-buffer
readbacks, and registry traces. The PNGs were visually inspected: five
instances render with distinct per-instance tints, the identity plane shows
five distinct false colors, and the depth plane separates the two cell depths.

Representative commands:

```powershell
cmake --build --preset vs2022-x64-debug
out/build/vs2022-x64-debug/tests/Debug/vf_unit_tests.exe "[phase12]"
ctest --test-dir out/build/vs2022-x64-debug -C Debug `
  -R "^contract\.instanced_scene_frame$" --output-on-failure
ctest --preset vs2022-x64-debug --output-on-failure

cmake --preset vs2022-x64-release
cmake --build --preset vs2022-x64-release
ctest --preset vs2022-x64-release --output-on-failure
```

## Promotion decision

The instance and streaming database, stable identity, content-hash
deduplication, generation exhaustion, descriptor quarantine, cell attach and
detach, upload cancellation, late-completion rejection, residency budgeting,
registry delta traces, versioned instance packet, GPU instancing, cell
transition replay, memory and handle plateau, and Debug/Release regression are
offline complete.

Phase 12 is not live-promoted. Its exit gate additionally requires a real
settlement or cell-transition capture from the engine, driven by observed
attach/detach and streaming events rather than a synthetic fixture. A
user-owned interactive Fallout 4 process was active during this checkpoint and
must not be attached to, stopped, or adopted. World draw suppression remains
disabled.
