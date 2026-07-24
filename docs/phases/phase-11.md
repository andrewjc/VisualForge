# Phase 11 — Opaque static raster scene mirror

Status: offline implementation complete; live capture and overlay gate pending

## Implemented slice

- `EngineScene` owns the multi-object boundary. `OpaqueObjectV1` is a 224-byte
  pointer-free record carrying stable object and material identity, the source
  draw index, the owning engine pass sequence, object flags, roughness,
  current and previous model transforms, local bounds, and separate geometric
  and shading normals.
- `.vfscene` is deterministic and checksummed. Version 1.0 uses magic
  `0x43534656` (`VFSC`), a 96-byte header, aligned object records, zero
  padding, an IEEE CRC over the payload, and at most 65,536 opaque objects.
- Validation fails closed on duplicate object identity, duplicate draw
  association, unknown or missing flags, zero identity, zero pass sequence,
  non-affine or singular transforms, inverted bounds, non-unit normals, and
  out-of-range roughness.
- `ValidateSceneAgainstRaster` requires a bijection between scene objects and
  raster draws, exact material association, and in-range triangle draw ranges.
- `ValidateSceneAgainstFrame` is the engine pass accounting seam. Each object
  must name a pass that exists in the mirrored view, writes the world target,
  and is classified `Opaque`. Every opaque world pass in that view must be
  mirrored, and any unclassified world-target writer makes the frame
  ineligible. `SceneCoverage` reports world-writing passes, opaque passes,
  mirrored passes, deferred classes, unknown world writers, and unmirrored
  opaque passes; `MirrorEligible()` is the arming predicate.
- Pass sequences wider than the 32-bit record field never match, so an
  oversized sequence stays unmirrored instead of aliasing another pass.
- `ProjectScenePacket` expands one shared draw list into per-object geometry
  by applying each model transform and the captured view-projection, reusing
  `EngineView` rather than duplicating coordinate conversion.
- `RenderReferenceGBuffer` is the CPU oracle. It rasterizes the projected
  scene with per-draw depth compare and front-face rules into an explicit
  64-byte G-buffer record: albedo, geometric normal plus roughness, shading
  normal plus depth, and object/material identity. `CompareGBuffer` reports
  differing pixels, identity mismatches, maximum absolute error, and mean
  absolute error.
- The Vulkan mirror renders the compatibility HDR color target plus four
  G-buffer planes (`R32G32B32A32_SFLOAT` x3 and `R32G32B32A32_UINT`) in one
  dynamic-rendering pass. Attachment writes are rasterization ordered, so
  overlapping opaque objects resolve by depth alone and submission order
  cannot change the result.
- Object records are uploaded to reflected storage binding 7 and read by both
  the scene vertex and fragment stages. A per-draw push constant selects the
  object, so one shared vertex/index buffer serves every instance of the
  shared draw list.
- The G-buffer planes are cleared to exactly the CPU oracle's initial world
  state, copied to a host-visible readback buffer, and interleaved into the
  reflected pixel record on copy-out. The interleave is validated at build
  time: four 16-byte planes must equal the 64-byte reflected pixel.
- The mirrored planes are created the first time a scene packet arrives and
  released with the extent they belong to, so Phase 6/8/9/10 replays do not
  pay for four extra render targets.
- Backend ABI minor 6 appends `sceneData`/`sceneSize` at offsets 96/104 and
  `gbufferData`/`gbufferCapacity` at offsets 112/120 without moving the Phase
  6/8/9/10 prefixes. The current request is 128 bytes.
- The backend fails closed on incomplete scene or G-buffer field pairs, scene
  packets without a captured view, G-buffer output without a scene, undersized
  G-buffer capacity, oversized scene payloads, decode failures, raster
  association failures, and any frame that is not mirror eligible.

## TDD/RGR evidence

The pass-accounting contract began red. The new case referenced
`OpaqueObjectV1::passSequence`, `SceneCoverage`, and
`ValidateSceneAgainstFrame` before they existed:

```text
EngineSceneTests.cpp(179): error C2039: 'passSequence' is not a member of
    'vf::renderer::scene::OpaqueObjectV1'
EngineSceneTests.cpp(211): error C2039: 'SceneCoverage' is not a member of
    'vf::renderer::scene'
EngineSceneTests.cpp(212): error C3861: 'ValidateSceneAgainstFrame':
    identifier not found
```

The Vulkan vertical contract was registered before replay support existed:

```text
contract.scene_raster_frame: Failed
vf_packet_replay: usage (unrecognized --render-scene)
```

With the fixture implemented but the backend still ignoring scene input, the
same gate failed on evidence rather than usage:

```text
gbuffer-differing=6144 gbuffer-identity-mismatches=377
interior-mismatches=5545 near-pixels=0 rotated-pixels=0
rejections=fail gbuffer=fail occlusion=fail result=fail
```

The first backend implementation wrote the G-buffer through a storage buffer.
That exposed a real defect rather than a test artifact: storage writes from
different draws to the same pixel are unordered, so reversing submission order
changed the mirror (`order-independent=no`). The G-buffer was converted to
real color attachments, which are rasterization ordered and depth tested.

The green/refactor result is:

- 7,031 assertions in six Phase 11 unit/ABI cases.
- `contract.scene_raster_frame` passes with validation enabled.
- 137/137 Debug CTest tests pass.
- 137/137 Release CTest tests pass.
- All seven GPU-labeled contracts pass, including the legacy raster, texture,
  material, captured-view, bridge, and probe slices.
- Vulkan core validation reports zero errors.

The refactor kept engine pass accounting in `EngineScene`, separate from
backend draw construction; reused `EngineView` for all coordinate conversion;
generated the object/G-buffer layout contract from SPIR-V reflection; and made
the mirrored attachments a lazy, extent-owned resource.

## Replay and artifact evidence

The validation-enabled Debug and Release replays both report:

```text
extent=96x64 objects=3
opaque-passes=2 mirrored-passes=2 deferred-classes=1 unknown-world-writers=0
gbuffer-differing=1316 gbuffer-identity-mismatches=0
gbuffer-max-error=4.2364e-05 gbuffer-mean-error=5.51457e-07
interior=5316 interior-mismatches=0 interior-max-error=3.97414e-05
color-differing=475 color-max-error=1
near-pixels=968 occluded-pixels=0 rotated-pixels=348
order-independent=yes rejections=pass submissions=2
validation-errors=0 result=pass
```

Interpretation of the numbers:

- Every one of the 1,316 differing pixels is a covered pixel differing only by
  interpolation rounding. Uncovered pixels reproduce the cleared world state
  exactly, and no pixel disagrees on object or material identity.
- 5,316 pixels are interior, meaning their whole 3x3 neighbourhood belongs to
  one object. All of them match the oracle within 1e-4 across all twelve float
  channels and exactly on identity. Only silhouette pixels are excused, and in
  practice none of them disagreed either.
- Object 1 projects strictly inside object 0 from twice the distance. Both the
  oracle and the mirror keep zero of its pixels, so full occlusion holds.
- Object 2 is yaw-rotated and partially occluded by object 0, so its depth
  varies across the triangle and its silhouette is cut by a nearer object.
- Reversing the scene object order reproduces a bit-identical G-buffer.

Rejections exercised through the public ABI, each leaving the submission count
unchanged at two: a forged duplicate object identity with a repaired checksum,
a scene without a captured view, a frame carrying an unclassified world-target
writer, and an undersized G-buffer capacity.

Artifacts:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `artifacts/phase-11/opaque-scene.vfscene` | 768 | `AEEE65B1F320DE92D44312E776BDEA28898E99A58153411C82C4B8822F530CEC` |
| `artifacts/phase-11/opaque-scene-release.vfscene` | 768 | `AEEE65B1F320DE92D44312E776BDEA28898E99A58153411C82C4B8822F530CEC` |
| `artifacts/phase-11/opaque-scene-debug.ppm` | 18,445 | `B0B98F27EF4D6B724637A37BACA4431603EB327603A6C42B279556FC83782428` |
| `artifacts/phase-11/opaque-scene-release.ppm` | 18,445 | `B0B98F27EF4D6B724637A37BACA4431603EB327603A6C42B279556FC83782428` |
| `artifacts/phase-11/opaque-scene-debug.vfgbuf` | 393,216 | `430372FF565F9ED2A3F3D90D055A97A043BD2B3EF9D1DB7387BD2B141AE3CC7C` |
| `artifacts/phase-11/opaque-scene-release.vfgbuf` | 393,216 | `430372FF565F9ED2A3F3D90D055A97A043BD2B3EF9D1DB7387BD2B141AE3CC7C` |
| `artifacts/phase-11/opaque-scene-debug.png` | 3,326 | `C3A13088ECA405D31A2ADE7F57C44AA95F125AC2B5D8A3ABDB879FD40AF83287` |
| `artifacts/phase-11/opaque-scene-gbuffer-debug.png` | 15,602 | `C1DDAEBCECD4D77DD5EBFB99DAF802974418FDC8966B7CC633FF1923C9D864A5` |

Debug and Release produce byte-identical scene packets, color output, and
G-buffer readbacks. `opaque-scene-debug.png` was visually inspected: the near
color-interpolated object and the rotated flat object are intact, the rotated
object is cut cleanly by the nearer silhouette, and the fully occluded object
is absent. `opaque-scene-gbuffer-debug.png` is a four-quadrant view of the
readback — albedo, shading normal, depth, and object identity with roughness
in blue — confirming each channel carries distinct per-object values.

Representative commands:

```powershell
cmake --build --preset vs2022-x64-debug
out/build/vs2022-x64-debug/tests/Debug/vf_unit_tests.exe "[phase11]"
ctest --test-dir out/build/vs2022-x64-debug -C Debug `
  -R "^contract\.scene_raster_frame$" --output-on-failure
ctest --preset vs2022-x64-debug --output-on-failure

cmake --preset vs2022-x64-release
cmake --build --preset vs2022-x64-release
ctest --preset vs2022-x64-release --output-on-failure
```

## Promotion decision

The pointer-free scene schema, strict validation, pass-to-packet accounting,
per-object transforms, ABI extension, Vulkan G-buffer attachments and
readback, CPU attachment/identity/depth oracle comparison, order independence,
fail-closed rejection paths, and Debug/Release regression are offline
complete.

Phase 11 is not live-promoted. Its exit gate additionally requires
engine-generated packets from the validated batch/accumulator seam, evidence
that every actual D3D draw touching the claimed opaque targets is associated
with a Vulkan packet or a declared unsupported class, and a captured static
scene matching vanilla transform, depth, identity, and material fixtures. A
user-owned interactive Fallout 4 process was active during this checkpoint and
must not be attached to, stopped, or adopted. World draw suppression remains
disabled.
