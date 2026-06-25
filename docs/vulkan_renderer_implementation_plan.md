# Vulkan renderer implementation plan — 27 vertical phases

This is the executable delivery plan for the target described in [`vulkan_renderer_design.md`](vulkan_renderer_design.md), using the build-specific evidence in [`engine_render.md`](engine_render.md). It refines the architecture's eight macro stages into **N = 27 discrete phases**.

Each phase implements one observable vertical slice through the relevant host, packet, backend, shader, presentation, and diagnostic boundaries. A phase is not complete because a component compiles; it is complete only when its slice can be demonstrated through its public boundary and its automated promotion gate is green.

**RGR means red–green–refactor.** TDD controls the development order within every phase. No calendar estimate is attached: evidence and gates, rather than elapsed time, decide when a phase advances.

**Status:** implementation active. Phases 1–7 are promoted. The offline
texture/residency, authored default-material, camera/view/frame, opaque
multi-object scene, instance/streaming/cell-lifecycle, deformation, and
terrain/landscape/LOD slices for Phases 8–14 are implemented and green in
Debug/Release; all await their bounded live capture/overlay gates. Live
capture is currently blocked on a missing Address Library binary in the game
install. Phase 15 is next. All game-facing additions are opt-in, hooks remain
build-gated, and world draw suppression is still disabled.

## 1. Delivery invariants

1. `Off` remains behaviorally equivalent to the current VisualForge plugin throughout development.
2. New engine hooks fail open to the original function and are installed transactionally.
3. No raw engine pointer crosses the asynchronous backend ABI.
4. No phase overwrites `Fallout4.exe`, a BA2 archive, a loose source asset, or a save game.
5. Mirror output remains non-authoritative until Phase 25; vanilla world submission is not suppressed earlier.
6. Every bug fix begins with a regression test that reproduces the fault.
7. A skipped, weakened, deleted, or re-baselined acceptance test requires a written contract change; it is not an implementation shortcut.
8. Vulkan core and synchronization validation must report zero errors at every GPU phase gate.
9. Tests use deterministic seeds, explicit tolerances, and versioned fixtures. A flaky test is a defect, not a candidate for automatic retry.
10. A phase can be split through an architecture-decision record if its red suite exposes more than one independent contract. Phases are never silently merged to hide an incomplete gate.

## 2. RGR operating contract

Every phase follows the same cycle, repeated in small increments rather than once per large feature:

### Red

- State the behavior at a public boundary in a test named `Pxx_<contract>_<behavior>`.
- Run it before production code is added and verify that it fails for the intended missing behavior, not because the test cannot compile, a fixture is absent, or the machine is misconfigured.
- Record the test name, command, and concise failure reason in the phase change record. Intentionally failing commits do not merge to the integration branch.
- Include negative and fault-injection cases before implementing a risky success path.

### Green

- Implement the smallest end-to-end path that satisfies the red test.
- Keep unsupported behavior explicit and fail-open; do not return plausible dummy data from a production path.
- Run the focused test after each increment, then the full fast suite before proceeding.
- Do not optimize or generalize beyond what the current contract requires.

### Refactor

- Refactor only while the focused and full fast suites are green.
- Remove duplication, improve ownership boundaries, replace test-only seams with stable interfaces, and add invariant assertions without changing observable behavior.
- Run contract, replay, GPU, and game tiers required by the phase after the refactor.
- Capture performance and diagnostics after correctness, so optimization cannot conceal a semantic change.

### Phase promotion

A phase promotes only from a clean checkout when its exit gate passes. The promotion record contains test results, validation output, relevant images/traces, known unsupported classifications, and performance counters. The next phase starts from that green baseline.

## 3. Test and build architecture established by Phase 1

The existing `VisualForge/CMakeLists.txt` builds one DLL and has no automated test target. Phase 1 introduces test seams without moving unrelated legacy modules merely for tidiness.

### 3.1 Targets

| Target | Purpose | May depend on Fallout running? |
|---|---|---:|
| `vf_core` | Platform-light state machines, packets, handles, math, format/material translation, and frame-graph logic | No |
| `VisualForge` | Existing F4SE host plus validated capture/interop adapters | Loaded only for game tests |
| `VisualForgeRenderer` | Lazily loaded Vulkan backend DLL | No for replay/GPU tests |
| `vf_unit_tests` | Pure deterministic unit and property tests | No |
| `vf_contract_tests` | ABI, serialization, shader-reflection, format-table, and fixture contracts | No |
| `vf_packet_replay` | Loads a trace and executes backend work without Fallout | No |
| `vf_gpu_tests` | Hidden-window/headless Vulkan and D3D11 interop tests on a real adapter | No |
| `vf_game_smoke` | Controlled F4SE launch, log/capture assertions, watchdog, and dump collection | Yes |

CTest is the orchestrator. Catch2 v3 is pinned to a reviewed release/commit, vendored with the other build dependencies, and linked only to test executables. New portable code compiles at `/W4`; warnings introduced by that code are errors. Shader compilation and reflection are build steps, not runtime surprises.

### 3.2 Test tiers

| Label | Contents | Normal cadence |
|---|---|---|
| `unit` | State, math, handles, packet helpers, parsers, translators | Every local edit and change request |
| `contract` | Binary ABI, layout, shader reflection, PE fixtures, format capabilities | Every change request |
| `replay` | Golden packet traces and deterministic image/buffer comparisons | Every change request |
| `gpu` | Vulkan validation, resource use, frame graph, RT and D3D11 interop | Required from Phase 4 onward |
| `game` | Actual 1.11.221 plugin load and bounded capture scenarios | Phase gate and scheduled run |
| `soak` | Resize, reload, streaming, bridge, memory, and device-fault stress | Promotion/nightly |
| `perf` | Calibrated CPU/GPU timestamps and allocation counts | Promotion; never substitutes for correctness |

The standard commands created by Phase 1 are:

```powershell
cmake --preset vs2022-x64-debug
cmake --build --preset vs2022-x64-debug
ctest --preset vs2022-x64-debug --output-on-failure -L unit
ctest --preset vs2022-x64-debug --output-on-failure -L contract
```

GPU and game presets are explicit opt-ins. Unit tests never launch Steam or Fallout. The game harness may use `cdb.exe` as a watchdog/debugger and must preserve the dump, VisualForge log, renderer log, trace ID, and command line on failure.

### 3.3 Fixture policy

- Small synthetic meshes, textures, shaders, packet traces, and approved lossless reference images live under `VisualForge/tests/fixtures/`.
- Tests for the installed executable and BA2 corpus identify local files by expected hashes/versions; they do not copy proprietary binaries into the test tree.
- Packet fixtures carry schema version, endian marker, source build, capability signature, random seed, and content hashes.
- Exact comparison is used for serialization, state transitions, integer IDs, pattern images, and deterministic shader buffers. Render parity uses declared per-fixture metrics such as maximum error, RMSE, alpha-coverage difference, normal angle, highlight width, and temporal convergence.
- Stochastic RT tests use fixed sample sequences. A separate statistical test can validate distribution properties without replacing deterministic regression tests.
- Golden updates require a before/after artifact and an explanation of the intended contract change.

### 3.4 Common definition of done

Every phase must satisfy all applicable items:

- focused red tests were observed failing for the intended reason;
- new tests and the complete lower-tier suite are green after refactoring;
- public packet/shader layouts have compile-time and reflection assertions;
- all error paths return a typed result and leave the renderer in a defined mode;
- fault injection covers allocation, loader, device, synchronization, and malformed-input failures introduced by the phase;
- resource ownership has a destruction/retirement test;
- zero Vulkan validation and synchronization-validation errors;
- no unexpected D3D world-target writer for any scenario claimed by the phase;
- logs and GPU markers contain stable frame/resource correlation IDs;
- documentation and the supported/unsupported ledger match the implementation;
- the current phase can be disabled independently without breaking the previous phase.

## 4. Phase map

| Phase | Vertical slice | First mode/capability unlocked | Architecture stage |
|---:|---|---|---:|
| 1 | Test spine and preserved plugin baseline | Testable `Off` | 0 |
| 2 | Build gate, renderer state machine, and fault controller | Validated `Off`/probe | 0 |
| 3 | Pointer-free packets, trace writer, inspector, and replay | `Observe` | 0 |
| 4 | Backend DLL ABI, Vulkan probe, and adapter match | Backend capability report | 1 |
| 5 | D3D11–Vulkan shared-image/fence bridge | Vulkan pattern in `Mirror` | 1 |
| 6 | First replayed Vulkan raster frame | Synthetic opaque mesh | 2 |
| 7 | Engine mesh/buffer capture and mirroring | Real captured mesh | 2 |
| 8 | Texture, sampler, view, and mip mirroring | Textured captured mesh | 2 |
| 9 | Specular/smoothness material translation | Authored default material | 2 |
| 10 | Camera, view, frame, and pass packet capture | Correct world projection | 3 |
| 11 | Opaque static raster scene mirror | Multi-object G-buffer scene | 3 |
| 12 | Instancing, streaming, generations, and cell lifecycle | Stable world residency | 3 |
| 13 | Skinning, morphs, wind, and dynamic geometry | Animated characters/objects | 3 |
| 14 | Terrain, landscape layers, and LOD | Exterior ground/LOD | 3 |
| 15 | Alpha-tested and two-sided visibility | Foliage/fences/hair cards | 3 |
| 16 | Specialized opaque material families | Skin/hair/eye/snow/POM/glow | 3 |
| 17 | Lights, sky, weather, fog, and raster parity | Lit raster mirror | 3 |
| 18 | BLAS/TLAS lifecycle and ray-traced shadows | RT shadow mirror | 4 |
| 19 | Ray-traced reflections and ray-footprint LOD | RT reflection mirror | 4 |
| 20 | Diffuse GI, temporal reconstruction, and denoising | Stable RT indirect light | 4 |
| 21 | Decals, sorted transparency, and particles | General effect composition | 5 prerequisite |
| 22 | Water, glass, refraction, and underwater views | Specialized transmissive surfaces | 5 prerequisite |
| 23 | Vulkan image-space/post chain | Post mirror | 6 prerequisite |
| 24 | Scaleform, Bink, Flex, overlay, and final bridge ordering | Complete compatibility composite | 6 prerequisite |
| 25 | Classified world-only suppression and fail-open recovery | World `Takeover` | 5 |
| 26 | World-and-post suppression ledger and acceptance matrix | Full `Takeover` | 6 |
| 27 | Native Vulkan WSI and compatibility-island retirement | Optional `Native` | 7 |

The order is deliberately conservative: all major world, special-surface, post, and compatibility paths are implemented in Mirror before the first authoritative world frame in Phase 25.

## 5. Detailed phases

### Phase 1 — Test spine and preserved plugin baseline

**Slice:** Build the existing plugin and a standalone test executable from one preset, with an observable health record proving that renderer work is disabled by default.

**Red:** Add a CMake-script test that requires the planned targets/presets and fails against the current single-DLL project. Add contract tests for the plugin filename/exports, default renderer mode, and a golden current-feature health record.

**Green:** Add `VF_BUILD_TESTS`, CTest, the pinned test dependency, presets, `vf_core`, and the unit/contract runners. Introduce injectable clock, filesystem, logger, and process/GPU service interfaces only where new renderer code needs them. Emit a versioned startup health record while leaving all existing Present, overlay, depth, LUT, and tuning behavior unchanged.

**Refactor:** Normalize new target settings and move only genuinely pure helpers into `vf_core`; do not perform a wholesale rewrite of existing plugin modules.

**Gate:** Debug and RelWithDebInfo configure/build from a clean tree; CTest discovers the labeled suites; export and startup-health contracts pass; the current F10 overlay, depth capture, post pass, and crash log complete a bounded game smoke test with renderer mode `Off`.

### Phase 2 — Build gate, modes, hooks, and fault controller

**Slice:** The plugin recognizes the exact executable, validates candidate hook sites, and publishes a renderer state without changing a render call.

**Red:** Test the recorded 1.11.221 PE fingerprint, Address Library manifest, instruction/vtable predicates, and all invalid/truncated/mismatched variants. Test legal and illegal state transitions, partial hook preparation, injected patch failure, and restoration to vanilla.

**Green:** Implement `BuildGate`, `HookManifest`, `HookTransaction`, `RendererMode`, and `FaultController`. Checks consume immutable descriptors; hook publication is one atomic generation; failures produce typed reasons and leave every original pointer active.

**Refactor:** Separate pure validation from process memory access and MinHook adapters. Centralize renderer state transitions so features cannot invent private enable flags.

**Gate:** The installed build reaches `Probing` then validated `Off`; every corrupt fixture remains `Disabled`; forced failure at every hook-transaction step leaves no enabled detour or stale vptr; existing plugin behavior remains unchanged.

### Phase 3 — Packet schema, observation trace, and replay

**Slice:** One vanilla frame produces a pointer-free trace that can be inspected and replayed outside Fallout, initially containing frame identity, swap-chain/view metadata, and classified D3D writer events.

**Red:** Test packet sizes/alignments, schema negotiation, endian marker, CRC, unknown-record skipping, truncated/oversized input, deterministic serialization, pointer rejection, and stable IDs across address changes. Add a golden minimal trace before the writer exists.

**Green:** Implement the versioned renderer API, frame/resource event envelopes, arena ownership, trace writer/reader, `FrameCapture`, and `vf_packet_replay --inspect`. Add render-thread identity and CPU/GPU correlation IDs at the current Present/Resize boundaries.

**Refactor:** Keep serialization independent from engine types and I/O. Replace ad-hoc logging payloads with shared typed diagnostic records where appropriate.

**Gate:** Two runs over the same synthetic event stream are byte-identical; corrupt traces fail without allocation abuse; an in-game `Observe` run writes a trace that the external tool parses to the same frame/view/writer summary; disabling trace capture restores the Phase 2 baseline.

### Phase 4 — Backend ABI and Vulkan capability probe

**Slice:** The host lazily loads `VisualForgeRenderer.dll`, negotiates an ABI, selects the Vulkan physical device matching the D3D adapter LUID, and returns a complete capability report without rendering.

**Red:** Test missing DLL/loader/export, ABI major mismatch, optional minor fields, null callbacks, wrong LUID, missing required extensions/features, queue-family selection, device-creation failure, and repeated load/unload-state requests. GPU tests initially fail because no backend exists.

**Green:** Implement the narrow C ABI, backend loader, Vulkan instance/device probe, LUID match, feature/property chains, required/optional capability bitsets, debug messenger, and typed failure report. The host remains loadable without `vulkan-1.dll` or the backend DLL.

**Refactor:** Isolate Vulkan calls behind a dispatch/service layer usable by fault-injection tests. Normalize capability signatures for cache and takeover gating.

**Gate:** The current machine reports the chosen adapter, driver, queue, required KHR features, BC formats, descriptor limits, and interop candidates; all negative cases remain `Off`; validation reports zero errors; unloading is deferred safely or explicitly unsupported rather than attempted unsafely.

### Phase 5 — D3D11–Vulkan interop bridge

**Slice:** Vulkan writes a deterministic pattern into a D3D11-created shared image, transfers ownership through external synchronization, and the current DXGI path displays it in Mirror/debug mode.

**Red:** Test the bridge format/usage table, LUID mismatch, handle ownership, monotonic tickets, illegal double-acquire, timeout, resize epoch, stale handle, device removal, and teardown with work in flight. GPU pattern tests cover color quadrants and frame-index encoding.

**Green:** Implement the D3D11 shared-texture ring, NT-handle import, shared-fence/timeline path, explicit queue ownership, bridge compositor, and resize recreation. Implement keyed-mutex fallback only when its capability contract is satisfied; otherwise report the bridge unsupported.

**Refactor:** Express bridge ownership as a small state machine with RAII handle wrappers and one timeline-retirement path. Remove duplicate D3D/Vulkan format logic.

**Gate:** 10,000 exchanges plus repeated resize, minimize/restore, fullscreen-mode, and alt-tab cycles show the correct sequence with no CPU readback, busy-wait, leaked handle, overlapping API ownership, timeout, or validation error. A forced ticket failure selects vanilla at the next safe boundary.

### Phase 6 — First replayed Vulkan raster frame

**Slice:** `vf_packet_replay` submits a synthetic frame containing one opaque indexed mesh and material; Vulkan rasterizes it to HDR/depth and the bridge or file-output path shows the expected image.

**Red:** Add packet fixtures for triangle winding, index width, viewport/scissor, depth pass/fail, missing resource, shader-layout mismatch, and resize. Add exact pattern and tolerance-based raster goldens before creating the pipeline.

**Green:** Implement the minimal resource registry, upload arena, descriptor set, dynamic-rendering raster pipeline, frame graph, shader build/reflection step, HDR target, depth target, tone-map/copy, and debug markers required by this one frame.

**Refactor:** Separate packet validation, resource realization, frame-graph declaration, and command recording. Share C++/shader layout definitions through generated reflection data rather than duplicated offsets.

**Gate:** The standalone replay renders all fixtures deterministically on the selected adapter; malformed packets render an explicit diagnostic or fail before submission; resize recreates only extent-dependent state; validation and retirement tests are clean.

### Phase 7 — Engine mesh and buffer capture

**Slice:** A verified engine resource-manager hook captures one real static mesh's CPU vertex/index payload, mirrors it by stable generation, and the replay/backend renders that mesh with a diagnostic material.

**Red:** Add randomized pack/unpack parity tests for every currently observed `VertexDesc`, index-range/base-vertex/winding cases, immutable/dynamic buffer descriptors, duplicate create, update-before-create, destroy-while-in-flight, and pointer-reuse generation changes.

**Green:** Clone the verified resource-manager interface or hook the mapped creation boundary, emit `MeshDesc` and buffer events, translate supported streams, upload them, and retain/retire resources by timeline. Unknown layouts remain classified and visible in Observe.

**Refactor:** Consolidate vertex-format tables and stable-handle generation logic. Keep hook code limited to validation/copy/event emission; decoding belongs to shared core/backend code.

**Gate:** A captured real mesh replays with matching bounds, topology, vertex attributes, and draw range; all observed supported layouts pass randomized parity; repeated create/destroy and address reuse produce no stale resource; no draw is suppressed.

### Phase 8 — Texture, sampler, view, and mip mirroring

**Slice:** The Phase 7 mesh uses a real captured base texture and sampler, including correct sRGB/linear view intent and streamed mip residency.

**Red:** Test DXGI-to-Vulkan mappings for observed BC1–BC7 and uncompressed formats, typeless legal views, cube/array metadata, row/block pitches, sampler normalization, missing fallback resources, mip promotion/eviction, out-of-order streaming completion, and descriptor quarantine.

**Green:** Capture texture source metadata/payloads at load/read/finish boundaries, preserve compression, create explicit views, normalize/cache samplers, upload resident mips, publish descriptors after barriers, and supply canonical neutral resources.

**Refactor:** Unify format capabilities and color-space decisions in one table. Separate immutable texture description from mutable residency and descriptor publication.

**Gate:** Real diffuse, normal-data, two-channel mask, cube, and fallback fixtures sample correctly in replay; a captured textured mesh matches UV/address/filter fixtures; streamed mip changes never expose incomplete data; memory returns to baseline after eviction/unload.

### Phase 9 — Authored specular/smoothness material translation

**Slice:** A captured default lighting material is translated from engine semantics into the canonical specular/smoothness GGX record and renders through a shared material evaluator.

**Red:** Test source precedence and every provenance state; `_s` R/G decode under the semantic role; base-color view, alpha meaning, tangent/model-space normal selection, missing maps, UV transforms, scalar ranges, static/dynamic record offsets, shader reflection, and material revision without descriptor churn. Add controlled dielectric/painted-metal light sweeps.

**Green:** Implement `MaterialCapture`, `MaterialTranslator`, `GpuMaterialStatic/Dynamic`, bindless texture resolution, default-material shader evaluation, debug views, and versioned smoothness/specular transfer LUT hooks. Begin with the documented approximation while preserving raw authored values.

**Refactor:** Make raster and future hit shaders call the same generated material-decode functions. Move heuristics behind explicit policy and provenance; no default material may need inferred metalness.

**Gate:** Captured default materials replay with authored bindings and no silent heuristic replacement; C++/shader layouts agree exactly; raw channel/debug views match fixtures; highlight width/peak response meet the initial declared envelope; unknown material classes remain diagnostic and cannot arm Takeover.

### Phase 10 — Camera, view, frame, and pass capture

**Slice:** A captured engine camera and classified pass stream drive the replay camera so the Phase 9 mesh appears at the same screen location and depth as vanilla.

**Red:** Test matrix storage/order, handedness, projection modes, viewport/scissor, camera-relative origin, near/far extraction, jitter, current/previous transforms, world-to-screen fixtures, camera cuts, resize/FOV changes, and history-epoch increments. Feed NaN, singular, stale, and wrong-thread camera records.

**Green:** Validate and hook the mapped camera/accumulator boundaries, copy immutable `ViewPacket`/`FramePacket` data, classify pass/domain/technique IDs, and drive Vulkan view constants. Preserve original engine traversal and submission.

**Refactor:** Put coordinate conversion in one tested module and make temporal-reset causes explicit events rather than inferred backend side effects.

**Gate:** Known world points agree with vanilla screen/depth observations within source precision; current/previous matrices and jitter reproduce motion fixtures; every tested camera transition increments history exactly once; a real captured mesh overlays its vanilla position in split view.

**Current evidence:** The offline slice is complete. A deterministic
camera-relative synthetic packet traverses `.vfframe` serialization, ABI minor
5, reflected Vulkan binding 6, vertex projection, and the CPU oracle with
byte-identical Debug/Release artifacts and zero validation errors. The mapped
live camera/pass capture and real-mesh vanilla overlay portions of the gate
remain pending; see [`phases/phase-10.md`](phases/phase-10.md).

### Phase 11 — Opaque static raster scene mirror

**Slice:** Engine-generated packets for multiple opaque static objects render into a Vulkan G-buffer and compatibility lighting buffer while vanilla remains authoritative.

**Red:** Test pass-to-packet classification, draw range/material association, sort independence for opaque draws, depth equality/occlusion, normal encoding, material/object IDs, duplicate submission, and unknown target writers. Add a small multi-object replay golden with occlusion and intersecting bounds.

**Green:** Hook the validated batch/accumulator seam, build immutable instance/draw packets, render opaque static geometry, and expose albedo, geometric/shading normal, roughness, depth, object/material ID, and difference views.

**Refactor:** Separate engine pass accounting from backend draw construction. Replace per-draw allocations and descriptors with frame arenas, bindless tables, and grouped indirect work where tests preserve identity.

**Gate:** The synthetic and captured static scenes match transform, depth, ID, and material fixtures; every actual D3D draw touching the claimed opaque targets is associated with a Vulkan packet or a declared unsupported class; no unclassified frame can be armed.

**Current evidence:** The offline slice is complete. A deterministic
three-object synthetic scene traverses `.vfscene` serialization, ABI minor 6,
reflected storage binding 7, per-object transforms, four rasterization-ordered
G-buffer attachments, and the CPU attachment/identity/depth oracle with
byte-identical Debug/Release artifacts and zero validation errors. Full
occlusion, partial occlusion, submission-order independence, and fail-closed
rejection of unclassified world writers are demonstrated through the public
ABI. Engine-generated packets from the batch/accumulator seam and the vanilla
static-scene comparison remain pending; see
[`phases/phase-11.md`](phases/phase-11.md).

### Phase 12 — Instancing, streaming, generations, and cell lifecycle

**Slice:** Repeated meshes render as instances while cell attach/detach, mip/mesh streaming, address reuse, and deferred destruction update the mirrored scene without leaks or stale content.

**Red:** Test instance transforms/current-previous state, per-instance material data, stable object IDs, deduplication versus independently mutable resources, generation rollover, descriptor reuse quarantine, attach/detach ordering, late upload completion, cancellation, and in-flight unload.

**Green:** Implement `SceneDatabase`, instance tables, content-hash deduplication, cell/resource groups, upload cancellation, residency budgeting, and timeline-based retirement. Add registry delta records to traces.

**Refactor:** Unify resource and instance lifetime tickets, bound hash work, and remove engine pointers from delayed jobs. Keep descriptor index 0 permanently valid.

**Gate:** A settlement/cell-transition replay preserves all expected instances and releases removed groups; repeated load/unload/address-reuse cycles reach a stable memory/handle plateau; late completion cannot resurrect an old generation; instance motion IDs remain stable.

**Current evidence:** The offline slice is complete. `SceneDatabase` drives a
five-instance, two-cell fixture through deduplicated geometry, scene packet
version 1.1, reflected storage binding 9, GPU instancing, a cell transition,
and the CPU oracle with byte-identical Debug/Release artifacts and zero
validation errors. Generation exhaustion, descriptor quarantine, upload
cancellation, late-completion rejection, residency budgeting, registry delta
traces, and a repeated load/unload plateau are demonstrated. A live
settlement/cell-transition capture remains pending; see
[`phases/phase-12.md`](phases/phase-12.md).

### Phase 13 — Skinning, morphs, wind, and dynamic geometry

**Slice:** Characters, animated objects, dynamic tri-shapes, morphs, and wind-deformed geometry render with current and previous Vulkan positions.

**Red:** Test bone index/weight normalization, skin matrices, morph accumulation, dynamic buffer ranges, update ordering, previous-position retention, topology change, zero/invalid weights, bounds expansion, and compute/raster barriers. Compare synthetic CPU reference deformation with GPU output.

**Green:** Capture deformation inputs, implement compute skin/morph/wind kernels and dynamic ring uploads, update bounds, and feed deformed streams into the raster scene. Topology changes create generations; fixed topology updates in place.

**Refactor:** Share deformation descriptors across raster and later BLAS builds. Consolidate current/previous buffer ownership and remove per-object GPU allocation.

**Gate:** Reference vertices and bounds meet declared numeric tolerances; characters and animated fixtures align in split view; motion vectors have correct sign/magnitude; dynamic rings wrap safely under stress; validation reports all compute-to-graphics dependencies correct.

**Current evidence:** The offline slice is complete. A six-frame animated
fixture traverses `.vfdeform` serialization, ABI minor 7, a reflected compute
kernel, a compute-to-vertex-input barrier, deformed vertex rasterization, and
motion readback, agreeing with the CPU reference to 6e-08 with zero validation
errors and byte-identical Debug/Release artifacts. Topology generations, ring
wrap safety, and union bounds are demonstrated. The captured engine
deformation inputs and the vanilla split-view comparison remain pending; see
[`phases/phase-13.md`](phases/phase-13.md).

### Phase 14 — Terrain, landscape layers, and LOD

**Slice:** Exterior terrain and LOD geometry render with captured layer weights, UV scales, normals, noise, snow/wetness inputs, and stable transitions.

**Red:** Test landscape vertex decoding, layer-index/weight normalization, texture-array selection, normal blending, cell seams, LOD morph/blend ranges, negative world coordinates, camera-relative precision, missing layers, and load/unload boundaries. Add near/far seam fixtures.

**Green:** Implement terrain packet capture, layer/material tables, texture-array bindings, landscape shader evaluation, and LOD geometry selection based on captured engine decisions rather than a second guessed culler.

**Refactor:** Factor common layer sampling and material evaluation from terrain pipeline selection. Batch compatible cells without losing cell/material diagnostic identity.

**Gate:** The exterior fixture has no cracks, incorrect layer IDs, or camera-origin swimming; near/far transitions remain inside the parity envelope; terrain resources unload to baseline; unsupported landscape variants are classified before Mirror comparison.

### Phase 15 — Alpha-tested and two-sided visibility

**Slice:** Foliage, fences, hair cards, and other cutout/two-sided geometry match vanilla coverage in color and depth across distance and fade transitions.

**Red:** Test alpha-source selection, cutoff/reference, blend-versus-test classification, alpha-to-coverage if observed, dither/fade state, alpha-coverage-preserving mip thresholds, UV/clamp parity, front/back normal frames, negative determinant transforms, and depth-only versus color coverage.

**Green:** Implement the alpha-tested raster class, derived alpha-coverage mip metadata/cache, two-sided frame handling, and identical material decode in depth and G-buffer passes. Keep sorted transparency outside this phase.

**Refactor:** Centralize coverage evaluation for raster, depth, and future any-hit use. Make fade/dither inputs per-instance dynamic data rather than pipeline permutations.

**Gate:** Coverage masks agree within the declared pixel threshold over the mip-distance ramp; depth and color silhouettes match; two-sided lighting normals remain in the geometric hemisphere; dense foliage stress has bounded draw/resource growth.

### Phase 16 — Specialized opaque material families

**Slice:** Skin/face, hair, eye, snow/wetness, glow, environment, parallax/POM, multilayer, dismemberment, and relevant LOD material families render through explicit class translators.

**Red:** Create one fixture per concrete material class and test feature dispatch, overloaded texture slots, palette/tint, model-space normals, anisotropy, subsurface/rim/backlight, eye transforms, wetness dynamics, glow/emission, POM scale/bias/UV, and unknown-derived layouts. Test that bright RGB never becomes emission without the flag.

**Green:** Add class-specific `MaterialDesc` translators, broad shader classes, dynamic controls, POM hit adjustment, and diagnostic provenance. Use the general specular/smoothness evaluator wherever the class does not define a specialized lobe.

**Refactor:** Replace technique-permutation growth with feature data when branching cost is bounded. Keep water/effect/transparent records out of the lighting-material base path.

**Gate:** Every installed/captured standard opaque feature resolves to a known translator or explicit fallback; fixtures pass normal, coverage, tint, lobe, and emission checks; live wetness/controller changes update dynamic records without static descriptor churn.

### Phase 17 — Lights, sky, weather, fog, and raster parity

**Slice:** The complete mirrored opaque scene receives captured direct/ambient lighting, sky, weather, and fog in linear HDR, producing a useful vanilla-difference view before RT shadows exist.

**Red:** Test light type, transform, range, attenuation, cone/area parameters, color/intensity conversion, eligibility masks, stable IDs, ambient/interior state, sun/moon direction, sky resources, fog equations, weather transitions, world/time discontinuities, and deterministic light-list overflow policy.

**Green:** Capture lights/weather at the accumulator/shadow-scene boundaries, build the light list/cluster structure, implement compatibility direct and ambient evaluation, sky, and fog. Mark the shadow term explicitly unavailable in this phase so parity metrics can mask it rather than hiding an error.

**Refactor:** Separate captured source units from shader-ready values and isolate optional physical relighting profiles. Share environment evaluation with future reflection miss shaders.

**Gate:** Controlled single-light and environment sweeps meet the declared unshadowed parity envelope; interior/exterior and weather transitions select the correct state without stale lights; the complete opaque capture matrix renders in Mirror with zero unknown mandatory material or geometry class.

### Phase 18 — Acceleration structures and ray-traced shadows

**Slice:** The mirrored scene builds BLAS/TLAS generations and replaces the explicitly missing shadow term with ray-traced direct-light visibility, including alpha-tested geometry.

**Red:** Test build-size/alignment queries, scratch allocation, device-address stability, static compaction, update-versus-rebuild decisions, transformed bounds, instance masks/custom indices, negative-determinant transforms, geometry opacity flags, alpha candidate confirmation, ray bias, destruction in flight, and precise AS barriers. GPU fixtures cover opaque, cutout, two-sided, instanced, and animated occluders.

**Green:** Implement static/dynamic BLAS lifecycle, compaction, compute-deformation dependency, TLAS per view, instance/material indirection, and ray-query shadow evaluation. Reuse the Phase 15 coverage function for non-opaque candidates and the geometric normal for safe ray origins.

**Refactor:** Centralize AS scheduling and scratch/timeline retirement; keep SBT/hit-group diversity to broad classes. Remove any all-commands barriers introduced during bring-up once precise tests exist.

**Gate:** Synthetic and captured bounds/instance counts match; opaque and alpha shadow silhouettes meet coverage fixtures; animated updates show no stale pose; validation is clean; camera cuts/resizes never reference an old TLAS; the raster shadow-term mask is removed from parity comparisons.

### Phase 19 — Ray-traced reflections and ray-footprint LOD

**Slice:** Opaque materials produce one-bounce glossy reflections using authored `F0`/roughness, RT geometry, and captured sky/probe behavior.

**Red:** Test reflection direction and hemisphere rules, `F0`/roughness response, ray-cone growth and mip selection, miss/probe fallback, environment masks, two-sided surfaces, self-intersection, maximum distance, roughness performance cutoff, history reset, and deterministic sampling. Use mirror, rough dielectric, rough conductor-like, normal-map, and thin-gap fixtures.

**Green:** Add the reflection ray pipeline/query path, shared hit-material evaluator, ray footprint propagation, environment miss evaluation, raw reflection buffers, material-aware fallback, and reflection-specific temporal/spatial filtering.

**Refactor:** Share ray launch/hit/miss records with later GI while keeping histories and denoiser parameters independent. Move performance policy out of material semantics.

**Gate:** Reflection direction, hit identity, selected mip, and raw radiance pass deterministic fixtures; filtered reflections converge without persistent ghosting on the motion/camera-cut suite; rough surfaces retain a filtered specular lobe instead of dropping to black; no implicit derivatives are used in hit shaders.

### Phase 20 — Diffuse GI, temporal reconstruction, and denoising

**Slice:** Opaque scenes receive bounded one-bounce diffuse indirect light with stable temporal reconstruction and disocclusion handling.

**Red:** Test deterministic diffuse sampling, direct/indirect separation, emissive-hit behavior, radiance clamps, motion reprojection, depth/normal/object/material rejection, local disocclusion, camera/history epochs, dynamic resolution, half-resolution mapping, history length/moments, and stationary convergence. Add light-leak, thin-wall, moving-object, camera-cut, and emissive fixtures.

**Green:** Implement the quality-bounded GI ray pass, explicit light/environment sampling, raw radiance/moments, temporal accumulation, spatial filtering, and final reconstruction. Emissive importance sampling remains disabled until calibrated area/radiance data passes its own tests.

**Refactor:** Build reusable history-resource and reprojection services while retaining independent shadow, reflection, GI, exposure, and final histories. Separate correctness parameters from quality presets.

**Gate:** Fixed-seed raw GI is deterministic; stationary fixtures converge inside their variance envelope; moving/cut scenarios reject invalid history without long trails; no cross-history contamination occurs; quality changes do not alter semantic capture or resource identity.

### Phase 21 — Decals, sorted transparency, and particles

**Slice:** Decals, blood, fire/smoke, soft particles, and general blended geometry composite over the RT-lit opaque scene with correct ordering and temporal masks.

**Red:** Test blend factors/operations including premultiplied and additive modes, stable sort keys, depth test/write, stencil receiver masks, decal projection/range, soft-particle depth fade, dissolve/falloff, refraction-source selection, reactive mask, and unsupported custom effects. Include intersecting transparent layers and particle/decal fixtures.

**Green:** Capture transparent/effect packets, implement the classified raster/composite passes after opaque RT lighting, produce reactive/transparency masks, and retain vanilla fallback for unknown custom domains.

**Refactor:** Normalize blend/depth/stencil descriptions and share effect texture/material bindings without folding sorted draws into opaque batching. Make the composition order explicit in the frame graph.

**Gate:** Blend/sort/stencil and soft-depth fixtures match expected values; captured representative decals, blood, fire, smoke, and particles appear in the correct layer; every remaining transparent draw is Vulkan-covered or on the explicit compatibility ledger.

### Phase 22 — Water, glass, refraction, and underwater views

**Slice:** Dedicated water and glass paths render reflection, refraction, depth fog, animated normals, and underwater state without forcing them through the base lighting material.

**Red:** Test `BSWaterShaderMaterial` translation, three-normal animation/scroll, shallow/deep/fog/silt colors, reflection plane, Fresnel/sparkle controls, SSR/RT selection, refraction source/depth, shoreline depth, underwater transition, glass alpha/refraction flags, thickness/IOR policy, and missing transmission metadata.

**Green:** Implement the water packet/resource translator, hybrid raster/RT water pass, screen/RT refraction inputs, underwater fog/composition, and a conservative glass class. Unknown IOR/thickness uses documented class defaults or fallback, never pixel inference.

**Refactor:** Share reflection/refraction ray services while keeping simulation, water constants, and transparent sorting specialized. Isolate compatibility defaults in versioned policy data.

**Gate:** Shoreline, above/below-water, moving-water, window glass, eye/environment, and resize fixtures pass state and image tolerances; transitions clear affected histories; water/glass writes the correct depth/reactive data; no lighting-material base cast is used for water.

### Phase 23 — Vulkan image-space and post chain

**Slice:** Vulkan consumes the HDR world and reproduces the selected image-space graph through exposure, bloom, volumetrics, temporal reconstruction/upscale, motion blur where enabled, tone mapping, LUT/grading, and final-color conversion in Mirror.

**Red:** Test the complete image-space effect ledger, graph dependency/alias lifetimes, borrowed texture semantics, exposure adaptation/reset, bloom thresholds, volumetric history, motion-vector convention, TAA/upscale jitter, tone-map/LUT order, SDR/HDR format transforms, disabled-effect identity, resize, and unknown-effect classification.

**Green:** Translate the mapped image-space schedule into frame-graph passes, implement required post kernels, reuse the current VisualForge LUT/CAS behavior through explicit Vulkan equivalents, and produce pre-UI output plus per-effect debug/timing data. Vanilla post remains active for the displayed frame.

**Refactor:** Deduplicate transient-resource declarations and history handling, group only mathematically compatible passes, and keep the effect suppression ledger separate from effect implementation.

**Gate:** Synthetic and captured post fixtures pass effect-order and image tolerances; disabled chains are identity operations within format precision; every engine effect ID is Vulkan-covered, explicitly retained, or explicitly unsupported; unknown effects prevent arming rather than disappearing.

### Phase 24 — UI, video, middleware, and final bridge ordering

**Slice:** Vulkan world/post output enters D3D11 at the exact pre-Scaleform boundary, after which Scaleform, Bink, retained Flex/middleware layers, ImGui, and external overlays compose in a documented order.

**Red:** Test pre/post-UI target selection, premultiplied alpha, color-space conversion, viewport/letterbox, UI depth/stencil consumers, Bink frame changes, retained middleware writer classification, overlay ordering, resize/recreation, skipped Vulkan frame, and bridge-ticket failure. Use pixel-coded layer fixtures to prove order.

**Green:** Implement the pre-Scaleform handoff, D3D target alias/copy path, compatibility whitelist, UI/video composition, and correlation markers spanning both APIs. Give each retained middleware path an explicit owner and resource contract.

**Refactor:** Centralize cross-API layer ordering and bridge-target selection. Keep middleware adapters outside the Vulkan scene renderer and eliminate incidental Present-hook ordering assumptions.

**Gate:** Menus/HUD, loading screens, Bink playback, ImGui, Steam-style overlay, retained Flex cases, window modes, and repeated resize pass the pixel/stability matrix; every D3D draw after handoff is classified; a bridge failure yields a complete vanilla frame at the next safe boundary.

### Phase 25 — World-only Takeover and fail-open recovery

**Slice:** At `Renderer::Begin`, a health decision can suppress classified vanilla world submission for the whole frame while retaining the declared D3D depth/post/UI compatibility path.

**Red:** Test every arming predicate, atomic per-frame decision, unknown geometry/material/pass/target writer, incomplete resource generation, backend lag, fence timeout, Vulkan error before and after suppression, last-good-frame behavior, next-frame vanilla restoration, mode transition, save/load, and hook removal while work is in flight.

**Green:** Implement classified suppression at the accumulator/batch boundary, retained depth/stencil contract, scene-color handoff to vanilla post, takeover watchdog, last-completed output, and automatic fault transition. Takeover remains opt-in and capability/build-signature specific.

**Refactor:** Express the frame decision as an immutable `TakeoverPermit` containing all evidence and expiry. Remove feature-local suppression flags and route all faults through the central controller.

**Gate:** Interiors, exteriors, dense settlements, combat, first/third person, terrain, water/weather, loading, Pip-Boy, VATS, dialogue, maps, terminals, and power armor complete the world capture matrix with zero missing visible class or unknown world-target writer. Fault injection at every frame phase restores vanilla exactly as specified; no half-vanilla/half-Vulkan world frame is attempted.

### Phase 26 — World-and-post Takeover

**Slice:** Vulkan owns world rendering and image-space processing; D3D11 remains only for the Phase 24 compatibility composite and DXGI presentation.

**Red:** Test the suppression decision for every mapped image-space effect, borrowed-target acquire/return, depth/stencil handoff, exposure/history continuity, pre-Scaleform output, post failure, screenshot/capture behavior, resize, and a deliberately injected unknown image-space dispatch.

**Green:** Enable the complete, versioned image-space suppression ledger, Vulkan post output, exact pre-UI bridge, and remaining depth/stencil adapters. Expose compatibility API state and debug images to cooperating plugins.

**Refactor:** Remove Mirror-only duplicate scheduling while retaining replay and difference views. Tighten the D3D whitelist to named UI/video/middleware/bridge operations.

**Gate:** No vanilla world or image-space draw/dispatch remains outside the whitelist across the complete acceptance matrix; UI/video output is correct; zero validation errors, busy-waits, or routine device-idle calls occur; memory stabilizes after repeated save/load/resize; forcing any gate false before frame begin produces a complete vanilla frame.

### Phase 27 — Native WSI and compatibility-island retirement

**Slice:** An optional `Native` mode presents through a Vulkan Win32 swap chain and disables the real D3D world/presentation path only after every remaining engine/middleware reader has a native backend, imported layer, or deliberately scoped facade.

**Red:** Test swap-chain format/color-space selection, SDR/HDR transitions, present modes/frame pacing, acquire/present errors, resize/minimize/display change, full-screen policy, device loss, Vulkan ImGui/UI input, Scaleform/Bink/Flex ownership, overlay capability, and an audit fixture for every surviving D3D resource consumer.

**Green:** Implement Vulkan WSI, HDR metadata/color policy, pacing, native ImGui, the chosen Scaleform/UI and video/middleware paths, and compatibility facades required by proven engine readers. Keep `Takeover` as a fully supported fallback mode.

**Refactor:** Retire bridge and D3D state only when dependency tests prove no reader remains. Keep presentation policy independent from scene/frame-graph semantics.

**Gate:** With D3D world resources and DXGI presentation disabled, the full game/overlay/video/capture matrix passes without invalid COM/resource access; Native handles display and device faults predictably; switching the configuration back to Takeover requires no asset or save migration. If this gate is not economical, Phase 26 remains the production end-state by design.

## 6. Cross-phase regression rules

- A phase inherits every prior test. Later functionality may add capability-conditioned expectations but cannot erase an earlier contract.
- Packet and shader ABI changes are versioned migrations with old-fixture reader tests until the compatibility window is intentionally closed.
- The previous completed runtime mode is always the fallback oracle. Phase 19 failure falls back to the Phase 18 output; Phase 25 failure falls back to vanilla, not to an incompletely mixed frame.
- New content encountered in Observe is classified as supported, explicitly retained, or blocking. “Probably equivalent” is not a class.
- Performance work begins from captured counters and preserves the same correctness tests. Quality profiles may change ray counts and resolution, never semantic capture, IDs, ownership, or synchronization.
- Driver/vendor capability branches receive contract tests and at least one real-device result before being advertised. An untested branch reports unsupported.
- Test-only engine memory fixtures are bounded byte spans with checked reads; unit tests never dereference process addresses.
- GPU tests name every object and command region with phase, test, frame, and resource identity so validation output is actionable.
- In-game failures preserve logs/traces/dumps before cleanup. Soak tests report first divergence rather than only their final timeout.

## 7. Promotion evidence checklist

Each completed phase records:

```text
Phase and capability signature
Red test IDs and intended failure observations
Green/refactor revision identifiers
Unit / contract / replay / GPU / game / soak results
Vulkan validation and synchronization-validation result
Supported, retained, unknown, and blocking coverage counts
Golden or difference artifacts and declared tolerances
CPU/GPU time, allocation, residency, and handle deltas
Fault-injection cases exercised
Documentation/ABI/schema versions
Rollback or disable result
```

No artifact alone promotes a phase. A visually plausible screenshot cannot replace packet, lifecycle, validation, and fallback evidence.

## 8. Initial execution order

Implementation starts with Phase 1 only:

1. capture clean Debug/Release build commands, DLL exports, size/hash, and bounded current-plugin smoke behavior;
2. add CMake presets, CTest, pinned Catch2, and test labels;
3. add the failing target/export/default-mode contracts;
4. make them green with the smallest `vf_core` and health-report seam;
5. refactor target configuration and run the Phase 1 gate.

No Vulkan SDK integration, engine detour, or renderer source tree is introduced merely to appear ahead of this sequence. Phase 2 begins only from the recorded green Phase 1 baseline.

## 9. Target-system completion

The required production target is Phase 26: Vulkan owns the world and post chain, while the real D3D11/DXGI shell safely retains UI, video, middleware, and presentation. Phase 27 is an optional native end-state whose value must justify its compatibility cost.

The renderer is not considered complete because ray tracing is visible. It is complete only when semantic capture, resources, materials, scene coverage, RT effects, post, compatibility composition, lifecycle, synchronization, diagnostics, and deterministic fail-open behavior all satisfy their accumulated phase gates.
