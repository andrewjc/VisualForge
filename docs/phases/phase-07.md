# Phase 7 — Engine mesh and buffer capture

Status: complete

## Implemented slice

- A checked Fallout 4 `VertexDesc` parser and raw stream codec. The low nibble
  supplies the static stride in four-byte units, the next nibble supplies the
  dynamic stride, attribute offsets are four-byte nibbles, and flags occupy
  bits 44–54.
- Exact storage translation for HALF4 position, HALF2 UV0, HALF4 UV1,
  engine basis bytes mapped from UNORM to `[-1, 1]`, UNORM8 color, HALF4 skin
  weights plus four byte indices, two UNORM8 landscape vectors, and float eye
  data.
- Strict rejection of unknown flags, missing position/offsets, overlapping or
  out-of-stride attributes, truncated/non-finite streams, invalid draw ranges,
  out-of-range vertices, and degenerate topology.
- A versioned, pointer-free `.vfmesh` container with fixed 112-byte header,
  checked sections, explicit resource generation and usage, draw range/base
  vertex, and payload CRC-32.
- A captured-mesh translator that compacts the referenced draw range, applies
  the base vertex, computes source bounds, selects winding, projects the source
  diagnostically, and emits the validated Phase 6 raster packet.
- A stable address-plus-generation resource registry. Duplicate creation,
  update-before-create, stale handles, double destroy, pointer reuse, and
  destruction before the last in-flight timeline value are explicit errors.
- A complete build-specific contract for all 52 slots of the resource-manager
  vtable at RVA `0x29139A8` and singleton pointer slot RVA `0x3438128`.
- Opt-in one-shot game capture through
  `VISUALFORGE_CAPTURE_MESH_ONCE=1`. When the singleton exists, a cloned
  52-slot vtable intercepts slots 4, 7, 40, and 41. At early startup, where the
  singleton is still null, the verified concrete `Renderer::CreateTriShape`
  boundary at RVA `0x1818760` is used instead.
- The concrete hook copies the packed vertex stream and `IndexBuffer` CPU data
  before calling the original exactly once. All copies are capped, guarded,
  and atomically published through a temporary file. Every path leaves draw
  suppression off.
- `vf_packet_replay --render-mesh` decodes a captured file, generates the
  diagnostic raster packet, renders a CPU oracle and the exact-adapter Vulkan
  result, compares them, writes PPM output, and checks validation and session
  retirement.

## Reverse-engineering evidence

Disassembly of the exact 1.11.221 executable established the concrete function
shape:

```text
TriShape* __fastcall Renderer::CreateTriShape(
    Renderer*, uint32_t* dataSize, void* packedVertices,
    uint64_t vertexDesc, IndexBuffer*)
```

The body derives stride as `(vertexDesc * 4) & 0x3C`, creates the vertex
buffer, references the supplied index buffer, and constructs the `TriShape`.
`IndexBuffer + 0x08` is the retained CPU data pointer and `+0x34` is its
current byte size. The existing hook manifest verifies the first 12 code bytes
at RVA `0x1818760` before MinHook can prepare this detour.

The engine `PackVertexData`/`UnpackVertexData` bodies corroborated all storage
conversions. The live descriptor added to the parity corpus is:

```text
VertexDesc  0x000BB00605430208
stride      32 bytes
attributes  position, UV0, normal, tangent, color, landscape0, landscape1
```

## TDD/RGR evidence

The red suite first exercised six descriptor shapes, 257 seeded randomized
vertices per layout, raw byte repacking, malformed layouts/streams, mesh
container corruption, draw-range/base-vertex/winding translation, resource
event ordering, generation reuse, retirement, and all resource-manager slots.
The production stubs deliberately returned unsupported or empty results.

The first live attempts found that the resource-manager singleton is not
published at plugin load or first Present. That observation promoted the
already-disassembled concrete creation boundary into a separately manifested
fallback. No timing loop or unchecked address was introduced. A later parity
run incorporated the exact descriptor observed by that fallback.

Final focused result:

- 16,850 assertions across 12 Phase 7 cases.
- 13/13 focused Phase 7 plus hook-manifest CTest cases passed.
- 89/89 Debug CTest tests passed.
- 89/89 Release CTest tests passed.
- Export, Vulkan capability, interop bridge, and raster GPU contracts remain
  green in both configurations.

## Live capture gate

Artifact directory:

    artifacts/phase-07/capture-20260815-204934

The bounded Fallout run reported:

```text
hook-manifest: accepted validated=6 failed-site=0 reason=none
renderer-health schema=1 mode=Off backend=absent suppression=off
renderer-mesh-capture: singleton unavailable; selecting verified concrete boundary suppression=off
renderer-mesh-capture: armed boundary=concrete rva=0x01818760 hooks=1 suppression=off
renderer-mesh-capture: observed desc=0x000BB00605430208 stride=32 layout=none attributes=7 suppression=off
renderer-mesh-capture: complete resource=9056614266584543940 generation=1 stride=32 vertices=289 indices=1536 attributes=7 bounds-min=-2048,-2048,-580 bounds-max=0,0,348 winding=counter-clockwise suppression=off
```

The harness reached the game, observed the capture, exited normally, restored
the staged files with no restore errors, and restored the installed plugin SHA:

    FCBB0032169C05BF992F77F96A6F3B5EA6C8AD7A5840DB8F516FCE90278FB29F

## Captured Vulkan replay gate

The 12,432-byte `real-static-mesh.vfmesh` was replayed at 128x96 with Vulkan
validation enabled:

```text
source-vertices=289 source-indices=1536 attributes=7
translated-vertices=289 translated-indices=1536
winding=counter-clockwise submission=1
differing=86 max-error=1 mean-error=0.00349935
validation-errors=0 result=pass
```

The resulting diagnostic surface is stored as `real-static-mesh.ppm` and
`real-static-mesh.png`. The PPM SHA-256 is:

    22EC94D7C0F477D62A3A709B9C064EE21C28A036ECF56E3C2E35EA1AFC8CA693

## Promotion decision

Phase 7 is promoted. A real engine-created mesh now crosses a validated,
pointer-free capture boundary and reaches the existing Vulkan raster backend
with CPU-oracle agreement. Phase 8 may attach captured textures, explicit
views, samplers, and mip residency to this mesh. Unsupported texture formats
or incomplete streamed payloads must remain diagnostic and cannot enable draw
suppression.
