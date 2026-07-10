# Phase 6 — First replayed Vulkan raster frame

Status: complete

## Implemented slice

- ABI minor 2 appends optional raster-session create, frame-render, destroy,
  and status functions after the stable Phase 5 prefix. Phase 4 and Phase 5
  minor constants remain explicit so older optional surfaces stay negotiable.
- A versioned, pointer-free raster packet with checked byte offsets for the
  header, vertices, 16- or 32-bit indices, draws, and materials.
- Pre-submit diagnostics for magic/version/size, section range and alignment,
  extent, viewport, scissor, draw range, index range, degenerate triangles,
  missing or duplicate stable resources, and shader-layout mismatch.
- A shared material registry and an aligned upload arena. Both the CPU oracle
  and Vulkan command recorder resolve the same stable material IDs.
- A four-pass declared frame graph: upload, opaque HDR/depth raster, tone map,
  and readback. Its producer ordering and five access transitions are tested.
- A persistent exact-LUID Vulkan 1.3 raster session with synchronization2,
  dynamic rendering, explicit fences, debug labels, and no queue/device idle
  in the normal frame path.
- R16G16B16A16 floating-point HDR and D32 depth images, followed by a separate
  full-screen tone-map pass into R8G8B8A8 UNORM and a transfer readback used
  only by the standalone replay tool.
- Six raster pipeline variants cover both front-face conventions and Less,
  Less-or-equal, and Always depth comparison without implicit state guessing.
- One dynamic material UBO descriptor and generated std140 constants. CMake
  compiles four GLSL shaders to Vulkan 1.3 SPIR-V, embeds them, invokes
  SPIR-V-Reflect, and fails the build if set 0/binding 0 or the 16-byte block
  layout drifts.
- Extent-owned HDR/depth/output/readback resources recreate only when the
  packet dimensions change. Pipelines, layouts, sampler, pool, command pool,
  and device remain live across resize.
- `vf_packet_replay --render-synthetic` selects the same adapter as D3D11,
  submits packets through the DLL ABI, checks exact probes and a tolerance
  golden, writes a P6 PPM, and reports validation/retirement state.

## TDD/RGR evidence

The initial pure skeleton deliberately returned empty packets, an empty frame
graph, and unsupported reference output. The first red run produced 12 failing
cases and 31 intended failing assertions. It covered winding, index width,
viewport/scissor, corrupt ranges, missing resources, reflected layout, depth,
exact probes, tolerance comparison, upload alignment, resize, and frame-graph
ordering.

The ABI minor-2 host surface had a separate red cycle: both forwarding cases
failed while `RasterAvailable` and the methods returned the deliberate
unavailable result. A final resource-registry red case proved that two
materials could initially reuse one stable ID; decoding returned the later
missing-material symptom until duplicate detection was moved ahead of draw
resolution.

The first real GPU golden also found a non-synthetic defect. Submission,
retirement, and validation were clean, but every triangle pixel remained at
clear color. The mismatching pixel count matched the triangle footprint.
Packet winding is classified in mathematical NDC, while a positive-height
Vulkan viewport reverses the framebuffer front-face convention. The explicit
translation was corrected; the golden was not weakened.

Final focused result:

- 104 assertions across 16 Phase 6 cases.
- 77/77 Debug CTest tests passed.
- 77/77 Release CTest tests passed.
- Plugin and renderer export contracts still expose exactly their intended
  entry points.
- Capability, 64-exchange bridge, and raster GPU contracts all pass in both
  configurations.

## GPU promotion gate

The validation-enabled Debug replay reported:

    raster-malformed result=rejected submission=0 diagnostic=bad-magic
    raster-fixture name=base extent=96x64 index=16 submission=1 extent-generation=1 differing=240 max-error=1 mean-error=0.0102946 probes=pass tolerance=pass
    raster-fixture name=uint32 extent=96x64 index=32 submission=2 extent-generation=1 differing=240 max-error=1 mean-error=0.0102946 probes=pass tolerance=pass
    raster-fixture name=depth-occlusion extent=96x64 index=16 submission=3 extent-generation=1 differing=240 max-error=1 mean-error=0.0102946 probes=pass tolerance=pass
    raster-fixture name=clockwise extent=96x64 index=16 submission=4 extent-generation=1 differing=240 max-error=1 mean-error=0.0102946 probes=pass tolerance=pass
    raster-fixture name=resize extent=112x72 index=16 submission=5 extent-generation=2 differing=220 max-error=1 mean-error=0.00691344 probes=pass tolerance=pass
    raster-fixture name=resize-stable extent=112x72 index=32 submission=6 extent-generation=2 differing=220 max-error=1 mean-error=0.00691344 probes=pass tolerance=pass
    raster-fixture name=resize-restore extent=96x64 index=16 submission=7 extent-generation=3 differing=240 max-error=1 mean-error=0.0102946 probes=pass tolerance=pass
    raster-lifecycle submissions=7 extent-generation=3 expected-generation=3 result=pass
    raster-replay fixtures=7 validation-errors=0 unload=deferred result=pass

The small nonzero difference count consists only of one-code-value half-float
and interpolation rounding; maximum channel error is 1. The corners match
exactly, the center probe is bounded, and a missing/flipped triangle fails the
mean-error and footprint tolerances by a large margin.

## Promotion decision

Phase 6 is promoted. Phase 7 may feed captured engine vertex/index payloads
into the same validated packet, registry, upload, and Vulkan draw path. It must
first prove the current engine buffer-creation boundary and every observed
vertex layout; unsupported layouts remain observable diagnostics and cannot
enable draw suppression.
