# Phase 14 — Terrain, landscape layers, and LOD

Status: offline implementation complete; live capture and overlay gate pending

## Implemented slice

- `EngineTerrain` owns the landscape boundary. `.vfterrain` version 1.0 is a
  pointer-free, CRC-protected envelope with a 112-byte header and four aligned
  sections: 32-byte layer records, 128-byte cell records, 80-byte landscape
  vertices, and a 32-bit index stream.
- A captured vertex carries exactly eight blend channels, because the engine
  vertex layout delivers landscape data as two UNorm byte quads
  (`VertexSemantic::Landscape0` and `Landscape1`). The slot-to-layer mapping
  belongs to the **cell**, not the vertex, which is how the engine actually
  binds a cell's texture set.
- A cell declares `layerSlotCount`. Any channel outside that count must be
  zero; a non-zero one is land data we have not classified and the packet is
  refused with `UnclassifiedLandChannel` rather than interpreted. A slot that
  points outside the captured layer table is `MissingLayer`; two slots that
  resolve to the same layer are `LayerSlotMismatch`.
- Weights are normalized explicitly at evaluation time and the count of
  normalized vertices is reported, so captured channels that do not already
  sum to one can never silently darken a blend. Zero-sum, negative, and
  out-of-range channels fail closed.
- All three axes are cell relative. `originX`, `originY`, and `originZ` are
  doubles; vertex positions are floats confined to one cell and its declared
  height bounds. A vertex carrying an absolute world coordinate is refused
  with `VertexOutOfCell`. `BuildGpuTerrainCells` performs the subtraction
  against the captured camera origin in double and narrows only the small
  residual, which is what keeps a distant exterior free of camera-origin
  swimming.
- Cells tile their vertex and index sections contiguously, and a cell's
  triangles may only reference its own vertices (`IndexOutOfCell`), so a cell
  can never lose its diagnostic identity by borrowing another cell's geometry.
  Full-resolution cells must sit exactly on the engine's 4096-unit grid.
- `EvaluateTerrain` compares every shared world position across cells in
  double precision and reports seam checks, seam mismatches, the maximum seam
  gap, and separately the subset of those checks that cross a LOD boundary.
  This is the crack detector, and it is a property of the captured data rather
  than of a rendered image.
- `LodBlend` is monotonic and clamped over the captured morph range; an
  inverted or negative range is `InvalidLod`. The engine's `lodLevel` is
  recorded and never recomputed, so the mirror cannot disagree with vanilla
  culling by running a second guessed culler.
- `TerrainResidency` tracks the streamed footprint by cell. Reloading a
  resident cell replaces its footprint instead of accumulating, unloading an
  unknown cell is reported rather than ignored, and the exterior returns to a
  zero-byte baseline with the peak retained.
- The Vulkan mirror adds a terrain pipeline that writes the same five
  rasterization-ordered attachments as the Phase 11 scene pass and shares its
  depth buffer. Bindings 10 and 11 are the derived cell records and the
  captured layer table; binding 12 is a `sampler2DArray` of landscape layers.
  Cell records and layer records come from reflected SPIR-V, so their sizes
  and bindings are build-time assertions rather than a hand-maintained copy.
- `PrepareSampledTexture` now creates 2D array images. The slot and the
  captured dimension must agree, so a layer array can never be bound where a
  flat texture is sampled, and vice versa.
- Backend ABI minor 8 appends `terrainData`/`terrainSize` at offsets 160/168
  without moving the Phase 6–13 prefixes. The current request is 176 bytes.
  The backend refuses an incomplete field pair, an oversized payload, terrain
  without a captured view, a terrain packet that claims another frame or view,
  and a terrain frame that also carries a material bundle — because a terrain
  frame's captured texture *is* its landscape layer array.

## TDD/RGR evidence

The terrain contract began red on a missing header:

```text
EngineTerrainTests.cpp(1): fatal error C1083: Cannot open include file:
    'renderer_core/EngineTerrain.h': No such file or directory
```

The ABI extension began red on missing fields:

```text
BackendContractTests.cpp(111): error C2039: 'terrainData': is not a member of
    'vf::renderer::abi::RasterFrameRequestV1'
BackendContractTests.cpp(118): error C2065: 'kBackendAbiPhase14Minor':
    undeclared identifier
```

Three defects were found by the red suite and fixed in the code, not the
tests:

1. The first draft modelled four blend weights per vertex with a per-vertex
   layer index. The engine vertex layout carries eight UNorm channels and the
   cell owns the slot mapping, so the record was rebuilt against the real
   layout before any of it reached the GPU.
2. Cell height was initially absolute. With the fixture camera at a
   Commonwealth-scale origin this forced world-scale float heights — exactly
   the precision loss the design exists to avoid. `originZ` was added so all
   three axes are cell relative.
3. The terrain pipeline was created with `VK_FRONT_FACE_CLOCKWISE`. The oracle
   covered 35,289 pixels while the GPU covered zero, which is a culled front
   face, not a shading difference:

```text
terrain-replay ... terrain-pixels=0 expected-terrain-pixels=35289
  gbuffer-identity-mismatches=35289 result=fail
```

A fourth failure was a harness defect rather than a contract defect: the
submission helper reset the whole result struct, discarding the oracle image
that had been rendered into it, which surfaced as an infinite comparison
error. Only the submission half is reset now.

The green/refactor result is:

- 6,365 assertions in 12 Phase 14 unit cases.
- `contract.terrain_scene_frame` passes with validation enabled.
- 188/188 Debug CTest tests pass.
- 188/188 Release CTest tests pass.
- Vulkan core and synchronization validation report zero errors.

The refactor extracted `BlendLayers`, the single place where captured land
channels become a normalized layer blend, so the packet evaluator and the
reference rasterizer cannot drift from each other or from the fragment
shader's guarded division. Artifacts were byte-identical before and after.

One acceptance test was re-baselined by contract change: the Phase 13 ABI case
asserted that the current ABI minor equalled the Phase 13 minor. Phase 14
appends fields, so it now asserts the current minor is at least the Phase 13
minor while every Phase 13 offset stays exact, and pins
`offsetof(RasterFrameRequestV1, terrainData) == 160` so the append cannot move
the prefix. This is the same convention Phases 10 and 11 already use.

## Replay and artifact evidence

The validation-enabled Debug and Release replays both report:

```text
terrain-replay extent=256x192 cells=2 layers=3
terrain-pixels=35289 expected-terrain-pixels=35289
cell0-pixels=4692 cell1-pixels=30597
seam-checks=2 lod-seam-checks=2 seam-mismatches=0 max-seam-gap=0
gbuffer-identity-mismatches=0 gbuffer-max-error=8.1718e-05
gbuffer-mean-error=1.24918e-07
interior=46786 interior-mismatches=0
frame-mismatch-rejected=yes stray-layer-rejected=yes
validation-errors=0 result=pass
```

Interpretation of the numbers:

- The fixture camera sits at world `(2 000 000, 512, -1 000 000)` and the two
  cells straddle it on the exterior grid, so cell placement only works if the
  double subtraction happens before the narrowing to float.
- GPU and oracle terrain coverage agree exactly at 35,289 pixels. Coverage is
  compared as a count and per pixel, so a partially drawn cell cannot pass.
- Zero identity mismatches means every covered pixel carries the right cell id
  **and** the right dominant layer texture id. The second cell deliberately
  permutes its slot mapping, so a shader that ignored the per-cell mapping
  would report the wrong layer on 30,597 pixels.
- 46,786 interior pixels — every pixel whose 3×3 neighbourhood belongs to one
  surface — agree within 1e-3, with a worst channel error of 8.2e-05 and a
  mean of 1.2e-07 across the whole frame. Silhouette pixels are excluded
  because the oracle's coverage rule and hardware rasterization legitimately
  differ there.
- Two seam checks pass with a zero maximum gap, and both cross a LOD boundary
  because the second cell is captured at LOD 1. The shared edge is watertight
  across a near/far transition.
- Scene objects are submitted twenty times closer than the terrain in the same
  render pass, so the shared depth buffer has to order the two passes; they
  appear in front in the rendered frame.
- A terrain packet that claims another frame is refused through the public
  ABI, and a cell whose slot points outside the captured layer table is
  refused before it can sample an undefined array slice.

Artifacts:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `artifacts/phase-14/terrain-cells.vfterrain` | 1,152 | `04A70AE48C37660E719975D3EE706FAB9C1EAE2DB5ED20E37238953B2A70BB4B` |
| `artifacts/phase-14/terrain-cells-release.vfterrain` | 1,152 | `04A70AE48C37660E719975D3EE706FAB9C1EAE2DB5ED20E37238953B2A70BB4B` |
| `artifacts/phase-14/terrain-debug.ppm` | 147,471 | `C2A4574BC35A52355633AB2C20EDAB01C09AAA5A6455A1DC3012639EC703D8CD` |
| `artifacts/phase-14/terrain-release.ppm` | 147,471 | `C2A4574BC35A52355633AB2C20EDAB01C09AAA5A6455A1DC3012639EC703D8CD` |
| `artifacts/phase-14/terrain-debug.vfgbuf` | 3,145,728 | `BDCBBEA7575D01D190532DCD72FE3CBC23CCA3A140F261BA378BF249AB38DF89` |
| `artifacts/phase-14/terrain-release.vfgbuf` | 3,145,728 | `BDCBBEA7575D01D190532DCD72FE3CBC23CCA3A140F261BA378BF249AB38DF89` |
| `artifacts/phase-14/terrain-debug.png` | 11,885 | rendered from the Debug PPM |

Debug and Release produce byte-identical terrain packets, color output, and
G-buffer readbacks. The PNG was visually inspected: the narrow left cell and
the wide right cell carry visibly different layer colours because the second
cell permutes its slot mapping, the shared edge between them shows no crack,
and the three scene triangles sit in front of the terrain in the lower left.

### Fixture note

Each landscape layer occupies a single-texel array slice with a distinct flat
colour. That is a deliberate fixture choice: it removes any disagreement
between hardware and oracle texture-filtering rules from the comparison while
still making a wrong slice selection a wrong pixel. UV scale is carried
through the layer record and applied by both sides, and is verified exactly in
the unit suite rather than through the pixel comparison.

Representative commands:

```powershell
cmake --build --preset vs2022-x64-debug
out/build/vs2022-x64-debug/tests/Debug/vf_unit_tests.exe "[phase14]"
ctest --test-dir out/build/vs2022-x64-debug -C Debug `
  -R "^contract\.terrain_scene_frame$" --output-on-failure
ctest --preset vs2022-x64-debug --output-on-failure

cmake --build --preset vs2022-x64-release
ctest --preset vs2022-x64-release --output-on-failure
```

## Deferred within this phase

The refactor step calls for batching compatible cells into shared draws. The
mirror currently issues one indexed draw per cell with the cell index in a
push constant. That preserves per-cell diagnostic identity, which is the
non-negotiable half of the requirement, but it does not yet reduce draw count.
Instancing over cells that share a slot mapping and an index count is the
natural form, and it is not implemented here. This is a stated gap, not a
silent one.

## Promotion decision

The terrain packet, eight-channel land data, cell-owned slot mapping,
unclassified-channel rejection, explicit weight normalization, three-axis
cell-relative positions, contiguous cell tiling, seam and LOD-seam detection,
LOD morph blending, residency baseline, ABI extension, texture-array support,
terrain pipeline, shared depth ordering, and Debug/Release regression are
offline complete.

Phase 14 is not live-promoted. Its exit gate additionally requires captured
engine landscape data — real cell grids, layer tables, land channels, and LOD
decisions from a running exterior — and a split-view comparison against
vanilla across a near/far transition. Live capture is currently blocked on a
missing Address Library binary; see `journal.md`. Snow, wetness, and noise
inputs named in the phase slice are not part of the captured record yet and
are deferred to the material-family phases that own them. World draw
suppression remains disabled.
