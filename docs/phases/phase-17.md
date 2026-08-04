# Phase 17 — Lights, sky, weather, fog, and raster parity

Status: offline implementation complete; live capture gate pending

## Implemented slice

Built on RTTI reconstructed from the installed binary, recorded in
`engine_render.md` 9.12a, so the light taxonomy is enumerated rather than
assumed. The engine has exactly four concrete `NiLight` subclasses —
`NiAmbientLight`, `NiDirectionalLight`, `NiPointLight`, `NiSpotLight` — all
sharing the 58-slot base vtable. `BSShadowLight` is a **separate 15-slot
object, not a fifth light type**, which is why shadow casting is a flag on a
light here: modelling it as a type would make "a point light that casts
shadows" inexpressible. `ShadowSceneNode` (67 slots) is the accumulator's
container and therefore the light-list capture boundary.

- `EngineLighting` (`src/renderer_core/EngineLighting.h/.cpp`, namespace
  `vf::renderer::lighting`): 112-byte `LightRecordV1`, 112-byte
  `EnvironmentRecordV1`, 80-byte `GpuLightRecordV1`, 112-byte
  `GpuEnvironmentV1`, 64-byte `LightPacketHeaderV1`, and the `.vffl` packet.
- **The shadow term is declared unavailable** (`ShadowTermAvailable()` is
  `false`) for the whole phase. That is the gate's own requirement: a parity
  metric can mask the term rather than compare a lit mirror against a
  shadowed vanilla frame and report the difference as error.
- Attenuation is clamped to the captured radius, so a light list can be
  culled by radius without changing the image. All-zero coefficients divide
  by zero at every distance and are refused, not defaulted.
- Cones are stored as **cosines**, because that is what a shader compares a
  dot product against; converting per fragment would repeat arithmetic for
  nothing. An inner angle exceeding its outer is a captured contradiction and
  is refused rather than silently reordered.
- The dimmer scales radiance and is kept **separate** from the colour, so a
  consumer can still read back the authored colour. A negative dimmer would
  subtract light from the scene.
- Light positions follow the terrain rule: the subtraction against the camera
  origin happens in **double** and only the small residual is narrowed. A
  light too far to narrow is `PositionOutOfRange` rather than silently
  swimming.
- **Interior zeroes the sun and moon.** Carrying an exterior's sun into an
  interior is exactly the stale-state bug the gate names.
- Interior and exterior **cannot be blended**. The engine cuts between them;
  a cross-fade would invent a state the game never shows.
- Blended sun and moon directions are renormalized, because interpolating two
  unit vectors shortens them and would dim the light mid-transition.
- Fog saturates at the **captured maximum**, not at one, or distant geometry
  would vanish into fog the engine never applied.
- Light-list overflow is deterministic and reported: ranked by brightness
  over squared distance, ties broken on captured order then identity, with
  `droppedCount` and `overflowed` surfaced. A list that dropped different
  lights each frame would make a static scene flicker.

### One evaluation, two mirrors

The evaluation exists once on each side and nowhere else:
`lighting::EvaluateDirectGpu` / `EvaluateFogGpu` / `ShadeSurfaceGpu` in C++,
and `vfDirectLighting` / `vfFog` / `vfShadeSurface` in
`shaders/phase17/lighting.glsl`, branch for branch.

Both evaluate the **GPU** records, not the host records. That is deliberate:
the host record holds a world position in double while the GPU record holds a
camera-relative float, so evaluating the host form on the CPU would compare
two different scenes and call the difference error.

### Backend

- Reflected `GpuLightRecordV1` at set 0 binding 15 and `GpuEnvironmentV1` at
  binding 16, both asserted from SPIR-V reflection at build time.
- ABI minor 10 appends `lightData`/`lightSize` at 208/216 without moving the
  Phase 6–16 prefixes. The request is now 224 bytes.
- A frame with no light packet uploads a zeroed environment whose
  **`EnvironmentPresent` bit is clear**, and both the shader and the
  reference read that as "this frame has no lighting" and leave the albedo
  alone. Without the explicit declaration a zeroed environment would read as
  ambient zero and black out every surface — which is exactly what happened
  before the flag existed, and is why the absence is declared rather than
  inferred from zeros.
- Light positions are narrowed against the same camera origin the geometry
  uses, so a light and the surface it lights agree about where they are.

## TDD/RGR evidence

1. **The interior stale-sun rule was mutation-tested.** Passing the captured
   intensity straight through instead of zeroing it for interiors fails
   `P17_environment_state_selects_interior_and_exterior_without_stale` with
   `3.0f == Approx(0.0)`.
2. **The lighting evaluation itself was mutation-tested.** Replacing the
   `max(0, dot(N,L))` cosine with a constant `1.0` in the shader fails the
   replay with `hdr-max-error=1.11852` against a declared 1e-2 threshold. The
   comparison measures the lighting math, not merely its presence.
3. **The reference lit the wrong point in space.** The first lit replay
   reported `hdr-max-error=0.0276641` — too large for half-float storage and
   therefore a real rule difference. `ProjectScenePacket` stores **clip
   space** in the projected vertices, so the reference was interpolating
   screen positions while the shader interpolated camera-relative world
   positions. Fixed by having projection preserve the camera-relative world
   position alongside each vertex; the error fell to `0.00454712`, which is
   half-float precision at these magnitudes.
4. **A lit fixture must prove lighting changed the frame.** `lit-pixels`
   counts pixels whose shaded colour differs from raw albedo and requires
   more than an eighth of the frame. Without it the comparison would pass
   just as well against a renderer that ignored every light, because agreeing
   on "unlit" is still agreeing.

## Replay and artifact evidence

`vf_packet_replay --render-family-scene --lit`, validation enabled, Debug and
Release both report:

```text
family-replay extent=256x192 families=2 lights=2 lit-pixels=11974
tint-pixels=8844 expected-tint-pixels=8844
emissive-pixels=3130 expected-emissive-pixels=3130
hdr-max-error=0.00454712
normal-encodings-differ=yes lobe-differs=yes
gbuffer-identity-mismatches=0 gbuffer-max-error=4.38094e-06
interior=46691 interior-mismatches=0
validation-errors=0 result=pass
```

- Two light **types**, not one: a directional sun and an attenuating point
  light. A fixture with only a directional light cannot tell an attenuation
  bug from a working renderer.
- 11,974 lit pixels out of 49,152 — the lighting demonstrably changes the
  frame rather than merely being uploaded.
- Worst HDR channel error 4.5e-03 against a declared 1e-02 threshold, which
  is half-float storage precision at these magnitudes.
- The G-buffer is **unchanged** by lighting: `phase17-lit.vfgbuf` is
  byte-identical to `phase16-family.vfgbuf` (`027835A9…`). Lighting belongs
  in the HDR target, and the artifact hashes prove the separation held.

Artifacts:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `artifacts/phase-17/lit-debug.ppm` | 147,471 | `F18ACFC9CF32EEDA165340A1DD2A2C3E39D43A6FC090221B788A7385690270BA` |
| `artifacts/phase-17/lit-release.ppm` | 147,471 | `F18ACFC9CF32EEDA165340A1DD2A2C3E39D43A6FC090221B788A7385690270BA` |
| `artifacts/phase-17/lit-debug.vffl` | 400 | `719B89601781111762574713424A87DB4CD74DE8D34D6A07FA65154D0A597166` |
| `artifacts/phase-17/lit-release.vffl` | 400 | `719B89601781111762574713424A87DB4CD74DE8D34D6A07FA65154D0A597166` |
| `artifacts/phase-17/lit-debug.vfgbuf` | 3,145,728 | `027835A9150DD117E0CCC27862A0C6B155B564B032CAE117A4B8C950635C6034` |
| `artifacts/phase-17/lit-debug.png` | 8,893 | rendered from the Debug PPM |

Debug and Release produce byte-identical light packets, scene packets, colour
output, and G-buffer readbacks. 243/243 tests pass in both configurations,
and the Phase 11, 14, 15, and 16 G-buffer artifacts are unchanged.

The PNG was visually inspected: the diffuse triangle is now dark, lit only by
the ambient term, the sun, and the point light and then fogged, while the
emissive triangle stays bright. That separation is the point — emission is
radiance a surface emits and is added after shading, so lighting must not
change it.

## Deferred within this phase

Stated gaps, not silent ones:

- **Shading is forward, not deferred.** Lighting is evaluated in the geometry
  pass and written to the HDR target. A separate deferred pass that re-reads
  the G-buffer is the natural refactor and is the right time to do it is
  Phase 18, when ray-traced shadows need the G-buffer anyway.
- **Sky and weather resources are not rendered.** The environment record
  carries sun, moon, ambient, and fog, and weather transitions blend between
  two environments, but no sky dome or cloud layer is drawn.
- **The moon contributes nothing yet.** Its direction, colour, and intensity
  are captured, validated, blended, and uploaded, but only the sun and the
  explicit light list are evaluated.
- **No live capture of engine lights.** The light list is a fixture. The
  capture boundary is identified (`ShadowSceneNode`) but not hooked.

## Promotion decision

The light taxonomy, attenuation, cone falloff, colour and intensity
conversion, camera-relative double positions, interior suppression,
non-blendable interior/exterior, captured-maximum fog, deterministic reported
overflow, the `.vffl` packet, ABI minor 10, reflected bindings 15 and 16, the
mirrored GPU/CPU evaluation, and the Debug/Release regression are offline
complete.

Phase 17 is **not live-promoted**. Its exit gate additionally requires
captured engine lights and weather — real `NiLight` instances read from
`ShadowSceneNode` in a running Fallout 4 — compared against this mirror
inside the declared unshadowed parity envelope.
