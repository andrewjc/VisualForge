# VisualForge

An F4SE plugin that gives Fallout 4 a live in-game rendering control panel, a
post-process grading and sharpening pass, and — as ongoing work — a Vulkan
renderer built to run alongside the engine's own D3D11 one.

![runtime](https://img.shields.io/badge/Fallout%204-1.11.221-blue)
![f4se](https://img.shields.io/badge/F4SE-0.7.8-blue)
![toolchain](https://img.shields.io/badge/MSVC%202022-C%2B%2B20-blue)
![graphics](https://img.shields.io/badge/Vulkan-SDK%20required-blue)

Two things live in this repository. The **plugin** is finished and usable: an
overlay that binds every INI-backed engine setting by name with no hardcoded
addresses, a colour-grading and sharpening pass, a crash reporter, and a
weapon-debris crash fix. The **Vulkan renderer** is in progress — it mirrors the
live scene into a second, independently rendered frame so its output can be
compared against the engine's, phase by phase, against a CPU reference.

Toggle the overlay with **F10**.

## Quick start

Requires Visual Studio 2022 (MSVC), CMake, and the
[Vulkan SDK](https://vulkan.lunarg.com/) with `VULKAN_SDK` set — the build
compiles shaders with `glslc` and links against `Vulkan::Vulkan`.

```powershell
git clone --recurse-submodules <this-repo> VisualForge
cd VisualForge
cmake --preset vs2022-x64-release
cmake --build --preset vs2022-x64-release
copy out\build\vs2022-x64-release\Release\VisualForge.dll "<game>\Data\F4SE\Plugins\"
```

Launch through `f4se_loader.exe` in the game root. If you cloned without
`--recurse-submodules`, run `git submodule update --init` first or the build
will not configure.

## What the plugin does

- **Live engine-setting binder.** On the first rendered frame it scans the
  running executable and binds INI-backed engine `Setting` objects by name —
  **2207 of 2282** catalogue entries on 1.11.221 — with no hardcoded addresses.
  Editing a value in the overlay writes straight into live memory.
- **Basic and Advanced modes.** Basic is a short list of one-click presets
  (visual quality, look, brightness/vividness/sharpness). Advanced is the full
  tabbed UI. The choice persists in `[Overlay] bBasicMode`.
- **Quick Tweaks.** Curated controls for shadows, TAA and sharpness, godrays,
  ambient occlusion, grass, LOD, terrain, and image-space effects.
- **Browser.** Filterable list of all 2282 known settings.
- **Colour and Sharpen.** A real-time grading pass — exposure, contrast,
  saturation, vibrance, white balance, optional ACES-approximate filmic
  tonemap, and a `.cube` 3D LUT loader — plus AMD FidelityFX Contrast Adaptive
  Sharpening, applied to the final frame in one pass. Drop `.cube` files in
  `Data\F4SE\Plugins\LUTs\`; four ship as examples.
- **Weapon-debris crash fix.** Guards the `CreateShaderResourceView` failure
  that crashes NVIDIA Flex debris on 10-series and later GPUs, with a control
  to re-enable `bNVFlexEnable`.
- **Panini projection** for ultrawide edge correction: a screen-space remap of
  the finished frame, plus the wider render FOV that gives the warp real pixels
  at the screen edge. A projection matrix cannot express Panini, because it is
  non-linear. The HUD warps with it, and the FOV change needs a restart.
  `[Panini] bEnablePanini`, default off.
- **Crash reporting.** An unhandled-exception filter writes
  `Documents\My Games\Fallout4\F4SE\VisualForge-crash.log`: exception type,
  faulting module and offset, the address an access violation touched, a
  register dump, a symbolised call stack where a PDB is available, and the
  module map. This exists because Buffout 4 cannot run on 1.11.x — it is built
  on CommonLibF4, which stops at 1.10.984. Unlike Buffout, this only observes:
  it never replaces engine allocators, so it is runtime-version independent.
- **Scene depth capture.** Keeps the full-resolution depth-stencil view the
  engine binds and copies it each frame into a shader-readable texture. On its
  own it changes nothing you see; it is the foundation screen-space GI and AO
  need.
- **Intro-movie skip.** Blanks `sIntroSequence` and `sIntroMovie` in live memory
  before the main menu is built, leaving the animated menu background and the
  S.P.E.C.I.A.L. videos intact. `[Startup] bSkipIntroMovies`, default on.

## The Vulkan renderer

`docs/vulkan_renderer_implementation_plan.md` defines 27 phases, taken in
order, each one red-green-refactor with mutation testing and verified in-engine
before the next begins.

The approach is a mirror rather than a replacement. The plugin captures the
engine's draw stream, vertex layouts, camera and constant buffers, rebuilds the
scene as pointer-free versioned packets, and renders it with Vulkan into a
separate frame. Every phase is pinned by a CPU reference implementation that
the GPU result is compared against, so a difference is attributable to one
term rather than to the renderer as a whole.

**Status.** Geometry, camera, terrain, alpha, material families, ray-traced
shadows and reflections, diffuse GI with temporal accumulation, sorted
transparency and decals, and the post chain are implemented and tested. The
engine's directional light is now recovered from its own shader reflection and
delivered to the backend. Per-draw texture binding is not done. The engine's
own shaders ship with their reflection stripped, so material constants remain
unnamed. `journal.md` carries the running record, including what is measured
and what is still unattributed.

Nothing here replaces the engine's renderer in normal play. The mirror is
opt-in and driven by the capture harness.

## Layout

```
src/
  renderer_api/        ABI shared between plugin and backend
  renderer_core/       packets, scene assembly, lighting, CPU reference
  renderer_backend/    the Vulkan renderer (VisualForgeRenderer.dll)
  renderer_host/       backend loading and lifecycle
  renderer_trace/      capture tracing
  tools/               replay and probe executables
shaders/               GLSL, compiled to SPIR-V and embedded at build time
tests/unit/            Catch2 suites
tools/game_smoke/      live capture harness (PowerShell)
docs/                  design, hook analysis, and per-phase records
external/              dependencies (submodules and vendored sources)
```

## Building and testing

```powershell
cmake --preset vs2022-x64-debug
cmake --build --preset vs2022-x64-debug
ctest --preset vs2022-x64-debug --output-on-failure
```

`vs2022-x64-release` selects the Release configuration. **Run both.** A
release-only crash at process exit once passed cleanly in debug and printed
`result=pass` before dying; a test that reports success and then crashes is a
failing test.

367 tests currently pass in both configurations. They carry labels — `unit`,
`contract`, `gpu`, and `phaseNN` — so a phase can be run alone:

```powershell
ctest --preset vs2022-x64-debug -L phase20
```

Unit tests never launch Steam or Fallout 4. GPU contract tests need a Vulkan
device.

## Configuration

`Data\F4SE\Plugins\VisualForge.ini`, created on first run:

```ini
[Overlay]
iToggleKey=121          ; virtual-key code; 121 = F10

[Startup]
bSkipIntroMovies=1

[Systems]
bWeaponDebrisCrashFix=1

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
bLut=0
fLutIntensity=1.0
sLutFile=               ; filename within Data\F4SE\Plugins\LUTs\
```

## How the binder works

Bethesda's `Setting` objects have the layout
`{ vtable*, Data (8 bytes), char* name }`. The binder:

1. dumps every setting name the game ships (`SettingsCatalog.h`),
2. locates those strings in the image's data sections,
3. finds every 8-byte-aligned pointer to one of them and treats the address
   `0x10` before it as a candidate `Setting`, and
4. confirms the qword at offset 0 — the vtable pointer — lands in a read-only
   data section.

Step 4 is what makes it address-independent and safe across game updates: no
offset is hardcoded, so a new runtime version usually needs nothing but a
regenerated catalogue.

Of the 75 entries that do not resolve, roughly 37 have a name string but no
static `Setting` object because they are registered dynamically, and roughly 38
have no name string at all — they are console-command tokens rather than
settings.

## Known gaps

- Some settings are read only at area load. The overlay marks these; changes
  take effect after a cell transition or a save/load.
- `sLocalSavePath` and `sStartingConsoleCommand` have name strings but no
  static object. They are consumed once at startup and are not tunable.
- The Panini warp is applied after the UI is composited, so the HUD warps too.

## Roadmap

Finishing the 27 renderer phases is the main line of work. Alongside it:
DLAA/DLSS through Streamline, godray grid and cascade patches beyond the INI
ceiling, an SSR ray-count patch, custom HLSL shader replacement, and HDR10
output.

## Dependencies

| Component | Source |
| --- | --- |
| F4SE 0.7.8 + common | [ianpatt/f4se](https://github.com/ianpatt/f4se), [ianpatt/common](https://github.com/ianpatt/common) (submodules) |
| Catch2 v3.15.3 | [catchorg/Catch2](https://github.com/catchorg/Catch2) (submodule) |
| Dear ImGui | vendored in `external/imgui` |
| MinHook | vendored in `external/minhook` |

## Licence

No licence has been chosen yet, so default copyright applies and no permissions
are granted. The bundled dependencies keep their own licences.
