# Phase 1 — test spine and preserved plugin baseline

**Status:** complete on 2026-08-15 (Australia/Sydney).

## Slice

The existing VisualForge DLL and a standalone Catch2 test executable build from checked-in
CMake presets. Renderer startup emits a versioned health record whose default is inert
`Off`, with no backend and no render suppression.

## Baseline

- Copied source baseline: `F:\SteamLibrary\steamapps\common\Fallout 4\VisualForge`.
- Development workspace: `F:\Development\fallout-mods\VisualForge`.
- Baseline `CMakeLists.txt` SHA-256: `6E89095E5B20A9359AD774DF0ED81F09852E148BCDBE3546BAB6EBDE8F6AA93B`.
- Baseline `src/main.cpp` SHA-256: `3177913BC9B80BCC0DF93E043E13B37D23F603849D39B5BD0C1080E57D61F7AB`.
- Baseline Debug DLL: 3,997,184 bytes, SHA-256 `6FFFFD981821A0CB1645A896B98225E0E081BEA6DCC5B821D080563B29B2F7F2`.
- Baseline Release DLL: 969,216 bytes, SHA-256 `B50BA10DD062A3D76280BB07DC44A587E6075F422E9D9476CF7F4233C60504B1`.
- Installed DLL before/after smoke: SHA-256 `FCBB0032169C05BF992F77F96A6F3B5EA6C8AD7A5840DB8F516FCE90278FB29F`.
- Export baseline: exactly `F4SEPlugin_Load` and `F4SEPlugin_Version`.

Catch2 is pinned at v3.15.3, commit
`8b08d4d79514f45f7e4ce2a607ac9c94e920d1bb`.

## Red evidence

`P01_startup_health_defaults_to_inert_off_mode`,
`P01_renderer_mode_names_are_stable`, and
`P01_startup_health_format_reports_active_fields` compiled and failed against the
deliberately unimplemented contract. Failures showed schema `0`, mode `Disabled`, the
placeholder mode names, and `renderer-health: unimplemented`.

The initial export harness exposed two harness defects—missing `CMAKE_DUMPBIN` under the
Visual Studio generator and decorated Debug output. Tool discovery and parsing were fixed;
the export characterization was green before product Green was accepted.

## Green evidence

- Debug: 4/4 unit/contract tests passed.
- Release: 4/4 unit/contract tests passed.
- New portable code builds with MSVC `/W4 /WX`.
- The health record is
  `renderer-health schema=1 mode=Off backend=absent suppression=off`.
- The plugin still exports exactly its two F4SE symbols.

## Game smoke

Artifact: `artifacts/phase-01/smoke-20260815-004719` (ignored from source control because it
contains local logs/backups).

The bounded harness observed:

- F4SE loader and Fallout process startup;
- the exact inert renderer health record;
- `hook: initialized` at 3440×1440, DXGI format 28;
- `post: shaders compiled`;
- no early process exit.

The harness closed only its tracked processes and restored the installed DLL, VisualForge
logs, and tracked Fallout INIs in `finally`. The restored installed DLL hash matches the
pre-test value above.

## Promotion result

Phase 1 is the fallback baseline for Phase 2. No Vulkan loader, backend DLL, engine renderer
detour, resource mirror, or draw suppression is active.
