# Fallout 4 renderer map and Vulkan replacement notes

This document maps the renderer in the installed `Fallout4.exe` and records the ABI that a replacement renderer must preserve. It is deliberately build-specific where exact addresses are shown, and build-independent where a semantic interface is more useful.

The companion implementation architecture is [`vulkan_renderer_design.md`](vulkan_renderer_design.md).

The immediate conclusion is important: Fallout 4's renderer is not isolated behind `Present`. D3D11 COM pointers are embedded in engine objects, passed through render-target management and shader code, and handed to Scaleform and NVIDIA middleware. A Vulkan backend therefore needs either a D3D11/DXGI compatibility facade or a staged replacement above the engine's D3D11 resource layer. A swap-chain-only hook is useful for instrumentation and coexistence, but is not a renderer replacement.

## 1. Notation and confidence

All addresses in this document are module-relative virtual addresses (RVAs). At runtime:

```text
absolute address = Fallout4.exe module base + RVA
```

The executable is ASLR-enabled; never encode the preferred image base as a runtime address.

Evidence tags:

| Tag | Meaning |
|---|---|
| **[BIN]** | Read directly from this exact executable: PE metadata, bytes, imports, strings, x64 unwind data, RTTI, vtables, or disassembly. |
| **[AL]** | Resolved through this build's F4SE Address Library, `version-1-11-221-0.bin`. |
| **[ABI]** | Layout or enum corroborated by current CommonLibF4 headers and checked against this binary's RTTI/uses. |
| **[SEM]** | Semantic name taken from a different-build symbol source or cross-build implementation, then matched structurally. It is not an address claim by itself. |
| **[ASSET]** | Read directly from an installed BA2 header, name table, or asset payload without changing the archive. |
| **[OBS]** | Observed by the existing local VisualForge instrumentation while the game was running. |
| **[INF]** | Inference that still needs a debugger/capture confirmation. |

An address shown without an explicit qualification refers only to the executable fingerprint below.

## 2. Exact target build

| Property | Value |
|---|---|
| File | `Fallout4.exe` |
| File/product version | `1.11.221.0` |
| Size | `55,293,864` bytes |
| SHA-256 | `428F9996CC4248E26C0F62F9FDD3EAF0E5EB305834B67EE5996538E593218B61` |
| PE machine | AMD64 / PE32+ |
| Preferred image base | `0x140000000` |
| `SizeOfImage` | `0x4244000` |
| Entry-point RVA | `0x420A310` (inside the Steam binder) |
| PE timestamp | `0x69E2A744` (`Sat Apr 18 2026 07:33:56` as decoded without asserting a timezone) |
| Linker version | Microsoft 14.16 |
| Address Library | `Data/F4SE/Plugins/version-1-11-221-0.bin`, 10,425,304 bytes, 651,581 ID/RVA records, SHA-256 `2FDCC2A0926659C37255D2EAF335775240EC7FEFDF6CB3B35B063FACA25A448F` |

### 2.1 PDB identity warning

The executable's CodeView record requests:

```text
GUID: {22776620-B648-4C12-98F7-D51833DAFFC9}
Age:  1
Path: E:\BuildAgent\work\45d9abfbfc210894\scripts\_workspace\Code\Build\PC\Fallout4.pdb
```

The locally installed `Data/F4SE/Plugins/Fallout4.pdb` is **not** that PDB:

```text
Local GUID:   {EE416AC2-2E44-4488-8C8F-FCFEFFE820BE}
Local SHA256: EEB5AB9E0CCC77D8D75B638327C67EB5B914D6374BF021B308B449EF0FB7D318
```

Its section layout also differs. Its names are useful as **[SEM]** evidence, but its RVAs, line records, and global addresses must not be applied to 1.11.221. Exact current addresses below come from the current Address Library or independent binary matching.

## 3. PE and `.rdata` map

| Section | RVA | Virtual size | Raw size | Protection | Renderer relevance |
|---|---:|---:|---:|---|---|
| `.text` | `0x00001000` | `0x242B2BC` | `0x242B400` | R-X | Engine code and almost all render call sites. |
| `.interpr` | `0x242D000` | `0xA6D0` | `0xA800` | R-X | Secondary executable/interpreter region. |
| `.rdata` | `0x2438000` | `0xA94BE2` | `0xA94C00` | R-- | Vtables, Complete Object Locators, class hierarchies, strings, constants, imports. |
| `.data` | `0x2ECD000` | `0xF923B0` | `0x20D200` | RW- | TypeDescriptors, globals, singletons, settings, and a large zero-filled tail. |
| `.pdata` | `0x3E60000` | `0x27E400` | `0x27E400` | R-- | 217,856 x64 `RUNTIME_FUNCTION` entries; authoritative function fragments/unwind roots. |
| `_RDATA` | `0x40DF000` | `0xE20` | `0x1000` | R-- | Additional compiler/runtime read-only data. |
| `.rsrc` | `0x40E0000` | `0xA67A0` | — | R-- | Win32 resources. |
| `.reloc` | `0x4187000` | `0x8219C` | — | R-- | ASLR relocations; useful for pointer validation. |
| `.bind` | `0x420A000` | `0x39048` | `0x39048` | R-X | Steam binder/entry wrapper; avoid as a renderer hook surface. |

Static inventory for this build:

- 775 imported functions.
- 12,261 MSVC RTTI TypeDescriptors. TypeDescriptors live primarily in `.data`; Complete Object Locators, hierarchy descriptors, and vtables live in `.rdata`.
- 162 type names begin with `BSImagespaceShader`. Of these, 161 belong to the actual `BSImagespaceShader` hierarchy (the base plus 160 derived classes). `BSImagespaceShaderCopyParam` is a separate `ImageSpaceEffectParam` helper.
- 32 `ImageSpaceEffect*` types including the base effect.

This placement matters to a runtime mapper: scanning only `.rdata` for class names misses the TypeDescriptors, while scanning only `.data` misses the vtables. The reliable route is TypeDescriptor name -> Complete Object Locator references -> vtable at `COL pointer + 8`.

## 4. Renderer architecture

```text
Main::DrawWorld / DrawWorld::* phase orchestration
        |
        +-- visibility: NiCamera -> BSCullingProcess -> scene nodes
        |                                  |
        |                                  v
        |                         BSShaderAccumulator
        |                                  |
        |                                  v
        |                         BSBatchRenderer
        |                    (pass lists + geometry groups)
        |                                  |
        |                                  v
        |           BSShader domains + BSShaderResourceManager
        |                                  |
        +--------------------------> BSGraphics::Renderer
        |                         / Context / shadow state
        |                                  |
        |                                  v
        |                    D3D11 immediate/deferred contexts
        |
        +-- RenderTargetManager -> named RT/depth/cube registries
        |
        +-- ImageSpaceManager -> ImageSpaceEffect graph -> imagespace shaders
        |
        +-- Scaleform UI + Bink + GFSDK HBAO/Godrays + Flex
                                           |
                                           v
                                IDXGISwapChain::Present
```

The world renderer is a deferred/forward hybrid. Cross-build symbol families and the current `DrawWorld::Forward` body expose the following semantic phase graph **[SEM+BIN]**:

1. `Begin` / camera and frame-state setup.
2. Main accumulation and visibility traversal.
3. Main render setup.
4. Deferred prepass, including LOD and blended-decals work.
5. AO (`NvidiaHBAO`, imagespace AO, or SAO variants).
6. Deferred decals.
7. Deferred lights.
8. Deferred composite.
9. Forward opaque/special passes.
10. Forward alpha, reticle, refraction, water, and first-person work.
11. Image-space/end-of-frame effects.
12. Pre-UI, UI/Scaleform, and post-UI work.
13. Renderer `End` and swap-chain presentation.

Other named families in the semantic symbol source include motion blur, shadow-map generation, Umbra/portal culling, first-person rendering, water, `Main::DrawWorld_PreRender`, `Main::DrawWorld_And_UI`, `BSShaderUtil::RenderScene`, and `BSShaderUtil::RenderSceneDeferred`. These names establish responsibilities, not current RVAs.

## 5. Current-build address anchors

### 5.1 Functions

| Function / role | Address Library ID | RVA | Evidence and notes |
|---|---:|---:|---|
| `BSGraphics::Renderer::Init` | — | `0x1815BB0` | **[BIN+SEM]** Exact body matched from arguments, `RendererData` offsets, window creation, device initialization, target creation, and `Context` allocation. Logical range ends at `0x18168CC`. |
| `Renderer::WindowSizeChanged` | 2276824 | `0x18174F0` | **[AL+BIN+SEM]** Uses a `0x50`-byte window stride; logical range `0x18174F0..0x1817892` spans chained unwind fragments. |
| `Renderer::Lock` | 2276828 | `0x1817DA0` | **[AL+BIN]** Enters the critical section at `Renderer + 0x2590` (`RendererData + 0x2580`). |
| `Renderer::Unlock` | 2276829 | `0x1817DC0` | **[AL+BIN]** Leaves the same critical section. |
| `Renderer::Begin` | 2276833 | `0x1817E30` | **[AL+BIN]** Selects a window, publishes it, advances frame state, and installs its RT state. |
| `Renderer::End` | 2276834 | `0x1818080` | **[AL+BIN]** Calls swap-chain vtable slot 8 (`Present`). |
| `Renderer::CreateTriShape(CPU)` | — | `0x1818760` | **[BIN+OBS]** Verified concrete mesh-creation boundary. Takes a size pointer, packed CPU vertex data, `VertexDesc`, and `IndexBuffer*`; used by the opt-in one-shot Phase 7 capture. |
| `Renderer::IncRef(Buffer*)` | 2276869 | `0x181A2C0` | **[AL]** Buffer lifetime entry point. |
| `Renderer::DecRef(Buffer*)` | 2276870 | `0x181A2D0` | **[AL]** Buffer lifetime entry point. |
| Graphics OS/device initialization | — | `0x1824470` | **[BIN]** Logical function with chained unwind fragments; calls DXGI/D3D11 creation and initializes GFSDK SSAO. |
| Set dynamic-resolution viewport as default | — | `0x1839A00` | **[BIN+SEM]** Call target at `DrawWorld` owner `+0xC5`; the cross-build hook gives the semantic name. |
| `RenderTargetManager::SetEnableDynamicResolution` | 2277197 | `0x1839E10` | **[AL]** Public dynamic-resolution switch. |
| Target materialization/rebuild pass | — | `0x183ABE0` | **[BIN]** Iterates registered RT and depth handles and frees/recreates resources from manager state; range `0x183ABE0..0x183AF3C`. Exact original name is unknown. |
| `BSGraphics::Utility::PackVertexData` | 2277106 | `0x182E0E0` | **[AL+ABI]** Packs engine vertex streams into the `VertexDesc` layout. |
| `BSGraphics::Utility::ConvertHALFToNiPoint3Stream` | 2277113 | `0x18310C0` | **[AL]** Half-to-position stream conversion. |
| `BSGraphics::Utility::ConvertNiPoint3ToHALFStream` | 2277114 | `0x1831150` | **[AL]** Position-to-half stream conversion. |
| `BSGraphics::Utility::UnpackVertexData` | 2277129 | `0x1833C00` | **[AL+ABI]** Decodes packed attributes; useful parity oracle for the Vulkan vertex translator. |
| `NiCamera::BoundInFrustum` | 2194525 | `0x365F20` | **[AL]** Scene bound/frustum test. |
| `NiCamera::ViewPointToRay` | 2270338 | `0x16D1060` | **[AL]** Viewport-to-world ray conversion. |
| `NiCamera::WorldPtToScreenPt3` | 2270344 | `0x16D2040` | **[AL]** World-to-screen conversion. |
| `BSShaderProperty::SetMaterial` | 2316285 | `0x2161BF0` | **[AL+ABI]** Installs/copies a material on a shader property. |
| `BSShaderTextureSet::CreateObject` | 2316324 | `0x2162680` | **[AL]** Texture-set construction. |
| `BSShaderUtil::SetMaterialAlpha` | 2317566 | `0x21BB670` | **[AL]** High-level material-alpha mutation. |
| `BSScaleformRenderer::Initialize` | 2284938 | `0x1A89BC0` | **[AL]** UI backend initialization boundary. |
| `BSCullingProcess::Process` variant 1 | 2275931 | `0x17E17A0` | **[AL]** Visibility/accumulation boundary. |
| `BSCullingProcess::Process` variant 2 | 2275932 | `0x17E19F0` | **[AL]** Visibility/accumulation boundary. |
| `DrawWorld::Forward` | 2318315 | `0x21F16D0` | **[AL+BIN+SEM]** Current high-level forward phase; range `0x21F16D0..0x21F1DA3`. Calls RT manager, camera-state, image-space, and alpha/reticle work. |
| Forward reticle/alpha sub-call | parent + `0x53D` | `0x21F1C0D` | **[BIN+SEM]** A useful before/after-alpha interception point demonstrated by a current Post-NG hook. |
| Draw-world dynamic-resolution/imagespace owner | 2318322 | `0x21F27D0` | **[AL+BIN+SEM]** Calls the viewport-default function at `+0xC5`; also invokes image-space processing. |

### 5.2 Globals and singleton slots

| Object / slot | Address Library ID | RVA | Storage semantics |
|---|---:|---:|---|
| `BSGraphics::RenderTargetManager` | 2666735 | `0x2F42760` | Direct singleton object, size `0xFF0`. |
| `BSGraphics::State` | 2704621 | `0x3D70920` | Direct singleton object, size `0x3C0`. |
| `BSGraphics::GetRendererData` | 2704429 | `0x38CAB20` | Pointer slot (`RendererData**`). |
| Current `RendererWindow` | 2704431 | `0x38CAB30` | Pointer slot (`RendererWindow**`). |
| `BSShaderResourceManager` | 2703483 | `0x3438128` | Singleton pointer slot. Its exact 52-slot vtable is at RVA `0x29139A8`; all entries are pinned by the Phase 7 runtime contract. |
| `ImageSpaceManager` | 2712627 | `0x3E47B70` | Singleton pointer slot, object size `0x1B8`. |
| Camera near scalar | 2712882 | `0x3E4B8F4` | Float global used by current upscaling integrations. |
| Camera far scalar | 2712883 | `0x3E4B8F8` | Float global used by current upscaling integrations. |
| `ImageSpaceEffectTemporalAA::MaskS` | 2713052 | `0x3E5AA14` | Static mask/state byte or scalar; type still needs live confirmation. |
| `ImageSpaceEffectHDR::UsePipboyScreenMask` | 4784697 | `0x2F8D484` | Static boolean. |

`Renderer::Init` also publishes raw device/context aliases around `0x38CAB10..0x38CAB38`. Use the named `RendererData**` slot and its fields as the canonical source; the adjacent aliases should be treated as implementation details until their readers are completely mapped.

## 6. Startup, device, frame, and presentation

### 6.1 `Renderer::Init` (`0x1815BB0`)

The exact current body performs these operations **[BIN]**:

- Copies application width/height into `BSGraphics::State` back-buffer and screen-size fields.
- Writes fullscreen, borderless, and vsync booleans into `RendererData + 0x30..0x32`.
- Stores adapter selection, desired refresh rate, `HWND`, window class, and instance information.
- Creates/configures the Win32 renderer window.
- Calls the graphics OS/device initializer rooted at `0x1824470`.
- Calls the render-target materialization pass at `0x183ABE0`.
- Allocates exactly `0x2FF0` bytes for `BSGraphics::Context` and initializes it.

The `0x2FF0` allocation independently confirms the current CommonLibF4 `Context` layout.

### 6.2 D3D11/DXGI creation (`0x1824470` logical root)

The routine calls `CreateDXGIFactory`, enumerates the selected adapter/output and display modes for DXGI format 28, then calls `D3D11CreateDeviceAndSwapChain` **[BIN]**.

The initial swap-chain descriptor is:

| Field | Value |
|---|---|
| Width / height | Current `BSGraphics::State` dimensions |
| Format | `DXGI_FORMAT_R8G8B8A8_UNORM` (28) |
| Sample count / quality | 1 / 0 |
| Buffer usage | `0x430`: shader input (`0x10`) + render-target output (`0x20`) + unordered access (`0x400`) |
| Buffer count | 2 |
| Windowed | 1 in this creation call |
| Swap effect | 0, `DXGI_SWAP_EFFECT_DISCARD` |
| Flags | 2, `DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH` |
| Requested feature-level array | Null; output feature level is initialized to `0x9100` before the call |
| Outputs | Swap chain -> first `RendererWindow + 0x18`; device -> `RendererData + 0x48`; immediate context -> `+0x50` |

After creation it publishes the engine's device/context/window globals and calls `GFSDK_SSAO_CreateContext_D3D11`. This ordering means a Vulkan replacement must decide how HBAO sees a compatible D3D11 device before normal rendering begins.

### 6.3 `Renderer::Begin` (`0x1817E30`)

`Begin(windowID)` performs per-frame/context preparation, checks foreground/current-window state, advances the `State` frame values, finds the window using a `0x50`-byte stride, publishes the selected `RendererWindow*`, mirrors its swap-chain target into renderer target state, invokes target-manager work, and resets per-frame globals **[BIN]**.

This is a strong frame boundary for resource retirement, command-pool reset, timeline-semaphore advancement, and instrumentation. It is too low-level to recover semantic pass names by itself.

### 6.4 `Renderer::End` (`0x1818080`)

`End()` calls `IDXGISwapChain::Present` through vtable offset `0x40` (slot 8), with:

```text
SyncInterval = RendererData.presentInterval  // +0x40
Flags        = BSGraphics::State.presentFlag // +0x13C
```

It brackets the call with an engine `inPresent` flag. If flags equal 8 and `Present` returns `0x887A000A` (`DXGI_ERROR_WAS_STILL_DRAWING`), it loops using `SwitchToThread`. `Renderer.skipNextPresent` suppresses the call once and is then cleared **[BIN]**.

The replacement must reproduce skip-present semantics and the engine-visible completion behavior even if Vulkan presentation itself is asynchronous.

### 6.5 Existing local D3D11 observations

VisualForge currently hooks swap-chain slots 8 (`Present`) and 13 (`ResizeBuffers`), plus immediate-context slots 33 (`OMSetRenderTargets`), 34 (`OMSetRenderTargetsAndUnorderedAccessViews`), and 53 (`ClearDepthStencilView`). Its runtime self-test has observed **[OBS]**:

- Main scene depth is `R24G8_TYPELESS` at back-buffer resolution.
- Depth uses conventional/standard Z and is cleared to 1.0.
- The relevant full-resolution depth clear occurs once per world frame.
- Copying at `Present` is too late because later work has cleared/reused depth; the useful snapshot is immediately before the next clear.

These are valuable facts for a bridge implementation, but should be revalidated in interiors, exteriors, menus, loading screens, and every antialiasing/dynamic-resolution mode.

### 6.6 Resize path (`0x18174F0`)

The current `WindowSizeChanged(windowID)` body gives the required resize ordering **[BIN]**:

1. Validate the selected `RendererWindow`/swap chain, call `GetClientRect`, and query swap-chain fullscreen state (vtable slot 11).
2. If dimensions changed, call the active D3D11 context's `ClearState` and `Flush` (context vtable slots 110 and 111).
3. Invalidate both current and last-draw `RendererShadowState` blocks via current helper RVA `0x182AB70`.
4. Release and null the window's swap-chain render-target resource/views and their renderer mirrors.
5. Run the target rebuild/materialization routine at `0x183ABE0` in the cleared state.
6. Call swap-chain slot 13, `ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH)`.
7. Run resize bookkeeping, invoke `0x183ABE0` again, call a registered reset callback if present, restore target mirrors, recompute dynamic-resolution/viewport values, update stored width/height, and invalidate shadow state again.

The zero dimensions deliberately ask DXGI to use the current client area. A Vulkan implementation must wait for all users of old swap-chain images, destroy/recreate size-dependent views and targets in the same visible order, republish target handles before subsequent engine calls, and fire the equivalent reset callback.

## 7. Graphics ABI and structure layouts

These layouts are part of the effective binary interface. Existing engine code indexes fields directly, so a replacement that leaves engine callers intact must preserve the address, size, alignment, and ownership meaning of every exposed object.

### 7.1 `BSGraphics::Renderer` and `RendererData`

`Renderer` is `0x25D0` bytes **[ABI+BIN]**:

| Offset | Size | Field |
|---:|---:|---|
| `0x0000` | 1 | `skipNextPresent` |
| `0x0008` | 8 | `resetRenderTargets` callback |
| `0x0010` | `0x25C0` | Embedded `RendererData` |

`RendererData` (`0x25C0`):

| Offset | Field | Backend implication |
|---:|---|---|
| `0x0000` | `RendererShadowState* shadowState` | Cached API state; currently opaque `0x910` bytes. |
| `0x0008` | `BSD3DResourceCreator* resourceCreator` | Engine resource-construction facade. |
| `0x0010` | `uint32 adapter` | Adapter selection. |
| `0x0014` | desired `DXGI_RATIONAL` refresh | Must be translated to display/surface policy. |
| `0x001C` | actual `DXGI_RATIONAL` refresh | Engine-visible result. |
| `0x0024` | `DXGI_MODE_SCALING` | Window/display policy. |
| `0x0028` | `DXGI_MODE_SCANLINE_ORDER` | Window/display policy. |
| `0x002C` | `int32 fullScreen` | Fullscreen state. |
| `0x0030..34` | app fullscreen, borderless, vsync, initialized, pending resize | Live state flags. |
| `0x0038/3C` | pending width/height | Resize request. |
| `0x0040` | `presentInterval` | Present pacing input. |
| `0x0048` | `ID3D11Device*` | Hard D3D11 ABI exposure. |
| `0x0050` | `ID3D11DeviceContext*` | Hard D3D11 ABI exposure. |
| `0x0058` | `RendererWindow renderWindow[32]` | Multi-window array, `0x50` each. |
| `0x0A58` | `RenderTarget renderTargets[101]` | Live color/UAV resource handles. |
| `0x1D48` | `DepthStencilTarget depthStencilTargets[13]` | Live depth/stencil views. |
| `0x2500` | `CubeMapRenderTarget cubeMapRenderTargets[2]` | Live cube targets. |
| `0x2580` | `BSCriticalSection rendererLock` | Renderer-wide lock used by public `Lock`/`Unlock`. |
| `0x25A8` | `const char* className` | Win32 class name. |
| `0x25B0` | instance pointer | Win32/module instance. |
| `0x25B8` | `nvapiEnabled` | Vendor-feature state. |

`RendererWindow` is `0x50`: `HWND` at `0x00`, x/y at `0x08/0x0C`, width/height at `0x10/0x14`, `IDXGISwapChain*` at `0x18`, and an embedded `RenderTarget` at `0x20`.

### 7.2 Live resource handles

| Type | Size | Layout |
|---|---:|---|
| `RenderTarget` | `0x30` | texture `+0x00`, copy texture `+0x08`, RTV `+0x10`, SRV `+0x18`, copy SRV `+0x20`, UAV `+0x28`. |
| `DepthStencilTarget` | `0x98` | texture `+0x00`; normal DSV[4] `+0x08`; read-only-depth DSV[4] `+0x28`; read-only-stencil DSV[4] `+0x48`; read-only-both DSV[4] `+0x68`; depth SRV `+0x88`; stencil SRV `+0x90`. |
| `CubeMapRenderTarget` | `0x40` | texture `+0x00`, face RTV[6] `+0x08`, SRV `+0x38`. |

The four DSV arrays encode multisample/view variants and read-only binding modes. A Vulkan registry must map one logical depth image to the required `VkImageView` variants and enforce layout/access compatibility that D3D11 handled implicitly.

### 7.3 Buffers and reference/lifetime state

`BSGraphics::Buffer`, `VertexBuffer`, and `IndexBuffer` are all `0x50` **[ABI]**:

| Offset | Field |
|---:|---|
| `0x00` | `ID3D11Buffer* buffer` |
| `0x08` | CPU `data` pointer |
| `0x10` | linked-list `next` |
| `0x18` | SRV |
| `0x20` | UAV |
| `0x28` | request event to wait on |
| `0x30/34` | maximum/current data size |
| `0x38` | reference count |
| `0x3C/40/44` | atomic SRV acquire, UAV acquire, and pending-request counts |
| `0x48` | data offset |
| `0x4C/4D/4E` | invalid CPU data, heap allocated, volatile pending-copy flags |

These fields show that resource ownership is not simple COM reference counting. The Vulkan backend needs an ABI-compatible facade plus deferred destruction keyed to GPU completion. `Renderer::IncRef`/`DecRef`, the acquire counters, pending request count, event, and pending-copy flag all need tracing before the facade can safely retire a `VkBuffer`.

`TextureHeader` is eight bytes: height `+0`, width `+2`, mip count `+4`, format `+5`, flags `+6`, tile mode `+7`. One public header comment labels the mip byte as offset 3, but the C++ layout and byte sequence put it at 4.

### 7.4 Shader records and constant groups

`ConstantGroup` is `0x18`: D3D constant buffer at `+0x00`, CPU float data at `+0x08`, and `dataIsCPUWorkBuffer` at `+0x10`.

| Record | Size | Important fields |
|---|---:|---|
| Vertex shader | `0x88` | ID `+0`, COM shader `+8`, bytecode size `+0x10`, three `ConstantGroup`s `+0x18`, descriptor `+0x60`, 32-byte constant table `+0x68`. |
| Hull shader | `0x88` | Same layout. |
| Domain shader | `0x88` | Same layout. |
| Compute shader | `0x88` | Same layout. |
| Pixel shader | `0x78` | ID `+0`, COM shader `+8`, three groups `+0x10`, constant table `+0x58`; no matching stored bytecode-size/descriptor fields. |

The persistent CPU constant data and small lookup tables are a favorable translation point: retain the groups and dirty/update behavior, but back them with per-frame Vulkan uniform/storage rings and descriptor writes.

### 7.5 `BSGraphics::Context` (`0x2FF0`)

The context is large because it owns the per-thread/per-command state cache and preallocated constant buffers:

| Offset | Field |
|---:|---|
| `0x0000` | `ID3D11DeviceContext* deferredContext` |
| `0x0008` | `ID3D11Buffer* shaderConstantBuffer[541]` |
| `0x10F0` | VS technique CB[20] |
| `0x1190` | VS material CB[10] |
| `0x11E0` | VS geometry CB[28] |
| `0x12C0` | HS technique CB[20] |
| `0x1360` | HS material CB[10] |
| `0x13B0` | HS geometry CB[20] |
| `0x1450` | DS technique CB[20] |
| `0x14F0` | DS material CB[10] |
| `0x1540` | DS geometry CB[20] |
| `0x15E0` | PS technique CB[36] |
| `0x1700` | PS material CB[20] |
| `0x17A0` | PS geometry CB[40] |
| `0x18E0` | CS technique CB[20] |
| `0x1980` | CS material CB[20] |
| `0x1A20` | CS geometry CB[34] |
| `0x1B30..48` | alpha-test, per-frame, compute, and instance-transform CBs |
| `0x1B50` | miscellaneous `ConstantGroup` |
| `0x1B70` | current `RendererShadowState` (`0x910`) |
| `0x2480` | last-draw `RendererShadowState` (`0x910`) |
| `0x2D90` | dynamic VB[8] |
| `0x2DD0` | availability query[8] |
| `0x2E10` | availability integers[8] |
| `0x2E30/34` | current dynamic VB and offset |
| `0x2E38/40` | shared particle index/static buffers |
| `0x2E48..2FB0` | three `ConstantGroup`s for each VS, PS, DS, HS, CS stage |
| `0x2FB0` | input-layout hash map |
| `0x2FE0` | particle input layout |

The paired `shadowState` / `lastDrawCallShadowState` strongly suggests dirty-state comparison before D3D11 calls. Fully decoding the `0x910` layout is a high-priority dynamic task: it is likely the shortest path to reconstructing graphics pipeline state, descriptor bindings, viewport/scissor, and draw parameters without intercepting every individual COM call.

### 7.6 Vertex and draw geometry

`VertexDesc` is a packed 64-bit value:

- Low nibble: stride in four-byte units.
- Bits 4–7: dynamic stride in four-byte units.
- Attribute offsets: packed four-bit units at bits 8, 12, 16, 20, 24,
  28, 32, and 36, each also in four-byte units.
- Flags are position bit 44, UV0 bit 45, UV1 bit 46, normal bit 47,
  tangent/binormal bit 48, color bit 49, skin bit 50, landscape bit 51,
  eye bit 52, and full precision bit 54. Bit 53 is not accepted without new
  binary evidence.

The exact observed storage model is **[BIN+OBS]**:

| Semantic | Storage | Decode |
|---|---|---|
| Position | four IEEE binary16 values | XYZ plus retained W |
| UV0 | two binary16 values | direct |
| UV1 | four binary16 values | direct |
| Normal/tangent | four bytes | `byte * (2/255) - 1` |
| Color | four bytes | UNORM8 |
| Skin | four binary16 weights followed by four byte indices | direct |
| Landscape | two four-byte vectors | two UNORM8x4 values |
| Eye | one 32-bit float | direct |

The engine's current `PackVertexData` and `UnpackVertexData` bodies were used
as the disassembly oracle. Six layouts, including live descriptor
`0x000BB00605430208`, pass seeded byte-for-byte pack/unpack parity. That live
layout has a 32-byte stride and seven attributes: position, UV0, normal,
tangent, color, and the two landscape vectors.

`TriShape` is `0x20`: `VertexDesc +0`, `VertexBuffer* +8`, `IndexBuffer* +0x10`, UI/reference count `+0x18`. It maps naturally to cached `VkVertexInputBindingDescription`/`VkVertexInputAttributeDescription` data plus buffer/offset bindings.

At concrete creation RVA `0x1818760`, the current build derives stride as
`(VertexDesc * 4) & 0x3C`, creates the vertex buffer, increments the supplied
index-buffer reference, and constructs the shape. Both buffer classes expose
their retained CPU data at `+0x08`; current byte size is `+0x34`. A live
one-shot capture at this boundary produced 289 vertices and 1,536 indices with
bounds `[-2048,-2048,-580]..[0,0,348]`, then replayed through Vulkan with zero
validation errors and no engine draw suppression **[OBS]**.

### 7.7 Camera and frame state

`ViewData` is `0x210` and contains viewport/depth range, camera up/right/direction, then view, projection, view-projection, unjittered, current unjittered, previous unjittered, and inverse first-person projection matrices. `CameraStateData` is `0x250` and adds position-adjust histories, a reference `NiCamera*`, and `useJitter`.

`BSGraphics::State` (`0x3C0` at RVA `0x3D70920`) is the central frame/history record:

| Offset | Field group |
|---:|---|
| `0x000` | current frame; jitter X/Y; current/previous frame offsets |
| `0x014` | `FogStateType` (`0x60`) |
| `0x074` | multisample level |
| `0x078..084` | back-buffer and screen dimensions |
| `0x088` | framebuffer viewport |
| `0x098/09C` | frame count and frame ID |
| `0x0A0..A4` | inside-frame, letterbox, depth-as-texture, shadows, compiled-shader flags |
| `0x0A8/AC` | TAA state and disable counter |
| `0x0B0` | trijuice state |
| `0x0B8..130` | default textures/LUT/noise/normal/diffuse/spline/dissolve resources |
| `0x138/13C` | immediate-present threshold and present flags |
| `0x140` | camera-state cache array |
| `0x160` | current `CameraStateData` (`0x250`) |
| `0x3B0/3B1` | commit-textures-on-create and immediate-texture-load flags |

Temporal AA, reprojection, motion-vector generation, dynamic resolution, and first-person rendering all depend on this history. A Vulkan replacement must update it at the same semantic points, not merely produce equivalent final pixels.

The scene-facing `NiCamera` is `0x1A0`, TypeDescriptor RVA `0x3098988`, with a 58-slot vtable at `0x267DD60` **[BIN+ABI]**. It embeds the world-to-camera matrix at `0x120`, frustum at `0x160`, minimum near plane at `0x17C`, maximum far/near ratio at `0x180`, viewport at `0x184`, and LOD adjustment at `0x194`. Its public helpers cover bound/frustum tests, world-to-screen projection, and viewport-point-to-ray conversion. Correlate this camera with `BSGraphics::CameraStateData.referenceCamera` and the copied current/previous matrices; changing only one representation will break culling or temporal reprojection.

### 7.8 Implemented view/frame replay boundary

The replacement now has an offline camera contract that preserves the mapped
state without exposing engine pointers to the backend:

- `CapturedView` records source matrix storage/vector convention, handedness,
  projection mode, stable view/camera identities, render target/mode, extent,
  viewport/scissor, clip planes, render scale, AA mode, current/previous
  jitter, and camera-relative current/previous origins.
- Translation normalizes the source convention once and applies one explicit
  D3D-to-Vulkan Y conversion. It derives and cross-validates current,
  previous, inverse, and unjittered view-projection matrices.
- `.vfframe` version 1.0 is a pointer-free, CRC-protected frame envelope with
  fixed 96-byte header, 640-byte views, and 40-byte classified pass records.
  Wrong-thread ownership, stale/invalid identity, malformed matrices/state,
  nonmonotonic passes, inconsistent classifications, nonzero padding, and
  unknown fields fail closed.
- The pass classifier uses the thirteen mapped shader domains, render mode,
  target, and writer flags. Unknown world-target writers remain counted and
  make the frame ineligible for takeover.
- ABI minor 5 appends the frame envelope to raster submission. Reflected
  binding 6 is a 240-byte vertex-stage view record with layout hash
  `0xC34D6F6AB1E8B527`; older ABI prefixes receive an identity/disabled record.
- The standalone replay proves the boundary by unprojecting a desired screen
  fixture into camera-relative input vertices, serializing the view, and
  requiring Vulkan to reconstruct the expected screen/depth result. Debug and
  Release match byte-for-byte, with maximum channel error 1, mean error
  0.0102946, and zero Vulkan validation errors.

A live capture has since read the running engine's camera record directly.
Rather than trusting a field offset, the reader locates the matrices by the
one relationship that cannot arise by chance, `viewProjection ==
projection * view`, and only accepts a triple whose residual is within
tolerance. Against the running game it found three consecutive 64-byte
matrices inside `CameraStateData` at `+0x050` (view), `+0x090` (projection),
and `+0x0D0` (view-projection), stored **column-major**, with residual
exactly `0` **[OBS]**. That corroborates the documented `ViewData` field
order and fixes its matrix block at an 0x50-byte header offset. The captured
`.vfframe` replays through the Vulkan backend at 1280x720 with maximum
channel error 1 and zero validation errors.

The observed clip planes on that capture were near `40.0` and far `396.2`,
which is a short range more typical of a first-person or secondary camera
than the main world view, so the captured record is a real engine camera but
not yet confirmed to be the primary one **[INF]**.

This remains replacement implementation evidence, not yet a live confirmation
of the exact `ViewData`/`CameraStateData` copy point for the main world view
**[INF]**. The build-gated live gate must correlate
`BSGraphics::State::BuildCameraStateData`, `SetCameraData`,
`UpdatePreviousFrameCameraData`, `NiCamera::WorldPtToScreenPt3`, and the
accumulator pass stream before the view capture is promoted. Exact artifacts
and limitations are recorded in [`phase-10.md`](phases/phase-10.md).

## 8. Render-target registry

### 8.1 Manager layout

`RenderTargetManager` is a direct `0xFF0` singleton at RVA `0x2F42760` **[AL+ABI]**:

| Offset | Field |
|---:|---|
| `0x000` | `RenderTargetProperties[100]` (`0x20` each) |
| `0xC80` | `DepthStencilTargetProperties[12]` (`0x18` each) |
| `0xDA0` | `CubeMapRenderTargetProperties[1]` (`0x24`) |
| `0xDC4` | color target IDs[100] |
| `0xF54` | depth target IDs[12] |
| `0xF84` | cube target ID[1] |
| `0xF88..FA0` | dynamic width/height, minima, increase/decrease rates, movement delta |
| `0xFA4..FA8` | increase/freeze/moving-only/default-viewport/currently-active flags |
| `0xFAC/FB0` | pause frames and frames since increase |
| `0xFB4` | atomic dynamic-resolution-disabled count |
| `0xFB8` | create callback |

Color properties include width, height, engine `Format`, multisample, copyable, UAV, mip-generation, force-linear, mip level, texture target, and fast-clear. Depth properties include width, height, array size, multisample, alias, sampleable, HTile, stencil, and 16-bit-depth flags.

The live `RendererData` arrays are one entry larger than the manager property arrays: 101 vs. 100 color, 13 vs. 12 depth, and 2 vs. 1 cube. The likely explanation is an extra swap-chain/platform/special slot **[INF]**. Do not collapse these counts until runtime registration identifies the special entries.

The manager's property arrays are zero in the on-disk image; registration/materialization happens at runtime. Therefore dimensions and formats cannot be truthfully recovered by dumping the file's `.data` bytes. The rebuild routine at `0x183ABE0` must be observed after initialization, and every live COM resource should be queried with `GetDesc`.

### 8.2 Partially identified color target IDs

The following map comes from a current Post-NG-compatible renderer integration and agrees with this build's array counts and current hook IDs **[SEM]**. Treat named entries as medium confidence until the live descriptor/usage trace confirms them:

| ID | Name | ID | Name |
|---:|---|---:|---|
| 0 | Frame buffer / swap-chain target | 1 | Refraction normal |
| 2 | Main pre-alpha | 3 | Main |
| 4 | Main temporary | 7 | SSR raw |
| 8 | SSR blurred | 9 | SSR blurred extra |
| 10 | SSR direction | 11 | SSR mask |
| 14 | Main vertical blur | 15 | Main horizontal blur |
| 17 | UI | 18 | UI temporary |
| 20 | G-buffer normal | 21 | G-buffer normal swap |
| 22 | G-buffer albedo | 23 | G-buffer emissive |
| 24 | G-buffer material: gloss/specular/backlighting/SSS | 26 | TAA accumulation |
| 27 | TAA accumulation swap | 28 | SSAO |
| 29 | Motion vectors | 36 | UI downscaled |
| 37 | UI downscaled composite | 39 | Main depth mips (color/sample representation) |
| 48 | SSAO temporary 1 | 49 | SSAO temporary 2 |
| 50 | SSAO temporary 3 | 57 | Unknown mask |
| 58 | Diffuse light buffer | 59 | Specular light buffer |
| 64 | Downscaled HDR | 65 | HDR luminance 2 |
| 66 | HDR luminance 3 | 67 | HDR luminance 4 |
| 68 | HDR luminance 5/adaptation | 69 | HDR luminance 6/adaptation swap |
| 70 | HDR luminance 6 | 100 | Last allocated color slot; unknown |

Unlisted IDs remain unknown. Do not infer identity from adjacency alone.

### 8.3 Partially identified depth IDs

| ID | Name |
|---:|---|
| 0 | Main other-other |
| 1 | Main other |
| 2 | Main |
| 3 | Main copy |
| 4 | Main copy-copy |
| 8 | Shadow map |
| 12 | Last allocated depth slot; unknown |

The names `other-other` and `copy-copy` are community labels, not recovered Bethesda symbols. Live binds, clears, copies, and dimensions must establish their exact roles.

### 8.4 Required runtime target dump

Immediately after target materialization and after every resize, record for all 101/13/2 slots:

- Target ID and manager registration index.
- Resource pointer and all view pointers.
- `D3D11_TEXTURE2D_DESC`: dimensions, mip/array counts, format, sample desc, usage, bind/CPU/misc flags.
- Every RTV/DSV/SRV/UAV view descriptor.
- First and last frame event: create, clear, bind, SRV/UAV read, copy/resolve, mip generation, release.
- Draw-world phase and thread ID for every event.
- Aliasing: different IDs/views referencing the same resource.
- Dynamic-resolution ratio and viewport at each use.

This trace becomes the authoritative resource graph and the input to explicit Vulkan image-layout/barrier generation.

## 9. Visibility, accumulation, batching, and draw submission

### 9.1 `BSCullingProcess`

`BSCullingProcess` is `0x1A0`, derives from `NiCullingProcess`, and has a 32-entry primary vtable at RVA `0x26979E8`; its TypeDescriptor is RVA `0x309D9A0` **[BIN+ABI]**. Key fields:

| Offset | Field |
|---:|---|
| `0x120` | shared room visibility map |
| `0x150` | portal graph entry |
| `0x158` | culling mode: normal, all-pass, all-fail, ignore multibounds, force-no-update |
| `0x15C` | ten-entry culling-mode stack |
| `0x184` | stack index |
| `0x188` | compound frustum |
| `0x190` | `NiAccumulator` smart pointer |
| `0x198` | recurse-to-geometry flag |

The added virtuals append non-accumulated objects and test multibounds, occlusion planes, and ordinary bounds. The two current `Process` functions at `0x17E17A0` and `0x17E19F0` are suitable for scene-submission tracing but not for GPU replacement by themselves.

### 9.2 `BSShaderAccumulator`

`BSShaderAccumulator` is `0x590` and contains a `BSBatchRenderer` by value at `+0x0C8`:

| Offset | Field |
|---:|---|
| `0x058..0B0` | sun query state, three `SunOcclusionTest`s, first-person flag |
| `0x0B1` | Z-prepass flag |
| `0x0B4` | silhouette color |
| `0x0C4` | render-decals flag |
| `0x0C8` | `BSBatchRenderer` |
| `0x548/54C` | current pass and bucket |
| `0x550` | current-active flag |
| `0x558` | active shadow scene node |
| `0x560` | render mode |
| `0x568` | shadow light |
| `0x570` | eye position |
| `0x580` | depth-pass index |

Its semantic API registers standard, interface, local-map, LOD, occlusion, shadow, and VATS objects; finishes accumulation before/after depth resolve; and renders batches, decals, alpha, normals, water stencil, and sun queries **[SEM]**. Those are high-value points for capturing engine-level draw intent before it becomes D3D11 state changes.

### 9.3 `BSBatchRenderer`

`BSBatchRenderer` is `0x480`; TypeDescriptor RVA `0x30D4EF0`, primary vtable RVA `0x290CFE0` **[BIN+ABI]**. Its exposed vtable contains only the destructor, so most behavior is reached through nonvirtual functions/call sites.

Important internal records:

- Thirteen arrays of `PassGroup`, plus thirteen pass maps and active-index pairs.
- `PassGroup` (`0x10`): pass-head pointer, pass enum, next group, command-buffer-pass index.
- 23 geometry-group pointers and a separate alpha/grouping-alpha path.
- `GeometryGroup` (`0x28`): owner, persistent pass list, depth, count, flags, group number.
- Command-buffer pass maps and linked `CommandBufferPassesData` pages. Each page contains 8,192 buffer pointers and frame/owner metadata.

The semantic API registers passes/groups/alpha, sorts them, begins and ends passes, sets up shaders/materials/geometry, issues normal and immediate draws, and executes command-buffer passes **[SEM]**. This is probably the best long-term Vulkan seam: preserve culling and accumulation, then translate `BSRenderPass`/geometry groups into backend-neutral draw packets instead of emulating every D3D11 call.

Geometry groups (`0x00..0x16`):

| ID | Group | ID | Group |
|---:|---|---:|---|
| 0 | LOD land | 1 | LOD objects |
| 2 | Multi-index decal | 3 | Opaque decal |
| 4 | Blended decal | 5 | Refraction active |
| 6 | First-person non-refraction | 7 | Fading |
| 8 | No shadow | 9 | Low anisotropy |
| 10 | Underwater fog | 11 | SSLR |
| 12 | Z only | 13 | Post-Z only |
| 14 | Sky/clouds | 15 | Smooth alpha test |
| 16 | VATS mask depth-only | 17 | VATS mask |
| 18 | Sun glare | 19 | Blood spatter |
| 20 | Z prepass | 21 | Water stencil |
| 22 | Water depth | — | — |

The engine also has jobified accumulation and deferred-pass registration through `BSMTAManager`, plus a culling-job batching layer **[SEM]**. Thread ownership and merge order must be logged before replacing command submission.

### 9.4 Geometry, shader properties, and pass generation

`BSGeometry` is `0x160`, TypeDescriptor RVA `0x30989D0`, with a 66-slot vtable at `0x267E0C8` **[BIN+ABI]**. Renderer-relevant fields are:

| Offset | Field |
|---:|---|
| `0x120` | model bound |
| `0x130` | two `NiProperty` smart pointers |
| `0x140` | skin instance |
| `0x148` | opaque renderer-data pointer |
| `0x150` | packed `BSGraphics::VertexDesc` |
| `0x158/159` | geometry type and registered flag |

Its added virtuals expose segment data, custom index buffers, combined/merged/multi-index shape classification, and renderable-triangle counts. The opaque `rendererData` pointer needs live type classification; it is likely the bridge from scene geometry to the `BSGraphics::TriShape`/buffer records.

`BSShaderProperty` is `0x70`, TypeDescriptor RVA `0x2FF06C8`, with a 64-slot vtable at `0x28F7BC0` **[BIN+ABI]**. Its renderer fields are:

| Offset | Field |
|---:|---|
| `0x28` | material alpha |
| `0x2C` | last render-pass state |
| `0x30` | 64-bit shader-property flags |
| `0x38/40` | normal and debug pass-list heads |
| `0x48` | fade node |
| `0x50` | effect-shader data |
| `0x58` | `BSShaderMaterial*` |
| `0x60` | last accumulation time |
| `0x64` | LOD fade |
| `0x68` | render-pass-clear spin lock |

Virtual slots `0x2B..0x3F` are the semantic scene-to-pass boundary: get ordinary, shadow/mask, and local-map pass lists; create a VATS-mask pass; get pass count and depth pass; test property merging; get/set material alpha; visit textures; identify shader/material; clear/clarify the shader; obtain base texture/water-fog passes; precache textures; determine utility declarations; and clear generated passes **[ABI]**.

The complete 64-bit property flag map is directly useful when constructing Vulkan pipeline and technique keys:

| Bit | Flag | Bit | Flag |
|---:|---|---:|---|
| 0 | Specular | 1 | Skinned |
| 2 | Temporary refraction | 3 | Vertex alpha |
| 4 | Greyscale-to-palette color | 5 | Greyscale-to-palette alpha |
| 6 | Falloff | 7 | Environment map |
| 8 | RGB falloff | 9 | Cast shadows |
| 10 | Face | 11 | UI mask rectangles |
| 12 | Model-space normals | 13 | Refraction clamp |
| 14 | Multi-texture landscape | 15 | Refraction |
| 16 | Refraction falloff | 17 | Eye reflection |
| 18 | Hair tint | 19 | Screen-door alpha fade |
| 20 | Local-map clear | 21 | FaceGen RGB tint |
| 22 | Own emit | 23 | Projected UV |
| 24 | Multiple textures | 25 | Tessellate |
| 26 | Decal | 27 | Dynamic decal |
| 28 | Character light | 29 | External emittance |
| 30 | Soft effect | 31 | Z-buffer test |
| 32 | Z-buffer write | 33 | LOD landscape |
| 34 | LOD objects | 35 | No fade |
| 36 | Two-sided | 37 | Vertex colors |
| 38 | Glow map | 39 | Transform changed |
| 40 | Dismemberment meat cuff | 41 | Tint |
| 42 | Vertex lighting | 43 | Uniform scale |
| 44 | Fit slope | 45 | Billboard |
| 46 | LOD land blend | 47 | Dismemberment |
| 48 | Wireframe | 49 | Weapon blood |
| 50 | Hide on local map | 51 | Premultiplied alpha |
| 52 | VATS target | 53 | Anisotropic lighting |
| 54 | Skew specular alpha | 55 | Menu screen |
| 56 | Multi-layer parallax | 57 | Alpha test |
| 58 | Inverted fade pattern | 59 | VATS target draw-all |
| 60 | Pip-Boy screen | 61 | Tree animation |
| 62 | Effect lighting | 63 | Refraction writes depth |

Texture-role IDs are base/diffuse 0, normal 1, glow 2, height 3, environment 4, wrinkles 5, multilayer 6, and backlight-mask/smooth-spec 7.

`BSLightingShaderProperty` extends this to `0xE8`, TypeDescriptor RVA `0x30D0DC0`, vtable RVA `0x28FA008` **[BIN+ABI]**. It adds projected-UV parameters/colors (`0x70/80`), three depth-map pass lists (`0x90`), a depth pass (`0xA8`), forward pass list (`0xB0`), emit color and root name, emit/darkness/LOD fades, base technique ID (`0xD8`), command-buffer-clear count, debug tint index, and stencil reference (`0xE4`). This object ties geometry/material flags to concrete depth and forward passes.

### 9.5 Material ABI and feature variants

`BSShaderMaterial` is `0x38`, TypeDescriptor RVA `0x30D0D98`, with a nine-slot vtable at `0x290B650`. The slots are destructor, `Create`, `CopyMembers`, `ComputeCRC32`, `GetDefault`, `GetFeature`, `GetType`, `ReceiveValuesFromRootMaterial`, and equality/copy comparison. Data consists of two UV offsets, two UV scales, a hash key, and unique code **[BIN+ABI]**.

Material types are base 0, effect 1, lighting 2, and water 3. Lighting feature IDs are:

| ID | Feature | ID | Feature |
|---:|---|---:|---|
| 0 | Default | 1 | Environment map |
| 2 | Glow map | 3 | Parallax |
| 4 | Face | 5 | Skin tint |
| 6 | Hair tint | 7 | Parallax occlusion |
| 8 | Landscape | 9 | LOD landscape |
| 10 | Snow | 11 | Multi-layer parallax |
| 12 | Tree animation | 13 | LOD objects |
| 14 | Multi-index snow | 15 | LOD objects HD |
| 16 | Eye | 17 | Cloud |
| 18 | LOD landscape noise | 19 | LOD landscape blend |
| 20 | Dismemberment | — | `-1` means none |

`BSLightingShaderMaterialBase` is `0xC0`. Beyond the base material it stores specular color; diffuse, normal, rim/soft-light, smoothness/spec-mask, and lookup textures; address mode and texture set; alpha, refraction power, smoothness, specular scale, Fresnel power; six wetness controls; subsurface rolloff; rim/back-light powers; lookup scale; and a texture-load lock **[ABI]**. This is the core material constant/texture source for a Vulkan lighting pass.

All 15 lighting-material RTTI variants have 18-slot primary vtables **[BIN]**:

| Class | TypeDescriptor RVA | Vtable RVA |
|---|---:|---:|
| `BSLightingShaderMaterial` | `0x30D0D30` | `0x28F8918` |
| `BSLightingShaderMaterialBase` | `0x30D0D60` | `0x2909CE0` |
| `BSLightingShaderMaterialDismemberment` | `0x30D37E0` | `0x290A070` |
| `BSLightingShaderMaterialEnvmap` | `0x30D36C0` | `0x2909D78` |
| `BSLightingShaderMaterialEye` | `0x30D36F8` | `0x2909E10` |
| `BSLightingShaderMaterialFace` | `0x30D3820` | `0x290A108` |
| `BSLightingShaderMaterialGlowmap` | `0x30D3730` | `0x2909EA8` |
| `BSLightingShaderMaterialHairTint` | `0x30D3890` | `0x290A238` |
| `BSLightingShaderMaterialLODLandscape` | `0x30D3900` | `0x290A368` |
| `BSLightingShaderMaterialLandscape` | `0x30D38C8` | `0x290A2D0` |
| `BSLightingShaderMaterialMultiLayerParallax` | `0x30D3980` | `0x290A498` |
| `BSLightingShaderMaterialParallax` | `0x30D3768` | `0x2909F40` |
| `BSLightingShaderMaterialParallaxOcc` | `0x30D37A0` | `0x2909FD8` |
| `BSLightingShaderMaterialSkinTint` | `0x30D3858` | `0x290A1A0` |
| `BSLightingShaderMaterialSnow` | `0x30D3940` | `0x290A400` |

`BSShaderData` is the non-polymorphic `0x1E0` root/material-data record whose values feed shader properties and materials **[ABI]**:

| Range | Contents |
|---:|---|
| `0x004..054` | Shader family, specular/emittance/smoothness/Fresnel values, wetness controls, and screen-space-reflection control. |
| `0x058..0C0` | Root/material paths; alpha/blend/test modes and reference; vertex/model-space/decal/face/hair/two-sided/tree/shadow/glow/SSR/tessellation/backlight/SSS/rim/anisotropy/Z/refraction flags and scalars. |
| `0x0C8..140` | Five controller arrays plus the alpha-test-reference controller. |
| `0x148..17C` | Parallax, POM, multilayer parallax, displacement, tessellation, and environment-mapping flags/scalars. |
| `0x180..1A8` | Environment mask scale; sky type; reflection/refraction/depth/UV/fog/falloff/soft-effect state. |
| `0x1AC..1D8` | UV offset/scale, greyscale-palette flags, hair tint, ten-slot shader texture set, and clamp mode. |

Its shader-family values are standard 0, water 1, effect 2, sky 3, and tall grass 4. This record is an asset/root-material description, while `BSShaderProperty` owns generated pass lists and a concrete `BSShaderMaterial`. The replacement should capture both rather than assuming the material object contains every authoring value.

Water uses a separate `BSWaterShaderMaterial` (`0x168`, TypeDescriptor `0x30D5008`, nine-slot vtable `0x290D578`) **[BIN+ABI]**. It contains a static reflection map and three normal maps; shallow, deep, reflection, underwater-fog, light-silt and dark-silt colors; four parameter vectors; normal scroll/scale/amplitude vectors; reflection plane; texture offsets; and update/default-reflection/SSR flags. Water therefore needs a dedicated resource/constant translator, not the lighting-material base layout.

The upstream `WaterShaderData` asset record is `0xCC` and supplies maximum depth, shallow/deep/fog/reflection/silt colors, alpha and fog ranges, underwater fog amount/distances, normal falloffs, reflection/Fresnel/surface-effect amounts, displacement force/velocity/falloff/damping/size, sun specular/sparkle values, light/shininess values, three wind directions/speeds/amplitudes/UV scales/noise falloffs, silt amount, and the SSR-enable flag **[ABI]**.

Texture-side ABI anchors:

- `BSShaderTextureSet` is `0x60`, TypeDescriptor `0x2FA5210`, vtable `0x24A7030`; ten `BSFixedString` texture names begin at `0x10`.
- `NiTexture` is `0x48`, TypeDescriptor `0x30988D0`, vtable `0x267D2D8`. It stores name `0x10`, flags `0x18`, intrusive previous/next links `0x20/28`, resource stream `0x30`, opaque `BSGraphics::Texture*` `0x38`, desired/saved degradation levels `0x40/41`, and DDX/sRGB bits at `0x42`.
- The concrete `BSGraphics::Texture` layout is not exposed by the current public ABI source and remains a priority. Key Vulkan mirrors by `NiTexture*` and its renderer-texture pointer until that type is decoded.

Derived-only fields for several variants remain unmapped in the public ABI source and require live object-size/field tracing. Do not flatten all features to the `0xC0` base size.

### 9.6 Installed material and texture corpus

The installed `Fallout4 - Textures*.ba2` name tables and texture records were scanned read-only on 2026-08-14. Counts below are archive entries, not guaranteed unique canonical paths: a later patch archive can repeat a path from an earlier archive. Ordinary texture archives report BA2 version 7; the patch texture archive reports version 8 **[ASSET]**.

| Filename suffix | Entries | Observed DXGI formats | What the corpus establishes |
|---|---:|---|---|
| `_d` | 7,617 | Mostly 71/77 (`BC1_UNORM`/`BC3_UNORM`), with a few 87/74 | Large authored color/alpha population; suffix alone does not prove alpha meaning. |
| `_n` | 4,609 | 4,597 format 83 (`BC5_UNORM`), 12 BC1 | Predominantly two-channel tangent-normal candidates. |
| `_msn` | 7,221 | 6,121 BC3, 1,100 BC1 | RGB-capable population consistent with model-space normals; the material flag must select this decode **[ASSET+ABI]**. |
| `_s` | 5,398 | 5,388 BC5, 10 BC1 | Deliberately two-channel material data at scale, consistent with specular weight plus smoothness. |
| `_g` | 136 | Mostly BC1 | Small auxiliary/glow-like population; bind role remains authoritative. |
| `_e` | 47 | Mixed BC1, BGRA, and BC3 | Feature-dependent auxiliary population; suffix is insufficient to assign a universal channel contract. |
| `_m` | 77 | Mostly BC1 | Mask-like population; material class and slot decide its meaning. |
| `_h` | 15 | BC1 | Small height-like population; scale/bias and POM/tessellation flags remain required. |

The installed general material archive is BA2 version 8 and contains 6,899 entries **[ASSET]**. This is enough material coverage to build a deterministic translator without inventing new images, but entry counts do not establish how often each map is referenced at runtime.

Current NifSkope source provides useful independent semantic corroboration: its FO4 material reader loads nine lighting-material texture paths and the specular color/multiplier, smoothness, Fresnel, wetness, emission, model-space-normal, glow, environment, hair, tree, face, skin, displacement, and palette fields. Its FO4 preview shader uses specular-map red as the specular mask and green as smoothness. This is **[SEM]**, not Bethesda shader source: [material reader](https://github.com/niftools/nifskope/blob/3a85ac55e65cc60abc3434cc4aaca2a5cc712eef/src/io/material.cpp) and [FO4 preview shader](https://github.com/niftools/nifskope/blob/3a85ac55e65cc60abc3434cc4aaca2a5cc712eef/res/shaders/fo4_default.frag).

The resulting evidence boundary is:

- High confidence: the game supplies substantial base-color, tangent-normal, model-space-normal, and two-channel specular/smoothness data; the runtime material records supply scalar and class controls **[ASSET+ABI+SEM]**.
- Medium confidence: when a lighting material selects the smooth/spec role, R is specular weight and G is smoothness **[ABI+SEM]**.
- Still unresolved: the exact Bethesda transfer from smoothness/specular values to highlight distribution and energy, per-feature swizzles in overloaded auxiliary slots, normal-axis handedness, and the precise interpretation of unusual/legacy textures **[INF]**.

Runtime validation must therefore capture the bound SRVs, material constant buffers, flags, and vanilla output for controlled material/light sweeps. Filename matching is a fallback diagnostic only; the concrete material, semantic slot, and feature flags own the decode. Alpha is coverage only when the corresponding alpha state says so, and model-space normals must never pass through the tangent-normal path.

### 9.7 Implemented texture/material replay boundary

The replacement now has an offline contract that preserves this evidence
boundary instead of filling the unknowns with a metallic/roughness guess:

- `.vftex` stores the exact resource format, explicit SRV format, normalized
  sampler, dimensions, array/cube/mip metadata, and compressed subresources.
  Semantic use owns the linear versus sRGB view.
- `.vfmat` stores one resolved default-lighting material plus the exact base,
  normal, and smooth/spec texture packets. Every scalar/vector retains its raw
  value, bounded evaluation value, and provenance.
- The implemented default convention is base color through sRGB, tangent BC5
  or explicitly selected model-space RGB normals, and linear smooth/spec R/G
  as specular weight/smoothness. Missing inputs use typed neutral resources.
- The compatibility BRDF is colored-F0 specular/smoothness GGX. It has a
  versioned 16-entry transfer table and does not infer metalness or emission
  from RGB values or filenames.
- Static/dynamic C++ and GLSL records are reflection-checked at 64 and 48
  bytes. The same material bundle executes through the CPU oracle and Vulkan
  fragment shader, including on the real Phase 7 captured mesh.
- Unsupported transfer versions, semantic/view mismatches, substituted
  textures, contradictory equal-provenance sources, and unknown material
  classes fail closed before GPU submission.

The current artifacts and exact promotion evidence are recorded in
[`phase-08.md`](phases/phase-08.md) and
[`phase-09.md`](phases/phase-09.md). This is implementation evidence for the
replacement, not new evidence about Bethesda's precise highlight-transfer
function. A live engine material/SRV/constant capture and controlled vanilla
light sweep remain required before the default translator is declared parity
calibrated **[INF]**.

### 9.8 Implemented opaque scene mirror boundary

The replacement now mirrors several opaque static objects per view without
letting a batch or accumulator pointer cross the backend ABI:

- `.vfscene` version 1.0 is a pointer-free, CRC-protected envelope of 224-byte
  opaque object records. A record carries stable object and material identity,
  the source draw index, the owning pass sequence, object flags, roughness,
  current and previous model transforms, local bounds, and separate geometric
  and shading normals.
- Object-to-draw association is a bijection against the raster packet, and
  material association is exact. Duplicate identity, duplicate draw claims,
  singular or non-affine transforms, inverted bounds, non-unit normals, and
  out-of-range roughness fail closed before submission.
- Engine pass accounting is a separate contract from backend draw
  construction. Every mirrored object must name a pass of the mirrored view
  that writes the world target and classifies as `Opaque` through the
  section 9 classifier; every opaque world pass of that view must be mirrored;
  and any unclassified world-target writer makes the frame ineligible to arm.
  Sky, water, alpha-test, transparent, and effect world writers remain
  declared unsupported classes that vanilla still owns.
- ABI minor 6 appends the scene envelope and a G-buffer output pointer/
  capacity to raster submission. Reflected storage binding 7 carries the
  object records for the vertex and fragment stages; a push constant selects
  the object for each expanded draw.
- The mirrored G-buffer is written through four rasterization-ordered color
  attachments — albedo, geometric normal with roughness, shading normal with
  depth, and object/material identity — then interleaved into the 64-byte
  reflected pixel record on readback. Storage-buffer writes were rejected for
  this purpose because writes from separate draws to one pixel are unordered.
- The standalone replay proves the boundary with three objects sharing one
  draw list: a near interpolated object, a strictly enclosed distant object
  that must never survive the depth test, and a yaw-rotated partially occluded
  object. Debug and Release match byte-for-byte, no pixel disagrees on object
  or material identity, and reversing submission order reproduces the same
  G-buffer.

This is replacement implementation evidence, not a live confirmation of the
`BSBatchRenderer`/`BSShaderAccumulator` capture point or of the exact set of
draws that touch the claimed opaque targets in the running engine **[INF]**.
The build-gated live gate must associate every actual D3D draw against those
targets with a mirrored packet or a declared unsupported class before the
scene capture is promoted. Exact artifacts and limitations are recorded in
[`phase-11.md`](phases/phase-11.md).

### 9.9 Implemented instance, streaming, and cell lifecycle boundary

The mirror now survives repetition and streaming without leaking or serving
stale content:

- `SceneDatabase` owns cell groups, instance identity, geometry residency, and
  retirement. It composes the resource registry from section 8's lifetime
  model and the descriptor quarantine from the texture path, so one timeline
  governs buffers, descriptors, and instances.
- Instance identity is minted as generation plus slot, is stable while
  attached, and always changes when a slot is reused, so a new object can
  never inherit another object's motion history.
- Repeated meshes are deduplicated by content hash across distinct engine
  addresses, but only when the resource is immutable. Two independently
  mutable buffers that hash identically today are kept separate because they
  can diverge next frame.
- Cell attach/detach, upload cancellation, and deferred destruction are all
  timeline gated. A completion that arrives after its generation retired is
  rejected, and an address reused for different content while its previous
  generation is still retiring fails closed instead of aliasing.
- Handle spaces are exhausted rather than wrapped. A wrapped generation would
  let a stale handle alias a live resource, so both geometry and instance
  handles report exhaustion at their limit.
- Every lifecycle transition is recorded as a delta and can be written into a
  `.vftrace` as record type 8. The trace carries stable identity, content
  hash, byte size, generation, group, and timeline value, never an address.
- Scene packet version 1.1 carries an explicit instance section; version 1.0
  expands to one implicit instance per object. The Vulkan mirror draws one
  indexed call per object with an instance count, selecting per-instance
  transforms and material parameters from reflected storage binding 9.

The exact artifacts, tolerances, and limitations are recorded in
[`phase-12.md`](phases/phase-12.md). The mapped cell attach/detach, streaming,
and address-reuse events in the running engine remain unconfirmed **[INF]**;
the fixture models them from the observed lifetime rules rather than from a
live capture.

### 9.10 Implemented deformation boundary

Dynamic geometry is mirrored through an explicit deformation contract rather
than by re-uploading deformed vertices every frame:

- `.vfdeform` carries four-influence skin records, current and previous 3x4
  bone palettes, sparse morph targets with current and previous weights, and
  wind parameters with a current and previous time. The engine's observed
  four-influence, byte-indexed skinning convention from section 7 is what the
  record models.
- Evaluation order is fixed: morph accumulation in bind space, then skinning,
  then wind displacement scaled by per-vertex flexibility. The previous pose
  uses the same order with previous inputs, so motion is defined by the
  contract instead of by renderer frame history.
- Weights are normalized explicitly and the normalized count is reported.
  Degenerate weights, out-of-palette indices, singular palettes, mismatched
  previous palettes, and out-of-range morph or wind values fail closed.
- Bounds are the union of the current and previous poses, so a moving object
  never leaves the region the renderer reserved for it.
- Fixed topology updates in place; a changed vertex, bone, or delta count
  requires a new generation, and generations never move backwards.
- Deformed output is sub-allocated from a ring that refuses to hand out bytes
  an earlier submission still reads, and wraps only into retired space.
- The Vulkan mirror dispatches a reflected compute kernel before the render
  pass, publishes the result with a compute-to-vertex-input barrier, and
  rasterizes the deformed stream. Current and previous positions are read back
  together so motion can be verified directly.

The exact artifacts and tolerances are recorded in
[`phase-13.md`](phases/phase-13.md). The engine's actual skin palette layout,
morph channel set, and wind state remain unconfirmed against a live capture
**[INF]**; the fixture exercises the contract, not Bethesda's exact
deformation inputs.

### 9.11 Implemented terrain and landscape boundary

Exterior landscape is mirrored through a cell-oriented contract that keeps the
engine's own placement and LOD decisions authoritative:

- `.vfterrain` carries a layer table, cell records, landscape vertices, and an
  index stream. A vertex holds exactly eight blend channels, which is what the
  vertex layout in section 6 delivers as `Landscape0`/`Landscape1` — two UNorm
  byte quads.
- The slot-to-layer mapping belongs to the cell, matching how a cell binds its
  own texture set. A channel outside the cell's declared slot count is land
  data this project has not classified, and the packet is refused rather than
  interpreted.
- Cell origins are doubles on all three axes and vertex positions are floats
  confined to one cell. The camera-relative subtraction happens in double and
  only the small residual is narrowed, which is the same camera-relative rule
  section 8 records for the engine's own view matrices.
- Cracks are detected in the contract, not in an image: every shared world
  position between two cells is compared in double precision, and checks that
  cross a LOD boundary are counted separately because that is where distant
  terrain seams appear.
- The engine's per-cell `lodLevel` and morph range are recorded and never
  recomputed, so the mirror cannot disagree with vanilla culling by running a
  second guessed culler.
- Streaming residency is tracked per cell so an exterior can be proven to
  return to its baseline footprint after unload.
- The Vulkan mirror binds the layer table verbatim, a derived per-cell record,
  and a landscape `sampler2DArray`, and writes the same rasterization-ordered
  attachments and depth buffer as the opaque scene pass.

The exact artifacts and tolerances are recorded in
[`phase-14.md`](phases/phase-14.md). The engine's actual cell grid extents,
layer table contents, land channel semantics beyond blend weight, and LOD
morph ranges remain unconfirmed against a live capture **[INF]**; the fixture
exercises the contract, not Bethesda's exact landscape data.

### 9.12 Implemented alpha and two-sided visibility boundary

Every draw carries a 64-byte visibility record: a 32-byte alpha state (source,
classification, flags, reference, constant alpha, fade), a face mode, and the
model determinant. Classification is captured from the engine's alpha
property, never inferred — alpha-to-coverage without a test, or a test with no
alpha source, is refused as unclassified rather than guessed at. The scene
packet carries the table at minor version 1.2; a scene without one resolves to
an implicit opaque, front-only, unmirrored record.

The coverage decision is defined once, in `visibility::EvaluateCoverage`, and
mirrored branch for branch in `shaders/phase15/alpha_coverage.glsl`. It is
independent of whether the caller is a depth-only or a colour pass, which is
what lets the Vulkan mirror run a genuine depth prepass — the same recorded
draws, the same dynamic state, only the pipeline differs — and then test
`VK_COMPARE_OP_EQUAL` with depth writes off in the colour pass. A silhouette
that disagreed between the two passes erases its own colour fragment instead
of differing quietly.

The G-buffer stores **the coverage decision's opacity, never the sampled
texture alpha**. An opaque surface stores opacity 1 whatever its base texture
holds in that channel, a cutout survivor stores 1 because the test that
admitted it was binary, and only a blended surface stores a partial value.
This matters against real content: Fallout 4 diffuse maps routinely carry a
mask or a height in the alpha channel of surfaces that are not alpha-tested
**[VER, offline]**, and folding that into a deferred G-buffer makes solid
geometry transparent.

Face handling: only a two-sided surface flips on a back face, and when it does
both the geometric and the shading normal flip together so they stay in one
hemisphere. A negative model determinant reverses the declared winding
(`EffectiveFrontFace`) rather than changing the geometry. Blended surfaces are
classified and then refused from the opaque raster class; sorted transparency
is not rendered by this boundary.

The exact artifacts and tolerances are recorded in
[`phase-15.md`](phases/phase-15.md). The engine's real `NiAlphaProperty`
values, per-material test references, and two-sided flags remain unconfirmed
against a live capture **[INF]**.

### 9.12a Light class taxonomy

Reconstructed from the installed binary's RTTI, so the light types the mirror
must translate are enumerated rather than assumed **[BIN]**:

| Class | TypeDescriptor RVA | Vtable RVA | Slots |
|---|---:|---:|---:|
| `NiLight` | `0x3098918` | `0x267E770` | 58 |
| `NiAmbientLight` | `0x3099488` | `0x26811D8` | 58 |
| `NiDirectionalLight` | `0x3099230` | `0x267F768` | 58 |
| `NiPointLight` | `0x2FA5900` | `0x267D820` | 58 |
| `NiSpotLight` | `0x3099648` | `0x2682318` | 58 |
| `BSShadowLight` | `0x30D55F8` | `0x2910B78` | 15 |
| `ShadowSceneNode` | `0x30D3590` | `0x2908F50` | 67 |

The four concrete `NiLight` subclasses share the 58-slot base vtable, so the
engine's light taxonomy is exactly ambient, directional, point, and spot.
`BSShadowLight` is a separate 15-slot object rather than a fifth light type,
which is why shadow casting is a property of a light in this mirror and not a
type of light. `ShadowSceneNode` is the container the accumulator draws from,
and is the capture boundary for the light list.

Fog state is a `FogStateType` of `0x60` bytes embedded at `BSGraphics::State`
`+0x014` **[ABI]**, immediately before the multisample level, so it is
per-frame state rather than a per-scene resource.

### 9.13 Implemented material family boundary

The twenty-one lighting feature IDs of 9.5 are mirrored verbatim as material
families, and the 64-bit property flag map of 9.4 is recorded in full. A
feature ID this build does not classify resolves to a declared fallback with
recorded provenance; it is never folded into Default, because a silent
Default renders an unknown family as an ordinary surface and looks plausible
while being wrong.

Texture slot meaning is resolved **per family**, which is what the overloaded
role IDs require. Role 7 is recorded as backlight-mask *and* smooth-spec:
Face, SkinTint, and HairTint read it as a backlight mask, everything else
reads the smoothness/specular mask, and reading it as the wrong one silently
turns a smoothness map into a rim-light mask. Slots 8 and 9 of the ten-slot
set carry no recorded role, so an authored one is counted as unclassified
rather than assigned a meaning **[ASSET+ABI]**.

Nothing is derived from whether a texture happens to be authored, and
captured scalars do not enable a lobe. Two rules follow, and both are
engine-facing rather than fixture conveniences:

- **Emission requires a declaration.** A glow map slot, own-emit (bit 22),
  external emittance (bit 29), or the glow-map family authorizes it. A
  saturated albedo is ordinary in authored content, and reading it as
  emission makes plain surfaces glow **[VER, offline]**.
- **Model-space normals never take the tangent path.** The decodes differ in
  two ways: a model-space texel reads three channels and is absolute, while a
  tangent-space texel reads two, reconstructs Z from the unit length it was
  compressed against, and stores that Z along the surface normal — so it must
  be rotated into the surface frame first. Treating the second as the first
  points every normal away from its surface **[ABI+SEM]**.

Twenty-one families collapse into eight broad shader classes, with everything
else that varies carried as per-object record data, so pipeline count does not
grow with content. Static and dynamic material change are separated: a
rebound texture or changed slot layout rebuilds descriptor sets, while a
wetness or controller change moves only a dynamic revision.

The exact artifacts and tolerances are recorded in
[`phase-16.md`](phases/phase-16.md). The mirror's tangent frame is generated
rather than read from the mesh's authored tangents, so tangent-space normals
are self-consistent but not yet vanilla-matching **[INF]**; the palette
lookup slot, the glow-map binding, and POM marching are likewise unresolved
**[INF]**.

## 10. Shader system

### 10.1 Shader domains and render modes

`BSShaderManager::ShaderEnum` selects thirteen shader domains **[ABI]**:

| ID | Domain | ID | Domain |
|---:|---|---:|---|
| 0 | Effect | 1 | Utility |
| 2 | Distant tree | 3 | Particle |
| 4 | Deferred prepass | 5 | Deferred light |
| 6 | Deferred composite | 7 | Sky |
| 8 | Lighting | 9 | Blood spatter |
| 10 | Water | 11 | Face customization |
| 12 | Image space | — | Total = 13 |

The render-mode enum runs through `0x26` (total marker `0x27`) and includes normal/debug modes, depth prepass (`0x0D`), occlusion (`0x0E`), three shadow-map modes (`0x0F..0x11`), local map (`0x12`), screen splatter (`0x14`), LOD/silhouette modes, deferred G-buffer (`0x18`), individual G-buffer debug outputs (`0x19..0x1E`), all G-buffers (`0x1F`), light visibility, VATS mask/mask debug/prepass (`0x21..0x23`), map maker (`0x25`), and simple lighting (`0x26`). Value `0x24` is unassigned in the public enum and should remain reserved.

`BSShaderManager::State` is `0x3F0`. It owns five shadow-scene-node pointers, shader timers/frame count, interior/deferred flags, SSR and screen-space-subsurface switches, Pip-Boy target/intensity state, LOD values, ambient and camera transforms, camera near/far/water state, UI masks, character lighting, and current/pending force-disable flags for SSR, godrays, and directional lights at `0x3E6..0x3EB` **[ABI]**.

### 10.2 `BSShader` interface

`BSShader` is `0x118` and uses multiple inheritance: `NiRefObject` at offset 0 and `BSReloadShaderI` at offset `0x10`. Its primary vtable has 13 slots **[BIN+ABI]**:

| Slot | Method |
|---:|---|
| 0 | Destructor |
| 1 | Inherited `NiRefObject` virtual |
| 2 | `SetupTechnique(currentPass)` |
| 3 | `RestoreTechnique(currentPass)` |
| 4 | `SetupMaterial` |
| 5 | `RestoreMaterial` |
| 6 | `SetupMaterialSecondary` |
| 7 | `SetupGeometry(BSRenderPass*)` |
| 8 | `RestoreGeometry(BSRenderPass*)` |
| 9 | `GetTechniqueName` |
| 10 | `RecreateRendererData` |
| 11 | `ReloadShaders(clear)` |
| 12 | `GetBonesVertexConstant` |

Fields: shader domain/type at `0x18`; technique maps for VS `0x20`, HS `0x50`, DS `0x80`, PS `0xB0`, CS `0xE0`; FXP filename at `0x110`. Each map is `0x30`. This is a central compatibility contract: material and pass logic expects to select stage objects by technique ID before the geometry setup call.

Each technique map is a `BSTSet` whose hash and equality policies use the stage record's 32-bit `id`. Preserve the engine IDs as stable variant keys even if the backend stores SPIR-V modules and Vulkan pipelines in side tables.

### 10.3 Core shader RTTI/vtable anchors

| Type | TypeDescriptor RVA | Primary vtable RVA | Notes |
|---|---:|---:|---|
| `BSShader` | `0x30D0E40` | `0x290DBC8` | 13 primary slots; secondary reload vtable at `0x290DC38`. |
| `BSBloodSplatterShader` | `0x30D3C58` | `0x290ACA0` | Blood domain. |
| `BSDFPrePassShader` | `0x30D4B98` | `0x290BFF0` | Deferred prepass. |
| `BSDFCompositeShader` | `0x30D4E30` | `0x290C9F8` | Deferred composite; also has a secondary vtable. |
| `BSSkyShader` | `0x30D4E60` | `0x290CB80` | Sky domain. |
| `BSFaceCustomizationShader` | `0x30D4F18` | `0x290CFF0` | Face customization. |
| `BSEffectShader` | `0x30D4F78` | `0x290D170` | Effect domain. |
| `BSDistantTreeShader` | `0x30D4FD8` | `0x290D4C0` | Distant tree. |
| `BSWaterShader` | `0x30D5060` | `0x290D7D0` | Water. |
| `BSLightingShader` | `0x30D5248` | `0x290E468` | Lighting/material domain; secondary vtable present. |
| `BSUtilityShader` | `0x30D5328` | `0x290EBC0` | Utility. |
| `BSParticleShader` | `0x30D53D0` | `0x290F930` | Particle. |
| `BSDFLightShader` | `0x30D53F8` | `0x290FA08` | Deferred light. |
| `BSComputeShader` | `0x30D5BF0` | `0x29120C8` | Compute wrapper/domain. |
| `BSShaderResourceManager` | `0x30D5C78` | `0x29139A8` | 52-entry vtable. |

`BSShaderResourceManager` implements the 52-slot `IRendererResourceManager` surface **[ABI]**:

| Slot(s) | Operations |
|---:|---|
| 0 | Destructor. |
| `1..5` | Three tri-shape creation forms, direct renderer-data creation, sub-index tri-shape creation. |
| `6..7` | Tri-shape renderer-data increment/decrement. |
| `8..11` | Two dynamic-tri-shape creation forms and their increment/decrement operations. |
| `12..14` | Convert static to dynamic shapes, apply materials, set stream dynamic flags. |
| `15..17` | Create and reference-count particle shapes. |
| `18..25` | Static/dynamic line-shape creation variants and reference counting. |
| `26..33` | Load/create/read/finish streaming textures and increment/decrement renderer textures. |
| `34..39` | Query texture dimensions/format and manage desired mip/upgrade streaming. |
| `40..42` | Create/decrement vertex buffers and create a default effect shader property. |
| `43..45` | Mesh-LOD tri-shape creation and instancing index-buffer update. |
| `46..47` | Fast tri-shape intersection and tangent-space generation. |
| `48..51` | Shader frame count, timer delta, fade-node settings, and camera vectors. |

This is a strong resource-translation facade because it already accepts CPU geometry/texture inputs and returns renderer data. Each concrete current-build implementation signature/caller still needs validation before detouring the vtable.

### 10.4 Reload/debug surface

The `RefreshShaders` command string is at RVA `0x24BAFD8`; `ReloadFXP` is at `0x24BB010`. The accepted reload-category string at RVA `0x24B2210` is:

```text
lighting, effect, utility, water, bloodsplatter, grass, distanttree,
particle, sky, imagespace, shcompute, dfp, dfl, dfc, shc, dftl
```

This reveals additional internal group abbreviations beyond the thirteen public shader-domain IDs, notably shader compute and deferred tiled-lighting groups.

## 11. Image-space/post-processing graph

### 11.1 `ImageSpaceEffect`

The abstract/base effect is `0xB0`, TypeDescriptor RVA `0x30D0FB0`, vtable RVA `0x2910DE8`. Its 12 slots are **[BIN+ABI]**:

1. destructor;
2. `Render`;
3. `Dispatch`;
4. `Setup`;
5. `Shutdown`;
6. `BorrowTextures`;
7. `ReturnTextures`;
8. `UpdateComputeShaderParam`;
9. `IsActive`;
10. `UpdateParams`;
11. `SetRenderStates`;
12. `RestoreRenderStates`.

Fields include active/dirty flags, arrays of child effects and parameters, texture arrays, VS textures, input descriptors, output indices, a compute-shader flag, output count, and a dynamic-resolution flag. `EffectDesc` (`0x20`) carries start/end effect IDs, parent, wait label, and write label; this is evidence of an internally scheduled graph with synchronization labels rather than only a linear post chain.

`ImageSpaceTexture` is `0x28`: force-anisotropic flag `0x00`, `NiTexture*` `0x08`, color target ID `0x10`, depth ID `0x14`, stencil ID `0x18`, filter mode `0x1C`, clamp mode `0x20`, and acquired-target flag `0x24` **[ABI]**. It can refer either to an ordinary texture or target-registry entries; `BorrowTextures`/`ReturnTextures` must preserve the acquired flag and manager reference semantics.

### 11.2 `BSImagespaceShader` multiple inheritance

The base TypeDescriptor is RVA `0x30D0F80`. RTTI gives this exact subobject layout **[BIN]**:

| Subobject offset | Base/interface | Vtable RVA | Slots |
|---:|---|---:|---:|
| `0x000` | `BSShader` / primary | `0x2910A38` | 18 |
| `0x010` | `BSReloadShaderI` | `0x2910AD0` | 1 |
| `0x190` | `ImageSpaceEffect` | `0x2910AE0` | 12 |

Every actual derived imagespace shader found here follows this three-vtable pattern. The complete per-class table is in Appendix B. Hooking only the primary vtable misses calls made through the embedded `ImageSpaceEffect` subobject at `this + 0x190`.

### 11.3 Effect implementation anchors

| Type | TypeDescriptor RVA | Primary vtable RVA | Known size/notes |
|---|---:|---:|---|
| `ImageSpaceEffect` | `0x30D0FB0` | `0x2910DE8` | `0xB0`, 12 slots. |
| `ImageSpaceEffectAmbientOcclusion` | `0x30D4BF0` | `0x290C1B0` | AO. |
| `ImageSpaceEffectBlur` | `0x30D5128` | `0x290DE10` | 16 slots; concrete size still unverified. |
| `ImageSpaceEffectBlurCS` | `0x30D59C8` | `0x2910FC8` | 17 slots. |
| `ImageSpaceEffectBokehDepthOfField` | `0x30D4DF8` | `0x290C810` | `0xC8`. |
| `ImageSpaceEffectDepthOfField` | `0x30D5950` | `0x2910EF8` | `0x170`. |
| `ImageSpaceEffectDepthOfFieldSplitScreen` | `0x30D5988` | `0x2910F60` | Split-screen DOF. |
| `ImageSpaceEffectFullScreenBlur` | `0x30D5158` | `0x290DE98` | `0x128`. |
| `ImageSpaceEffectFullScreenColor` | `0x30D5270` | `0x290E598` | Color overlay. |
| `ImageSpaceEffectGetHit` | `0x30D5190` | `0x290DF58` | `0x108`. |
| `ImageSpaceEffectHDR` | `0x30D4BC0` | `0x290C0F8` | `0xB0`. |
| `ImageSpaceEffectHDRCS` | `0x30D59F8` | `0x2911058` | Compute HDR. |
| `ImageSpaceEffectHUDGlass` | `0x30D5A88` | `0x29111E8` | HUD glass. |
| `ImageSpaceEffectMap` | `0x30D5A28` | `0x29110C0` | Map. |
| `ImageSpaceEffectModMenu` | `0x30D50C0` | `0x290DB50` | Mod-menu post effect. |
| `ImageSpaceEffectMotionBlur` | `0x30D4CA8` | `0x290C558` | `0xB0`. |
| `ImageSpaceEffectNoise` | `0x30D5A58` | `0x2911128` | Noise. |
| `ImageSpaceEffectOption` | `0x30D4D58` | `0x290C7A8` | `0xC8`. |
| `ImageSpaceEffectParam` | `0x30D39C8` | `0x290A5C0` | Separate parameter hierarchy; one-slot vtable. |
| `ImageSpaceEffectPipboyScreen` | `0x30D50F0` | `0x290DD18` | `0xC0`. |
| `ImageSpaceEffectRadialBlur` | `0x30D51C0` | `0x290E008` | `0xB0`. |
| `ImageSpaceEffectRainSplash` | `0x30D4EB8` | `0x290CF10` | Rain. |
| `ImageSpaceEffectRefraction` | `0x30D5918` | `0x2910E50` | Refraction. |
| `ImageSpaceEffectScalableAmbientObscurance` | `0x30D4CE0` | `0x290C620` | Pixel SAO. |
| `ImageSpaceEffectScalableAmbientObscuranceCS` | `0x30D4C30` | `0x290C2B8` | Compute SAO. |
| `ImageSpaceEffectSunbeams` | `0x30D4C78` | `0x290C4D8` | Sunbeams. |
| `ImageSpaceEffectTemporalAA` | `0x30D4D20` | `0x290C700` | `0xC0`; previous inverse size and dynamic ratios. |
| `ImageSpaceEffectUpsampleDynamicResolution` | `0x30D5B20` | `0x2911358` | Dynamic-resolution output seam. |
| `ImageSpaceEffectVLS` | `0x30D5AE8` | `0x29112F0` | Volumetric-lighting composite. |
| `ImageSpaceEffectVLSLight` | `0x30D5AB8` | `0x2911288` | Volumetric-lighting generation. |
| `ImageSpaceEffectVatsTarget` | `0x30D3C88` | `0x290ADA0` | VATS target. |
| `ImageSpaceEffectWaterDisplacement` | `0x30D5088` | `0x290D8B0` | Water simulation/effect chain. |

### 11.4 `ImageSpaceManager`

The singleton pointer is at RVA `0x3E47B70`; object size is `0x1B8` **[AL+ABI]**:

| Offset | Field |
|---:|---|
| `0x000` | integer scissor rectangle |
| `0x010` | effect list |
| `0x028/30/38/40` | full-screen, colored, dynamic, and partial screen tri-shapes |
| `0x048` | partial-render enable |
| `0x04C` | main target ID |
| `0x050` | refraction tint |
| `0x060..078` | base, override, underwater, console image-space data pointers |
| `0x080` | current end-of-frame image-space data |
| `0x130` | LUT data |
| `0x1A0` | override LUT pointer |
| `0x1A8/1AC` | motion-blur intensity/max blur |
| `0x1B0` | force-no-bokeh counter |
| `0x1B4..1B6` | ready, save-target, use-bokeh flags |

The semantic API initializes and shuts down effects/geometry, adds and renders individual effects or effect ranges, processes end-of-frame stacks, handles image-space modifiers and LUTs, and borrows/returns intermediate textures **[SEM]**.

Its index space is a direct roadmap of the post pipeline:

| Range | Meaning |
|---:|---|
| `0..22` | Top-level world camera, pre-HDR TAA, sunbeams, HDR/HDR-CS, refraction, DOF, radial/full blur, motion blur, get-hit, VATS, color, gamma, FXAA/TAA, bokeh, dynamic upsample, EOF/map. |
| `23..60` | Pixel and compute blur families, including bright-pass/HDR variants. |
| `61..71` | Water displacement, noise, rain, volumetric lighting, Pip-Boy/HUD glass/mod menu, AO/SAO/SAO-CS. |
| `72..85` | Copy, scale/bias, greyscale, depth/stencil/water/shadow copies, refraction, texture mask, map/world camera shaders. |
| `86..98` | DOF, bokeh, distant blur, radial blur shaders. |
| `99..113` | HDR tonemap/downsample/luminance/adaptation shaders. |
| `114..140` | Blur, non-HDR blur, bright-pass, and HDR compute blur shaders. |
| `141..149` | Water-displacement simulation shaders. |
| `150..166` | Noise, local map, alpha/UI/Pip-Boy/HUD, VATS, mod-menu and AO shaders. |
| `167..175` | Volumetric-lighting slice/scatter/application/composite shaders. |
| `176..188` | SAO pixel and compute shaders. |
| `189..207` | Motion blur, TAA variants, gamma, sunbeams, SSR, lens flare, rain, dynamic upsample, fullscreen color, HUD glass. |
| `208..230` | Vertical/horizontal and bright-pass compute blur families. |

Appendix A records every enum name and alias. The terminal marker is 231; valid concrete shader indices end at 230.

## 12. External graphics and UI dependencies

### 12.1 Current import/call map

| Import | IAT RVA | Current call site(s) / notes |
|---|---:|---|
| `d3d11!D3D11CreateDeviceAndSwapChain` | `0x2439758` | Import thunk `0x22BD2A9`; call at `0x1824880`. |
| `dxgi!CreateDXGIFactory` | `0x2439768` | Import thunk `0x22BD2AF`; call at `0x18244A2`. |
| `GFSDK_SSAO_CreateContext_D3D11` | `0x2438150` | Import thunk `0x22B9708`; call at `0x1824A07`. |
| `nvtxRangePushA` | — | Call at `0x3CCA92`. |
| `nvtxRangePop` | — | Call at `0x3CCDB9`. |

Other imported renderer-adjacent libraries include `GFSDK_GodraysLib.x64.dll`, `GFSDK_SSAO_D3D11.win64.dll`, `flexRelease_x64.dll`, `flexExtRelease_x64.dll`, `nvToolsExt64_1.dll`, `bink2w64.dll`, Scaleform-linked engine code, XInput/audio, and Steam.

### 12.2 NVIDIA Godrays

| Imported operation | IAT RVA | Current code/call RVA | Semantic containing function |
|---|---:|---:|---|
| `OpenDX` | `0x2438118` | `0x2210EE3` | `Initialize`, logical root `0x2210ECF` |
| `Close` | `0x2438140` | leaf/import jump `0x2210F91` | `Shutdown` |
| `BeginAccumulation` | `0x2438108` | `0x221156C` | `0x2210FF0..0x22115F9` |
| `GetInternalDepth` | `0x2438138` | `0x221161F` | `SetupDepthPass`, `0x221161A..0x221165A` |
| `SetDebugMode` | `0x2438120` | `0x221267A` | `RenderVolume`, logical `0x2211BFF..0x2212B4B` |
| `RenderVolume` | `0x2438128` | `0x22127B9` | Same containing function |
| `EndAccumulation` | `0x2438130` | leaf/import jump `0x2212B84` | `EndAccumulation` |
| `ApplyLighting` | `0x2438110` | `0x2212DE2` | `0x2212B90..0x22130D2` |

Decorated import signatures expose D3D11 device/context, RTV/SRV/DSV pointers and GFSDK `ViewerDesc`, `MediumDesc`, `ShadowMapDesc`, `LightDesc`, `PostProcessDesc`, technique, buffer-size, upsample-quality and blend-state arguments **[BIN]**. This middleware cannot consume native Vulkan resources. It must be replaced, disabled, or run in a D3D11 compatibility/interop island.

### 12.3 HBAO, Scaleform, Bink, and Flex

- HBAO is initialized from the same D3D11 device-creation path. Its returned context interface and parameter flow still need vtable capture; only the creation import is statically named.
- `BSScaleformRenderer::Initialize` at `0x1A89BC0` establishes the UI backend. The renderer object is `0x398`: Scaleform `Renderer2D`, HAL/render configuration, and texture manager pointers at `0x58/60/68`; 100 `Scaleform::Render::RenderTarget*` entries at `0x70`; initialized flag at `0x390` **[ABI]**. Scaleform output must remain compositable even after world rendering moves to Vulkan.
- Bink owns video decode/playback and copies frames into graphics resources; intercept its texture/copy path before removing D3D11.
- Flex and FlexExt expose simulation buffers and may create/use D3D resources in integration code. Treat them as a compatibility subsystem, not merely unrelated physics.

A practical coexistence precedent exists in the inspected Post-NG integration: it creates shared NT-handle D3D11 textures and imports them into a D3D12 presentation path. Vulkan can use the same architectural idea with external-memory and external-semaphore support, subject to adapter/LUID matching and format support.

## 13. Console commands, settings, and string anchors

These are `.rdata` string RVAs, useful for finding registration tables and xrefs; they are not the command implementation addresses.

| String RVA | Console command |
|---:|---|
| `0x24BA840` | `ToggleScreenSpaceReflections` |
| `0x24BA868` | `ToggleScreenSpaceSubsurfaceScattering` |
| `0x24BA8C0` | `ToggleHDRCS` |
| `0x24BA8D8` | `ViewGBuffer` |
| `0x24BBE10` | `SetHDRParam` |
| `0x24BBEF0` | `PrintHDRParam` |
| `0x24BBF28` | `ToggleHDRDebug` |
| `0x24BCF50` | `ToggleEOFImageSpace` |
| `0x24BFFD0` | `DynamicResolution` |
| `0x24C0A80` | `SetSSRIntensity` |
| `0x24C0AE0` | `SetSSRBlendingPower` |
| `0x24C0B40` | `SetSSRAngleThreshold` |
| `0x24C0BA0` | `SetSSREdgeFadeFactor` |
| `0x24C0C00` | `SetSSRVerticalBlurPower` |
| `0x24C0C68` | `SetSSRVerticalAlignmentPower` |
| `0x24C0CD8` | `SetSSRVerticalStretchingPower` |
| `0x24C0D50` | `SetSSRRayStepScale` |
| `0x24C1728` | `UpdateGodraySettings` |
| `0x24C17C8` | `UpdateHBAOSettings` |
| `0x25BC7D0` | `EnablePipboyHDRMask` |
| `0x25BC8B0` | `ForceDisableSSRGodraysDirLight` |
| `0x269D4DF` | `CbForceAllRenderTarget:Display` |

Major settings and static name anchors **[BIN]** include:

- Window/device: `bFull Screen:Display` (`0x269C890`), `bBorderless:Display` (`0x269C8A8`), `iPresentInterval:Display` (`0x269C8F0`), `bShaderCache:Display` (`0x269BA50`).
- Pipeline: `bComputeShaderDeferredTiledLighting:Display` (`0x290BBC8`), `bZPrePass:Display` (`0x290BC18`), `bVolumetricLightingForceCasters:Display` (`0x290BC30`), `bUseAutoDynamicResolution:Display` (`0x290BC58`).
- SAO: enable/intensity/radius/bias at `0x290C5B8`, `0x290C5D0`, `0x290C5E8`, `0x290C600`.
- Temporal AA: five settings at `0x290C680`, `0x290C698`, `0x290C6B0`, `0x290C6C8`, `0x290C6E0`.
- Screen-space reflections: a contiguous setting family at `0x290C890..0x290C970`.
- Volumetric-lighting quality: `0x290CB08`.
- Shadow map resolution: `0x290DDE8`; shadow-distance family begins at `0x2910BF0`.
- Imagespace DOF enable: `0x2910EB0`.

The complete NVHBAO string block is `0x269B808..0x269BA30` and names 19 controls: enable, radius, bias, power exponent, blur enable/radius/sharpness, sharpness-profile enable and foreground/background depth/scales, detail AO, coarse AO, depth clamp mode, depth-threshold enable/max depth/sharpness, background-AO enable, and background-AO view depth.

These strings should be xrefed to the live `Setting` registrations and cached globals. Changing the `Setting` object alone may not affect a subsystem that latched its values; the `UpdateGodraySettings` and `UpdateHBAOSettings` execution paths are the corresponding live-apply boundaries.

### 13.1 Console command table **[verified]**

The console command table was located by anchoring on the `.rdata` command-name
strings above and finding the `.data` qwords that point at them. Pointers are
stored at the preferred image base `0x140000000`, so a name at RVA `X` appears
in the table as `0x140000000 + X`.

| Property | Value |
|---|---|
| First record | RVA `0x2EF0580` (`SetLODObjectDistance`, opcode 321) |
| Last record | RVA `0x2EF94A0` (`PBGeneric`, opcode 779) |
| Record stride | `0x50` (80 bytes) |
| Record count | 459 |

The table is self-validating: opcodes run 321..779 inclusive, which is exactly
459 values for 459 records, so `opcode == 321 + index`.

`SCRIPT_FUNCTION` record layout **[verified]**:

| Offset | Field |
|---:|---|
| `0x00` | `const char* longName` |
| `0x08` | `const char* shortName` |
| `0x10` | `uint32 opcode` |
| `0x18` | `const char* helpText` |
| `0x20` | `uint32` flags/param descriptor (`0x50000` on the observed render commands) |
| `0x28` | parameter table pointer (`.data`) |
| `0x30` | **per-command `Execute`** (`.text`) |
| `0x38` | shared compile/parse handler (`.text 0x5B6160` on every observed record) |
| `0x40`, `0x48` | null |

Worked examples:

| Command | Short | Opcode | Record RVA | Execute RVA |
|---|---|---:|---|---|
| `CenterOnCell` | `COC` | 323 | `0x2EF0620` | `0x5EB240` |
| `UpdateGodraySettings` | `gr` | 752 | `0x2EF8C30` | `0x5FD640` |
| `UpdateFlexSettings` | `flex` | 753 | `0x2EF8C80` | `0x5FE560` |
| `UpdateHBAOSettings` | `hbao` | 754 | `0x2EF8CD0` | `0x5FF...` |

The `0x38` handler being identical across records is the shared default parse
routine; the per-command work is at `0x30`. Invoking a command still requires
the compiled parameter stream that the engine's own compiler produces, so the
table alone does not make commands callable — see the journal for the
outstanding execution decision.

### 13.2 Address Library database format **[verified]**

`Data\F4SE\Plugins\version-1-11-221-0.bin` is a flat, ID-sorted table, not the
versioned Address Library v1/v2 container:

```
u64 entryCount
entryCount x { u64 id, u64 rva }      // strictly ascending by id
```

For the target build: `entryCount == 651,581`, and `8 + 651581*16` is exactly
the 10,425,304-byte file size. The database corroborates addresses this project
recorded independently — `kGraphicsStateRva` `0x03D70920` is present as ID
`2,704,621`. `renderer_host/AddressLibrary` parses it and refuses a database
that is truncated, mis-sized, non-ascending, or that names an offset outside
the PE `SizeOfImage` (`0x4244000`).

## 14. Vulkan replacement boundaries

### 14.1 Why replacing `Present` is insufficient

The engine exposes `ID3D11Device*`, immediate/deferred contexts, buffers, textures, every view type, shaders, input layouts, queries, and swap chains throughout stable in-memory structures. Middleware also accepts D3D11 pointers. Consequently:

- A `Present` hook can post-process or copy the finished image, but all scene work remains D3D11.
- Replacing only device creation would fail because the engine immediately calls a large D3D11 COM surface on the returned objects.
- Replacing only `BSShader` draw methods still leaves resource creation, target management, queries, UI, videos, and middleware on D3D11.
- Replacing only `BSBatchRenderer` is promising, but a bridge must still supply resources and final composites expected by unreplaced passes.

### 14.2 Viable architectural choices

| Choice | Boundary | Advantages | Cost/risk |
|---|---|---|---|
| D3D11/DXGI-on-Vulkan compatibility layer | Implement/proxy the D3D11 COM surface the engine already uses. | Preserves most engine and middleware behavior; enables incremental backend substitution. | Essentially a game-specific translation layer; implicit hazards/state and unusual middleware calls must be emulated. |
| Engine facade replacement | Preserve `BSGraphics::*` layouts/APIs but put Vulkan handles behind side tables and replace renderer/resource methods. | Keeps scene, materials, culling, batching, and game logic; avoids implementing unrelated D3D11 behavior. | Every remaining direct COM dereference must be found; Scaleform/GFSDK need a compatibility island. |
| Draw-packet replacement | Preserve culling/accumulation and translate `BSRenderPass`/geometry groups into Vulkan packets. | Best long-term control over passes and synchronization; operates at semantic draw level. | Requires `BSRenderPass`, material techniques, shader constants, state cache, and resource lifecycle to be fully decoded. |
| Full scene renderer replacement | Consume scene graph/game state and rebuild visibility/material/rendering independently. | Maximum freedom. | Reimplements most of the engine renderer and is the highest-risk route; not a sensible first milestone. |

The recommended direction is a combination of the middle two: maintain a small D3D11 compatibility/interop island while progressively moving engine draw packets and image-space effects to Vulkan.

## 15. Proposed staged migration

### Phase 0 — deterministic trace and replay inventory

1. Add build fingerprint and fail closed on any executable mismatch.
2. Dump all render-target descriptors and lifetime/use events.
3. Record frame phases, pass/technique IDs, geometry groups, shaders, resources, state changes, draws/dispatches, copies/resolves, queries, and thread IDs.
4. Hash every shader bytecode blob and capture its input signature, constant tables, bound resources, and representative constants.
5. Decode `RendererShadowState` by correlating changed offsets with D3D11 calls.
6. Produce small reproducible captures for menu, exterior, interior, combat, water, Power Armor/Pip-Boy, VATS, workshop, loading screen, and video playback.

Exit criterion: a frame can be described as an ordered semantic pass/resource graph, including all unknowns, without relying on a graphics debugger's UI.

### Phase 1 — Vulkan ownership and coexistence

1. Create a Vulkan instance/device on the same physical adapter as D3D11; match DXGI adapter LUID.
2. Establish Vulkan surface/swap-chain creation and resize handling, initially without taking final presentation away from the engine.
3. Prove D3D11 <-> Vulkan shared-image interop for the main color target using external memory and explicit cross-API synchronization.
4. Run a no-op Vulkan pass, then a controlled copy/composite, while preserving `Renderer::End` behavior.
5. Add validation-layer/debug-utils naming keyed by engine target IDs.

Exit criterion: a Vulkan command buffer can safely read or produce a frame image every frame, including resize/fullscreen transitions, with no CPU readback.

### Phase 2 — backend-neutral resource and draw facade

1. Introduce side tables keyed by stable engine wrapper addresses (`Buffer*`, `Texture*`, target ID, shader record pointer).
2. Mirror vertex/index buffers and textures into Vulkan; retain D3D11 objects only for unreplaced consumers.
3. Translate `VertexDesc` and input-layout cache entries.
4. Translate constant groups into per-frame rings and descriptors.
5. Capture/translate `BSRenderPass` packets and render one low-risk geometry group in Vulkan.
6. Implement lifetime retirement through timeline semaphores/fences while maintaining engine ref/pending counters.

Exit criterion: selected engine geometry renders from original assets and constants in Vulkan while the rest of the frame remains D3D11.

### Phase 3 — world pipeline

Suggested order:

1. Z prepass and simple opaque geometry.
2. G-buffer outputs.
3. Deferred lights and composite.
4. Shadows and shadow queries.
5. Forward opaque/special materials.
6. Alpha, decals, particles, first person, VATS, water, sky, and volumetrics.

Each migrated pass must preserve target contents expected by later D3D11 consumers until those consumers also migrate.

### Phase 4 — image space and temporal systems

1. Dynamic-resolution upsample.
2. TAA/history and motion vectors.
3. HDR/luminance/adaptation and tonemap.
4. AO/SAO/SSR.
5. DOF, blur, get-hit, VATS, rain, water displacement, HUD glass, and remaining effects.

The `ImageSpaceManager` enum and Appendix B give a finite migration ledger. Replace effects by ID and maintain borrowed-texture/output semantics until the complete graph is Vulkan-native.

### Phase 5 — UI, video, and middleware removal

1. Composite Scaleform UI through a shared texture or replace its render HAL.
2. Bridge or replace Bink frame upload.
3. Replace GFSDK HBAO and Godrays with Vulkan-native implementations.
4. Audit Flex integration and keep only simulation data crossing the boundary.
5. Remove D3D11 target mirrors and the compatibility device once no consumer remains.

## 16. D3D11-to-Vulkan object/state mapping

| Existing engine concept | Vulkan implementation |
|---|---|
| `RendererData.device/context` | ABI facade plus backend singleton; per-thread `VkCommandPool`/`VkCommandBuffer`. |
| Immediate context ordering | Explicit command-buffer order, barriers, queue submissions, and timeline values. |
| Deferred context / command-buffer pages | Secondary command buffers or recorded backend draw packets merged in deterministic engine order. |
| `RendererShadowState` | Decoded pipeline-state key, dynamic-state cache, descriptor binding state, current render scope. |
| VS/HS/DS/PS/CS records | SPIR-V module and reflection record keyed by engine shader/bytecode hash. |
| Technique maps | Pipeline/program variant maps keyed by the same technique IDs. |
| `ConstantGroup` | Persistently mapped per-frame uniform/storage ring slice plus descriptor/buffer offset. |
| SRV | Sampled/storage image or buffer descriptor; view format/aspect retained. |
| UAV | Storage image/buffer descriptor plus explicit read/write hazard tracking. |
| RTV/DSV arrays | `VkImageView`s and dynamic-rendering attachments or cached render-pass/framebuffer objects. |
| `RenderTargetManager` | Logical image registry, allocation policy, alias graph, layout/access history, view registry. |
| `Buffer` acquire/pending fields | Engine-compatible ownership facade plus GPU-completion retirement queue. |
| `VertexDesc` / input-layout map | Vertex binding/attribute descriptions included in graphics-pipeline keys. |
| D3D11 state objects | Immutable Vulkan pipeline/sampler objects cached by normalized descriptors. |
| Dynamic vertex-buffer ring | Host-visible upload/ring allocation with completion-aware wraparound. |
| `OcclusionQuery` | `VkQueryPool` slot plus availability/result policy. |
| D3D11 event/query synchronization | Fences/timeline semaphores and explicit result polling with matching engine-visible latency. |
| `RendererWindow.swapChain` | `VkSurfaceKHR`/`VkSwapchainKHR` state hidden behind the window facade. |
| NVTX ranges | Vulkan debug labels and optional NVTX correlation. |

### 16.1 Pipeline and descriptor keys

Do not build a Vulkan pipeline from ad-hoc observations at draw time. Normalize a key containing at least shader IDs/hashes, vertex descriptor, primitive topology, raster/cull/fill state, depth/stencil state, blend/write masks for every attachment, sample count, attachment formats, and specialization/technique bits. Separate dynamic state only where supported and profitable.

Descriptor keys need the exact engine stage/slot, resource view (not only underlying image), sampler, buffer range, and read/write intent. Null/default engine bindings must map to valid fallback descriptors or descriptor-indexing policy.

### 16.2 Barriers and aliasing

D3D11 inferred hazards from bindings; Vulkan does not. Build a per-resource usage stream from the trace and track:

- pipeline stage and access masks;
- image layout per mip/layer/aspect;
- queue-family ownership if multiple queues are used;
- render-target -> sampled/storage transitions;
- depth-write -> depth-read/sample transitions;
- UAV write-after-read/read-after-write ordering;
- copies, resolves, mip generation, and target aliases;
- external-memory acquire/release while the D3D11 island exists.

Start on one graphics queue. Async compute should only be enabled after dependency and ownership data prove overlap is safe; the existing `bUseAsyncComputeAO` setting is an engine policy hint, not proof that arbitrary Vulkan queue overlap is correct.

### 16.3 Shader conversion

Inventory whether each stage retains DXBC in memory. For stages with available bytecode, a controlled DXBC-to-SPIR-V toolchain may bootstrap parity; long-term maintainable shaders should be rebuilt from recovered HLSL/FXP semantics and validated against captured constants/resources. Preserve:

- D3D register/space-to-descriptor mapping;
- constant-buffer packing and row/column-major matrix convention;
- input semantics and packed/normalized formats;
- tessellation control-point/patch conventions;
- depth range (both APIs use 0..1, but viewport Y/front-face handling differs);
- UAV counter/atomic behavior;
- derivative and texture LOD behavior;
- clip/discard and alpha-test ordering;
- render-target format and sRGB/linear view selection.

Using a negative Vulkan viewport height can preserve D3D-style framebuffer orientation, but front-face and scissor behavior must be normalized and tested rather than assumed.

### 16.4 ABI and engine behavior that must remain stable

- Object sizes, offsets, alignments, singleton addresses, and vtable shape for any object still touched by engine code.
- Window IDs and the 32-entry window array.
- Frame counters, `insideFrame`, jitter offsets, current/previous matrices, camera cache, and TAA disable behavior.
- Dynamic-resolution ratios, default viewport selection, and target recreation timing.
- `skipNextPresent`, present interval/flags, resize requests, and foreground-window behavior.
- Buffer references, pending copies, events, and asynchronous resource-request completion.
- Pass ordering, query-result latency, sun occlusion, and job merge order.
- Default textures and behavior for absent resources.
- UI/Pip-Boy/Power Armor/VATS special masks and premultiplied-alpha conventions.

## 17. Dynamic reverse-engineering runbook

### 17.1 CDB attachment and build check

Prefer attaching to a deliberately launched test instance rather than starting the Steam-bound executable under the debugger unexpectedly:

```text
cdb.exe -p <Fallout4 PID>
lm m Fallout4
? Fallout4+0x1815BB0
```

Confirm the loaded image timestamp/size and independently hash the on-disk executable before installing any breakpoint. Example frame-boundary logging:

```text
bp Fallout4+0x1817E30 ".printf \"Renderer::Begin tid=%x window=%x\\n\", @$tid, @edx; gc"
bp Fallout4+0x1818080 ".printf \"Renderer::End tid=%x\\n\", @$tid; gc"
bp Fallout4+0x21F16D0 ".printf \"DrawWorld::Forward tid=%x\\n\", @$tid; gc"
```

Useful one-time breakpoints:

```text
bp Fallout4+0x1815BB0  ; Renderer::Init
bp Fallout4+0x1824880  ; call D3D11CreateDeviceAndSwapChain
bp Fallout4+0x1824A07  ; call GFSDK_SSAO_CreateContext_D3D11
bp Fallout4+0x183ABE0  ; target materialization/rebuild
bp Fallout4+0x18174F0  ; window size change
bp Fallout4+0x1A89BC0  ; Scaleform renderer initialize
```

Inspect the canonical pointers after initialization:

```text
dq Fallout4+0x38CAB20 L1  ; RendererData*
dq Fallout4+0x38CAB30 L1  ; current RendererWindow*
dq Fallout4+0x3E47B70 L1  ; ImageSpaceManager*
```

Given the printed `RendererData*` value, device/context are at `+0x48/+0x50`, window 0 at `+0x58`, and its swap chain at window `+0x18`. Use those live COM vtables to verify slots rather than assuming another runtime's table.

### 17.2 Instrumentation layers

Use three correlated layers:

1. **Engine layer:** `DrawWorld` phase functions, culling, accumulator registrations, batch pass begin/end, shader setup, target manager, and image-space effect IDs.
2. **D3D11 layer:** device resource/shader/state creation; context binds, draws, dispatches, copies, resolves, clears, maps, queries, and markers.
3. **Presentation/middleware layer:** Scaleform, Bink, GFSDK, Flex graphics interop, resize, and present.

Every event should include monotonically increasing frame/event IDs, thread ID, phase/pass/geometry group, relevant engine object addresses, D3D resource/view pointers, and a compact stack or caller RVA. This permits offline joins without logging full stacks on every draw.

The current local hooks have verified context vtable slots 33, 34, and 53 for target binds and depth clears. Add draw/shader/resource interception through a tested proxy/layer or validated live vtable; avoid blind patching of dozens of COM slots.

### 17.3 Experiments that decode `RendererShadowState`

At each D3D11 state-setting call:

1. Snapshot current and last-draw `0x910` blocks.
2. Diff changed offsets.
3. Log the exact D3D11 arguments and pass/technique.
4. Repeat with a single controlled setting change (wireframe, cull, depth test/write, blend, sampler, viewport, one texture slot).
5. Cluster offsets that change with the same API state.

Once fields are known, add a typed overlay structure only for proven ranges and leave unknown bytes explicit. Do not force a guessed monolithic struct over the block.

### 17.4 Capture matrix

Minimum scenarios:

- main menu and loading screen;
- exterior day/night and weather;
- interior with/without settlement lights;
- first- and third-person weapon fire;
- Power Armor, Pip-Boy, VATS, scope/reticle;
- water above/below surface and rain;
- workshop/build mode and local map;
- dialogue/cutscene and Bink playback;
- each AA mode, dynamic resolution on/off, all quality presets;
- resize, borderless/fullscreen transitions, alt-tab, device removal simulation if practical.

Run captures with debug mods disabled first, then with the expected production mod stack to identify hooks that depend on modified shaders or targets.

## 18. Unresolved map and next priorities

| Priority | Unknown | Completion test |
|---:|---|---|
| 1 | `RendererShadowState` fields | Every state-setting and draw-relevant offset has a typed meaning or an explicitly unused range. |
| 2 | `BSRenderPass` and material packet layouts | One packet can be serialized and replayed into a matching Vulkan draw. |
| 3 | Full 101/13/2 target map | Every slot has name/role, descriptor, lifetime, alias, producer, and consumers. |
| 4 | Engine `BSGraphics::Format` enum | Every observed resource format maps losslessly to DXGI and Vulkan formats/views. |
| 5 | RenderTargetManager method map | Acquire/release/create/destroy/sync/bind/copy/save/mip/current-target/viewport methods have current signatures and RVAs. |
| 6 | All `DrawWorld` phase RVAs | Current high-level frame graph has exact boundaries and callers. |
| 7 | Shader bytecode and constants | Every technique has bytecode/hash, resources, constant layout, and representative captures. |
| 8 | Thread/job submission model | Context ownership, packet allocation, merge order, and synchronization are deterministic and documented. |
| 9 | HBAO returned interface | Vtable methods/signatures and all live parameter values are captured. |
| 10 | Scaleform resource/submit boundary | UI can render to or be imported as a standalone image without D3D11 world rendering. |
| 11 | Bink and Flex graphics dependencies | All D3D resource crossings are identified and bridged/replaced. |
| 12 | Adjacent renderer globals | Readers/writers around `0x38CAB10..0x38CAB38` are mapped and aliases named. |

## 19. Sources and reproducibility

Primary evidence is the local executable and its current Address Library. External code is used only to assign ABI/semantic names to patterns that are separately compatible with this build.

- Installed `Fallout4.exe` fingerprint recorded in section 2.
- `Data/F4SE/Plugins/version-1-11-221-0.bin` for current address IDs.
- [CommonLibF4](https://github.com/libxse/commonlibf4), inspected at commit `7362677f9eec648250124c866cbef2c9d520c3f9` (2026-08-07), for current structure layouts, enums, RTTI IDs, and vtable IDs.
- [fo4test](https://github.com/doodlum/fo4test), inspected at commit `ad14b4a645fdc0923482b347192e6474a4db8b34` (2025-12-15), for the Post-NG target subset and verified high-level hook semantics.
- Local `VisualForge` source and runtime logs for depth/presentation observations.
- The mismatched local PDB only for semantic symbol-family inventories; never for current addresses.

Static extraction used PE parsing, import/IAT decoding, `.pdata` function boundaries, MSVC x64 RTTI reconstruction, address-library decoding, string xrefs, and `llvm-objdump` disassembly. Re-run all address extraction after an executable update; retain semantic names only after rematching the new build.

## Appendix A — complete `ImageSpaceManager` index enum

This is the declaration order from the inspected CommonLibF4 header. Range markers and aliases are retained because engine comparisons may use them even when they do not identify a concrete effect. Duplicate numeric values are intentional.

| Value | Enumerator | Declaration |
|---:|---|---|
| 0 | `EFFECT_WORLD_CAMERA` | explicit |
| 1 | `EFFECT_TEMPORAL_AA_PRE_HDR` | explicit |
| 2 | `EFFECT_SUNBEAMS` | explicit |
| 3 | `EFFECT_HDR` | explicit |
| 4 | `EFFECT_HDR_CS` | explicit |
| 5 | `EFFECT_REFRACTION` | explicit |
| 6 | `EFFECT_DEPTH_OF_FIELD` | explicit |
| 7 | `EFFECT_DEPTH_OF_FIELD_SPLIT_SCREEN` | explicit |
| 8 | `EFFECT_RADIAL_BLUR` | explicit |
| 9 | `EFFECT_FULLSCREEN_BLUR` | explicit |
| 10 | `EFFECT_MOTIONBLUR` | explicit |
| 11 | `EFFECT_GETHIT` | explicit |
| 12 | `EFFECT_VATS_TARGET` | explicit |
| 13 | `EFFECT_FULLSCREEN_COLOR` | explicit |
| 14 | `EFFECT_SHADER_GAMMA_CORRECT` | explicit |
| 15 | `EFFECT_SHADER_GAMMA_CORRECT_LUT` | explicit |
| 16 | `EFFECT_SHADER_GAMMA_CORRECT_RESIZE` | explicit |
| 17 | `EFFECT_SHADER_FXAA` | explicit |
| 18 | `EFFECT_TEMPORAL_AA` | explicit |
| 19 | `EFFECT_TEMPORAL_OLD_AA` | explicit |
| 20 | `EFFECT_BOKEH_DEPTH_OF_FIELD` | explicit |
| 21 | `EFFECT_UPSAMPLE_DYNAMIC_RESOLUTION` | explicit |
| 22 | `EFFECT_ENDOFFRAME_END` | explicit |
| 22 | `EFFECT_MAP` | alias of `EFFECT_ENDOFFRAME_END` |
| 23 | `EFFECT_BLUR_START` | explicit |
| 23 | `EFFECT_BLUR3` | alias of `EFFECT_BLUR_START` |
| 24 | `EFFECT_BLUR5` | explicit |
| 25 | `EFFECT_BLUR7` | explicit |
| 26 | `EFFECT_BLUR9` | explicit |
| 27 | `EFFECT_BLUR11` | explicit |
| 28 | `EFFECT_BLUR13` | explicit |
| 29 | `EFFECT_BLUR15` | explicit |
| 29 | `EFFECT_BLUR_END` | alias of `EFFECT_BLUR15` |
| 30 | `EFFECT_NONHDR_BLUR3` | explicit |
| 31 | `EFFECT_NONHDR_BLUR5` | explicit |
| 32 | `EFFECT_NONHDR_BLUR7` | explicit |
| 33 | `EFFECT_NONHDR_BLUR9` | explicit |
| 34 | `EFFECT_NONHDR_BLUR11` | explicit |
| 35 | `EFFECT_NONHDR_BLUR13` | explicit |
| 36 | `EFFECT_NONHDR_BLUR15` | explicit |
| 37 | `EFFECT_BRIGHTPASS_BLUR3` | explicit |
| 38 | `EFFECT_BRIGHTPASS_BLUR5` | explicit |
| 39 | `EFFECT_BRIGHTPASS_BLUR7` | explicit |
| 40 | `EFFECT_BRIGHTPASS_BLUR9` | explicit |
| 41 | `EFFECT_BRIGHTPASS_BLUR11` | explicit |
| 42 | `EFFECT_BRIGHTPASS_BLUR13` | explicit |
| 43 | `EFFECT_BRIGHTPASS_BLUR15` | explicit |
| 44 | `EFFECT_BRIGHTPASS_HDR_BLUR15_320x180CS` | explicit |
| 45 | `EFFECT_BRIGHTPASS_HDR_BLUR15_480x270CS` | explicit |
| 46 | `EFFECT_BRIGHTPASS_HDR_BLUR15_1024x1024CS` | explicit |
| 47 | `EFFECT_BLUR_CS_START` | explicit |
| 47 | `EFFECT_BLUR3_480x270CS` | alias of `EFFECT_BLUR_CS_START` |
| 48 | `EFFECT_BLUR5_480x270CS` | explicit |
| 49 | `EFFECT_BLUR7_480x270CS` | explicit |
| 50 | `EFFECT_BLUR9_480x270CS` | explicit |
| 51 | `EFFECT_BLUR11_480x270CS` | explicit |
| 52 | `EFFECT_BLUR13_480x270CS` | explicit |
| 53 | `EFFECT_BLUR15_480x270CS` | explicit |
| 53 | `EFFECT_BLUR_CS_END` | alias of `EFFECT_BLUR15_480x270CS` |
| 54 | `EFFECT_BRIGHTPASS_BLUR3_480x270CS` | explicit |
| 55 | `EFFECT_BRIGHTPASS_BLUR5_480x270CS` | explicit |
| 56 | `EFFECT_BRIGHTPASS_BLUR7_480x270CS` | explicit |
| 57 | `EFFECT_BRIGHTPASS_BLUR9_480x270CS` | explicit |
| 58 | `EFFECT_BRIGHTPASS_BLUR11_480x270CS` | explicit |
| 59 | `EFFECT_BRIGHTPASS_BLUR13_480x270CS` | explicit |
| 60 | `EFFECT_BRIGHTPASS_BLUR15_480x270CS` | explicit |
| 61 | `EFFECT_WATER_DISPLACEMENT` | explicit |
| 62 | `EFFECT_NOISE` | explicit |
| 63 | `EFFECT_RAINSPLASH` | explicit |
| 64 | `EFFECT_VLS_LIGHT` | explicit |
| 65 | `EFFECT_VLS` | explicit |
| 66 | `EFFECT_PIPBOY_SCREEN` | explicit |
| 67 | `EFFECT_HUD_GLASS` | explicit |
| 68 | `EFFECT_MOD_MENU` | explicit |
| 69 | `EFFECT_AO` | explicit |
| 70 | `EFFECT_SAO` | explicit |
| 71 | `EFFECT_SAO_CS` | explicit |
| 72 | `EFFECT_TOP_LEVEL_END` | explicit |
| 72 | `EFFECT_NIGHTVISION` | alias of `EFFECT_TOP_LEVEL_END` |
| 72 | `EFFECT_END` | alias of `EFFECT_TOP_LEVEL_END` |
| 72 | `EFFECT_SHADER_START` | explicit |
| 72 | `EFFECT_SHADER_COPY` | alias of `EFFECT_SHADER_START` |
| 73 | `EFFECT_SHADER_COPY_SCALE_BIAS` | explicit |
| 74 | `EFFECT_SHADER_COPY_VIS_ALPHA` | explicit |
| 75 | `EFFECT_SHADER_GREYSCALE` | explicit |
| 76 | `EFFECT_SHADER_DOWNSAMPLE_DEPTH` | explicit |
| 77 | `EFFECT_SHADER_COPY_STENCIL` | explicit |
| 78 | `EFFECT_SHADER_COPY_WATER_MASK` | explicit |
| 79 | `EFFECT_SHADER_COPY_SHADOWMAPTOARRAY` | explicit |
| 80 | `EFFECT_SHADER_REFRACTION` | explicit |
| 81 | `EFFECT_SHADER_DOUBLEVIS` | explicit |
| 82 | `EFFECT_SHADER_TEXTUREMASK` | explicit |
| 83 | `EFFECT_SHADER_MAP` | explicit |
| 84 | `EFFECT_SHADER_WORLD_CAMERA` | explicit |
| 85 | `EFFECT_SHADER_WORLD_CAMERA_NO_SKY_BLUR` | explicit |
| 86 | `EFFECT_SHADER_DEPTH_OF_FIELD` | explicit |
| 87 | `EFFECT_SHADER_DEPTH_OF_FIELD_FOGGED` | explicit |
| 88 | `EFFECT_SHADER_DEPTH_OF_FIELD_SPLIT_SCREEN` | explicit |
| 89 | `EFFECT_SHADER_BOKEH_DEPTH_OF_FIELD_PASS1` | explicit |
| 90 | `EFFECT_SHADER_BOKEH_DEPTH_OF_FIELD_PASS2` | explicit |
| 91 | `EFFECT_SHADER_BOKEH_DEPTH_OF_FIELD_PASS3` | explicit |
| 92 | `EFFECT_SHADER_BOKEH_DEPTH_OF_FIELD_PASS4` | explicit |
| 93 | `EFFECT_SHADER_BOKEH_DEPTH_OF_FIELD_PASS4_FOGGED` | explicit |
| 94 | `EFFECT_SHADER_DISTANT_BLUR` | explicit |
| 95 | `EFFECT_SHADER_DISTANT_BLUR_FOGGED` | explicit |
| 96 | `EFFECT_SHADER_RADIAL_BLUR` | explicit |
| 97 | `EFFECT_SHADER_RADIAL_BLUR_MED` | explicit |
| 98 | `EFFECT_SHADER_RADIAL_BLUR_HIGH` | explicit |
| 99 | `EFFECT_SHADER_HDR_BLENDINSHADER_CINEMATIC` | explicit |
| 100 | `EFFECT_SHADER_HDR_BLENDINSHADER_CINEMATIC_FADE` | explicit |
| 101 | `EFFECT_SHADER_HDR_DOWNSAMPLE16` | explicit |
| 102 | `EFFECT_SHADER_HDR_DOWNSAMPLE4` | explicit |
| 103 | `EFFECT_SHADER_HDR_DOWNSAMPLE16LUM` | explicit |
| 104 | `EFFECT_SHADER_HDR_DOWNSAMPLE4RGB2LUM` | explicit |
| 105 | `EFFECT_SHADER_HDR_DOWNSAMPLE4_LUMCLAMP` | explicit |
| 106 | `EFFECT_SHADER_HDR_DOWNSAMPLE4_LIGHTADAPT` | explicit |
| 107 | `EFFECT_SHADER_HDR_DOWNSAMPLE16_LUMCLAMP` | explicit |
| 108 | `EFFECT_SHADER_HDR_DOWNSAMPLE16_LIGHTADAPT` | explicit |
| 109 | `EFFECT_SHADER_HDR_DOWNSAMPLE4CS` | explicit |
| 110 | `EFFECT_SHADER_HDR_DOWNSAMPLE64RGB2LUMCS` | explicit |
| 111 | `EFFECT_SHADER_HDR_DOWNSAMPLE4LUMCS` | explicit |
| 112 | `EFFECT_SHADER_HDR_DOWNSAMPLE16LUMCS` | explicit |
| 113 | `EFFECT_SHADER_HDR_DOWNSAMPLE2_LIGHTADAPTCS` | explicit |
| 114 | `EFFECT_SHADER_BLUR_START` | explicit |
| 114 | `EFFECT_SHADER_BLUR3` | alias of `EFFECT_SHADER_BLUR_START` |
| 115 | `EFFECT_SHADER_BLUR5` | explicit |
| 116 | `EFFECT_SHADER_BLUR7` | explicit |
| 117 | `EFFECT_SHADER_BLUR9` | explicit |
| 118 | `EFFECT_SHADER_BLUR11` | explicit |
| 119 | `EFFECT_SHADER_BLUR13` | explicit |
| 120 | `EFFECT_SHADER_BLUR15` | explicit |
| 120 | `EFFECT_SHADER_BLUR_END` | explicit |
| 121 | `EFFECT_SHADER_HDR_BLURX15_320CS` | explicit |
| 122 | `EFFECT_SHADER_HDR_BLURX15_480CS` | explicit |
| 123 | `EFFECT_SHADER_HDR_BLURX15_1024CS` | explicit |
| 124 | `EFFECT_SHADER_NONHDR_BLUR_START` | explicit |
| 124 | `EFFECT_SHADER_NONHDR_BLUR3` | explicit |
| 125 | `EFFECT_SHADER_NONHDR_BLUR5` | explicit |
| 126 | `EFFECT_SHADER_NONHDR_BLUR7` | explicit |
| 127 | `EFFECT_SHADER_NONHDR_BLUR9` | explicit |
| 128 | `EFFECT_SHADER_NONHDR_BLUR11` | explicit |
| 129 | `EFFECT_SHADER_NONHDR_BLUR13` | explicit |
| 130 | `EFFECT_SHADER_NONHDR_BLUR15` | explicit |
| 130 | `EFFECT_SHADER_NONHDR_BLUR_END` | alias of `EFFECT_SHADER_NONHDR_BLUR15` |
| 131 | `EFFECT_SHADER_BRIGHTPASS_BLUR_START` | explicit |
| 131 | `EFFECT_SHADER_BRIGHTPASS_BLUR3` | alias of `EFFECT_SHADER_BRIGHTPASS_BLUR_START` |
| 132 | `EFFECT_SHADER_BRIGHTPASS_BLUR5` | explicit |
| 133 | `EFFECT_SHADER_BRIGHTPASS_BLUR7` | explicit |
| 134 | `EFFECT_SHADER_BRIGHTPASS_BLUR9` | explicit |
| 135 | `EFFECT_SHADER_BRIGHTPASS_BLUR11` | explicit |
| 136 | `EFFECT_SHADER_BRIGHTPASS_BLUR13` | explicit |
| 137 | `EFFECT_SHADER_BRIGHTPASS_BLUR15` | explicit |
| 137 | `EFFECT_SHADER_BRIGHTPASS_BLUR_END` | alias of `EFFECT_SHADER_BRIGHTPASS_BLUR15` |
| 138 | `EFFECT_SHADER_BRIGHTPASS_HDR_BLURY15_180CS` | explicit |
| 139 | `EFFECT_SHADER_BRIGHTPASS_HDR_BLURY15_270CS` | explicit |
| 140 | `EFFECT_SHADER_BRIGHTPASS_HDR_BLURY15_1024CS` | explicit |
| 141 | `EFFECT_SHADER_WATER_DISPLACEMENT_START` | explicit |
| 141 | `EFFECT_SHADER_WATER_DISPLACEMENT_CLEAR_SIMULATION` | alias of `EFFECT_SHADER_WATER_DISPLACEMENT_START` |
| 142 | `EFFECT_SHADER_WATER_DISPLACEMENT_TEX_OFFSET` | explicit |
| 143 | `EFFECT_SHADER_WATER_DISPLACEMENT_WADING_RIPPLE` | explicit |
| 144 | `EFFECT_SHADER_WATER_DISPLACEMENT_RAIN_RIPPLE` | explicit |
| 145 | `EFFECT_SHADER_WATER_DISPLACEMENT_WADING_HEIGHTMAP` | explicit |
| 146 | `EFFECT_SHADER_WATER_DISPLACEMENT_RAIN_HEIGHTMAP` | explicit |
| 147 | `EFFECT_SHADER_WATER_DISPLACEMENT_BLEND_HEIGHTMAPS` | explicit |
| 148 | `EFFECT_SHADER_WATER_DISPLACEMENT_SMOOTH_HEIGHTMAP` | explicit |
| 149 | `EFFECT_SHADER_WATER_DISPLACEMENT_NORMALS` | explicit |
| 149 | `EFFECT_SHADER_WATER_DISPLACEMENT_END` | explicit |
| 150 | `EFFECT_SHADER_NOISE_SCROLL_AND_BLEND` | explicit |
| 151 | `EFFECT_SHADER_NOISE_NORMALMAP` | explicit |
| 152 | `EFFECT_SHADER_LOCAL_MAP` | explicit |
| 153 | `EFFECT_SHADER_LOCAL_MAP_COMPANION` | explicit |
| 154 | `EFFECT_SHADER_ALPHA_BLEND` | explicit |
| 155 | `EFFECT_SHADER_PIPBOY_SCREEN` | explicit |
| 156 | `EFFECT_SHADER_HUD_GLASS` | explicit |
| 157 | `EFFECT_SHADER_HUD_GLASS_DROPSHADOW` | explicit |
| 158 | `EFFECT_SHADER_HUD_GLASS_BLURY` | explicit |
| 159 | `EFFECT_SHADER_HUD_GLASS_BLURX` | explicit |
| 160 | `EFFECT_SHADER_HUD_GLASS_MARKERS` | explicit |
| 161 | `EFFECT_SHADER_VATS_TARGET_DEBUG` | explicit |
| 162 | `EFFECT_SHADER_VATS_TARGET` | explicit |
| 163 | `EFFECT_SHADER_MOD_MENU_EFFECT` | explicit |
| 164 | `EFFECT_SHADER_MOD_MENU_GLOW_COMPOSITE` | explicit |
| 165 | `EFFECT_SHADER_AO` | explicit |
| 166 | `EFFECT_SHADER_AO_BLUR` | explicit |
| 167 | `EFFECT_SHADER_VLS_SPOTLIGHT` | explicit |
| 168 | `EFFECT_SHADER_VLS_APPLICATION` | explicit |
| 169 | `EFFECT_SHADER_VLS_COMPOSITE` | explicit |
| 170 | `EFFECT_SHADER_VLS_SLICE_COORD` | explicit |
| 171 | `EFFECT_SHADER_VLS_SLICE_INTERP` | explicit |
| 172 | `EFFECT_SHADER_VLS_SLICE_STENCIL` | explicit |
| 173 | `EFFECT_SHADER_VLS_SLICE_SCATTER_RAY` | explicit |
| 174 | `EFFECT_SHADER_VLS_SLICE_SCATTER_INTERP` | explicit |
| 175 | `EFFECT_SHADER_VLS_SCATTER_ACCUM` | explicit |
| 176 | `EFFECT_SHADER_SAO_CAMERAZ` | explicit |
| 177 | `EFFECT_SHADER_SAO_MINIFY` | explicit |
| 178 | `EFFECT_SHADER_SAO_RAWAO` | explicit |
| 179 | `EFFECT_SHADER_SAO_BLUR_H` | explicit |
| 180 | `EFFECT_SHADER_SAO_BLUR_V` | explicit |
| 181 | `EFFECT_SHADER_SAO_RAWAO_EDITOR` | explicit |
| 182 | `EFFECT_SHADER_SAO_CAMERAZ_CS` | explicit |
| 183 | `EFFECT_SHADER_SAO_MINIFY_CS` | explicit |
| 184 | `EFFECT_SHADER_SAO_CAMERAZ_AND_MIPS_CS` | explicit |
| 185 | `EFFECT_SHADER_SAO_MIPS_CS` | explicit |
| 186 | `EFFECT_SHADER_SAO_RAWAO_CS` | explicit |
| 187 | `EFFECT_SHADER_SAO_BLUR_H_CS` | explicit |
| 188 | `EFFECT_SHADER_SAO_BLUR_V_CS` | explicit |
| 189 | `EFFECT_SHADER_MOTIONBLUR` | explicit |
| 190 | `EFFECT_SHADER_TEMPORAL_AA` | explicit |
| 191 | `EFFECT_SHADER_TEMPORAL_AA_MASKED` | explicit |
| 192 | `EFFECT_SHADER_TEMPORAL_AA_POWERARMOR` | explicit |
| 193 | `EFFECT_SHADER_GAMMA_LINEARIZE` | explicit |
| 194 | `EFFECT_SHADER_SUNBEAMS` | explicit |
| 195 | `EFFECT_SHADER_SSR_PREPASS` | explicit |
| 196 | `EFFECT_SHADER_SSR_RAYTRACING` | explicit |
| 197 | `EFFECT_SHADER_SSR_BLURH` | explicit |
| 198 | `EFFECT_SHADER_SSR_BLURV` | explicit |
| 199 | `EFFECT_SHADER_LENSFLARE` | explicit |
| 200 | `EFFECT_SHADER_RAINSPLASH_SPAWN` | explicit |
| 201 | `EFFECT_SHADER_RAINSPLASH_UPDATE` | explicit |
| 202 | `EFFECT_SHADER_RAINSPLASH_DRAW` | explicit |
| 203 | `EFFECT_SHADER_LENSFLAREVISIBILITY` | explicit |
| 204 | `EFFECT_SHADER_UPSAMPLE_DYNAMIC_RESOLUTION` | explicit |
| 205 | `EFFECT_SHADER_FULLSCREEN_COLOR` | explicit |
| 206 | `EFFECT_SHADER_HUDGLASS_CLEAR` | explicit |
| 207 | `EFFECT_SHADER_HUDGLASS_COPY` | explicit |
| 208 | `EFFECT_SHADER_CS_V_START` | explicit |
| 208 | `EFFECT_SHADER_ISBlur3_V_270_CS` | explicit |
| 209 | `EFFECT_SHADER_ISBlur5_V_270_CS` | explicit |
| 210 | `EFFECT_SHADER_ISBlur7_V_270_CS` | explicit |
| 211 | `EFFECT_SHADER_ISBlur9_V_270_CS` | explicit |
| 212 | `EFFECT_SHADER_ISBlur11_V_270_CS` | explicit |
| 213 | `EFFECT_SHADER_ISBlur13_V_270_CS` | explicit |
| 214 | `EFFECT_SHADER_ISBlur15_V_270_CS` | explicit |
| 215 | `EFFECT_SHADER_CS_V_END` | explicit |
| 216 | `EFFECT_SHADER_CS_H_START` | explicit |
| 216 | `EFFECT_SHADER_ISBlur3_H_480_CS` | alias of `EFFECT_SHADER_CS_H_START` |
| 217 | `EFFECT_SHADER_ISBlur5_H_480_CS` | explicit |
| 218 | `EFFECT_SHADER_ISBlur7_H_480_CS` | explicit |
| 219 | `EFFECT_SHADER_ISBlur9_H_480_CS` | explicit |
| 220 | `EFFECT_SHADER_ISBlur11_H_480_CS` | explicit |
| 221 | `EFFECT_SHADER_ISBlur13_H_480_CS` | explicit |
| 222 | `EFFECT_SHADER_ISBlur15_H_480_CS` | explicit |
| 223 | `EFFECT_SHADER_CS_H_END` | explicit |
| 224 | `EFFECT_SHADER_BRIGHTPASS_CS_V_START` | explicit |
| 224 | `EFFECT_SHADER_ISBrightPassBlur3_V_270_CS` | explicit |
| 225 | `EFFECT_SHADER_ISBrightPassBlur5_V_270_CS` | explicit |
| 226 | `EFFECT_SHADER_ISBrightPassBlur7_V_270_CS` | explicit |
| 227 | `EFFECT_SHADER_ISBrightPassBlur9_V_270_CS` | explicit |
| 228 | `EFFECT_SHADER_ISBrightPassBlur11_V_270_CS` | explicit |
| 229 | `EFFECT_SHADER_ISBrightPassBlur13_V_270_CS` | explicit |
| 230 | `EFFECT_SHADER_ISBrightPassBlur15_V_270_CS` | explicit |
| 231 | `EFFECT_SHADER_BRIGHTPASS_CS_V_END` | explicit |

## Appendix B — complete `BSImagespaceShader` RTTI/vtable map

All 161 actual members of the hierarchy have three vtables at subobject offsets `0x000`, `0x010`, and `0x190`, with 18, 1, and 12 slots respectively. The table records the address of the first callable slot (the Complete Object Locator pointer is eight bytes before it). These are exact RVAs for the fingerprinted executable **[BIN]**.

`BSImagespaceShaderCopyParam` is intentionally excluded: despite its prefix, it derives from `ImageSpaceEffectParam`, not `BSImagespaceShader`. Its TypeDescriptor is `0x30D4B40` and its one-slot vtable is `0x290BC88`.

| Class | TypeDescriptor | Primary vtable (`+0x000`) | Reload vtable (`+0x010`) | Effect vtable (`+0x190`) |
|---|---:|---:|---:|---:|
| `BSImagespaceShader` | `0x30D0F80` | `0x2910A38` | `0x2910AD0` | `0x2910AE0` |
| `BSImagespaceShaderAlphaBlend` | `0x30D1010` | `0x28FB278` | `0x28FB310` | `0x28FB320` |
| `BSImagespaceShaderAmbientOcclusion` | `0x30D2E00` | `0x2906310` | `0x29063A8` | `0x29063B8` |
| `BSImagespaceShaderAmbientOcclusionBlur` | `0x30D2E40` | `0x2906470` | `0x2906508` | `0x2906518` |
| `BSImagespaceShaderBlur11` | `0x30D1108` | `0x28FBB48` | `0x28FBBE0` | `0x28FBBF0` |
| `BSImagespaceShaderBlur13` | `0x30D1138` | `0x28FBCB0` | `0x28FBD48` | `0x28FBD58` |
| `BSImagespaceShaderBlur15` | `0x30D1168` | `0x28FBE10` | `0x28FBEA8` | `0x28FBEB8` |
| `BSImagespaceShaderBlur3` | `0x30D1048` | `0x28FB5A8` | `0x28FB640` | `0x28FB650` |
| `BSImagespaceShaderBlur5` | `0x30D1078` | `0x28FB730` | `0x28FB7C8` | `0x28FB7D8` |
| `BSImagespaceShaderBlur7` | `0x30D10A8` | `0x28FB888` | `0x28FB920` | `0x28FB930` |
| `BSImagespaceShaderBlur9` | `0x30D10D8` | `0x28FB9E0` | `0x28FBA78` | `0x28FBA88` |
| `BSImagespaceShaderBlurX11_480CS` | `0x30D1620` | `0x28FD830` | `0x28FD8C8` | `0x28FD8D8` |
| `BSImagespaceShaderBlurX13_480CS` | `0x30D15E8` | `0x28FD6E0` | `0x28FD778` | `0x28FD788` |
| `BSImagespaceShaderBlurX15_480CS` | `0x30D15B0` | `0x28FD590` | `0x28FD628` | `0x28FD638` |
| `BSImagespaceShaderBlurX3_480CS` | `0x30D1700` | `0x28FDD70` | `0x28FDE08` | `0x28FDE18` |
| `BSImagespaceShaderBlurX5_480CS` | `0x30D16C8` | `0x28FDC20` | `0x28FDCB8` | `0x28FDCC8` |
| `BSImagespaceShaderBlurX7_480CS` | `0x30D1690` | `0x28FDAD0` | `0x28FDB68` | `0x28FDB78` |
| `BSImagespaceShaderBlurX9_480CS` | `0x30D1658` | `0x28FD980` | `0x28FDA18` | `0x28FDA28` |
| `BSImagespaceShaderBlurY11_270CS` | `0x30D17A8` | `0x28FE170` | `0x28FE208` | `0x28FE218` |
| `BSImagespaceShaderBlurY13_270CS` | `0x30D1770` | `0x28FE020` | `0x28FE0B8` | `0x28FE0C8` |
| `BSImagespaceShaderBlurY15_270CS` | `0x30D1738` | `0x28FDEC0` | `0x28FDF58` | `0x28FDF68` |
| `BSImagespaceShaderBlurY3_270CS` | `0x30D1888` | `0x28FE6A8` | `0x28FE740` | `0x28FE750` |
| `BSImagespaceShaderBlurY5_270CS` | `0x30D1850` | `0x28FE560` | `0x28FE5F8` | `0x28FE608` |
| `BSImagespaceShaderBlurY7_270CS` | `0x30D1818` | `0x28FE410` | `0x28FE4A8` | `0x28FE4B8` |
| `BSImagespaceShaderBlurY9_270CS` | `0x30D17E0` | `0x28FE2C0` | `0x28FE358` | `0x28FE368` |
| `BSImagespaceShaderBokehDepthOfFieldPass1` | `0x30D32F8` | `0x2908188` | `0x2908220` | `0x2908230` |
| `BSImagespaceShaderBokehDepthOfFieldPass2` | `0x30D3338` | `0x2908330` | `0x29083C8` | `0x29083D8` |
| `BSImagespaceShaderBokehDepthOfFieldPass3` | `0x30D3378` | `0x2908498` | `0x2908530` | `0x2908540` |
| `BSImagespaceShaderBokehDepthOfFieldPass4` | `0x30D33B8` | `0x29085F8` | `0x2908690` | `0x29086A0` |
| `BSImagespaceShaderBokehDepthOfFieldPass4Fogged` | `0x30D3400` | `0x2908750` | `0x29087E8` | `0x29087F8` |
| `BSImagespaceShaderBrightPassBlur11` | `0x30D1400` | `0x28FCDA0` | `0x28FCE38` | `0x28FCE48` |
| `BSImagespaceShaderBrightPassBlur13` | `0x30D1440` | `0x28FCEF0` | `0x28FCF88` | `0x28FCF98` |
| `BSImagespaceShaderBrightPassBlur15` | `0x30D1480` | `0x28FD040` | `0x28FD0D8` | `0x28FD0E8` |
| `BSImagespaceShaderBrightPassBlur3` | `0x30D1320` | `0x28FC838` | `0x28FC8D0` | `0x28FC8E0` |
| `BSImagespaceShaderBrightPassBlur5` | `0x30D1358` | `0x28FC9B0` | `0x28FCA48` | `0x28FCA58` |
| `BSImagespaceShaderBrightPassBlur7` | `0x30D1390` | `0x28FCB00` | `0x28FCB98` | `0x28FCBA8` |
| `BSImagespaceShaderBrightPassBlur9` | `0x30D13C8` | `0x28FCC50` | `0x28FCCE8` | `0x28FCCF8` |
| `BSImagespaceShaderBrightPassBlurY11_270CS` | `0x30D1A30` | `0x28FEEB8` | `0x28FEF50` | `0x28FEF60` |
| `BSImagespaceShaderBrightPassBlurY13_270CS` | `0x30D19F0` | `0x28FED78` | `0x28FEE10` | `0x28FEE20` |
| `BSImagespaceShaderBrightPassBlurY15_270CS` | `0x30D19B0` | `0x28FEC18` | `0x28FECB0` | `0x28FECC0` |
| `BSImagespaceShaderBrightPassBlurY3_270CS` | `0x30D1B30` | `0x28FF438` | `0x28FF4D0` | `0x28FF4E0` |
| `BSImagespaceShaderBrightPassBlurY5_270CS` | `0x30D1AF0` | `0x28FF2D8` | `0x28FF370` | `0x28FF380` |
| `BSImagespaceShaderBrightPassBlurY7_270CS` | `0x30D1AB0` | `0x28FF178` | `0x28FF210` | `0x28FF220` |
| `BSImagespaceShaderBrightPassBlurY9_270CS` | `0x30D1A70` | `0x28FF018` | `0x28FF0B0` | `0x28FF0C0` |
| `BSImagespaceShaderBrightPassHDRBlurY15_1024CS` | `0x30D1960` | `0x28FEAB8` | `0x28FEB50` | `0x28FEB60` |
| `BSImagespaceShaderBrightPassHDRBlurY15_180CS` | `0x30D18C0` | `0x28FE7F8` | `0x28FE890` | `0x28FE8A0` |
| `BSImagespaceShaderBrightPassHDRBlurY15_270CS` | `0x30D1910` | `0x28FE958` | `0x28FE9F0` | `0x28FEA00` |
| `BSImagespaceShaderCopy` | `0x30D1B70` | `0x28FF598` | `0x28FF630` | `0x28FF640` |
| `BSImagespaceShaderCopyNormals` | `0x30D1D68` | `0x29001B8` | `0x2900250` | `0x2900260` |
| `BSImagespaceShaderCopyScaleBias` | `0x30D1BA0` | `0x28FF6C8` | `0x28FF760` | `0x28FF770` |
| `BSImagespaceShaderCopyShadowMapToArray` | `0x30D1D28` | `0x2900058` | `0x29000F0` | `0x2900100` |
| `BSImagespaceShaderCopyStencil` | `0x30D1CB8` | `0x28FFDA0` | `0x28FFE38` | `0x28FFE48` |
| `BSImagespaceShaderCopyVisAlpha` | `0x30D1BD8` | `0x28FF828` | `0x28FF8C0` | `0x28FF8D0` |
| `BSImagespaceShaderCopyWaterMask` | `0x30D1CF0` | `0x28FFEF8` | `0x28FFF90` | `0x28FFFA0` |
| `BSImagespaceShaderDepthOfField` | `0x30D1DA0` | `0x2900308` | `0x29003A0` | `0x29003B0` |
| `BSImagespaceShaderDepthOfFieldFogged` | `0x30D1DD8` | `0x2900480` | `0x2900518` | `0x2900528` |
| `BSImagespaceShaderDepthOfFieldSplitScreen` | `0x30D1E20` | `0x29005E8` | `0x2900680` | `0x2900690` |
| `BSImagespaceShaderDistantBlur` | `0x30D1E60` | `0x2900748` | `0x29007E0` | `0x29007F0` |
| `BSImagespaceShaderDistantBlurFogged` | `0x30D1E98` | `0x2900890` | `0x2900928` | `0x2900938` |
| `BSImagespaceShaderDoubleVision` | `0x30D1ED8` | `0x29009E0` | `0x2900A78` | `0x2900A88` |
| `BSImagespaceShaderDownsampleDepth` | `0x30D1C80` | `0x28FFC38` | `0x28FFCD0` | `0x28FFCE0` |
| `BSImagespaceShaderFullScreenColor` | `0x30D1F10` | `0x2900B50` | `0x2900BE8` | `0x2900BF8` |
| `BSImagespaceShaderFXAA` | `0x30D1F48` | `0x2900CB0` | `0x2900D48` | `0x2900D58` |
| `BSImagespaceShaderGammaCorrect` | `0x30D2038` | `0x2901338` | `0x29013D0` | `0x29013E0` |
| `BSImagespaceShaderGammaCorrectLUT` | `0x30D20E8` | `0x2901730` | `0x29017C8` | `0x29017D8` |
| `BSImagespaceShaderGammaCorrectResize` | `0x30D2070` | `0x2901470` | `0x2901508` | `0x2901518` |
| `BSImagespaceShaderGammaLinearize` | `0x30D20B0` | `0x29015D0` | `0x2901668` | `0x2901678` |
| `BSImagespaceShaderGreyScale` | `0x30D1C48` | `0x28FFAD8` | `0x28FFB70` | `0x28FFB80` |
| `BSImagespaceShaderHDRBlurX15_1024CS` | `0x30D1570` | `0x28FD448` | `0x28FD4E0` | `0x28FD4F0` |
| `BSImagespaceShaderHDRBlurX15_320CS` | `0x30D14C0` | `0x28FD190` | `0x28FD228` | `0x28FD238` |
| `BSImagespaceShaderHDRBlurX15_480CS` | `0x30D1530` | `0x28FD300` | `0x28FD398` | `0x28FD3A8` |
| `BSImagespaceShaderHDRDownSample16` | `0x30D21D8` | `0x2901CC8` | `0x2901D60` | `0x2901D70` |
| `BSImagespaceShaderHDRDownSample16LightAdapt` | `0x30D22E0` | `0x2902278` | `0x2902310` | `0x2902320` |
| `BSImagespaceShaderHDRDownSample16Lum` | `0x30D2158` | `0x2901A08` | `0x2901AA0` | `0x2901AB0` |
| `BSImagespaceShaderHDRDownSample16LumClamp` | `0x30D2250` | `0x2901F90` | `0x2902028` | `0x2902038` |
| `BSImagespaceShaderHDRDownSample16LumCS` | `0x30D2448` | `0x2902978` | `0x2902A10` | `0x2902A20` |
| `BSImagespaceShaderHDRDownSample2LightAdaptCS` | `0x30D24E0` | `0x2902C28` | `0x2902CC0` | `0x2902CD0` |
| `BSImagespaceShaderHDRDownSample4` | `0x30D2120` | `0x2901888` | `0x2901920` | `0x2901930` |
| `BSImagespaceShaderHDRDownSample4CS` | `0x30D23C8` | `0x29026D0` | `0x2902768` | `0x2902778` |
| `BSImagespaceShaderHDRDownSample4LightAdapt` | `0x30D2290` | `0x29020F0` | `0x2902188` | `0x2902198` |
| `BSImagespaceShaderHDRDownSample4LumClamp` | `0x30D2210` | `0x2901E18` | `0x2901EB0` | `0x2901EC0` |
| `BSImagespaceShaderHDRDownSample4LumCS` | `0x30D2408` | `0x2902820` | `0x29028B8` | `0x29028C8` |
| `BSImagespaceShaderHDRDownSample4RGB2Lum` | `0x30D2198` | `0x2901B70` | `0x2901C08` | `0x2901C18` |
| `BSImagespaceShaderHDRDownSample64RGB2LumCS` | `0x30D2490` | `0x2902AC8` | `0x2902B60` | `0x2902B70` |
| `BSImagespaceShaderHDRTonemapBlendCinematic` | `0x30D2330` | `0x29023D8` | `0x2902470` | `0x2902480` |
| `BSImagespaceShaderHDRTonemapBlendCinematicFade` | `0x30D2380` | `0x2902560` | `0x29025F8` | `0x2902608` |
| `BSImagespaceShaderHUDGlass` | `0x30D29F0` | `0x29048B8` | `0x2904950` | `0x2904960` |
| `BSImagespaceShaderHUDGlassBlurX` | `0x30D2AA0` | `0x2904CA0` | `0x2904D38` | `0x2904D48` |
| `BSImagespaceShaderHUDGlassBlurY` | `0x30D2A68` | `0x2904B58` | `0x2904BF0` | `0x2904C00` |
| `BSImagespaceShaderHUDGlassClear` | `0x30D2B10` | `0x2904F40` | `0x2904FD8` | `0x2904FE8` |
| `BSImagespaceShaderHUDGlassCopy` | `0x30D2B48` | `0x2905088` | `0x2905120` | `0x2905130` |
| `BSImagespaceShaderHUDGlassDropShadow` | `0x30D2A28` | `0x2904A00` | `0x2904A98` | `0x2904AA8` |
| `BSImagespaceShaderHUDGlassMarkers` | `0x30D2AD8` | `0x2904DE8` | `0x2904E80` | `0x2904E90` |
| `BSImagespaceShaderLensFlare` | `0x30D35B8` | `0x29091D0` | `0x2909268` | `0x2909278` |
| `BSImagespaceShaderLensFlareVisibility` | `0x30D35F0` | `0x29092E0` | `0x2909378` | `0x2909388` |
| `BSImagespaceShaderLocalMap` | `0x30D2528` | `0x2902D90` | `0x2902E28` | `0x2902E38` |
| `BSImagespaceShaderLocalMapCompanion` | `0x30D2560` | `0x2902ED0` | `0x2902F68` | `0x2902F78` |
| `BSImagespaceShaderMap` | `0x30D25A0` | `0x2903030` | `0x29030C8` | `0x29030D8` |
| `BSImagespaceShaderModMenuEffect` | `0x30D2D88` | `0x2906068` | `0x2906100` | `0x2906110` |
| `BSImagespaceShaderModMenuGlowComposite` | `0x30D2DC0` | `0x29061C0` | `0x2906258` | `0x2906268` |
| `BSImagespaceShaderMotionBlur` | `0x30D2E80` | `0x2906600` | `0x2906698` | `0x29066A8` |
| `BSImagespaceShaderNoiseNormalmap` | `0x30D2610` | `0x2903350` | `0x29033E8` | `0x29033F8` |
| `BSImagespaceShaderNoiseScrollAndBlend` | `0x30D25D0` | `0x2903170` | `0x2903208` | `0x2903218` |
| `BSImagespaceShaderNonHDRBlur11` | `0x30D1278` | `0x28FC478` | `0x28FC510` | `0x28FC520` |
| `BSImagespaceShaderNonHDRBlur13` | `0x30D12B0` | `0x28FC5B8` | `0x28FC650` | `0x28FC660` |
| `BSImagespaceShaderNonHDRBlur15` | `0x30D12E8` | `0x28FC6F8` | `0x28FC790` | `0x28FC7A0` |
| `BSImagespaceShaderNonHDRBlur3` | `0x30D1198` | `0x28FBF78` | `0x28FC010` | `0x28FC020` |
| `BSImagespaceShaderNonHDRBlur5` | `0x30D11D0` | `0x28FC0B8` | `0x28FC150` | `0x28FC160` |
| `BSImagespaceShaderNonHDRBlur7` | `0x30D1208` | `0x28FC1F8` | `0x28FC290` | `0x28FC2A0` |
| `BSImagespaceShaderNonHDRBlur9` | `0x30D1240` | `0x28FC338` | `0x28FC3D0` | `0x28FC3E0` |
| `BSImagespaceShaderPipboyScreen` | `0x30D29B8` | `0x2904758` | `0x29047F0` | `0x2904800` |
| `BSImagespaceShaderRadialBlur` | `0x30D2648` | `0x29034B0` | `0x2903548` | `0x2903558` |
| `BSImagespaceShaderRadialBlurHigh` | `0x30D26C0` | `0x2903770` | `0x2903808` | `0x2903818` |
| `BSImagespaceShaderRadialBlurMedium` | `0x30D2680` | `0x2903620` | `0x29036B8` | `0x29036C8` |
| `BSImagespaceShaderRainSplash` | `0x30D3448` | `0x2908888` | `0x2908920` | `0x2908930` |
| `BSImagespaceShaderRainSplashDraw` | `0x30D34C0` | `0x2908B90` | `0x2908C28` | `0x2908C38` |
| `BSImagespaceShaderRainSplashUpdate` | `0x30D3480` | `0x2908A30` | `0x2908AC8` | `0x2908AD8` |
| `BSImagespaceShaderRefraction` | `0x30D26F8` | `0x29038C0` | `0x2903958` | `0x2903968` |
| `BSImagespaceShaderSAOBlurH` | `0x30D2F60` | `0x2906BE8` | `0x2906C80` | `0x2906C90` |
| `BSImagespaceShaderSAOBlurHCS` | `0x30D30F0` | `0x29074D8` | `0x2907570` | `0x2907580` |
| `BSImagespaceShaderSAOBlurV` | `0x30D2F98` | `0x2906D40` | `0x2906DD8` | `0x2906DE8` |
| `BSImagespaceShaderSAOBlurVCS` | `0x30D3128` | `0x2907638` | `0x29076D0` | `0x29076E0` |
| `BSImagespaceShaderSAOCameraAndMipsZCS` | `0x30D3008` | `0x2906FC8` | `0x2907060` | `0x2907070` |
| `BSImagespaceShaderSAOCameraZ` | `0x30D2EB8` | `0x2906788` | `0x2906820` | `0x2906830` |
| `BSImagespaceShaderSAOCameraZCS` | `0x30D2FD0` | `0x2906E88` | `0x2906F20` | `0x2906F30` |
| `BSImagespaceShaderSAOMinify` | `0x30D2EF0` | `0x29068E8` | `0x2906980` | `0x2906990` |
| `BSImagespaceShaderSAOMinifyCS` | `0x30D3080` | `0x2907258` | `0x29072F0` | `0x2907300` |
| `BSImagespaceShaderSAOMipsZCS` | `0x30D3048` | `0x2907118` | `0x29071B0` | `0x29071C0` |
| `BSImagespaceShaderSAORawAO` | `0x30D2F28` | `0x2906A48` | `0x2906AE0` | `0x2906AF0` |
| `BSImagespaceShaderSAORawAOCS` | `0x30D30B8` | `0x2907398` | `0x2907430` | `0x2907440` |
| `BSImagespaceShaderSAORawAOEditor` | `0x30D3160` | `0x2907780` | `0x2907818` | `0x2907828` |
| `BSImagespaceShaderSSLRBlurH` | `0x30D3198` | `0x29078D0` | `0x2907968` | `0x2907978` |
| `BSImagespaceShaderSSLRBlurV` | `0x30D31D0` | `0x2907A48` | `0x2907AE0` | `0x2907AF0` |
| `BSImagespaceShaderSSLRPrepass` | `0x30D3240` | `0x2907D38` | `0x2907DD0` | `0x2907DE0` |
| `BSImagespaceShaderSSLRRaytracing` | `0x30D3208` | `0x2907B88` | `0x2907C20` | `0x2907C30` |
| `BSImagespaceShaderSunbeams` | `0x30D3278` | `0x2907EB0` | `0x2907F48` | `0x2907F58` |
| `BSImagespaceShaderTemporalAA` | `0x30D1F78` | `0x2900E28` | `0x2900EC0` | `0x2900ED0` |
| `BSImagespaceShaderTemporalAAPipboy` | `0x30D1FB0` | `0x2901070` | `0x2901108` | `0x2901118` |
| `BSImagespaceShaderTemporalAAPowerArmorPipboy` | `0x30D1FF0` | `0x29011C0` | `0x2901258` | `0x2901268` |
| `BSImagespaceShaderTextureMask` | `0x30D1C10` | `0x28FF978` | `0x28FFA10` | `0x28FFA20` |
| `BSImagespaceShaderUpsampleDynamicResolution` | `0x30D32B0` | `0x2908010` | `0x29080A8` | `0x29080B8` |
| `BSImagespaceShaderVatsTarget` | `0x30D0FD8` | `0x28FB168` | `0x28FB200` | `0x28FB210` |
| `BSImagespaceShaderVatsTargetDebug` | `0x30D0F48` | `0x28FAFC0` | `0x28FB058` | `0x28FB068` |
| `BSImagespaceShaderVLSApplication` | `0x30D2D18` | `0x2905DA8` | `0x2905E40` | `0x2905E50` |
| `BSImagespaceShaderVLSComposite` | `0x30D2D50` | `0x2905F28` | `0x2905FC0` | `0x2905FD0` |
| `BSImagespaceShaderVLSScatterAccum` | `0x30D2CA8` | `0x2905AC8` | `0x2905B60` | `0x2905B70` |
| `BSImagespaceShaderVLSSliceCoord` | `0x30D2B80` | `0x29051D0` | `0x2905268` | `0x2905278` |
| `BSImagespaceShaderVLSSliceInterp` | `0x30D2BB8` | `0x2905368` | `0x2905400` | `0x2905410` |
| `BSImagespaceShaderVLSSliceScatterInterp` | `0x30D2C68` | `0x2905938` | `0x29059D0` | `0x29059E0` |
| `BSImagespaceShaderVLSSliceScatterRay` | `0x30D2C28` | `0x2905660` | `0x29056F8` | `0x2905708` |
| `BSImagespaceShaderVLSSliceStencil` | `0x30D2BF0` | `0x29054E0` | `0x2905578` | `0x2905588` |
| `BSImagespaceShaderVLSSpotLight` | `0x30D2CE0` | `0x2905C58` | `0x2905CF0` | `0x2905D00` |
| `BSImagespaceShaderWaterBlendHeightmaps` | `0x30D28E8` | `0x2904310` | `0x29043A8` | `0x29043B8` |
| `BSImagespaceShaderWaterDisplacementClearSimulation` | `0x30D2730` | `0x2903A00` | `0x2903A98` | `0x2903AA8` |
| `BSImagespaceShaderWaterDisplacementNormals` | `0x30D2970` | `0x29045F0` | `0x2904688` | `0x2904698` |
| `BSImagespaceShaderWaterDisplacementRainRipple` | `0x30D2820` | `0x2903EE8` | `0x2903F80` | `0x2903F90` |
| `BSImagespaceShaderWaterDisplacementTexOffset` | `0x30D2780` | `0x2903B90` | `0x2903C28` | `0x2903C38` |
| `BSImagespaceShaderWaterDisplacementWadingRipple` | `0x30D27D0` | `0x2903D48` | `0x2903DE0` | `0x2903DF0` |
| `BSImagespaceShaderWaterRainHeightmap` | `0x30D28A8` | `0x29041C0` | `0x2904258` | `0x2904268` |
| `BSImagespaceShaderWaterSmoothHeightmap` | `0x30D2928` | `0x2904490` | `0x2904528` | `0x2904538` |
| `BSImagespaceShaderWaterWadingHeightmap` | `0x30D2868` | `0x2904050` | `0x29040E8` | `0x29040F8` |

## Appendix C — semantic renderer API inventory

The following distinct public signatures were extracted from the mismatched local PDB. They are retained as **[SEM]** names for cross-build matching and replacement-surface planning. They do **not** establish current RVAs, and signatures may have changed in 1.11.221. Compiler-generated deleting destructors, operators, and dynamic-initializer helpers are omitted.

### C.1 `BSGraphics::Renderer` (237 signatures)

- `AddTextureToArray(BSGraphics::Texture*,BSGraphics::Texture*,uint)`
- `AlphaBlendStateSetMode(BSGraphics::AlphaBlendMode)`
- `AlphaBlendStateSetWriteMode(BSGraphics::AlphaBlendWriteMode)`
- `ApplyConstantGroupCS(BSGraphics::ConstantGroup*,BSGraphics::ConstantGroupLevel)`
- `ApplyConstantGroupDS(BSGraphics::ConstantGroup*,BSGraphics::ConstantGroupLevel)`
- `ApplyConstantGroupPS(BSGraphics::ConstantGroup*,BSGraphics::ConstantGroupLevel)`
- `ApplyConstantGroupVS(BSGraphics::ConstantGroup*,BSGraphics::ConstantGroupLevel)`
- `ApplyConstantGroupVSPS(BSGraphics::ConstantGroup*,BSGraphics::ConstantGroup*,BSGraphics::ConstantGroupLevel)`
- `Begin(uint)`
- `BeginOcclusionQuery(BSGraphics::OcclusionQuery*)`
- `BuildDrawCommandBuffer(BSGraphics::TriShape*,BSGraphics::CommandBufferPlatform*,BSGraphics::VertexShader*,BSGraphics::AlphaBlendMode,BSGraphics::AlphaBlendWriteMode,char*,uint,char*,uint,uint)`
- `BuildDynamicTriShapeData(BSGraphics::DynamicTriShapeData&,uint)`
- `BuildPSBufferSRCommandBuffer(uint,BSGraphics::IndexBuffer*,BSGraphics::CommandBufferShaderResource*)`
- `BuildPSBufferSRCommandBuffer(uint,BSGraphics::VertexBuffer*,BSGraphics::CommandBufferShaderResource*)`
- `BuildTextureCommandBuffer(uint,BSGraphics::Texture**,BSGraphics::TextureAddressMode,BSGraphics::TextureFilterMode,BSGraphics::CommandBufferTexture*)`
- `BuildVSBufferSRCommandBuffer(uint,BSGraphics::ByteBuffer*,BSGraphics::CommandBufferShaderResource*)`
- `BuildVSBufferSRCommandBuffer(uint,BSGraphics::IndexBuffer*,BSGraphics::CommandBufferShaderResource*)`
- `BuildVSBufferSRCommandBuffer(uint,BSGraphics::VertexBuffer*,BSGraphics::CommandBufferShaderResource*)`
- `BuildVSStructuredBufferSRCommandBuffer(uint,BSGraphics::StructuredBuffer*,BSGraphics::CommandBufferShaderResource*)`
- `CSSetBufferSR(uint,BSGraphics::IndexBuffer*)`
- `CSSetBufferSR(uint,BSGraphics::VertexBuffer*)`
- `CSSetBufferUAV(uint,BSGraphics::IndexBuffer*)`
- `CSSetBufferUAV(uint,BSGraphics::VertexBuffer*)`
- `CSSetStructuredBufferSR(uint,BSGraphics::StructuredBuffer*)`
- `CSSetStructuredBufferUAV(uint,BSGraphics::StructuredBuffer*)`
- `CanTextureDegrade(BSGraphics::Texture*)`
- `CheckCompatibleWithArray(BSGraphics::Texture*,BSGraphics::Texture*)`
- `CleanupCommandBuffer(char*)`
- `CleanupDynamicTriShapeData(BSGraphics::DynamicTriShapeData&)`
- `ClearBufferUAVSRV(BSGraphics::ByteBuffer*)`
- `ClearBufferUAVSRV(BSGraphics::IndexBuffer*)`
- `ClearBufferUAVSRV(BSGraphics::VertexBuffer*)`
- `ClearColor(void)`
- `ClearComputeShader(void)`
- `ClearDepthStencil(BSGraphics::ClearDepthStencilTarget)`
- `ClearPixelShader(void)`
- `ClearResources(void)`
- `ConvertTriShapeToDynamicTriShape(BSGraphics::TriShape*,uint)`
- `CopyBuffer(BSGraphics::IndexBuffer*,uint,BSGraphics::IndexBuffer*,uint,uint,bool,BSGraphics::ComputeContextId)`
- `CopyBuffer(BSGraphics::VertexBuffer*,uint,BSGraphics::VertexBuffer*,uint,uint,bool,BSGraphics::ComputeContextId)`
- `CopyRenderTargetToRenderTargetCopy(uint,uint)`
- `CopyTexture(BSGraphics::Texture*,BSGraphics::Texture*,uint,uint,uint,uint)`
- `CopyTexturesOffline(BSGraphics::Texture*,uint,BSGraphics::Texture**,int*,int*)`
- `Create3DTextureFrom2D(BSResourceNiBinaryStream&)`
- `Create3DTextureFrom2D(char*)`
- `CreateBufferSR(BSGraphics::IndexBuffer*)`
- `CreateBufferSR(BSGraphics::VertexBuffer*)`
- `CreateBufferUAV(BSGraphics::IndexBuffer*)`
- `CreateBufferUAV(BSGraphics::VertexBuffer*)`
- `CreateByteBuffer(uint,void*)`
- `CreateComputeShaderFromStream(BSIStream*)`
- `CreateContext(bool,bool,bool,unsigned___int64,bool)`
- `CreateCubeMapRenderTarget(int,wchar_t*,BSGraphics::CubeMapRenderTargetProperties&)`
- `CreateDepthStencilTarget(int,wchar_t*,BSGraphics::DepthStencilTargetProperties&)`
- `CreateDomainShaderFromStream(BSIStream*)`
- `CreateDynamicLineShape(void*,uint,unsigned___int64,uint,ushort*,uint)`
- `CreateDynamicTriShape(unsigned___int64,uint)`
- `CreateDynamicTriShape(void*,uint,unsigned___int64,uint,ushort*,uint)`
- `CreateEmptyTexture(uint,uint)`
- `CreateHullShaderFromStream(BSIStream*)`
- `CreateIndexBuffer(uint,ushort*)`
- `CreateLineShape(void*,uint,unsigned___int64,uint,ushort*,uint)`
- `CreateOcclusionQuery(void)`
- `CreatePixelShaderFromBuffer(void*,unsigned___int64,uint,uchar*,uchar*)`
- `CreatePixelShaderFromStream(BSIStream*)`
- `CreateRenderTarget(int,wchar_t*,BSGraphics::RenderTargetProperties&)`
- `CreateStreamingTexture(BSTextureStreamer::NativeDesc<BSGraphics::TextureHeader>&,bool,bool,BSTextureStreamer::ChunkReadDesc&,bool)`
- `CreateStreamingTextureArraySlice(BSGraphics::Texture*,BSTextureStreamer::NativeDesc<BSGraphics::TextureHeader>&,BSTextureStreamer::ChunkReadDesc&,uint)`
- `CreateStructuredBuffer(uint,uint,bool,bool,void*,bool,bool)`
- `CreateTextureArrayEmpty(BSGraphics::Texture*,uint)`
- `CreateTextureArrayEmpty(BSGraphics::Texture*,uint,uint)`
- `CreateTextureArrayEmpty(BSGraphics::TextureHeader&,uint)`
- `CreateTextureArrayEmpty(BSGraphics::TextureHeader&,uint,uint)`
- `CreateTextureFromBuffer(uint,uint,char*,bool,bool,BSGraphics::Usage,BSGraphics::Format,bool)`
- `CreateTextureFromFile(char*,BSGraphics::TextureFileFormat,bool)`
- `CreateTextureFromRenderTarget(uint)`
- `CreateTextureFromStream(BSResourceNiBinaryStream*,bool,bool,bool)`
- `CreateTextureStreamData(BSTextureStreamer::ChunkDesc*,uint*,uint,uint)`
- `CreateTriShape(BSGraphics::VertexBuffer*,unsigned___int64,BSGraphics::IndexBuffer*)`
- `CreateTriShape(BSGraphics::VertexBuffer*,unsigned___int64,ushort*,uint)`
- `CreateTriShape(BSScrapArray<BSGraphics::TriShape*>&)`
- `CreateTriShape(NiStream&,unsigned___int64,uint,uint,char*&)`
- `CreateTriShape(uint&,void*,unsigned___int64,BSGraphics::IndexBuffer*)`
- `CreateTriShape(uint&,void*,unsigned___int64,ushort*,uint)`
- `CreateVertexBuffer(uint&,void*,uint,unsigned___int64)`
- `CreateVertexShaderFromBuffer(void*,unsigned___int64,uint,unsigned___int64,uchar*,uchar*)`
- `CreateVertexShaderFromStream(BSIStream*)`
- `DecRef(BSGraphics::ByteBuffer*)`
- `DecRef(BSGraphics::DynamicLineShape*)`
- `DecRef(BSGraphics::DynamicTriShape*)`
- `DecRef(BSGraphics::IndexBuffer*)`
- `DecRef(BSGraphics::LineShape*)`
- `DecRef(BSGraphics::StructuredBuffer*)`
- `DecRef(BSGraphics::Texture*)`
- `DecRef(BSGraphics::TextureStreamData*)`
- `DecRef(BSGraphics::TriShape*)`
- `DecRef(BSGraphics::VertexBuffer*)`
- `DepthStencilStateSetDepthMode(BSGraphics::DepthStencilDepthMode)`
- `DepthStencilStateSetExMode(BSGraphics::DepthStencilExtraMode)`
- `DestroyContext(BSGraphics::Context*)`
- `DestroyCubeMapRenderTarget(uint)`
- `DestroyDepthStencilTarget(uint)`
- `DestroyOcclusionQuery(BSGraphics::OcclusionQuery*)`
- `DestroyRenderTarget(uint)`
- `DestroyShader(BSGraphics::ComputeShader*)`
- `DestroyShader(BSGraphics::DomainShader*)`
- `DestroyShader(BSGraphics::HullShader*)`
- `DestroyShader(BSGraphics::PixelShader*)`
- `DestroyShader(BSGraphics::VertexShader*)`
- `DoZPrePass(NiCamera*,NiCamera*,float,float,float,float)`
- `DrawDynamicLineShape(BSGraphics::DynamicLineShape*,uint,uint)`
- `DrawDynamicTriShape(BSGraphics::DynamicTriShape*,BSGraphics::DynamicTriShapeDrawData&,uint,uint,BSGraphics::IndexBuffer*)`
- `DrawInstancedTriShape(BSGraphics::TriShape*,uint,uint,uint,unsigned___int64,BSGraphics::VertexBuffer*)`
- `DrawLineShape(BSGraphics::LineShape*,uint)`
- `DrawParticleShaderTriShape(void*,uint)`
- `DrawTriShape(BSGraphics::TriShape*,uint,uint)`
- `DrawTriShape(BSGraphics::TriShape*,uint,uint,BSGraphics::IndexBuffer*)`
- `DrawTriShapeLargeIndices(BSGraphics::TriShape*,uint,uint)`
- `End(void)`
- `EndOcclusionQuery(BSGraphics::OcclusionQuery*)`
- `FinishStreamingTexture(BSGraphics::Texture*)`
- `FinishStreamingTextureUpgrade(BSGraphics::Texture*)`
- `Flush(void)`
- `FlushConstantGroup(BSGraphics::ConstantGroup*,BSGraphics::ConstantGroup*)`
- `GetCustomShaderConstantGroup(uint,float**)`
- `GetDynamicTriShapeStaticDataAccess(BSGraphics::DynamicTriShape*,bool)`
- `GetOcclusionQueryResults(BSGraphics::OcclusionQuery*,bool,bool)`
- `GetParticlesDynamicTriShape(void)`
- `GetScaleformDepthSurfaceType(uint)`
- `GetScaleformRendererParameters(BSGraphics::ScaleformRendererParameters&)`
- `GetScaleformSurfaceType(uint)`
- `GetScaleformTextureType(BSGraphics::Texture*)`
- `GetScaleformTextureTypeFromRenderTarget(uint)`
- `GetShaderConstantGroup(BSGraphics::ComputeShader*,BSGraphics::ConstantGroupLevel)`
- `GetShaderConstantGroup(BSGraphics::DomainShader*,BSGraphics::ConstantGroupLevel)`
- `GetShaderConstantGroup(BSGraphics::HullShader*,BSGraphics::ConstantGroupLevel)`
- `GetShaderConstantGroup(BSGraphics::PixelShader*,BSGraphics::ConstantGroupLevel)`
- `GetShaderConstantGroup(BSGraphics::VertexShader*,BSGraphics::ConstantGroupLevel)`
- `GetTextureDataSize(BSGraphics::Texture*)`
- `GetTextureDimensions(BSGraphics::Texture*,uint&,uint&)`
- `GetTextureDimensions(BSGraphics::Texture*,uint&,uint&,uint&)`
- `GetTextureDimensions(BSGraphics::Texture*,uint&,uint&,uint&,uint&)`
- `GetTextureFormat(BSGraphics::Texture*)`
- `GetTextureInfoFromFile(char*,BSGraphics::TextureFileFormat,BSGraphics::TextureInfo&)`
- `GetTextureMaxDegradeLevel(BSGraphics::Texture*)`
- `GetTriShapeDataAccess(BSGraphics::IndexBuffer*,bool)`
- `GetTriShapeDataAccess(BSGraphics::TriShape*,bool)`
- `GetTriShapeDataIsReady(BSGraphics::TriShapeDataAccess*)`
- `IncRef(BSGraphics::ByteBuffer*)`
- `IncRef(BSGraphics::IndexBuffer*)`
- `IncRef(BSGraphics::StructuredBuffer*)`
- `IncRef(BSGraphics::Texture*)`
- `IncRef(BSGraphics::TriShape*)`
- `IncRef(BSGraphics::VertexBuffer*)`
- `Init(BSGraphics::RendererInitOSData&,BSGraphics::ApplicationWindowProperties&,BSGraphics::RendererInitReturn&)`
- `InvalidateBuffer(BSGraphics::Buffer&)`
- `IsBufferWritePending(BSGraphics::StructuredBuffer&)`
- `IsFormatSRGB(uint)`
- `IsTextureCubeMap(NiTexture*)`
- `KillWindow(uint)`
- `LoadUpgradeTextureDataFromStream(BSGraphics::Texture*,BSResourceNiBinaryStream*)`
- `LoadZPrePassShader(BSIStream*)`
- `Lock(void)`
- `LockForDebugGeometry(void)`
- `MapDynamicLineShapeDynamicData(BSGraphics::DynamicLineShape*,uint)`
- `MapDynamicTriShapeDynamicData(BSDynamicTriShape*,BSGraphics::DynamicTriShape*,BSGraphics::DynamicTriShapeDrawData&,uint)`
- `MapTexture(BSGraphics::Texture*,BSGraphics::Map,BSGraphics::TextureAccess&)`
- `MarkStructuredBufferForReadBack(BSGraphics::StructuredBuffer*)`
- `PSSetStructuredBufferSR(uint,BSGraphics::StructuredBuffer*)`
- `PopDebugMarker(void)`
- `ProcessCommandBuffer(char**)`
- `PushDebugMarker(char*)`
- `QPosAdjust(void)`
- `RasterStateSetCullMode(BSGraphics::RasterStateCullMode)`
- `RasterStateSetScissorRect(float,float,float,float)`
- `RasterStateSetScissorRect(uint,uint,uint,uint)`
- `ReadBackStructuredBuffer(BSGraphics::StructuredBuffer*,void*,uint,uint)`
- `ReadStreamingTextureDataToArraySlice(BSGraphics::Texture*,BSTextureStreamer::Stream&,uint)`
- `ReadTextureDataToAraySlice(BSResourceNiBinaryStream*,BSGraphics::Texture*,uint,BSGraphics::TextureHeader*)`
- `ReleaseTriShapeDataAccess(BSGraphics::TriShapeDataAccess*)`
- `RemoveTextureArraySlice(BSGraphics::Texture*,uint)`
- `Renderer(void)`
- `ResetState(void)`
- `ResetWindow(uint)`
- `ResetZPrePass(void)`
- `ResizeWindow(uint,uint,uint,bool,bool)`
- `RestorePreviousClearColor(void)`
- `RunComputeShader(BSGraphics::ComputeShader*,uint,uint,uint)`
- `SaveTextureToFile(BSGraphics::Texture*,char*,BSGraphics::TextureFileFormat,bool,bool)`
- `SetCSTextureMode(uint,BSGraphics::TextureAddressMode,BSGraphics::TextureFilterMode)`
- `SetCSUnorderedAccessTarget(uint,int)`
- `SetClearColor(float,float,float,float)`
- `SetClearStencil(uint)`
- `SetComputeConstants(uchar*,uint)`
- `SetContext(BSGraphics::Context*)`
- `SetDSTextureMode(uint,BSGraphics::TextureAddressMode,BSGraphics::TextureFilterMode)`
- `SetInstanceTransformConstants(uchar*,uint)`
- `SetPerFrameConstants(void)`
- `SetRenderTarget(BSGraphics::RendererShadowState&,uint,int,BSGraphics::SetRenderTargetMode)`
- `SetSRGB(BSGraphics::TextureHeader&,bool)`
- `SetShaders(BSGraphics::VertexShader*,BSGraphics::HullShader*,BSGraphics::DomainShader*,BSGraphics::PixelShader*)`
- `SetTexture(uint,BSGraphics::Texture*)`
- `SetTexture(uint,BSGraphics::Texture*,BSGraphics::TextureAddressMode)`
- `SetTexture(uint,BSGraphics::Texture*,uint)`
- `SetTextureDepth(uint,int)`
- `SetTextureDepth(uint,int,BSGraphics::TextureAddressMode)`
- `SetTextureLoadLevel(uint)`
- `SetTextureMode(uint,BSGraphics::TextureAddressMode,BSGraphics::TextureFilterMode)`
- `SetTextureRenderTarget(uint,int,BSGraphics::TextureAddressMode,bool)`
- `SetTextureRenderTarget(uint,int,bool)`
- `SetTextureStencil(uint,int)`
- `SetTextureStencil(uint,int,BSGraphics::TextureAddressMode)`
- `SetVSTextureDepthStencil(uint,int)`
- `SetVSTextureDepthStencil(uint,int,BSGraphics::TextureAddressMode)`
- `SetVSTextureRenderTarget(uint,int,BSGraphics::TextureAddressMode,bool)`
- `SetVSTextureRenderTarget(uint,int,bool)`
- `SetVertexBuffer(BSGraphics::TriShape*,BSGraphics::VertexBuffer*)`
- `SetWindowActiveState(bool)`
- `Shutdown(void)`
- `StreamingTextureUpgrade(BSGraphics::Texture*,uint,bool,BSGraphics::Texture*&,BSTextureStreamer::ChunkDesc*,BSTextureStreamer::ChunkReadDesc&)`
- `TextureDegrade(BSGraphics::Texture*,uint,bool)`
- `TextureUpgrade(BSGraphics::Texture*,uint,bool,BSGraphics::Texture*&)`
- `TryLock(void)`
- `TryUnlockForDebugGeometry(void)`
- `Unlock(void)`
- `UnmapDynamicLineShapeDynamicData(BSGraphics::DynamicLineShape*)`
- `UnmapDynamicTriShapeDynamicData(BSGraphics::DynamicTriShape*,BSGraphics::DynamicTriShapeDrawData&)`
- `UnmapTexture(BSGraphics::Texture*)`
- `UpdatePendingBufferStatus(void)`
- `UpdateStructuredBuffer(BSGraphics::StructuredBuffer*,void*,uint,uint)`
- `UpdateViewPort(BSGraphics::RendererShadowState&,uint,uint,bool)`
- `VSSetBufferSR(uint,BSGraphics::ByteBuffer*)`
- `VSSetBufferSR(uint,BSGraphics::IndexBuffer*)`
- `VSSetBufferSR(uint,BSGraphics::VertexBuffer*)`
- `VSSetStructuredBufferSR(uint,BSGraphics::StructuredBuffer*)`
- `WindowSizeChanged(uint)`
- `WriteToStructuredBuffer(BSGraphics::StructuredBuffer*,void*,uint,uint)`

### C.2 `BSGraphics::RenderTargetManager` (60 signatures)

- `AcquireCubemap(int)`
- `AcquireDepthStencil(int)`
- `AcquireRenderTarget(int)`
- `AcquireTarget<BSGraphics::CubeMapRenderTargetProperties>(int,BSGraphics::RenderTargetManager::Persistency,BSGraphics::CubeMapRenderTargetProperties&,uint&,BSGraphics::RenderTargetManager::TargetHandle*&,BSGraphics::RenderTargetManager::TTargetHandles<BSGraphics::CubeMapRenderTargetProperties>*)`
- `AcquireTarget<BSGraphics::DepthStencilTargetProperties>(int,BSGraphics::RenderTargetManager::Persistency,BSGraphics::DepthStencilTargetProperties&,uint&,BSGraphics::RenderTargetManager::TargetHandle*&,BSGraphics::RenderTargetManager::TTargetHandles<BSGraphics::DepthStencilTargetProperties>*)`
- `AcquireTarget<BSGraphics::RenderTargetProperties>(int,BSGraphics::RenderTargetManager::Persistency,BSGraphics::RenderTargetProperties&,uint&,BSGraphics::RenderTargetManager::TargetHandle*&,BSGraphics::RenderTargetManager::TTargetHandles<BSGraphics::RenderTargetProperties>*)`
- `CopyRenderTargetToClipboard(int)`
- `CopyRenderTargetToTexture(int,BSGraphics::Texture*,bool,bool)`
- `CreateCubeMapRenderTarget(int,BSGraphics::CubeMapRenderTargetProperties&,BSGraphics::RenderTargetManager::TARGET_PERSISTENCY)`
- `CreateDepthStencilTarget(int,BSGraphics::DepthStencilTargetProperties&,BSGraphics::RenderTargetManager::TARGET_PERSISTENCY)`
- `CreateRenderTarget(int,BSGraphics::RenderTargetProperties&,BSGraphics::RenderTargetManager::TARGET_PERSISTENCY)`
- `DecompressDepthStencilTarget(int)`
- `DestroyRenderTargets(void)`
- `GenerateMipsForRenderTarget(int)`
- `GetPlatformTargetFromDepthStencilTarget(int)`
- `GetPlatformTargetFromRenderTarget(int)`
- `InitializeTargets(void)`
- `QCurrentCubeMapRenderTargetHeight(void)`
- `QCurrentCubeMapRenderTargetWidth(void)`
- `QCurrentPlatformDepthStencilTarget(void)`
- `QCurrentPlatformRenderTarget(void)`
- `QCurrentRenderTarget(void)`
- `QCurrentRenderTargetHeight(void)`
- `QCurrentRenderTargetWidth(void)`
- `QIsAcquiredDepthStencil(int)`
- `QIsAcquiredRenderTarget(int)`
- `QShadowMapArrayDepthStencil(void)`
- `RecreateRenderTargets(void)`
- `ReleaseCubemap(int)`
- `ReleaseDepthStencil(int)`
- `ReleaseRenderTarget(int)`
- `RenderTargetManager(void)`
- `ResummarizeHTileDepthStencilTarget(int)`
- `SaveRenderTargetToFile(int,char*,BSGraphics::TextureFileFormat,bool)`
- `SaveRenderTargetToTexture(int,bool,bool,BSGraphics::Usage)`
- `SetCSTextureDepthStencil(uint,int)`
- `SetCSTextureRenderTarget(uint,int,bool)`
- `SetCSUnorderedAccessTarget(uint,int)`
- `SetCurrentCubeMapRenderTarget(int,BSGraphics::SetRenderTargetMode,int)`
- `SetCurrentDepthStencilTarget(int,BSGraphics::SetRenderTargetMode,int,bool)`
- `SetCurrentRenderTarget(int,int,BSGraphics::SetRenderTargetMode)`
- `SetCurrentViewportCustomDimensions(uint,uint)`
- `SetCurrentViewportDefault(void)`
- `SetCurrentViewportForceToRenderTargetDimensions(void)`
- `SetEnableDynamicResolution(bool)`
- `SetFrameBufferProperties(BSGraphics::RenderTargetProperties&)`
- `SetRenderTargetPersistency(int,bool)`
- `SetTextureCubeMap(uint,int)`
- `SetTextureDepth(uint,int)`
- `SetTextureDepth(uint,int,BSGraphics::TextureAddressMode)`
- `SetTextureRenderTarget(uint,int,BSGraphics::TextureAddressMode,bool)`
- `SetTextureRenderTarget(uint,int,bool)`
- `SetTextureStencil(uint,int)`
- `SetTextureStencil(uint,int,BSGraphics::TextureAddressMode)`
- `SetUseDynamicResolutionViewportAsDefaultViewport(bool)`
- `SetVSTextureDepthStencil(uint,int,BSGraphics::TextureAddressMode)`
- `SetVSTextureRenderTarget(uint,int,BSGraphics::TextureAddressMode,bool)`
- `SyncDepthTarget(int)`
- `SyncRenderTarget(int,uint)`
- `UpdateDynamicResolution(NiPoint3,NiPoint3,NiPoint3,NiPoint3)`

### C.3 `BSBatchRenderer` (47 signatures)

- `AddToPassMap(uint&,int,uint)`
- `AllocateCommandBufferPassesData(uint)`
- `BSBatchRenderer(BATCHRENDERER_CREATION_MODE)`
- `BeginPass(uint,BSShader*)`
- `ClearAlphaLists(void)`
- `ClearPassGroups(void)`
- `ClearRenderPasses(int,bool,bool)`
- `Draw(BSRenderPass*)`
- `EndPass(void)`
- `GeometryGroup::ClearAndFreePasses(int)`
- `GeometryGroup::ClearPasses(int)`
- `GeometryGroup::RegisterPass(BSRenderPass*,bool,bool)`
- `GeometryGroup::Render(bool)`
- `GeometryGroup::~GeometryGroup(void)`
- `GetCommandBufferPasses(uint,uint,BSBatchRenderer::PassGroup*)`
- `GetNextAlphaDepth(NiPoint3&,float)`
- `InitSDM(void)`
- `KillSDM(void)`
- `PassGroup::Clear(bool,bool)`
- `QPassesWithinRange(int,uint,uint)`
- `RegisterAlphaPass(BSRenderPass*)`
- `RegisterGeometryGroupPass(BSRenderPass*,BSBatchRenderer::GEOMETRY_GROUP_ENUM,BSBatchRenderer::GeometryGroup*,bool)`
- `RegisterGeometryGroupPassImpl(BSRenderPass*,BSBatchRenderer::GEOMETRY_GROUP_ENUM,BSBatchRenderer::GeometryGroup*,bool)`
- `RegisterPass(BSRenderPass*)`
- `RegisterPassGeometryGroup(BSRenderPass*,BSBatchRenderer::GEOMETRY_GROUP_ENUM,bool)`
- `RegisterPassImpl(BSRenderPass*)`
- `RenderBatches(int,bool,uint)`
- `RenderBatchesInOrder(int,bool)`
- `RenderCommandBufferPassesImpl(int,BSBatchRenderer::CommandBufferPassesData*,uint,bool)`
- `RenderNextAlpha(NiPoint3&,float,float)`
- `RenderPassImmediately(BSRenderPass*,uint,bool)`
- `RenderPassImmediatelySameTechnique(BSRenderPass*,bool)`
- `RenderPassImmediately_Custom(BSRenderPass*)`
- `RenderPassImmediately_Skinned(BSRenderPass*,bool)`
- `RenderPassImmediately_Standard(BSRenderPass*,bool)`
- `RenderPassImpl(BSRenderPass*,uint,bool)`
- `RenderPassInstanceGroup(BSRenderPass*,uint)`
- `RenderPersistentPassListImpl(PersistentPassList&,bool)`
- `ResetGroupingAlphaGroups(void)`
- `ShaderSetup(BSRenderPass*,BSShader*)`
- `ShaderSetup(BSRenderPass*,BSShader*,bool)`
- `SortAlphaPasses(NiCamera*,bool)`
- `SortGroupingAlphaGroups(void)`
- `StartGroupingAlphas(NiBound&,bool,NiCamera*,bool)`
- `StopGroupingAlphas(void)`
- `UpdateCommandBufferPasses(uint,uint,char*,BSBatchRenderer::PassGroup*)`
- `~BSBatchRenderer(void)`

### C.4 `BSShaderAccumulator` (53 signatures)

- `BSShaderAccumulator(BATCHRENDERER_CREATION_MODE)`
- `ClearActivePasses(bool)`
- `ClearBloodSplatter(void)`
- `ClearDebugStats(void)`
- `ClearEffectPasses(void)`
- `ClearGroupPasses(int,bool)`
- `ClearRenderPasses(void)`
- `ClearSunQueries(void)`
- `DoSunOcclusionQueury(BSGeometry*,uint,NiCamera*)`
- `EvaluateSunOcclusionResults(uint)`
- `FinishAccumulating(void)`
- `FinishAccumulatingPostResolveDepth(void)`
- `FinishAccumulatingPreResolveDepth(void)`
- `FinishAccumulating_LODOnly(BSShaderAccumulator*)`
- `FinishAccumulating_OcclusionMap(BSShaderAccumulator*)`
- `FinishAccumulating_ShadowMapOrMask(BSShaderAccumulator*)`
- `FinishAccumulating_Standard(BSShaderAccumulator*)`
- `FinishAccumulating_Standard_PostResolveDepth(BSShaderAccumulator*)`
- `FinishAccumulating_Standard_PreResolveDepth(BSShaderAccumulator*)`
- `FinishAccumulating_VatsMask(BSShaderAccumulator*)`
- `FinishAccumulating_VatsMaskDebug(BSShaderAccumulator*)`
- `FinishAccumulating_VatsQuery(BSShaderAccumulator*)`
- `GetPassesInGroup(int)`
- `GetRefractivePassCount(void)`
- `IsGroupingAlphas(void)`
- `QPassesWithinRange(int,uint,uint)`
- `RegisterObject(BSGeometry*)`
- `RegisterObject_Interface(BSShaderAccumulator*,BSGeometry*,BSShaderProperty*)`
- `RegisterObject_LODOnly(BSShaderAccumulator*,BSGeometry*,BSShaderProperty*)`
- `RegisterObject_LocalMap(BSShaderAccumulator*,BSGeometry*,BSShaderProperty*)`
- `RegisterObject_OcclusionMap(BSShaderAccumulator*,BSGeometry*,BSShaderProperty*)`
- `RegisterObject_ScreenSplatter(BSShaderAccumulator*,BSGeometry*,BSShaderProperty*)`
- `RegisterObject_ShadowMapOrMask(BSShaderAccumulator*,BSGeometry*,BSShaderProperty*)`
- `RegisterObject_Standard(BSShaderAccumulator*,BSGeometry*,BSShaderProperty*)`
- `RegisterObject_VatsMask(BSShaderAccumulator*,BSGeometry*,BSShaderProperty*)`
- `RegisterObject_VatsQuery(BSShaderAccumulator*,BSGeometry*,BSShaderProperty*)`
- `RenderAllBatches(void)`
- `RenderAlphaGeometry(void)`
- `RenderBatches(int,bool,int)`
- `RenderBatchesInOrder(int,bool)`
- `RenderBlendedDecals(void)`
- `RenderGeometryGroup(uint,bool)`
- `RenderNormals(NiCamera*,float,float,float,float)`
- `RenderOpaqueDecals(void)`
- `RenderStencilAboveWater(void)`
- `ResetSunOcclusion(void)`
- `SetDepthPassIndex(uint)`
- `SetRenderMode(BSShaderManager::etRenderMode)`
- `SetShadowLight(BSLight*)`
- `SetShadowSceneNode(ShadowSceneNode*)`
- `SortAlphaPasses(void)`
- `StartGroupingAlphas(NiBound&,bool)`
- `~BSShaderAccumulator(void)`

### C.5 `ImageSpaceManager` (42 signatures)

- `AddEffect(ImageSpaceManager::ImageSpaceEffectEnum,ImageSpaceEffect*)`
- `ApplyModifier(ImageSpaceBaseData&,ImageSpaceBaseData&,ImageSpaceModData&,float)`
- `Copy(int,int)`
- `CreatePartialScreenGeometry(float,float)`
- `CreatePartialScreenGeometry(uint,uint)`
- `FinishApplyModifiers(void)`
- `Gamma(int,int,bool)`
- `GetConsoleBaseData(void)`
- `GetDepthOfFieldParamsFromPoint(NiPoint3&,NiPoint2&)`
- `GetDepthOfFieldParamsFromWorldBound(NiBound&,NiPoint2&)`
- `GetOrCreateLUTDataTexture(NiPointer<NiTexture>&,BSFixedString&)`
- `GetRadialBlurCenterFromWorldBound(NiBound&,NiPoint2&)`
- `ImageSpaceManager(void)`
- `InitEffects(void)`
- `InitGeometry(void)`
- `Initialize(void)`
- `PrepareEffectRange(int,int)`
- `PrepareEndOfFrameEffects(void)`
- `PushEOFData(void)`
- `ReloadShaders(void)`
- `RenderEffect(ImageSpaceEffect*,NiTexture*,int,ImageSpaceEffectParam*)`
- `RenderEffect(ImageSpaceEffect*,int,int,ImageSpaceEffectParam*)`
- `RenderEffect(ImageSpaceEffect*,int,int,int,ImageSpaceEffectParam*)`
- `RenderEffect(ImageSpaceManager::ImageSpaceEffectEnum,NiTexture*,NiTexture*,int,ImageSpaceEffectParam*)`
- `RenderEffect(ImageSpaceManager::ImageSpaceEffectEnum,NiTexture*,int,ImageSpaceEffectParam*)`
- `RenderEffect(ImageSpaceManager::ImageSpaceEffectEnum,int,ImageSpaceEffectParam*)`
- `RenderEffect(ImageSpaceManager::ImageSpaceEffectEnum,int,int,ImageSpaceEffectParam*)`
- `RenderEffectRange(int,int,int,int)`
- `RenderEndOfFrameEffects(int,int)`
- `ResetToBaseData(void)`
- `RestoreEOFData(void)`
- `SetBaseData(ImageSpaceBaseData*)`
- `SetLUTData(ImageSpaceLUTData*)`
- `SetOverrideBaseData(ImageSpaceBaseData*)`
- `SetOverrideLUTData(ImageSpaceLUTData*)`
- `SetUnderwaterBaseData(ImageSpaceBaseData*)`
- `SetupEffects(void)`
- `Shutdown(void)`
- `ShutdownEffects(void)`
- `ShutdownGeometry(void)`
- `UpdateDynamicGeometry(void)`
- `~ImageSpaceManager(void)`

### C.6 `BSGraphics::State` (15 signatures)

- `BuildCameraStateData(BSGraphics::CameraStateData&,NiCamera*,bool)`
- `CacheCameraData(NiCamera*,bool)`
- `CalculateCameraViewProj(DirectX::XMMATRIX&,DirectX::XMMATRIX&,DirectX::XMMATRIX&,NiPoint3&,NiPoint3&,NiPoint3&,NiFrustum&)`
- `CreateDefaultTextures(void)`
- `DestroyDefaultTextures(void)`
- `FindCameraStateData(NiCamera*,bool)`
- `Halton(float,float)`
- `SetCameraData(NiCamera*,bool,float,float)`
- `SetCameraViewPort(NiRect<float>&,float,float)`
- `SetScreenSpaceCameraData(NiRect<float>*)`
- `State(void)`
- `UpdateAllPreviousFrameCameraData(void)`
- `UpdatePreviousFrameCameraData(BSGraphics::CameraStateData&)`
- `UpdateTemporalData(void)`
- `~State(void)`

### C.7 `BSGraphics::Context` (6 signatures)

- `Context(void)`
- `GetConstantBuffer(BSGraphics::ComputeShader*,BSGraphics::ConstantGroupLevel)`
- `GetConstantBuffer(BSGraphics::DomainShader*,BSGraphics::ConstantGroupLevel)`
- `GetConstantBuffer(BSGraphics::HullShader*,BSGraphics::ConstantGroupLevel)`
- `GetConstantBuffer(BSGraphics::PixelShader*,BSGraphics::ConstantGroupLevel)`
- `GetConstantBuffer(BSGraphics::VertexShader*,BSGraphics::ConstantGroupLevel)`

### C.8 `BSGraphics::RendererShadowState` (3 signatures)

- `Invalidate(void)`
- `QSameStates(BSGraphics::RendererShadowState&)`
- `RendererShadowState(void)`
