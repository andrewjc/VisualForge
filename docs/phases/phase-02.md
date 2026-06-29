# Phase 2 — Build gate, modes, hooks, and fault controller

Status: complete

## Implemented slice

- Exact Fallout 4 1.11.221 fingerprint validation:
  - runtime version;
  - AMD64 PE machine;
  - PE timestamp and SizeOfImage;
  - executable byte size and streaming SHA-256;
  - Address Library byte size and streaming SHA-256.
- Bounds-checked PE32+ header parsing with typed truncation/corruption errors.
- Immutable hook-site predicates for:
  - Renderer::WindowSizeChanged at RVA 0x18174F0;
  - Renderer::Begin at RVA 0x1817E30;
  - Renderer::End at RVA 0x1818080;
  - DrawWorld::Forward at RVA 0x21F16D0;
  - relocated NiCamera::vtable[0] at RVA 0x267DD60.
- Transactional hook-set preparation/publication and reverse-order rollback.
- Central renderer state machine and first-fault controller.
- Pure bootstrap decision that keeps rejected builds and predicates disabled and
  reaches Probing then validated Off only on complete success.
- Windows startup integration. This phase validates only; it installs no new
  renderer detour and suppresses no vanilla render call.

## TDD/RGR evidence

The initial pure-contract red run contained 13 failing Phase 2 cases while all
Phase 1 contracts remained green.

The integration red run preserved 14 prior Phase 2 cases and failed six new
cases with nine assertions on the deliberate hook-manifest and Windows-probe
stubs. The bootstrap red run failed all three new cases with nine assertions.

During Green, the unchanged missing-file regression exposed a stack overflow
caused by a 1 MiB local streaming buffer. The implementation was corrected to a
bounded 64 KiB chunk and the same test passed.

Final automated results:

- Debug: 27/27 CTest tests passed.
- Release: 27/27 CTest tests passed.
- Phase 2 unit coverage: 141 assertions across 20 cases before the final
  bootstrap cases; bootstrap adds 21 assertions across three cases.
- Plugin export contract still exposes only F4SEPlugin_Load and
  F4SEPlugin_Version.

Both configurations build with /W4 /WX on vf_core and the Windows platform
adapter.

## Live game gate

Release artifact:

out/build/vs2022-x64-release/Release/VisualForge.dll

Bounded smoke artifact:

artifacts/phase-02/smoke-20260815-102435

Observed startup records:

    build-probe: complete
    build-gate: accepted
    hook-manifest: accepted validated=5 failed-site=0 reason=none
    startup-probe: ready hooks=5
    renderer-probe: validated mode=Off state=Disabled
    renderer-health schema=1 mode=Off backend=absent suppression=off
    hook: initialized
    post: shaders compiled

The game did not exit early. The harness restored the pre-existing installed
plugin, logs, and INIs in finally. The restored installed DLL SHA-256 is:

FCBB0032169C05BF992F77F96A6F3B5EA6C8AD7A5840DB8F516FCE90278FB29F

## Promotion decision

Phase 2 is promoted. Phase 3 may add observation packets and trace I/O, but
Off and all Phase 1/2 contracts remain inherited acceptance gates.
