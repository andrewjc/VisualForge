# Fallout 4 Engine Hook Surface — Deep Analysis

A reverse-engineering survey of `Fallout4.exe` 1.11.221 (55 MB, next-gen build) aimed at
finding concrete integration points for VisualForge. Everything here was extracted from the
shipped binary — PE headers, import/export tables, and MSVC RTTI class descriptors — not
guessed. Confidence is marked **[verified]** (read directly from the binary) or
**[inferred]** (a technique that follows from what's verified but needs implementation to
confirm).

---

## 1. Segment map

| Section | Virtual size | Flags | What's in it | Hook relevance |
|---|--:|---|---|---|
| `.text` | 37.9 MB | R-X | All engine code | Function/detour targets; the render, movie, and physics call sites live here |
| `.interpr` | 42 KB | R-X | Small secondary code blob (Bethesda "interpreter" stub) | Low |
| `.rdata` | 11.1 MB | R-- | Vtables, RTTI, string literals, GFSDK/const tables | vtable hooks resolve here; hardcoded clamps and default strings live here (writable via VirtualProtect) |
| `.data` | 15.9 MB (2 MB raw + BSS) | RW- | Globals, singletons, **cached setting flags** | Direct memory patches; where settings get cached after INI load |
| `.pdata` | 2.5 MB | R-- | x64 exception unwind tables | **Function boundary enumeration** — every function's start/end is listed here, useful for a signature scanner |
| `.rsrc`/`.reloc` | — | R-- | Resources, relocations | Relocs already used by our name-pointer scan |
| `.bind` | 234 KB | R-X | SteamStub DRM wrapper | Avoid — self-modifying at launch |

The binder VisualForge already ships walks `.rdata`/`.data` for `Setting` objects. The three
segments that open new capabilities are **`.text`** (call sites to detour), the **middleware
import tables**, and the **`.data` cached flags**.

---

## 2. Hook classes, ranked by value-to-effort

### A. IAT hooks on middleware DLLs — highest value, lowest risk

The engine offloads several subsystems to separate NVIDIA/RAD DLLs and calls them through
the Import Address Table. Redirecting an IAT slot (or hooking the export) intercepts every
call with no signature scanning and survives game patches. **[verified]** imports:

**Bink video — `bink2w64.dll`** → `BinkOpen`, `BinkDoFrame`, `BinkNextFrame`, `BinkWait`,
`BinkPause`, `BinkShouldSkip`, `BinkClose`, `BinkCopyToBufferRect`.
- Correction from a first look: `BinkShouldSkip` is a **frame-pacing helper** (skip *displaying*
  a frame to keep A/V sync), **not** a whole-video skip — hooking it does not skip the intro,
  so it is not an improvement over the current `sIntroSequence` blank. Leave intro-skip as-is.
- `BinkOpen`/`BinkNextFrame` remain a real hook point for **per-video policy** (e.g. force a
  one-frame length on a specific clip) or for re-encoding/upscaling menu video later — but the
  setting-blank already covers the intro cleanly, so this is low priority.

**NVIDIA Flex physics — `flexRelease_x64.dll` / `flexExtRelease_x64.dll`** → `flexInit`,
`flexCreateSolver`, `flexUpdateSolver`, `flexCreateTriangleMesh`, `flexSetParams`, plus the
Ext container APIs.
- The weapon-debris crash on modern (RTX) GPUs — the reason your prefs carry `bNVFlexEnable=0`
  — occurs inside `flexCreateSolver`/`flexInit`. Wrapping those (validate/guard the descriptor,
  catch the failure) is what lets weapon debris be **re-enabled** safely. **[inferred]** exact
  crash site; **[verified]** these are the functions in play.

**NVIDIA Godrays — `GFSDK_GodraysLib.x64.dll`** → `GFSDK_GodraysLib_OpenDX`,
`_BeginAccumulation`, `_RenderVolume`, `_EndAccumulation`, `_ApplyLighting`, `_SetDebugMode`,
with `_MediumDesc`, `_LightDesc`, `_PostProcessDesc`, `_ViewerDesc` parameter structs and a
`_UpsampleQuality` enum. **[verified]**
- Hooking `_RenderVolume`/`_BeginAccumulation` lets us rewrite the `MediumDesc` (scattering,
  fog, phase, intensity) and buffer size **beyond the INI ceiling** — the same knobs the `gr`
  console command exposes, but forced every frame at values the launcher never allows.

**NVIDIA HBAO+ — `GFSDK_SSAO_D3D11.win64.dll`** → `GFSDK_SSAO_CreateContext_D3D11`
(returns an interface whose `RenderAO` takes an `AO_Parameters` struct). **[verified]**
- Intercept the context's parameters to push radius/power/detail past `[NVHBAO]` limits and
  raise the AO resolution.

**D3D/DXGI — `d3d11.dll!D3D11CreateDeviceAndSwapChain`, `dxgi.dll`** **[verified]**
- We already vtable-hook the swapchain. Hooking device/swapchain **creation** additionally
  allows requesting a **flip-model + `R10G10B10A2`/`FP16` swapchain and an HDR10 colorspace**
  — the foundation for true HDR output (the RenoDX approach), paired with a tonemapper
  replacement (§C). **[inferred]**

### B. Console-command "apply-live" bridge — medium effort, unlocks live tuning

The engine registers these console commands **[verified]**:

| Command | Effect |
|---|---|
| `UpdateGodraySettings` | Re-applies godray config to the live GFSDK context |
| `UpdateHBAOSettings` | Re-applies HBAO+ config to the live SSAO context |
| `UpdateFlexSettings` | Re-applies Flex config |
| `SetINISetting` / `GetINISetting` | Read/write any engine setting by `name:section` |
| `SetFog` (start,end), `SetClipDist` | Scene depth control |
| `SetForceWetness`, `SetWetnessLevel`, `SetRainLevel`, `SetImageSpaceGlow` | Live weather/scene |
| `ToggleMTRDeferredLights` (`tMTRDFL`), `SetLightingPasses`, `ToggleVBlankOptim` | Render/perf toggles |

The `Update*Settings` trio is the key: today VisualForge's godray/HBAO edits need an area
reload because those subsystems latch their config. If the overlay can **invoke
`UpdateGodraySettings`/`UpdateHBAOSettings`** after an edit, those changes apply **instantly**.
Implementation: locate the console command executor (F4SE's `ConsoleManager`/script-compiler
path, or call the command's registered `Execute` function found via the command table). **[inferred]**
This turns the overlay from "edit + reload" into a true real-time grading tool.

### C. Vtable hooks on render classes — highest ceiling, most effort

MSVC RTTI exposes **3,325 classes**, including **161 `BSImagespaceShader*`** post-process
passes — the entire pipeline is individually addressable by vtable. **[verified]** The
load-bearing ones:

| Class | Enables |
|---|---|
| `BSImagespaceShaderTemporalAA`, `ImageSpaceEffectTemporalAA` | Replace the blurry 2015 TAA with **DLAA**, or reweight it; the engine already produces the motion vectors + jitter DLAA needs |
| `ImageSpaceEffectUpsampleDynamicResolution` | Injection point for **DLSS / FSR upscaling** (the game already has a dynamic-resolution upsample stage to slot into) |
| `BSImagespaceShaderHDRTonemapBlendCinematic` | Replace the tonemapper — filmic/AGX, or **HDR10** output paired with §A |
| `BSImagespaceShaderGammaCorrectLUT` | Custom **LUT color grading** |
| `BSDFCompositeShader` | The deferred lighting resolve — **PBR lighting** improvements |
| `BSLightingShaderMaterial{Parallax,ParallaxOcc,MultiLayerParallax,Snow,SkinTint,Eye,Envmap,...}` | Per-material **PBR upgrades**, real parallax-occlusion tuning |
| `BSWaterShader`, `BSSkyShader`, `BSParticleShader{Rain,Snow}Emitter` | Water, sky, and weather-particle upgrades |
| `BSImagespaceShaderVLSComposite` | Volumetric-lighting composite — deeper godray integration |
| `BSImagespaceShaderBokehDepthOfField*` (4 passes) | DOF quality/artistic control |

The `ShaderEngine` community pattern (replace compiled HLSL by signature) targets exactly
these — but with the vtables enumerated, VisualForge can hook a specific pass's `Draw`/`Setup`
and swap constants or the shader itself, surgically.

### D. Memory patches on globals/consts — targeted, for hard ceilings

- **`.data` cached flags.** Some settings are copied into plain globals after INI load (this is
  why `bVolumetricLightingEnable` had no live `Setting` object until the binder fix). Those
  globals can be located and patched to force-enable a feature the INI can't. **[inferred]**
- **`.rdata` const clamps.** Godray grid-size max, SSR ray-step counts, cascade caps, and
  similar ceilings are immediate constants; VirtualProtect + rewrite raises them. **[inferred]**
- **`.rdata` default strings.** Already used for the intro (the `sIntroSequence` default *is*
  `GameIntro_V3_B.bk2`). Same lever fits other hardcoded defaults. **[verified]**

---

## 3. Mapping to VisualForge modules

| Capability | New module | Reuses |
|---|---|---|
| IAT hooking helper (find import, swap slot) | `ImportHook.{h,cpp}` | `PatternScan` for the IAT walk |
| Bink intro skip (proper) | fold into `IntroSkip` | `ImportHook` |
| Flex debris crash-fix + re-enable | `FlexGuard.{h,cpp}` | `ImportHook`, MinHook |
| Godray/HBAO over-cap + live apply | `MiddlewareTuner.{h,cpp}` | `ImportHook`, overlay, console bridge |
| Console "apply-live" bridge | `Console.{h,cpp}` | signature scan for the executor |
| DLAA / DLSS-FSR | `Upscaler.{h,cpp}` | swapchain hook, `BSImagespaceShaderTemporalAA`/`UpsampleDynamicResolution` vtables |
| HDR10 output | `Hdr.{h,cpp}` | device/swapchain creation hook + tonemapper vtable |
| Tonemap / LUT grading | `Grading.{h,cpp}` | ImageSpace vtable hooks |

All of these compose with the existing address-independent design: IAT and vtable hooks need
no hardcoded offsets, and RTTI gives us the vtables by name.

---

## 4. Build status & remaining order

**Done (shipping in VisualForge):**
- **Scene depth capture** — `DepthCapture.cpp`. Hooks `OMSetRenderTargets` (33),
  `OMSetRenderTargetsAndUnorderedAccessViews` (34) and `ClearDepthStencilView` (53) on the
  immediate context. The essential detail: copying depth at Present yields a **cleared**
  buffer; the depth must be snapshotted **immediately before the engine clears it**, when it
  still holds the rendered scene. On 1.11.221 the scene depth is `R24G8_TYPELESS` at
  backbuffer resolution, standard-Z (cleared = 1.0), with exactly **one depth clear per
  frame**. A CPU-readback self-test logs min/max/spread to prove the contents are real.
  This is the prerequisite for screen-space GI/AO.
- **Weapon-debris crash fix** — `WeaponDebris.cpp` guards `ID3D11Device::CreateShaderResourceView`
  (the real crash site) so `bNVFlexEnable` can be turned on safely. Exposed in the Systems tab.
- **Color grading + sharpening** — `PostProcess.cpp` adds a real grading pass (exposure,
  contrast, saturation, vibrance, white balance, filmic tonemap) alongside CAS, in the Color &
  Sharpen tab. Covers the "grading" half of the HDR/grading item without any engine hook.

**Remaining, with the concrete prerequisite each needs:**
1. **Live godray/HBAO tuning** — needs the console-command executor located by signature (to
   call `UpdateGodraySettings`/`UpdateHBAOSettings`) *or* the GFSDK structs matched exactly.
   Both want in-game verification.
2. **DLAA, then DLSS/FSR** — the biggest image-quality jump but needs external SDK binaries
   (NVIDIA Streamline/`nvngx_dlss.dll`; FSR is open but needs the engine's depth+motion-vector
   resources identified). Multi-session, not fakeable.
3. **HDR10 output** — the swapchain-creation hook (foundation) plus a tonemapper replacement,
   and an HDR display to verify. The grading pass above is the SDR-side down payment.

---

## 5. Reference — key extracted data

- Imported middleware: `bink2w64`, `flexRelease_x64`, `flexExtRelease_x64`,
  `GFSDK_GodraysLib.x64`, `GFSDK_SSAO_D3D11.win64`, `nvToolsExt64_1`, `d3d11`, `dxgi`,
  `X3DAudio1_7`, `XINPUT1_3`, `steam_api64`.
- Godray API: `OpenDX`, `BeginAccumulation`, `RenderVolume`, `EndAccumulation`,
  `ApplyLighting`, `SetDebugMode` (+ `MediumDesc`/`LightDesc`/`PostProcessDesc`/`ViewerDesc`).
- SSAO API: `GFSDK_SSAO_CreateContext_D3D11` → context with `RenderAO`.
- Flex API: `flexInit`, `flexCreateSolver`, `flexUpdateSolver`, `flexCreateTriangleMesh`,
  `flexSetParams`, `flexSetShapes`, Ext container APIs.
- Render RTTI: 3,325 classes total, 161 `BSImagespaceShader*` passes; deferred resolve
  `BSDFCompositeShader`; volumetric composite `BSImagespaceShaderVLSComposite`.
- Live-apply console commands: `UpdateGodraySettings`, `UpdateHBAOSettings`, `UpdateFlexSettings`.

*Generated from static analysis; `.text` call-site offsets are intentionally not recorded
here — VisualForge resolves them at runtime by signature to stay version-independent.*
