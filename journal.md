# VisualForge development journal

This file is the authoritative resume checkpoint for an agent with no prior conversation context. Update it whenever a phase milestone, verification result, blocker, or safety condition changes.

## Current checkpoint

- Checkpoint time: 2026-08-16 06:10 +10:00 (Australia/Sydney).
- Repository: `F:\Development\fallout-mods\VisualForge`.
- Game root: `F:\SteamLibrary\steamapps\common\Fallout 4`.
- Objective: replace Fallout 4's existing renderer with a compatible Vulkan ray-tracing renderer, first mapping the existing renderer and then implementing/test-driving discrete vertical slices.
- Current implementation phase: all 27 offline CPU contracts complete; GPU verticals for 20-27 and live promotion for 8-27 outstanding. Phase 20 owes its temporal reconstruction and spatial filtering; Phase 21 owes its GPU composite pass; Phase 22 owes its GPU water/glass pass; Phase 23 owes its GPU post chain; Phase 24 owes its GPU handoff. Phases 8-19 are offline-complete with their live capture gates pending; Phase 16 additionally owes its installed-corpus sweep. Phases 18 and 19 also carry GPU verticals: ray-traced shadows and one-bounce reflections are drawn by the Vulkan backend and match the CPU oracle (269/269, Debug and Release byte-identical).
- Repository state: this directory is not a Git worktree. Preserve every existing file and change; do not assume reset/revert is available.
- Toolchain: Visual Studio 2022, `cdb.exe`, CMake presets, and Vulkan SDK `C:\VulkanSDK\1.3.268.0` are available.

## The Vulkan renderer draws in-engine, driven by the live world camera (2026-08-16)

The bridge pattern proves the transport; it does not prove the renderer. `-EnableMirror` (`VISUALFORGE_MIRROR`) now renders a 3D scene with `VulkanRasterRenderer` inside the running game, using the **live engine world camera's** view-projection, and presents it through the bridge.

```text
renderer-mirror: first-frame displayed extent=1280x720 camera-slot=0 near=15.000
  far=15000.0   cameras=1 validation-errors=0 suppression=off
renderer-mirror: camera changed  extent=1280x720 camera-slot=4 near=15.000
  far=353467.6  cameras=6 validation-errors=0 suppression=off
renderer-mirror: camera changed  extent=1280x720 camera-slot=0 near=15.000
  far=15000.0   cameras=3 validation-errors=0 suppression=off
```

The middle line is the settled world period: the mirror renders through **cache slot 4, far 353,467.6** — the world camera. The first line is the loading screen before the cache fills, the last is the quit teardown. The camera is chosen by measurement (largest far plane), reported whenever it changes, and never hardcoded to a slot.

The renderer's own output is written straight from the readback buffer via `VISUALFORGE_MIRROR_DUMP`, one file per camera slot. That is evidence a window screenshot cannot give: it does not depend on the game owning the foreground, and it names which camera produced it.

**The two frames are exactly what the two measured cameras predict, which confirms the camera identification independently of the projection arithmetic:**

| Frame | Camera | Measured basis | What Vulkan drew |
| --- | --- | --- | --- |
| `mirror-frame.slot0.ppm` | fov 24, far 15,000 | `forward=0,1,0` (unrotated) | The +Y red panel head-on, centred, filling the narrow frame |
| `mirror-frame.slot4.ppm` | fov 50.53, far 353,467.6 | `forward=0.707,0.707,0.038` (45 deg yaw) | Red (+Y) and green (+X) panels symmetric about centre, wider view |

A 45-degree yaw puts the forward and right panels either side of centre, and that is precisely what the world-camera frame shows. The geometry placement and the camera basis were derived independently and agree.

**What this proves:** the replacement renderer runs inside Fallout 4, transforms geometry by matrices read live out of the engine's own camera-state cache, rasterizes on the GPU, and puts the result on the player's screen, with zero validation errors.

**What this does not prove:** that the *engine's own scene* is what is drawn. The mirror draws its own camera-relative tiles, not captured world geometry. Capturing the live draw stream is the Phase 11-17 live work. Render suppression stays disabled.

Two defects found by live runs, both now pinned offline so they cannot cost another launch:

- **Raster packet validation classifies winding from the XY signed area alone.** World-space geometry standing upright has zero area in that plane and is refused as `DegenerateTriangle` however well formed it is in 3D. The mirror's tiles therefore lie in the ground plane. Pinned by `PM_camera_relative_ground_quads_encode_but_upright_ones_do_not`, which asserts the ground quad encodes *and* the upright one is refused.
- **The raster session needs the adapter LUID.** Creating it with a defaulted LUID fails with "raster device creation failed"; the probe's LUID is now kept and reused so the mirror lands on the adapter the game already presents from.

Also: every mirror gate now names itself in the log. The first attempt returned false from six different places and reported nothing, which is the same defect as `reason=encode` and cost a game launch to narrow down.

## Vulkan pixels reach the live screen (2026-08-16)

Verified in-engine. `-EnableBackend -EnableBridgePattern` creates a Vulkan device inside `Fallout4.exe` and presents Vulkan-rendered images into the live D3D11 swapchain:

```text
renderer-backend: ready device="NVIDIA GeForce RTX 4090" driver="NVIDIA" vendor=0x10DE
  device-id=0x2684 api=0x00404149 required=pass missing=0x0 bc=on d3d11-import=on
  d3d12-fence=on validation-errors=0 unload-policy=process-lifetime
renderer-bridge: ready extent=1280x720 format=R8G8B8A8_UNORM ring=3 epoch=1
  sync=d3d11-fence-timeline validation-errors=0
renderer-bridge: first-frame displayed release=1 ready=2 image=0 validation-errors=0
vulkanBackendReady=True vulkanPixelsDisplayed=True success=True
```

The screenshots prove it independently of the log: `after-captures.png` is a four-quadrant Vulkan test pattern filling the whole game window, **7,254 bytes**, against **2,154,500 bytes** for the real D3D11 game frame captured in the observation-only run minutes earlier. A flat four-colour image cannot be a Fallout 4 frame.

**What this proves:** a real Vulkan device on the real GPU inside the running game, images produced by it, and those images presented to the swapchain the player sees, synchronised through a D3D11 fence timeline with a three-deep ring and zero validation errors.

**What this does not prove:** that the *mirrored scene* is what is displayed. The bridge currently presents a test pattern, not the replacement renderer's G-buffer output. Wiring the mirrored scene through the bridge is the takeover work still gated on parity. Render suppression stays disabled.

**Correction to earlier runs in this session:** the first three live captures were observation-only — `backendRequested=False`, so every pixel on screen was the game's own D3D11 renderer and the plugin only read engine state. Camera evidence from those runs stands; none of it was Vulkan output.

Safety on the backend run: `forcedKill=False`, `quitRequested=True`, no new save files, no restore errors, plugin uninstalled afterwards, no process left running.

## The world camera is found (2026-08-16)

**Resolved.** The main world view lives in the camera-state cache array at `BSGraphics::State + 0x140`, which no capture had ever followed. `BSGraphics::State` is only `0x3C0` bytes: `0x160` is the **current** `CameraStateData` (`0x250`), and every previous capture read only that. Whatever camera the engine had set last is not the world view.

The four qwords at `+0x140` read `{heap pointer, 8, 5, 0}` — an array of capacity 8 holding **5** `CameraStateData` records **by value**, each with its camera at the documented `+0x050`/`+0x090`/`+0x0D0` offsets. Walking all five plus the embedded record yields six cameras:

```text
camera index=0 source=state view-offset=0x1B0 residual=0 near=15.000 far=15000.0   fov=24.00deg
camera index=1 source=cache view-offset=0x050 residual=0 near=15.000 far=15000.0   fov=24.00deg
camera index=2 source=cache view-offset=0x050 residual=0 near=15.000 far=15000.0   fov=24.00deg
camera index=3 source=cache view-offset=0x050 residual=0 near= 1.000 far=349526.3  fov=50.53deg
camera index=4 source=cache view-offset=0x050 residual=0 near=15.000 far=353467.6  fov=50.53deg
camera index=5 source=cache view-offset=0x050 residual=0 near=15.000 far=353467.6  fov=50.53deg
```

**Views 3-5 are the world camera.** Three independent facts agree, and none of them is a guess:

1. **The fov is arithmetically exact.** 50.534 degrees vertical is `2*atan(tan(40deg) / (16/9))` — precisely 80 degrees horizontal at 1280x720, the configured world FOV. The 24-degree cameras match nothing configured.
2. **The far plane is world scale.** ~353,468 units against 15,000 for the narrow cameras, with near 15 matching the logged `fNearDistance`.
3. **The basis is actually rotated.** Views 0-2 report `right=1,0,0 up=0,0,1 forward=0,1,0` — world-axis aligned, i.e. never oriented. Views 3-5 report `right=0.707115,-0.707099,0`, `forward=0.706577,0.706594,0.0383934`: a real 45-degree yaw with a slight pitch, which is a player facing direction.

The `position=0,0,0` that made earlier captures look wrong is **expected, not a defect**: the engine's view matrices are camera-relative and always carry a zero translation. Reading it as "the camera is at the origin" was an inference trap that helped hide the real problem for several sessions.

Verified end to end: the captured `.vfframe` replays through the Vulkan backend at the real game resolution with `view=captured source=camera-relative`, max channel error 1, mean 0.0097, probes and tolerance pass, and **zero validation errors**.

Two defects were fixed to get here, both recorded as reds:

- **Identity collided across records.** View identity was derived from the offset a camera was found at. Every cached record holds its camera at the same `+0x050`, so all five collided, the frame packet was refused for duplicate `viewId`, and the capture failed with `reason=encode` — a message naming the symptom, not the cause. `CameraScanResult::sourceSlot` now carries which record a camera came from and is mixed into both identities. Pinned by `P10L_cameras_from_separate_records_keep_separate_identities`.
- **The replay accepted only single-view captures.** `LoadFrame` required `views.size() == 1`, written when a capture could only hold one camera. It now takes `--frame-view <index>` and narrows the packet to that view and its passes; a single-view capture with the default index is unchanged byte for byte.

Reproduce:

```powershell
tools/game_smoke/Invoke-LiveCapture.ps1 -GameRoot "<install>" `
  -PluginDll out/build/vs2022-x64-release/Release/VisualForge.dll `
  -ArtifactDirectory artifacts/live-camera -StartingConsoleCommand "coc SanctuaryExt"
out/build/vs2022-x64-debug/Debug/vf_packet_replay.exe --render-synthetic `
  --backend out/build/vs2022-x64-debug/Debug/VisualForgeRenderer.dll `
  --output live-world-camera.ppm --frame <capture>.vfframe --frame-view 4 `
  --width 1280 --height 720 --validation
```

## Artifacts are reproducible, not archival

`/artifacts/` is gitignored and this tree is not a Git worktree, so the archive is **not durable** — the Phase 15 directory created earlier in a session was gone later in the same session, and Phase 7/10/14 directories referenced by the phase docs were never present at all. Do not treat a missing `artifacts/` subdirectory as evidence that a phase was not completed.

The SHA-256 values recorded in each phase doc are the durable evidence, and they are reproducible: rebuilding from clean and re-running the CTest regenerates byte-identical files. Verified on 2026-08-16 for all six recorded Phase 14 and Phase 15 hashes (`terrain-cells.vfterrain`, `terrain-debug.ppm`, `terrain-debug.vfgbuf`, `alpha-debug.ppm`, `alpha-debug.vfgbuf`, `alpha-debug.vfscene`) — every one reproduced exactly. Each phase doc carries the exact commands.

## Non-negotiable safety invariants

- The previously running user-owned `Fallout4.exe` (PID 67140) exited during this session, and the user then authorized automated live runs. Automated launches are therefore in scope; adopting a process this harness did not start is still forbidden.
- Every live run must go through `tools/game_smoke/Invoke-LiveCapture.ps1` or an equivalent that backs up and restores the installed plugin, the VisualForge logs, `Fallout4.ini`, `Fallout4Prefs.ini`, and `Fallout4Custom.ini`, and that refuses to start when a game process already exists.
- Runs must not create or modify save games. The harness disables autosave settings for the run and reports any new save file it observes; both live runs so far reported none.
- Never inject keyboard input without first confirming the game window owns the foreground. Injected keys go to whatever is focused, and console commands typed into another window would be a real accident. The guard works: when it fails, nothing is sent. What it cannot do is stop a *screenshot* from capturing whatever now covers the game, so screenshots must confirm foreground too.
- A hung game will hang the harness with it unless every Win32 path checks first. `Confirm-VfForeground` calls `AttachThreadInput`/`SetForegroundWindow` against the game's input queue; when that queue is stalled the call blocks indefinitely, so the harness never reaches its own `finally` and the user's INIs stay modified. Observed live: `Responding=False` at 22.5s CPU, harness wedged for over ten minutes, only freed by killing the game. `Test-VfGameResponsive` now gates foreground, input, and screenshots, and `Wait-VfLogMarker` abandons a wait after 180s of sustained unresponsiveness. The 180s grace matters: a cell load legitimately pumps no messages for a while, so a short stall must not abort a run.
- Never treat "input was injected" as "the game reacted". The attract screen accepts and ignores keys, so a run can report a dismissed menu while sitting on "press any key". Prove the world was reached by something the engine produced — a camera capture that actually found a camera — not by the fact that a key was sent.
- Do not terminate, suspend, attach a debugger to, inject into, relaunch, or otherwise adopt a user-owned process if one appears again.
- Re-enumerate Fallout 4 processes immediately before any live smoke and defer if one is running. Offline/unit/Vulkan replay tests are safe at any time.
- Render suppression remains disabled. Do not enable takeover/suppression until the documented parity and live promotion gates are satisfied.
- Live D3D11 capture hooks are one-shot and diagnostic only. They do not authorize interacting with the currently running process.
- Before any future live smoke, enumerate Fallout 4 processes and defer if one is already running.

## Phase status

The implementation plan contains 27 discrete, test-driven phases.

| Phase | Status | Resume meaning |
| --- | --- | --- |
| 1 | Complete | Project/toolchain/test scaffold. |
| 2 | Complete | Renderer observation and contract groundwork. |
| 3-5 | Complete | Early bridge/backend vertical slices. |
| 6-7 | Complete and promoted | Vulkan raster packet and real static-mesh replay are validated. |
| 8 | Offline implementation complete; live promotion pending | Texture packet, sampling, residency/quarantine, D3D11 one-shot capture hooks, and textured Vulkan replay are green. |
| 9 | Offline implementation complete; live promotion pending | Material transfer/model, shared GGX evaluation, bindless GPU records, material replay, and ABI extension are green. |
| 10 | Offline implementation complete; live promotion pending | View/frame/pass contracts, shader reflection, ABI, camera-relative Vulkan replay, artifacts, and Debug/Release regression are green. |
| 11 | Offline implementation complete; live promotion pending | Scene packet, pass accounting, per-object transforms, Vulkan G-buffer attachments, oracle comparison, artifacts, and Debug/Release regression are green. |
| 12 | Offline implementation complete; live promotion pending | Scene database, dedup, generations, quarantine, cell lifecycle, registry delta traces, instance packet, GPU instancing, artifacts, and Debug/Release regression are green. |
| 13 | Offline implementation complete; live promotion pending | Deformation packet, weight normalization, morph/wind/skin order, topology generations, dynamic ring, compute kernel, motion readback, artifacts, and Debug/Release regression are green. |
| 14 | Offline implementation complete; live promotion pending | Terrain packet, eight-channel land data, cell-owned slot mapping, cell-relative origins on all three axes, seam/LOD-seam detection, LOD morph, residency baseline, ABI minor 8, texture-array support, terrain pipeline, artifacts, and Debug/Release regression are green. |
| 15 | Offline implementation complete; live promotion pending | Visibility record, alpha classification, coverage contract, ordered dither fade, alpha-coverage-preserving mip scales, two-sided shading frames, negative-determinant winding/handedness, scene packet minor 1.2, GPU binding 13, depth prepass, EQUAL colour pass, per-draw dynamic cull/front-face/depth-compare, the stored-opacity rule, replay fixture, artifacts, and Debug/Release regression are green. |
| 16 | Offline implementation complete; live promotion pending | Family classification for all 21 lighting feature IDs, the full 64-bit property flag map, per-family slot roles including the overloaded role 7, the emission declaration rule, the normal-encoding rule and its orthonormal frame, LOD lobe suppression, eye/parallax/layer rejections, eight broad shader classes, the static/dynamic update split, the `.vffam` packet, ABI minor 9, the family pipeline, the HDR readback, replay fixture, artifacts, and Debug/Release regression are green. Corpus sweep outstanding. |
| 17 | Offline implementation complete; live promotion pending | `EngineLighting` is implemented and green (11 cases, 303 assertions): the RTTI-confirmed four-type light taxonomy with shadow casting as a flag, radius-clamped attenuation, cosine cones, colour/intensity separation, camera-relative double positions, interior sun suppression, non-blendable interior/exterior, captured-maximum fog, and deterministic reported light-list overflow. The shadow term is declared unavailable. |
| 18 | Offline implementation complete, GPU vertical included; live promotion pending | `EngineAcceleration` green (14 cases, 149 assertions): alignment-honouring build sizes, topology-forced rebuilds, static-only compaction, mirrored-but-not-singular instances, eight-corner transformed bounds, geometric-normal ray offsets, alpha candidates resolved through the Phase 15 coverage function, blended geometry excluded from occluders, and one shared rule for the ray a light casts. On the GPU: conditional ray-query enablement, a second SPIR-V variant of the family shader, a bottom level with one transformed geometry per drawn instance, an identity top level, precise build barriers, and a replay fixture whose shadows match `TraceShadowRay` over 41,981 interior pixels with zero mismatches. |
| 19 | Offline implementation complete, GPU vertical included; live promotion pending | `EngineReflection` green (10 cases, 581 assertions): dielectric/metal F0 separation, Schlick to one at grazing, exact mirror at zero roughness, below-surface samples rejected rather than clamped, deterministic per-pixel sampling, two-sided hit normals, ray-cone growth and absolute mip level, roughness cutoff as policy, captured-environment miss resolution with the interior light-leak rule, and epoch-scoped histories. On the GPU: a reflection traced against the Phase 18 top level, a geometry-to-object table at binding 18 so a hit can be shaded, hits shaded through the same function the raster pass uses including their own shadows, and a fixture whose reflection changes 13,489 pixels and matches the oracle across 41,981 interior pixels with zero mismatches. |
| 20 | CPU contract and one-bounce GPU vertical complete; temporal reconstruction, spatial filtering and live promotion pending | `EngineIndirect` green (6 cases, 1,088 assertions): cosine-weighted hemisphere sampling by Malley's method, direct/indirect separation that never goes negative, firefly clamping that bounds variance rather than biasing the mean, relative-depth reprojection with named rejection reasons, epoch-first invalidation, separate object and material identity tests, reset-not-blend on rejection, mean and second moment for a denoiser, and explicit half-resolution mapping that rounds up so an odd extent keeps its last row. Mutation-verified: a uniform hemisphere fails the mean-cosine check; blending a rejected sample fails the trail check. On the GPU: a diffuse bounce traced against the Phase 18 top level, hits shaded and shadowed through the same function the raster pass uses, and a fixture whose bounce changes 13,689 pixels and matches the oracle within a bounded 12 of 41,981 interior pixels. History resources, spatial filtering and motion-vector capture are outstanding. |
| 21 | GPU vertical in progress; CPU contract complete; GPU composite pass and live promotion pending | `EngineTransparency` green (8 cases, 67 assertions): straight, premultiplied, additive and multiply blends kept distinct, a sort key that is layered *and* total so two draws at one depth cannot swap between frames, blended geometry that tests depth and refuses to write it, view-space soft-particle fade, dissolve with a width rather than a bare threshold, decals bounded by receiver stencil, range, radius and facing, refraction reading the prior target, a reactive mask that accounts for additive radiance, and an explicit compatibility ledger naming every unsupported draw. Mutation-verified: dropping the sort tiebreak fails the total-order check; compositing premultiplied as straight alpha fails the blend check. |
| 22 | CPU contract complete; GPU water/glass pass and live promotion pending | `EngineWater` green (6 cases, 78 assertions): three independent scrolling normal layers summed as gradients so they combine into one surface rather than three fighting, depth-blended shallow/deep/silt colour that never continues past what was authored, shoreline fade, a reflection plane where a point on it is its own mirror, an underwater state that reports the crossing rather than leaving a consumer to detect it, Beer-Lambert fog that approaches the fog colour without overshooting, and reflection/refraction selection kept in policy so one material serves every device. **Transmission metadata is never inferred from the image**: a documented physical constant is used and the substitution is reported, an unknown class borrows nothing, and an impossible captured value is refused rather than clamped into plausibility. Mutation-verified both ways. |
| 23 | CPU contract complete; GPU post chain and live promotion pending | `EnginePostChain` green (7 cases, 727 assertions): an effect ledger where an **unknown** effect prevents arming while an explicitly unsupported one does not, a chain order that is checked rather than assumed and that a disabled effect still holds its place in, transient aliasing that refuses touching lifetimes and never aliases a borrowed image, borrowed images that may be read and never written, asymmetric exposure adaptation that resets outright on a cut, a bloom knee, Halton jitter that restarts on resize, a stated motion-vector convention, monotonic tone mapping before grading, an exactly-identity linear output transform, and a disabled effect that must be *exactly* the identity. Mutation-verified: letting unknown effects arm fails 2 assertions; letting touching lifetimes alias fails the aliasing check. |
| 24 | CPU contract complete; GPU handoff and live promotion pending | `EngineBridgeOrder` green (6 cases, 48 assertions): every layer after the handoff classified, with an **unclassified** draw preventing arming and a retained one required to name an owner; a checked composition order that keeps the world underneath and an external overlay on top; the handoff always to the pre-UI target, because the post-UI one puts the world over the menu; video replacing the world rather than blending with it; premultiplied output converted rather than assumed; colour-space mismatch refused; and aspect-preserving letterbox instead of a stretch that changes the field of view silently. **A missing ticket or an unproduced frame yields a whole vanilla frame, never a partial one** -- half a world over a menu is the state that gets reported as a crash. Mutation-verified: compositing without a ticket fails the fail-open check. |
| 25 | CPU contract complete; live world-capture matrix and promotion pending | `EngineTakeover` green (7 cases, 225 assertions). Thirteen arming predicates, each denying on its own and **accumulating** rather than short-circuiting, so one investigation names every reason instead of one rebuild per reason. The decision is an immutable `TakeoverPermit` carrying its evidence and expiring with its own frame: a second `BeginFrame` in the same frame returns the first answer whatever the evidence says by then, because two consumers disagreeing about whether the frame was taken over is exactly how a frame gets drawn half by each renderer. **Fault injection at all five frame phases x six fault kinds yields a whole frame every time** -- before suppression vanilla finishes the frame; after it those draws are gone and the engine has walked past them, so the frame is completed by the last completed Vulkan output, or by holding what the display already had when none exists. A fault latches out of takeover, and a recovery frame is spent only on a frame that was itself quiet, so a continuously broken backend cannot re-arm on schedule and alternate. Lifecycle events (mode transition, save/load, hook removal, device reset, resize) invalidate the permit immediately, not at the next frame boundary, and drop the last-good frame with it. `WorldCaptureMatrix` makes the live gate machine-checkable: observed is not covered, and a class with an unknown world-target writer or a missing visible class stays outstanding and named. Mutation-verified: restoring vanilla after suppression fails 5 assertions; re-deciding mid-frame fails the atomicity case. |
| 26 | CPU contract complete; GPU post ownership and live promotion pending | `EngineImageSpace` green (6 cases, 66 assertions). The image-space suppression ledger is **complete and versioned**, and completeness is checked before unknowns: an effect nobody listed is an effect nobody decided about, and a partial ledger reports zero unknowns precisely because the unmapped ones were never enumerated. Duplicates refused (that is how a ledger reaches the right count with the wrong contents); retained effects must name an owner; a newer ledger version is refused rather than silently accepted, because it describes frames this build cannot be compared against. Borrowed targets are returned in the frame that took them -- the engine reuses them next frame, so a late return releases a surface something else has already started drawing into -- and an outstanding borrow **blocks the next frame instead of being tidied away**, because tidying hides a leak that only appears as corruption under load. Depth handoff checks the reversed-Z convention, not just format and extent: depth written reversed and read as standard makes fog, depth of field and decals all wrong by an amount that reads as a bias setting. Exposure is adopted from vanilla across the handoff rather than restarted at a default; history is invalidated by extent, ledger version, upscale ratio or takeover epoch. A capture takes the final composite, not the pre-UI world, and not before the composite completes. Residual D3D is whitelisted by **named operation**, never by category -- a world draw and a UI draw are the same category of call. Mutation-verified: accepting a reversed-Z mismatch and accepting a partial ledger each fail their case. |
| 27 | CPU contract complete; native WSI implementation and live promotion pending | `EnginePresentation` green (8 cases, 140 assertions). Surface selection is deterministic, not first-available: first-available differs by driver, so the same build produces a different colour on two machines and reproduces on neither. HDR requires a capable display **and** a ten-bit-or-wider format -- eight bits stretched across a PQ curve bands every dark gradient and reads as a tone-mapping fault -- and it falls back to a whole SDR surface rather than a bad HDR one. FIFO is the only honest fallback (the specification guarantees it); mailbox for vsync without the latency; relaxed FIFO only when the caller has accepted a tear on a late frame. `Suboptimal` presents **then** recreates, because rebuilding before showing a presentable image drops a frame for a condition the specification calls acceptable; a lost surface rebuilds the surface, not just the chain, or the loop spins; and **minimized wins over every recreate except device loss**, because a zero-extent swap chain is invalid and a loop that keeps trying to build one is measured as the game hanging. Pacing yields and never spins or idles the device, asserted as a property across every input, and never claws back time after a late frame. Exclusive fullscreen is opt-in and yields to overlays. `AuditConsumers` retires the compatibility island only when every reader is attributed and resolved -- a reader nobody found keeps reading a resource nobody creates. Switching between Native and Takeover migrates nothing, which is what keeps the fallback usable. Mutation-verified: recreating while minimized fails 3 assertions; accepting 8-bit HDR10 fails 3. |

Live promotion of Phases 8-16 is no longer blocked on the camera: the world view was located on 2026-08-16 in the camera-state cache array and replays through the Vulkan backend with zero validation errors (see "The world camera is found" above). What each phase still needs is its own captured engine data — real alpha properties, real material families, real landscape cells — compared against the mirror.

## Established evidence and artifacts

### Phase 7: real static mesh

- Capture: `artifacts/phase-07/capture-20260815-204934/real-static-mesh.vfmesh`.
- Mesh: 289 vertices, 1536 indices, descriptor `0x000BB00605430208`, seven attributes.
- Reference PPM SHA-256: `22EC94D7C0F477D62A3A709B9C064EE21C28A036ECF56E3C2E35EA1AFC8CA693`.

### Phase 8: texture boundary

- `.vftex` uses a 200-byte pointer-free header.
- BC1-BC7 mappings are defined; BC1-BC5 have CPU decode coverage.
- sRGB transfer, samplers, fallbacks, residency, and quarantine are explicit.
- Raster packet v2 carries UVs and validates finite channels.
- D3D11 one-shot capture slots: `CreateTexture2D` 5, `CreateShaderResourceView` 7, `PSSetShaderResources` 8, `PSSetSamplers` 10.
- Capture variables: `VISUALFORGE_CAPTURE_TEXTURE_ONCE=1` and `VISUALFORGE_CAPTURE_TEXTURE_PATH=<path>`.
- Focused verification: 202 assertions across 16 cases.
- Historical full suite at completion: 106/106 Debug and Release.
- Offline synthetic replay: 1229 differing pixels, max error 4, mean error 0.129517, zero validation messages, accepted.
- Offline real-mesh replay: 3921 differing pixels, max error 4, mean error 0.276815, zero validation messages, accepted.
- Synthetic Debug/Release PPM SHA-256: `0995B678A4568BD8FCA2487A7ADB90DDB27E48FE036CDD634D2D41E480281A49`.
- Real-mesh PPM SHA-256: `3CF61E74376762A3CAEFFDF7C093F8929BBAFA072913EAF5A1554013065C0355`.
- Documentation: `docs/phases/phase-08.md`.

### Phase 9: material boundary

- `EngineMaterial` preserves provenance. Metalness is never inferred from supplied images.
- `_s` convention: red = specular, green = smoothness. Normal encoding is explicit.
- Raw and bounded values, fallbacks, UV transforms, and transfer version are preserved.
- Implemented transfer version is exactly `kMaterialTransferVersion = 1`; unsupported versions fail closed in encode, decode, evaluator, GPU-record construction, and backend submission.
- `MaterialPacketError::UnsupportedTransferVersion` and the test `phase9 material replay rejects unimplemented transfer versions` enforce this.
- Shared GGX evaluator drives oracle/shader parity.
- Shader material static record is 64 bytes, dynamic record 48 bytes; layout hash `0xF97A35789BC84031`.
- `.vfmat` header is 80 bytes, each record is 288 bytes, and a record embeds three `.vftex` payloads.
- Vulkan bindings 1-3 are textures; bindings 4-5 are static/dynamic material buffers; legacy binding 0 remains.
- Backend ABI Phase 9 minor was 4. Required request prefix was 80 bytes before Phase 10 appended fields.
- Real-mesh CLI honors `--render-mesh --material`; it must never silently ignore material input.
- Upload layout uses checked add/multiply arithmetic.
- `.vfmat` SHA-256: `A21657D4CF79EB9C8AF54B9F7755D1A836380FE770A66D783F3A0C1692EFAB36`.
- Material sphere Debug/Release SHA-256: `095894ACD90D221BD3508F2B4DB41E4FFE53848C55864E17DA70EB637E1E819C`.
- Synthetic Vulkan Debug/Release SHA-256: `EBE512B469C69D44F7EED23D46F1A92B9F06288C7D46DD948B6F79F0491891CE`.
- Real-mesh material Debug/Release SHA-256: `9B689F19DDB98CB84BDF3A9E4710C004125A767CCB9C56C6AB043A2876420764`.
- Synthetic Vulkan metrics: 1128 differing pixels, max error 3, mean error 0.0953776, probes/tolerance accepted, zero validation messages.
- Real-mesh material metrics: 1993 differing pixels, max error 4, mean error 0.269979, zero validation messages.
- Historical full suite at completion: 120/120 Debug and Release.
- Documentation: `docs/phases/phase-09.md`; rendering map section: `docs/engine_render.md` section 9.7.

## Phase 10 implementation already present

### Pure CPU contracts

New files:

- `src/renderer_core/EngineView.h`
- `src/renderer_core/EngineView.cpp`
- `tests/unit/EngineViewTests.cpp`

Implemented behavior:

- Normalizes source row/column matrix storage and row/column vector convention once.
- Builds/extracts left- and right-handed perspective and orthographic D3D zero-to-one projections.
- Applies exactly one explicit D3D-to-Vulkan Y conversion.
- Preserves camera-relative origin, current jitter, and previous jitter.
- Strictly rejects non-finite, inconsistent/invertibility-invalid, malformed projection/FOV/clip, viewport/scissor, flags, and reserved data.
- Pointer-free frame packet with magic `0x57564656` (bytes `VFVW`), 96-byte header, 640-byte view records, 40-byte pass records, CRC, section validation, zero padding, maximum 16 views and 65,536 passes.
- Pass domains/categories/classifier keep unknown world writers visible.
- `ViewHistoryTracker` resets exactly once per epoch transition and rejects stale/previous mismatch.
- `ProjectWorldPoint` and `BuildGpuViewConstants` are implemented.
- Deliberate red run: all eight Phase 10 cases failed, 82 assertions, before implementation.
- Green focused run: 147 assertions across eight Phase 10 cases.

### Shader and layout contract

New files:

- `shaders/phase10/view_transform.glsl`
- `shaders/phase10/view_layout.comp`
- `cmake/GenerateViewShaderLayout.cmake`
- generated `ViewShaderLayout.generated.h` in the build tree

Integration:

- Phase 10 reflection validates binding 6 as a 240-byte uniform block.
- `shaders/phase6/mesh.vert` includes `../phase10/view_transform.glsl` and calls `vfTransformPosition`.
- `vf_core` depends on Phase 6, 9, and 10 shader-generation targets.
- GPU record layout: current VP rows 0, previous VP 64, unjittered VP 128, clip+jitter 192, viewport 208, identifiers 224.
- GPU record size 240, binding 6, layout hash `0xC34D6F6AB1E8B527`.
- `identifiers[0]` is the enabled flag. Legacy submissions receive identity matrices and disabled transform; captured view submissions receive the encoded record and enabled transform.

### Backend ABI and Vulkan upload

- `src/renderer_api/BackendAbi.h` now defines `kBackendAbiPhase10Minor = 5`; current ABI minor is 5.
- `RasterFrameRequestV1` appends `frameData` at offset 80 and `frameSize` at offset 88; current request size is 96.
- `kRasterFrameRequestV1FrameRequiredSize` identifies the Phase 10 prefix.
- Historical prefix tests retain Phase 8 texture requirement 64 and Phase 9 material requirement 80.
- `tests/unit/BackendContractTests.cpp` contains the Phase 10 offset/size contract.
- `VulkanRasterRenderer.cpp` accepts a maximum 16 MiB frame packet, parses only when the request prefix reaches 96 bytes, requires exactly one view, matches frame ID/output/viewport/scissor to the raster request, and fails closed otherwise.
- Upload allocation appends an aligned 240-byte view record after material records and before textures, copies it, and updates binding 6.
- Descriptor set has seven bindings; binding 6 is a regular vertex-stage uniform buffer. The descriptor pool includes the uniform type.
- Renderer reset clears the view state. Debug labels identify the Phase 10 path.
- Whole Debug build succeeded after these changes.
- Legacy Phase 6/8/9 Vulkan GPU contracts were rerun after the shader/backend integration and all three passed with zero validation messages.

### CPU replay oracle

- `RasterGolden.h/.cpp` now provides `ProjectPacketForView(packet, view, projected)`.
- It validates exact output extent/viewport/scissor agreement and projects each camera-relative vertex through the view-projection matrix into NDC before invoking the existing raster oracle.
- `vf_packet_replay` is wired to it through `--frame`, `--view-fixture`, and `--frame-output`.
- The fixture unprojects desired NDC vertices into camera-relative source positions, re-encodes the raster packet, and projects only the CPU-oracle copy. The Vulkan vertex shader must therefore consume binding 6 to pass.
- Ambiguous `--fixtures` plus frame options fail at parse time. Normalized captured-mesh replay explicitly rejects frame transforms instead of ignoring them.

### Phase 10 final verification

- Red integration gate: `contract.view_raster_frame` failed with CLI usage before frame options existed.
- Focused green: 155 assertions in nine Phase 10 cases; the validation-enabled GPU contract passes.
- Full Debug: 130/130 CTest tests passed.
- Full Release: 130/130 CTest tests passed.
- GPU replay metrics in both configurations: 240 differing pixels, maximum channel error 1, mean absolute error 0.0102946, probes/tolerance pass, zero validation errors.
- Debug and Release PPM SHA-256: `812ECC0D640EB86542BE310CCEF988C4FE60446523EB910AE711C44EDF56F426`.
- Debug and Release `.vfframe` SHA-256: `E6E556D46F6F167773AC9AA4CB7419ED8E213AD7AAE1F5159105DE2677FA0DED`.
- PNG SHA-256: `AB127BAFCEC06382BE1E04DA8013F73465B78CC676F006002165D6EE0B7835AE`.
- Reloading the saved `.vfframe` produced a byte-identical PPM.
- The PNG was visually inspected and contains the expected intact color-interpolated triangle on the dark clear color.
- Artifacts: `artifacts/phase-10/`.
- Documentation: `docs/phases/phase-10.md`, `docs/engine_render.md` section 7.8, and the status block in `docs/vulkan_renderer_implementation_plan.md`.

## Phase 11: opaque static scene mirror

- `.vfscene` magic `0x43534656` (`VFSC`), 96-byte header, 224-byte objects, CRC over payload, at most 65,536 objects.
- `OpaqueObjectV1` fields: object/material identity, `drawIndex`, `passSequence`, flags, roughness, model, previousModel, bounds, geometric normal, shading normal. GPU record size 224, storage binding 7.
- `passSequence` is 32-bit by design. A wider engine pass sequence can never match, so it stays unmirrored instead of aliasing another pass.
- `ValidateSceneAgainstRaster` enforces an object/draw bijection and exact material association. `ValidateSceneAgainstFrame` enforces pass accounting and fills `SceneCoverage`; `MirrorEligible()` is the arming predicate.
- Errors added: `UnknownPass`, `PassClassMismatch`, `UnclassifiedWorldWriter`, `UncoveredPass`.
- Backend ABI minor 6: `sceneData` 96, `sceneSize` 104, `gbufferData` 112, `gbufferCapacity` 120; request size 128.
- The G-buffer is four rasterization-ordered color attachments (`R32G32B32A32_SFLOAT` x3 plus `R32G32B32A32_UINT`), copied to a readback buffer and interleaved into the 64-byte `GBufferPixelV1` on copy-out. Planes are created lazily on first scene submission and released with the extent.
- Replay CLI: `vf_packet_replay --render-scene --backend <dll> --output <ppm> [--scene-output <vfscene>] [--gbuffer-output <vfgbuf>] [--validation]`. CTest name: `contract.scene_raster_frame`.
- Focused green: 7,031 assertions in six Phase 11 cases. Full Debug 137/137, full Release 137/137.
- Replay metrics, identical in Debug and Release: 1,316 differing pixels (all covered), 0 identity mismatches, max error 4.2364e-05, mean error 5.51457e-07, 5,316 interior pixels with 0 mismatches, near 968, occluded 0, rotated 348, order-independent, rejections pass, 0 validation errors.
- `.vfscene` SHA-256: `AEEE65B1F320DE92D44312E776BDEA28898E99A58153411C82C4B8822F530CEC`.
- PPM SHA-256: `B0B98F27EF4D6B724637A37BACA4431603EB327603A6C42B279556FC83782428`.
- `.vfgbuf` SHA-256: `430372FF565F9ED2A3F3D90D055A97A043BD2B3EF9D1DB7387BD2B141AE3CC7C`.
- Artifacts: `artifacts/phase-11/`. Documentation: `docs/phases/phase-11.md`; rendering map section: `docs/engine_render.md` section 9.8.

## Phase 12: instancing, streaming, generations, and cell lifecycle

- `SceneDatabase` (`src/renderer_core/SceneDatabase.h/.cpp`) composes `ResourceRegistry` (geometry generations/retirement) and `DescriptorQuarantine` (descriptor reuse). Descriptor index 0 stays permanently reserved, so the quarantine is constructed one larger than the instance capacity.
- Instance identity: `objectId = (generation << 32) | (slot + 1)`. Stable while attached, always different after slot reuse.
- Dedup: immutable content hashes fold distinct addresses into one canonical geometry (alias entries own no bytes and no upload). Dynamic resources never fold.
- `ResourceRegistry` now takes a generation limit and returns `GenerationExhausted` instead of wrapping. `SceneDatabase` applies the same rule to instance slots.
- Errors added: `DatabaseError` set including `GeometryRetiring`, `BudgetExceeded`, `CapacityExceeded`, `UploadNotPending`, `GenerationExhausted`.
- `DescriptorQuarantine::Release` returns a slot acquired but never published to the GPU without a quarantine round trip.
- Trace: `RecordType::RegistryDelta = 8`, 56-byte `RegistryDelta`, `RegistryDeltaKind` ordinals static_asserted against `DeltaKind`. `TraceSummary::registryDeltaCount` and a `registry-deltas=` field in `FormatTraceSummary` (this changed the Phase 3 golden summary string; the golden trace bytes did not change).
- Scene packet version 1.1: 160-byte `InstanceV1` records, contiguous run per object, `instanceCount`/`instancesOffset` in the header. A scene without instances still encodes as version 1.0, which is why the Phase 11 artifacts are still byte-identical.
- GPU: reflected storage binding 9 holds instances; the push constant carries `objectIndex` and `firstInstance`; one `vkCmdDrawIndexed` per object with `instanceCount`.
- Replay CLI: `vf_packet_replay --render-instanced-scene --backend <dll> --output <ppm> [--scene-output <vfscene>] [--gbuffer-output <vfgbuf>] [--trace-output <vftrace>] [--validation]`. CTest name: `contract.instanced_scene_frame`.
- Focused green: 825 assertions in fourteen Phase 12 cases. Full Debug 152/152, full Release 152/152.
- Replay metrics, identical in Debug and Release: 5 instances from 2 objects/2 draws, 2 resident geometries, 3 shared instances, 8192 resident bytes, 5 visible before and 3 after the cell transition, 1 released geometry, 1 silhouette identity mismatch, 0 interior mismatches of 5199, 52 registry deltas, stable plateau, 0 validation errors.
- `.vfscene` SHA-256: `ADD9C3FDCD3699E650D19FC953314F40EAC2C8666761F50135A8C9DCB4ABE997`.
- PPM SHA-256: `7495F6C88884480F7A579781ED814D95D7DD5A2BAB32D066DEF40349FB16C4E1`.
- `.vfgbuf` SHA-256: `A68D118E879E787CC9700F5F5CFFB308F119387A0F0BE003E28AAA663724B307`.
- `.vftrace` SHA-256: `D754774CB67E1F2E847E4F0F1A9C492A27A9FFED9CF30AD18927847F1A940D07`.
- Artifacts: `artifacts/phase-12/`. Documentation: `docs/phases/phase-12.md`; rendering map section: `docs/engine_render.md` section 9.9.

## INCIDENT 2026-08-16 01:26: Steam repaired Fallout 4 and F4SE is gone

Stop and read this before starting another live run.

- At 01:26:44-48, after four automated launches, Steam committed an update to AppID 377160: `22 updated, 0 moved, 0 deleted files`, same `BuildID 22848228`. `Fallout4.exe` is still version 1.11.221.0, so this was a file repair, not a version change. Every stock file now carries a 01:26 creation time.
- `f4se_loader.exe` and `f4se_1_11_221.dll` are no longer in the game root. Also gone: `Data\F4SE\Plugins\Addictol.*`, `VisualForge.dll`, `VisualForge.ini`, `LUTs\`, `version-1-11-221-0.bin`, `Fallout4.pdb`, `msdia140.dll`, and the root `VisualForge\` and `FlexRevive\` folders. `FlexRevive.dll/.ini/.pdb` survived.
- Steam's own log claims it deleted nothing, so the removal is not fully explained by that log. What is certain is that the automation launched the game four times and force-killed it whenever the console `qqq` did not take, and that repeated abnormal termination is a plausible trigger for Steam deciding to repair.
- The harness only ever writes or restores `VisualForge.dll`, `VisualForgeRenderer.dll`, and the three INIs, and its restore only removes a file it created, so it did not remove F4SE or the other plugins.
- Saves were never touched and no run created one. The Documents INIs were restored correctly.
- Recoverable from `artifacts/live/*/backup/00-VisualForge.dll`: the user's original 975,360-byte plugin. F4SE itself is not recoverable locally and has to be reinstalled by the user.
- Prefer a clean quit over a force kill: if `qqq` fails, close the window and wait rather than `Stop-Process -Force`. The harness now escalates over 45 s then 30 s and records `forcedKill` in `result.json`, and it fingerprints `f4se_loader.exe`, `f4se_1_11_221.dll`, `Fallout4.exe`, and the plugin directory before and after every run so an install change is reported immediately.

### Address Library restored, 2026-08-16 07:17 — live hooks and Vulkan backend confirmed

The user placed `version-1-11-221-0.bin` (10,425,304 bytes, SHA-256 `2FDCC2A0926659C37255D2EAF335775240EC7FEFDF6CB3B35B063FACA25A448F`) into `Data\F4SE\Plugins`. `Fallout4.exe` verified byte-identical to expected (`428F9996CC4248E26C0F62F9FDD3EAF0E5EB305834B67EE5996538E593218B61`). Confirmed live in run `artifacts/live/20260816-071727-camera-provenance`:

- `build-gate: accepted`; `hook-manifest: accepted validated=6 failed-site=0 reason=none`.
- `renderer-backend: ready device="NVIDIA GeForce RTX 4090" ... required=pass missing=0x0 validation-errors=0`.
- `renderer-health schema=1 mode=Mirror backend=loaded suppression=off`.
- `renderer-bridge: ready extent=1280x720 ... sync=d3d11-fence-timeline` then `first-frame displayed`.
- Real engine texture captured: 2048x1024, 12 mips, resource/view format 78, 12 subresources, 2,796,824 bytes, anisotropy 16, ps-slot 0. A `.vftrace` from the same run decodes cleanly (`records=7 frames=1 views=1 writers=2`).

Two live-run defects found and fixed in the harness, both of which had been producing misleading evidence:

1. `attractDismissed` was set from "the keypress was injected", not "the menu advanced". The game sat on "press any key" while the harness sent `coc SanctuaryExt` into it and then scanned for a camera in a world that had never loaded (`renderer-camera-capture: rejected reason=no-camera-found attempts=240 candidates=0`). It now presses repeatedly (`-AttractKeyPresses`, default 8) and proves the world was reached only by an actual camera find (`worldReached`), aborting loudly otherwise.
2. Screenshots are screen scrapes of the game's client rectangle, so when another window covered it the harness saved a picture of that window and recorded it as run evidence. One such image was a VS Code window. `Save-VfWindowImage` now refuses unless the game owns the foreground. No stray input ever reached the other window: `Invoke-VfConsoleCommand` and `Invoke-VfKeyPress` both return without sending anything when `Confirm-VfForeground` fails, which is exactly what happened.
3. The frame-capture completion marker was missing from the harness switch, so a `frame` capture could never be marked complete. Both the inline wait and the completion loop now match `renderer-camera-capture: complete path=<escaped target>`; a generic marker would match the previous capture's line because the log is rescanned from the start each poll.

Also observed: with `-EnableBackend -EnableBridgePattern` the run stalled during the cell load and the backend logged `UNASSIGNED-VkQueue-state-timeout`. The bridge's fence-timeline wait does not tolerate a heavy cell load. Run live captures without the bridge until that is addressed.

Screenshot geometry is unreliable: `GetClientRect` reports the 1280x720 swapchain while the window occupies roughly 600px of screen width, so captures include whatever sits beside the game. Treat screenshots as a rough state indicator, never as framebuffer evidence.

### Post-reinstall state, 2026-08-16 01:42

- The user reinstalled the game. F4SE 0.6.23 is back and `Fallout4.exe` is byte-identical to the expected fingerprint: 55,293,864 bytes, SHA-256 `428F9996CC4248E26C0F62F9FDD3EAF0E5EB305834B67EE5996538E593218B61`.
- **Live capture is blocked on one missing file.** `Data\F4SE\Plugins\version-1-11-221-0.bin` (Address Library for F4SE Plugins, 1.11.221) is absent: expected 10,425,304 bytes, SHA-256 `2FDCC2A0926659C37255D2EAF335775240EC7FEFDF6CB3B35B063FACA25A448F`. No copy exists anywhere on disk.
- Without it the probe returns an empty fingerprint, so `build-gate: rejected` lists every field including `executable-sha256`. That list is a consequence of the early bail-out, not evidence that the executable is wrong. Do not go hunting for an executable problem.
- Consequence: mesh capture, camera capture, and the frame trace all stay inert because they require a validated build. Texture capture still works. This is the gate behaving correctly.

## Live capture: what actually runs in the game today

Read this before claiming anything about "our renderer" in a live frame.

- Normal live runs are observation only: `renderer-health schema=1 mode=Observe backend=absent suppression=off`. Every pixel on screen is the game's own D3D11 renderer. The plugin hooks Present, captures, and draws its ImGui overlay; it draws no world content.
- With the backend DLL installed and `VISUALFORGE_BACKEND_PROBE=1`, the Vulkan backend loads inside Fallout 4 and health moves to `mode=Mirror backend=loaded suppression=off`.
- With `VISUALFORGE_BRIDGE_PATTERN=1`, the Vulkan device produces an image, hands it to D3D11 through the shared-texture and fence-timeline bridge, and it is composited into the live swapchain (`renderer-bridge: first-frame displayed`). The pixels are a CPU-filled four-quadrant conformance pattern copied by Vulkan, not a Vulkan-rasterized scene, and it covers the whole frame.
- The Vulkan rasterizer that draws G-buffers, instanced scenes, and deformed meshes has only ever run in the offline replay harness. It has never drawn the game's scene inside the game.
- Live capture control: set `VISUALFORGE_CAPTURE_REQUEST=<file>`; the plugin polls that file every 15 presented frames, and a request arms one capture. Format is `sequence=`, `kind=`, `path=`. Sequences only move forward, paths must be absolute with the extension owned by the kind, and every outcome is logged as `renderer-capture-request: ... result=complete|rejected`. Implemented kinds: `mesh`, `texture`, `trace`, `frame`. `scene` and `deformation` report `kind-not-implemented`.
- Arm one-shot captures *before* the action that creates content. Meshes and textures are created during a cell load, so arming after the load can wait forever. Per-frame captures (`trace`, `frame`) are armed after the load instead.
- `Enabled()` on the trace means "a trace is recording right now", not "installed". Do not use it as an arming precondition; `ArmTrace` owns that decision.
- Harness: `tools/game_smoke/Invoke-LiveCapture.ps1` installs the plugin, forces windowed 1280x720, disables autosaves, launches through F4SE, dismisses the attract screen, travels with `coc`, arms captures around the load, screenshots the window, quits with `qqq`, and restores every file it touched. It refuses to adopt a game process it did not start, and it verifies the game window is focused before injecting any input.
- Live findings so far: in-world texture capture works (512x512 BC3 sRGB, 10 mips, anisotropy 16, bound at PS slot 9); the mesh capture only ever completed on the menu-path TriShape boundary (289 vertices, descriptor `0x000BB00605430208`) and produced nothing during a world load, so the world creation path is not covered by the hooked concrete boundary yet; in-world frame traces record 1 frame, 1 view, 2 writers (3 with the bridge composite active).
- Live camera capture works. `CameraStateScan` locates the matrices inside `CameraStateData` by the only relationship that cannot happen by chance, `viewProjection == projection * view`, so nothing depends on a guessed field offset. Observed live: view at `+0x050`, projection at `+0x090`, view-projection at `+0x0D0` inside the camera record, three consecutive 64-byte matrices, **column-major** storage, residual exactly 0, near 40.0, far 396.2, extent 1280x720. Those offsets corroborate the documented `ViewData` order with an 0x50-byte header.
- The captured `.vfframe` replays through the Vulkan backend at the real game resolution: `view=captured source=camera-relative`, max channel error 1, mean 0.0097, probes and tolerance pass, zero validation errors.
- Open question on that capture: near 40 / far 396 is a short range and is more characteristic of a first-person or secondary camera than the main world view. The capture is a real, self-consistent engine camera; that it is the *primary world* camera is not yet confirmed. Correlate across frames and against `NiCamera::WorldPtToScreenPt3` before claiming the Phase 10 live gate.
- Live-captured mesh plus live-captured texture replay through the Vulkan backend with 291 differing pixels, max error 7, mean error 0.0139, and zero validation errors.
- `WeaponDebris` and `EngineTextureCapture` both want `CreateShaderResourceView`; MinHook gives it to whoever installs first and the loser logs a failure. Capture hooks are only installed when the capture env vars are set, so normal play is unaffected, but a capture run silently disables the debris hook.

## Phase 13: skinning, morphs, wind, and dynamic geometry

- `EngineDeformation` (`src/renderer_core/EngineDeformation.h/.cpp`): `.vfdeform` magic `0x46444656` (`VFDF`), 144-byte header, 48-byte `DeformVertexV1` (4 influences + flexibility), 48-byte `BoneTransformV1` (3x4 affine), 16-byte `MorphTargetV1`, 32-byte sparse `MorphDeltaV1`, 32-byte wind block in the header.
- Evaluation order is morph (bind space) → skin → wind, for both the current and previous pose. Weights are normalized explicitly and `DeformationResult::normalizedVertices` reports how many needed it.
- `DeformationResult::bounds` is the union of both poses; `motionMagnitude` is the maximum per-vertex displacement.
- `TopologyRegistry`: fixed topology updates in place; changed vertex/bone/delta counts require a new generation; generations never move backwards.
- `DynamicRing(capacity, alignment)`: `Allocate` wraps only into retired space and refuses overlapping in-flight ranges. `Retire(completedValue)` frees every allocation with `timelineValue <= completedValue`.
- Backend ABI minor 7: `deformationData` 128, `deformationSize` 136, `deformationOutputData` 144, `deformationOutputCapacity` 152; request size 160. Output records are 32 bytes: current position then previous position.
- GPU: compute kernel `shaders/phase13/deform.comp` over eight storage bindings, 64-vertex workgroups, dispatched before the render pass; barrier COMPUTE/SHADER_STORAGE_WRITE → VERTEX_ATTRIBUTE_INPUT/VERTEX_ATTRIBUTE_READ and TRANSFER_READ; the raster pass binds the compute output as its vertex buffer.
- Replay CLI: `vf_packet_replay --render-deformed-scene --backend <dll> --output <ppm> [--deform-output <vfdeform>] [--gbuffer-output <vfgbuf>] [--frames N] [--validation]`. CTest name: `contract.deformed_scene_frame`.
- Focused green: 69 assertions in seven Phase 13 cases. Full Debug 160/160, full Release 160/160.
- Replay metrics, identical in Debug and Release: 6 frames, max position error 5.96e-08, max motion error 5.96e-08, reference motion 0.100304, 0 identity mismatches, 0 interior mismatches of 5496, topology change rejected, new generation accepted, 7 submissions, 0 validation errors.
- `.vfdeform` SHA-256: `AAC67C47CE9D8C21FFB7FA50C90FB7A2BD71EED78448B42832809780C3702EAB`.
- PPM SHA-256: `7E1427DE4737DCDA1371ACD188389202143D366290D88EE590A819EFD5818CD2`.
- `.vfgbuf` SHA-256: `CCF50E281BCCB9DD28A402FD4144F32C8417D135E02991996C9C652DD006FB25`.
- Artifacts: `artifacts/phase-13/`. Documentation: `docs/phases/phase-13.md`; rendering map section: `docs/engine_render.md` section 9.10.

## Phase 14: terrain, landscape layers, and LOD

- `EngineTerrain` (`src/renderer_core/EngineTerrain.h/.cpp`): `.vfterrain` magic `0x52544656` (`VFTR`), 112-byte header, 32-byte `LandscapeLayerV1`, 128-byte `TerrainCellV1`, 80-byte `LandscapeVertexV1`, 32-bit index stream. Sections in order: layers, cells, vertices, indices.
- Eight blend channels per vertex, because `EngineVertex` decodes landscape data as two UNorm byte quads (`Landscape0`/`Landscape1`). The slot-to-layer mapping lives on the **cell** (`layerSlots[8]`, `layerSlotCount`), not on the vertex.
- Channels at slots beyond `layerSlotCount` must be zero → `UnclassifiedLandChannel`. A slot pointing outside the layer table → `MissingLayer`. Two slots resolving to the same layer → `LayerSlotMismatch`. Two layers sharing an `arraySlice` → `InvalidLayer`.
- All three axes are cell relative: `originX`/`originY`/`originZ` are doubles, vertex positions are floats bounded to one cell and to `boundsMinimumZ/MaximumZ`. An absolute world coordinate in a vertex → `VertexOutOfCell`. Non-LOD cells must sit exactly on the 4096-unit grid.
- Cells tile their vertex/index sections contiguously; a cell's indices may only reference its own vertices → `IndexOutOfCell`.
- `EvaluateTerrain` reports `seamChecks`, `seamMismatches`, `maximumSeamGap`, `lodSeamChecks`, `lodSeamMismatches`, `normalizedVertices`, and `maximumLocalMagnitude`. Seam comparison runs on exact double world positions, not on the float stream.
- `LodBlend(cell, distance)` is monotonic and clamped; inverted or negative morph ranges → `InvalidLod`. The engine's `lodLevel` is recorded, never recomputed.
- `TerrainResidency`: `Load` replaces a resident cell's footprint rather than accumulating; `Unload` of an unknown cell → `UnknownCell`; `ResidentBytes()` returns to zero and `PeakBytes()` is retained.
- `BlendLayers` (private to `EngineTerrain.cpp`) is the single place captured channels become a normalized blend; the evaluator and the reference rasterizer both call it, and it mirrors the fragment shader's guarded division by `1e-6`.
- Backend ABI minor 8: `terrainData` 160, `terrainSize` 168; request size 176. A terrain frame requires a captured view, must match its frame/view ids, and carries its landscape layer array in the captured-texture slot (so it cannot also carry a material bundle).
- GPU: `shaders/phase14/terrain.vert|frag`, bindings 10 (derived cell records, 64 bytes), 11 (captured layer table verbatim, 32 bytes), 12 (`sampler2DArray`). Sizes and bindings are asserted from SPIR-V reflection via `cmake/GenerateTerrainShaderLayout.cmake`. Terrain writes the same five attachments and shares the depth buffer with the Phase 11 scene pass. `PrepareSampledTexture` now builds 2D array images; the slot and captured dimension must agree.
- Terrain pipeline front face is `VK_FRONT_FACE_COUNTER_CLOCKWISE` (opposite the scene pipeline) because terrain quads are wound to a negative framebuffer-space signed area. Getting this wrong culls all terrain silently.
- Replay CLI: `vf_packet_replay --render-terrain-scene --backend <dll> --output <ppm> [--terrain-output <vfterrain>] [--gbuffer-output <vfgbuf>] [--width N] [--height N] [--validation]`. CTest name: `contract.terrain_scene_frame`.
- Focused green: 6,365 assertions in 12 Phase 14 cases. Full Debug 188/188, full Release 188/188.
- Replay metrics, identical in Debug and Release: 2 cells, 3 layers, terrain pixels 35,289 on both sides, cell0 4,692 / cell1 30,597, seam checks 2, LOD seam checks 2, seam mismatches 0, max seam gap 0, 0 identity mismatches, max error 8.1718e-05, mean error 1.24918e-07, 46,786 interior pixels with 0 mismatches, frame-mismatch rejected, stray-layer rejected, 0 validation errors.
- `.vfterrain` SHA-256: `04A70AE48C37660E719975D3EE706FAB9C1EAE2DB5ED20E37238953B2A70BB4B` (1,152 bytes).
- PPM SHA-256: `C2A4574BC35A52355633AB2C20EDAB01C09AAA5A6455A1DC3012639EC703D8CD`.
- `.vfgbuf` SHA-256: `BDCBBEA7575D01D190532DCD72FE3CBC23CCA3A140F261BA378BF249AB38DF89`.
- Artifacts: `artifacts/phase-14/`. Documentation: `docs/phases/phase-14.md`.
- Deferred within the phase and stated in the doc: cells are not yet batched into shared draws. One indexed draw per cell with the cell index in a push constant preserves diagnostic identity but does not reduce draw count. Instancing over cells that share a slot mapping and index count is the natural form.

## Phase 15 (offline-complete): alpha-tested and two-sided visibility

- `EngineVisibility` (`src/renderer_core/EngineVisibility.h/.cpp`), namespace `vf::renderer::visibility`. 32-byte `AlphaStateV1`, 64-byte `VisibilityRecordV1`.
- `ClassifyAlphaState(AlphaPropertyCapture, observedSource, state)`: blend → `Blended`, test-only → `Tested`, neither → `Opaque` with source forced to `None`. Reference is `testReference / 255` exactly, never a rounded 0.5. Alpha-to-coverage without a test, and a test with no alpha source, both return `UnclassifiedAlpha` rather than being guessed at.
- `EvaluateCoverage` ignores `CoverageContext::depthOnly` by construction, which is what keeps the depth prepass and the color pass on the same silhouette. The comparison is `>=` (the engine's convention), so the boundary sample survives. Constant alpha scales the sample before the test.
- `DitherThreshold` is a 4x4 Bayer matrix holding each of 16 levels once, so fade coverage over a tile is exactly `round(fade * 16)` and rises monotonically.
- Alpha-to-coverage quantizes to `round(alpha * samples) / samples`; with `sampleCount == 1` it falls back to the identical binary result as a plain alpha test.
- `ComputeAlphaCoverageScales` bisects a per-mip scale (coverage is monotonic in scale) so every mip's coverage matches mip 0's, which is what stops cutouts dissolving with distance. Ties prefer the scale that does not lose coverage.
- `ResolveShadingFrame`: only a two-sided surface flips on a back face; a shading normal below the geometric horizon is lifted onto it (`liftedToHorizon`) rather than lighting from behind; a negative determinant flips the bitangent to keep the tangent basis handedness (`mirrored`). `EffectiveFrontFace` reverses the declared winding for a mirrored instance.
- `ValidateOpaqueRasterClass` refuses `Blended` with `BlendedNotSupported`. Sorted transparency is classified, not rendered as a cutout.

Done since: scene packet minor 2 with the per-object visibility section, and the GPU binding.

- `ScenePacketHeaderV1` kept its exact 96 bytes by carving `visibilityCount` (offset 80) and `visibilityOffset` (84) out of the former `reserved[2]`, leaving one `std::uint64_t reserved` at 88. `EncodeScenePacket` emits the lowest version that can represent the scene, so a scene with no visibility records still encodes as 1.0 or 1.1. Verified: the freshly built Phase 11 and Phase 12 `.vfscene` artifacts are byte-identical to the archived ones (`AEEE65B1...F530CEC` and `ADD9C3FD...B4ABE997`), and 198/198 tests pass.
- `ValidateSceneVisibility` requires exactly one record per object, positionally matching each object's ids; a partial table is `UncoveredObject`, a reordered one is `InvalidVisibility`, and an unclassified record is `UnclassifiedWorldWriter`. `ResolveVisibility` gives 1.0/1.1 scenes an implicit opaque, front-only, unmirrored record so every consumer has one rule.
- Binding 13 carries `GpuVisibilityRecordV1`, reflected from `scene_layout.comp` so its 64-byte size and binding are build-time assertions.

Backend depth prepass is now implemented and green (203/203, zero validation errors, and the Phase 11-14 GPU contract tests still pass, which confirms the prepass is inert when nothing is alpha tested).

- `RecordAlphaDraws` is shared by both passes, so the prepass and the colour pass cannot diverge in geometry, dynamic state, or push constants — only the bound pipeline differs. That is what makes "the silhouettes match" a structural property rather than something to be tested for.
- Pass order: depth-only pass (`loadOp` CLEAR, alpha-tested objects only) then the main pass with depth switched to `loadOp` **LOAD**. When no object is alpha tested the prepass is skipped and depth clears as before.
- The colour pass tests `VK_COMPARE_OP_EQUAL` with depth writes off. A silhouette disagreement between the passes therefore erases the colour fragment and shows up in the G-buffer comparison, instead of differing quietly.
- Cull mode, front face, and depth compare are dynamic state driven by the resolved visibility record: two-sided sets `VK_CULL_MODE_NONE`, and a negative determinant reverses the declared winding through `visibility::EffectiveFrontFace`.
- Alpha-tested objects are skipped by the opaque loop and drawn by the alpha colour pass afterwards, against the depth the prepass established.

CPU reference is now alpha-aware, and the drift check passed. `RenderReferenceGBuffer` gained a four-argument overload taking a base-colour texture; the three-argument form forwards with a null texture, meaning opaque white, so nothing changes for callers that do not pass one. Verified: the freshly produced `phase11-scene.vfgbuf` and `phase12-instanced.vfgbuf` are byte-identical to the archived artifacts (`430372FF...`, `A68D118E...`), and Phase 13/14 are untouched. 204/204 green.

- Coverage goes through `visibility::EvaluateCoverage` on the same `AlphaStateV1` the shader reads, so a cutout silhouette cannot differ by interpretation between the two sides.
- Face handling mirrors the backend: `visibility::EffectiveFrontFace` applies the captured determinant, a back face is culled unless the surface is two-sided, and a two-sided back face flips **both** normals so they stay in the same hemisphere.
- Fixture note for future tests: in the Phase 11 scene, **object 1 is deliberately occluded** ("projects strictly inside object 0 but four units away, so it must never survive the depth test"). It has zero coverage in any render, so it is useless as a subject for a coverage test. Object 0 is the large visible one; object 2 is visible and sits behind object 0's right edge.

### Phase 15 is offline-complete

Full record in `docs/phases/phase-15.md`; contract summary in `engine_render.md` 9.12. GPU replay `contract.alpha_scene_frame` passes, 206/206 in Debug and Release, artifacts archived under `artifacts/phase-15/`.

```text
alpha-replay extent=256x192 cutout-pixels=4422 expected-cutout-pixels=4422
gbuffer-identity-mismatches=0 gbuffer-max-error=1.21593e-05
gbuffer-mean-error=1.05739e-07 interior=46334 interior-mismatches=0
validation-errors=0 result=pass
```

Cutout coverage agrees exactly rather than within a threshold, so the gate's "declared pixel threshold" allowance is unused and unclaimed. Debug and Release produce byte-identical `.ppm`, `.vfgbuf`, and `.vfscene`. Phases 11–14 `.vfgbuf` artifacts re-verified byte-identical by SHA-256 after the changes below.

**The stored-opacity rule — the substantive finding of this phase.** The G-buffer's opacity channel is the coverage decision's own opacity, never the sampled texture alpha. `CoverageResult::coverage` already computed it and the CPU reference was discarding it. Opaque → 1 whatever the texture's alpha channel holds; cutout survivor → 1, because the test that admitted it was binary; blended → the effective alpha. The rule now lives in exactly two mirrored places, `visibility::EvaluateCoverage` and `vfEvaluateCoverage` in `alpha_coverage.glsl`, and **all three** fragment shaders source opacity from it, including the opaque Phase 11 `scene.frag`. This is engine-facing, not fixture-facing: a Fallout 4 diffuse map may carry a mask or a height in the alpha of a surface that is not alpha-tested, and the old behaviour made such surfaces transparent in the deferred buffer.

`scene.frag` calls the rule but deliberately does not `discard` — the opaque raster class is validated before a frame is armed, so its coverage is unconditionally 1 and `early_fragment_tests` stays sound.

Reds recorded before their fixes:

1. Five Vulkan validation errors — the depth prepass drew before `vkCmdBindVertexBuffers`/`vkCmdBindIndexBuffer`, which sat inside the main pass. Buffer bindings are command-buffer state, not render-pass state.
2. `interior-mismatches=4198 gbuffer-max-error=1` with coverage and identity already exact. A bare mismatch count cannot name a cause, so the replay gained per-channel diagnostics; they isolated it to `albedo[3]` alone (expected 1, actual 0) and led to the stored-opacity rule.

Also corrected: the GPU has always modulated albedo RGB by the base texture and the CPU reference never did. Phases 11–14 hid it because their reference renders pass no texture (opaque white), and the Phase 15 fixture nearly hid it again because its cutout texture is white. The reference now applies it.

**Inference trap this phase produced.** I compared freshly built artifacts against the journal's recorded hashes using MD5 and read the mismatch as a regression. The recorded values are SHA-256. Hash algorithm is part of the evidence — a hash comparison with an unstated algorithm is not evidence.

Stated gaps, carried forward rather than closed silently: the mip coverage-scale ramp is unit-tested only (the replay fixture has one mip level); `AlphaClass::Blended` is classified and refused, never rendered; alpha-to-coverage has no multi-sample GPU evidence.

## Phase 16 (offline-complete): specialized opaque material families

Full record in `docs/phases/phase-16.md`; contract summary in `engine_render.md` 9.13.

- `EngineMaterialFamily` (`src/renderer_core/EngineMaterialFamily.h/.cpp`), namespace `vf::renderer::material`. It extends the Phase 9 boundary rather than replacing it, so `CanonicalMaterial` and the transfer LUT are untouched.
- `MaterialFamily` mirrors the engine's 21 lighting feature IDs exactly (0 Default … 20 Dismemberment), with `None` for the engine's -1 and `Unknown` for anything else. An unclassified ID is **never** folded into Default: a silent Default renders an unknown family as an ordinary surface and looks plausible while being wrong.
- The full 64-bit property flag map is recorded in `PropertyFlag`. All 64 bits, not just the ones used, because a partial copy invites reading a neighbouring bit by accident.
- Slot roles are resolved per family, which is the point: role 7 is recorded as backlight-mask **and** smooth-spec. Face/SkinTint/HairTint read it as a backlight mask; everything else reads the smoothness/specular mask. Reading it as the wrong one silently turns a smoothness map into a rim-light mask.
- Slots 8 and 9 of the ten-slot texture set carry no recorded role. An authored one increments `diagnostic.unclassifiedAuthoredSlots` rather than being assigned a meaning.
- **Nothing is derived from whether a texture happens to be authored.** An authored texture is not a declaration that it is used. Equally, captured scalars are not a licence to enable a lobe: a Default material with a non-zero subsurface rolloff still resolves with subsurface off and a zeroed rolloff.
- The headline rule: **bright RGB never becomes emission without the flag.** Emission is authorized by a glow-map slot, `OwnEmit` (bit 22), `ExternalEmittance` (bit 29), or the GlowMap family itself. A saturated albedo is ordinary in authored content; reading it as emission makes plain surfaces glow. Verified by mutation: adding `capture.emitColor[0] > 1.0f` to the emission condition fails `P16_bright_base_colour_never_becomes_emission_without_the_flag` with 4 assertions (90.0f vs 0.0). The test has teeth.
- LOD families never pay for a specialized lobe whatever their captured scalars hold — parallax, subsurface, and anisotropy stay off, and `features.reducedDetail` is set.
- Rejections rather than clamps: `InvalidEyeTransform` for a non-positive eye radius or iris scale, `InvalidParallaxRange` for a zero or inverted step range, `InvalidLayer` for a refraction index below one, `MissingRequiredSlot` for an unauthored base colour / height / inner layer / glow map, `NonFiniteSource` for any non-finite scalar.
- 21 families collapse into 8 broad `ShaderClass` values (Standard, Skin, Hair, Eye, Parallax, MultiLayer, Terrain, Lod), which is the refactor requirement: everything else that varies rides along as feature data so pipeline count does not grow with content. Asserted as `distinct <= 8` over all 21 IDs.
- `RequiresDescriptorRebuild` / `RequiresDynamicUpdate` split static from dynamic: a rebound texture, changed family, changed slot layout, or bumped `staticRevision` rebuilds; a wetness change moves only the dynamic revision. Without that split every rain transition would rebuild descriptor sets.

Build-correctness fix made here: `shaders/phase11/scene.frag` now includes `../phase15/alpha_coverage.glsl`, but its CMake rule did not depend on it, so editing the shared coverage rule would have left the opaque shader holding a stale copy. The dependency is declared and verified — touching `alpha_coverage.glsl` regenerates `scene.frag.spv`.

GPU vertical is implemented and green at 225/225, with all five prior-phase `.vfgbuf` artifacts re-verified byte-identical by SHA-256 after the reference refactor.

- Reflected `GpuFamilyRecordV1`, 144 bytes, binding 14, asserted at build time. Mutation-verified: adding one `vec4` to the GLSL struct fails the build with "phase16 family record drifted from 144 bytes".
- `.vffam` packet: 64-byte header, 368-byte `FamilyRecordV1`, explicit layout rather than a serialized `FamilyDescriptor` so a compiler's packing choice can never change what a captured artifact means. CRC'd, pointer-free, duplicate/unclassified/non-finite/slot-role validated. `ResolveFamilyRecord` gives an object with no captured family the ordinary lit surface, the same shape as `scene::ResolveVisibility`.
- ABI minor 9 appends `familyData`/`familySize` at 176/184 and `hdrData`/`hdrCapacity` at 192/200 without moving the Phase 6–14 prefixes. Request is now 208 bytes. The HDR pair is separate so a caller can supply families without also asking for the float colour target.
- The HDR readback exists because emission needs comparing at its authored magnitude: `kHdrFormat` is already `R16G16B16A16_SFLOAT`, so only a `TRANSFER_SRC` usage bit, a host buffer, and a copy were needed — no new attachment. Decoded through the existing `mesh::HalfToFloat` rather than a second decoder.
- One `familyScenePipeline` serves all eight shader classes, with cull/front-face/depth-compare dynamic. A frame carrying families routes its opaque draws through it; a frame without them keeps the Phase 11 path exactly, which is why the earlier artifacts do not move.
- `scene::ReferenceInputs` + `HdrImage` generalize the CPU reference. `GBufferPixelV1` was deliberately **not** grown — its 64 bytes are a captured artifact — so emission lands in a separate HDR image instead.
- `GpuFeatureNormalMap` exists because the shader cannot otherwise tell an unbound slot from one holding a flat texture, and would replace the object's shading normal with a decoded constant.

Stated gaps in the GPU vertical, carried forward rather than closed silently:

- **Greyscale-to-palette is declared but not applied.** The lookup texture's slot in the ten-slot set is not among the recorded role IDs, so applying a palette would mean inventing a ramp. The flags travel in the record so it can be finished from a live capture.
- **Glow-map modulation is declared but not applied.** The material bundle binds base, normal, and smooth/spec only; the engine's glow map is slot 2 of the shader texture set and has no binding yet. A glow-mapped material emits its declared colour unmodulated rather than borrowing an unrelated channel.
- **POM marching is not implemented.** Scale, bias, UV scale, and the step range are carried and validated; the march itself is not in the shader.

Replay fixture, CTest `contract.family_scene_frame`, artifacts, and docs are complete. 226/226 in Debug and Release, all Phase 16 artifacts byte-identical between configurations, and all five prior-phase `.vfgbuf` artifacts re-verified unchanged.

```text
family-replay extent=256x192 families=2
tint-pixels=8844 expected-tint-pixels=8844
emissive-pixels=3130 expected-emissive-pixels=3130
hdr-max-error=0.00265646 normal-encodings-differ=yes lobe-differs=yes
gbuffer-identity-mismatches=0 gbuffer-max-error=4.38094e-06
interior=46691 interior-mismatches=0 validation-errors=0 result=pass
```

**The fixture that passed while measuring nothing — the most important lesson of this phase.** The first `--render-family-scene` reported `result=pass` even with the shader forced to ignore the model-space encoding entirely. Two independent defects were behind it:

1. The `normal-encodings-differ` check compared object 0's shading normal to object 2's. Those objects face different ways (yaw 0 and yaw 25 degrees), so it was measuring geometry, not encoding, and would have been true no matter what the shader did.
2. The tangent decode was treated as already being in model space. A reconstructed tangent Z is always positive while the fixture's surfaces face -Z, so every tangent normal pointed away from its surface, the horizon lift flattened both decodes onto nearly the same plane, and the real difference collapsed to 5e-4 — below the comparison threshold.

Defect 2 is a genuine renderer bug, not a fixture artifact: **a tangent-space normal map stores its Z along the surface normal and has to be rotated into the surface frame.** Fixed with Duff et al.'s branchless orthonormal basis built from the already face-signed geometric normal, mirrored branch for branch between `family_shading.glsl` and `EngineScene.cpp`. The fixture now predicts *both* decodes for the **same** object and asserts the render took the declared one. Re-running the identical mutation now fails loudly: `normal-encodings-differ=no`, interior mismatches 0 → 8320, max error 4.4e-06 → 0.199, which is exactly the predicted separation between the decodes.

Two further inference traps recorded from this phase:

- **A fixture that passes on its first run has not yet been shown to measure anything.** Both Phase 16 gates were mutation-tested; one held and one did not. The one that did not had passed cleanly.
- **A single "fixture construction failed" message named no cause** across six distinct failure modes. Split into per-step diagnostics, it immediately identified `invalid-material` (base colour must be sRGB) and then `texture-packet-failed` (a non-typeless resource cannot carry a differing view format).

Remaining for Phase 16: the installed-corpus sweep. The structural half of the gate's first clause is already satisfied by construction — the fallback path means nothing can fail to resolve — but which families actually occur in `Fallout4 - Materials.ba2` (1,770,496 bytes, BA2 v8, ~6,899 entries) and at what frequency is not yet measured. No BA2 reader or zlib exists in the tree, so this needs a complete GNRL reader, a complete DEFLATE implementation, and a BGSM parser. `Fallout4 - Materials.ba2` (1,770,496 bytes, BA2 v8, ~6,899 entries) is the installed general material archive. No BA2 reader or zlib exists in the tree, so this needs a complete GNRL reader and a complete DEFLATE implementation — not a partial one — plus a BGSM parser. The structural half of the clause is already satisfied: the fallback path means nothing can fail to resolve. The sweep adds which families actually occur and at what frequency.
4. `docs/phases/phase-16.md`, `engine_render.md` section, journal update.

## Phase 17 (offline-complete): lights, sky, weather, fog, raster parity

Full record in `docs/phases/phase-17.md`. 243/243 in Debug and Release, all Phase 17 artifacts byte-identical between configurations, and the Phase 11, 14, 15, and 16 G-buffer artifacts unchanged.

Built on RTTI reconstructed from the installed binary rather than assumption — recorded in `engine_render.md` 9.12a. The engine has exactly four concrete `NiLight` subclasses (`NiAmbientLight`, `NiDirectionalLight`, `NiPointLight`, `NiSpotLight`, all sharing the 58-slot base vtable). `BSShadowLight` is a **separate 15-slot object, not a fifth light type**, which is why shadow casting is a flag on a light here: modelling it as a type would make "a point light that casts shadows" inexpressible. `ShadowSceneNode` (67 slots) is the accumulator's container and the light-list capture boundary.

- `EngineLighting` (`src/renderer_core/EngineLighting.h/.cpp`), namespace `vf::renderer::lighting`. 112-byte `LightRecordV1`, 112-byte `EnvironmentRecordV1`, 80-byte `GpuLightRecordV1`, 64-byte `LightPacketHeaderV1`, `.vffl` packet.
- **The shadow term is declared unavailable** (`ShadowTermAvailable() == false`) for the whole phase. That is the gate's own requirement: a parity metric can mask the term instead of comparing a lit mirror against a shadowed vanilla frame and reporting the difference as error.
- Attenuation is clamped to the captured radius, so a light list can be culled by radius without changing the image. All-zero coefficients would divide by zero and are refused, not defaulted.
- Cones are stored as **cosines**, because that is what a shader compares a dot product against; converting per fragment would repeat arithmetic for nothing. An inner angle exceeding its outer is a captured contradiction and is refused rather than silently reordered.
- The dimmer scales radiance and is kept **separate** from the colour, so a consumer can still read back the authored colour. A negative dimmer would subtract light from the scene and is refused.
- Light positions follow the terrain rule: subtraction against the camera origin in **double**, narrowing only the small residual. A light too far to narrow is `PositionOutOfRange` rather than silently swimming.
- **Interior zeroes the sun and moon.** Carrying an exterior's sun into an interior is exactly the stale-state bug the gate names. Mutation-verified: passing the captured intensity straight through fails `P17_environment_state_selects_interior_and_exterior_without_stale` with `3.0f == Approx(0.0)`.
- Interior and exterior **cannot be blended**. The engine cuts between them; a cross-fade would invent a state the game never shows, so `BlendEnvironment` returns `IncompatibleEnvironments`.
- Blended sun/moon directions are renormalized, because interpolating two unit vectors shortens them and would dim the light mid-transition.
- Fog saturates at the **captured maximum**, not at one, or distant geometry would vanish into fog the engine never applied.
- Light-list overflow is deterministic and reported: ranked by brightness over squared distance, ties broken on captured order then identity, `droppedCount` and `overflowed` surfaced. A list that dropped different lights each frame would make a static scene flicker.

## Phase 18 (in progress): acceleration structures and ray-traced shadows

CPU contract complete and green (12 cases, 135 assertions; 255/255 overall). The GPU ray-query vertical, replay fixture, and docs are outstanding.

- `EngineAcceleration` (`src/renderer_core/EngineAcceleration.h/.cpp`), namespace `vf::renderer::accel`. 80-byte `InstanceDescV1`, 48-byte `AccelPacketHeaderV1`, `.vfas` packet, `AccelSchedule`.
- **Build sizes round up to the device's alignments.** The live capability report gives `accelerationStructureScratchAlignment = 128`; scratch that is not aligned to it is a validation error at build time, not a slow path. A non-power-of-two alignment cannot be rounded to by masking and is refused.
- **A static structure reserves no update scratch at all** — it is never refitted, so the reservation would be pure waste.
- **Topology change forces a rebuild.** Refitting a structure whose triangle count changed produces one that no longer matches its geometry, and the corruption is silent: rays simply miss. Opacity is part of that test because it decides which hit groups run. Mutation-verified: dropping the opacity comparison fails `P18_update_is_chosen_only_when_topology_is_unchanged` with `1 == 0`.
- **Only static structures are compacted.** Compacting one that is refitted every frame costs more than it saves, because it would have to be rebuilt to be compacted again.
- A **mirrored** instance is legal and keeps its winding reversal — the determinant sign is information, not an error. Only a **singular** transform, which collapses the instance to zero volume, is refused. A zero instance mask and a custom index above 24 bits are refused too: the index truncates silently and points at the wrong material.
- **Transformed bounds transform all eight corners.** A rotated box's axis-aligned bounds are not its bounds rotated, and anything less clips geometry that is really inside the bound. The test checks every corner lands inside the reported bounds.
- **Ray origins offset along the geometric normal**, not the shading normal, which can point into the surface and would push the origin below it. The offset scales with distance from the origin because float spacing does: a fixed epsilon that works near the camera is invisible far away.
- **Alpha candidates resolve through `visibility::EvaluateCoverage`**, the same function the raster pass uses, exactly as the plan requires. Two alpha tests would drift and a shadow silhouette disagreeing with the surface silhouette is the artefact this phase exists to avoid. Mutation-verified: inflating the sampled alpha makes a cutout hole occlude and fails 3 assertions across 2 cases.
- **Blended geometry casts no ray-traced shadow** in this phase and is excluded from the occluder set rather than defaulting to opaque, which would put a solid shadow under every pane of glass.
- `AccelSchedule` owns traceability and retirement in one place, so "is this traceable" has exactly one answer. A retired handle stays refused even after a newer build exists, and a structure still in flight cannot be destroyed because its scratch is live.
- `ShadowTermAvailable()` is now **true** and `ShadowMaskRequired()` is false. Phase 17 declared the term unavailable so parity could mask it; supplying it means the mask comes off, or a broken shadow would be hidden from every comparison that follows.

Trap re-encountered twice here: a mutation that fails to **compile** leaves the previous test binary in place, and running it reports "All tests passed" for code that was never built. Both times the give-away was the case count not moving. A mutation check is only evidence if the build succeeded — prefer mutations that keep every parameter used.

The raster device now enables `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations`, and `bufferDeviceAddress`, and resolves the six extension entry points it needs. All of it is **conditional**: a device that does not advertise ray query still creates, and the mirror falls back to unshadowed lighting rather than failing to render. A missing entry point after a successful enable withdraws the capability instead of faulting on first use. 255/255 with the extensions enabled and zero validation errors.

**BLAS/TLAS construction now runs on the GPU.** One bottom level over the packet's triangles and one identity top-level instance, both recorded into the frame's own command buffer — one submission, no second fence, and no command buffer that can be freed while still pending. The BLAS→TLAS and TLAS→fragment orderings are precise `VkMemoryBarrier2` dependencies on `ACCELERATION_STRUCTURE_BUILD`, not all-commands barriers. Storage and scratch are device-local; build inputs come from the upload buffer, which gains the build-input and device-address usages when ray query is available. 255/255 with the build active and zero validation errors.

The build cost a long debugging session and the lesson is worth more than the fix. Symptom: every fixture failed with `RasterRenderFailed`, `submissions=0`, and validation completely silent. **The cause was one hardcoded enum**: `triangles.indexType = VK_INDEX_TYPE_UINT32` while the packet's default is `IndexType::Uint16`. The layers pass it — the build is legal, the addresses are legal, the sizes are legal — and the GPU reads `{0,1,2}` as 65536, fetches a vertex two megabytes past a 436-byte buffer, and the submission dies as `VK_ERROR_DEVICE_LOST`.

What actually found it was bisection, after several rounds of hypothesising had found nothing:

1. Short-circuit the whole build → **pass**. The build is the cause, not the extensions, descriptor pool, or allocator.
2. Tag every failure return in the render path and log the fence-wait result → `pre-stop-1 wait=-4`, i.e. `VK_ERROR_DEVICE_LOST` in `WaitForSubmission`. **A GPU fault, not an API misuse.**
3. Log the real inputs → `tris=1 verts=3 stride=32 vtxOff=0 idxOff=96 total=436`. Every number sane, so the fault is an *address*, not a count.
4. Keep all creation and the descriptor write but record no build commands → **pass**. The fault is the build command itself.
5. Empty TLAS, then no TLAS command at all → still faults. **The BLAS is the faulting build.**
6. Switch to `VK_INDEX_TYPE_NONE_KHR` (non-indexed) → **pass**. Vertices, scratch, storage and the command are all correct; only the index read faults.

Step 6 is the one that named the bug. Each step above is a measurement that halves the search space, and the whole sequence took less time than the guessing that preceded it.

Two traps recorded from that session. A `VK_ERROR_DEVICE_LOST` reported at the fence wait names the *waiter*, never the operation that faulted — the failing command is somewhere in the submission, and only bisection tells you where. And output filtered through `grep` for expected strings hides the unexpected ones; the layers had been silent all along, which was itself the evidence that the fault was in the GPU rather than the API.

**The ray-query shadow term is in the shader and matches the oracle.** `accel::ShadowRayForLight` and `vfShadowRayForLight` are the same rule written twice, branch for branch: offset origin, negated travel direction for a directional light reaching `kDirectionalShadowDistance`, and a positional ray measured from the offset origin that stops at the light. Ambient is never shadowed on either side — it is not cast from anywhere, so no ray of it can be blocked. Mutation-verified both ways.

Ray query is a capability of the SPIR-V module, not a runtime branch, so `family_scene.frag` is compiled twice (`-DVF_RAY_QUERY=1 --target-spv=spv1.4`) and the backend picks by what the device enabled.

`--render-family-scene --shadows` is the Phase 18 fixture: an occluder and the light it blocks, **appended** to the Phase 16/17 scene rather than edited into the shared builders, because Phase 17's artifacts are recorded by hash. Its `.ppm`, `.vffl`, and `.vfgbuf` were re-verified byte-identical afterwards. 7,278 pixels shadowed, 41,981 shadow-interior pixels compared, 0 mismatches, 0 validation errors; Debug and Release byte-identical; 258/258 in both. Mutation-verified: a GPU shadow term that always returns lit fails with 5,900 interior mismatches.

Two things that fixture taught, beyond the shadow itself:

- **The inherited fixture's point lights contribute nothing.** They are authored camera-relative, but `BuildGpuLight` narrows a light against the camera origin, so after narrowing they sit a whole camera origin away and fall outside their own radius. Phase 17's parity passed regardless, because both sides evaluate the same records. Agreeing about nothing is still agreeing — the Phase 18 light is authored in world space for exactly this reason.
- **A fixed absolute HDR tolerance gets tighter as the scene gets brighter.** With a bright in-range light the two sides agree to 0.1% and a flat `1e-2` still calls it a failure; the interior bound is `1e-2 + 1e-3·|expected|`. The deepest interior disagreement is a pixel that is not shadowed at all. The shadow term is all-or-nothing and cannot hide in that band, which the mutation confirms.

Full detail in `docs/phases/phase-18.md`.

Remaining for Phase 18: the live capture gate (a captured occluder set promoted through the harness), an any-hit path for cutout occluders, and letting the backend consult `DecideBuild`/`ShouldCompact` instead of rebuilding every frame.

## Plugin-side console execution (option A), in progress

Goal: remove synthetic keyboard input from the live-capture critical path, because focus contention and the attract screen make it unreliable.

Delivered and green:

- `renderer_host/AddressLibrary` parses `version-1-11-221-0.bin`. The file is **not** the Address Library v1/v2 container; it is `u64 count` followed by `count x {u64 id, u64 rva}` sorted strictly by id. 651,581 entries, and `8 + 651581*16` is exactly the file size. Load refuses truncated, mis-sized, non-ascending/duplicate, and out-of-image databases; `Resolve`, `Identify` (reverse), and `ResolveAddress` are covered. 4 cases, 33 assertions, including one that reads the real installed database.
- Corroboration that matters: `kGraphicsStateRva = 0x03D70920`, which this project recorded by hand, is present in the database as ID `2,704,621`. The recorded constant and the parse each confirm the other.
- Console command table fully mapped and recorded in `engine_render.md` 13.1. Found by anchoring on the documented `.rdata` command-name strings and searching `.data` for qwords equal to `0x140000000 + stringRva`. 459 records at RVA `0x2EF0580..0x2EF94A0`, stride `0x50`. Self-validating: opcodes 321..779 is exactly 459 values, so `opcode == 321 + index`. Record layout: `+0x00` longName, `+0x08` shortName, `+0x10` opcode, `+0x18` help, `+0x28` params, `+0x30` per-command Execute, `+0x38` shared parse handler. `CenterOnCell`/`COC` is opcode 323, record `0x2EF0620`, Execute `0x5EB240`.

The user chose route 2 (locate the compiler) over route 1 (hand-build a parameter blob). Route 1 is therefore not to be attempted.

New tool: `vf_engine_analysis` (`src/tools/EngineAnalysisMain.cpp`). Offline static analysis of a PE on disk; it never attaches to a running game. Modes: `--sections`, `--command-table`, `--find-command <name>`, `--xref-lea <lo> [--high <hi>]` (RIP-relative `lea reg,[rip+disp32]` references into a range), `--find-pointer <lo> [--high]` (absolute 8-byte pointers anywhere in the image), `--calls <rva>` (direct `E8 rel32` call sites). It reproduces the command-table discovery from scratch, which is what makes the recorded addresses reproducible rather than trusted because a one-off script said so.

The xref scanner is validated against a known-good target: 76 `lea` references to `0x3D70920`. That is a third independent confirmation of `kGraphicsStateRva`, alongside the address-library ID and the project's own use of it.

Where route 2 stalled, and it is worth knowing before resuming: the console command table has **no static references at all**. Zero RIP-relative `lea` references into `0x2EF0580..0x2EF94F0`, and zero absolute pointers into that range anywhere in the image, while a wider window (`0x2EE0000..0x2F00000`) has 530 `lea` references that all land past the table's end. So the table is not reached by any statically visible reference, and walking outward from it to the compiler does not work.

### CommonLibF4 checked out and evaluated — its ids do not apply to this build

Cloned `Ryan-rsm-McKenzie/CommonLibF4` (both `master` and `community`). Two outcomes, one good and one that closes a door.

**Format confirmed exactly.** `REL::IDDatabase::load` builds its span as `mmap.data() + sizeof(uint64)` with length `*reinterpret_cast<const uint64*>(mmap.data())` over `mapping_t { uint64 id; uint64 offset; }`. That is byte-for-byte the format `renderer_host/AddressLibrary` was written against, independently reverse engineered. The reader is right.

**The ID spaces differ, and this is a safety issue.** CommonLibF4 targets 1.10.163 (`RUNTIME_1_10_163`; it also handles 1.10.980, which shares that ID space). This install is 1.11.221.0 and its address library was regenerated with a different ID space. Every id tested is absent:

| CommonLibF4 symbol | ID | In 1.11.221.0 database |
| --- | ---: | --- |
| `Script::CompileAndRun` | 526625 | absent (neighbours 526621, 526635) |
| `Script::ParseParameters` | 1607 | absent |
| `RTTI::Script` | 740077 | absent |
| `RTTI::ConsoleData` | 542072 | absent |
| `RTTI::TESForm` | 1344421 | absent |

The database spans ids 15..10,260,203 over 651,581 entries, so it is sparse and **populated around the missing ids**. A CommonLibF4 id can therefore resolve successfully against this database and point at an entirely unrelated address. Never resolve a CommonLibF4 id here; treat the two id spaces as unrelated.

What CommonLibF4 is still worth using for: exact struct layouts and signatures. `Script` derives from `TESForm` with `SCRIPT_HEADER header` at `0x20`, `char* text` at `0x38`, `std::byte* data` at `0x40`. `void Script::CompileAndRun(ScriptCompiler*, COMPILER_NAME, TESObjectREFR*)`. `ScriptCompiler` is an **empty class** (`static_assert(std::is_empty_v<ScriptCompiler>)`), so the compiler argument is just a non-null placeholder.

### Script compiler located in this build

Anchors found in `.rdata` and xrefed with the tool:

| String RVA | Text | Referenced from |
| --- | --- | --- |
| `0x24AC240` | `Script command "%s" is a console-only command.` | `0x5BAF61` |
| `0x24AC270` | `Script command "%s" not found.` | `0x5BB001` |
| `0x24ADBB5` | `ScriptCompiler::StandardCompile.` | no `lea` reference |

`.pdata` puts `0x5BB001` inside the function `0x5BAFB4..0x5BB1B1` — the compiler's command-resolution routine. It has **zero direct `E8` callers**, which is consistent with the command records' shared `+0x38` compile handler at `0x5B6160` being reached through the table rather than by direct call.

### AddressLibGen evaluated — it cannot regenerate a matching id space

`AddressLibGen/src/main.cpp` reads a corpus of binary-diff files from `mappings/`, named `<maj>.<min>.<build>_<maj>.<min>.<build>.txt`, each an 18-line header followed by tab-separated address pairs linking one game version's addresses to another's. It unions those links, assigns ids, and writes `version-*.bin` in exactly the format already implemented here — a third independent confirmation of the format.

It cannot be used, for two independent reasons:

1. **The corpus is not in the repository.** `AddressLibGen/mappings/` contains a single 0-byte `dummy` placeholder. The address-pair files come from binary diffing successive game builds and are not published with the source.
2. **Ids are not reproducible even with a corpus.** `assign_ids` is `id = 0; for each version in version order { for each (offset, mapping) in offsetMap { if unassigned assign(id++) } }`, and `offset_map` is `robin_hood::unordered_node_map` — an **unordered** container. Assignment order therefore depends on hash iteration order, not on offset. Two runs over different corpora, or with a different hash implementation, produce different id spaces.

That is the full explanation for what was observed: an id space is an artifact of one specific generation run, so the 1.11.221.0 database and CommonLibF4's 1.10.163 lineage are simply unrelated numbering. **CommonLibF4 symbol ids can never be resolved against this database.** Do not try again.

### Conclusion this investigation reaches, which matters beyond the console

The Address Library is useful here as a **corroboration** source, not a symbol source. It confirmed `kGraphicsStateRva` independently (id 2,704,621), and that is the role it should keep. Engine addresses in this project must continue to come from discovery against the binary — string anchors, xrefs, RTTI, self-consistency — gated by the exact executable hash, which is what the build gate already enforces. That is the approach the camera matrix scan already uses and it is the one that survives a version change with evidence rather than faith.

### RTTI reconstruction implemented and validated

`vf_engine_analysis --rtti <substring>` reconstructs MSVC RTTI with no id database at all: mangled name string -> `TypeDescriptor` (name is at `+0x10`, so the descriptor begins `0x10` earlier) -> `RTTICompleteObjectLocator` (matched on the image-relative `pTypeDescriptor` at `+0x0C`, and confirmed by the x64-only self-pointer at `+0x14` equalling the locator's own RVA) -> vtable (the qword preceding a vtable holds its locator).

Validated: `.?AVScript@@` resolves to descriptor `0x2FA5670`, which is exactly the address predicted by hand from the name string at `0x2FA5680`. Results:

| Class | Descriptor | Locator | Vtable | Slots |
| --- | --- | --- | --- | ---: |
| `.?AVScript@@` | `0x2FA5670` | `0x29543B8` | `0x24ABDE8` | 74 |
| `.?AVConsoleData@@` | `0x2FA5F20` | `0x29549A8` | `0x24B0008` | 3 |

Also added `--class-of <rva>` (attributes a vtable slot to its owning class and slot index) and `--vtable <rva>` (dumps slots, resolving each to its containing function).

### Unwind chain resolution — this one is load-bearing

The compiler's functions are **split into chunks**, so a naive `.pdata` lookup attributes an address to a fragment that has no callers of its own. `ResolvePrimaryFunction` now follows `UNWIND_INFO` chain records (flag `0x04` in the top five bits of the first byte; the chained `RUNTIME_FUNCTION` follows the unwind codes, padded to an even count) until the primary entry is reached. Before this, `--callers-of` reported zero callers for everything in the compiler and the trail looked dead. Any future call-graph work here must resolve chunks first.

### Script compiler mapped in this build

Walking up from the error string, with chunk resolution applied at each step:

| Level | Function | Reached from |
| --- | --- | --- |
| command resolution | chunk `0x5BAFB4..0x5BB1B1`, primary `0x5BACF0..0x5BAD37` | emits `Script command "%s" not found.` at `0x5BB001` |
| | `0x5B9DB0..0x5BA004` | calls the above at `0x5B9E0A` |
| | `0x5B6010..0x5B60CE` and `0x5B60E0..0x5B6154` | call `0x5B9DB0` |
| compile entry points | `0x5B2370..0x5B23EC`, `0x5B2450..0x5B24CE`, `0x5B2550..0x5B25B7` | call `0x5B6010` |
| callers | `0xA731A0`, `0x1035170`, `0x10364F0`, `0x1038A0D` | call the compile entries |

Separately, the shared per-command compile handler named at `+0x38` in every command record is `0x5B6160..0x5B9224` (12,452 bytes) and has **zero direct callers**, confirming it is dispatched through the table pointer exactly as section 13.1 records.

`Script::CompileAndRun` is still not pinned down. `0x1035170` and `0xA731A0` have no direct callers and are reached through function-pointer tables at `0x25A3C60` and `0x252EF18`/`0x252F120`, but `--class-of` finds no class vtable containing those addresses, so they are static dispatch tables rather than vtables. Candidates are now down to roughly four functions.

Next step to close it: a length-decoding disassembler pass so the compile entry points can be read directly and the argument shape matched against `void Script::CompileAndRun(ScriptCompiler*, COMPILER_NAME, TESObjectREFR*)`. Note the possibility that this subtree is the Papyrus or quest-script compile path rather than the console's; the console path should be confirmed independently before assuming.

Two PE-parsing bugs were found and fixed while building the tool, both worth remembering: `SizeOfOptionalHeader` is the **low** half of the dword at `ntOffset+20` (the high half is `Characteristics`), and only the part of a section that exists on disk (`min(rawSize, virtualSize)`) can be read — `.data` here has `virtualSize 0xF923B0` against `rawSize 0x20D200`, so most of it is zero-filled at load and has no file bytes.

## Live promotion: exact next actions

0b. **The engine can run a console command by itself.** `sStartingConsoleCommand:General` is a registered `Setting` in this build: its name string is at RVA `0x255C8B8`, the `Setting` object at `0x2F274C0` (name pointer at object+`0x10`, the same shape `EngineSettings.cpp` already relies on), and code reads that object at RVA `0x147652` inside `0x147620..0x14767B`. It is already listed in `src/SettingsCatalog.h`.

   That means console execution does **not** require calling into the engine at all. `-StartingConsoleCommand` writes the value into `Fallout4Custom.ini` under `[General]` before launch, and the engine executes it during startup. No key is injected, window focus is irrelevant, and the attract screen is out of the critical path. The file is backed up and restored like every other INI the harness touches. `-StartupWorldSeconds` bounds the wait, and the wait still ends only when a camera capture actually finds a camera.

   This is the discovery route paying off where the id route could not: found by string anchor, `Setting`-object shape, and xref, with no address-library id and no disassembly.

   **The user already had `sStartingConsoleCommand=gr quality 3` in their `Fallout4Custom.ini`**, which the harness correctly backed up and restored. That is independent evidence the setting is real and honoured. It also means the setting must never be assumed empty; always restore it.

   **It works.** A probe screenshot (`artifacts/live/20260816-114108-startup-diagnostic/probe-4.png`) shows the menu carrying QUICKSAVE/SAVE/LOAD — entries that only exist once a game session is loaded — over a blurred Sanctuary exterior. So `coc SanctuaryExt` executed and the world loaded, with no injected input at any point.

   What blocked the camera was the next thing along: the engine pauses behind a menu as soon as the window loses focus, and a paused menu has no active world camera, so every probe correctly reported `candidates=0`. `bAlwaysActive:General` (also already in `SettingsCatalog.h`) is now set alongside the startup command so an unattended run keeps rendering while unfocused.

   **`bAlwaysActive` worked, and the result is now conclusive.** `artifacts/live/20260816-114517-camera-rotation/probe-1.png` shows Sanctuary Hills in first person with the HUD compass up, unpaused, no menu. The game is in a loaded world and rendering — and the camera scan still reports `candidates=0` over the full `BSGraphics::State` block (`ResolveGraphicsStateWindow`, `0x3C0` bytes), not just the `0x250` camera-state sub-window.

   So the defect is the scan window itself for this build, isolated at last from every environmental cause. `EngineCameraCapture` now dumps `0x4000` bytes from `kGraphicsStateRva` to `<capture path>.state.bin` when the scan finally fails, so the matrices can be located offline by the same self-consistency test at whatever offset they actually live. Note the earlier session's recorded offsets (view `+0x050`, projection `+0x090`, viewProjection `+0x0D0`) have not reproduced here and should be treated as unconfirmed until the dump says otherwise.

   **Root cause found and fixed: the candidate budget truncated the scan.** The `.state.bin` dump was analysed offline with `vf_engine_analysis --scan-dump`, which found the triple immediately and exactly: **view `+0x1B0`, projection `+0x1F0`, viewProjection `+0x230`, column-major, residual 0** (a second product at `+0x270` shares the same view/projection pair, consistent with the record holding both a jittered and an unjittered viewProjection).

   `kMaximumCandidates` was 96 and the candidate loop advances 4 bytes at a time, so the scan could examine at most offset `0x180` before spending its budget — and the camera sits at `0x1B0`. It ran out roughly 48 bytes short of the answer, every time. The cap is now a genuine allocation ceiling (4096) with the reservation sized to what the block can actually yield, and a regression test (`P10L_camera_scan_reaches_a_camera_late_in_a_dense_record`) builds a dense `0x3C0` record with the camera at the measured live offsets.

   A second, quieter defect: the failure log's `candidates=%u` came from a **default-constructed** result whenever nothing was found, so it always printed `candidates=0`. That is a hardcoded value, not a measurement, and it actively misled the investigation into suspecting the window rather than the budget. The capture now re-scans once on the failure path to report the real count and the block size.

   **The loading screen has its own camera, and it will fool this experiment.** With the budget fixed, the first run captured a camera immediately and both frames reported an identical, exactly axis-aligned basis (`right=1,0,-0 up=0,0,1 forward=-0,1,-0`) with `near=15 far=15000 fov-deg=24`. That looks like a clean negative result — a camera that does not follow the player — and it is not. `world.png` from that run is a **black loading screen with the Vault Boy icon**: the game was still loading, and what was captured is the loading screen's model-viewer camera. Two loading-screen frames will always match, so the comparison proved nothing.

   The tell is the FOV. The plugin logs the live settings at startup: `fDefaultWorldFOV:Display = 70.000` and `fDefault1stPersonFOV:Display = 80.000`. A captured camera reporting 24 degrees is not the player's view. **Always check the captured FOV against those settings before drawing any conclusion about which camera was found.** `-WorldSettleSeconds` (default 75) now delays the experiment after the first camera appears so the load can finish.

   Two inference traps worth remembering. A terrain mesh capture is **not** evidence of a loaded world — the menu renders scenery too, and a 289-vertex 17x17 quadrant was captured while still at the menu. And `candidates=0` is **not** evidence that the camera scan window is wrong; it is equally consistent with a paused game. Only the screenshot separated these, which is why the probe loop now saves one every third attempt.

0a. `-ManualWorldEntry` also exists, for when the setting is not usable. It skips synthetic attract-screen input entirely, prints an operator instruction, and then **polls by arming a throwaway `frame` capture every few seconds until one actually finds a camera**. A menu has no world camera — every probe at the menu reports `rejected reason=no-camera-found attempts=240 candidates=0` — so a successful camera capture is a sound signal that a world is loaded, and it cannot be faked by timing guesses. Each probe uses a distinct path (`live-frame-probeN.vfframe`) because `Wait-VfLogMarker` rescans the log from the start and a shared path would match the previous probe's line. Once a camera appears the harness continues unattended: capture, `player.setangle z 90`, capture. Use `-ManualWorldEntrySeconds` to size the window.

   First attempt (`artifacts/live/20260816-105814-camera-rotation`) expired after 420 s with twelve probes, all `candidates=0`, because nobody was at the keyboard. It cleaned up correctly: no save files created, no restore errors, no forced kill, plugin uninstalled. That run is evidence the polling works, not evidence about the camera.

   Also note: pass **absolute** paths to the harness. The shell's working directory is not necessarily the repository root, and a relative `-PluginDll` resolved against `out/build/vs2022-x64-debug` and failed the plugin-missing check.

0. The blocker is reaching a loaded world, not the renderer. The harness now presses the attract key repeatedly and proves the world was reached by an actual camera find, but the game has still been observed sitting on "press any key" after ten injected presses. Until a run reports `worldReached=true`, every camera, mesh, scene, and terrain capture is capturing a menu. Options if injection keeps failing: have the user dismiss the menu by hand once per run, or drive the menu through a route that does not depend on synthetic input.
0d. **CONCLUSION: the camera at `BSGraphics::State +0x1B0` is not the primary world view.** Phase 10's live gate cannot be claimed from it, and locating the real world camera is a separate discovery task.

   The decisive comparison is between two states verified by screenshot: run `20260816-121505` was in a **fully loaded** Sanctuary (`world.png` shows first-person daylight Sanctuary with the HUD compass), and run `20260816-125221` was still on a **loading screen** (`world.png` shows a S.P.E.C.I.A.L. poster). The captured camera is byte-identical in both: `near=15 far=15000 fov=24`, `right=1,0,-0 up=0,0,1 forward=-0,1,-0`, translation zero. A camera that tracked the world could not be unchanged between a loading screen and gameplay. That contrast is stronger than the two-cells A/B originally planned, which is why it settles the question despite Diamond City never finishing its load.

   Corroborating: the rendered frame is plainly a wide view near the configured 70 degrees while the capture reports 24; the basis is exactly the world axes while the player faces down a street at compass heading 07; the position is the origin.

   It is a genuine engine camera, not noise — matrices self-consistent at residual 0, and `near=15.000` matching the logged `fNearDistance:Display` exactly — so a shadow or distant-LOD camera is the likely identity. The search must widen beyond `BSGraphics::State`; the world camera is not in that `0x3C0` block, which holds exactly one camera.

0c. **How the scan itself was fixed, and the evidence trail.** With `kMaximumCandidates` corrected the live scan works and is reproducible: `candidates=181`, `view-offset=0x1B0 projection-offset=0x1F0 residual=0 storage=column-major`, matching the offline dump analysis exactly. A run in a fully loaded Sanctuary (screenshot `artifacts/live/20260816-121505-camera-rotation/world.png` shows first-person daylight Sanctuary with the HUD compass) captured a camera reporting:

   `near=15.000 far=15000.0 fov=24.00deg position=0.0,0.0,0.0`, basis `right=1,0,-0 up=0,0,1 forward=-0,1,-0`.

   Strong circumstantial evidence that this is **not** the primary world view: the FOV is 24 degrees where the plugin's own startup log reports `fDefaultWorldFOV = 70.000` and `fDefault1stPersonFOV = 80.000`; the position is the origin; and the basis is exactly axis aligned while the player is visibly looking down a street at an arbitrary heading. Only one camera exists in the whole `0x3C0` state block.

   **But the designed discriminator did not fire, so this is not proven.** `player.setangle z 90` reported as sent, yet `after-captures.png` shows the identical view direction to `world.png` — same street, same tree, only Codsworth having wandered into frame. `player.setangle` does not reliably turn the player's view in this engine, so "the camera did not change" is fully explained by "nothing rotated". Do not record a negative result on this evidence.

   **Injected console commands do not reach this game at all.** `fov 100 100` was tried as a second discriminator and also produced no change in the rendered view (`world.png` versus `after-captures.png` in `artifacts/live/20260816-124716-fov-discriminator` are the same framing; a 70-to-100 degree change would be unmistakable). Both `player.setangle` and `fov` reported as sent and neither took effect, so `moveCommandSent` means only that keys were injected, never that the game acted on them. Any future in-run discriminator must not depend on injected console input.

   The one console mechanism that demonstrably works is `sStartingConsoleCommand`, because the engine executes it itself. That makes the definitive test a **two-launch A/B**: run once with `coc SanctuaryExt` and once with `coc DiamondCityMarket`, and compare the captured camera between the two. Different cells put the player at different positions and headings, so a camera that tracks the world must differ; a byte-identical basis across two unrelated cells is a verified negative that needs no injected input.

   **A/B run 1 result, and its caveat.** `coc DiamondCityMarket` produced a capture byte-identical to the Sanctuary one — `near=15 far=15000 fov=24`, `right=1,0,-0 up=0,0,1 forward=-0,1,-0`, translation zero. The caveat is that run B's `world.png` is a **loading screen** (a S.P.E.C.I.A.L. poster), so Diamond City had not finished loading in the 120 s settle; interior/urban cells load far slower than Sanctuary. The intended two-**loaded**-cells comparison therefore still has not run.

   It is nonetheless informative, because run A *was* verified loaded by its screenshot. So the same camera record appears, unchanged to the bit, both during a loading screen and during gameplay in a fully loaded Sanctuary. A record that does not move between those two states is not tracking the world.

   The settle is the weak link: a fixed delay cannot tell "loaded" from "still loading", and `worldReached` fires on the loading screen's own camera. A future run needs a real loaded-world signal rather than a timer — the simplest being to keep probing until a captured camera differs from the known loading-screen constant, with an overall deadline so it still fails closed.

   Supporting evidence already gathered, short of that test: the rendered frame is plainly a wide view (roughly the configured 70 degrees), while the capture reports 24 degrees — those cannot be the same camera. The basis is exactly the world axes (`right=+X, up=+Z, forward=+Y`) while the player faces down a street at compass heading 07. The capture is nonetheless a real engine camera, not noise: the matrices are self-consistent with residual 0, and its `near=15.000` matches the plugin's own logged `fNearDistance:Display = 15.000` exactly. A plausible identity is a shadow or distant-LOD camera rather than the main view.

1. **Find the real world camera.** Settled: it is not in `BSGraphics::State`, so widen the search. `vf_engine_analysis --scan-dump` already locates camera triples in a raw dump by self-consistency, so the cheapest next step is to dump a much larger region — or several candidate globals — and scan each. Good starting points are the other camera-shaped globals reachable from the 76 `lea` xrefs to `0x3D70920`, and `NiCamera`/`BSShaderManager` RTTI vtables via `--rtti`. Confirm any candidate by the FOV matching the logged `fDefaultWorldFOV`/`fDefault1stPersonFOV` and by the basis changing between two verified-loaded cells. Only then claim any part of the Phase 10 live gate.
2. Fix the mesh capture's world path. The hooked concrete TriShape boundary sees menu-path creation but produced nothing across a full cell load; find the creation path the world loader uses before claiming Phase 7 world coverage.
3. Implement the remaining capture kinds behind the existing request control: `material` (Phase 9, three SRVs plus constants at a draw), `scene` (Phase 11, from the batch/accumulator seam), cell deltas (Phase 12), and deformation inputs (Phase 13).
4. Resolve the `CreateShaderResourceView` hook contention between `WeaponDebris` and the texture capture with a shared dispatch rather than first-installer-wins.
5. Keep suppression off. Nothing here changes the rule that the mirror stays non-authoritative until Phase 25.

## Offline work: exact next actions

Resume Phase 15 (alpha-tested and two-sided visibility) in this order:

1. Re-read the Phase 15 definition in `docs/vulkan_renderer_implementation_plan.md` and inspect `EngineMaterial`'s alpha fields, the Phase 8 texture sampling path, and the Phase 11/14 raster classes before choosing the smallest vertical extension.
2. Write the Phase 15 red tests first. At minimum cover alpha-source selection, cutoff/reference, blend-versus-test classification, alpha-to-coverage if observed, dither/fade state, alpha-coverage-preserving mip thresholds, UV/clamp parity, front/back normal frames, negative determinant transforms, and depth-only versus color coverage. Record the red failure output before writing production code.
3. Implement the alpha-tested raster class, derived alpha-coverage mip metadata, two-sided frame handling, and identical material decode in depth and G-buffer passes. Keep sorted transparency out of this phase.
4. Add a replay mode and CTest that proves coverage masks agree over a mip-distance ramp and that depth and color silhouettes match, through the public ABI. Record the red failure first.
5. Run focused, full Debug, and full Release suites; create lossless artifacts and `docs/phases/phase-15.md`; update `engine_render.md`, the implementation plan, and this journal.
6. Also still open from Phase 14: batch compatible terrain cells into instanced draws without losing per-cell diagnostic identity.
7. The Phase 8-14 live promotion gates need the Address Library restored first (see below), and then explicit user approval before launching the game. Ask first; otherwise continue safe offline work.

## Important implementation pitfalls

- Never write a per-pixel G-buffer through a storage buffer from multiple draws. Storage writes from separate draws to the same pixel are unordered, so submission order changes the result even with `early_fragment_tests`. Phase 11 hit this exactly; color attachments are rasterization ordered and are the correct mechanism.
- `VkRenderingInfo` is reused for the geometry and tone-map passes in `RecordAndSubmit`. Reset `colorAttachmentCount` when swapping attachment arrays, or the tone-map pass reads past a single-attachment pointer.
- The Vulkan G-buffer planes must stay full precision (`R32G32B32A32`). Quantizing them would silently destroy the sub-1e-4 oracle comparison that proves parity.
- Do not duplicate coordinate conversion. `EngineView` owns it; `ProjectPacketForView` and `ProjectScenePacket` are the only projection oracles.
- The raster packet still stores one global vertex/index/draw/material set. Scene packets expand it per object through `drawIndex`; any further extension must be versioned and must retain old packet decode behavior.
- `GBufferPixelV1` is the interleave contract between four 16-byte GPU planes and the CPU oracle. `kSceneGBufferPlaneCount * kSceneGBufferPlaneSize == kGpuGBufferPixelSize` is asserted at build time from SPIR-V reflection; keep the plane order aligned with the record.
- The CPU oracle interpolates vertex color linearly in screen space while the GPU is perspective correct. They agree only when a triangle has constant `w`. The Phase 11 fixture keeps the rotated object flat-colored for that reason; do not add rotated interpolated-color geometry without accounting for the difference.
- Comparing float G-buffers by "differing pixel count" is meaningless because rounding touches every covered pixel. Gate on identity mismatches, maximum/mean absolute error, and exact interior (non-silhouette) agreement instead. With several instanced silhouettes an edge pixel can swing a full channel, so bound how many pixels may disagree rather than how far one edge pixel may move.
- A detached cell stops emitting its opaque pass. A mirrored frame that still claims that pass is correctly refused by Phase 11 pass accounting; rebuild the frame packet on a cell transition instead of weakening the accounting.
- `EncodeScenePacket` picks the minimum representable version: no instance records means version 1.0. Do not "simplify" this into always emitting 1.1 without re-baselining the Phase 11 artifact hashes.
- Never let a generation counter wrap. A wrapped generation lets a stale handle alias a live resource; exhaust the handle space instead.
- Registry delta traces record identity, content hash, size, generation, group, and timeline, never an engine address. Keep it that way when extending the record.
- A completed GPU timeline value retires every *earlier* allocation, not only the one with that exact value. Ring and retirement tests must encode that.
- Phase-specific ABI tests must assert `kBackendAbiMinor >= kBackendAbi<Phase>Minor`, never equality, so a later phase can append fields without re-baselining an earlier phase's test.
- The deformation fixture deliberately keeps geometry inside the frame. If a pose pushes vertices off screen the comparison starts measuring clipping parity instead of deformation parity.
- The deformed vertex buffer is compute output; binding it needs the VERTEX_ATTRIBUTE_INPUT barrier, and the raster path must switch both the buffer handle and the offset.
- Phase 10's `RenderCapturedMesh` frame rejection is deliberate because mesh translation currently normalizes source geometry to NDC. Do not silently remove the rejection until scene transforms define truthful coordinate ownership.
- The frame validator requires nonzero `engineFrameId`, capture sequence, render thread ID, and current thread ID; the two thread IDs must agree. Flags must be zero and at least one view is required.
- Pass category must equal `ClassifyPass(pass.domain, pass.writer)`; do not hand-author an inconsistent category.
- CRC is checked before semantic frame validation.
- Binding 6 must always be valid: identity/disabled constants preserve all Phase 6-9 behavior.
- `RasterFrameRequestV1` frame fields may only be read when `structSize >= 96`; scene fields at `>= 112` and G-buffer fields at `>= 128`. Old ABI prefixes must remain safe.
- A scene packet requires a captured frame; the scene vertex shader depends on binding 6 and the accounting depends on the frame's pass records.
- Exact float comparisons in `ProjectPacketForView` are intentional for a generated fixture; construct viewport values directly from the same width/height.
- Unprojecting existing desired NDC vertices through the inverse view-projection preserves their projected winding/front-face while proving the source data is no longer already in NDC.
- Vulkan tests require a usable Vulkan runtime. Validation output must remain zero.
- Do not weaken validation or increase image tolerance to make a test pass; diagnose the transform/layout mismatch.
- A captured view only round-trips its clip planes through float within a relative 5e-4 tolerance, which effectively caps the far/near ratio at roughly 1000:1. A fixture that needs to see a 4096-unit terrain cell must move both planes out together (Phase 14 uses near 4, far 4000), not just the far plane.
- Terrain quads are wound to a *negative* framebuffer-space signed area, so the terrain pipeline uses `VK_FRONT_FACE_COUNTER_CLOCKWISE` while the scene pipeline uses the opposite. A wrong front face culls all terrain silently and shows up as zero GPU coverage against a fully covered oracle, not as a shading difference.
- The Phase 14 reference rasterizer is perspective correct on purpose (interpolate attribute/w and 1/w, then divide; depth stays screen-space linear). Terrain spans large depth ranges where the Phase 11 screen-space shortcut would not hold.
- Rebuild `VisualForgeRenderer` after any change to a packet record layout. The DLL links its own copy of `vf_core`; a stale backend decodes new records at old offsets and reports a nonsense validation error such as "invalid flags".
- A submission helper must not reset the whole result struct if the oracle image was rendered into it first. Reset only the submission half, or the comparison silently degenerates to a size mismatch.
- Landscape textures are one captured `Texture2DArray`, and slot 3 (binding 12) is the only slot that accepts one. The slot and the captured dimension must agree in both directions so a layer array can never be bound where a flat texture is sampled.
- Never call a teardown helper from the middle of a build. `DestroyAccelerationStructures()` resized the bottom-level storage and also destroyed the transform buffer created and filled a few lines above, leaving every geometry's `transformData` pointing at freed memory. Geometry 0 survived on whatever the allocator had not reused yet and the rest were flung out of the scene, so exactly one object was ever traceable and no shadow appeared — with zero validation errors. Destroy precisely what is being replaced.
- The packet's vertices are **local space**. `scene.vert` multiplies them by the instance's model rows to reach the camera-relative space fragments are shaded in, so an acceleration structure built straight from them describes a scene with every object collapsed onto the origin. Vulkan's per-geometry `transformData` applies the same rows at build time; rows 0-2 of the record's row-major 4x4 are exactly a `VkTransformMatrixKHR`.
- A `VK_ERROR_DEVICE_LOST` reported at a fence wait names the *waiter*, never the operation that faulted. The failing command is somewhere in the submission and only bisection says where. Likewise, filtering tool output through `grep` for the strings you expect hides the ones you do not — the layers being silent was itself the evidence that the fault was in the GPU rather than the API.
- A term added to every lit surface re-baselines every earlier lit phase's colour artifact, and that is a re-baseline rather than a regression only if the earlier contracts still pass. Phase 19 moved Phase 18's; Phase 20 moved 17, 18 and 19's. The G-buffer artifacts never move, because they carry albedo and normals rather than radiance -- which is what makes them the useful thing to compare across phases.
- A stochastic estimator cannot be held to an absolute per-pixel bound. A ray-triangle hit decision at an edge is not bit-identical between a CPU Moller-Trumbore and the hardware intersector, and with eight rays one flipped ray moves a pixel by an eighth of the radiance behind it. Bound how many pixels may disagree, not how far one may move, exactly as the Phase 11 silhouette rule does.
- An acceleration-structure build reads the *same* index buffer the draw binds, so it must derive `indexType` from `packet.header.indexType` exactly as `vkCmdBindIndexBuffer` does. The packet's default is `IndexType::Uint16`; hardcoding `VK_INDEX_TYPE_UINT32` reads `{0,1,2}` as 65536, and the vertex fetch that follows walks megabytes past a 436-byte buffer. There is no validation error for this — the layers pass it, the build records, and the submission dies as `VK_ERROR_DEVICE_LOST` on the fence wait.

## Standard commands

Run from `F:\Development\fallout-mods\VisualForge`:

```powershell
cmake --preset vs2022-x64-debug
cmake --build --preset vs2022-x64-debug
ctest --preset vs2022-x64-debug --output-on-failure

cmake --preset vs2022-x64-release
cmake --build --preset vs2022-x64-release
ctest --preset vs2022-x64-release --output-on-failure
```

Useful focused discovery/filtering:

```powershell
ctest --test-dir out/build/vs2022-x64-debug -C Debug -N
ctest --test-dir out/build/vs2022-x64-debug -C Debug -L phase13 --output-on-failure
ctest --test-dir out/build/vs2022-x64-debug -C Debug -R "contract\.(raster_frame|textured_raster_frame|material_raster_frame|view_raster_frame|scene_raster_frame|instanced_scene_frame|deformed_scene_frame)" --output-on-failure
out/build/vs2022-x64-debug/tests/Debug/vf_unit_tests.exe "[phase13]"
```

## The live draw stream is measured (2026-08-16)

`src/EngineDrawCapture.h/.cpp` hooks `ID3D11DeviceContext::DrawIndexed`,
`DrawIndexedInstanced`, `IASetVertexBuffers`, and `IASetIndexBuffer` (context
vtable slots 12, 20, 18, 19, resolved from the same dummy device the texture
capture uses). It is off unless `VISUALFORGE_DRAW_CAPTURE` is set, because it
runs on the render thread once per draw; the whole hook body is a few relaxed
atomics with no allocation, no lock, and no logging.

Measured in Sanctuary, steady state, one frame each:

| Metric | Value |
| --- | ---: |
| Draw calls | ~4,270 |
| Instanced draw calls | 102 |
| Indices | ~9.8 million |
| Largest single draw | 65,211 indices |
| **Distinct vertex buffers** | **6** |
| Table overflows | 0 |

**Six vertex buffers, not thousands of meshes.** This is the finding that
makes a full-scene mirror tractable: Fallout 4 pools geometry into a handful
of large shared buffers and each draw indexes a range inside one, so capturing
the drawn world means capturing six buffers and a list of ranges rather than
resolving thousands of individual meshes. The existing one-shot
`EngineMeshCapture` cannot serve this — it writes a single mesh to a file and
retains nothing queryable.

The buffers behind those draws, described on first sighting in the same run:

| Buffer | Bytes | Stride | Usage | Bind | Draws |
| ---: | ---: | ---: | --- | --- | ---: |
| 0 | 134,217,728 | 12 | DEFAULT | VERTEX+INDEX+SRV+UAV | 4,522,775 |
| 2 | 134,217,728 | 20 | DEFAULT | VERTEX+INDEX+SRV+UAV | 327,695 |
| 1 | 1,747,616 | 16 | DYNAMIC | VERTEX | 14,045 |
| 3 | 67,600 | 16 | IMMUTABLE | VERTEX | 1,136 |
| 4 | 128 | 16 | IMMUTABLE | VERTEX | 1,136 |
| 5 | 16,384 | 4 | DEFAULT | VERTEX | 6,802 |

**Two 128 MB pooled buffers, strides 12 and 20.** That is a split vertex
layout — a position-only stream and an attribute stream — and it explains the
draw counts: essentially all geometry comes from buffer 0. They are
`DEFAULT` usage with `SHADER_RESOURCE` and `UNORDERED_ACCESS` bind flags, so
they are persistent GPU-side pools written when geometry streams in, not
rewritten per frame. 256 MB is far too much to stage-copy every frame and
does not need to be: the right move is to share them with Vulkan through
external memory once, or to copy only the ranges a frame actually draws.

Thirty-two distinct vertex-shader constant buffers were seen, nearly all
`DYNAMIC` with `WRITE_DISCARD`. Ranked by how often the engine maps them for
writing, the small ones dominate: **96 bytes mapped 2,035,508 times**, 128
bytes 988,884, 80 bytes 898,135. A buffer rewritten that often is per draw,
not per pass.

**The per-draw world transform is the first 64 bytes of that buffer, as a
row-major 4x4.** Sampled at `Unmap`, before the original commits, so the
bytes are the ones the engine actually wrote:

```
-0.700 -0.714  0.000   30.000
 0.714 -0.700  0.000  210.000
 0.000  0.000  1.000  -93.000
 0.000  0.000  0.000    1.000
```

A rotation about Z of roughly 134 degrees with translation (30, 210, -93) in
world units. The same leading matrix appears in the 80-, 96-, and 128-byte
variants, so it is one layout across shader permutations, and the 128-byte
one carries it twice (world and a second copy, likely previous-frame world
for motion vectors). Rows 0 to 2 are exactly what `scene::OpaqueObjectV1`
stores and exactly what `VkTransformMatrixKHR` wants, so no conversion is
needed beyond narrowing against the camera origin, which `EngineView` already
owns.

That closes the last unknown. All three pieces a full-scene mirror needs are
now identified rather than assumed: the geometry pool, the per-draw range,
and the per-draw transform.

## A live frame becomes a valid scene packet (2026-08-16)

`src/renderer_core/EngineDrawStream.h/.cpp` (namespace
`vf::renderer::drawstream`) translates a captured draw list into the scene
packet every later phase already consumes. Green at 4 cases, 44 assertions,
and the rules are the ones that matter rather than the ones that were easy:

- **Mesh identity is the pooled range, never the transform.** The engine
  pools geometry, so what identifies a mesh is which buffer and which range
  inside it. Folding the placement in would make every copy of a fence post a
  separate mesh and multiply the object count by however many are on screen.
  Mutation-verified: adding one transform element to the identity fails 2
  cases and 5 assertions, and turns 2 objects into 6.
- **Repeated meshes become instances, not objects.** Instances are emitted in
  one contiguous run per object because that is what the scene packet
  requires and what a single draw can cover.
- **A draw the capture never saw bound is a hole, not an object.** A zero
  buffer handle, an index count that is not a multiple of three (a strip, not
  a triangle list), a non-finite or singular transform, and a draw larger
  than the per-draw cap are all refused and counted.
- **Refused, not truncated, at the limits.** A scene that silently stops at a
  cap looks exactly like a scene that drew everything.

Run live in Sanctuary, one frame, translated by those rules:

| Metric | Value |
| --- | ---: |
| Draws recorded | 8,192 (arena cap) |
| **Objects** | **881** |
| **Instances** | **8,185** |
| Meshes reused | 7,304 |
| Draws rejected | 7 |
| `ValidateScenePacket` | **none** |

**A real Fallout 4 frame now produces a scene packet the engine's own
validator accepts.** Eight hundred and eighty-one distinct meshes placed
eight thousand times, which is the instancing the engine already found being
preserved rather than flattened.

One defect was found and fixed by that measurement. Taking the world
transform from "the last small constant buffer unmapped" was too loose:
shader variants map several small buffers per draw, so some draws were placed
by a matrix that was not a transform at all and the packet came back `invalid
transform`. The transform is now read only from the buffer bound at
**vertex-shader constant slot 0**, which is the buffer the draw will actually
read it from, and the packet validates.

The `dropped` figure in the log is cumulative between collections, not per
frame: the arena holds 8,192 records and a frame issues about 4,270 draws, so
a frame drained every frame drops nothing.

What the mirror still needs before it can draw a whole cell, in order:

1. ~~Record the draw list.~~ **Done.** A bounded arena of 8,192 records,
   written with one atomic increment per draw and no allocation, drained at
   Present and translated by `TranslateDrawStream`.
2. **Get the geometry into Vulkan.** The two pools are 256 MB of persistent
   `DEFAULT` memory. A per-frame copy of a quarter gigabyte is not a
   renderer, it is a memcpy benchmark — but the whole pool is not what a
   frame needs. Only the ranges the draws touch are, and those are cached by
   mesh identity because pooled geometry does not change once it has streamed
   in. `PlanMeshExtraction` and `VertexRangeForIndices` implement that and are
   green (6 cases, 100 assertions):
   - **A mesh is read once, ever.** Already cached, or already requested this
     frame, means no request.
   - **The frame has a budget**, in meshes and in bytes, because a readback
     from a `DEFAULT` buffer is a staging copy and a map, which synchronises
     the GPU. Meshes over budget are *deferred* and counted, so a scene still
     filling in says so instead of looking finished and being wrong.
   - **A rejected draw is never a request**, or a readback is spent on
     geometry the translation refuses anyway.
   - **The vertex window follows the indices**, not the pool: copying from
     zero to the highest index would move megabytes for a fence post. The
     arithmetic is signed throughout, so a negative `baseVertex` that would
     push an index below zero is refused rather than wrapping into a read of
     unrelated memory at the top of the address space. Mutation-verified:
     making that arithmetic unsigned fails the case.

   `src/EngineMeshExtractor.h/.cpp` performs the copy: a staging buffer,
   `CopySubresourceRegion` over the byte range, and a map, run at Present and
   never inside a draw hook. Indices are widened to 32-bit on the way out so
   nothing downstream can mistake a 16-bit pool for a 32-bit one, and they are
   rebased against the copied window so a mesh is self-contained and replayable
   without the 128 MB pool it came from. The index format is taken from
   `IASetIndexBuffer` rather than guessed: a wrong width reads every index at
   the wrong stride and produces geometry that is not merely misplaced but
   meaningless.

   **Run live, it works.** Eight meshes per collection, **zero failures**,
   cache growing steadily:

   | Collections | Meshes cached | Bytes |
   | ---: | ---: | ---: |
   | 1 | 8 | 36,556 |
   | 4 | 32 | 1,584,552 |
   | 16 | 128 | 3,693,304 |

   128 meshes cost 3.7 MB, so the frame's 881 meshes are roughly 25 MB —
   nothing, against a pool of 256 MB that never needed copying whole. The
   `deferred` figure is large only because this run drained every sixtieth
   frame; drained every frame at eight meshes each, a cell fills in about a
   hundred and ten frames.
3. ~~Translate the draw list.~~ **Done and validated on a live frame.**

All three inputs a full-scene mirror needs now exist and are proven against
the running game: **geometry** (extracted, CPU-side, self-contained),
**placement** (a scene packet the engine's own validator accepts), and the
**camera** (found on 2026-08-16, replaying with zero validation errors).

**The pooled vertex layout is measured, not assumed.** A mesh dumped straight
out of the extractor reads `stride=12 verts=3 idx=3 v0=0.01,0.00,0.00
v1=-0.01,0.00,0.01 i=0,1,2` — three floats per vertex, position at offset
zero, indices exactly as expected. That is the last layout unknown closed.

`AssembleSceneGeometry` builds the frame: the cached meshes concatenated into
one self-contained raster packet, one draw per object over the range that
mesh occupies in the concatenation, and the scene packet narrowed to the
objects that packet can actually draw. Green at 7 cases, 128 assertions.

**Both packets are rewritten together.** A cell fills in over many frames
because reading geometry is budgeted, so at any moment some objects have no
geometry yet. Those objects are removed and counted, not drawn empty — an
object whose draw index no longer matches its mesh places the *wrong* model,
which is worse than an unfinished cell. Mutation-verified: letting a missing
mesh borrow another object's geometry fails 6 assertions.

Attributes beyond position are left at their defaults rather than filled with
a guess, because a guess reads as real data downstream. So the first
full-scene mirror is untextured geometry at correct world placements, lit by
the captured lights — and it should be described that way rather than as a
finished frame.

**The mirror now draws captured game geometry, and only that.**
`BuildLiveSceneGeometry` in `RendererBackendProbe.cpp` collects the frame's
draws, translates them, reads back a budgeted slice of whatever geometry is
still missing, assembles the cached meshes, and submits the raster and scene
packets through the same path every offline fixture uses. Best run so far
submitted **637 objects, 291,919 vertices, 916,569 indices** from a live
Sanctuary frame.

**The synthetic mirror quads are deleted, not disabled.** They used to be a
fallback for when the live scene could not be built, and that was a mistake
worth recording: the captured artifacts then showed an earlier phase's test
fixture looking exactly like a rendered cell, and the only thing separating
"the renderer drew the world" from "the renderer drew four quads" was a log
line nobody reads next to the picture. The mirror now declines the frame and
leaves vanilla on screen, which is honest about having nothing to show.
`BuildMirrorGeometry` and its constants are gone from the file.

Three filters were needed before the engine's draw stream could be treated as
a scene, and each came from a specific failure rather than from caution:

- **A draw with no fresh world transform is not world geometry.** User
  interface and fullscreen passes bind small constant buffers and would
  otherwise inherit whatever matrix the previous world draw left behind,
  placing a screen-space quad somewhere in the cell. Per-draw constant
  buffers are written immediately before the draw that reads them, so a
  matrix-sized write since the last draw is the signal. Deciding by buffer
  size failed because the description table fills up; deciding by "is bound
  at slot zero" failed because the engine binds *after* it maps.
- **A mesh whose bytes do not read as finite positions is dropped, counted.**
  A pooled format this build does not understand yet would otherwise make the
  encoder reject the whole frame and cost every other object its render.
- **A transform the scene packet cannot carry is refused early.** It requires
  an affine bottom row and a *positive* determinant, so a mirrored placement
  cannot be represented. Discovering that at encode time cost the entire
  frame: `invalid transform`, 4,652 times in one run, for want of checking it
  where a single object could be dropped instead.

Where it stands: the frame renders genuine captured geometry, but the
transform filters are still rejecting most draws — the last capture put one
object on screen, a large untextured surface. The next step is measuring
*which* rule rejects what, because "reject and count" only helps if the
counts are read. Materials and textures remain uncaptured, so even a complete
cell will be untextured geometry lit by the captured lights until that lands.

Until those land, `--EnableMirror` draws the mirror's own geometry with the
real camera. That is what the per-phase capture below records, and it is
labelled as such rather than presented as a full scene.

## Per-phase in-game scene capture (required from 2026-08-16)

Every completed phase must leave an in-game scene, rendered through the Vulkan
renderer, as PNG in `artifacts/phase-NN/`. Run once per phase:

```powershell
tools/game_smoke/Export-PhaseScene.ps1 -Phase 18 `
  -GameRoot "F:\SteamLibrary\steamapps\common\Fallout 4"
# Re-export from a capture already taken, without launching the game again:
tools/game_smoke/Export-PhaseScene.ps1 -Phase 18 -GameRoot "<root>" `
  -ExistingCapture artifacts/live-phase-18
```

It launches the game through `Invoke-LiveCapture.ps1` with the mirror enabled,
so every safety invariant above still applies, and it writes three kinds of
image that are **not** interchangeable:

- `scene-live-slot<N>.png` — frames the Vulkan renderer produced *inside the
  running game*, driven by the live engine camera in slot N.
- `scene-mesh.png` — a mesh and texture captured from the running game,
  replayed through the Vulkan backend. Real game content, real renderer.
- `game-window.png` — a screenshot. Evidence of what was on screen, never
  evidence of what the renderer produced.

**State the limit rather than let the picture imply otherwise.** The live
frames draw the mirror's own geometry with the real camera; feeding a whole
visible cell to the backend needs the per-frame world capture the live
promotion gates still owe. `scene-mesh.png` is the closest thing to real
in-game content through Vulkan that exists today, and it is one mesh, not a
scene. When live promotion lands, these same images get richer without the
procedure changing.

The renderer writes PPM because a plugin the game loads must not carry an
image library. `tools/game_smoke/VfImage.ps1` converts to PNG by emitting the
file directly — `System.Drawing` is a Windows-only shim in PowerShell 7 and
does not resolve for compiled helpers here, and a capture run is far too
expensive to lose to a missing assembly. The first converter written for this
returned a fully black image and reported success; nothing about a wrong pixel
copy raises an error, so it was caught by converting a PPM whose correct PNG
already existed and comparing. Do that whenever the converter changes.

## Resume completion rule

Phases 10 through 13 satisfy their offline completion rules. Do not mark any of them live-promoted until mapped engine camera/pass capture, batch/accumulator scene capture, streaming/cell-transition capture, captured deformation inputs, and vanilla overlay evidence exist. Phase 14 is likewise incomplete until its terrain contract traverses capture, ABI submission, Vulkan rendering, CPU oracle comparison, Debug/Release regression, artifacts, and documentation; compilation alone is not completion.

## The vertex byte offset, and why one verified mesh proved nothing

**2026-08-16.** The mirror rendered captured game geometry as a fan of huge
black spikes radiating from a point. The geometry was genuinely live — the
extractor, the placement and the camera were all working — and the vertices
were being read from the wrong addresses.

`IASetVertexBuffers` carries three parallel arrays: buffers, strides and
**offsets**. The capture hook took the buffer and the stride from slot zero
and passed the offsets straight through to the original without recording
them. Fallout 4 draws every mesh out of one of two 128 MB pools, so that
offset is the mesh's address inside its pool. Dropping it made every readback
start at `poolBase + baseVertex * stride` instead of
`poolBase + offset + baseVertex * stride`, which returns whatever geometry
happens to occupy that address: real triangles, from a different object
somewhere else in the cell, connected by this object's indices.

The same omission produced a second, independent fault. `MeshIdentity` mixed
buffer, startIndex, indexCount, baseVertex and stride — every field except
the offset. Two meshes at different offsets in one pool therefore collapsed
onto a single identity, so the first one read was served for both.

**The pitfall is the verification, not the omission.** The journal recorded
the pooled layout as *measured, not assumed*, on the strength of one mesh that
dumped as `stride=12 verts=3 idx=3 v0=0.01,0.00,0.00 i=0,1,2` — every number
plausible. It was plausible because that mesh sat near offset zero, where the
missing term is zero. A sample of one cannot distinguish "the address is
right" from "the address is wrong by an amount that happens to be zero here",
and reading it as confirmation closed a layout question that was still open.
A second mesh from anywhere else in the pool would have failed immediately.

Fixed by carrying `vertexByteOffset` from the hook through `DrawRecordV1`,
`MeshIdentity` and `MeshExtractionRequest` into the readback address, with a
bounds check on `offset + range` against the pool. Mutation-verified: dropping
the offset from the identity fails the collision check.

## Half-precision positions, and the layout the engine already told us

The offset fix did not change the picture. The answer was already in the log,
written by a different capture path:

```
renderer-mesh-capture: observed desc=0x000BB00605430208 stride=32 attributes=7
... bounds-min=-2048,-2048,-580 bounds-max=0,0,348
```

Those bounds are a sane 2048-unit terrain quad, so that path decodes Fallout 4
vertices correctly. Decoding `0x000BB00605430208` by nibble: size `8*4 = 32`
(matching the reported stride exactly), UV at 8, normal at 12, tangent at 16,
colour at 20, landscape at 24, flags `0xBB` = VERTEX|UV|NORMAL|TANGENT|
COLORS|LANDDATA. **`VF_FULLPREC` is clear**, which means position is stored as
**four half-floats**, not three float32s — and `EngineVertex` already models
exactly that, as `VertexStorage::Half4`.

`AssembleSceneGeometry` never used it. It `memcpy`d twelve bytes and called
them a position. Half bit patterns reinterpreted as float32 land in the
denormal range, so nearly every vertex collapses onto the origin while the
occasional one becomes enormous: not geometry that is misplaced, but a fan of
spikes radiating from a point. That is the picture, exactly.

**The pooled draw stream has no engine descriptor**, only a D3D stride, and a
stride of 32 is equally consistent with four halves plus attributes and with
three floats plus attributes. So the layout is now taken from the engine's own
`CreateInputLayout` call: `ID3D11Device` slot 11 records the element
descriptions (D3D11 offers no way to read them back off an `ID3D11InputLayout`
afterwards), context slot 17 tracks which layout is bound, and the draw record
carries the handle. `BuildLayoutFromInputElements` turns the declared
semantics and DXGI formats into an `EngineVertexLayout`, which the existing
tested decoder then reads. A mesh whose layout was never seen is **declined,
not guessed at**.

Two pitfalls worth keeping:

- **A decoder that assumes either width is wrong for half the meshes.** Both
  occur in one game. `DecodeEngineVertex` now switches on the attribute's
  storage rather than its semantic.
- **The layout is part of a mesh's identity.** The same bytes read under two
  formats are two different meshes; without it in the hash, one is served for
  the other and correct data draws wrong.

Mutation-verified: decoding half4 positions as float3 fails 4 assertions;
accepting an attribute that runs past the stride fails the bounds check.

## The layout hook was installed too late, and the counters said so

The half-precision fix produced no picture at all on its first live run: zero
mirror frames, `reason=live-scene-unavailable`, and the camera plainly found
(six cameras, world camera at index 4). Rather than guess again, the mesh path
was instrumented — counts for layouts recorded, lookup hits, misses, layouts
that could not be built, and meshes declined for each distinct reason.

One run answered it:

```
objects=1294 usable=576 not-extracted=718 no-layout=0
layouts-recorded=43 overflow=0 hits=62383 misses=0 unbuildable=0
```

**`engine_draw_capture::Install()` ran inside the first `Present`.** Fallout 4
builds its vertex formats during renderer initialisation, long before a frame
is presented, so `CreateInputLayout` — which only reports what is created after
it is hooked — saw none of them. Every mesh was correctly declined as
undecodable, and the mirror correctly declined the frame. The behaviour was
right; the timing was wrong. Installing at plugin load, from the dummy-device
vtable that was already resolved there, fixed it: 43 layouts, 62,383 lookups,
**zero misses**.

Confirmed in-engine: a power-armour figure with its weapon, correct silhouette
and perspective, drawn from live captured geometry. Flat-shaded, because the
raster vertex carries no normal and materials are still default.

Two pitfalls worth keeping:

- **A creation hook only sees what is created after it.** Anything the engine
  builds during initialisation is invisible to a hook installed at first frame,
  and the symptom is not an error but an empty result.
- **"Unavailable" is not a diagnosis.** Geometry not read back yet and a mesh
  with no recorded layout look identical from outside and have entirely
  different fixes. Counting them apart turned a second guessing session into a
  single run.

`not-extracted=718` is the readback budget filling the cell in over frames, not
a fault. The mirror now asks for 64 meshes and 8 MB of indices per frame rather
than the contract default of 8 and 1 MB: the index-byte limit was what actually
bound the rate, and at the default a screenshot taken at any normal moment
showed a half-empty world that read as missing geometry.

## An interrupted run left settings in the player's configuration

A run killed part-way through never reaches its `finally`, so an interruption
after the INIs were edited left `sStartingConsoleCommand=coc SanctuaryExt` and
five disabled autosave keys in the player's real `Fallout4Custom.ini`. Every
later run then backed that file up as though it were the original and
faithfully restored the leftovers; no clean backup survives in `artifacts/`.

Fixed by making the repair survive the process rather than depend on it. The
harness writes a restore journal to `%LOCALAPPDATA%\VisualForge\
restore-pending.json` **before** its first edit, and clears it only once every
file is actually back. The next run completes any unfinished restore before
taking its own backup, so a backup can no longer capture the previous run's
leftovers. A missing backup file is reported rather than acted on, because
restoring from nothing would delete a file the player still has.

The existing pollution is left in place at the user's request.

## Per-vertex normals, and a mutation that exposed a toothless test

Live geometry rendered flat because the raster vertex had no normal: the
fragment shaders took `objectRecord.geometricNormal`, the per-object axis
`ModelUpAxis` derives from the captured transform. Every surface of an object
therefore shaded as one plane, which makes phases 17 to 20 meaningless on
captured geometry -- shadows, reflections and indirect all start from N.

`RasterVertexV3` adds it, as minor version 2, with readers for both older
layouts and a writer that still emits them, so an old fixture round-trips. A
new field is a new minor version: encoding forty-eight-byte vertices under the
old number hands a reader a stride it walks at thirty-two, which is not a
decode failure but a scene of garbage. Both migrations default the normal to
+Z rather than zero, because a zero normal is not "no lighting" but a division
by zero wherever the shading normalises it.

Threading it through cost more than the packet change:

- `scene.vert` rotates the normal by the instance's upper 3x3, exact for the
  rigid and uniformly scaled placements the draw stream admits.
- `scene.frag`, `alpha_scene.frag` and `family_scene.frag` all take it, with
  the same length threshold, so a mesh without usable normals falls back on
  every path at the same moment. `alpha_scene.frag` needed its *shading*
  normal changed too: taking the vertex normal for one and the object record
  for the other left the two describing different surfaces on one pixel.
- The CPU oracle interpolates it barycentrically and applies the same model
  rotation. Interpolating and forgetting to rotate lights every rotated object
  as though it had never turned.
- **`deform_layout.glsl` still declared the thirty-two-byte vertex.** A stride
  that disagrees with the host struct neither fails to compile nor trips
  validation; the compute pass just writes each vertex at the wrong address,
  and it reads as deformation being broken. Caught by
  `max-position-error=0.95` in the phase 13 contract.

**The instructive part is a mutation that passed.** Reverting
`family_scene.frag` to the per-object normal broke nothing, because the
fixtures' vertex normals had been set equal to the rotated object normal --
correct, but it makes the two paths indistinguishable, so those contracts had
no teeth for this change at all. A fixture that cannot tell two code paths
apart tests neither. `P11_the_gbuffer_normal_follows_the_vertices_not_the
_object` leans the three corners in different directions and asserts the
g-buffer holds more than one normal, that at least one differs materially from
the object's declared axis, and that all of them stay unit length. That one
does fail when the oracle ignores the vertex normal.

## Phase 21 GPU vertical: the scene had to carry its blend state first

The composite could not be written until the frame carried what to composite
with. Transparency is a property of the frame rather than of the material --
the same material is drawn blended in one pass and opaque in another -- so the
blend state has to travel with the capture or the composite invents an order
and the layers land wrong.

`ScenePacket` now carries a transparent draw table at minor version 3, its
count and offset carved out of the last reserved qword exactly as the
visibility pair was carved out of the one before it, so the header still
occupies exactly 96 bytes and no earlier field moves. A scene with no blended
draws still encodes at the older version, so every capture taken before the
section existed round-trips byte for byte instead of acquiring an empty
section it never had. A reader below 1.3 refuses a packet that declares the
section rather than reading those bytes at whatever they used to mean, and a
draw naming an object the scene does not have is refused at encode, where the
only other symptom would be an effect that never appears.

`shaders/phase21/transparency.glsl` mirrors `EngineTransparency.cpp` function
for function -- the five blend modes, the soft fade in view-space units rather
than depth-buffer difference, and the dissolve with its smooth and hard cuts.
An unrecognised blend mode leaves the destination alone on both sides: falling
through to a blend would put an effect on screen under a rule nobody chose.

`TransparentAlpha` composes the fade and the dissolve into the contributed
**alpha**, not the colour. Scaling colour instead makes an additive effect dim
toward its own hue rather than disappear, so a fading fire goes dark red
instead of going away. The two reductions multiply, because two independent
reasons to be less present do not each get the whole budget.

Mutation-verified: encoding the new section at the old minor version fails the
round-trip, and taking the minimum of fade and dissolve instead of their
product fails the composition case.

**Still outstanding for this phase:** the backend does not yet consume the
table -- sorting, the depth-test-without-depth-write pass state, and the
decal stencil path are unwritten, so no GPU contract exercises the new shader
yet. The packet and the shader contract are the prerequisites, and they are
done and tested; the pass that reads them is not.

## The Phase 21 backend pass, and a contract with almost no teeth

The backend now consumes the transparent table. After every opaque draw it
builds the blended list, refuses any draw the contract rejects (an
unclassified effect keeps its vanilla path rather than being drawn under a
guessed rule), sorts it with `blend::SortsBefore` -- **the contract's own
comparator, so the backend and the oracle cannot sort differently and disagree
about which layer is on top** -- and draws each through a pipeline chosen by
its blend mode.

Two things the pass gets right that are easy to get wrong:

- **One pipeline per blend mode.** Blend factors are pipeline state in core
  Vulkan, not dynamic state, so a single pipeline cannot serve additive and
  multiply. Building all four up front is what keeps the pass from creating
  pipelines inside a frame.
- **Only the HDR attachment is written.** A transparent fragment in the
  G-buffer would make the reflection and indirect passes treat a particle as
  an opaque surface, and every ray behind it would stop there. That needs
  `independentBlend`: without it every attachment must share one blend state.
  The feature is now requested, and where it is absent the blended pipelines
  are simply not built and the pass declines, rather than compositing
  particles into the surface data the ray passes read.

Depth is tested and never written, because writing occludes the transparent
draws behind it and the layer collapses to whichever was drawn first.

**The contract test is weak, and saying so is the point.** Three mutations all
passed:

- reversing the composite order -- the fixture has one blended draw, so order
  cannot be observed;
- letting blended draws write depth -- one draw has nothing behind it to
  occlude;
- making additive replace instead of add -- replacing is still brighter at
  enough pixels to satisfy the gate.

The gate asserts only that the pass composited something and that validation
is clean. A stronger one was tried first -- additive can only brighten, so a
darker pixel proves the blend wrong -- and it had to be withdrawn: the CPU
reference does not model transparency at all, so it was measuring the
pre-existing divergence between oracle and device rather than the blend. The
exact bound needs a second device render differing only in the transparent
table, the way the shadow and bounce terms are isolated, plus a fixture with
at least two overlapping draws in different modes so order is observable.

Phase 21 is therefore **implemented but not properly tested**. The packet,
the shader contract, the pipelines and the pass exist and run clean; the
evidence that they are correct does not.

## The Phase 21 gate, made exact — and what it caught

The weak gate was replaced by differencing **two device renders** that differ
in exactly one thing: the same frame is submitted again with the transparent
table removed. Additive can only brighten what is behind it, so against that
baseline the bound is exact rather than approximate. The oracle-parity checks
read the baseline render instead of the composited one, because the CPU
reference does not composite at all and would otherwise report the layer as an
oracle disagreement.

**It immediately found that the pass had never drawn anything.** With the
exact bound in place the first run reported `transparent-brighter=0
transparent-darker=0` -- the two renders were identical. The blended draw is
coplanar with the opaque geometry it sits on, so a strict less-than depth test
rejected every one of its fragments. The pass ran, reported no error, produced
no pixels, and the earlier weak gate had called that a pass on the strength of
127 pixels that were nothing but pre-existing oracle/device noise.

Fixed by admitting the tie: a blended draw coplanar with its surface -- a
decal, a scorch mark -- must not be rejected by an exact-depth equality, and
blended draws never write depth, so there is no z-fighting to cause. The pass
now composites 8,844 pixels, none of them darker.

Two of the three mutations that previously slipped through now fail:

- additive replacing instead of adding: **caught**;
- the pass never running: **caught**;
- the G-buffer left writable: **still passes**, because the fixture's blended
  draw uses the same material and geometry as the object beneath it, so
  writing the G-buffer again writes almost the same values. Catching it needs
  a blended draw whose surface data differs from what it covers.

Sorting is still uncovered: one blended draw cannot show an order, and two
additive draws commute. A fixture with a straight-alpha draw over an additive
one at different depths would show it.

The lesson is the same one the fixtures taught earlier, in a sharper form: a
gate that cannot fail is worse than no gate, because it is reported as
evidence. This one was reporting a pass for a feature that was drawing
nothing at all.

## Two blended layers, and the coverage that still is not there

The single-draw fixture was replaced with `AppendTransparencyFixture`: two
blended quads in front of the opaque scene, one additive and one straight
alpha, at different depths and with roughness deliberately unlike the surfaces
they cover. The intent was to close the two gaps the mutations exposed --
sorting, and the G-buffer write mask.

**Neither closed, and both were checked rather than assumed.**

Reversing the sort produces a byte-identical frame. With the layers apart on
screen there is no overlap for an order to matter in; moved on top of each
other, one of them falls behind the opaque geometry and fails the depth test,
so again only one draws. Two layers are necessary for order to be observable
but not sufficient: they have to overlap *and* both survive the depth test,
and this fixture's geometry does not do both at once.

Leaving the G-buffer writable still passes, even with the blended objects
carrying a different roughness. The blended draws land where the comparison
does not look, so the mask is still unproven.

Recorded because the alternative is worse. A `transparent-blue-bias` metric
was added on the theory that compositing the blue alpha layer last would show
in the colour balance; measured, it reads identically with the sort reversed,
so it observes nothing and is kept only as a diagnostic. Believing the theory
and gating on it would have produced a third gate that cannot fail.

Phase 21's contract covers two properties -- the blend arithmetic (additive
never darkens, measured against the same device rendering the same frame
without the table) and that the pass composites at all. Sorting and the
G-buffer mask are **not** covered, and the fixture work needed to cover them
is geometry placement that this attempt did not get right.

**A third placement attempt failed too**, and the pattern in the failures is
the useful part. Concentric quads at z=1.00 and z=0.80 -- both nearer than the
nearest opaque object at z=1.30, which should have put them in front of
everything -- drew nothing at all, while the original pair at z=0.90 and
z=0.60 draws 4,053 pixels. That is the opposite of what a near-plane or a
depth-test explanation predicts, so the mental model of this fixture's camera
is simply wrong, and three rounds of moving coordinates on that model produced
no coverage.

The next attempt should read the fixture's view matrix and near/far planes and
place the quads from that, rather than guessing offsets and measuring what
comes out. Guessing was cheap the first time and has now cost three rounds.

## The root cause was never the geometry

Three rounds of moving the blended quads around produced no coverage, and the
reason was not placement at all. `AppendTransparencyFixture` pushes its quads
as ordinary scene objects, so **the opaque pass drew them as well** -- in both
the composited render and the baseline. The blended pass then composited each
quad over an opaque copy of itself, which cancels to nothing, and moving them
around only changed which of them was hidden behind which.

The fix is a rule the backend needed anyway: an object the transparent table
claims is drawn by the blended pass **and by nothing else**. Drawing it opaque
first writes its depth and its G-buffer, so the blended draw that follows
composites over itself -- and a blend that cancels out is indistinguishable
from a pass that never ran, which is exactly how this hid for three rounds.

With that in place the sort became observable, and the contract gained a
property that needs no knowledge of the right answer: **the composite must not
change when the transparent draws are listed in the opposite order**, because
the pass sorts them itself. A third device render with the table reversed must
come out pixel for pixel identical.

Two more corrections fell out of it:

- Blended geometry is no longer an occluder, on either side. A particle or a
  pane of glass casting a hard opaque shadow is wrong, and it reads as the
  shadow pass being broken rather than as the wrong geometry being in the
  acceleration structure.
- The transparency contract is scoped to lit-only. Inheriting the shadow gate
  meant inheriting a ratio bound tuned for a three-object scene, which a
  fixture that adds two more trips by a single pixel -- reporting a shadow
  regression that is really just a larger scene.

**What the order property does and does not catch.** Removing the sort so the
draws are taken in capture order: caught. *Reversing* the comparator: not
caught, because both renders then sort the same wrong way and still agree. The
property proves the pass sorts by something other than arrival order; it does
not prove the direction. Proving the direction needs the composite compared
against `blend::Composite` applied in known order, which is not built.

Cost of the detour worth recording: the third render initially inherited the
baseline's G-buffer pointer and silently overwrote the surface data the
oracle-parity comparisons read, so the contract failed with every printed
metric reading clean. Four rounds of printing single variables found it; one
round printing every clause of the pass expression at once would have.

## Phase 21 GPU contract: four properties, all mutation-verified

The last gap closed with a property that needed no new fixture work, only
noticing where nobody was looking: **no pixel of the composited G-buffer may
carry a blended object's identity.** The parity comparisons read the baseline
render, so the composited render's surface data was never examined by
anything; leaving the mask off wrote 13,858 pixels of particle into it and no
gate saw them.

Phase 21's GPU contract now covers:

| Property | Mutation that fails it |
| --- | --- |
| Additive never darkens what is behind it | additive replaces instead of adding |
| The pass composites at all | the blended pipelines are never built |
| The draws are sorted, not taken in arrival order | the sort is removed |
| Blended geometry stays out of the G-buffer | the write mask is removed |

The direction of the sort is still unproven: reversing the comparator leaves
both renders sorting the same wrong way, so they agree and the
order-independence property is satisfied. Proving direction needs the
composite compared against `blend::Composite` applied in a known order.

A grep lesson, cheap but real: `grep -E "tests passed"` matches "100% tests
passed" and "0% tests passed" equally, and it reported a mutation as caught
when it had not been, and as uncaught when it had. Anchoring on the percentage
is the difference between a mutation report that means something and one that
is noise.

## Phase 22: the water shading contract, both sides

`shaders/phase22/water.glsl` mirrors `EngineWater.cpp` for the terms a water
or glass surface actually shades with: Schlick with F0 derived from the index
of refraction, Beer-Lambert underwater fog, the shallow-to-deep depth ramp,
and the combination of the two sides.

`ShadeWater` is the new piece on both sides, and its rule is that **the
reflected and transmitted shares are a partition of one**, not two
independently tuned terms. Anything else creates light at grazing angles --
where Fresnel approaches one and a separately authored transmission is still
adding -- or loses it head-on, and both read as the water being the wrong
colour rather than as the split being wrong.

Two details that are easy to get backwards:

- **F0 comes from the index of refraction when the capture has one**, and from
  the authored bias only when it does not. Preferring the bias makes every
  surface reflect like the one material that came without an index.
- **Fog applies to the transmitted side only.** Fogging the reflection dims
  the sky by the depth of the water underneath it, which is wrong at every
  depth and most obviously wrong where the water is deepest.

Mutation-verified: dropping the `(1 - fresnel)` complement fails 17
assertions; skipping the fog fails the transmitted-side case.

**Not done for this phase:** the backend has no water pass. The refraction
source -- reading the prior colour target rather than sampling the surface
being drawn -- and the planar reflection path are unwritten, so nothing on the
GPU consumes this shader yet. As with Phase 21 the contract and the shader
come first; unlike Phase 21 the pass that reads them does not exist, and no
GPU contract covers it.

## Phase 22 GPU: the refraction source snapshot

The backend now takes a copy of the colour target as the opaque pass left it,
before any blended draw, and binds it at scene descriptor binding 19. A
refractive surface samples that copy rather than the live target, so what
shows through it does not depend on which refractive draws happened to precede
it -- two panes of glass would otherwise show each other, and which one won
would change with the sort.

The mechanical detail that shapes the pass: **a copy cannot be recorded inside
a render pass instance.** The scene pass therefore ends before the blended
draws, records the copy, and resumes with `LOAD` on every colour attachment
and on depth. A resumed pass that clears instead discards everything the
opaque pass just drew, which is the single easiest way to get this wrong and
produces a frame containing only the blended layer.

The binding sits outside the `VF_RAY_QUERY` guard, unlike the acceleration
structure and the geometry table: a refractive surface reads what is behind it
whether or not the device can trace. Without ray query the reflection falls
back; the refraction does not.

Evidence: 341 tests green with zero validation errors, and the four Phase 21
transparency properties still hold -- which is what proves the split pass did
not disturb the frame, since a resumed pass that cleared would fail every one
of them.

**Not verified: the contents of the snapshot.** Nothing samples it yet, so a
copy that wrote the wrong region, or nothing at all, would pass unnoticed.
That is the same shape of gap the transparency contract had before its
properties were written, and it closes the same way -- by having the shader
consume it and asserting a refractive surface changes when the geometry behind
it changes. The shader side needs the transparent table's `domain` and the
water material reaching the fragment stage, which is not plumbed.

## Phase 22 GPU: the refractive path runs, and what that does not prove

The shading path is wired end to end. `ScenePushConstantsV1` grew a
`refractive` flag and a resolved index of refraction -- carried per draw
rather than per object, because the same mesh is drawn refractive in one pass
and opaque in another -- and `family_scene.frag` splits a refractive surface
between what it reflects and what it lets through using `vfWaterShade`, with
the transmitted side sampled from the snapshot.

Two Vulkan details that cost a cycle each:

- **An image still `UNDEFINED` at submit is a layout error even when nothing
  reads it.** Every scene pipeline references binding 19 because the shader
  mentions it, whether or not a given draw is refractive, so the refraction
  source has to be made readable once and left that way between the copies
  that refill it.
- The additive-only "never darker" bound broke the moment the fixture gained a
  refractive layer, because a refractive surface legitimately darkens. Rather
  than weaken the bound, the harness now renders the frame a **fourth** time
  with the refractive draws removed: differencing that against the baseline
  isolates additive blending exactly, and differencing the full composite
  against it isolates the refraction.

**The refraction property is weaker than it looks, and the mutation says so.**
Replacing the sampled snapshot with a constant black still passes: the
refractive draw continues to draw, so the composite still differs from the
additive-only render. What the property proves is that the refractive path
executes -- not that it reads what is behind it. Proving that needs a fifth
render in which the geometry behind the refractive surface differs, asserting
the refracted pixels move with it.

Phase 22 GPU status: snapshot, binding, push constants, shader path and one
property, all green with zero validation errors across 341 tests. The property
that would make the snapshot's contents load-bearing is not written.

## Making the refraction snapshot load-bearing

The property that finally discriminates: **of the pixels the refractive layer
covers, most must move when the geometry behind them is recoloured.** Getting
there took three wrong versions, and each was wrong in a way worth keeping.

1. *"The composite differs from the additive-only render."* Passes with the
   sampled snapshot replaced by constant black, because the refractive draw
   still draws. It proves the path executes, nothing more.
2. *Recolour every material.* Passes with the mutation too -- the recolour hit
   the blended quad's **own** material, so its pixels changed through its own
   shading and every covered pixel counted as reading the snapshot. A
   measurement that reports 100 per cent under both the correct code and the
   mutation is measuring the fixture, not the renderer.
3. *Straight alpha for the refractive layer.* Also passes: `dst * (1 - a)`
   carries the background through the hardware blend whether or not the shader
   reads anything. The layer has to be **premultiplied** for the snapshot to
   be the only route from the geometry behind to the surface.

With all three fixed the counts separate cleanly: 8,559 of 12,076 covered
pixels move with the background when the snapshot is read, against 1,536 when
it is not -- 71 per cent against 13. The residual is the quad's edge, where
partial coverage lets the blend leak the destination through no matter what
the shader does, so the bound is a majority rather than a total.

Phase 22 GPU contract, all mutation-verified: the refraction snapshot is taken
before any blended draw, the refractive path runs, and **it reads what is
behind it**. 341 tests green, zero validation errors.

The recurring shape, now three times in this phase and twice in Phase 21: a
property that passes under its own mutation is measuring something other than
what it claims. Running the mutation is not a formality at the end -- it is
the only thing that distinguishes a gate from a number.

## Phase 23, and a build hazard that invalidates mutation runs

The tone map pass now shares its arithmetic with the post contract instead of
restating it: `shaders/phase23/post.glsl` holds the Reinhard curve and the
sRGB transfer function, and `tone_map.frag` includes it. Exposure reaches the
curve as a push constant, applied **before** the curve so the whole range
shifts -- scaling after tone mapping compresses first and brightens second,
which lifts black instead of exposing the image. It is one for now, because
the contract's `AdaptExposure` has no home on the device and a value invented
in the backend would be a second source of truth for it.

The contract property compares the device's 8-bit output against
`post::ToneMap` and `post::ApplyOutputTransform` applied on the host to the
same HDR pixels the device read -- the whole output stage against its oracle.
One code of slack, because the device encodes in half precision and rounds in
fixed function, so an exact match would assert bit-identical arithmetic across
two machines rather than the same curve. Measured: **zero differing pixels**.

**The hazard: shader include dependencies were not tracked.** Each shader's
`DEPENDS` list is hand-maintained, and a missing include does not fail the
build -- it silently keeps the previous SPIR-V. A mutation to `post.glsl`
therefore reported a **false pass** twice in a row: the shader was never
recompiled, so the device was still running the unmutated code. It was only
visible because two different mutations both passed when one of them plainly
should not have; touching the dependent `.frag` by hand proved it, and the
same mutation then failed with 3,132 differing pixels and a ten-code error.

Fixed for this include by adding it to the `DEPENDS` list, which is how the
existing `view_transform.glsl` is handled. The general fragility remains: every
shared include has to be listed against every shader that includes it, by
hand. The robust fix is `glslc -MD` with `add_custom_command(DEPFILE ...)`, so
the compiler reports its own includes.

**This casts doubt on any earlier mutation run whose target was a shared
`.glsl` include.** The mutations recorded for phases 17, 19 and 20 were run
against those files; the ones for 21 and 22 targeted `.cpp` and `.frag` files
and are unaffected. Re-running the shared-include mutations is outstanding
work, and until it is done those three phases' mutation evidence should be
treated as unverified rather than as passed.

## Re-running the shared-include mutations, and what they found

With every shader now depending on every shared header -- a `file(GLOB
CONFIGURE_DEPENDS)` over `shaders/*/*.glsl`, applied to all thirteen shader
rules -- the mutations that had been run against those headers were repeated.
Over-depending rebuilds more than strictly necessary and cannot be silently
wrong, which is the right trade for a list nobody remembers to update.

| Phase | Mutation | Result |
| --- | --- | --- |
| 17 | the shadow ray query never proceeds | **caught** |
| 19 | Fresnel returns F0 flat | passes, output byte-identical |
| 19 | the mirror direction returns the view vector | passes, output byte-identical |
| 20 | eight indirect rays reduced to one | **caught** (2,092 differing against a bound of 466) |

Phase 17 and 20 hold up. **Phase 19 does not**, and the byte-identical output
is the tell: the rebuild demonstrably happened -- the `.spv.inc` is newer than
the mutated header -- so the mutated code is running and changes nothing. The
reflection direction and the Fresnel weight are not load-bearing in that
fixture.

Reading the contract explains it. `reflected-pixels` is computed by
differencing two **oracle** renders, `unreflectedHdr` against `unshadowedHdr`.
It measures whether the reference reflects, not whether the device does, so it
cannot fail because of a reflection bug on the GPU -- and the remaining bound,
`hdr-differing`, does not move because the device's reflection contributes
nothing measurable to this fixture in the first place.

That is the same shape as every other gap this session: a number that looks
like evidence and is measuring something else. Phase 19's GPU reflection is
**unverified**, and the fix is the one Phase 22 needed -- a property that
isolates the device's reflection term by differencing two device renders, plus
a fixture where a wrong reflection direction has somewhere visibly wrong to
point.

## Correcting the Phase 19 finding, and pinning it down

The previous entry blamed `reflected-pixels` for differencing two oracle
renders. That is true and worth fixing, but it was not the whole story, and
two of the three mutations behind it were badly chosen:

- `vfMirrorDirection` is used only in the **skipped** branch, when the surface
  is rougher than the cutoff, and in the rejected-sample fallback. The traced
  path takes its direction from `vfSampleReflectionDirection`. Mutating it and
  concluding "the reflection direction is not load-bearing" was wrong: it was
  a mutation aimed off the hot path.
- Flattening `vfFresnelSchlick` to F0 moves the result by less than the
  comparison's slack at the incidence angles this fixture presents.

Probing it properly settles it. Forcing `vfReflection` to return a constant
changes 13,689 pixels and moves the maximum error to 50.6, so the function is
called and its value reaches the frame. But **deleting the reflection ray's
`rayQueryProceedEXT` loop changes nothing at all** -- byte-identical output.
The rays are traced and miss everything, so the traced branch and the miss
branch return the same thing, and only the environment path is ever verified.

Why they miss: the reflective plate faces the camera exactly, so its mirror
direction points back at the space between itself and the viewer -- which is
necessarily empty, because anything there would occlude the plate. Turning the
plate by 0.9 radians did not fix it either; the scene's other objects sit
behind it, and the reflected direction still finds nothing.

Two placement attempts, no coverage, which is the Phase 21 trap repeated. The
fix is not another guess: compute the mirror direction from the plate's actual
orientation and the camera, and place a bright object along it. Until then
**Phase 19's traced reflection is unverified** -- the environment fallback is
what its contract covers.

## The Phase 19 probe, and what it exposed

Rather than guess at geometry a fourth time, the replay now probes the fixture
with the contract's own tracer: for every surface smooth enough to trace, it
casts one ray along the mirror direction and reports hits. The numbers are on
every reflection run as `reflection-probe-rays` and `reflection-probe-hits`.

It reports **2 rays, 0 hits**, which settles the question the mutations could
not: nothing in this fixture is positioned where a reflection can find it. The
traced branch and the miss branch therefore return the same value, and any
mutation of the traced path passes. The probe also showed the reflection
geometry contains only **four triangles**, so most of the scene is not
reflectable at all.

With the measured directions in hand -- object zero at (-0.35, 0.05, 2.0) with
a mirror direction of (-0.172, 0.025, -0.985) -- a target placed one unit
along that ray produced a hit on the first attempt, against three failures by
guesswork. Measuring beats guessing, and the probe is cheap enough that it
should have come first.

**What the hit exposed immediately:** `reflection-max-delta` rose from 0.23 to
1.92, and the device and the oracle disagreed on 755 pixels against a bound of
466. That divergence has been invisible for the whole life of this contract,
because the rays never hit anything for the two implementations to disagree
about.

The target is reverted so the suite stays green, and the probe is kept: it
passively reports zero hits on every run, which is the standing evidence that
this contract does not yet test what its name says. Re-applying the target and
diagnosing the 755-pixel divergence between `vfReflection` and
`EvaluateReflection` is the next step, and it is a real disagreement to
resolve rather than a tolerance to widen.

## Diagnosing the Phase 19 divergence: not a reflection bug

Re-applying the target and measuring across three configurations settles what
the 755 pixels are:

| Target | probe hits | hdr-differing | hdr-max-error |
| --- | ---: | ---: | ---: |
| none | 0 | 273 | 1.2788 |
| bright (4.0) | 1 | 755 | 1.2788 |
| dim (0.9) | 1 | 540 | 1.2788 |

**The maximum error is identical in all three.** Whatever produces the largest
single disagreement between device and oracle is already present without the
target and is untouched by it. What the target adds is a larger count of
*small* differences -- 273 to 540 -- against a bound of 491, which is ten per
cent over.

That is the signature of the stochastic indirect term over a slightly larger
scene, not of a reflection that is computed differently. The shader traces
indirect for every lit surface whenever ray query is available, so adding an
object adds bounce light everywhere, and a bound calibrated on three objects
is measured against four. It is the same shape as the shadow-interior
threshold that read 41 against a bound of 40 when the transparency fixture
grew.

The target is reverted rather than the bound widened. Widening it would hide
the pre-existing 1.2788 as readily as the new noise, and that maximum error is
still unexplained -- it predates this work and no contract currently isolates
it.

What is now known, and was not before:

- the traced reflection path **can** be exercised, with a placement measured
  from the mirror direction rather than guessed;
- doing so does not reveal a reflection implementation difference, because the
  worst disagreement does not move;
- the obstacle is that this contract compares a frame containing a stochastic
  term against an absolute per-pixel bound, so any change to the scene moves
  the count.

Isolating the reflection term with an extra device render -- reflections on
against reflections off, both on the device, the way Phase 21 and 22 isolate
their terms -- is what would make this contract able to test what it claims.
That is the outstanding work, and it is now specified rather than guessed at.

## Why the Phase 19 reflection cannot be isolated yet

The isolation was built as specified -- a second device render carrying a
bright target one unit along the measured mirror direction, differenced
against the ordinary render, with the target's own pixels excluded so the
count means "light the reflection carried" rather than "a new object is on
screen". It does not work, and the mutation says why:

| Measurement | reflection on | reflection ray disabled |
| --- | ---: | ---: |
| all pixels except the target's | 151 | 147 |
| only pixels on a smooth surface | 135 | 131 |

Four pixels of difference either way. **The target's light reaches the frame
almost entirely through the indirect bounce, not through the reflection**, and
filtering to the reflective surface does not help because that surface
receives the bounce too.

The blocker is concrete: the shader traces indirect for every lit surface
whenever ray query is available, and there is **no device-side switch to turn
it off**. Isolating a reflection means rendering the same frame twice with
only the reflection changing, and that is not currently expressible. The CPU
oracle has `inputs.indirectEnabled`; the device has no counterpart.

The isolation attempt is reverted -- a gate that cannot fail is worse than no
gate, and this one would have passed with the traced branch deleted. What
remains is last cycle's probe, which reports `reflection-probe-hits=0` on
every run and is honest standing evidence that the traced path is untested.

The work this needs, in order: a scene-level flag that disables indirect on
the device, mirroring `indirectEnabled`; then the two-render isolation, which
is already written down and was demonstrated to run; then the target, whose
placement is already computed. None of it is guesswork now.

## A device-side switch for the bounce

`EnvironmentIndirectDisabled` (bit 2 of the environment record's flags) turns
the diffuse bounce off for a frame, on both sides: `EvaluateIndirect` returns
before tracing, and `vfIndirect` returns black before it does. The flag rides
the environment record because that is already a per-frame structure both the
oracle and the shader read, so it needed no new binding, no packet migration
and no push constant.

It is a renderer feature, not test scaffolding. Every other ray-traced term
can already be rendered with and without on the device, which is what lets a
contract difference two frames and attribute the change to one term. The
bounce could not, and that is precisely why the reflection isolation failed:
the target's light arrived through the bounce as well as the reflection, both
moved together, and neither could be measured alone.

The rule the tests pin down is that it returns **exactly nothing**, not a
small residue. A term that is almost off still moves every pixel it touches,
and an isolation built on one measures the remainder rather than the term it
meant to remove.

Mutation-verified: the oracle ignoring the flag fails 3 assertions, and giving
the flag the same bit as `EnvironmentPresent` fails 6 -- the second matters
because a frame that merely has no environment must not read as one that
switched the bounce off, or a capture could not tell them apart.

This unblocks the Phase 19 isolation, which is already written and was
demonstrated to run: render the frame twice with a reflection target, holding
`EnvironmentIndirectDisabled` in both, and the only thing left that can move a
pixel on the mirror is the reflection.

## Four attempts at the Phase 19 isolation, and a decision to stop

With `EnvironmentIndirectDisabled` available the isolation was built exactly as
specified: two device renders differing only in a reflection target, both
carrying the flag, counting pixels on a smooth surface that moved between
them. It reports **3 pixels**, and disabling the reflection ray leaves it at
**3**. Those pixels are the target's *shadow* falling on the mirror, not its
reflection.

Four attempts now, none of which produced a working property:

1. guessed placements, three of them -- rays missed;
2. differencing with the target, whole frame -- measured the bounce;
3. the same, filtered to smooth surfaces -- still the bounce;
4. the same with the bounce switched off -- measures the shadow.

Each attempt removed one confound and revealed the next. That is progress of a
kind, but it is slow progress against a fixture that was never built for this,
and the pattern says the fixture is the problem rather than the measurement. A
scene with one small mirror facing the camera, in a frame that also carries
shadows and a bounce, has no clean signal to isolate.

**Stopping here deliberately.** What this needs is a purpose-built reflection
fixture -- a large mirror, an unmistakable target squarely in its reflected
direction, no other lights, no shadow caster, and the bounce off -- rendered as
its own contract rather than as another rider on the family scene. That is a
different piece of work from adding one more measurement to this one, and
attempting it as a fifth increment would repeat the same shape again.

The scaffolding is reverted. What stands is the probe, reporting
`reflection-probe-hits=0` on every reflection run, and `EnvironmentIndirect
Disabled`, which is a genuine renderer capability the frame needed regardless
and is mutation-verified on both sides. Phase 19's traced reflection remains
**unverified**, and the honest reading is that its contract tests the
environment fallback and always has.

## The purpose-built mirror fixture: built, geometry verified, still not reflecting

`--render-mirror-scene` is the fixture the previous four attempts said this
needed: two objects and nothing else, one directional light, the bounce
switched off, rendered twice with and without a target, counting the mirror's
own pixels that moved.

The geometry is computed and then **checked against the renderer's own
records** rather than assumed:

```
mirror normal=-0.707,0,-0.707 centre=0,0,3
target normal=1,0,0          centre=-1.5,0,3
```

A mirror at (0,0,3) turned 45 degrees has a reflected direction of exactly
(-1,0,0) for a view direction of (0,0,-1), and the target sits along it facing
back at +X. The mirror covers **10,167 pixels**, so there is plenty of surface
for a reflection to land on. `reflected-pixels` is nevertheless **0**.

Two fixture faults were found and fixed on the way, both by measurement:

- the frame declares a distant-tree opaque pass that nothing in a two-object
  scene draws, and an uncovered opaque pass is refused outright;
- the target was first placed lying in the plane z = 3, which is the plane the
  reflected ray travels **within** -- parallel to the triangle, so no
  intersection was possible however bright it was.

Neither fixed the result. What remains untested is whether the reflection ray
runs at all in this mode: forcing `vfReflection` to a constant does not move
the metric, because the metric differences two renders that both carry the
forced value, so that probe cannot answer the question it was aimed at.

The mode is kept and deliberately **not registered as a contract**: it builds,
runs clean with zero validation errors, and reports `reflected-pixels=0`,
which is honest standing evidence rather than a passing gate. Registering it
would add a red test; deleting it would throw away a fixture whose geometry is
now verified correct.

The next diagnostic is the one this cycle got wrong: compare the mirror's
pixels against a render with reflections suppressed **in that render alone**,
rather than differencing two renders that share whatever the shader does.

## The mirror fixture, cycle six: one real fix, one confounded metric

**Found and fixed:** the mirror mode supplied no family packet, so
`phase16FamilyActive` was false and the frame rendered through the plain
phase-11 scene shader -- which never calls `vfReflection` at all. Roughness
changed nothing and the traced path agreed with the fallback because neither
of them ran. A family record must also name an object the scene owns, so the
packet is built per variant; the variants differ by exactly one object.

That is a genuine root cause, found by asking the right question -- "does the
mirror's appearance depend on its roughness?" -- rather than by moving
geometry again.

**Still not reflecting.** `reflected-pixels` is 0: adding the target, and then
enlarging it fourfold, changes nothing on the mirror. Whatever the reflection
returns, it is not the target.

**And the new metric is confounded.** `traced-vs-fallback` compares a smooth
mirror against a rough one, and roughness changes the direct specular
highlight as well as the reflection path. Its 7,610 pixels may be measuring
the highlight. It is reported, not gated on, and it is not evidence of tracing
until the direct term is held constant -- which needs the two variants to
differ in the reflection alone, the same problem in a smaller frame.

Six cycles on this contract. The honest position has not moved: **Phase 19's
traced reflection is unverified.** What has accumulated is real -- the
`EnvironmentIndirectDisabled` capability, the shader include-dependency fix,
the probe, and now the family-packet requirement -- but none of it is the
property, and the property is what was being sought.

The remaining unknown is narrow and stated: does the reflection ray intersect
anything at all in this fixture? The way to answer it is the CPU tracer, which
`BuildReflectionGeometry` and `reflect::TraceReflection` already provide and
which reported hits correctly in the family fixture. Running that over the
mirror scene's own triangles, before involving the device, would settle in one
step what three device-side attempts have not.

## The mirror fixture proves the device wrong

The host tracer settles what three device-side attempts could not:

```
mirror-pixels=10167 reflected-pixels=0
host-trace-hit=1 host-trace-distance=1.5
```

`reflect::TraceReflection`, the contract's own tracer, fires a ray from the
mirror's centre (0,0,3) along its mirror direction (-1,0,0) and **hits the
target at distance 1.5**, exactly where the geometry puts it. The target's
world corners are (-1.5,-3,0), (-1.5,3.6,3), (-1.5,-3,6): a plane at x=-1.5,
squarely across the ray.

So the fixture is correct and the ray is reachable, and the device's
reflection still does not change when the target is added or when it is
enlarged fourfold. That is no longer an unproven contract -- it is a
**discrepancy between the host tracer and the device**, reproducible in a
two-object scene with one light and the bounce switched off.

Both numbers are now reported side by side on every run, which is the useful
form: `host-trace-hit=1` beside `reflected-pixels=0` is the evidence, and it
cannot be mistaken for a fixture problem the way six cycles of ambiguous
results were.

The next question is narrow and answerable: does the device's acceleration
structure contain the target at all? The backend builds one bottom-level
geometry per drawn object, and the mirror scene has two draws; a build that
skipped the second, or a transform that placed it elsewhere, would produce
exactly this. `blasPrims` and the geometry-to-object table are already logged
by the backend for the shadow work, and reading them for this scene is the
next step rather than another fixture change.

## A real device defect, isolated

Four facts, each measured, that together characterise it:

1. **The target is in the acceleration structure.** The backend now logs what
   it built: `acceleration plans=2 objects=2 instances=0` for the two-object
   variant. Both objects have a bottom-level geometry.
2. **The host tracer hits it.** `reflect::TraceReflection` fires the same ray
   from the same origin along the same direction and reports
   `hit=1 distance=1.5`.
3. **The device reaches its hit branch.** Forcing that branch to return a
   constant changes 10,163 of the mirror's 10,167 pixels, so the ray is
   committing an intersection on almost the whole surface.
4. **The target is not what it hits.** Adding the target, and enlarging it
   fourfold, leaves `reflected-pixels` at 0. In the variant without a target
   the only geometry present is the mirror itself.

Taken together the device's reflection ray commits an intersection in a scene
whose only other object it demonstrably should reach, and the result does not
depend on that object existing. The ray is hitting the mirror it started from.
A non-zero `tmin` of 1e-3 does not change the result, so this is not ordinary
self-intersection at the ray origin -- it is structural, and the candidates
are the bottom-level transform placing a geometry where it does not belong, or
the geometry-to-object mapping resolving a hit to the wrong record.

This is the first defect in this area with a **minimal reproduction**: two
objects, one light, the bounce off, and a single ray whose expected answer is
known independently from the host tracer. Six cycles of ambiguity in the
family fixture produced nothing this sharp, because every result there could
be blamed on the scene.

The backend's new `acceleration plans=` line stays. A reflection that finds
nothing has two possible causes -- the ray missed, or the geometry was never
there -- and without that line they are indistinguishable from outside.

Phase 19 is not merely untested: there is a defect underneath it, and the
mirror fixture is the thing that will confirm the fix when it lands.

## Correcting the previous entry: hit versus miss is still undetermined

The previous entry claimed the device reaches its hit branch, on the evidence
that forcing that branch to a constant moved 10,163 mirror pixels. **That
inference was wrong.** The measurement it came from compares the smooth mirror
against the roughened one, and those two differ whether the ray hits or
misses -- the rough variant takes the skipped branch either way. It could not
distinguish the two cases, and it was read as though it had.

Colouring the branches apart -- hit red, miss green -- was the right test and
gave `reflected-pixels=0` with `traced-vs-fallback=10167`, which says both
variants take the **same** branch as each other but not which one. Reading the
image would settle it, and the mirror mode does not write one: it compares
buffers and reports numbers, and the `--output` path it accepts is ignored.

So the honest state of the defect is narrower than last time:

- the target **is** in the acceleration structure (`plans=2 objects=2`);
- the host tracer **does** hit it (`hit=1 distance=1.5`);
- the device's result **does not depend** on the target existing;
- whether the device's ray hits anything at all is **undetermined**.

Two cheap things would close it, in this order: have the mirror mode write its
PPM so the branch colours can be read directly, and log the `source` the
shader reports -- it already computes `kVfReflectionGeometry` versus the miss
values and nothing reads them back.

The lesson repeats in a new place. A measurement that moves when the thing
under test changes is not evidence that the thing under test is what moved it;
the rough variant differs from the smooth one for several reasons at once, and
only one of them was the question.

## The reflection ray self-intersects, measured directly

Returning the hit's own values as colour -- `vec3(distance, geometryIndex,
objectIndex)` -- and reading the mirror's mean answers in one run what six
cycles of differencing could not:

```
mirror-mean = 0.053, 0.057, 0.070
```

Hit distance **0.05**, geometry index **0**, object index **0**. The
reflection ray commits an intersection five hundredths of a unit from its own
origin, against the mirror it started from. Every later measurement was
downstream of that: the target could not be reached because the ray never got
past the surface it left.

Reading the branch as a colour was the technique that worked. A difference
between two renders tells you they differ; returning the quantity under test
tells you what it is. That should have been the first move rather than the
twentieth.

Raising the ray's minimum distance to 0.1 clears the self-hit -- and then the
ray **misses everything**, `traced-vs-fallback` falling to 0. So the
self-intersection was masking a second fault rather than being the whole of
it: with the surface no longer in the way, the reflected direction still does
not find a target that the host tracer reaches from the same surface.

The full suite passes with the larger minimum, so the change is safe in the
sense of breaking nothing -- but it is **not committed**, because a magic
0.1 in camera-relative units is a number that happens to work at this scale
and would be wrong at another. `vfOffsetRayOrigin` already exists to push the
origin off the surface proportionally, and it is what should be doing this
job; why an offset along the geometric normal leaves the ray able to re-enter
the same triangle is the question to answer before choosing a constant.

Two faults are now separated and each has a measurement that shows it. That is
what the mirror fixture was built for.

## The acceleration structure does not match the rasterised geometry

Every input to the reflection ray was read back individually, by returning it
as colour and averaging over the mirror:

| Quantity | Measured | Correct? |
| --- | --- | --- |
| shading normal | (-0.707, 0, -0.707) | yes, and not flipped by `faceSign` |
| ray origin | the fragment's own `vertexCameraRelative` | yes |
| direction | the exact mirror direction, and forcing it changes nothing | yes |
| hit distance | 0.05 | -- |
| hit geometry / object | 0 / 0, the mirror itself | -- |
| hit position | (0.199, -0.333, 2.922) | on the rasterised mirror |

The direction has a component of **+0.707 along the surface normal**, so the
ray travels away from the rasterised plane and is 0.035 clear of it after
0.05 units. It cannot be on that plane. Yet it commits an intersection with
that object there.

The only remaining explanation is that **the bottom-level geometry is not
coplanar with the triangle the raster pass draws.** The transform path is the
suspect: `object.model`'s first twelve floats are copied into a
`VkTransformMatrixKHR`, which is correct only if that layout is row-major
three-by-four in the same basis the vertex shader uses -- and the vertex
shader builds its position from `instanceRecord.modelRows` via three dot
products, which is a different code path reading a different buffer.

Two paths place the same geometry and only one of them is checked by any
contract. That is the defect, and it is not specific to reflections: shadows
trace against the same structure, which makes the Phase 18 result worth
re-examining under this fixture too.

Every earlier symptom follows from it. Rays "missing" the target in the family
fixture, the traced branch agreeing with the fallback, six cycles of
measurements that moved for the wrong reasons -- all of it is downstream of an
acceleration structure whose contents do not sit where the picture does.

## Retracting the previous root cause: the acceleration structure is correct

The previous entry concluded that the bottom-level geometry is not coplanar
with the rasterised triangle, and named the transform path as the defect.
**That was wrong**, and the test that refutes it is the one that should have
been run before writing it down: aim the ray directly at the target's known
centre instead of along the mirror direction.

```
ray aimed at (-1.5, 0, 3):  distance 1.93  geometry 1.06  object 1.07
```

It hits the target -- geometry index one, object index one -- at the distance
the host tracer predicts. The acceleration structure holds both objects, in
the right places, reachable from the mirror's surface. The transform path is
fine.

So the reasoning that produced the retracted conclusion was sound in every
step but one: a ray leaving a plane cannot re-hit that plane, therefore the
plane must be elsewhere. The measurement says the plane is where it should be,
which means one of the premises is false -- and the untested premise is that
the direction actually used is the one I computed. Every check of the
direction so far has been indirect: substituting `vfMirrorDirection` changed
nothing, which shows the sampler is not responsible but not that the value is
what I think.

The next measurement is therefore the direction itself, returned as colour the
way the normal, origin, distance and hit position already were. That technique
has answered every question in this fixture on the first try, and it is the
one thing about this ray that has still never been read back.

A note on method, since this is the second retraction in three cycles: an
inference from geometry is not a measurement. Both retractions came from
reasoning that was locally correct and rested on an unmeasured premise, in a
fixture where measuring is cheap.

## Three silent edit failures, and a statistic that cannot answer the question

Two process faults, both of which produced measurements that were reported as
results:

1. A `perl` substitution replacing the sampled direction with the mirror
   direction **did not match**, so the shader was never changed. The output
   was byte-identical to the unmodified run, and that identity was read as
   "the sampler is not responsible" -- a conclusion drawn from an experiment
   that never ran.
2. The `sed` that replaced it left a stray brace. The shader **failed to
   compile**, the build reported one error, and the run used the previous
   SPIR-V -- again producing the same number, again looking like a result.

Both were caught only by printing the edited lines and the error count before
trusting the measurement. That is now three times in this session that a shader
change silently did not take effect: the include-dependency gap, and these
two. **A shader edit is not applied until the file has been read back and the
build has reported zero errors.**

With the edit verified applied, the direction returned is
`(0.465, -0.569, 0.673)` -- but that figure cannot support any conclusion,
because it is the **mean of ten thousand unit vectors**. A mean is a valid
statistic for the hit distance and the object index, which is why those
readings stand; it is meaningless for a direction field that varies across the
surface. Averaging reflected directions over a curved fan yields a vector
shorter than one that points nowhere in particular, which is exactly what
`length(0.465, -0.569, 0.673) = 0.99` fails to reveal only by coincidence.

What still stands, because it was measured with a statistic that suits it:
nearly every ray hits object 0 -- the mirror -- at a mean distance of 0.05.
What does not stand is any statement about the direction.

The fixture needs a single-pixel probe: the mirror's centre fragment, whose
expected reflected direction is exactly (-1, 0, 0) by construction. One pixel,
one vector, compared against a number known in advance.

## In-engine verification: the extraction pipeline is healthy

A live run with the current build, `artifacts/live-normals`. Safety clean:
`success: true`, no new save files, no restore errors, no forced kill.

```
renderer-meshes: objects=1602 usable=1602 not-extracted=0 no-layout=0
                 misses=0 unbuildable=0 layouts-recorded=42
```

**Every object the frame draws is captured, decoded and usable.** Before this
session's fixes the same measurement read 576 usable of roughly 1300 with 718
not extracted. The difference is the vertex byte offset carried through to the
readback address, the input-layout hooks installed at plugin load rather than
at first Present, and the readback budget raised to 64 meshes and 8 MB of
indices per frame. That is the first in-engine confirmation of any of them.

The captured frame nevertheless shows a single power-armour figure against the
clear colour, where vanilla shows the whole of Sanctuary. The log explains it
without ambiguity:

```
[01:32:41] renderer-mirror: source=live-scene objects=16 vertices=47182
[01:32:41] frame written  [01:32:57] frame written  [01:32:59] frame written
[01:35:23] renderer-draws: objects=1602 instances=3407
```

Three frames were dumped, all within eighteen seconds of the load, and none
afterwards -- the dump fires on a camera-slot change. The artifact is a
snapshot of a cell that had sixteen objects in it at the time, while the
renderer went on to carry sixteen hundred. **The sparse picture is a capture
timing artifact, not a rendering fault**, and no amount of looking at that PNG
would have said so; the counters did.

The fix is in the harness rather than the renderer: dump a frame at the end of
the settle period, when the cell has finished streaming, instead of only when
the camera slot changes. Until that lands, every mirror artifact in
`artifacts/` should be read as an early-load snapshot rather than as the
settled scene -- which recasts the earlier "one object, 426 vertices" and
"spike fan" captures as well.

## The mirror renders the cell from the world origin

Nine live runs, `artifacts/live-unreadable` through `artifacts/live-distribution`.
All safety clean: `success: true`, no new save files, no restore errors, no
forced kill, Fallout 4 re-enumerated as absent before each launch.

### What the diagnostics were hiding

The previous entry's fix -- dumping on a cadence rather than only on a camera
slot change -- landed, and the dumps still read `objects=8`. That looked like
`AssembleSceneGeometry` discarding 1,594 of the 1,602 usable meshes. It was
not. `AssemblyResult` counts `unreadableMeshes` and nothing reported it; with
it reported the answer was `objects=4 missing=0 unreadable=0`, and the mesh
counter beside it read `objects=4 usable=4`. **The assembly never received the
1,602 meshes.** Every dump had fired at frames 120 through 600, which is the
loading screen; the cell does not appear until forty seconds later. The
assembly was not dropping anything and never had been.

The dumps then stopped entirely once the cell loaded, and nothing said why,
because `decline()` shared one `s_mirrorFaultLogged` flag with three other
fault sites. The mirror always declines its opening frames -- there is no
world camera yet -- so the flag was spent on a startup reason before the run
began, and every later failure was silent. Split into one flag per site plus a
last-reason string for `decline`, the missing line appeared immediately:
`render failed diagnostic=invalid raster frame contract`, at the exact second
the cell finished loading.

That diagnostic named neither the offending field nor a size. The rejection
was `packetSize > kMaximumPacketBytes`, a fixed 64 MiB. Measured, a Sanctuary
exterior cell encodes to **104,938,288 bytes** -- 1,810,514 vertices at 48
bytes plus 4,482,732 indices at 4. A real cell had never fitted.

`kMaximumPacketBytes` is gone. The ceiling is now `MaximumPacketBytes(
reportedDeviceAllocationBytes)`, derived from the device's own
`maxMemoryAllocationSize`: a packet has to fit in one allocation, so that is
the only bound here that is neither arbitrary nor a guess, and Vulkan
guarantees at least 1 GiB. Five properties, three mutations, all caught
(dropping the floor clamp, shrinking the floor below a measured cell,
`max` to `min`). A sixth assertion was deleted rather than kept: it checked
the ceiling against `size_t`, which on the only word size this builds for can
never fail.

With the ceiling right, the full cell renders: **1,598 objects, 1,810,514
vertices, 4,482,732 indices**, written from the world camera.

### Where the picture actually goes wrong

The frame shows the whole cell squeezed into a thin horizontal band. Measuring
rather than guessing, in order:

- The captured view matrix carries `view-translation=[0,0,0]`.
- The view-projection is a valid matrix: rows decode to a 50.6 degree vertical
  field of view, 16:9, forward `(0.7066, 0.7066, 0.0384)`, and a fourth column
  of `(0, 0, -15.0006, 0)` -- an infinite far plane with near at 15.0006,
  matching the reported near of 15.000. Multiplied as a transpose it puts
  every instance behind the camera, so the convention in use is the right one.
- The scan's hardcoded `MatrixStorage::RowMajor` in `BuildFrameSeries` is
  **not** a bug: `CameraStateScan` stores `columnMajor` pre-transposed, so its
  output is already canonical whichever storage it picked.
- Only 8 of 3,333 instances land inside the frustum, all at `ndc y = 0.080`
  and `ndc z = 1.000` exactly. `0.0568 / 0.7066 = 0.0804` is what that ratio
  becomes when the vertical component is zero, and only 96 instances have one.

The measurement that settles it is the distribution, which every earlier
bounds line had missed by reporting extremes over a mixed population -- those
describe the furthest single object, not the scene:

```
loading screen  distance p10=394.5    p50=396.2    p90=425.2
                vertical p10=-39.0    p50=4.4      p90=9.8    below-eye=5
settled cell    distance p10=117361.1 p50=120267.6 p90=122592.4
                vertical p10=7545.4   p50=7836.9   p90=7991.6 below-eye=0
```

In the cell every instance is about 120,000 units from the origin and 7,800
above it, p10 to p90 spanning four per cent. A scene centred on its camera
spreads from the object underfoot out to distant terrain; a tight cluster
120,000 units out means **the origin is not the camera**. The instance
transforms are absolute world coordinates and the view matrix is
camera-relative, so the mirror draws Sanctuary from the world origin, where it
subtends a band at the horizon. That is the picture, exactly.

This also retracts an inference made earlier in the same investigation. A
nearest instance at `[0,0,1]` was read as proof of camera-relative transforms;
that sample came from the loading-screen frame, where it is true and
irrelevant. In the cell, `below-eye=0` across 3,365 instances -- there is no
ground beneath the player, which no camera-relative frame can produce.

### What this makes task 20

The engine's world matrices are absolute and its view-projection is
camera-relative, which cannot both feed one shader: Fallout 4 subtracts a
per-frame camera position in the vertex shader. Closing this needs that value
captured from the per-frame vertex constant buffer and carried into
`ViewRecordV1::cameraRelativeOrigin`, the field that already exists for it
beside the `ViewCameraRelative` flag that is already set. Estimating it from
the geometry would be a guess and is not the fix.

Standing rule reaffirmed, having cost four silent failures this session: a
`perl` or `sed` edit is not applied until the file is read back. The
`unreadable` counter that opened this entry was written twice, because the
first attempt reported a clean build over an unmodified file.

## Sanctuary, from the engine, through Vulkan

The cell renders. `artifacts/live-density`, safety clean: `success: true`, no
new save files, no restore errors, no forced kill.

```
renderer-origin-select: status=0 at=[-80352.0,89600.0,7904.8] nearest=386.26
                        neighbours=1643 index=132 candidates=146
renderer-distribution:  distance p10=1495.5 p50=4178.5 p90=9080.7
                        vertical p10=-357.4 p50=-67.9 p90=90.5 below-eye=2482
renderer-projected:     ndc x=[-0.993,0.997] y=[-0.319,0.713] on-screen=2616
```

Against the previous entry's measurements: median distance 120,268 to 4,185,
median vertical 7,837 to -67.9, instances below eye level 0 to 2,482, instances
landing on screen 8 to 2,616 of 3,349. The captured frame shows pitched roofs,
a bare tree, a treeline, a hillside and ground -- Sanctuary, untextured and
unlit because neither is wired into the mirror path yet, but unmistakably the
cell the player is standing in.

### Recovering the camera position

Fallout 4 publishes absolute per-object transforms and a view matrix with a
zero fourth column. Reconciling them needs the camera's world position, and it
is recovered rather than estimated, because an estimate misplaces the whole
cell by however far it is out.

`RecoverCameraOrigin` solves for the position that turns the camera-relative
view-projection into a translated one: the candidate's first three columns must
match, which leaves four equations in three unknowns, fitted by least squares
with a residual. Five properties, four mutations caught. One property records
what the residual *cannot* do, which is most of what it looks like it does: in
a projection times a rotation the third and fourth rows have parallel linear
parts, so the system has a one-dimensional left null space and only an error
along it produces any residual at all. A test asserting a caught forgery was
written first, failed, and was replaced by two -- one for the direction that is
caught and one for the direction that is absorbed into the answer instead.

Measured live, no candidate in the camera record is a translated
view-projection: `found=no` across 131 candidates. The engine stores the
position as three floats, not as a second matrix.

So the record's position triples are collected and the frame's own geometry
decides between them, in `SelectCameraOrigin`. The first rule tried was "the
camera has geometry at it", which is true -- in first person the view model
sits at the eye -- and useless: the record is full of zeroes, a frame contains
a few identity-placed quads at the origin, and an all-zero candidate scores a
perfect nearest distance. Measured live, it chose `[0,0,1]` with
`nearest=0.00`. The rule that works is density: the cell is built around the
player, so the camera is the candidate with the most instances within one cell
of it, and the handful of quads at the origin cannot outnumber a cell. Three
mutations caught; one "survivor" was a false result from a build that failed on
an unused parameter and left the previous binary in place -- re-run compiling,
it was caught.

### Where the narrowing belongs

The first attempt narrowed instances in the backend and in the reference
rasteriser. Eleven contract tests failed, and they were right to: the scene
packet's contract is that instance transforms are *already* camera-relative,
which is why `cameraRelativeOrigin` narrows only lights and terrain, the two
things that arrive absolute. Narrowing downstream would make every consumer
responsible for knowing which kind of capture it had been handed. Both changes
were reverted and the narrowing happens once, where the live packet is built.

`NarrowInstance` moves only the translation -- rotation belongs to the object,
and turning it would turn the whole cell with the player -- and moves the
previous transform by the same origin, because motion vectors are the
difference of the two and narrowing one alone would smear the frame with a
hundred thousand units of false motion. Four mutations caught.

Its precision claim was wrong on first writing and is corrected in place. Two
floats near a hundred and twenty thousand subtract exactly even in single
precision, so nothing is saved on the object positions; the saving is that the
origin is a double, and rounding it to float before subtracting discards about
eight thousandths and shifts the whole cell whenever the camera moves far
enough to land on a different float. The test now uses an origin a float cannot
represent, which is the only case where the two arithmetics differ.

Three hundred and forty-six tests pass.

## Phase 20's temporal half, on the device

The diffuse ray pass had a GPU side; reprojection, accumulation and variance
did not. `EngineIndirect` had all three on the host, `phase20/indirect.glsl`
had none of them, and the scene shader traced with a frame index of zero and
no history at all. A traced pass with no accumulation is not the phase.

Now `phase20/accumulate.comp` runs one invocation per pixel over three storage
buffers and writes a fourth, and `contract.indirect_accumulation` compares
every result against `gi::Reproject`, `gi::Accumulate` and `gi::Variance`:

```
indirect-replay extent=8x4 reason-mismatches=0 length-mismatches=0
                mean-mismatches=0 variance-mismatches=0
                max-mean-error=5.96e-08 max-variance-error=5.96e-08
                reasons-exercised=6 validation-errors=0 result=pass
```

One float ULP across thirty-two pixels, with six of the seven rejection
reasons reached. The replay compares whole histories rather than a picture,
because accumulation has no picture of its own: it decides how quickly one
appears, and a pass that blended a rejected sample would look like a slightly
soft frame rather than like a trail behind everything that moved.

### What the mutations changed

Four mutations, all initially survivors, and three of them survived because the
fixture was weak rather than because the gate was:

- Blending a rejected sample instead of resetting. Caught.
- Flooring the motion sum instead of truncating the motion first. Survived: the
  only non-zero motions in the fixture were an integer and an off-screen one,
  and the two agree everywhere except on fractional negative motion. A pixel
  with a motion of minus one and a half now separates them, and the two pixels
  it names hold different histories so the choice changes an answer.
- An absolute depth tolerance instead of a relative one. Survived: every depth
  pair in the fixture was either identical or four times apart, which both
  rules treat the same. A pair at a thousand and a thousand and two now sits
  inside the relative bound and outside the absolute one.
- Dropping the variance clamp at zero. Survived: no history had a second moment
  below the square of its mean, so the raw variance was never negative. One
  now does.

All four are caught. A fifth apparent survivor was not a survivor at all: the
first two runs rebuilt only `vf_packet_replay`, and the SPIR-V is embedded in
the backend, so the mutated shader never ran. The mutation harness builds
everything now. That is the same silent-shader-edit failure this journal has
recorded four times, caught this time because the baseline was measured
alongside the mutants and passed when it should have.

### Two things deliberately not done

The host struct's `length` became `samples` because GLSL reserves `.length()`
on a struct and the mirror would not compile. Renaming the host to match the
shader keeps the two readable side by side rather than leaving a name that
differs for a reason nobody would guess.

`P17_backend_ABI_appends_lighting_without_prefix_drift` asserted the request
struct's total size. That assertion fails on every append while catching none
of the drift the offsets beside it exist to catch, so it is now a lower bound
and the exact-size claim moved into a phase 20 case of its own.

Three hundred and forty-nine tests pass.

## Phase 21's reactive mask, and what it took to make it mean anything

The transparency composite had a GPU side and the reactive mask did not. The
frame now carries a fifth G-buffer plane: how much of each pixel a transparent
effect decided, which is what an upscaler needs so it stops reconstructing a
particle from history that never contained it.

Written by all four G-buffer shaders, blended with `MAX` on the transparent
pipelines so the most reactive draw on a pixel wins, read back through the same
interleave as the other planes, and compared by `CompareGBuffer` alongside
them. Three hundred and fifty tests pass.

### The plane was invisible until the comparison read it

Added as an attachment, the whole contract suite went on passing while the
device wrote one and the oracle wrote zero. `CompareGBuffer` walked three float
groups and identity; a fifth plane it did not read was a plane nothing
verified. A unit test now fails when a reactive value differs, and with the
comparison reading it, `contract.scene_raster_frame` and
`contract.instanced_scene_frame` failed until their shaders wrote the plane --
which is the evidence that it has teeth.

Along the way a run of "350 passed" turned out to be a stale
`vf_packet_replay`: `--target vf_unit_tests` rebuilds `vf_core` but does not
relink the replay, so the contract tests ran against the previous comparison.
Two probes settled it -- writing a constant to the plane and reading it back as
`gbuffer-max-error`, then writing the push constant's own value. The plumbing
was correct all along; the binary was not.

### The mask meant the opposite of what it should have

First implementation had opaque geometry write one, on the reasoning that an
opaque draw owns its pixel outright. A mutation survived that should not have,
and the reason was the design: with opaque writing one and the transparent pass
combining by max, every pixel with geometry behind it was already one and the
transparent contribution could not be seen at all.

It also inverts the meaning. Opaque geometry is the *stable* part of a frame --
the part an upscaler reprojects from history -- so marking it reactive tells
the upscaler to distrust the whole image. Opaque now writes zero and only the
transparent pass raises the mask. The mutation that had survived is now caught:
making the opaque pass claim its pixels fails the contract.

### What is verified and what is not

Verified: the plane exists, is written, is read back, is compared like every
other plane, opaque draws leave it at zero, transparent draws raise it, and no
pixel the composite left alone carries a mask.

Not verified: the additive branch of `vfReactiveMask`, which weighs a spark by
its radiance rather than by an alpha the spark never uses. Its mutation
survives, and the fixture is the reason -- the transparency layers carry no
visibility record, so they resolve to an implicit opaque alpha class where
coverage is forced to one, and `max(alpha, brightest)` and `alpha` agree at
one. Lowering the additive material's alpha changed nothing for the same
reason. Reaching it needs the fixture's transparent layers classified as
blended rather than left implicit, which changes what the composite produces
and is not a change to make on the way past.

`vfProjectDecal` was written and then removed. It had no caller: decals need a
projection in the scene packet, which the transparent record does not carry,
and a mirror with nothing calling it is dead code claiming coverage. The plan's
gate for this phase asks that captured decals appear in the correct layer, and
that remains the open half of the phase.

### The additive rule, and why one layer could never test it

The reactive mask's additive branch takes the larger of the draw's alpha and
its brightest channel, because an additive draw contributes its radiance
whatever its alpha says. Its mutation survived, and four attempts to make it
fail are worth recording, because three of them were wrong for instructive
reasons.

Lowering the additive material's alpha did nothing: the fixture's layers push
no visibility record, so every object resolves to an implicit opaque alpha
class, and an opaque class returns a coverage of one whatever the material
holds.

Giving the layers visibility records classified as blended made the coverage
carry the alpha and broke the contract instead -- 2,346 pixels came out darker
than the baseline, which the composite gate forbids. The premultiplied layer is
built around an alpha of one; that is what makes the snapshot the only route
from the geometry behind it to its pixels, and a coverage below one puts the
destination back through the one-minus-alpha term.

The mistake was in the shader, not the fixture. It passed the resolved coverage
to the mask, and coverage is what the alpha *class* decided this fragment
contributes to the target. The mask is about the effect, so it reads the
effect's own alpha -- which is also what the host contract takes. With that
changed the visibility records were not needed at all and were removed.

Then the branch was reachable and still only half testable. One additive layer
exercises whichever side of the maximum is larger, never both: bright enough to
prove the radiance term and the alpha term is dead, dim enough to prove the
alpha term and the radiance term is dead. The fixture now carries two, the
second dim and high in alpha and set off to the side where additive blending
-- which only ever brightens -- cannot disturb the ordering the concentric
pair exists to make observable.

Both mutations are caught: removing the branch, and dropping the alpha from
the maximum. The threshold on the bright layer sits above the dim layer's alpha
deliberately, because a mask that returned the alpha for every additive draw
would otherwise still clear a lower bound through that second layer -- which is
exactly what happened on the first attempt at this gate.

One mutation is left alive knowingly: taking a non-finite radiance instead of
skipping it. No input in the fixture produces one, and the guard is kept
because the host has the same branch and this file exists to hold every rule
once on each side. A mirror that dropped a branch its oracle keeps would be a
divergence, not a simplification.

Three hundred and fifty tests pass in both configurations.

### Decals project, and what the first three gates could not see

`TransparentDrawRecordV1` now carries the volume a decal projects into -- an
origin, an axis, a range, a radius, and the stencil receiver mask and reference
-- at scene packet minor 1.4. The record grew rather than gaining a section of
its own, because the volume belongs to the draw that projects it and a parallel
list would have to be kept in step by hand. A packet written at 1.3 is refused:
the same bytes mean different things at the two versions, and reading them at
the new stride would not fail, it would assemble each draw's volume out of its
neighbour's fields.

`vfProjectDecal` is back and called from the composite, against the fragment's
own position and normal, with the volume arriving in the push constants. The
stencil receiver test is passed a mask of zero, which is the host contract's
own "no receiver restriction" path: this pass has no stencil to read, and
passing the draw's own reference as if it were the receiver's would make the
test compare a value against itself and always agree.

Three measurements were needed before one of them meant anything.

The first compared the decal render against the baseline and reported 364
pixels either way. It could not work: the decal object is excluded from the
opaque pass once the transparent table claims it, so "drawn blended" and
"clipped away entirely" both differ from a baseline that drew it opaque.

The second compared the two decal renders to each other -- one projecting, one
with its volume removed -- and reported 135. Better, but all three rejection
mutations still survived, because the falloff dims a pixel without rejecting
it and a colour difference cannot tell the two apart.

The third counts the reactive plane. A discarded fragment writes no attachment
at all, so pixels present in the unclipped render's mask and absent from the
projecting one are exactly what the rules rejected. Sixty-seven of them.

Two of the three rules are still equivalent mutants, and the reason is
structural rather than a gap in the fixture: the falloff is
`clamp(facing * (1 - distance / radius), 0, 1)`, which is already zero beyond
the radius and already zero on a surface facing away, and the shader discards
on a coverage of zero. Removing either guard changes nothing the device can
produce. They are kept because the host contract has them and this file exists
to hold every rule once on each side -- the host returns before computing the
falloff, so there the guards are load-bearing.

The range is not redundant, and it was unexercised for a different reason: the
fixture's decal quad sat perpendicular to its own projection axis, so every
fragment was the same distance along it and the range decided nothing. The axis
is tilted now and the range is shorter than the quad measures along it, so both
ends fall outside. Removing the range test is caught, and so is comparing only
its far end.

One more coupling surfaced on the way. The composite's "a transparent layer
never darkens" bound is measured on a scene filtered to the additive draws, and
that filter excluded refractive draws by name rather than keeping additive ones
by name. A multiply decal -- which darkens by definition, because that is what
the mode is for -- passed straight through it. The filter now names what it
wants.

Three hundred and fifty-one tests pass in both configurations.

## Phase 23's post kernels, and an adapter that was not the one

Bloom runs on the device. `post::Luminance` is new -- the host had no stated
luminance and the shader needed one, and a rule that exists on only one side is
not a mirror -- and `vfBloomWeight` mirrors `BloomWeight` beside it. The
tone-map pass thresholds the scene before exposure and the curve, because the
highlight that blooms is a property of the scene and thresholding afterwards
would make the bloom level move with the exposure.

The rules travel in the frame request rather than being constants in the
backend: they are caller data like the light list, and a backend holding its
own copy would be a second source of truth for a value the contract owns.
Bloom is armed by a frame flag and off by default, which is what makes
"disabled is an exact identity" demonstrable rather than asserted.

Measured on the transparency fixture: `bloom-differing=0 bloom-max-code=1
bloom-changed=14147`. The device matches the oracle within the one-code slack
and the kernel changes fourteen thousand pixels, which are two different claims
and both are gated -- a kernel that returned its input would satisfy the first
alone, because the oracle would then be compared against a frame it had not
altered either.

Four mutations, all caught: ignoring the knee, a linear ramp instead of the
smoothstep, an unweighted mean instead of Rec. 709, and adding a flat term
instead of scaling the scene. Two of them survived at first. The replay had
been declaring the contract's default tuning, and at an intensity of a
twentieth the peak difference between the smoothstep and a linear ramp is half
a per cent of radiance -- below one code in an eight-bit comparison, so the
shape of the curve was unobservable. The replay now declares its own threshold
and knee, chosen so the fixture's luminances land *inside* the knee where the
shape decides something.

### The adapter was not the one anyone meant

Partway through, every GPU contract failed with "raster device creation
failed". It was not a regression: a headset runtime on this machine enumerates
a mirrored DXGI adapter reporting the same name and the same twenty-three
gigabytes as the real card, carrying a LUID no Vulkan device has, and it
enumerates first. Everything that asked D3D for "the default adapter" got it.

```
dxgi 0 luid=0:448994067 vram=25310527488 flags=0   <- mirrored
dxgi 1 luid=0:61740     vram=25310527488 flags=0   <- the card Vulkan reports
dxgi 2 luid=0:65554     vram=0           flags=2   <- WARP
```

Three tools needed different answers, because they need different things.

The replay does not share images with D3D, so it may use any capable device:
`RasterCreateAnyAdapter` is an explicit opt-in flag that lets the backend fall
back when nothing carries the requested LUID, and an exact match still wins
because the fallback is taken only after the whole list is searched. The
in-game path never sets it -- there the adapter is the one the game presents
on, and a different one would render into an image the swap chain cannot
import.

The probe tool asks the backend about each hardware adapter in turn and keeps
the first it accepts. The bridge tool does share images, so it cannot fall
back: it loads the backend first, asks which adapter is a Vulkan device, and
then creates its D3D device on that one. Loading the backend per candidate does
not work -- it is process-lifetime and a second load is refused -- which cost a
run to discover.

Picking by largest video memory was tried and is not enough: both entries
report the same memory, so the tie went to whichever enumerated first, which is
the mirrored one.

Three hundred and fifty-three tests pass in both configurations.

## In-engine verification of the current build

`artifacts/live-gpupref`. Safety clean: `success: true`, no new save files, no
restore errors, no forced kill, Fallout 4 re-enumerated as absent before launch.

```
renderer-backend:       ready device="NVIDIA GeForce RTX 4090"
renderer-mirror: dump   objects=1602 missing=0 unreadable=0
                        vertices=1810514 indices=4482732
renderer-origin-select: status=0 at=[-80351.9,89600.0,7904.8] neighbours=1613
renderer-distribution:  distance p50=4214.2 vertical p50=-67.9 below-eye=2480
renderer-projected:     on-screen=2621 of 3311 in front
```

The whole cell, through every change this session made: the device-derived
packet ceiling, the recovered camera origin, the fifth G-buffer plane and its
reactive mask, the widened transparent record with its decal volume, the
temporal accumulation pass, and the bloom kernel. The captured frame is
Sanctuary -- pitched roofs, the bare tree, the treeline, the hillside, ground
underfoot -- still untextured and unlit, because neither is wired into the
mirror path.

### The run before it did not get that far, and the reason was not the renderer

Two runs failed with `probe failed host=10 backend=6` and stayed on vanilla.
`VirtualDesktop.Service` had started between 03:39 and 05:58, and its virtual
monitor driver enumerates an adapter mirroring the RTX 4090 -- same name, same
twenty-three gigabytes -- carrying a LUID no Vulkan device has. The engine's
swap chain landed on it, and there is no Vulkan device to share images with, so
the plugin did the only honest thing and left the frame vanilla.

The probe now says so:

```
renderer-backend: no Vulkan device carries the engine's adapter
                  luid=0:448994067. A virtual display adapter is usually the
                  cause; the renderer cannot share images with a device Vulkan
                  does not have.
```

`iAdapter` does not fix it. The harness gained an `-AdapterIndex` switch and
setting it to 1 changed nothing, because Fallout 4 resolves `sD3DDevice` by
name and both adapters report the same name. What does fix it is a Windows
per-application GPU preference on `Fallout4.exe` and `f4se_loader.exe`
(`HKCU\Software\Microsoft\DirectX\UserGpuPreferences`, `GpuPreference=2;`),
which is a persistent machine setting and was made with the operator's
agreement rather than as part of a run.

## Why the mirrored scene has no materials and no lighting

Both are capture gaps, not renderer gaps, and the measurements say so exactly.

**Lighting.** The mirror's frame request supplies `packetData`, `outputData`,
`frameData` and `sceneData` and nothing else. With no `lightData` the
environment is undeclared, and `vfShadeSurface` returns the albedo unchanged --
by design, and the comment in it says why: "a frame with no captured lighting
leaves the albedo alone, exactly as every phase before this one did. Without
the declaration a zeroed environment would read as ambient zero and black out
the scene." Everything downstream gates on the same flag, so the ray-traced
shadow term and the diffuse bounce are inert too.

**Materials.** The backend accepts one texture per frame and falls back to
`MakeFallbackTexture(White)` when none is supplied, and `AssembleSceneGeometry`
sets every material's base colour to white. So `shaded = vertexColour * white *
white`, which is exactly the flat pastel the captures show: those colours are
the engine's own vertex colours, decoded correctly, and nothing else.

### Finding the engine's lighting block

Slot 16 of the `ID3D11DeviceContext` vtable is `PSSetConstantBuffers`, and the
numbering is not a guess: it is the same table the existing hooks come from,
where 12 is `DrawIndexed`, 14 `Map`, 15 `Unmap` and 17 `IASetInputLayout`.

The first sampler took the first buffer in a size window and got a per-draw
material block -- `2.4401 2.4401 2.4401 1.0000` and then zeros. Sizes alone do
not identify a per-frame block, so the hook now reports every pixel-shader
constant buffer the engine binds, with how often it was rewritten. Measured
over a settle period:

```
bytes=16   maps=0        bytes=272   maps=22191
bytes=32   maps=0        bytes=368   maps=3611
bytes=64   maps=1275     bytes=384   maps=3503
bytes=80   maps=0        bytes=752   maps=18689
bytes=96   maps=27230    bytes=65520 maps=0
```

The ratio is the discriminator. Twenty to twenty-seven thousand rewrites is
per-draw -- material constants. One to four thousand is per-frame or per-pass,
which leaves 64, 368 and 384 bytes as the candidates for the block the sky
publishes. The sampler now keeps one sample per distinct size rather than the
most recent of any size, which was always a per-draw block.

Reading those candidates is where this stopped, and not for a renderer reason:
the run that would have printed them landed on the mirrored adapter again.

### The constant buffers do not give the lighting up, and why

The hunt is worth recording because the negative result is what saves the next
attempt.

Slot 16 is `PSSetConstantBuffers`, and the hook works: eleven distinct buffers,
with the rewrite count separating per-frame from per-draw. Two of the
candidates decoded cleanly and neither is lighting. The 64-byte block holds
`15` and `353825` -- the near and far planes. The 384-byte block holds four
taps weighted a quarter and then a four-by-four grid weighted a sixteenth,
which is a filter kernel.

The 752-byte per-draw block looked right. It contains `-80351.992 89600.000
7904.783`, which is the camera origin recovered independently by
`SelectCameraOrigin`, and `0.707 0.707 0.038`, which is the camera forward read
off the view-projection -- two separate cross-checks that the buffer is real
and that the earlier work was right.

But asking which of its words hold still across a frame answers the question
the wrong way round: **every meaningful word varies per draw, and only padding
is invariant** -- including the words that read as the camera origin in a single
sample. A quantity that is by definition constant for a frame cannot vary
across that frame's draws, so the buffer is not one layout. The engine reuses a
single wide pixel-shader constant buffer across shader techniques, and an
offset means different things depending on which technique last bound it.

That makes offset archaeology on these buffers a dead end, not a slow path.
Reading a colour out of word 168 and calling it the sun would produce a number
that is right for whichever technique drew last and wrong for the frame.

Two methods remain, and both are their own piece of work rather than another
sampling pass. Hooking `CreatePixelShader` and reflecting the DXBC would give
each technique's constant layout by name, which is the rigorous answer and
turns every buffer above into labelled fields. Reading the sky singleton for
the sun's direction and colour and the current weather's ambient is the smaller
answer and does not depend on shader internals at all.

Until one of them lands the mirror declares no environment, which is the
renderer behaving correctly: `vfShadeSurface` leaves the albedo alone rather
than lighting the scene by numbers nobody measured.

## The engine's shaders name their own constants (2026-08-17)

Offset archaeology is over. The pixel shaders carry their own reflection, and
reading it turns every anonymous constant buffer into named fields with
offsets. `src/renderer_core/ShaderReflection.{h,cpp}` parses the RDEF chunk out
of a DXBC container; `CreatePixelShader` (ID3D11Device slot 15, confirmed by
slot 11 already working as `CreateInputLayout`) is hooked to catch the bytecode
at the only moment it is readable.

Measured live in Sanctuary: **2818 pixel shaders created, 425 reflected, 2393
refused.** The refusals are Bethesda's own shaders, shipped with the reflection
chunk stripped. The 425 that kept theirs belong to the volumetric lighting
library, and they declare the blocks the mirror needs:

| buffer | bytes | shaders | what it carries |
| --- | --- | --- | --- |
| `cbVolume` | 736 | 6 | `g_vLightDir@608`, `g_vLightColor@640`, `g_vLightPos@624`, cascade matrices |
| `cbPass` | 336 | 18 | `g_mViewProj@64`, `g_mViewProjInv@128`, `g_vEyePosition@224`, `g_fZNear@320`, `g_fZFar@324` |
| `cbPost` | 112 | 7 | `g_vIntensity@0`, `g_vFogLight@16`, `g_mHistoryXform@48` |

**The sun, read by name rather than guessed at:**

```
renderer-named: cbVolume.g_vLightDir@608  = -0.3505 0.6911 -0.6320 (varies)
renderer-named: cbVolume.g_vLightColor@640 = 0.3490 0.3804 0.4196
renderer-named: cbVolume.g_vLightPos@624   = 0.0000 0.0000 0.0000
```

`g_vLightDir` has magnitude 0.99995 — a unit vector, which is the check that
says this is a direction and not four bytes that happened to look like one.
`g_vLightPos` is zero and `g_vLightAttenuationFactors` is zero, both of which
are what a *directional* light looks like. It varies across a capture because
the sun moves; the neighbouring `g_fGodrayBias@620 = 0.0015`,
`g_fTargetRaySize@652 = 8.0` and `g_fLightToEyeDepth@76 = 5000.0` hold still
and land exactly where reflection says they do, which is what makes the
alignment a measurement rather than a reading.

### Three defects found on the way, all of which hid the block completely

- **The lighting sample was keyed by byte width.** The engine binds several
  752-byte pixel-shader blocks; keyed by size their contents interleaved, every
  word's range spanned two unrelated meanings, and nothing held still. This is
  what made the earlier archaeology stall — it was not that the layout was hard
  to find, it was that two layouts were being averaged. Now keyed by buffer.
- **The description table was far too small.** At 32 entries every slot filled,
  nineteen of them with immutable blocks that can never be written. Raised to
  256 and it filled again. It is now a 4096-entry open-addressed table indexed
  by a hash of the buffer address: **3814 distinct buffers** get described where
  256 did before, and the lookup no longer costs its whole length on each of the
  million-odd binds a minute of play makes. `cbVolume` and `cbPass` only ever
  appeared after this.
- **Only one of the two write paths was hooked.** `Map`/`Unmap` covers DYNAMIC
  buffers; a DEFAULT-usage buffer cannot be mapped at all and is written through
  `UpdateSubresource` (context slot 48). `cbPost` was invisible until that hook
  went in — its 144-byte neighbour went from `maps=0` to `maps=238` in the same
  run, which is the evidence the hook works rather than the assertion that it
  should.

Reproduce:

```powershell
tools/game_smoke/Invoke-LiveCapture.ps1 -GameRoot "<install>" `
  -PluginDll out/build/vs2022-x64-release/Release/VisualForge.dll `
  -BackendDll out/build/vs2022-x64-release/Release/VisualForgeRenderer.dll `
  -ArtifactDirectory artifacts/live-named -StartingConsoleCommand "coc SanctuaryExt" `
  -EnableBackend -EnableMirror
# then: grep "renderer-named: cbVolume" artifacts/live-named/VisualForge-live.log
```

### What this does not settle

`cbVolume` belongs to the volumetric lighting library, so the sun is readable
only while `bVolumetricLightingEnable` is on. It was on for these runs. A
session with godrays disabled will find no `cbVolume`, and the mirror must go on
declaring no environment in that case rather than lighting the scene from a
stale sample. Bethesda's own shaders remain stripped, so their per-material
constants are still unnamed — the texture track does not benefit from this.

### A release-only crash at exit, found by running both configurations

`contract.indirect_accumulation` segfaulted in release while passing in debug,
*after* printing `result=pass` — every comparison in the test had already
succeeded. `RenderIndirectAccumulation` never called `host.DestroyRaster`,
which every other replay mode does, so the Vulkan device was torn down after
the module that created it had gone. Debug's different teardown order hid it.

The fix is the missing destroy, and the mode now reports `destroyed=` and
`lifecycle=` and folds them into its own pass condition, so a renderer left
alive fails the test that owns it rather than crashing the process afterwards.
This is the argument for running both configurations: a test that prints
`result=pass` and then dies is still a failing test.

## The sun reaches the shading (2026-08-17)

The measurement from the reflection work is now wired through to the backend,
red-green-refactor with every guard mutation-tested.

`src/renderer_core/EngineEnvironmentSource.{h,cpp}` joins the two halves that
are useless alone -- the layout a shader declares, and the bytes the engine
wrote -- into an `EnvironmentRecordV1` and a directional `LightRecordV1`.
Sixteen mutations of its guards, all caught; the unit-length gate is the one
that makes it a measurement rather than a reading, because three floats at the
right offset are only a direction if they have unit length.

Live: `renderer-mirror-light: source=cbVolume status=none bytes=288
layouts=9 samples=20 candidates=1 width=736`.

### Four defects found on the way, each one silent

- **The environment's sun is not what shades anything.** `ShadeSurfaceGpu`
  reads the ambient term and the *light list*; `sunDirection` and `sunColor`
  are carried for provenance and evaluated by nothing. A sun delivered only as
  environment metadata contributes exactly zero, which on screen is
  indistinguishable from no sun. It is now also emitted as a directional
  `LightRecordV1`. Caught by a test that shades two opposed normals and
  demands they differ -- it read 0.0 difference.
- **The direction's sign was inverted.** `LightRecordV1::direction` is the
  direction light *travels* (`EvaluateDirectGpu` computes `toLight` as its
  negation) while the engine's vector points toward the light. Passing it
  through unchanged lit exactly the surfaces facing away from the sun. This is
  the one error no "the picture changed" check can catch, because it changes
  the picture just as much as the correct sign does; the test asserts which of
  the two surfaces is brighter, not merely that they differ.
- **No vertex in the world had a normal.** `AssembleSceneGeometry` wrote
  position, colour and texture coordinates and left `RasterVertexV3::normal` at
  its +Z default for every vertex, so each object shaded to one flat tone
  wherever the sun was. The decoder already read the engine's per-vertex
  normal and the shader already expects it in object space; only the fill was
  missing. A "zero" normal in the engine's byte encoding decodes to 0.0039
  rather than to nothing, so the degenerate guard needs a real threshold --
  `> 0.0f` accepted it.
- **A misleading status code sent the search to the wrong layer.** The mirror
  reported `no-candidate` -- "the sun was never found" -- when the sun had in
  fact been found and the *light packet encode* had failed on `fogFar >
  fogNear`. Encode failures are now reported as themselves. The engine
  publishes no fog in this buffer, so the record now says "no fog" explicitly
  with a zero maximum rather than inventing distances.

### What is measured and what is not

`renderer-mirror: ... vertices=64562 normals=32281 no-normals=32281` -- half
the mirrored vertices now carry the engine's own normal and half take the
default, counted from whether the layout *declares* NORMAL rather than from the
decoded value, because `DecodedEngineVertex` defaults to a perfectly good unit
+Z that a length test cannot tell from a real one. The exact halving is worth a
second look rather than being taken at face value.

The live frame is still largely one tone, and this counter is the instrument
that says which half of the pipeline to look in next: normals do arrive for a
large part of the scene, so "the meshes have no normals" is no longer the
explanation. Also unresolved: the mirror dump races the harness's PPM/PNG
conversion, so a live frame cannot be relied on to hold geometry -- which is
why the end of this chain is pinned by a deterministic test that shades two
opposed normals rather than by a screenshot.

## Per-draw material textures: the CPU-side contract (2026-08-17)

Resuming task #27. The packet, ABI, and CPU-oracle layers are done,
red-green-refactor with mutation testing throughout. 373/373 in both
configurations.

- **`RasterMaterialV1` carries a `textureIndex`.** Grew from 32 to 48 bytes
  (`kPacketMaterialTextureVersionMinor = 3`). `kNoMaterialTexture =
  0xFFFF'FFFFu` is the sentinel, not zero -- zero is the first real slot in a
  frame's texture library, and a material that defaulted to it would silently
  sample whatever texture happened to be uploaded first. The sentinel is
  pinned by its literal bit pattern, not compared against its own symbol,
  after a first version of that test proved unable to catch a mutation that
  redefined the constant globally -- both sides of the equality moved
  together.
- **`RasterFrameRequestV1` carries `textureLibraryData`/`textureLibrarySize`**
  (`kBackendAbiTextureLibraryMinor = 13`), following the `lightData`/
  `lightSize` precedent exactly: caller data, not a backend-owned fallback.
- **`RenderReferenceTextureLibrary`** is the new CPU oracle: each draw's
  material selects its own texture from a supplied library by index, or
  shades flat from `baseColor` at the sentinel. This is what the Vulkan
  descriptor-indexed array will be checked against. An out-of-range index is
  refused (`UnsupportedState`), not guessed at.

Two knock-on test failures from the version bump, both the same shape as an
earlier lesson in this project: a test that pins an exact version number or
exact struct size goes stale the moment anything else in the packet grows.
Both changed to `>=`/`<=` against the specific floor each test actually cares
about, with a new exact-pin test added for the texture library's own tail
(matching the `P17`/`P20`/`P23` "prefix drift" pattern already established).

One fixture bug caught by its own symptom, not by inspection: a hand-built
two-triangle test packet (left material -> red texture, right material ->
blue texture) initially read pure background at both sample points. The
vertex order was the mirror image of the known-good phase6 fixture --
CCW in the working example is the wide pair at negative NDC y and the apex at
positive NDC y; this fixture had them swapped, which rasterizes as clockwise
and is silently dropped by the default `FrontFace::CounterClockwise`. Fixed by
matching the known-good order exactly rather than reasoning about it fresh.

### Not yet done

- The plugin does not yet resolve a real per-draw texture index from the
  engine -- `PSSetShaderResources` (device context slot 8, already confirmed
  in this build's vtable) is not hooked, and nothing in `RendererBackendProbe`
  populates `textureLibraryData` or writes a non-sentinel `textureIndex`.
- The Vulkan backend still binds one material bundle (bindings 1..3) for the
  whole frame. `GenerateRasterShaderLayout.cmake` still gates the reflection
  on exactly one combined-image-sampler at binding 1, and `phase11/scene.frag`
  still declares `uniform sampler2D baseTexture` rather than an array. Neither
  has been touched yet -- this is real GPU/shader surface, not a small step,
  and needs `VK_EXT_descriptor_indexing` wired through capability negotiation
  before the array binding itself.

## The base-colour texture slot is not fixed either, and is now readable (2026-08-17)

Extending the material work: the base-colour texture's PS shader-resource
slot has no fixed convention in this engine, the same way the lighting
constant's offset did not. `ShaderReflection` now also parses the RDEF
bound-resource table (textures and samplers, `D3D11_SHADER_INPUT_BIND_DESC`),
not just constant buffers -- red-green-refactor with mutation testing on the
new bounds checks and the texture/sampler discriminator.

**Validated against real Fallout 4 shaders, not just the synthetic fixture**,
the same discipline the constant-buffer offsets got after the first version
was wrong:

```
renderer-reflect-resource: name=tex[0] kind=texture slot=0 count=1 shaders=150
renderer-reflect-resource: name=sampler_tex[0] kind=sampler slot=0 count=1 shaders=150
renderer-reflect-resource: name=tex[1] kind=texture slot=1 count=1 shaders=152
renderer-reflect-resource: name=sampler_tex[1] kind=sampler slot=1 count=1 shaders=152
```

Every texture/sampler pair lands at the *identical* slot with the *identical*
shader count -- `tex[0]`/`sampler_tex[0]` both slot 0, both 150 shaders. If
`bindPoint` (RDEF offset 20) were the wrong field, there is no reason a
texture and its own sampler would agree on slot *and* usage count; this is
strong evidence the byte offset is right, not just plausible-looking. `tex[0]`
at slot 0, used by 150 distinct pixel shaders, is almost certainly the general
opaque/family material path's base-colour texture -- by far the largest count
of any texture found. `cbVolume`/`cbPass`/`cbPost`/`Constants` also appear
here as `kind=other` (D3D_SIT_CBUFFER), with the same names the constant-buffer
table already reported, which is a second independent cross-check that both
tables are reading the same shaders correctly.

### What this still does not give the mirror

Knowing "slot 0 is base colour for the shaders that use `tex[0]`" is not yet
"which texture is bound at slot 0 for *this* draw" -- that requires knowing
*which pixel shader* is active when a draw fires (not yet hooked: `PSSetShader`
is unhooked) and reading slot 0 specifically for that shader's declared name,
not slot 0 unconditionally, since a different technique may use a different
slot for the same purpose or a different purpose for the same slot.

Also unresolved, discovered while designing the capture hook:
`EngineTextureCapture` is architecturally a one-shot dump-to-file tool --
`s_state.candidates` is cleared after one publish and gated permanently by
`s_state.captured`. It cannot serve as a live, continuously-queried,
multi-texture store for the mirror; that needs a new always-on residency
tracker, the texture-side counterpart to `EngineMeshExtractor`/`SceneDatabase`
for meshes, not a retrofit of the one-shot tool. Not started.

### What is real and shippable right now

The CPU-side material/texture contract, independent of any of the above:
`RasterMaterialV1::textureIndex`, the ABI's `textureLibraryData`/
`textureLibrarySize`, and `RenderReferenceTextureLibrary` as the CPU oracle.
374/374 in both configurations. Nothing in the live engine writes a
non-sentinel `textureIndex` yet -- the mirror still shades every surface from
`baseColor` alone, correctly, because that is what "no candidate found" is
supposed to do.

## Per-draw base-colour texture identity, and the wall reflection hits (2026-08-17)

Steps 1-3 of the texture-library goal. `PSSetShader` (context slot 9) is
hooked, each created pixel shader's base-colour register is resolved once from
its own reflection at creation time, and every draw stamps the engine texture
bound at that register into `DrawRecordV1::baseColorTexture`.

`FindBaseColorTextureSlot` is the rule, unit-tested and mutation-tested: the
material path declares `tex[0]` (or a scalar `tex`) and the register comes from
the reflected bind point, never from the digit in the name. The texture/sampler
discrimination is load-bearing and has a test that proves it -- a shader
declaring `SamplerState tex` with no matching texture would otherwise hand back
a sampler register as a shader-resource slot.

### The measurement that changed the design

The reflection route alone resolves almost nothing. Measured live, per draw:

| outcome | draws | share |
| --- | --- | --- |
| shader carried no reflection chunk | 1,154,552 | 63% |
| no pixel shader recorded at draw time | 664,902 | 36% |
| described, declares no material texture | 4,581 | 0.25% |
| resolved from the shader's own declaration | 2,564 | **0.14%** |

The rule is *correct* where it applies -- `draws-missing=0` on that path, every
draw whose shader named a base colour had one bound. It simply almost never
applies: Fallout 4 ships 2393 of its 2818 pixel shaders stripped of reflection,
and those are the ones that draw the world. This is the same wall the constant
buffers hit, now measured against draw volume rather than shader count.

So a second path was added, and **kept separate in every counter** rather than
folded in: when a shader carried no reflection, register 0 is used by
Bethesda's own convention for the diffuse map. That convention is corroborated
by every shader that *can* be read -- `tex[0]` and the scalar `tex` both bind
at register 0 across 294 of the 316 reflected shaders that declare one -- so it
is a convention supported by all available evidence, not a guess. It is still
not the shader's own word, which is why `by-convention` is reported as its own
number and never added to `draws-with`.

With both paths: **2,745 of 4,381 draws in a mirrored frame (63%) carry a
resolved base-colour texture identity.** `draws-missing` is honestly non-zero
(14,855 cumulative) where register 0 has nothing bound.

### Still open

`no-shader=518,148` -- 36% of draws see a null `t_currentPixelShader` despite
`ps-shader-binds=92,051` proving the hook fires. Most likely the engine binds
and draws from different threads, or replays command lists (which do not
re-enter the vtable hooks). Not yet diagnosed; it is the next thing to
understand, because it is the largest remaining population.

An identity is not a texture. Nothing has been read back from the GPU yet:
step 4, the always-on residency tracker, has not been started, so
`textureIndex` is still the sentinel on every material and the mirror still
shades from base colour alone.

## The texture library reaches the mirror's materials (2026-08-17)

Steps 4-6 of the goal, and the done condition is met:

```
renderer-texlib: materials=1601 textured=1034 library=121 bytes=128967824
  resident=1067 resident-bytes=467501424 rejected=2571 budget-dropped=0
  unreadable=0
```

**1,034 of 1,601 materials in the mirrored scene carry a resolved
`textureIndex`**, backed by a 121-entry library holding 129 MB of the engine's
own pixel data, encoded through `EncodeTextureLibrary` and passed on
`RasterFrameRequestV1::textureLibraryData`. No assembly rejection, no
validation error, the mirror still renders, and the residency budget was never
reached.

`src/EngineTextureResidency.{h,cpp}` is the new always-on tracker -- the
texture-side counterpart to `EngineMeshExtractor`. 1,067 textures resident at
467 MB, 2,571 creations correctly rejected as non-material (render targets,
scratch surfaces, non-BC formats), nothing unreadable.

### Deliberate deviation from the plan, and why

The goal said pixels "read back from the GPU". They are taken from the
engine's own upload at `CreateTexture2D` instead. Fallout 4 creates its
material textures immutable with contents supplied up front, so the bytes are
already in hand at that moment; a readback would mean a staging copy and a map
on the render thread, synchronising the GPU inside the engine's own
submission. `EngineMeshExtractor` moved its reads to Present for exactly that
reason, and a texture that arrives with its data needs no such trade. The
pixels are the engine's real texture contents either way.

### A bug that would have made all of this silently find nothing

The draw path keyed textures on the raw `ID3D11Resource*` from
`GetResource`, while the residency tracker sees an `ID3D11Texture2D*` at
creation. COM guarantees only that *IUnknown* is identical across an object's
interfaces -- the two pointers need not be the same address. The existing
one-shot capture already did the `QueryInterface(IUnknown)` reduction; the new
code did not. Both now do. This was caught by reading the existing module's
`Identity` helper before building on top of it, not by a failing test: the
symptom would have been a lookup that finds nothing, forever, with every
counter looking healthy.

### What is measured and what is not

`textured=1034` of `materials=1601` is 65%. The remainder split between
materials whose draws never resolved a texture and those whose texture was not
resident. Nothing here is guessed: a material without a resolved texture keeps
the sentinel and shades from base colour.

The Vulkan backend still does not *consume* the library -- it binds one
material bundle for the whole frame, `GenerateRasterShaderLayout.cmake` still
gates on a single combined-image-sampler, and `phase11/scene.frag` still
declares a scalar `baseTexture`. That was explicitly out of scope for this
goal, which ends at "the CPU has a correct, encoded texture library and knows
which material wants which entry". It does.

## The Vulkan backend samples a different texture per material

The previous entry ended with the CPU holding a correct texture library and
the backend still binding one material bundle for the whole frame. That gap is
now closed: `phase11/scene.frag` samples
`sceneMaterialTextures[scenePush.textureIndex]`, a 256-entry combined
image-sampler array at set 0 binding 20, and the per-draw index travels in the
scene push constants.

### Why a new binding rather than widening binding 1

Binding 1 is the single `baseTexture` the phase 6, 9 and 16 shaders all
declare, and each has a build-time reflection gate asserting its exact shape.
Widening it would have meant changing all of them and their gates to buy
nothing. Binding 20 leaves every one untouched, and the sentinel
`kNoMaterialTexture` keeps sampling binding 1, so every frame built before the
library existed renders exactly as it did.

### The index is dynamically uniform, so no `nonuniformEXT`

It arrives in a push constant, which is uniform across the draw by
construction. `nonuniformEXT` exists for indices that vary across the
invocations of a single draw, and using it here would have implied a
requirement the frame does not have. The array is fixed-size for a related
reason: an unsized one would have forced `#extension` into every translation
unit including `scene_layout.glsl`, several of which include it after another
header's declarations, where the directive is not valid.

### Descriptor indexing was required and probed but never enabled

The device query chained `VkPhysicalDeviceVulkan12Features` and reported the
feature as present, and nothing ever put it in the *enabled* structure at
device creation. Every frame so far used exactly one descriptor per binding,
so nothing had noticed. The four descriptor-indexing features are now always
chained into `enabled12`, and `PrepareMaterialTextures` refuses a
multi-entry library outright when the device lacks the feature rather than
partially filling the array -- a frame that sampled index 3 of a one-entry
array would read an undefined descriptor.

### A stale-library bug the contract caught

`PrepareMaterialTextures` was only called when a frame *supplied* a library.
A frame that supplied none skipped the call entirely, so the previous frame's
images stayed resident and binding 20 still pointed at them: its draws sampled
textures their own packet never named. The contract renders the same scene
twice, once with the library and once without, and the two came back
pixel-identical -- `withheld-differing=0` -- which is what exposed it.

The call is now unconditional for any request new enough to carry the field,
with an empty span releasing the library. Alongside it,
`ResolveMaterialTextureIndex` honours a material's index only when *this*
frame's library covers it, and yields the sentinel otherwise. Together those
make a frame's shading a function of its own packet. The withheld frame now
differs across 11,974 pixels.

### What the contract asserts

`contract.texture_library_frame` renders the phase 11 scene fixture with
object 0's material naming a red library entry and object 2's a blue one,
vertex colours and material base colours flattened to white so the texel is
the only thing deciding a pixel's hue. It counts, over every pixel the CPU
oracle painted red, how many the device also painted red, and likewise for
blue: `red=8844/8844 blue=3130/3130`, `max-error=1`.

Counting whole regions rather than probing two coordinates is deliberate --
the assertion does not depend on where the projection happens to land each
triangle. Requiring *both* regions is what makes it discriminating: the
mutation that makes every material sample entry 0 leaves red untouched and
collapses blue to `0/3130`. A single-region assertion would have passed it.

### Out of scope, unchanged

Normal maps, the 36% of live draws with no resolvable shader, and the 65%
material coverage ceiling. This goal ends at "a different texture per
material, on the device, proved against the oracle".

### The live A/B is armed but not yet run

`renderer-mirror-texture-probe` renders the first mirrored frame that has both
a scene and a library twice -- withheld arm first, so the image reaching the
swapchain is always the one with textures applied -- and logs
`pixels/differing/max-channel`. It runs once per session.

The run itself is deferred: a user-owned Fallout 4 process was present at every
check, and the invariant is to defer rather than adopt or disturb it. Nothing
about the GPU result waits on this; the live probe measures the same property
in the engine that `contract.texture_library_frame` measures on the bench.

### The live A/B, and why the first one measured nothing

The first run reported `differing=0`, and the probe was right to report it: it
had fired on the load screen. `renderer-texlib` at that moment read
`materials=4 textured=2 library=2`, while the world that followed read
`materials=351 textured=29 library=20`. The first frame *holding a library* is
not the first frame *worth measuring*, and the two were being confused.

The probe now waits for a library of at least eight entries and, having
measured, latches only on a non-zero difference -- retrying up to eight
attempts otherwise. A zero is not an answer here; it means the textured
materials that frame contributed no visible pixels, which is a statement about
the frame rather than about the library. The cap keeps the extra submission off
the steady-state frame cost.

Second run, Sanctuary, 1280x720:

```
renderer-mirror-texture-probe: attempt=1 pixels=921600 differing=16996
  max-channel=187 library=20 textured=29 materials=351
```

16,996 of 921,600 pixels move when the library is supplied, with a maximum
channel delta of 187. The withheld arm renders first, so the image reaching the
swapchain is always the one with textures applied.

### A cost this run made visible

The library is rebuilt from scratch whenever its contents change --
`DestroyMaterialTextures` then a fresh upload of every entry, behind a
`vkDeviceWaitIdle`. Walking into Sanctuary changed it eleven times in ninety
seconds, climbing to 110 textures and 122 MB, so the frame that follows each
change pays a full pipeline stall plus a re-upload of textures that did not
change. The signature check makes a *stable* library free, which is the common
case once a cell settles, but the settling itself is expensive. An incremental
path that adds only new entries is the obvious next move and is not part of
this goal.

## Phase 25: world-only draw suppression

`suppression=off` had been a string in about thirty log messages, not a
mechanism. `EngineTakeover` -- permits, fault phases, the whole-frame rule --
was fully unit-tested and referenced by nothing outside its own tests. Turning
suppression on therefore meant implementing phase 25's green step.

- `renderer_core/EngineSuppression` is the draw-time predicate: a draw is
  dropped only when the permit grants, the draw writes the world target, and
  something is reproducing the world. Three vetoes, tested across all eight
  combinations; all four mutations (drop each veto, invert the whole-frame
  rule) are killed.
- `depth::SceneDepthBound()` supplies the world-target half. A draw with the
  main scene depth bound is a world draw; the HUD, the Pip-Boy and every
  Scaleform surface are not, which is what makes the suppression world-only.
  The comparison goes through `IUnknown` because only that is identical across
  an object's interfaces -- the same rule the texture path had to learn -- and
  it is cached per thread behind a generation counter, so a resize that hands
  back the same view address cannot leave a stale classification alive.
- Opt-in via `VISUALFORGE_SUPPRESS_WORLD=1`, exposed as `-SuppressWorldDraws`.
  Opt-in because the mirror reproduces a subset of the world, so a suppressed
  frame is incomplete by exactly that subset. The plugin says so in full at
  startup rather than as a flag value.
- Draws are recorded before being dropped. The mirror is built out of these
  draws, so a suppressed frame must observe exactly what an unsuppressed one
  does or suppression starves the renderer replacing it.

379 tests pass in both configurations.

### The first measurement run produced no measurement, and it is not yet attributed

The run reached the world and was force-killed on the way out, and its restore
failed with the installed plugin held open by another process. The log it left
carries no `renderer-backend` or `renderer-mirror` line at all, and
`vulkanBackendReady` is false, while `result.json` records that both were
requested. So *none* of the `VISUALFORGE_*` variables took effect in the
process that wrote that log -- not only the new one.

Environment inheritance through `Start-Process` was tested directly afterwards
and works, and the harness sets the variable before the launch, unconditionally.
That rules out the two obvious explanations and leaves the observation
unattributed. The most consistent reading is that a second, separately launched
game instance was running -- it would carry none of the variables and would
hold the plugin DLL open, which is exactly the restore that failed -- but that
is a hypothesis, not a finding, and it is recorded as one.

What was verified by hand afterwards: no save files were created, all three INIs
match their backups byte for byte, the harness-installed plugin was removed so
the install is back to the `.disabled` state it started in, and the log was
restored from the backup the harness took. No further live run was started.

## The mirror stops rebuilding static state every frame

The mirrored frame cost about 900 ms, and roughly 700 ms of that was CPU work
re-deriving a scene that does not change between frames.

The first measurement decided the design. On a settled cell:

```
renderer-mesh-churn: meshes=940 unchanged=940 changed=0 added=0 removed=0
```

Nothing changes. Not "little" -- nothing. So the whole 454 ms geometry stage
was producing a byte-identical result each frame, and caching was the entire
fix rather than part of it.

### What the fingerprint has to be

Identity *and* the bytes behind it. A mesh identity is derived from the pooled
buffer and the range inside it, so the engine can re-extract different geometry
into the same range and keep the identity. A cache keyed on identity alone
draws the previous object's geometry under the new one's name -- a scene made
of the right objects in the wrong shapes, which reads as a capture bug rather
than a cache bug. That mutation survived the first version of the test and is
now killed by one that changes the bytes while holding the identity.

### Costs found by removing the one in front of them

Each fix exposed the next, and none of them were where the previous guess said.

| stage | before | after |
| --- | --- | --- |
| texture library | 207,671 us | 71 us |
| raster encode | 40,620 us | 3,341 us |
| geometry assembly | 454,273 us | 5,222 us |

- The library was re-encoded whole on every addition, and the mirror copied
  every `CapturedTexture` into a contiguous vector first purely to satisfy the
  encoder's span -- a redundant pass over 130 MB before the encoder had copied
  anything. A pointer overload removed the pass; a generation in the frame
  request removed the backend's matching decode-and-hash.
- Eviction was worse than the addition it was meant to fix. Compacting the
  entry list shifted every later index, and those indices are part of what the
  encoded raster packet says, so a single evicted texture invalidated the
  packet cache. An evicted entry now keeps its place and is filled.
- The packet cache also never matched because the key was order-sensitive
  while object order is not stable. Every draw carries its own ranges and
  material, and the class is opaque and depth-tested, so the key is now the
  draw *set*.
- With the decode cached, two quadratic passes were left standing:
  `PositionsAreFinite` decoding every vertex in the selection pass, and the
  instance list rescanned per object. Both are now linear.

### Where it stands against the gate

Settled Sanctuary, 940 meshes with zero churn, windowed:

```
geometry-us=5222  library-us=77  encode-us=3341  render-us=40271
```

Library and encode are inside the 5,000 us the goal asks for. Geometry sits at
5,222 -- 87 times better than the 454,273 it started at, and about 4% over the
line. The remaining time is no longer any single rebuild: it is spread across
translating six thousand recorded draws into a scene packet, building the
object and instance lists, and the per-frame bookkeeping around them. Getting
under the line means not re-deriving the scene packet either, which is a larger
change than this goal scoped.

The scene did not shrink to buy any of this: 940 draws, 1.19 M vertices and 862
of 940 materials textured, against a baseline of 923 objects and 1.12 M
vertices. That half of the gate is what a cache that quietly dropped geometry
would have failed, and it is why it was written in.

### Still open

Spikes during streaming remain large -- a cell arriving re-adds the whole mesh
set and the library grows past 200 entries, and those frames still cost
hundreds of milliseconds. Steady state is what improved. The CPU readback and
the fence that serialises it were never started.

### The three stages are not independent

`geometry-us` is the outer timer around `BuildLiveSceneGeometry`, and both the
texture library build and the raster encode happen inside it. So geometry can
never be smaller than library plus encode, and reading the three as separate
budgets overstates what is left. Attributed on a settled cell:

```
geometry-us=4987
  encode-us=3363  library-us=77
  translate-us=253  extract-us=115  assemble-us=250  collect-us=50
```

The named parts come to 668 us. Encode is the bulk of the rest, and it is
already 12 times better than it was.

### What the last passes found

Each remaining chunk was something re-derived per frame rather than a rebuild
of the scene itself:

- `FindInputLayout` scans a fixed table of five hundred atomic slots and then
  *rebuilds* the vertex layout from its element descriptors. Doing that for all
  nine hundred meshes every frame re-derived a few dozen distinct layouts that
  never change. Memoised on layout and stride.
- Two thirds of recorded draws are depth-only or off-screen and are rejected
  before they can become an object, yet each was paying for an identity hash
  and two probes into persistent maps.

Two changes were tried and reverted or recorded rather than kept:

- Caching the derived scene packet never hit. The recorded draw stream varies
  between frames -- 6,268 draws one frame and 5,941 the next -- while the mesh
  set it produces does not, so a key over the draws could not match and the
  signature was pure added cost. Reverted.
- Skipping revalidation of meshes already in the arena is an equivalent mutant:
  no test can distinguish it, because it only repeats a check whose answer is
  known. Kept for the frame time and labelled as such.

### Where it lands

Quiet settled windows meet all three: geometry 4,914-4,988, library 70-80,
encode 3,306-3,414. Windows in the same run that are not quiet do not: geometry
reaches 11,042 when the texture library gains or loses entries, which changes
material indices and legitimately invalidates the packet cache. That is a real
remaining cost and not a measurement artefact.

The frame went from about 900 ms to about 44 ms. The scene did not shrink to
buy it: 940 draws, 1.15 M vertices, 862 of 940 materials textured, against a
baseline of 923 objects and 1.12 M vertices.

### Two measured reverts

Both were reverted on evidence rather than suspicion, and both are worth
keeping in the record because the reasoning looked sound in each case.

- Caching the derived scene packet never hit once. The recorded draw stream
  varies between frames -- 6,268 one frame, 5,941 the next -- while the mesh
  set it produces does not, so a key over the draws could not match. The
  signature was pure added cost.
- Patching only the material section of the encoded packet instead of
  re-encoding it looked like the obvious fix for the encode spikes, and it has
  a passing test proving a patched packet is byte-identical to a re-encoded
  one. In the live run it made encode fourteen times *worse*: 3,341 us became
  45,900. The revert restored 3,394, which is what identifies the change as the
  cause rather than the machine. Why it is slower is not explained, and saying
  so is better than keeping a change that measures worse.

### The steady state is not steady over minutes

Within one settled run, the early windows meet the gate and the late ones do
not:

```
00:58:19  geometry-us=4983   encode-us=3394   render-us=39631  present-us=0
00:58:36  geometry-us=19010  encode-us=17443  render-us=58739  present-us=382
```

The mesh set is unchanged across all of them. What moves in step is
`present-us`, from zero to several hundred microseconds, with render and encode
rising alongside it. That points at the present path -- the submit, the fence
and the readback -- accumulating back-pressure, which is step 4 of the goal and
was never started. It is not the geometry, the library or the encode: those are
each doing the same work in the first window and the last.

### The drift was churn, and `encode-us` over-attributes

With hit and miss counters on the packet cache the question settles:

```
01:15:24  encode-cache hits=4641 misses=110   geometry-us=4922 encode-us=3337
01:15:26  encode-cache hits=4671 misses=110   geometry-us=4935 encode-us=3321
01:15:30  encode-cache hits=4731 misses=110   geometry-us=4919 encode-us=3329
01:15:31  encode-cache hits=4761 misses=110   geometry-us=4967 encode-us=3338
01:15:33  encode-cache hits=4789 misses=112   geometry-us=7850 encode-us=6226
```

Hits advance by exactly thirty per window -- the window length -- with misses
flat, so the cache holds on every frame of those windows. 97.7% cumulative.
The window that jumps is the one whose churn line reports `added=2`: new
geometry genuinely has to be encoded, and a window where the mesh set changes
is not a settled window.

The earlier runs that drifted to 19,000 us were runs with continuous churn, not
runs where a cache had stopped working.

Two caveats on the numbers themselves, both mine:

- `encode-us` is measured by a timer declared at function scope, so it runs to
  the end of `BuildLiveSceneGeometry` and includes the scene packet encode and
  everything after it. It is an upper bound on the raster encode, not the
  raster encode. On a cache hit the raster encode is a hundred-byte header
  patch, so most of that 3,337 us is something else inside the same span.
- `geometry-us` contains `library-us` and `encode-us`, so the three are not
  independent budgets and cannot be added.

Neither changes what the mirror does; both change what the log means, which is
worth stating before someone reads the three numbers as separable.

## The ground was never rendered, and a log line that lied about it

The mirrored scene has no landscape. The reading that it was showing the world
twice was wrong: what is below the horizon is the undersides and far faces of
objects, visible because there is no ground in front of them.

The cause is not a regression. Searching for who fills the terrain slot:

- `abi::RasterFrameRequestV1` carries `terrainData`/`terrainSize`.
- The backend consumes it and has a full terrain path (`phase14TerrainActive`).
- The only code that ever fills it is `src/tools/PacketReplayMain.cpp`, the
  offline replay tool.
- `RendererBackendProbe.cpp` -- the live mirror -- contains the word terrain
  exactly once, in an unrelated comment.

So Phase 14 is offline-complete and was never promoted to the live mirror,
which is what the status line at the top of this file already said: "live
promotion for 8-27 outstanding". The ground has never been in a mirrored frame.

### A diagnostic that printed whatever was on the stack

Chasing this produced a worse finding. The `renderer-draws` line had twenty
conversions and sixteen arguments: `depth-only`, `offscreen`, `attributed` and
`slots` had nothing behind them. Two perl edits had added the format text while
the matching argument edits silently failed to apply, and nothing complained.

They printed as zeros. Zero is a plausible measurement, so it was read as
evidence that the two new rejection rules were rejecting nothing -- which very
nearly closed the investigation on the wrong answer. It also made `rejected`
disagree with the sum of its own reasons by two and a half thousand draws, and
that discrepancy was itself dismissed as a puzzle rather than treated as the
symptom it was.

Three things came out of it:

- The arguments are supplied, and the line now prints its own `attributed`
  total beside `rejected`. A breakdown that cannot be checked against its total
  is worse than none, because it reads like a measurement.
- `log::Write` is annotated `_Printf_format_string_`. That only binds under
  static analysis rather than /W4, so it documents the contract more than it
  enforces it.
- `tools/lint/check-log-formats.pl` counts conversions against arguments at
  every call site. Across 204 sites in the plugin, the core and the tools, the
  only mismatch was the one above. The checker was wrong twice before it was
  right -- counting the format string as an argument, then counting commas
  inside comments -- and each time the fix was to make it disagree with the
  code for a reason, not to accept a clean result.

## Phase 24 step 1: why the CPU round-trip exists

The premise I wrote into the goal was wrong. It said the Phase 5 bridge "was
built with shared images and a shared fence; the mirror simply does not use
that path". The mirror cannot use it.

Reading the bridge end to end, the D3D side is already right and already
GPU-only:

1. D3D signals the shared fence at `releaseValue`; Vulkan waits on it.
2. The backend writes into the shared image `images[imageIndex]`.
3. The backend signals `readyValue`.
4. D3D calls `context4->Wait` -- a **GPU-side** wait, which does not block the
   CPU -- and then `CopyResource` from the shared image to the backbuffer.

There is no readback anywhere in the bridge. The round-trip is entirely inside
the backend, and the reason is structural:

```
src/renderer_backend/VulkanInteropBridge.cpp:510   vkCreateDevice(...)
src/renderer_backend/VulkanRasterRenderer.cpp:1350 vkCreateDevice(...)
```

`BackendContext` holds a `bridge` and a `raster`, and each creates its **own
VkDevice**. The bridge's images are shared with D3D on the bridge's device; the
frame is rendered on a different device. An image cannot cross that boundary,
so the only route from one to the other is host memory -- which is exactly the
readback and re-upload that costs the frame.

So removing the round-trip is not "use the existing path". It needs one of:

- **One device for the backend.** Both already select the physical device
  carrying the engine's adapter LUID, so they are the same GPU; the merged
  device needs the union of the features (external memory from the bridge,
  ray tracing and descriptor indexing from the raster renderer), which the
  physical device demonstrably supports because the raster renderer already
  creates such a device today. This also deletes a duplicate device, queue and
  allocator.
- Or exporting the raster output as external memory and importing it into the
  bridge device: a second interop hop between two devices on the same card,
  which is more machinery to reach the same place.

The first is the correct one. It is a larger change than the goal's step 3
described, and the goal's own "where I expect trouble" note -- that step 4 may
be larger than it reads -- is now confirmed with a specific reason rather than
a suspicion.

## The outer shell was culled: the engine's cull mode was captured and dropped

Reported from a live loading screen: rotating the object showed its interior
rather than its outer shell. Not the winding this time -- the winding is read
from the engine and was already right. The faces were being culled.

Two facts, both in the code rather than inferred:

- `cullMode` is captured from `RSSetState`, carried through `DrawRecordV1` and
  into `AssembledMesh` -- and then read by nobody. `EngineDrawStream.cpp`
  contained zero uses of it.
- `AssembleSceneGeometry` never emitted a single visibility record, so
  `ScenePacket::visibility` was always empty. The packet defines that as "every
  object is opaque, front-facing only, and unmirrored".

So every mirrored object was back-face culled however the engine had drawn it.
A two-sided model loses its outer shell and shows its interior, which reads as
inverted geometry rather than as a missing cull mode -- and that is the second
time this session that a culling symptom has had a cause other than winding.

The assembly now emits one record per surviving object:

| engine | record |
| --- | --- |
| `D3D11_CULL_NONE` | `FaceMode::TwoSided` |
| `D3D11_CULL_FRONT` | `FaceMode::BackOnly` |
| `D3D11_CULL_BACK` | `FaceMode::FrontOnly` |
| never observed | `FaceMode::FrontOnly`, D3D11's own default |

`modelDeterminant` is filled from the object's transform at the same time. It
had been left at 1.0, which told the backend no placement was ever mirrored;
the backend already folds the determinant into the effective front face, so it
was the mirror withholding the input rather than the backend ignoring it.

Three mutations killed: two-sided falling through to front-only, front-cull
falling through, and emitting no records at all. 387 tests pass in both
configurations.

Worth naming: the backend has honoured `faceMode` and `modelDeterminant` since
phase 15. Everything needed to draw these objects correctly was already there,
and the mirror simply never filled the fields in. That is the same shape as the
texture library, the winding and the terrain -- offline-complete, live-empty.

## Inside out, third cause: the view reverses the winding the engine declared

Two-sided models had started rendering correctly and single-sided ones had not,
which is the detail that separated this from the previous two culling bugs. A
hat showed the inside of its crown; a deathclaw and a house looked right. The
difference is not the model, it is that `CULL_NONE` keeps both faces whatever
the winding says, so the visibility fix hid the remaining defect on exactly the
objects that would have shown it.

The engine declares a front face against its own clip space. The mirror
re-projects those triangles through the captured view, and that view reverses
orientation -- so the declared winding arrives inverted and every single-sided
model is culled on the side it meant to keep.

`view::ViewOrientationSign` derives this from the matrix rather than assuming
it: the determinant of the upper-left 3x3 of the view-projection is negative
exactly when the pair reverses handedness. A packet fixture, authored directly
in packet space, gets +1 and is unaffected; a live capture gets -1 and has its
declared winding flipped. One rule, correct for both, and no constant to guess
at -- which matters, because guessing the polarity is what produced the first
two wrong answers.

Measured on the mirrored Sanctuary frame, over a fixed sample of 102,480
pixels:

| | tinted pixels | dominant colour |
| --- | --- | --- |
| before | 12.3% | flat `(0,188,188)`, 3,884 samples across two exact values |
| after | 3.7% | scattered, the largest single colour 8 samples |

The flat constant is gone entirely. A back-facing surface shaded to a single
value across whole regions, which is why the symptom read as a colour bug and
was chased as one -- the histogram said the colour was constant, and a constant
is not something an albedo texture produces. That was the clue that mattered
and it took two wrong theories to use it.

What is left is diffuse and varied, and Sanctuary's houses are painted teal in
vanilla, so some of the remaining 3.7% is the scene rather than a defect. It is
not yet separated, and it is not claimed as fixed.

Still missing: the landscape, which the mirror has never sent (phase 14 live
promotion), so the ground below the horizon is still open sky.

## Inside out, fourth cause: the winding was right and the convention was not

A loading-screen book settled this one. The cover read backwards through its
own spine: not a mirrored image -- the book sat upright and only the face
nearest the camera was missing, with the far one showing through it. That is
the exact signature of an inverted cull, and it is the fourth distinct cause
this session to produce the same visible symptom.

Before changing anything, every input that combines into the final cull
direction was reported together, because each had been reasoned about
separately and each was individually defensible -- which is how a pair of them
cancels and a third leaves the whole scene inverted. Measured on the Sanctuary
frame:

| input | measured | consequence |
| --- | --- | --- |
| `view::ViewOrientationSign` | `+1.000`, flips on 0 frames | the third cause never fires live |
| engine `FrontCounterClockwise` | 1043 of 1043 meshes | the engine's answer is uniform |
| `modelDeterminant < 0` | 0 objects | `EffectiveFrontFace` is a no-op here |
| `FaceMode::TwoSided` | 416 of 1043 | 40% survive either way |

The third cause, then, was inert: the captured view does not reverse
orientation, and the flip added for it has never once fired in a live frame. It
is kept because a view that did reverse would still need it, but it was not
what fixed anything, and the pixel measurement that appeared to confirm it was
measuring tint, not winding.

The real defect is a convention mismatch. D3D11 decides facing in *screen*
space, after the viewport transform, where Y points down. The packet's
`frontFace` is mathematical NDC, where Y points up -- that is what the backend
documents and what every offline fixture is authored against. A Y flip reverses
a triangle's signed area, so the two differ by exactly one inversion, and the
probe was copying the engine's flag across without it. The backend then applied
its own Y-down inversion and landed on the opposite of the engine's rule.

Reading the rasterizer state from the engine was necessary and not sufficient:
the value was right, the convention was not, and the stream carried the
engine's own answer while still drawing every single-sided model inside out.
The 416 two-sided meshes in the same frame kept both faces regardless, which is
why this looked intermittent rather than total.

`visibility::PacketFrontFaceFromEngine` names the conversion and is applied
where the engine's declaration becomes packet data. Deliberately not a bare `!`
at the call site: three inversions exist in this path and two of them cancel,
so a negation is indisputable only until somebody counts them.

Mutating it back to the identity mapping fails both the new test and the
existing winding test. 389 tests pass in debug and in release. Live, all 642
draws now declare clockwise in NDC, which the backend maps to
`VK_FRONT_FACE_COUNTER_CLOCKWISE` -- the engine's own rule.

### The ground is not a decoding problem

The engine's 42 vertex declarations were dumped from its own
`CreateInputLayout` calls rather than inferred. None carries landscape blend
weights. The two layouts holding `TEXCOORD4`-`TEXCOORD7` put them on input slot
1 as four half4s, which is instance data -- a per-instance matrix -- not the
eight land channels a terrain vertex needs.

So the mesh decoder is not silently dropping the landscape: the stream that
draws it has not been found in the capture at all. `BuildLayoutFromInputElements`
does skip `TEXCOORD` past index 1, which was the first suspicion, but nothing
in the engine's declarations would be recovered by mapping them. A per-format
histogram of which declarations the world geometry actually draws with, and how
much geometry each carries, is now reported every 120 frames to narrow this.

The dump has to be incremental. A one-shot version ran on the load screen and
reported 11 of the 42 an exterior cell ends up declaring, and none of the 11
was a candidate -- a measurement taken at the wrong moment, which reads exactly
like a measurement that answers the question.

Unresolved and not claimed: the texture capture failed to complete on two of
five live runs, including runs predating this change.
