# Phase 16 — Specialized opaque material families

Status: offline implementation complete; live capture gate pending

## Implemented slice

- `EngineMaterialFamily` (`src/renderer_core/EngineMaterialFamily.h/.cpp`,
  namespace `vf::renderer::material`) extends the Phase 9 boundary rather
  than replacing it, so `CanonicalMaterial` and the transfer LUT are
  untouched.
- `MaterialFamily` mirrors the engine's twenty-one lighting feature IDs
  exactly (0 Default … 20 Dismemberment), with `None` for the engine's `-1`
  and `Unknown` for anything else. An unclassified ID is **never** folded
  into Default: a silent Default renders an unknown family as an ordinary
  surface and looks plausible while being wrong.
- The complete 64-bit shader property flag map is recorded in
  `PropertyFlag` — all sixty-four bits, not just the ones consumed, because
  a partial copy invites reading a neighbouring bit by accident.
- Slot roles are resolved **per family**, which is the point of the phase.
  Recorded role 7 is backlight-mask *and* smooth-spec; Face, SkinTint, and
  HairTint read it as a backlight mask and everything else reads the
  smoothness/specular mask. Reading it as the wrong one silently turns a
  smoothness map into a rim-light mask.
- Slots 8 and 9 of the ten-slot texture set carry no recorded role. An
  authored one increments `diagnostic.unclassifiedAuthoredSlots` rather than
  being assigned a meaning.
- **Nothing is derived from whether a texture happens to be authored.** An
  authored texture is not a declaration that it is used. Equally, captured
  scalars are not a licence to enable a lobe: a Default material with a
  non-zero subsurface rolloff resolves with subsurface off and a zeroed
  rolloff.
- Rejections rather than clamps: `InvalidEyeTransform` for a non-positive
  eye radius or iris scale, `InvalidParallaxRange` for a zero or inverted
  step range, `InvalidLayer` for a refraction index below one,
  `MissingRequiredSlot` for an unauthored base colour, height, inner layer,
  or glow map, and `NonFiniteSource` for any non-finite scalar.
- LOD families never pay for a specialized lobe whatever their captured
  scalars hold — parallax, subsurface, and anisotropy stay off and
  `features.reducedDetail` is set. A distant object paying for a specialized
  lobe is both wrong and slow.
- Twenty-one families collapse into eight broad `ShaderClass` values
  (Standard, Skin, Hair, Eye, Parallax, MultiLayer, Terrain, Lod). That is
  the refactor requirement: everything else that varies rides along as
  feature data, so pipeline count does not grow with content.
- `RequiresDescriptorRebuild` / `RequiresDynamicUpdate` split static from
  dynamic. A rebound texture, changed family, changed slot layout, or bumped
  `staticRevision` rebuilds; a wetness change moves only the dynamic
  revision. Without that split every rain transition would rebuild
  descriptor sets.

### The emission declaration rule

Emission is authorized by a declaration and never by a bright colour: a glow
map slot, `OwnEmit` (bit 22), `ExternalEmittance` (bit 29), or the GlowMap
family itself. A saturated albedo is ordinary in authored content, and
reading it as emission makes plain surfaces glow. Externally driven emission
is owned by the reference rather than the material, so the material asserts
no colour of its own.

### The normal-encoding rule

`engine_render.md` records that model-space normals must never pass through
the tangent-normal path. The two decodes differ in **two** ways, and both
matter:

- A model-space texel reads three channels and is already absolute, so a
  two-sided back face flips it like any other normal.
- A tangent-space texel reads two channels, reconstructs Z from the unit
  length it was compressed against, and stores that Z **along the surface
  normal** — so it has to be rotated into the surface frame before it means
  anything. Treating it as already absolute points every normal away from
  the surface.

The frame is Duff et al.'s branchless orthonormal basis, built from the
already face-signed geometric normal and mirrored branch for branch between
`family_shading.glsl` and `EngineScene.cpp`. The decoded shading normal is
then lifted onto the geometric horizon by the same rule
`visibility::ResolveShadingFrame` uses.

### Backend

- Reflected `GpuFamilyRecordV1`: 144 bytes at set 0 binding 14, reflected
  from `scene_layout.comp` so its size and binding are build-time assertions
  rather than a hand-maintained copy.
- `.vffam` packet: 64-byte header, 368-byte `FamilyRecordV1`. The wire record
  is laid out explicitly rather than serialized from `FamilyDescriptor`, so a
  compiler's packing choice can never change what a captured artifact means.
  Pointer-free, CRC-protected, and validated for duplicate objects,
  unclassified shader classes, non-finite payloads, unknown slot roles, and
  non-zero padding.
- `ResolveFamilyRecord` gives an object with no captured family the ordinary
  lit surface, the same shape as `scene::ResolveVisibility`, so the binding
  is always valid and every consumer sees one rule.
- A family table naming an object the scene does not own is refused rather
  than ignored — it would otherwise apply to nothing, silently.
- Backend ABI minor 9 appends `familyData`/`familySize` at 176/184 and
  `hdrData`/`hdrCapacity` at 192/200 without moving the Phase 6–14 prefixes.
  The request is now 208 bytes. The HDR pair is separate so a caller can
  supply families without also asking for the float colour target.
- One `familyScenePipeline` serves all eight shader classes, with cull mode,
  front face, and depth compare as dynamic state. A frame carrying families
  routes its opaque draws through it; a frame without them keeps the Phase 11
  path exactly, which is why every earlier artifact is unchanged.
- `GBufferPixelV1` was deliberately **not** grown. Its sixty-four bytes are a
  captured artifact whose layout cannot change without invalidating every
  earlier comparison, so emission lands in a separate `HdrImage` and a float
  readback of the existing `R16G16B16A16_SFLOAT` colour target.

## TDD/RGR evidence

Every red below was observed and recorded before the fix.

1. **The emission rule was mutation-tested, not assumed.** Adding
   `capture.emitColor[0] > 1.0f` to the emission condition fails
   `P16_bright_base_colour_never_becomes_emission_without_the_flag` with four
   assertions (`90.0f == Approx(0.0)`). The gate has teeth.
2. **The reflected layout assertion was mutation-tested.** Adding one `vec4`
   to the GLSL family record fails the build with *"phase16 family record
   drifted from 144 bytes"*.
3. **The first replay fixture measured nothing, and passed anyway.** With the
   shader forced to ignore the model-space encoding, the fixture still
   reported `result=pass`. Two independent defects were behind it:
   - The `normal-encodings-differ` check compared object 0's normal to object
     2's. Those objects face different ways (yaw 0 and yaw 25°), so it was
     measuring geometry, not encoding.
   - The tangent decode was being treated as already in model space. The
     fixture's surfaces face `-Z` while a reconstructed tangent Z is always
     positive, so every tangent normal pointed away from its surface, the
     horizon lift flattened both decodes onto nearly the same plane, and the
     difference collapsed to 5e-4 — under the comparison threshold.

   The fix was the orthonormal basis above, plus a normal texel whose blue
   channel sits below the midpoint so both decodes land inside the geometric
   hemisphere, plus a check that predicts *both* decodes for the **same**
   object and asserts the render took the declared one. Re-running the same
   mutation now fails loudly: `normal-encodings-differ=no`,
   `interior-mismatches` 0 → 8320, `gbuffer-max-error` 4.4e-06 → 0.199 —
   which is exactly the predicted separation between the two decodes.

4. **"Fixture construction failed" named no cause.** The family replay
   reported one message for six distinct failure modes. Split into per-step
   diagnostics, which immediately identified `invalid-material` (a base
   colour texture must be sRGB) and then `texture-packet-failed` (a non-
   typeless resource cannot carry a differing view format).

## Replay and artifact evidence

`vf_packet_replay --render-family-scene`, validation enabled, Debug and
Release both report:

```text
family-replay extent=256x192 families=2
tint-pixels=8844 expected-tint-pixels=8844
emissive-pixels=3130 expected-emissive-pixels=3130
hdr-max-error=0.00265646
normal-encodings-differ=yes lobe-differs=yes
gbuffer-identity-mismatches=0 gbuffer-max-error=4.38094e-06
interior=46691 interior-mismatches=0
validation-errors=0 result=pass
```

Interpretation, against the five checks the phase gate names:

- **Coverage** — GPU and oracle agree exactly at 8,844 pixels for the tinted
  object and 3,130 for the emissive one. Compared as counts and per pixel.
- **Tint** — the tinted object's albedo is compared per pixel through the
  interior comparison; a wrong or missing tint is an interior mismatch. The
  rendered PNG shows it plainly as a green surface.
- **Lobe** — `lobe-differs=yes` means the anisotropic hair class changed the
  stored roughness away from the object's own `0.15`, so the class actually
  selected a lobe rather than passing the value through.
- **Emission** — checked in the float colour target, because an 8-bit
  tone-mapped output would clamp an emission of `(6, 3, 1.5)` away entirely.
  Worst channel error 2.7e-03 against a declared 1e-02 threshold, which is
  half-float storage precision at that magnitude.
- **Normal** — `normal-encodings-differ=yes` asserts the observed shading
  normal matches the model-space prediction within 1e-02 *and* differs from
  the tangent-space prediction by more than 5e-02.

- 46,691 interior pixels agree within 1e-3 with a worst channel error of
  4.4e-06, which is float rounding between the oracle's barycentric
  interpolation and hardware rasterization rather than a rule disagreement.
- Zero identity mismatches and zero validation errors with the family
  pipeline, per-draw dynamic state, and the HDR readback all active.

Prior-phase regression, checked by SHA-256 rather than by assumption:

| Artifact | SHA-256 | Matches archived |
| --- | --- | --- |
| `phase11-scene.vfgbuf` | `430372FF…1AE3CC7C` | yes |
| `phase12-instanced.vfgbuf` | `A68D118E…3724B307` | yes |
| `phase13-deformed.vfgbuf` | `CCF50E28…D006FB25` | yes |
| `phase14-terrain.vfgbuf` | `BDCBBEA7…AB38DF89` | yes |
| `phase15-alpha.vfgbuf` | `7614FA37…D4C67F45` | yes |

The Phase 16 fixture's own `.vfscene` is byte-identical to the archived Phase
11 scene packet (`AEEE65B1…F530CEC`), which is independent confirmation that
the visibility and family sections do not perturb a scene that has neither.

Artifacts:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `artifacts/phase-16/family-debug.ppm` | 147,471 | `04891ABC02C6E64E9FF25CC8A33B7E96685A6CDD215E04A2AE6C3ED9839D6B56` |
| `artifacts/phase-16/family-release.ppm` | 147,471 | `04891ABC02C6E64E9FF25CC8A33B7E96685A6CDD215E04A2AE6C3ED9839D6B56` |
| `artifacts/phase-16/family-debug.vfgbuf` | 3,145,728 | `027835A9150DD117E0CCC27862A0C6B155B564B032CAE117A4B8C950635C6034` |
| `artifacts/phase-16/family-release.vfgbuf` | 3,145,728 | `027835A9150DD117E0CCC27862A0C6B155B564B032CAE117A4B8C950635C6034` |
| `artifacts/phase-16/family-debug.vffam` | 800 | `1DD39E6BFF5D52D2D5163DF20BA41CF3DC31593DC0D82D19E967CB26632D1183` |
| `artifacts/phase-16/family-release.vffam` | 800 | `1DD39E6BFF5D52D2D5163DF20BA41CF3DC31593DC0D82D19E967CB26632D1183` |
| `artifacts/phase-16/family-debug.vfscene` | 768 | `AEEE65B1F320DE92D44312E776BDEA28898E99A58153411C82C4B8822F530CEC` |
| `artifacts/phase-16/family-release.vfscene` | 768 | `AEEE65B1F320DE92D44312E776BDEA28898E99A58153411C82C4B8822F530CEC` |
| `artifacts/phase-16/family-debug.png` | 15,708 | rendered from the Debug PPM |

Debug and Release produce byte-identical family packets, scene packets,
colour output, and G-buffer readbacks. 226/226 tests pass in both
configurations. The PNG was visually inspected: the left triangle carries the
green hair tint over its vertex-colour gradient, and the right triangle is
blown out to a warm white by an emission the flag authorized.

### Fixture note

Every texture is a single texel. One texel removes any disagreement between
hardware and oracle filtering rules from the comparison — the same discipline
Phase 14 used — while still making a wrong decode a wrong pixel. The base
colour is sRGB because the material boundary enforces that for the base role
and treats everything else as data; the resource is typeless so the view can
choose.

The normal texel `(140, 115, 5)` is chosen so both declared decodes land
inside the geometric hemisphere of surfaces that face `-Z`. A texel with blue
above the midpoint decodes to a model-space normal pointing away from the
surface, the horizon lift then flattens both decodes onto the same plane, and
the comparison silently measures nothing. That is precisely what the first
version of this fixture did, and it is why the texel is documented rather
than merely chosen.

Object 1 of the Phase 11 fixture is deliberately occluded and has no coverage
in any render, so it is left as the ordinary lit surface and exercises the
implicit-record path instead of being used as a subject.

Representative commands:

```powershell
cmake --build --preset vs2022-x64-debug
out/build/vs2022-x64-debug/tests/Debug/vf_unit_tests.exe "[phase16]"
ctest --test-dir out/build/vs2022-x64-debug -C Debug `
  -R "^contract\.family_scene_frame$" --output-on-failure
ctest --preset vs2022-x64-debug --output-on-failure

cmake --build --preset vs2022-x64-release
ctest --preset vs2022-x64-release --output-on-failure
```

## Deferred within this phase

Stated gaps, not silent ones:

- **The tangent frame is generated, not authored.** The engine uses the
  mesh's authored tangents; no capture supplies them yet, so a tangent-space
  normal will not match vanilla until a per-vertex tangent capture exists.
  The generated basis is deterministic and mirrored, so the mirror agrees
  with itself — it does not yet agree with Bethesda. `ShadingFrameInput`
  already accepts tangent and bitangent, so the capture is the missing half.
- **Greyscale-to-palette is declared but not applied.** The palette lookup
  texture's slot is not among the recorded role IDs, so applying a palette
  would mean inventing a ramp. The flags travel in the record.
- **POM marching is not implemented.** Scale, bias, UV scale, and the step
  range are carried and validated; the march itself is not in the shader.
- **The installed-corpus sweep has not been run.** The structural half of the
  gate's first clause is satisfied by construction — the fallback path means
  nothing can fail to resolve — but which families actually occur in
  `Fallout4 - Materials.ba2`, and at what frequency, is not yet measured.

## Promotion decision

The family classification, full property flag map, per-family slot roles
including the overloaded role 7, the emission declaration rule, the
normal-encoding rule and its orthonormal frame, LOD lobe suppression,
eye/parallax/layer rejections, eight broad shader classes, the static/dynamic
update split, the `.vffam` packet, ABI minor 9, the family pipeline, the HDR
readback, and the Debug/Release regression are offline complete.

Phase 16 is **not live-promoted**. Its exit gate additionally requires
captured engine material families — real feature IDs, real property flags,
and real texture sets read from a running Fallout 4 — compared against this
mirror. That capture is blocked behind the same unresolved item as Phases
8–15: the world camera has not been located, so no live frame can be
mirrored yet. See `journal.md` for the camera provenance conclusion.

## Closed since: glow-map modulation

`vfEmission` has taken a glow sample since this phase landed and was called
with a hardcoded `vec3(1.0)`, so a material declaring a glow map emitted its
colour unmasked. The map was captured, classified onto
`MaterialSlotRole::GlowMap`, uploaded to the device and read by nothing.

The binding was never the obstacle. The glow map is slot 2 of the shader
texture set -- `roles[2]`, set by `MaterialFamily::GlowMap` or by
`PropertyFlag::GlowMap` -- and the backend already binds that slot at
descriptor binding 3. The fragment shader simply declared no sampler there.

Both sides now read it, and both apply the same rule: a glow map is a *mask*
over the declared colour, so a material declaring one and sampling black emits
nothing, and one declaring none is unmodulated. `ReferenceEmission` takes the
sample through the same interpolated coordinates the base colour uses.

The fixture exercises it: object 2 declares `OwnEmit | GlowMap`, which makes
slot 2 required and authored, and the mask's blue channel is zero. A shader
that carries the map and never reads it emits blue where the material says it
must not, and the reference comparison sees it -- which is what makes the
mutation fail rather than pass quietly.

Mutation-verified: restoring the hardcoded white sample fails
`contract.family_scene_frame`.

## The corpus sweep

Run against `Fallout4 - Materials.ba2` with
`vf_packet_replay --sweep-materials <path>`. It needed three pieces this
renderer did not have -- a BA2 GNRL reader, a complete DEFLATE and zlib
decompressor, and a parser for BGSM and BGEM -- all of which are tested
against synthetic fixtures, since the sweep itself reads the installed game
and cannot be a ctest.

**6899 entries, 6899 extracted, 6899 parsed exactly.** The parser requires
each file to be consumed to the byte. That is the load-bearing check: a field
order that is wrong but self-consistent reads every field without complaint
and simply stops in the wrong place, so "it parsed" would not distinguish a
correct layout from a plausible one. Two layouts landing exactly on the end
for 6899 files is the evidence.

| family | count | share |
| --- | --- | --- |
| Default | 3991 | 57.8% |
| EnvironmentMap | 2356 | 34.2% |
| None (effect materials) | 283 | 4.1% |
| GlowMap | 151 | 2.2% |
| TreeAnimation | 60 | 0.9% |
| SkinTint | 20 | 0.3% |
| HairTint | 18 | 0.3% |
| Face | 12 | 0.2% |
| Eye | 8 | 0.1% |

Nine of twenty-three families occur, and two are 92% of the corpus. The
absent ones are not a gap: Landscape, Snow, LodLandscape and the rest are
selected by the engine from geometry and region data rather than from a
material file, so a material archive cannot hold them.

A material declares several of these at once, so the table is a stated rule
applied to the flags, most specific first. The flags are reported alongside
it and stay meaningful if the rule changes: `environment-mapping=2533`,
`specular-enabled=6149`, `alpha-test=1892`, `two-sided=913`,
`alpha-blend=611`, `subsurface-lighting=429`, `non-occluder=381`,
`decal=530`, `glowmap=154`, `emit-enabled=157`.

### It answers two other items on this page

The sweep counts authored texture slots, which settles two deferrals that
rested on the data not existing:

- **The greyscale-to-palette ramp is authored in the corpus** -- 287
  materials fill slot 3, and 319 declare the flag. The note above says
  applying a palette "would mean inventing a ramp"; it would not. The ramp is
  missing from the *recorded role IDs*, not from the game, which is a capture
  gap and a much smaller one.
- **POM has real height data** -- 58 materials author a displacement map.
  0.8% of the corpus, but the march would run against authored data rather
  than a synthetic fixture.

And one that argues the other way: `inner-layer=4`. Multi-layer parallax is
effectively absent and does not earn a shader path on this evidence.
