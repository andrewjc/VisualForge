# Phase 13 — Skinning, morphs, wind, and dynamic geometry

Status: offline implementation complete; live capture and overlay gate pending

## Implemented slice

- `EngineDeformation` owns the deformation boundary. `.vfdeform` version 1.0
  is a pointer-free, CRC-protected envelope with a 144-byte header and five
  aligned sections: 48-byte influence records, current and previous 3x4 bone
  palettes, 16-byte morph targets, and 32-byte sparse morph deltas. Wind
  parameters live in the header.
- Influence weights are normalized explicitly at evaluation time and the
  count of normalized vertices is reported, so a mesh whose supplied weights
  do not sum to one can never silently scale its geometry. Zero, negative, or
  non-finite weights, out-of-palette bone indices, singular bone matrices,
  mismatched previous palettes, out-of-range morph weights, out-of-range
  delta vertices, and negative wind amplitude or frequency all fail closed.
- The evaluation order is fixed and identical on both sides: morph
  accumulation in bind space, then skinning through the palette, then wind
  displacement scaled by per-vertex flexibility. The previous pose runs the
  same order with the previous palette, previous morph weights, and previous
  wind time, so motion is a property of the contract rather than of a
  renderer's frame history.
- Deformed bounds cover both poses. The mirrored bounds are the union of the
  current and previous extents, so motion can never leave the region reserved
  for the object, and the maximum per-vertex motion magnitude is reported.
- `TopologyRegistry` enforces the phase rule: fixed topology updates in
  place, while a changed vertex, bone, or delta count requires a new
  generation. A generation may not move backwards.
- `DynamicRing` sub-allocates deformation output with alignment, wraps to the
  front when the tail cannot serve a request, and refuses any range that
  overlaps bytes an earlier submission is still reading. A completed timeline
  value retires every earlier allocation.
- The Vulkan mirror runs a reflected compute kernel over eight storage
  bindings, dispatching one workgroup per 64 vertices before the render pass.
  A buffer barrier publishes the deformed stream from
  `COMPUTE_SHADER`/`SHADER_STORAGE_WRITE` to
  `VERTEX_ATTRIBUTE_INPUT`/`VERTEX_ATTRIBUTE_READ` and to `TRANSFER_READ`, and
  the raster pass binds the compute output as its vertex buffer instead of the
  packet's bind-pose stream.
- Backend ABI minor 7 appends `deformationData`/`deformationSize` at offsets
  128/136 and `deformationOutputData`/`deformationOutputCapacity` at 144/152
  without moving the Phase 6-12 prefixes. The current request is 160 bytes.
  The backend rejects incomplete field pairs, an output without a packet, an
  oversized payload, a vertex-count mismatch against the raster stream, and
  any topology change that reuses its generation.
- The copy-out interleaves current and previous positions into 32-byte
  records so motion can be verified directly instead of being inferred from
  the rendered image.

## TDD/RGR evidence

The deformation contract began red on a missing header:

```text
EngineDeformationTests.cpp(1): fatal error C1083: Cannot open include file:
    'renderer_core/EngineDeformation.h': No such file or directory
```

The Vulkan vertical contract was registered before replay support existed:

```text
contract.deformed_scene_frame: Failed
vf_packet_replay: usage (unrecognized --render-deformed-scene)
```

One test encoded a wrong expectation and was corrected rather than the code:
a completed GPU timeline value retires every earlier ring allocation, not
only the allocation with that exact value.

```text
EngineDeformationTests.cpp(270): FAILED:
  CHECK( ring.InFlightBytes() == 64 )
with expansion:
  0 == 64
```

The green/refactor result is:

- 69 assertions in seven Phase 13 unit cases.
- `contract.deformed_scene_frame` passes with validation enabled.
- 160/160 Debug CTest tests pass.
- 160/160 Release CTest tests pass.
- Vulkan core validation reports zero errors, including the compute-to-
  graphics dependency.

One acceptance test was re-baselined by contract change: the Phase 11 ABI case
asserted that the current ABI minor equalled the Phase 11 minor. Later phases
append fields, so it now asserts the current minor is at least the Phase 11
minor while every Phase 11 offset stays exact — the same convention the Phase
10 case already used.

## Replay and artifact evidence

The validation-enabled Debug and Release replays both report:

```text
extent=96x64 frames=6 vertices=3
max-position-error=5.96046e-08 max-motion-error=5.96046e-08
reference-motion=0.100304
gbuffer-identity-mismatches=0 gbuffer-mean-error=4.56666e-07
interior=5496 interior-mismatches=0
topology-rejected=yes generation-accepted=yes
submissions=7 validation-errors=0
```

Interpretation of the numbers:

- Six consecutive frames each change the bone pose and wind time. The backend
  receives only the bind pose and the deformation packet, while the oracle
  rasterizes the CPU-deformed stream, so a skipped dispatch or a stale ring
  range cannot pass by reproducing an earlier frame.
- GPU and CPU deformed positions agree to 6e-08, far inside the declared 1e-4
  tolerance, and the per-axis motion between the current and previous pose
  agrees to the same bound, so motion vectors carry the correct sign and
  magnitude.
- The rendered G-buffer has zero identity mismatches and zero interior
  mismatches against the deformed reference.
- A grown topology that reuses its generation is refused through the public
  ABI; the same topology under a new generation is accepted.

Artifacts:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `artifacts/phase-13/deformed-pose.vfdeform` | 528 | `AAC67C47CE9D8C21FFB7FA50C90FB7A2BD71EED78448B42832809780C3702EAB` |
| `artifacts/phase-13/deformed-pose-release.vfdeform` | 528 | `AAC67C47CE9D8C21FFB7FA50C90FB7A2BD71EED78448B42832809780C3702EAB` |
| `artifacts/phase-13/deformed-pose-debug.ppm` | 18,445 | `7E1427DE4737DCDA1371ACD188389202143D366290D88EE590A819EFD5818CD2` |
| `artifacts/phase-13/deformed-pose-release.ppm` | 18,445 | `7E1427DE4737DCDA1371ACD188389202143D366290D88EE590A819EFD5818CD2` |
| `artifacts/phase-13/deformed-pose-debug.vfgbuf` | 393,216 | `CCF50E281BCCB9DD28A402FD4144F32C8417D135E02991996C9C652DD006FB25` |
| `artifacts/phase-13/deformed-pose-release.vfgbuf` | 393,216 | `CCF50E281BCCB9DD28A402FD4144F32C8417D135E02991996C9C652DD006FB25` |
| `artifacts/phase-13/deformed-pose-debug.png` | 2,704 | rendered from the Debug PPM |

Debug and Release produce byte-identical deformation packets, color output,
and G-buffer readbacks. The PNG was visually inspected: the apex is lifted by
its bone, the third vertex is both lifted by its blended influence and slid by
the morph, and the silhouette is asymmetric with respect to the bind pose.

Representative commands:

```powershell
cmake --build --preset vs2022-x64-debug
out/build/vs2022-x64-debug/tests/Debug/vf_unit_tests.exe "[phase13]"
ctest --test-dir out/build/vs2022-x64-debug -C Debug `
  -R "^contract\.deformed_scene_frame$" --output-on-failure
ctest --preset vs2022-x64-debug --output-on-failure

cmake --preset vs2022-x64-release
cmake --build --preset vs2022-x64-release
ctest --preset vs2022-x64-release --output-on-failure
```

## Promotion decision

The deformation packet, explicit weight normalization, fixed evaluation
order, current and previous poses, union bounds, topology generations,
dynamic ring safety, ABI extension, compute kernel, compute-to-raster
barrier, deformed vertex input, motion readback, and Debug/Release regression
are offline complete.

Phase 13 is not live-promoted. Its exit gate additionally requires captured
engine deformation inputs — real bone palettes, morph weights, and wind state
— and a split-view comparison of characters and animated fixtures against
vanilla. Deformed shading normals also remain per-object until a later phase
consumes them. A user-owned interactive Fallout 4 process was active during
this checkpoint and must not be attached to, stopped, or adopted. World draw
suppression remains disabled.
