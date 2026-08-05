# Phase 18 — Acceleration structures and ray-traced shadows

Status: offline implementation complete, GPU vertical included; live capture
gate pending

## Implemented slice

- `EngineAcceleration` (`src/renderer_core/EngineAcceleration.h/.cpp`,
  namespace `vf::renderer::accel`): 80-byte `InstanceDescV1`, 48-byte
  `AccelPacketHeaderV1`, the `.vfas` packet, and `AccelSchedule`.
- **Build sizes round up to the device's alignments.** The live capability
  report gives `accelerationStructureScratchAlignment = 128`; scratch that is
  not aligned to it is a validation error at build time, not a slow path. A
  non-power-of-two alignment cannot be rounded to by masking and is refused.
- **A static structure reserves no update scratch.** It is never refitted, so
  the reservation would be pure waste.
- **Topology change forces a rebuild.** Refitting a structure whose triangle
  count changed produces one that no longer matches its geometry, and the
  corruption is silent: rays simply miss. Opacity is part of that comparison
  because it decides which hit groups run.
- **Only static structures are compacted.** Compacting one that is refitted
  every frame costs more than it saves, because it would have to be rebuilt
  to be compacted again.
- A **mirrored** instance is legal and keeps its winding reversal — the
  determinant sign is information, not an error. Only a **singular**
  transform is refused, along with a zero instance mask and a custom index
  above 24 bits, which truncates silently and points at the wrong material.
- **Transformed bounds transform all eight corners.** A rotated box's
  axis-aligned bounds are not its bounds rotated.
- **Alpha candidates resolve through `visibility::EvaluateCoverage`**, the
  same function the raster pass uses. Two alpha tests would drift, and a
  shadow silhouette disagreeing with the surface silhouette that produced it
  is the artefact this phase exists to avoid.
- **Blended geometry casts no ray-traced shadow** and is excluded from the
  occluder set rather than defaulting to opaque, which would put a solid
  shadow under every pane of glass.
- `ShadowTermAvailable()` is now **true** and `ShadowMaskRequired()` is
  false. Phase 17 declared the term unavailable so a parity metric could mask
  it; supplying it means the mask comes off, or a broken shadow would be
  hidden from every comparison that follows.

## One rule for the ray a light casts

`accel::ShadowRayForLight` and `vfShadowRayForLight` in
`shaders/phase17/lighting.glsl` are the same rule written twice, branch for
branch, and nowhere else:

- The origin is `OffsetRayOrigin(position, geometricNormal, 1.0)`, never the
  surface point. A ray starting on the surface re-hits the triangle that
  spawned it and every lit pixel shadows itself. The offset scales with the
  point's distance from the origin because float spacing does.
- A directional light has no position, so its ray reaches
  `kDirectionalShadowDistance` (1e6). The measured world camera reports a far
  plane near 353,468 units, so that clears the visible world without being
  infinite, which a builder cannot bound.
- A positional light's ray is measured **from the offset origin** and stops
  **at the light**. Anything past it is behind the light and cannot shadow
  the surface, so reaching further would invent occluders.
- A light on the offset origin leaves no direction to normalize; the ray
  comes back zero length rather than carrying a division by zero into the
  trace.

**Ambient is never shadowed.** It is not cast from anywhere, so no ray of it
can be blocked; shadowing it would black out the interior of every shadow
instead of leaving the ambient floor the engine shows. Both
`lighting::ShadeSurfaceGpu` and `vfShadeSurface` skip it explicitly.

## The GPU vertical

- The device enables `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`,
  `VK_KHR_deferred_host_operations`, and `bufferDeviceAddress`, and resolves
  the six extension entry points it needs. All of it is **conditional**: a
  device that does not advertise ray query still creates, and a missing entry
  point after a successful enable withdraws the capability instead of
  faulting on first use.
- **Ray query is a capability of the SPIR-V module, not a runtime branch.** A
  device without it cannot create a pipeline from a module that declares it,
  so `family_scene.frag` is compiled twice — once plainly and once with
  `-DVF_RAY_QUERY=1 --target-spv=spv1.4` — and the backend picks by what the
  device actually enabled.
- One bottom level holding **a geometry per drawn instance**, and one
  top-level structure holding a single identity instance. The builds are
  recorded into the frame's own command buffer: one submission, no second
  fence, and no command buffer that can be freed while still pending. The
  BLAS→TLAS and TLAS→fragment orderings are precise `VkMemoryBarrier2`
  dependencies on `ACCELERATION_STRUCTURE_BUILD`, not all-commands barriers.
- **The per-geometry transform is load-bearing.** `scene.vert` reads the
  packet's vertices as *local* space and multiplies them by the instance's
  model rows to reach the camera-relative space the fragment is shaded in. A
  structure built straight from those vertices describes a scene with every
  object collapsed onto the origin, and the shadows traced against it would
  be geometrically unrelated to the image. Vulkan's `transformData` applies
  the same model rows at build time. Rows 0–2 of the record's row-major 4×4
  are exactly a `VkTransformMatrixKHR`, so the numbers the vertex shader
  reads are the numbers the build applies.

## Fixture

`vf_packet_replay --render-family-scene --shadows` appends a small occluder
and the light it blocks on top of the Phase 16/17 scene. Appending rather
than editing the shared builders is deliberate: Phase 17's artifacts are
recorded by hash, and changing its scene would re-baseline them for a reason
unrelated to it. Its artifacts were re-verified byte-identical after this
phase.

The geometry that decides the test: the source triangles face −Z, so a light
in front of the objects lights them. The lamp sits at z 0.6, the occluder at
z 1.3, and object 0 receives at z 2.0, all offset in X so the occluder shades
object 0's middle while sitting beside it on screen rather than in front.

The light is authored in **world** space because `BuildGpuLight` narrows a
light against the camera origin. This exposed a real defect in the inherited
fixture: its two lights are authored camera-relative, so after narrowing they
land a whole camera origin away and fall outside their own radius,
contributing nothing at all. Phase 17's parity passed regardless because both
sides evaluate the same records — a reminder that agreeing about nothing is
still agreeing.

Measured, not assumed:

| Metric | Value |
| --- | ---: |
| Pixels the shadow term darkens in the reference | 7,278 |
| Largest darkening | 1.431 |
| Shadow-interior pixels compared | 41,981 |
| Shadow-interior mismatches | 0 |
| Largest shadow-interior error | 0.0104 |
| Validation errors | 0 |

A shadow boundary is a silhouette: the oracle resolves it by tracing from a
pixel centre the reference rasterizer chose, the ray query resolves it from
the centre hardware rasterization chose, and they disagree along the edge for
the same reason object silhouettes do. Every pixel whose whole 5×5
neighbourhood is on one side of the boundary and belongs to one object in
**both** images must agree; the boundary itself is counted and reported
rather than silently tolerated. 211 pixels differ in total, all within two
pixels of the boundary.

The interior bound is mixed absolute and relative, `1e-2 + 1e-3·|expected|`.
These are unbounded HDR values, so a fixed epsilon silently becomes a tighter
relative bound the brighter the light: the deepest interior disagreement is a
pixel that is **not shadowed at all**, 6.62024 against 6.61328 — two sides of
the same formula agreeing to 0.1% on a bright, close point light. The shadow
term is all-or-nothing and cannot hide inside that band, which the mutation
below confirms.

## Mutation evidence

| Mutation | Result |
| --- | --- |
| Shadow the ambient light too | `P18_shadowing_darkens_direct_light_and_never_ambient` fails, 2 assertions |
| Positional shadow ray does not stop at the light | `P18_a_light_casts_exactly_one_shadow_ray_rule` fails, 1 assertion |
| GPU shadow term always returns lit | `contract.shadowed_scene_frame` fails with 5,900 shadow-interior mismatches |

## Artifacts

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `artifacts/phase-18/shadowed-debug.ppm` | 147,471 | `C4764D90D7E3C1602FE906755478E1EB918963117A9942E2226E26BF74F104DD` |
| `artifacts/phase-18/shadowed-release.ppm` | 147,471 | `C4764D90D7E3C1602FE906755478E1EB918963117A9942E2226E26BF74F104DD` |
| `artifacts/phase-18/shadowed-debug.vffl` | 512 | `50FA96ED43E3D1B360BAEE89C83270575AC6423001396CD86C91D3534782BCE3` |
| `artifacts/phase-18/shadowed-release.vffl` | 512 | `50FA96ED43E3D1B360BAEE89C83270575AC6423001396CD86C91D3534782BCE3` |
| `artifacts/phase-18/shadowed-debug.vfscene` | 992 | `299ED2790D3871DE624D7DF10BA19180F305144BFBB44F8415C7FFFE17F36A5D` |
| `artifacts/phase-18/shadowed-release.vfscene` | 992 | `299ED2790D3871DE624D7DF10BA19180F305144BFBB44F8415C7FFFE17F36A5D` |
| `artifacts/phase-18/shadowed-debug.vfgbuf` | 3,145,728 | `88C8B134856200155439F7C8ADE26909133CFF260BE3BCB40F4DDEDC7D15625A` |
| `artifacts/phase-18/shadowed-release.vfgbuf` | 3,145,728 | `88C8B134856200155439F7C8ADE26909133CFF260BE3BCB40F4DDEDC7D15625A` |

Debug and Release are byte-identical across all four. 269/269 tests pass in
both configurations, and the Phase 17 colour, light-packet, and G-buffer
artifacts are unchanged.

**The colour hash was re-baselined once, by Phase 19.** That phase adds a
reflection term to every lit surface, so the shadow fixture.'s colour output
legitimately moved (`DDF7F2AE...` to `C4764D90...`) while its scene, light,
and G-buffer artifacts stayed byte-identical -- the G-buffer carries albedo
and normals, not the reflected radiance. The shadow contract itself still
passes unchanged: 7,278 pixels shadowed, 41,981 interior pixels compared,
zero mismatches. A later phase making the renderer do more is a re-baseline,
not a regression, and saying which is which is the point of recording the
hashes at all.

## In-game scene

Captured on 2026-08-16 through `tools/game_smoke/Export-PhaseScene.ps1`, with
the game launched by the live capture harness and the Vulkan mirror enabled.
`vulkanMirrorDisplayed` and `worldReached` both true, plugin SHA-256
`1C79BECD506DD0E463DAE686C01FDCA397183C8AC646FD151EF1BB82CAE8B262`.

| Image | What it is |
| --- | --- |
| `artifacts/phase-18/scene-live-slot4.png` | 1280x720 frame the Vulkan renderer produced inside the running game, driven by the live world camera (slot 4, the camera with the 353,468-unit far plane). |
| `artifacts/phase-18/scene-live-slot0.png` | The same, for the engine's other live camera slot. |
| `artifacts/phase-18/scene-mesh.png` | A 2048x2048 landscape mesh and its texture, both captured from the running game, replayed through the Vulkan backend. 289 vertices, 1,536 indices, 7 attributes, zero validation errors. |
| `artifacts/phase-18/game-window.png` | A screenshot of the game window, for comparison. Not renderer evidence. |
| `artifacts/phase-18/shadowed-debug.png` | The offline shadow fixture, showing the occluder and the shadow it casts. |

The live frames draw the mirror's own geometry with the real camera. Feeding a
whole visible cell to the backend needs the per-frame world capture the live
promotion gates still owe, so `scene-mesh.png` is the closest thing to real
in-game content through the replacement renderer that exists today — one
mesh, not a scene. Saying so is the point: the picture would otherwise imply a
completeness this phase has not reached.

## What this phase cost, and why it is written down

Two bugs in this vertical were invisible to validation and cost most of the
session. Both were found by bisection after hypothesising had found nothing,
and both are recorded in `journal.md` as pitfalls.

1. `triangles.indexType` was hardcoded to `VK_INDEX_TYPE_UINT32` while the
   packet's default is `Uint16`. The layers pass it — the build is legal, the
   addresses are legal, the sizes are legal — and the GPU reads `{0,1,2}` as
   65536, fetches a vertex two megabytes past a 436-byte buffer, and the
   submission dies as `VK_ERROR_DEVICE_LOST` on the fence wait.
2. `DestroyAccelerationStructures()` was called from the middle of a build to
   resize the bottom-level storage, and it also destroyed the transform
   buffer that had been created and filled a few lines above. Every geometry's
   `transformData` then pointed at freed memory; geometry 0 survived on
   whatever the allocator had not reused yet and the rest were flung out of
   the scene, so exactly one object was ever traceable and no shadow appeared.

## Not in this phase

- No live capture gate. The camera is found and the renderer draws in-engine,
  but a captured occluder set has not been promoted yet.
- The bottom level is rebuilt every frame. `DecideBuild` and `ShouldCompact`
  exist and are tested, but the backend does not yet consult them.
- One instance per drawn object with no BLAS sharing. Real reuse needs stable
  geometry identity across frames, which arrives with streaming.
- Cutout occluders are flagged `VK_GEOMETRY_OPAQUE_BIT_KHR` in the backend
  because this fixture carries none. `RequiresAnyHit` and
  `ConfirmAlphaCandidate` decide the cutout path and are tested on the CPU;
  wiring an any-hit shader to them is outstanding.
