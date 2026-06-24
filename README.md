# VisualForge

A single F4SE plugin for Fallout 4 (runtime **1.11.221**, F4SE **0.7.8**) that gives you
a live, in-game control panel over the engine's rendering settings, plus a post-process
sharpening pass — so visual tuning happens in real time instead of by editing INIs and
relaunching. Built to be extended toward deeper engine modernization (see Roadmap).

Toggle the overlay with **F10**.

The overlay opens in **Basic** mode — a short list of one-click "wrap-up" controls (a Visual
Quality preset, a Look/color preset, and simple Brightness/Vividness/Sharpness sliders) that
each configure many low-level settings at once. A **Basic / Advanced** switch at the top flips
to the full tabbed UI below for per-setting control. The choice is remembered
(`[Overlay] bBasicMode`).

## What it does today

- **Live engine-setting binder.** On the first rendered frame it scans the running
  executable and binds every INI-backed engine `Setting` object by name — **2132 of 2282**
  catalog entries on 1.11.221 — with *no hardcoded addresses*. Editing a value in the
  overlay writes directly into the engine's live memory.
- **Quick Tweaks tab.** Curated sliders/toggles for shadows, TAA & sharpness, godrays,
  ambient occlusion, grass/LOD/terrain, and image-space effects, each with sane ranges
  and tooltips.
- **Browser tab.** Filterable list of all 2282 known settings; edit any resolved one.
- **Color & Sharpen tab.** A real-time grading pass — exposure, contrast, saturation, vibrance,
  white balance (temperature/tint), an optional ACES-approx filmic tonemap, and a **cinematic
  3D LUT loader** (`.cube` files) — plus AMD FidelityFX Contrast Adaptive Sharpening, all applied
  to the final frame in a single pass. Drop `.cube` files in `Data\F4SE\Plugins\LUTs\`; four
  examples ship (Neutral_Identity, Warm_Cinematic, Cool_Wasteland, Filmic_Contrast).
- **Systems tab.** Weapon-debris crash fix (guards the `CreateShaderResourceView` failure that
  crashes NVIDIA Flex debris on 10-series+ GPUs) with a control to re-enable `bNVFlexEnable`,
  plus **scene depth capture** status and a depth debug view.
- **Panini projection** (ultrawide edge correction). Rectilinear projection stretches the
  edges of a wide field of view; Panini trades that for gently curved horizontal lines while
  keeping verticals straight. Two parts, because a projection *matrix* mathematically cannot
  express Panini (it is non-linear):
  1. a screen-space remap of the finished frame, and
  2. the engine-side change that makes it correct — the game is told to render at a **wider
     FOV** so the warp has real pixels at the screen edge instead of smearing. The overlay
     computes the required FOV and writes it to `[Display]` in `Fallout4Custom.ini`.
  Caveats: the warp is applied after the UI is composited, so the **HUD warps with it**; and
  the FOV change needs a restart. `[Panini] bEnablePanini` (default off).
- **Crash reporting.** An unhandled-exception filter writes
  `Documents\My Games\Fallout4\F4SE\VisualForge-crash.log` on a fault: exception type, the
  **faulting module and offset**, the address an access violation touched, a full register
  dump, a symbolised call stack (function names and source lines where a matching PDB is
  available), and the loaded-module map. This exists because Buffout 4 cannot run on the
  1.11.x runtime — it is built on CommonLibF4, which stops at 1.10.984, and F4SE treats
  1.11.137+ as a different structure layout. Unlike Buffout this only *observes*: it never
  replaces engine allocators, so it is runtime-version independent.
- **Scene depth capture.** Watches every depth-stencil view the engine binds and keeps the
  full-resolution one — the main scene depth — copying it each frame into a shader-readable
  texture. This is the foundation screen-space GI/AO needs; on its own it changes nothing you
  see. A one-shot self-test reads the depth back on the CPU and logs min/max/spread so the
  capture can be verified from the log without touching the UI.
- **Info tab.** FPS, GPU, resolution, a **"Write changes to game INIs"** button that
  persists your session edits to `Fallout4Prefs.ini` / `Fallout4Custom.ini` (routing each
  key to the file the engine actually reads it from), and a **Skip intro movies** toggle.
- **Intro-movie skip.** At load (before the main menu is built) the plugin blanks
  `sIntroSequence` — whose value is `GameIntro_V3_B.bk2`, the startup Bethesda/Vault-Tec
  montage — and `sIntroMovie` in live memory, leaving the animated main-menu background and
  the S.P.E.C.I.A.L. videos intact. Controlled by `[Startup] bSkipIntroMovies` (default on).

## Layout

```
VisualForge/
  src/                 plugin source (see module list below)
  external/            build dependencies (not checked in)
    f4se/  common/     ianpatt F4SE 0.7.8 + common (built from master)
    imgui/             Dear ImGui 1.91.8 (+ dx11/win32 backends)
    minhook/           MinHook 1.3.4
  CMakeLists.txt
  build/               CMake build tree (build/Release/VisualForge.dll)
```

Modules: `main` (F4SE entry + version data), `Log`, `Config`, `PatternScan` (PE walk +
.rdata checks), `EngineSettings` (the binder), `IniWriter`, `IntroSkip`, `PostProcess`
(grading + CAS), `WeaponDebris` (crash fix), `D3D11Hook` (Present/ResizeBuffers hook +
WndProc + ImGui lifecycle), `Overlay` (the UI), `SettingsCatalog.h` (generated list of every
engine setting name). See `docs/EngineHooks.md` for the deep hook-surface analysis and roadmap.

## Build & install

```
cmake -B build -S .
cmake --build build --config Release
copy build\Release\VisualForge.dll "..\Data\F4SE\Plugins\VisualForge.dll"
```

Requires VS 2022 + MSVC and CMake (both already on this machine). Launch the game with
`f4se_loader.exe` in the game root.

### Test-driven renderer build

The renderer work uses the checked-in CMake presets and CTest/Catch2 suites:

```powershell
cmake --preset vs2022-x64-debug
cmake --build --preset vs2022-x64-debug
ctest --preset vs2022-x64-debug --output-on-failure
```

Use `vs2022-x64-release` for the Release configuration. GPU, replay, game, soak, and
performance labels are added as their implementation phases land; unit tests never launch
Steam or Fallout. The full red–green–refactor sequence is documented in
`docs/vulkan_renderer_implementation_plan.md`.

## Config

`Data\F4SE\Plugins\VisualForge.ini` (created on first run):

```ini
[Overlay]
iToggleKey=121          ; virtual-key code; 121 = F10

[Startup]
bSkipIntroMovies=1

[Systems]
bWeaponDebrisCrashFix=1 ; guard CreateShaderResourceView so Flex debris can't crash

[Sharpen]
bEnabled=0
fSharpness=0.400

[Grade]
bEnabled=0
fExposure=0.0
fContrast=1.0
fSaturation=1.0
fVibrance=0.0
fTemperature=0.0
fTint=0.0
bFilmic=0
bLut=0              ; enable the loaded 3D LUT
fLutIntensity=1.0
sLutFile=           ; filename within Data\F4SE\Plugins\LUTs\
```

## How the binder works (the interesting part)

Bethesda's `Setting` objects have the layout `{ vtable*, Data(8 bytes), char* name }`.
We:
1. dump every setting name string the game ships (`SettingsCatalog.h`),
2. locate those strings in the image's data sections,
3. find every 8-byte-aligned pointer to one of those strings, treat the address 0x10
   before it as a candidate `Setting`, and
4. confirm the qword at offset 0 (the vtable pointer) lands in a read-only data section.

That confirmation is what makes it address-independent and safe across game updates: no
offset is ever hardcoded, so a new runtime version just needs the catalog regenerated
(and usually not even that).

## Binder coverage

**2207 of 2282** catalog entries resolve on 1.11.221. The remaining 75 split into ~37
whose name string exists but has no static `Setting` object (dynamically registered) and
~38 with no name string at all (console-command tokens, not real settings). An earlier
build resolved only 2132: the linker packs some setting names directly after non-string
data with no leading NUL, so the name is a *suffix* of the run ending at the terminator —
Pass 1 now tests every name-length suffix ending at each NUL, which recovered 75 settings
including `bVolumetricLightingEnable`.

## Known gaps

- Some settings are only read at area-load; the overlay notes these ("Applied on area
  reload"). Changing them takes effect after a cell transition or save/load.
- `sLocalSavePath` / `sStartingConsoleCommand` have a name string but no static object;
  they aren't tunable at runtime (they're consumed once at startup).

## Roadmap (all into this same plugin)

NVFlex debris crash-fix wrapper (re-enable `bNVFlexEnable`), DLAA/DLSS via Streamline,
godray grid/cascade patches beyond the INI ceiling, SSR ray-count patch, custom HLSL
shader replacement, and HDR10 output.
