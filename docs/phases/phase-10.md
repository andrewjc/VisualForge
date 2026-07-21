# Phase 10 — Camera, view, frame, and pass capture

Status: offline implementation complete; live camera/pass-capture and overlay gate pending

## Implemented slice

- `EngineView` is the single coordinate-conversion boundary. It normalizes
  source matrix storage and vector convention once, supports left- and
  right-handed perspective/orthographic D3D zero-to-one projections, and
  applies exactly one D3D-to-Vulkan Y conversion.
- `CapturedView` preserves stable view/camera identity, projection mode,
  handedness, render mode/target, extent, render scale, AA mode, clip planes,
  viewport, scissor, camera-relative current/previous origins, jitter, and
  current/previous transforms.
- Translation reconstructs view-projection, inverse, previous, and
  unjittered matrices. Validation rejects non-finite values, singular or
  inconsistent matrices, invalid FOV/clip planes, malformed extent/state,
  unknown flags, and nonzero reserved fields.
- `ViewHistoryTracker` owns explicit discontinuity causes. First observation,
  cuts, teleports, loads, worldspace/time changes, skipped frames, identity,
  projection, clip, extent, scale, AA, special-view, bridge, and fault changes
  cannot become an untracked temporal reset. A transition increments the
  epoch exactly once; stale frames and wrong previous transforms fail closed.
- Pass records preserve shader domain, category, technique, render mode,
  target, flags, sequence, and view ownership. Categories are derived by one
  classifier; unknown world-target writers remain visible and block takeover.
- `.vfframe` is pointer-free and deterministic. Version 1.0 uses magic
  `0x57564656` (`VFVW`), a 96-byte header, 640-byte view records, 40-byte pass
  records, aligned sections, zero padding, and an IEEE CRC over the payload.
  The schema permits at most 16 views and 65,536 passes.
- Backend ABI minor 5 appends `frameData` and `frameSize` at offsets 80 and 88
  without changing the Phase 6/8/9 request prefix. The current request is 96
  bytes; older callers receive a disabled identity view record.
- Shader reflection fixes binding 6 as a 240-byte vertex-stage uniform record.
  Its offsets are current VP 0, previous VP 64, unjittered VP 128,
  clip/jitter 192, viewport 208, identifiers 224. The reflected layout hash is
  `0xC34D6F6AB1E8B527`.
- The Phase 6 mesh vertex shader calls `vfTransformPosition`. Identity records
  retain legacy NDC behavior; captured records transform camera-relative
  positions through the supplied view-projection matrix.
- The Vulkan backend accepts at most 16 MiB of frame data, requires one view
  for this slice, verifies frame ownership and exact raster extent/viewport/
  scissor agreement, uploads binding 6, and labels captured-view submissions.
- `vf_packet_replay` supports `--frame`, `--view-fixture`, and
  `--frame-output`. The fixture starts from desired NDC coverage, unprojects
  it into non-NDC camera-relative vertices, re-encodes the raster packet, and
  projects only a CPU-oracle copy. Vulkan therefore has to consume the frame
  packet and execute the vertex transform to match the reference.
- Frame options combined with multi-fixture lifecycle replay are rejected
  because one frame cannot truthfully own several frame IDs/extents. Frame
  transforms on normalized captured-mesh translation are also explicitly
  rejected rather than silently ignored; that path remains a live/captured
  scene concern for Phase 10/11 promotion.

## TDD/RGR evidence

The pure view/frame suite began red with deliberate stubs. All eight Phase 10
cases failed across 82 assertions for the intended missing matrix,
projection, validation, history, classification, and serialization behavior.

The Vulkan vertical contract was then registered before replay support:

```text
contract.view_raster_frame: Failed
vf_packet_replay: usage (unrecognized --frame-output/--view-fixture)
```

The green/refactor result is:

- 155 assertions in nine Phase 10 unit/ABI cases.
- `contract.view_raster_frame` passes with validation enabled.
- 130/130 Debug CTest tests pass.
- 130/130 Release CTest tests pass.
- All six GPU-labeled contracts pass, including legacy raster, texture,
  material, bridge/probe, and the captured-view slice.
- Vulkan core validation reports zero errors.
- Debug and Release serialize byte-identical frame packets and render
  byte-identical PPM output.

The central refactor kept conversion in `EngineView`, moved the CPU projection
oracle into `ProjectPacketForView`, generated the shader layout from SPIR-V
reflection, retained identity constants for historical ABI callers, and kept
frame decoding/validation ahead of GPU submission.

## Replay and artifact evidence

The validation-enabled Debug and Release replays both report:

```text
submission=1 extent-generation=1
differing=240 max-error=1 mean-error=0.0102946
probes=pass tolerance=pass
view=captured source=camera-relative
validation-errors=0 result=pass
```

Artifacts:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `artifacts/phase-10/synthetic-perspective.vfframe` | 776 | `E6E556D46F6F167773AC9AA4CB7419ED8E213AD7AAE1F5159105DE2677FA0DED` |
| `artifacts/phase-10/synthetic-perspective-release.vfframe` | 776 | `E6E556D46F6F167773AC9AA4CB7419ED8E213AD7AAE1F5159105DE2677FA0DED` |
| `artifacts/phase-10/synthetic-perspective-debug.ppm` | 18,445 | `812ECC0D640EB86542BE310CCEF988C4FE60446523EB910AE711C44EDF56F426` |
| `artifacts/phase-10/synthetic-perspective-debug-replay.ppm` | 18,445 | `812ECC0D640EB86542BE310CCEF988C4FE60446523EB910AE711C44EDF56F426` |
| `artifacts/phase-10/synthetic-perspective-release.ppm` | 18,445 | `812ECC0D640EB86542BE310CCEF988C4FE60446523EB910AE711C44EDF56F426` |
| `artifacts/phase-10/synthetic-perspective-debug.png` | 4,884 | `AB127BAFCEC06382BE1E04DA8013F73465B78CC676F006002165D6EE0B7835AE` |

The saved `.vfframe` was loaded through `--frame` in a separate Debug replay;
its output is byte-identical to the fixture-generation run. The PNG was
visually inspected: the expected single color-interpolated triangle is intact
against the dark clear color, with no clipping, corruption, or unintended
orientation change.

Representative commands:

```powershell
cmake --build --preset vs2022-x64-debug
out/build/vs2022-x64-debug/tests/Debug/vf_unit_tests.exe "[phase10]"
ctest --test-dir out/build/vs2022-x64-debug -C Debug `
  -R "^contract\.view_raster_frame$" --output-on-failure
ctest --preset vs2022-x64-debug --output-on-failure

cmake --preset vs2022-x64-release
cmake --build --preset vs2022-x64-release
ctest --preset vs2022-x64-release --output-on-failure
```

## Promotion decision

The pointer-free frame/view/pass schema, strict validation, coordinate
conversion, temporal history contract, reflected GPU constants, ABI extension,
Vulkan upload/shader path, serialized replay, CPU/GPU comparison, and
Debug/Release regression are offline-complete.

Phase 10 is not live-promoted yet. Its exit gate requires a build-gated capture
from the mapped `BSGraphics::State`/camera/accumulator boundaries, comparison
against vanilla world-to-screen/depth observations, current/previous motion
fixtures, and a split-view overlay of a real captured mesh. A user-owned
interactive Fallout process was active during this checkpoint and must not be
attached to, stopped, or adopted. World draw suppression remains disabled.

The next safe work is Phase 11's offline multi-object opaque-scene slice while
the Phase 8-10 bounded live gates remain queued for a controlled game session.
