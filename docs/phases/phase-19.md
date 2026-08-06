# Phase 19 — Ray-traced reflections and ray-footprint LOD

Status: offline implementation complete, GPU vertical included; live capture
gate pending

## Implemented slice

`EngineReflection` (`src/renderer_core/EngineReflection.h/.cpp`, namespace
`vf::renderer::reflect`), mirrored branch for branch by
`shaders/phase19/reflection.glsl`.

- **F0 separates dielectrics from metals.** A dielectric reflects about 4% at
  normal incidence whatever its colour; a metal reflects its own base colour
  and has no diffuse term. One rule for both makes gold grey or plastic gold.
  Intermediate metalness interpolates, because captured materials do author
  it. Fresnel rises to one at grazing incidence, which is the rim that makes a
  wet or polished surface read as one.
- **A zero-roughness lobe is a delta, not the limit of a sampling scheme.** A
  mirror samples its mirror direction exactly, whatever the sample is.
- **Samples below the surface are rejected, never clamped.** Clamping piles
  every rejected direction onto the horizon and draws a bright ring around
  each rough surface at grazing angles. At roughness 0.45 more than half of
  256 samples survive, and a rejection scheme that rejects everything is not a
  sampler — the test pins both ends.
- **Sampling is deterministic from the pixel alone.** A reflection pass has no
  cheap way to carry per-pixel state, and the oracle and the GPU must land on
  the same pair from the same inputs or the fixture compares two different
  sets of rays and reports the difference as a reflection error. Pixel, frame,
  and sample index are each mixed with a distinct constant, so a whole tile
  cannot collapse onto one direction and band.
- **A two-sided hit flips its normal toward the ray.** Without the flip every
  dot product goes negative and the reflection resolves to black on exactly
  the surfaces two-sided rendering exists to support. A single-sided surface
  keeps its normal, so a ray that leaked through geometry stays visible as
  one instead of looking like a valid front-face hit.
- **The roughness cutoff is policy, not material.** The same material traces
  or does not purely by `ReflectionPolicy`; if the cutoff lived in the
  material a quality preset would have to rewrite captured records to take
  effect.
- **A missed ray resolves by what was captured.** A probe is a local
  measurement and beats the global environment. Indoors with neither, the
  result is `Unresolved` and contributes nothing: substituting an exterior sky
  indoors is the light leak this rule prevents, and zero is visible in the
  frame where a plausible grey would be accepted by every later comparison.
- **A history survives only its own epoch.** A camera cut, a resolution
  change, or a different view rejects it. Rejecting costs one frame of
  convergence; accepting smears the previous scene across the new one for as
  long as the history lives.

## Ray footprint, because hit shaders have no derivatives

A cone carries the footprint a ray represents. Its width grows by the spread
over the distance travelled; the spread itself does not change along a
straight segment. The mip level is the base-2 log of the texel count the cone
covers at the hit — the triangle's own UV-to-world ratio times the texture's
texel dimension — which is what a derivative would have produced had a hit
shader had one. A cone that does not grow selects mip 0 at every distance and
aliases the moment a reflection is minified.

The initial spread is the pixel's own footprint plus the lobe's characteristic
width (GGX alpha). Without the pixel term a minified mirror reads mip 0 at
every distance; without the roughness term the reflection reads texels finer
than the lobe it represents. Degenerate triangles and zero-sized textures are
refused rather than producing an infinite level.

The mip test pins the **absolute** level, not only how it moves. An earlier
version checked differences alone, and a mutation that inverted the
texel-density ratio passed it — that mutation reads mip 0 where it should read
the coarsest level, and aliases on exactly the dense textures the footprint
exists to tame.

## The GPU vertical

- The reflection reuses the Phase 18 top-level structure. Nothing new is
  built: the same geometry that casts shadows is what a reflection sees.
- **A ray query recovers which geometry it hit but has no vertex attributes
  bound.** A new storage buffer at binding 18 maps a bottom-level geometry
  index back to the object whose records describe it, so a hit reads the same
  per-object normal and family record on both sides. Without it a hit cannot
  be shaded at all.
- The hit is shaded through `vfShadeSurface` — the same function the raster
  pass uses — so a reflection cannot disagree with the surface it reflects.
  That includes the Phase 18 shadow term: the oracle shadows its reflection
  hits through the same geometry, because a reflection of a shadowed surface
  that is brighter than the surface itself is the artefact, not a subtlety.
- The engine authors specular colour and smoothness, not metalness, so there
  is no metalness channel to read. The environment-map feature is the closest
  declared signal a captured material gives for a metal-like surface and is
  used as one until a real channel is captured. Both sides apply the identical
  rule, so the approximation is shared rather than a place they can drift.

## Fixture

`vf_packet_replay --render-family-scene --reflections` makes the appended
object a smooth metal (roughness 0.05, environment-map family), so it carries
a lobe narrow enough to trace and an F0 high enough to see.

The reflection is isolated by **three** reference renders, each adding one
term: neither, reflections only, then reflections and shadows. The difference
between the first two is the reflection; between the last two, the shadow.
Rendering only two would leave a mask marking every pixel either term touched,
and the interior comparison would stop isolating the boundary it exists to
exclude — which is exactly what happened first time and showed up as 54
phantom mismatches.

Measured, not assumed:

| Metric | Value |
| --- | ---: |
| Pixels the reflection term changes | 13,489 |
| Largest reflection change | 0.114 |
| Pixels the shadow term darkens | 7,278 |
| Shadow-interior pixels compared | 41,981 |
| Shadow-interior mismatches | 0 |
| Largest interior error | 0.0111 |
| Validation errors | 0 |

## Mutation evidence

| Mutation | Result |
| --- | --- |
| Interior falls back to the exterior sky | `P19_missed_rays_resolve_by_captured_environment` fails |
| Mip ignores texel density (ratio inverted) | `P19_cone_growth_drives_mip_selection` fails on the absolute level |
| GPU drops the reflection term | `contract.reflected_scene_frame` fails with 1,287 interior mismatches |

## Artifacts

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `artifacts/phase-19/reflected-debug.ppm` | 147,471 | `7B4593C5F553253C9996DB548A1A72025F65334C0E591B1188229B981183075A` |
| `artifacts/phase-19/reflected-release.ppm` | 147,471 | `7B4593C5F553253C9996DB548A1A72025F65334C0E591B1188229B981183075A` |
| `artifacts/phase-19/reflected-debug.vfscene` | 992 | `8D45D60A0783D9C4FFBD4392FCE9E9A1CD514A53C6B08851868D52471BACBB29` |
| `artifacts/phase-19/reflected-release.vfscene` | 992 | `8D45D60A0783D9C4FFBD4392FCE9E9A1CD514A53C6B08851868D52471BACBB29` |
| `artifacts/phase-19/reflected-debug.vffl` | 512 | `50FA96ED43E3D1B360BAEE89C83270575AC6423001396CD86C91D3534782BCE3` |
| `artifacts/phase-19/reflected-release.vffl` | 512 | `50FA96ED43E3D1B360BAEE89C83270575AC6423001396CD86C91D3534782BCE3` |
| `artifacts/phase-19/reflected-debug.vfgbuf` | 3,145,728 | `C45B999A900125AA6936CE88269659D035127ACFD83B9597CF7B23112AD9F9D9` |
| `artifacts/phase-19/reflected-release.vfgbuf` | 3,145,728 | `C45B999A900125AA6936CE88269659D035127ACFD83B9597CF7B23112AD9F9D9` |

Debug and Release are byte-identical across all four. 269/269 tests pass in
both configurations.

**This phase re-baselined Phase 18's colour artifact**, from `DDF7F2AE...` to
`C4764D90...`. The reflection term is added to every lit surface, so the
shadow fixture's colour output legitimately moved while its scene, light, and
G-buffer artifacts stayed byte-identical — the G-buffer carries albedo and
normals, not reflected radiance. Phase 18's shadow contract still passes
unchanged. A later phase making the renderer do more is a re-baseline, not a
regression, and distinguishing the two is the reason the hashes are recorded.

## Not in this phase

The plan's slice is wider than what is verified here. What is outstanding is
named rather than implied by omission:

- **No texture fetch at a reflection hit.** The hit's albedo is the object's
  family tint, not its base-colour texture, because a ray query has no UVs
  bound. `SelectMipLevel` and the ray cone are implemented and tested against
  the oracle, but nothing samples a texture with the level yet. The hit
  attribute and index-buffer bindings that would make it possible are the next
  step, and they are what turns the footprint from a tested rule into a
  visible one.
- **No temporal or spatial filtering.** `ReflectionHistoryKey` and
  `ResetHistory` define when a history may be reused and are tested, but no
  history resource exists yet, so a rough reflection is one raw sample per
  pixel and will be noisy. The plan's convergence and ghosting gate cannot be
  claimed until the filter exists.
- **One sample per pixel.** `ReflectionPolicy::samplesPerPixel` is carried and
  honoured by the oracle but the shader traces once.
- **No probe capture.** The probe path is implemented, tested, and exercised
  by the oracle, but nothing captures a probe from the engine yet, so the
  fixture runs with `probeAvailable` false and interiors resolve to
  `Unresolved`.
- **Metalness is inferred**, as described above, rather than captured.
