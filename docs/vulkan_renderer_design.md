# Vulkan ray-tracing renderer takeover design

This is the implementation design for a Fallout 4 renderer replacement based on the current-build map in [`engine_render.md`](engine_render.md). It targets the installed `Fallout4.exe` 1.11.221.0 fingerprint recorded there. Exact engine addresses are module-relative and must be resolved and validated at runtime; the mismatched local PDB remains semantic evidence only.

The test-driven execution sequence is [`vulkan_renderer_implementation_plan.md`](vulkan_renderer_implementation_plan.md), which refines the delivery stages below into 27 red–green–refactor vertical phases.

**Status:** architecture and implementation contract only. No render-suppression or takeover hooks are enabled by this document.

The chosen architecture is a **Vulkan world-renderer takeover inside a real D3D11 compatibility shell**:

- Vulkan owns scene rendering, hardware ray tracing, lighting, post-processing, temporal history, and eventually presentation.
- Fallout's actual D3D11 device remains alive during the compatibility phases because engine objects, Scaleform, Bink, Flex, HBAO, Godrays, overlays, and other plugins can hold real COM pointers.
- D3D11-created shared textures are imported into Vulkan. A shared D3D11 fence, imported as a Vulkan timeline semaphore, transfers ownership without CPU readback.
- Engine culling, asset streaming, animation, gameplay, and UI continue to run. Their renderer-facing data is translated into versioned, pointer-free packets.
- Vanilla draw submission is suppressed only after a mirror renderer has proved that it can account for the frame. Every probe and hook fails open to the original renderer.

This is not a `Present` replacement, a shader injector, or a plan to pretend that Vulkan objects are D3D11 interfaces. Those approaches do not satisfy the ABI mapped in `engine_render.md`.

## 1. Required outcome

The replacement is complete when all of the following are true:

1. The engine still performs simulation, visibility, animation, streaming, menu, input, and UI work normally.
2. No vanilla D3D11 world draw or image-space dispatch is required to produce the displayed world image.
3. Vulkan produces primary visibility, lighting, shadows, reflections, global illumination, transparencies, temporal reconstruction, and tone mapping.
4. Ray-traced features use the cross-vendor KHR ray-tracing interfaces, not an NVIDIA-only API.
5. Scaleform, Bink, Flex, the Steam overlay, ordinary UI mods, and the F4SE plugin remain functional through a documented compatibility path.
6. Save games and gameplay objects are unaffected; renderer-side state is disposable and rebuildable.
7. Resize, alt-tab, fullscreen changes, loading screens, cell transitions, camera cuts, Pip-Boy, VATS, first-person weapons, power armor, local map, and photo/debug modes have explicit handling.
8. A failed build check, capability check, hook validation, or pre-takeover render check leaves the original renderer untouched.

The first production target is the compatibility architecture. A native Vulkan swap chain is a later mode, not a prerequisite for calling this a Vulkan renderer: in compatibility mode Vulkan still performs all world rendering and DXGI is only the final presentation/UI shell.

## 2. System architecture

```text
Fallout engine
  simulation / scene graph / animation / streaming / culling / Scaleform
        |
        | validated hooks and cloned interface vtables
        v
VisualForge renderer host (inside the F4SE plugin)
  BuildGate | HookTransaction | EngineABI | D3D11Interop | FaultController
        |
        | versioned POD resource events and immutable FramePacket
        v
VisualForge Vulkan backend (separate, lazily loaded DLL)
  ResourceRegistry -> SceneDatabase -> FrameGraph
                         |               |
                         |          raster + RT + post
                         v               v
                    BLAS / TLAS     Vulkan HDR output
                                           |
                         shared texture + shared GPU fence
                                           v
Real D3D11 compatibility shell
  optional vanilla depth/stencil | Scaleform/UI | Bink/Flex | DXGI Present
```

### 2.1 Ownership boundaries

| Object or responsibility | Compatibility owner | Native end-state owner |
|---|---|---|
| Game simulation, scene graph, animation | Engine | Engine |
| Visibility and pass registration | Engine initially; Vulkan may add GPU culling | Same hybrid model |
| Asset decoding and streaming policy | Engine, mirrored at resource boundaries | Engine front end with Vulkan storage |
| World color, lighting, RT effects, post | Vulkan | Vulkan |
| World depth/stencil | Vanilla depth prepass initially; then Vulkan plus a D3D handoff | Vulkan |
| Scaleform menus and HUD | Real D3D11 backend | Vulkan Scaleform/backend replacement |
| Bink and Flex | Real D3D11/middleware | Bridged or replaced independently |
| Swap chain and presentation | DXGI | Vulkan WSI after the compatibility island is gone |
| Engine `BSGraphics::*` memory | Engine; read or modified only through mapped contracts | Engine-compatible facade or retired callers |
| Vulkan resources and queues | Vulkan backend | Vulkan backend |

No raw engine pointer may cross into an asynchronous backend job. The host either retains a mapped engine reference for a bounded period or copies the relevant data into a packet owned by a frame arena.

### 2.2 Why the alternatives are rejected

| Alternative | Decision |
|---|---|
| Hook only `IDXGISwapChain::Present` | Useful for observation and final compositing, but too late to replace culling, materials, targets, temporal data, or world rendering. |
| Return fake Vulkan-backed `ID3D11*` objects | Requires a production-quality D3D11/DXGI implementation and exact COM/state semantics before the first image can render. This is essentially rebuilding DXVK. |
| Use DXVK alone | Translates the vanilla renderer to Vulkan but does not expose the semantic scene/material boundary needed for a ray-traced replacement. It may remain a diagnostic comparison target. |
| Replace `DrawWorld::Forward` and never call it | Also removes engine traversal, pass generation, target bookkeeping, and phase transitions that the replacement initially needs. |
| Copy D3D11 textures through the CPU | Correctness fallback for diagnostics only; unacceptable for per-frame rendering. |
| Start with full path tracing | It maximizes compatibility risk. The production baseline is hybrid primary visibility plus ray-traced secondary effects, with a path-traced mode added after scene coverage is complete. |

## 3. Runtime modes and takeover state

### 3.1 User-visible modes

| Mode | Vanilla D3D11 draws | Vulkan work | Displayed image | Purpose |
|---|---:|---:|---|---|
| `Off` | All | Capability probe only | Vanilla | Guaranteed fallback. |
| `Observe` | All | Resource/pass accounting only | Vanilla | Decode the engine without GPU duplication. |
| `Mirror` | All | Full Vulkan frame | Vanilla, Vulkan, split, or difference view | Prove scene and shader coverage. |
| `Takeover` | UI/middleware and declared compatibility passes only | Full world + post | Vulkan through DXGI bridge | Default production architecture. |
| `Native` | Headless compatibility calls only | Full frame + WSI | Vulkan swap chain | Later, after UI and middleware migration. |

`Takeover` cannot be selected directly on a newly seen executable or driver. It must pass Observe and Mirror gates for the active build/capability signature. A developer override may shorten the observation interval but cannot bypass structural validation.

### 3.2 Internal state machine

```text
Disabled
   -> Probing
   -> Observing
   -> Mirroring
   -> Armed
   -> TakingOver
               -> Draining -> Disabled
               -> Faulted  -> restore at next safe frame boundary
```

Transitions are monotonic within a frame. Hook installation is transactional: prepare trampolines and cloned vtables, validate all originals, suspend takeover decisions, enable the complete set, then publish one atomic hook-generation value. Partial installation is never considered active.

Failure rules:

- Before `TakingOver`, every failure immediately selects vanilla rendering.
- At the start of each takeover frame, a health check decides whether vanilla submission will be suppressed. If the decision is false, that frame runs entirely vanilla.
- A Vulkan error after vanilla submission was suppressed presents the last completed Vulkan frame, marks the backend faulted, and restores vanilla at the next `Renderer::Begin`. It must not attempt to run the original world renderer halfway through a mutated frame.
- `VK_ERROR_DEVICE_LOST`, D3D device removal, an interop fence timeout, or a bridge ownership violation disables hot re-entry for the process. A restart is required after logging device-fault data.
- Hooks and backend code are not unloaded while any engine thread could still execute a trampoline.

## 4. Bootstrap and capability contract

### 4.1 Exact-build gate

The host validates, in order:

1. `Fallout4.exe` version, size, PE timestamp, `SizeOfImage`, and SHA-256 against the fingerprint in `engine_render.md`.
2. The Address Library file identity and every ID used by the takeover manifest.
3. Each direct RVA's containing section, unwind range, expected instruction/prolog hash, and critical call targets.
4. Every hooked object's TypeDescriptor, Complete Object Locator, vtable address, slot count, and executable slot targets.
5. Singleton pointer range/alignment and mapped structure invariants before dereference.

A manifest is data, not scattered constants:

```cpp
struct HookSiteManifest {
    uint32_t manifestVersion;
    uint32_t addressLibraryId;  // zero for independently matched sites
    uint32_t expectedRva;
    uint32_t minimumLength;
    uint64_t instructionHash;
    HookRole role;
};
```

The host records the manifest version, executable fingerprint, Vulkan device UUID, driver UUID, extension/feature mask, and shader-pack hash in every renderer log and pipeline cache name.

### 4.2 Vulkan baseline

Require Vulkan 1.3 for the replacement backend even though the KHR ray-tracing family can be exposed on older core versions. Vulkan 1.3 gives one consistent baseline for dynamic rendering, synchronization2, timeline semaphores, and modern feature queries.

Required device capabilities:

- `VK_KHR_acceleration_structure`
- `VK_KHR_ray_tracing_pipeline`
- `VK_KHR_ray_query`
- `VK_KHR_deferred_host_operations`
- buffer device address
- descriptor indexing features used by the bindless texture/material tables
- timeline semaphores, synchronization2, and dynamic rendering
- `VK_KHR_external_memory_win32`
- `VK_KHR_external_semaphore_win32`
- support for importing `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT`
- support for importing `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT` or, as a fallback, `VK_KHR_win32_keyed_mutex`
- `shaderInt64`, anisotropic sampling, BC texture formats used by the assets, and the selected bridge image formats/usages

Optional capabilities:

- `VK_EXT_memory_budget`
- `VK_KHR_ray_tracing_maintenance1`
- `VK_KHR_ray_tracing_position_fetch`
- `VK_EXT_mesh_shader`
- pipeline libraries, pipeline creation cache control, HDR metadata, present ID/wait, calibrated timestamps, device fault reporting, and vendor crash diagnostics

Mesh shaders and vendor-specific ray-tracing extensions must never be required. The KHR ray-tracing extensions deliberately support hybrid raster/ray designs and define the acceleration-structure and ray-pipeline split used here. See the official [Khronos ray-tracing guide](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html) and [`VK_KHR_ray_tracing_pipeline`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_ray_tracing_pipeline.html).

### 4.3 Same-adapter selection

The Vulkan physical device must be the same GPU as the D3D11 device:

1. Query the D3D device's `IDXGIDevice`, parent `IDXGIAdapter1`, and `DXGI_ADAPTER_DESC1::AdapterLuid`.
2. Query every Vulkan device using `VkPhysicalDeviceIDProperties`.
3. Require `deviceLUIDValid == VK_TRUE` and a byte-for-byte LUID match.
4. Reject software, remote, or unmatched adapters; do not fall back to “first discrete GPU.”

The Vulkan specification explicitly defines a valid Windows `deviceLUID` as equal to the corresponding DXGI adapter LUID. This is also a prerequisite for reliable external-object sharing: [`VkPhysicalDeviceIDProperties`](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceIDProperties.html).

### 4.4 Reference machine discovered during design

The current development machine reports:

| Item | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 4090 (`vendorID 0x10DE`, `deviceID 0x2684`) |
| Driver | 596.36 |
| Device Vulkan API | 1.4.329 |
| Vulkan loader instance API | 1.4.321 |
| Installed SDK | 1.3.268.0 |
| Required RT/external-memory/external-semaphore extensions | Present in the current driver report |

The backend still targets Vulkan 1.3 and runtime feature queries. This machine is a validation target, not the compatibility definition.

## 5. Hook and frame-integration plan

### 5.1 Current-build hook manifest

The exact anchors below come from `engine_render.md`. Observation hooks call their original unless a later row explicitly owns suppression.

| Boundary | Current address | Initial behavior | Takeover behavior |
|---|---:|---|---|
| `D3D11CreateDeviceAndSwapChain` IAT call | call at RVA `0x1824880` | Capture device, immediate context, swap chain, adapter, and feature level. | Still call the real API; never return a fake device. |
| `BSGraphics::Renderer::Init` | RVA `0x1815BB0` | Validate dimensions, `RendererData`, and initialization order. | Initialize the host after the real device and target registry exist. |
| `Renderer::WindowSizeChanged` | AL ID 2276824, RVA `0x18174F0` | Bracket and log the exact resize sequence. | Drain bridge slots before original teardown; recreate them after original target rebuild. |
| `Renderer::Begin` | AL ID 2276833, RVA `0x1817E30` | Snapshot frame/window/state values after the original call. | Select a frame arena, consume resource events, and begin scene capture. |
| `Renderer::End` | AL ID 2276834, RVA `0x1818080` | Timestamp and validate present semantics. | Final safety check; retain original skip-present and retry behavior while DXGI owns presentation. |
| `BSCullingProcess::Process` variants | AL IDs 2275931/2275932, RVAs `0x17E17A0`/`0x17E19F0` | Associate cameras and visible objects with an accumulation phase. | Feed the visible-instance set; engine culling remains authoritative initially. |
| `BSShaderResourceManager` singleton/vtable | AL ID 2703483, slot RVA `0x3438128`, vtable `0x29139A8` | Mirror resource creation, streaming, and reference events. | Dual-create Vulkan resources from CPU inputs before they are lost behind D3D objects. |
| `BSShaderProperty::SetMaterial` | AL ID 2316285, RVA `0x2161BF0` | Track material/property revisions. | Rebuild immutable GPU material records on revision changes. |
| `BSShaderTextureSet::CreateObject` | AL ID 2316324, RVA `0x2162680` | Capture ten-slot texture-set identity and names. | Resolve bindless texture handles and streaming dependencies. |
| Accumulator registration/final render | exact current methods still to be matched | Count objects, pass groups, render modes, cameras, and targets. | Primary semantic takeover: serialize registered passes and suppress only classified world submission. |
| `BSBatchRenderer::RenderPassImpl` family | exact current methods still to be matched | Capture the final material/geometry/technique packet and compare with accumulator data. | Safety net for immediate, skinned, instanced, alpha, and custom paths. |
| `DrawWorld::Forward` | AL ID 2318315, RVA `0x21F16D0` | High-level phase bracket and coverage accounting. | Keep orchestration; do not skip the whole function. |
| Dynamic-resolution/image-space owner | AL ID 2318322, RVA `0x21F27D0` | Identify target, post, and UI ordering. | Suppress vanilla image space only in the world-and-post profile. |
| `BSScaleformRenderer::Initialize` | AL ID 2284938, RVA `0x1A89BC0` | Capture the real UI device/targets. | Leave the D3D11 Scaleform backend operational until native UI migration. |

The accumulator and batch-renderer method addresses are a hard pre-takeover gate. Their semantic names from the mismatched PDB are not sufficient. They must be matched in the current executable, validated through `.pdata`, exercised in Observe mode, and added to the build manifest before any draw is suppressed.

`EngineAbi` also validates the direct `RenderTargetManager` singleton (AL ID 2666735), `BSGraphics::State` (2704621), `RendererData**` (2704429), current `RendererWindow**` (2704431), `ImageSpaceManager*` (2712627), and camera near/far globals (2712882/2712883). These are read/snapshot contracts, not generic writable configuration addresses.

### 5.2 Hook mechanics

- Prefer F4SE/Address Library resolution for functions with current IDs.
- Use an IAT-slot exchange for D3D creation imports; retain the exact original pointer.
- Use MinHook only for validated function entries. Never detour into the middle of a chained unwind fragment merely because a byte pattern matches.
- For `BSShaderResourceManager`, allocate a writable clone of the 52-slot vtable, replace only known slots, validate all untouched entries, and atomically exchange the singleton object's vptr. This avoids globally patching code shared by unrelated objects.
- Bracket recursive or nested calls with a thread-local hook depth. Resource hooks may execute on streaming threads; frame hooks must verify the game render thread.
- Every detour has a generation cookie and original-call guard so shutdown or rollback cannot enter a stale backend.
- Do not use `MH_EnableHook(MH_ALL_HOOKS)` for the renderer. Enable only the prepared takeover set so unrelated VisualForge hooks cannot be accidentally changed as a group.

### 5.3 Takeover cut profiles

Two cuts are supported because a correct UI boundary and a correct engine-target bridge are separate milestones.

**World-only profile**

1. Engine traversal, culling, pass registration, target selection, and the vanilla depth/stencil compatibility prepass run.
2. Classified world color/lighting submission is suppressed at the accumulator/batch boundary.
3. Vulkan renders an HDR world image and publishes it into the engine's expected scene-color target before `ImageSpaceManager` consumes it.
4. Vanilla image-space and UI continue.

This profile is useful early, but it requires every target consumed by vanilla post-processing to be populated with compatible values.

**World-and-post profile (production target)**

1. Engine traversal, culling, pass registration, animation, and UI preparation run.
2. Vanilla world and image-space submission are suppressed by classified hooks; orchestration and state transitions still execute.
3. Vulkan renders and tone-maps the final world image.
4. The image is handed to D3D11 immediately before Scaleform/HUD submission.
5. D3D11 renders UI and middleware overlays, then the original DXGI `Present` runs.

The exact current `Main::DrawWorld_And_UI`/Scaleform boundary is not yet mapped. World-and-post takeover must remain disabled until a trace proves the selected handoff is after every world/post producer and before the first UI consumer in gameplay, loading, menu, Pip-Boy, VATS, and local-map frames.

### 5.4 Compatibility-frame sequence

```text
Renderer::Begin
  call original
  BuildGate + backend health check
  acquire FrameArena[N % framesInFlight]
  drain resource events; snapshot camera and renderer state

engine traversal/culling/pass registration
  Observe: account only
  Mirror: account and retain vanilla draws
  Takeover: account and suppress classified world draws

takeover boundary
  seal immutable FramePacket
  submit Vulkan uploads, deformation, AS builds, raster, RT, post
  enqueue D3D11 GPU wait for bridge-ready fence value
  copy or sample bridge output into the UI/present target

Scaleform / Bink / compatibility overlays
  real D3D11 device and context

Renderer::End
  preserve engine skip/retry/inPresent behavior
  original DXGI Present
  retire frame resources by completed timeline values
```

There is no per-frame CPU wait between Vulkan and D3D11. `ID3D11DeviceContext4::Wait` and the Vulkan imported timeline semaphore create a GPU dependency. CPU waits are restricted to shutdown, destructive resize, and diagnostic fault handling.

## 6. Host/backend ABI

### 6.1 DLL separation

Keep the existing F4SE-facing host small and make the renderer a lazily loaded `VisualForgeRenderer.dll`:

- `VisualForge.dll` owns F4SE entry points, engine hooks, the real D3D11 objects, build validation, fault policy, and configuration.
- `VisualForgeRenderer.dll` owns Vulkan, shaders, the scene database, frame graph, acceleration structures, and denoisers.
- The host loads the backend only after the executable and D3D device pass validation. A missing Vulkan loader/backend leaves the game on vanilla D3D11.
- The boundary is a versioned C ABI using fixed-width POD types. No STL object, exception, allocator ownership, COM pointer, or engine pointer crosses it.

```cpp
struct VfBlobSpan {
    uint32_t byteOffset;
    uint32_t elementCount;
    uint32_t elementStride;
};

struct VfFramePacketHeader {
    uint32_t abiVersion;
    uint32_t byteSize;
    uint64_t frameId;
    uint64_t engineFrameId;
    uint64_t historyEpoch;
    uint32_t flags;
    uint32_t viewCount;
    VfBlobSpan views;
    VfBlobSpan instances;
    VfBlobSpan lights;
    VfBlobSpan decals;
    VfBlobSpan particles;
};

struct VfRendererApiV1 {
    uint32_t abiVersion;
    uint32_t (*Initialize)(const VfHostServices*, const VfBackendCreateInfo*);
    uint32_t (*CreateBridge)(const VfBridgeImportInfo*);
    void (*ApplyResourceEvents)(const void* bytes, uint32_t byteSize);
    VfSubmitResult (*SubmitFrame)(const void* packet, uint32_t byteSize);
    void (*Resize)(const VfResizeInfo*);
    void (*Drain)();
    void (*Shutdown)();
};

extern "C" const VfRendererApiV1* VF_GetRendererApi(uint32_t requestedAbi);
```

All packets include `abiVersion`, `byteSize`, endianness/packing constants, and a CRC in debug builds. Unknown trailing fields are ignored so compatible additions do not require lockstep updates.

### 6.2 Stable handles

Renderer packets identify resources with a 64-bit host handle, never an address:

```text
bits  0..31: slot
bits 32..47: generation
bits 48..55: resource kind
bits 56..63: host namespace/version
```

The host maps engine pointer plus observed lifetime generation to this handle. Reuse of an engine address after destruction therefore cannot alias an old Vulkan resource. Destroy events carry a final host sequence number, and Vulkan destruction is deferred until every submitted timeline value that references the generation has completed.

### 6.3 Threading rules

- The game render thread owns frame begin/seal and reads live renderer structures.
- Streaming/resource hooks copy arguments into an MPSC event queue and return quickly. They never call Vulkan.
- The backend render thread consumes sealed packets and records primary command buffers.
- Worker threads may record secondary command buffers, build pipeline objects, decompress assets, and prepare BLAS inputs.
- Engine `IncRef`/`DecRef` calls occur only on an approved engine thread. When retention is necessary, the host schedules release back to that thread.
- A frame arena is immutable after seal. Three arenas are the default; the runtime may raise this to match measured GPU latency but cannot reuse one before its Vulkan and bridge fence values complete.

## 7. Scene extraction contract

### 7.1 Visibility and pass accounting

Reuse engine culling first. Every `RegisterObject*`, `RegisterPass*`, immediate-render path, and final batch path is assigned a capture category:

| Category | Capture action |
|---|---|
| Main opaque / alpha test | Create or update a world instance; eligible for BLAS/TLAS and primary visibility. |
| Skinned / morphing | Capture mesh handle plus current bone/morph/deformation inputs. |
| Instanced | One mesh/material record plus all instance transforms and per-instance data. |
| Shadow-only | Capture visibility/mask state; do not duplicate as visible geometry unless another pass does. |
| Decal | Capture projection/mesh, blend mode, material, sort group, and receiver masks. |
| Transparent | Capture sort keys and material class; normally rasterized after RT lighting. |
| Water | Capture surface mesh plus water material, reflection plane, and simulation inputs. |
| Particle / effect | Capture emitter/dynamic buffers where decoded; otherwise allow a declared D3D compatibility pass. |
| First person | Place in a distinct visibility layer/TLAS mask with its own projection behavior. |
| VATS/local map/occlusion/debug | Route to a specialized view or retain the vanilla pass until implemented. |

Observe mode compares counts at accumulator registration, batch registration, and actual D3D draw calls. A frame is not takeover-capable if an unclassified draw writes a world target.

### 7.2 Views and temporal state

Each `VfView` contains copied current/previous view, projection, inverse matrices, camera-relative origin, viewport/scissor, near/far values, jitter, exposure inputs, render mode, eye/view identifier, and engine target identity.

Vulkan and D3D both use a zero-to-one depth range. Use a negative Vulkan viewport height or an explicit projection conversion for Y orientation; do not silently transpose/flip individual shaders. The engine's observed standard-Z depth convention is preserved initially. A single validated conversion matrix is applied at the packet boundary and covered by world-to-screen parity tests.

Increment `historyEpoch` and discard temporal reservoirs/history on:

- camera cuts, teleports, load screens, cell/worldspace changes, or time discontinuities;
- FOV, near/far, projection-mode, render-scale, output-size, or AA-mode changes;
- entering/leaving Pip-Boy, VATS, power-armor scopes, local map, dialogue cameras, or first-person special views;
- material/shader-pack incompatibility changes;
- bridge recreation or a skipped/faulted frame.

### 7.3 Geometry

Resource hooks capture CPU geometry at the `IRendererResourceManager` creation boundary. The translator must support every `VertexDesc` stream present in `engine_render.md`, including packed half positions, normals/tangents, UVs, colors, skin indices/weights, instancing data, and dynamic shape buffers.

The mapped engine `PackVertexData`/`UnpackVertexData` routines are parity oracles during development. For every vertex layout, generate randomized valid inputs, run the engine utility and the Vulkan-side decoder, and require byte/value agreement within the source encoding's precision.

Geometry is stored camera-relative for frame work while stable object/world transforms remain double- or high-precision on the CPU. Static meshes share one compacted BLAS per geometry/opacity variant; instances carry transforms and material indirection.

### 7.4 Materials and textures

The material translator starts from semantics, not D3D bytecode. Its input combines:

- `BSShaderProperty`'s 64 flags and generated pass state;
- concrete `BSShaderMaterial` family and lighting feature ID;
- `BSShaderData` authoring values;
- the ten-slot `BSShaderTextureSet` and resolved texture metadata;
- live wetness, snow, glow, alpha, refraction, stencil, VATS, Pip-Boy, and environment state;
- pass technique/domain IDs for diagnostics and compatibility matching.

The compatibility model is **authored specular/smoothness**, not mandatory metallic/roughness. Fallout's textures and material records already describe diffuse response, colored specular response, smoothness, Fresnel shaping, wetness, emission, opacity, and several specialized lobes. Forcing those inputs through a guessed metalness map would discard information and misclassify painted metal, dirty dielectrics, skin, and stylized assets.

Immutable `GpuMaterial` records use bindless texture indices and a feature bitset. A shader permutation is selected by broad material class; minor features branch from record data to prevent the original combinatorial technique count from becoming a Vulkan pipeline explosion.

Required material classes are opaque specular/smoothness, alpha-tested, skin, hair, eye, landscape, snow/wetness, emissive/glow, parallax/POM, decal, glass/refraction, water, sky, effect, particle, and UI. Unknown classes render with a conspicuous diagnostic material in Mirror mode and force vanilla fallback in Takeover mode.

#### 7.4.1 Canonical surface model

The general opaque BRDF is GGX with a colored normal-incidence reflectance (`F0`) and no required metalness parameter. This represents the source specular workflow directly:

```text
authoredSmoothness = saturate(specSmooth.g * materialSmoothness)
perceptualRoughness = SmoothnessTransfer[materialClass](authoredSmoothness)
specularWeight = max(0, specSmooth.r * materialSpecularScale)
F0 = SpecularTransfer(materialSpecularColorLinear, specularWeight)
```

`SmoothnessTransfer` and `SpecularTransfer` are versioned calibration LUTs, not hard-coded claims about the original shader. Mirror captures under controlled light sweeps determine them. `1 - authoredSmoothness` is an acceptable bring-up approximation only. `F0` is clamped to a finite shader-safe range, but the original, unclamped authoring values remain in the trace for diagnostics.

The BRDF has two selectable energy policies over the same decoded inputs:

- `Compatibility` preserves the source diffuse/specular balance as closely as captures permit.
- `Conserving` applies diffuse energy reduction and multiple-scattering compensation for relit and path-traced modes.

`Compatibility` is the default for Takeover. Changing energy policy is a renderer profile change, never an implicit consequence of enabling ray tracing.

The canonical surface output is:

```text
SurfaceSample {
    baseColorLinear, opacity,
    geometricNormal, shadingNormal,
    F0, perceptualRoughness,
    emissiveRadiance,
    height, environmentWeight,
    materialClass, featureFlags, provenanceMask
}
```

Metalness may exist as an optional override or specialized-material parameter, including the existing wetness control, but it is not inferred as a required texture channel. A surface can be shaded entirely from diffuse color plus colored `F0` and roughness.

#### 7.4.2 Source precedence and provenance

Every translated value records how it was obtained. Resolution order is:

1. live controller or `BSShaderProperty` override;
2. concrete `BSShaderMaterial` value;
3. `BSShaderData`/root-material value;
4. texture bound to a known semantic slot under the active feature flags;
5. filename/format heuristic when no semantic owner remains available;
6. class-specific canonical fallback.

The provenance states are `Authored`, `LiveOverride`, `DerivedFromAuthored`, `Heuristic`, `Fallback`, and `Unknown`. They are packed into diagnostics, not evaluated in the hot BRDF. A heuristic cannot silently replace an authored value, and Takeover policy can reject a material with `Unknown` provenance in a critical channel.

Texture suffixes are evidence, not an ABI. Mods can use arbitrary names, and several engine slots are feature-dependent. The translator therefore keys on the resolved material class, feature flags, texture-set slot, and loaded image metadata before considering `_d`, `_n`, `_msn`, `_s`, or other naming conventions.

#### 7.4.3 Texture decode contract

| Semantic input | Decode/view | Surface use | Missing behavior |
|---|---|---|---|
| Base/diffuse RGB | Normally sampled through an sRGB view and converted to linear | `baseColorLinear`; palette/tint logic is applied before BRDF evaluation | White texture multiplied by material/tint factors |
| Base alpha | Linear channel; meaning enabled only by alpha state | Coverage, alpha test, blend opacity, or feature-specific mask | Opaque; never infer transparency from RGB |
| Tangent normal | Linear signed XY, commonly BC5; reconstruct non-negative Z and normalize | Transform through the captured tangent frame | Flat `(0,0,1)` normal |
| Model-space normal | Linear RGB vector selected by the model-space-normal flag | Transform object to world; never pass through the tangent basis | Diagnostic/fallback unless class and transform are known |
| Smooth/spec map | Linear channels; authored FO4 convention is R specular weight and G smoothness when that role is selected | Colored `F0` weight and calibrated roughness | Unit masks multiplied by scalar material values |
| Glow/emissive | Usually an sRGB color view, multiplied by emit color and multiplier | Visible emission; eligible for sampled-light extraction only after intensity calibration | Material emission color alone, or zero when emission is disabled |
| Height/displacement | Linear scalar plus material scale/bias | POM first; geometric displacement only for explicitly supported classes | Zero height |
| Environment mask/cubemap | Linear scalar mask and explicit cube view | Compatibility reflection/probe weighting | No local environment term; RT reflection remains available by policy |
| Palette, lookup, rim/backlight, wrinkles, multilayer | Feature-specific view and swizzle | Evaluated only by the owning material class | Class-specific neutral resource |

Normal reconstruction is defined and tested identically across raster and ray shaders:

```text
xy = encodedRG * 2 - 1
z  = sqrt(max(1 - dot(xy, xy), 0))
n  = normalize(float3(xy, z))
```

The engine-parity suite determines green-channel handedness per decoded convention. The renderer does not globally flip all normal maps based on a content-tool convention. For model-space maps it validates object-space axis conventions with known meshes before enabling Takeover.

BC compression does not determine color space. The same Vulkan image may expose legal linear and sRGB views when needed, but the selected material semantic owns the view choice. Scalar masks, normals, smoothness, height, and lookup coordinates are always sampled as data rather than color.

#### 7.4.4 CPU and GPU records

The host emits a pointer-free `MaterialDesc` with stable ID and revision. It contains original material/feature IDs, texture bindings with explicit semantic/view/swizzle, scalar authoring values, alpha and depth state, UV transforms, class flags, and per-field provenance. The trace serializer writes this description verbatim so a material can be replayed outside the game.

The shader-facing record uses explicit 16-byte groups and integer fields; it has no C++ bitfields or host pointers:

```text
GpuMaterialStatic
  uvec4 textures0       // base, normal, specSmooth, emissive
  uvec4 textures1       // height, environment, lookup, auxiliary
  uvec4 samplerAndFlags // surface sampler, auxiliary sampler, flagsLo, flagsHi
  vec4  baseFactor      // linear RGB, material alpha
  vec4  specular        // linear color RGB, scalar strength
  vec4  surface         // smoothness, Fresnel power, normal scale, alpha cutoff
  vec4  emission        // linear RGB, multiplier
  vec4  secondary0      // subsurface/rim/backlight/class-specific values
  vec4  uvTransform     // scale XY, offset XY

GpuMaterialDynamic
  vec4 wetness0
  vec4 wetness1
  vec4 liveColorAlpha
  uvec4 revisionAndFlags
```

The final C++/shader definition must `static_assert` every offset and size and have a serialization round-trip test. Static records are immutable per revision. Frequently changing wetness, fade, tint, controller, VATS, and Pip-Boy values live in a separate per-frame dynamic table so controller animation does not churn bindless descriptors or static material allocations.

#### 7.4.5 Specialized material policy

| Class | Compatibility treatment | Initial RT treatment |
|---|---|---|
| Opaque/default | Specular/smoothness GGX with captured tint, Fresnel, and environment behavior | Full shadow, reflection, and GI participation |
| Alpha-tested foliage/geometry | Identical sampled coverage and alpha reference in visibility, G-buffer, and shadow paths | Any-hit alpha test; lower profiles may rasterize dense animated foliage shadows |
| Face/skin | Skin tint, normal/model-space-normal mode, subsurface rolloff, rim/backlight | Opaque geometry with validated diffusion/backlight approximation before optional SSS rays |
| Hair | Hair tint and anisotropic lobe using captured tangent direction | Alpha-tested cards and conservative any-hit; no guessed transmission |
| Eye | Separate sclera/iris/cornea response and eye-environment behavior | Specialized raster/RT hit group after eye transforms are validated |
| Landscape/LOD/snow | Preserve layer weights, landscape normals, noise, snow and wetness controls | Opaque RT participation; no flattening to one baked base texture |
| Parallax/POM | Captured height, scale, bias, and UV behavior | POM changes shading/visibility locally; BLAS remains undisplaced until a geometric path is implemented |
| Glow/emissive | Authored glow and material emittance | Visible emission immediately; importance sampling is opt-in after flux calibration |
| Decal/effect/particle | Preserve blend, stencil, falloff, soft-particle, and sorting rules | Raster/composite first; specialized ray participation later |
| Glass/refraction | Dedicated alpha/refraction class; never inferred from bright or low-alpha pixels alone | Raster/refraction first; ray transmission only with validated IOR/thickness policy |
| Water | Dedicated `BSWaterShaderMaterial` translator and simulation inputs | Specialized hybrid water path; never interpreted as the base lighting record |

#### 7.4.6 Safe derivation from existing images

Derived data is allowed only when an authored channel is absent, and is stored in a renderer cache keyed by source-image hash, decoder version, derivation algorithm version, and settings. BA2 files and loose source assets are never modified.

Safe or bounded derivations are:

- normal-map variance per mip for Toksvig-style specular antialiasing;
- conservative micro-cavity from normal/height data, limited to indirect/micro-occlusion rather than geometry visibility;
- a roughness prior from material class and image statistics only as an explicitly marked `Heuristic` fallback;
- height-assisted horizon/self-shadow terms for POM;
- alpha-coverage-preserving mip thresholds for alpha-tested assets.

The following are not compatibility-safe pixel inferences: metalness, IOR, transmission, subsurface depth, whether a bright pixel emits light, or whether dark diffuse detail is ambient occlusion. Optional offline estimation may propose overrides, but it cannot become the default Takeover path and must remain reversible and visible in provenance debug views.

#### 7.4.7 Ray-tracing shader rules

- The geometric normal controls face orientation, ray origin offset, and front/back classification. The normal map supplies a hemisphere-corrected shading normal and must never move the ray origin below the geometric surface.
- Ray hit shaders have no implicit screen-space derivatives. Texture LOD uses ray cones or an equivalent footprint propagated through reflection/refraction bounces; primary-hit raster derivatives are not assumed to exist in secondary rays.
- Alpha any-hit uses the same UV transform, texture view, mip policy, alpha reference, and dither/fade state as the corresponding raster material. Shadow and camera visibility cannot use different cutout semantics.
- Roughness controls reflection-ray policy and denoiser metadata, but a performance cutoff returns a filtered probe/environment result rather than silently deleting the specular lobe.
- Emissive triangles enter an importance-sampling table only when their world-space area and calibrated radiance are known. An emissive texture alone is not automatically treated as a high-energy light.
- Two-sided shading changes the shading frame deliberately; it does not blindly negate every decoded normal. Thin foliage transmission/backlight is a class feature rather than generic transparency.
- POM affects the shading hit and UVs but not traversal visibility. Its silhouettes and inter-object shadows remain geometric until tessellated/displaced BLAS support is explicitly enabled.

#### 7.4.8 Material validation and debug contract

Mirror mode exposes at least these views: semantic texture role, raw base color, linear base color, geometric normal, decoded normal, final shading normal, specular R, smoothness G, calibrated roughness, `F0`, emissive radiance, opacity/alpha reference, environment weight, material class, and per-field provenance.

The automated material fixture contains dielectric, painted and bare metal references; every alpha mode; tangent- and model-space normals; mip-distance ramps; skin, hair, eye, foliage, landscape, snow/wetness, glow, POM, glass, and water representatives. It renders camera/light/environment sweeps through vanilla capture and Vulkan replay.

Takeover material gates require:

1. every visible standard material resolves to a known class and semantic texture set;
2. the alpha classifier agrees for primary, shadow, and depth passes;
3. missing/heuristic critical channels remain below the configured coverage threshold;
4. roughness highlight width and peak response fall within the calibrated parity envelope;
5. mip transitions, normal orientation, UV transforms, and palette/tint behavior pass their fixtures;
6. an unknown specialized material is classified for vanilla fallback before world submission is suppressed.

Material overrides are keyed by normalized material path plus optional source-content hashes and feature predicates. They can replace semantics or factors without replacing source files, are listed in captures, and lose to live controller values unless explicitly declared otherwise. This gives shader/material mods a documented porting surface without pretending D3D bytecode is portable.

### 7.5 Lights, weather, and special geometry

- Capture active lights from the accumulator/shadow-scene boundaries, not by walking guessed object layouts. Copy type, transform, range, color/intensity, attenuation, cookie, volumetric flags, shadow mask, and stable identity.
- Preserve vanilla light equations first. Physically based units and artistic relighting are an optional profile after image parity.
- Sky/weather packets include time, directional light, ambient/image-space values, fog, precipitation, wind, and relevant cubemap/cloud resources.
- Terrain and settlement statics use cell-scoped resource groups. Cell attach/detach events schedule BLAS residency and deferred destruction.
- Skinned meshes run Vulkan compute skinning before BLAS update. Fixed topology uses `ALLOW_UPDATE`; topology-changing geometry rebuilds. A configurable distance/update-rate policy limits animation BLAS cost.
- Foliage/hair alpha geometry supports any-hit alpha testing, but dense wind-deformed foliage may use raster shadows or reduced-frequency BLAS updates in lower profiles.
- Water, glass, particles, and decals remain raster/composite stages after opaque RT lighting until their specialized ray models are validated.

## 8. Resource registry and streaming

### 8.1 Registry model

The backend registry is keyed by stable host handles and contains separate immutable descriptions and mutable GPU state:

```text
MeshDesc / TextureDesc / MaterialDesc / SamplerDesc
                    |
                    v
ResourceRecord { generation, contentHash, residency, lastUseTimeline }
                    |
                    v
VkBuffer / VkImage / views / descriptor indices / BLAS
```

Content hashes deduplicate identical mod assets without conflating independently mutable engine objects. A material revision points to texture handles; it never owns textures directly. Descriptor indices are quarantined until the retirement timeline passes so an in-flight shader cannot observe an index reused for another texture.

### 8.2 Texture path

Capture texture source metadata and decoded/streaming payloads at `LoadTexture`, `ReadTexture`, and `FinishStreamingTexture` boundaries in the resource-manager interface. The Vulkan copy should be produced from the CPU/file payload, not by reading back the engine's D3D texture.

Required behaviors:

- Preserve BC1 through BC7 compression where Vulkan format support allows it.
- Preserve sRGB versus linear interpretation per view, not by duplicating image bytes.
- Translate typeless D3D resources into an explicit Vulkan image plus the legal view formats required by the material.
- Supply canonical fallback images for missing diffuse, normal, material, opacity, cubemap, and lookup slots.
- Track streamed mip residency and update descriptor-visible views only after upload barriers complete.
- Apply sampler addressing/filter/aniso from engine state; cache immutable Vulkan samplers by normalized description.
- Budget residency using `VK_EXT_memory_budget` when available. Eviction is based on last visible/use timeline, mip priority, UI/world class, and estimated re-stream cost.

During compatibility phases D3D11 and Vulkan may both own copies of world assets. The host therefore enforces a configurable Vulkan budget and never assumes the RTX 4090's memory size. A later “Vulkan-primary resource” phase may suppress unused D3D world-resource creation, but only after all engine readers have been mapped and given valid compatibility placeholders.

### 8.3 Mesh and dynamic-buffer path

- Static CPU vertex/index payloads upload once into device-local buffers with device addresses.
- Dynamic tri-shapes use per-frame ring allocations; avoid creating one Vulkan allocation per engine buffer update.
- Skin/morph source streams live in stable buffers; compute writes the current deformed vertex buffer used by raster and BLAS build/update.
- Index width, primitive range, base vertex, winding, two-sidedness, and strip/list conversion are explicit in `MeshDesc`.
- CPU-visible staging uses persistently mapped arenas with non-coherent atom alignment respected.
- AS scratch memory uses a timeline-retired suballocator aligned to `minAccelerationStructureScratchOffsetAlignment`.
- Static BLAS compaction is asynchronous: query compacted size, copy to the compact allocation, switch the registry record only after completion, then retire the source.

### 8.4 Lifetime and mutation

Resource-manager increment/decrement hooks are signals, not permission to immediately destroy Vulkan objects. A destruction sequence is:

1. Host observes final engine release and emits `Destroy(handle, generation, sequence)`.
2. Backend prevents new frame packets from resolving that generation.
3. Existing frames retain it through their submission timeline.
4. After `lastUseTimeline` completes, descriptors are cleared/quarantined and GPU objects are destroyed.
5. The host slot generation increments before the engine address can be associated with a new object.

Material alpha/wetness/controller changes are small update events. Geometry topology, texture format/extent, and shader-pack compatibility changes create a new generation rather than mutating in-flight state.

## 9. Vulkan backend

### 9.1 Device and queues

Create one Vulkan device on the LUID-matched physical device. Prefer:

- one graphics/compute/ray-tracing queue for the externally synchronized frame chain;
- a dedicated transfer queue when it does not introduce ownership-transfer overhead for small uploads;
- an optional asynchronous compute queue only after timestamp traces prove overlap rather than contention.

All queues use timeline values for internal retirement. External D3D synchronization occurs on the primary graphics queue. Never call `vkDeviceWaitIdle` during normal rendering.

Enable validation and synchronization validation only in developer builds/configuration. Name every Vulkan object with frame/resource identity through debug utils, and emit calibrated CPU/GPU timestamps when supported.

### 9.2 Descriptor model

Use descriptor indexing with large update-after-bind arrays:

| Set | Contents |
|---:|---|
| 0 | Per-frame/view constants, TLAS, global light/instance/material buffers. |
| 1 | Bindless sampled images and samplers. |
| 2 | Storage images/buffers for frame-graph passes. |
| 3 | Pass-local descriptors for exceptional resources that cannot use the stable tables. |

Required feature bits include runtime descriptor arrays, partially bound descriptors, sampled-image non-uniform indexing, and the selected update-after-bind limits. Query and clamp capacities; never assume desktop maximums. Descriptor slot 0 in each table is a valid fallback resource.

### 9.3 Canonical frame resources

| Resource | Preferred format | Role |
|---|---|---|
| HDR scene color | `R16G16B16A16_SFLOAT` | Lighting and pre-tonemap composition. |
| Albedo/coverage | `R8G8B8A8_SRGB` or paired linear view | Base color and coverage. |
| Normal | `R16G16_SNORM` octahedral | View/world normal reconstruction. |
| Material parameters | packed `R8/R16` targets | Perceptual roughness, micro-occlusion and class auxiliaries; colored `F0` is reconstructed through material ID when packing it would lose precision. |
| Motion | `R16G16_SFLOAT` | Pixel motion in output-pixel convention. |
| Linear depth | `R32_SFLOAT` | RT reconstruction, post, and D3D depth handoff. |
| Object/material ID | `R32_UINT` | Reprojection validation and debug. |
| Reactive/transparency mask | `R8_UNORM` | Temporal reconstruction. |
| Direct/indirect radiance | `R16G16B16A16_SFLOAT` | Separate denoiser inputs. |
| Moments/history length | pass-specific `R16/R32` | Temporal denoising confidence. |

Formats are preferences subject to runtime format/usage queries. External bridge resources have a smaller compatibility set and are not assumed to support every storage/attachment usage.

### 9.4 Frame graph

Use a declarative frame graph that compiles logical resources and read/write declarations into image layouts, synchronization2 barriers, queue dependencies, and transient alias allocations.

Baseline world-and-post graph:

```text
DrainUploads
  -> SkinMorphWind
  -> BuildOrUpdateBLAS
  -> BuildTLAS
  -> CullAndBuildDraws
  -> PrimaryVisibility / GBuffer raster
  -> RayTracedDirectShadows
  -> RayTracedReflections
  -> RayTracedDiffuseGI (quality-dependent)
  -> Temporal reservoirs / reprojection
  -> Shadow + reflection + GI denoisers
  -> DeferredLightingComposite
  -> Decals
  -> Water / glass / transparent geometry / particles
  -> Volumetrics / sky / weather
  -> Exposure / bloom / temporal reconstruction / upscale
  -> ToneMapAndGrade
  -> BridgeExport
```

BLAS work precedes TLAS reads; AS build writes use the precise acceleration-structure stage/access masks. Shader reads use the actual ray-tracing, compute, or graphics stages. Do not use `ALL_COMMANDS` as a substitute for a correct dependency model.

Transient aliasing is enabled only when logical lifetimes do not overlap and memory requirements are compatible. History, bridge, acceleration-structure, shader-binding-table, readback, and imported allocations are never transient-aliased.

### 9.5 Pipeline and shader system

- Author shaders in HLSL and compile offline to SPIR-V with DXC for Vulkan 1.3. Ship SPIR-V and reflection metadata; runtime compilation is a developer feature only.
- Generate descriptor layouts and C++ structure assertions from reflection so host and shader layouts cannot drift silently.
- Key raster pipelines by shader pack, pass class, vertex layout, attachment formats, sample count, and normalized fixed state.
- Keep original engine technique/domain IDs in diagnostics and material records, but do not use them as raw Vulkan pipeline keys.
- Persist a driver/device/shader-pack-specific `VkPipelineCache`. Reject caches whose UUID or design version differs.
- Compile expensive ray pipelines asynchronously during Observe/Mirror. Takeover requires all mandatory fallback pipelines to be ready.
- Hot reload creates a new shader-pack generation and resets affected temporal history; it never destroys pipelines referenced by submitted frames.

## 10. Ray-tracing architecture

### 10.1 Production rendering model

The default renderer is hybrid:

- Rasterization provides deterministic primary visibility, exact alpha coverage, decals, most transparencies, and a compact G-buffer.
- Ray queries provide low-overhead visibility/shadow tests from raster/compute stages.
- Ray-tracing pipelines provide reflection and diffuse-GI paths that benefit from miss/hit shaders and a shader binding table.
- An optional path-traced photo/debug mode replaces primary visibility after scene coverage and material validation are complete.

This matches the KHR design, which explicitly supports integrating ray-tracing pipelines with traditional rasterization.

### 10.2 BLAS policy

| Geometry | Build flags and update policy |
|---|---|
| Static opaque | Prefer fast trace, allow compaction, build once per content generation. |
| Static alpha test | Separate geometry/range where practical; any-hit enabled and compacted. |
| Rigid moving object | Reuse static BLAS; update only the TLAS transform. |
| Skinned fixed topology | Compute deform, allow update/refit; rebuild if measured quality or bounds degrade. |
| Topology-changing dynamic mesh | Rebuild from the dynamic ring allocation. |
| Terrain/cell static | BLAS per stable chunk/cell with background build and residency policy. |
| Dense foliage | Quality-dependent BLAS update frequency or raster-only shadow fallback. |
| Water/particles | Excluded from baseline TLAS unless a specialized representation is enabled. |

Each BLAS record stores source-buffer generations, geometry ranges, opacity classification, build flags, compacted size, last build/update timeline, and validation bounds.

### 10.3 TLAS and instance contract

Build one TLAS per semantically distinct view when visibility/projection differs. Reuse is allowed only when the instance set and transforms are valid for both views.

Suggested instance masks:

| Bit | Layer |
|---:|---|
| `0x01` | Opaque world and terrain. |
| `0x02` | Alpha-tested foliage/fences. |
| `0x04` | Characters and creatures. |
| `0x08` | First-person weapon/hands. |
| `0x10` | Glass/refraction candidates. |
| `0x20` | Water/special procedural candidates. |
| `0x40` | Shadow-only or hidden-from-primary geometry. |
| `0x80` | Debug/experimental geometry. |

`instanceCustomIndex` indexes a `GpuInstance` record containing geometry, material, previous transform, object ID, flags, and skin/deformation references. Shader-binding-table offsets select only broad hit-group classes (opaque, alpha test, specialized); material diversity stays in buffers to minimize SBT size and pipeline divergence.

### 10.4 Rays and lighting

- **Direct shadows:** one or more ray queries selected through the light list, with distance/cone/radius sampling based on light type. Preserve vanilla shadow eligibility/masks first.
- **Reflections:** one glossy/specular ray at full or checkerboard/half resolution, material-dependent roughness termination, sky/local probe miss behavior, and temporal-spatial denoising.
- **Diffuse GI:** quality-dependent half-resolution rays with temporal reservoirs and spatial reuse. Clamp/firefly handling occurs on radiance, not final color.
- **Ambient occlusion:** a short-range visibility term can be generated from the same TLAS; HBAO remains only as a fallback comparison path.
- **Transparency:** alpha-test any-hit samples the base opacity and threshold. Stochastic glass/transmission is a later specialized hit path; baseline glass remains rasterized.
- **Emissives:** material emissive contributes to visible radiance immediately. Emissive-light sampling requires an explicit light-selection structure and is enabled only when stable enough for denoising.

Ray origin offsets use scale-aware geometric/normal bias and must be tested against Fallout's world scale. Hardcoded “one unit” biases are prohibited.

### 10.5 Temporal reconstruction and denoising

Store separate histories for direct shadow, reflection, diffuse GI, exposure, and final reconstruction. Reprojection validates depth, normal, object/material ID, motion, and history epoch. Disocclusion clears history locally.

The baseline denoiser is an SVGF-style temporal accumulation plus edge-aware spatial passes; reservoirs may be added for light/GI sampling without changing the packet ABI. Every denoiser has a raw-signal debug view, confidence/history-length view, and a no-temporal mode for correctness diagnosis.

Dynamic resolution changes do not reinterpret old buffers in place. Histories are resampled only for small declared changes with matching projection; large changes increment `historyEpoch`.

## 11. D3D11/Vulkan interoperability

### 11.1 Bridge resource creation

The real D3D11 device creates a ring of shared 2D textures. Vulkan imports them; Vulkan-owned opaque Win32 memory is not presented to D3D11 as though it were a D3D texture.

For each candidate format and usage:

1. Query Vulkan external-image support with `VkPhysicalDeviceExternalImageFormatInfo` and handle type `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT`.
2. Create the D3D11 texture with compatible dimensions, format, bind flags, one mip/array layer/sample, and NT-handle sharing flags. Include keyed-mutex sharing when that synchronization fallback is selected.
3. Obtain an NT handle through `IDXGIResource1::CreateSharedHandle`.
4. Create a matching `VkImage` chained to `VkExternalMemoryImageCreateInfo`.
5. Import the handle with `VkImportMemoryWin32HandleInfoKHR`, use a dedicated allocation when required, bind it, and create only queried-compatible views.
6. Run a startup round-trip pattern test in both directions before enabling Mirror.

Khronos defines `D3D11_TEXTURE` handles as NT handles returned by `IDXGIResource1::CreateSharedHandle` for D3D10/11 textures: [`VkExternalMemoryHandleTypeFlagBits`](https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalMemoryHandleTypeFlagBits.html). Microsoft's [`IDXGIResource1::CreateSharedHandle`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgiresource1-createsharedhandle) documents the corresponding D3D resource requirements. External memory must always have matching external synchronization; see the [Khronos external-memory and synchronization guide](https://docs.vulkan.org/guide/latest/extensions/external.html).

Preferred bridge formats are FP16 HDR where cross-API capabilities allow it and `R8G8B8A8_UNORM` as the baseline SDR candidate. Every format remains runtime-queried. If an imported image cannot be a Vulkan storage/attachment target, Vulkan renders into a native image and copies/blits through a supported transfer path.

### 11.2 Shared-fence protocol

Preferred synchronization:

1. Query `ID3D11Device5` and `ID3D11DeviceContext4`.
2. Create an `ID3D11Fence` with `D3D11_FENCE_FLAG_SHARED` and export its handle.
3. Import it into a Vulkan timeline semaphore using `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT` (the alias of the D3D12 fence handle type).
4. Allocate monotonically increasing tickets; never infer ownership from frame number alone. Each slot stores its last `availableTicket`.

Per bridge slot:

```text
initially: availableTicket = the shared fence's completed initial value

Vulkan queue waits availableTicket
Vulkan writes bridge image and signals readyTicket = nextTicket()
D3D context Wait(readyTicket) before sampling/copying the image
D3D context Signal(consumedTicket = nextTicket()) after its final use
slot.availableTicket = consumedTicket for the next Vulkan use
```

The host enqueues the D3D `Wait(readyTicket)` only after the Vulkan queue submission that signals that ticket has returned `VK_SUCCESS`. A submission failure selects the prior completed bridge image and faults the backend; D3D must never wait on a speculative ticket.

The Vulkan extension accepts handles created by `ID3D11Device5::CreateFence`; timeline semaphores are the preferred modern import form. See [`VkExternalSemaphoreHandleTypeFlagBits`](https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalSemaphoreHandleTypeFlagBits.html), [`VK_KHR_external_semaphore_win32`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_external_semaphore_win32.html), and Microsoft's [`ID3D11Device5::CreateFence`](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nf-d3d11_4-id3d11device5-createfence).

If shared D3D11 fences are unavailable but external D3D11 textures and `VK_KHR_win32_keyed_mutex` are supported, use an `IDXGIKeyedMutex`/`VkWin32KeyedMutexAcquireReleaseInfoKHR` path. It is a compatibility fallback and may impose CPU blocking; it is benchmarked separately.

The bridge defaults to three slots. A slot is not reused until both APIs have completed their ticket sequence. Handle ownership and `CloseHandle` behavior follow the queried external-handle type rules and are encapsulated in one RAII object; copied code must not guess those rules.

### 11.3 Depth and stencil

Depth/stencil compatibility is staged:

1. **Initial takeover:** retain the vanilla D3D11 depth/stencil prepass and suppress later world shading. Vulkan creates its own primary depth for RT/post.
2. **Shared-depth experiment:** intercept target creation and query whether the exact typeless/depth resource can be created shareable and imported with all required Vulkan usages.
3. **Handoff fallback:** Vulkan exports linear/standard depth and a stencil-class mask. A D3D11 fullscreen pass writes `SV_Depth`; bounded passes with fixed stencil references reconstruct required stencil classes.
4. **Native mode:** Vulkan owns depth/stencil and all consumers, so no reconstruction is required.

The replacement cannot declare the depth prepass removed until depth value, stencil class, occlusion, decal, water, first-person, and every remaining D3D consumer pass their captures. Stencil is semantic data, not disposable padding.

### 11.4 UI and presentation

In the production compatibility path:

- Vulkan tone-maps/grades the world into the selected bridge slot.
- At the validated pre-Scaleform boundary, D3D11 waits on the ready ticket and copies or samples that image into the current engine UI/present target.
- Scaleform, Bink, the current ImGui D3D11 backend, Steam overlay, and compatible late overlays render normally.
- `Renderer::End` and the original DXGI `Present` retain sync interval, present flags, skip-next-present, and `WAS_STILL_DRAWING` retry semantics.
- VisualForge's existing D3D post-process is disabled in takeover mode or ported into the Vulkan graph; it must not grade/sharpen twice.

This preserves D3D11 overlay compatibility and avoids two swap chains competing for the same `HWND`.

Native WSI is enabled only after UI and middleware are rendered into Vulkan-consumable layers and engine back-buffer readers are gone. At that point the host intercepts presentation semantics, creates one Vulkan Win32 surface/swap chain, and provides explicit compatibility behavior for D3D11-only overlays rather than silently breaking them.

### 11.5 Resize, display transitions, and device faults

The hook around `Renderer::WindowSizeChanged` follows the mapped engine ordering:

1. Mark the backend draining and stop assigning new bridge slots.
2. Wait for only the bridge/internal timeline values that reference size-dependent images.
3. Release D3D views and Vulkan imports in the handle-type-correct order.
4. Call the original engine resize path, including its `ClearState`, `Flush`, target rebuilds, callback, and shadow-state invalidation.
5. Re-query actual client/back-buffer size and format; recreate bridge resources and run the interop pattern test.
6. Increment `historyEpoch`; use one vanilla frame if the bridge is not armed by the next begin.

Minimize/zero extent suspends Vulkan submission without spinning. Alt-tab/fullscreen transitions remain DXGI-owned in compatibility mode. D3D device removal and Vulkan device loss are logged as distinct faults with both APIs' diagnostic data.

## 12. Compatibility policy

Compatibility is explicit rather than implied:

| Component/mod class | Observe/Mirror | Takeover | Native WSI |
|---|---|---|---|
| Vanilla gameplay/save data | Unchanged | Unchanged | Unchanged |
| Mesh/texture/material mods using normal assets | Mirrored | Supported when their material class is covered | Supported |
| Scaleform/HUD/menu mods | Real D3D11 | Real D3D11 | Requires Vulkan UI backend/bridge |
| Bink video | Real D3D11 path | Real D3D11 path | Requires imported video layer or replacement |
| Flex/weapon debris | Real D3D11 path | Real D3D11 compatibility pass | Requires explicit bridge/replacement |
| NVIDIA HBAO/Godrays | Vanilla comparison | Disabled after equivalent Vulkan passes own those effects | Removed/replaced |
| Steam/NVIDIA/Discord-style D3D overlays | Normal | Normal because DXGI still presents | Requires Vulkan-capable overlay |
| VisualForge ImGui overlay | Existing D3D11 backend | Existing D3D11 backend | Port to Vulkan backend |
| D3D11 ReShade final-color effects | Normal | Sees final bridged color; depth access is not promised | Requires Vulkan ReShade path |
| ENB or plugins expecting vanilla world passes/targets | Normal | Not generally compatible; use Mirror/Off or a specific adapter | Not compatible without port |
| Vanilla D3D shader-replacement mods | Normal | Not used for Vulkan world shading | Requires a semantic Vulkan shader/material port |
| F4SE plugins reading mapped engine objects | Normal | Usually normal; renderer-resource consumers need review | Review against compatibility facade |
| Capture tools | D3D11 plus Vulkan process | API-specific captures; use renderer markers/correlation IDs | Vulkan capture |

In Takeover mode, maintain a D3D draw whitelist. Expected D3D work is UI, Bink/Flex, the declared depth/stencil compatibility path, bridge composition, and known overlays. Any other draw that writes a world target increments an unknown-writer counter and can force the next frame to vanilla.

Expose a small compatibility API so other F4SE plugins can query renderer mode, frame ID, output/depth bridge handles where safe, history epoch, and registered debug textures without discovering private globals.

## 13. Delivery stages and gates

### Stage 0 — host hardening and trace schema

Deliver:

- exact-build manifest and instruction/vtable validation;
- hook transaction/fault state machine;
- render-thread identification and frame correlation IDs;
- binary trace format for cameras, resource events, pass registration, actual D3D draws, target writes, and lifecycle events;
- a replay/inspection utility that runs outside the game.

Gate: running with renderer mode `Off` is behaviorally identical to the current plugin and every hook can be disabled independently without a crash or stale vptr.

### Stage 1 — Vulkan and interop bring-up

Deliver:

- lazy backend DLL load;
- LUID-matched Vulkan device and feature report;
- D3D11 shared texture import, shared-fence timeline, pattern tests, resize recreation, and bridge debug colors;
- Vulkan validation/debug names and timeline diagnostics.

Gate: 10,000 bridge exchanges plus repeated resize/fullscreen/alt-tab cycles complete with no CPU readback, validation error, timeout, corruption, or leaked handle.

### Stage 2 — resource mirroring

Deliver:

- cloned `BSShaderResourceManager` interface hooks;
- stable host handles/generations;
- vertex/index/texture/sampler translation;
- semantic `MaterialDesc` translation with per-field provenance, explicit texture views/swizzles, and static/dynamic GPU record generation;
- derived-texture cache for normal variance and alpha-coverage mips, with source/version hashes and no source-asset writes;
- streaming mip and deferred-destruction handling;
- offline vertex/format parity tests plus material serialization/decode fixtures.

Gate: a representative modded playthrough produces no unknown live resource referenced by a captured standard world pass, every referenced standard material replays with the same class/semantic bindings, no authored value is replaced by a heuristic, and registry/cache memory returns to a stable level after load/unload cycles.

### Stage 3 — scene and raster mirror

Deliver:

- current-build accumulator/batch method mapping;
- camera/view/light/instance packet generation;
- Vulkan G-buffer and specular/smoothness GGX compatibility lighting, sky, terrain, characters, alpha test, and material debug visualization;
- calibrated smoothness/specular transfer LUTs and reference light/environment sweeps;
- split/difference presentation while vanilla remains authoritative.

Gate: all actual D3D world-target draws are classified, every visible standard pass maps to an instance/material or a declared compatibility category, transforms and projection pass parity tests, normal orientation and alpha coverage agree, highlight width/peak response meet the material parity envelope, and no unclassified frame can become armed.

### Stage 4 — acceleration structures and RT mirror

Deliver:

- static/dynamic BLAS lifecycle and compaction;
- TLAS instance masks and per-view updates;
- ray-query shadows, reflections, optional diffuse GI, raw/denoised debug views;
- shared raster/hit material evaluation, ray-cone texture LOD, hemisphere-safe shading normals, and matching alpha any-hit semantics;
- temporal history reset and disocclusion validation.

Gate: AS validation is clean, bounds/instance counts match captured geometry, alpha-test shadows match raster coverage across mip distances, ray-hit material values match the raster evaluator, and camera cuts/resizes never reuse invalid history.

### Stage 5 — world-only takeover

Deliver:

- classified suppression at accumulator/batch submission;
- retained vanilla depth/stencil prepass;
- scene-color bridge into mapped engine targets;
- automatic fallback on unknown target writer/pass.

Gate: interiors, exteriors, settlement density, combat, first/third person, water/weather, loading, Pip-Boy, VATS, dialogue, local map, and power armor complete the capture matrix without missing geometry or target-state corruption.

### Stage 6 — world-and-post takeover

Deliver:

- exact pre-Scaleform handoff boundary;
- Vulkan exposure, bloom, volumetrics, motion blur as desired, temporal reconstruction/upscale, tone mapping, LUT/grading, and bridge composition;
- vanilla image-space suppression ledger keyed by the complete effect enum;
- depth/stencil handoff or replacement for every remaining D3D consumer.

Gate: no vanilla world/image-space draw remains outside the whitelist, UI and video are pixel/stability correct, and Present/resize behavior matches the engine contract.

### Stage 7 — native WSI and compatibility retirement

Deliver:

- Vulkan Win32 surface/swap chain, HDR/color-space policy, frame pacing, and presentation fault handling;
- Vulkan Scaleform/UI or an imported UI layer;
- Bink/Flex migration plan completed;
- engine D3D target/resource callers removed, bridged, or served by a deliberately scoped facade.

Gate: DXGI presentation and real D3D world resources can be disabled without an engine reader observing an invalid object. Native remains optional if the compatibility cost outweighs its benefit.

## 14. Validation and acceptance

### 14.1 Automated tests

- PE/build-manifest and Address Library resolution tests against the exact binary.
- RTTI/vtable validation for every cloned/hooked interface.
- Packet ABI size/alignment/version/CRC tests.
- Randomized vertex pack/unpack and matrix/projection parity tests.
- DXGI-to-Vulkan format/view compatibility table tests.
- Resource handle generation/reuse and timeline-retirement tests.
- Frame-graph dependency, alias lifetime, and barrier unit tests.
- BLAS/TLAS build-size/alignment/bounds tests on synthetic scenes.
- Shader reflection versus C++ layout assertions.
- Golden packet replay tests that render without Fallout running.

### 14.2 In-game capture matrix

At minimum capture:

- exterior day/night, rain/fog/radstorm, dense forest, open terrain, water shore/underwater;
- multiple interior lighting classes, workshop/settlement scenes, elevators/loading doors;
- first- and third-person weapons, scopes, muzzle flash, dismemberment, hair/skin/eyes;
- Pip-Boy normal/power-armor, VATS masks, dialogue camera, local/world map, terminals;
- transparent glass, particles, decals, blood, fire/smoke, godray/volumetric cases;
- pause/main/loading menus, Bink playback, Steam overlay, ImGui overlay;
- save/load, fast travel, cell reset, modded asset hot paths, dynamic-resolution changes;
- borderless/windowed/fullscreen, minimize/restore, monitor/display changes, repeated resize.

Each capture stores the host trace, Vulkan timestamps, pass coverage, resource registry deltas, raw RT buffers, final output, D3D whitelist counts, bridge tickets, and history reset events.

### 14.3 Takeover acceptance gates

Takeover is build/capability-signature specific and requires:

1. Zero unresolved mandatory hook sites and zero failed structural validations.
2. Zero unclassified D3D world-target writers across the qualifying capture set.
3. Every captured pass maps to Vulkan output or an explicit compatibility whitelist entry.
4. Zero Vulkan core/synchronization validation errors in the qualifying run.
5. No per-frame CPU GPU-readback or interop busy-wait.
6. No `vkDeviceWaitIdle` outside startup failure, destructive recreation, or shutdown.
7. Stable handle/memory counts after repeated world/save/resize cycles.
8. Deterministic fallback: forcing any gate false before `Renderer::Begin` yields a complete vanilla frame.
9. GPU markers prove bridge ownership never overlaps between APIs.
10. Crash logs identify hook generation, renderer state, last frame/pass, timeline tickets, Vulkan result, and D3D removal reason.

### 14.4 Performance accounting

Report separate costs for host capture, resource upload, deformation, BLAS, TLAS, raster, each RT effect, denoising, post, bridge wait/copy, D3D UI, and Present. Report GPU critical path and overlap, not the sum of pass durations.

Quality profiles control ray counts, GI/reflection resolution, denoiser iterations, animated BLAS distance/update rate, foliage behavior, and render scale. They do not change semantic capture or resource correctness.

## 15. Proposed source and build layout

```text
VisualForge/
  src/
    renderer_host/
      BuildGate.{h,cpp}
      HookManifest.{h,cpp}
      HookTransaction.{h,cpp}
      EngineAbi.{h,cpp}
      FrameCapture.{h,cpp}
      ResourceCapture.{h,cpp}
      MaterialCapture.{h,cpp}
      D3D11Interop.{h,cpp}
      RendererHost.{h,cpp}
    renderer_api/
      RendererApi.h
      PacketFormat.h
      ResourceEvents.h
  renderer/
    CMakeLists.txt
    src/
      VulkanDevice.*
      ExternalBridge.*
      ResourceRegistry.*
      MaterialTranslator.*
      DerivedTextureCache.*
      SceneDatabase.*
      FrameGraph.*
      DescriptorHeap.*
      PipelineCache.*
      AccelerationStructures.*
      RayTracing.*
      Temporal.*
      RendererBackend.*
    shaders/
      common/
        MaterialDecode.*
        SpecGlossBrdf.*
      raster/
      raytracing/
      denoise/
      post/
    tests/
  tools/
    trace_inspector/
    packet_replay/
    shader_reflect/
```

Build two targets:

- existing `VisualForge.dll`, with host sources and no mandatory Vulkan loader dependency;
- `VisualForgeRenderer.dll`, C++20, Vulkan 1.3 headers/loader, offline shader build steps, and the versioned C API export.

Use the installed Visual Studio/Windows SDK for D3D11.4 interfaces and DXC. Compile production shaders at build time and package them under a versioned renderer data directory. A header-only allocator such as AMD's official [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) is appropriate for ordinary allocations, but imported external images and AS-specialized allocations remain explicit paths.

The deployment remains fail-open: `VisualForge.dll` loads even when `VisualForgeRenderer.dll`, `vulkan-1.dll`, a shader pack, or an optional diagnostic library is absent.

## 16. Major risks and controlling decisions

| Risk | Control |
|---|---|
| Missing immediate/custom draw paths | Three-level accounting: accumulator registration, batch submission, actual D3D context writers; unknown writers disarm takeover. |
| Engine objects mutate after capture | Copy frame data, use generations, retain only mapped resources, and seal immutable packets. |
| D3D/Vulkan queue deadlock | One documented ticket state machine, monotonic values, timeout diagnostics, ownership markers, and stress tests. |
| Unsupported shared format/usage | Query every combination and use native Vulkan render plus a compatible transfer bridge. |
| Duplicate D3D/Vulkan VRAM pressure | Streaming-aware Vulkan budget, mip residency, content deduplication, and later suppression of proven-unused D3D resources. |
| Skinned/foliage BLAS cost | Distance/rate policy, refit where valid, rebuild diagnostics, raster fallback profile. |
| Stencil-dependent vanilla consumers | Retain prepass until a class mask and handoff are validated; never drop stencil implicitly. |
| UI boundary differs by mode | Trace and validate per frame class; boundary mapping is a takeover gate. |
| Shader/material mods expect D3D bytecode | Provide semantic override API and Mirror/Off fallback; do not claim transparent compatibility. |
| Driver/API fault after suppression | Present last completed frame, fault state, restore only at next safe Begin, require restart after device loss. |
| Game update invalidates addresses/layouts | Exact manifest and hashes; no “close enough” pattern fallback in Takeover. |
| Native WSI breaks overlays/middleware | Keep DXGI compatibility mode as the supported default until each dependency has a Vulkan path. |

## 17. First implementation slice

The first code milestone should stop before any draw suppression and deliver these pieces in order:

1. Add `renderer_api` POD headers and a trace file format.
2. Add `BuildGate`, `HookManifest`, and per-hook enable/rollback support to the existing host.
3. Replace renderer-wide `MH_EnableHook(MH_ALL_HOOKS)` usage with explicitly owned hook sets while preserving current VisualForge behavior.
4. Capture the real D3D device/adapter LUID at creation or first swap-chain initialization.
5. Add the lazy backend DLL and produce a complete Vulkan capability report for the matched adapter.
6. Implement one shared `R8G8B8A8_UNORM` bridge texture, shared D3D11 fence import, pattern round-trip, and resize recreation.
7. Expand to a three-slot bridge and display a Vulkan debug frame through the existing DXGI path without altering engine world submission.
8. Clone and instrument the `BSShaderResourceManager` vtable; log stable resource generations without creating Vulkan assets yet.
9. Map current-build accumulator/batch functions dynamically and prove pass/draw/target accounting in Observe mode.
10. Only then implement resource mirroring and a Vulkan raster Mirror frame.

This slice proves the two highest-risk foundations—engine coverage and cross-API ownership—without putting gameplay behind an incomplete renderer.

## 18. Source of truth

- [`engine_render.md`](engine_render.md) is the build-specific engine/ABI/address map and remains authoritative for current offsets.
- The installed executable and Address Library are authoritative over all semantic symbol sources.
- The existing `VisualForge/src/D3D11Hook.cpp` and `DepthCapture.cpp` prove current swap-chain/context interception and depth observations, but their Present-time post path is not the takeover architecture.
- Khronos [ray-tracing guide](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html), [external-memory guide](https://docs.vulkan.org/guide/latest/extensions/external.html), and current Vulkan reference pages define Vulkan behavior.
- Microsoft Learn pages for [`ID3D11Device5::CreateFence`](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nf-d3d11_4-id3d11device5-createfence), [`ID3D11DeviceContext4::Wait`](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext4-wait), [`ID3D11DeviceContext4::Signal`](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext4-signal), and [`IDXGIResource1::CreateSharedHandle`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgiresource1-createsharedhandle) define the D3D side of the GPU bridge.
- Runtime feature/format/handle queries override assumptions from any document or development machine.
