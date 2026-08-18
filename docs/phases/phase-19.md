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
- **One sample per pixel, and `samplesPerPixel` is honoured by nothing.** This
  document said the oracle honoured it and only the shader traced once.
  Checked: the field is read by no code at all -- `EvaluateReflection` takes a
  single sample and averages nothing, and no caller loops. The claim was
  written from the field's existence rather than from its use.

  Wiring it is not just a loop. Both sides would draw from `SampleSequence`,
  which is mirrored and would agree, but every extra sample is another traced
  ray, and averaging N of them multiplies the pixels where the oracle and the
  hardware intersector legitimately disagree against bounds set for one. It
  wants the same prerequisite the per-hit attribute work wants: a comparison
  that excludes pixels whose hit geometry differs between the two.
- **No probe capture.** The probe path is implemented, tested, and exercised
  by the oracle, but nothing captures a probe from the engine yet, so the
  fixture runs with `probeAvailable` false and interiors resolve to
  `Unresolved`.
- **Metalness is inferred**, as described above, rather than captured.

## The traced reflection is verified

`contract.mirror_scene_frame` is registered and renders three variants of a
purpose-built fixture: the mirror alone, the mirror with a target along its
reflected direction, and the same target with the mirror roughened past the
tracing cutoff. The geometry is computed rather than chosen -- a 45-degree
mirror at (0, 0, 3) reflects a view down +Z to exactly (-1, 0, 0), so the
target goes at (-1.5, 0, 3), facing the mirror.

The fixture already existed and had never been registered. Running it exposed
four defects in sequence, each hiding the next:

1. `acceleration ... instances=0` reports the scene's *explicit* instance
   table, which an implicit-instance scene leaves empty. It is not an empty
   structure, and reading it as one sends the investigation the wrong way.
2. `traced-vs-fallback` compares roughness 0.02 against 0.90. Roughness alone
   changes the BRDF whether or not a ray is cast, so it read 7,610 while every
   ray was missing. It is not evidence of tracing.
3. The material pipelines statically sample bindings 1 through 3, and a frame
   supplying only a base colour left two of them never written. The shader
   read undefined memory, the shading normal was garbage, and every reflection
   ray hit its own mirror 0.007 units away -- the ray-origin epsilon. The
   validation layer had been reporting it as
   `VUID-vkCmdDrawIndexed-None-08114` throughout.
4. The hit albedo was `tintColor`, which is zero unless a tint is *declared*.
   Every ordinary material reflected black. `shaders/phase20/indirect.glsl`
   carried the identical line, so the diffuse bounce was black on the same
   surfaces.

Mutation-verified: deleting the ray query's `rayQueryProceedEXT` loop now
fails the contract (`reflected-pixels` 10,167 to 0). The previous cycle
recorded that same mutation as byte-identical, which is what "unverified"
meant. Restoring the `tintColor` albedo also fails it.

## Still outstanding

- **No texture fetch at a reflection hit.** The hit shades from the object's
  declared base colour, which is a real per-object value rather than the zero
  the tint supplied, but it is still not the surface's base-colour *texture*.
  A ray query has no UVs bound; the hit attribute and index-buffer bindings
  that would make a fetch possible are the next step.
- **No temporal or spatial filtering**, one sample per pixel, and no probe
  capture. Unchanged from the list above.

## What the hit-identity mask settled, and what it did not

The contract now reports and excludes pixels where the device and the oracle
disagree about which geometry *and primitive* a reflection ray found, bounded
at one per cent of the interior so the exclusion cannot grow into a way of not
comparing the picture. On this fixture it excludes 117 of 41,981.

It does **not** make an interpolated per-hit attribute landable. Measured with
the vertex-colour interpolation re-applied: 117 excluded and 200 still
mismatching against a bound of 41. The disagreement is sub-primitive -- the
same triangle, a different point on it -- because the bottom level is built
from vertices transformed at build time while the oracle transforms its own
copy separately, so the two intersect independently-computed approximations of
the same triangles.

The prerequisite for texture fetch at a hit, for interpolated attributes, and
for more than one sample per pixel is therefore narrower than previously
recorded here: both sides must intersect **bit-identical** triangle data. That
is a change to how the reference derives its geometry, not a tolerance and not
a mask.

## Correction: the per-hit attribute blocker is a fetch defect, not a limit

This document previously recorded that interpolated attributes, texture fetch
at a hit and multi-sample reflections all needed both sides to intersect
bit-identical triangle data. Measured, that prerequisite is already met: the
device and the reference agree about where a reflection ray hits to
`5.1e-05` units, so the barycentrics that follow agree to about the same and
cannot account for the `0.036` error the interpolation produced.

The remaining explanation is that the two interpolate different *data*. The
device recovers its corner attributes through offsets this renderer computes
from the upload layout and the plan's vertex offset, with a separate 16-bit
index-unpacking path; reading from the wrong place there produces real values
from the wrong vertices, which is indistinguishable from a shading
disagreement until it is looked for.

The next step is to prove the fetch, not to change the reference: hold the
corner colours constant per triangle so interpolation cannot matter, confirm
the device reproduces the oracle exactly, then vary one corner at a time.

## Where the per-hit attribute work actually stands

Four candidate causes proposed, four eliminated by measurement:

- different hit geometry -- excluded and counted, 117 of 41,981, and removing
  them does not fix it
- sub-primitive hit points -- the two agree to `5.1e-05` units
- a defect in the attribute fetch -- the fetched corner is bit-identical
- the fetch infrastructure itself -- with it present and the interpolation
  unused, the contract passes at 41 mismatches

With the interpolation applied to both the reflection and the indirect paths
the interior comparison reports 200 mismatches against a bound of 41, while
the interpolated red agrees between the two sides to `1.2e-07`.

The residual is therefore small, real, and unnamed. The probe watches one
channel of one path; widening it to all three channels and separating the
specular term from the bounce is the next step, and it is a lane and a
contract run rather than a design change.

## The reflection path is exonerated

Widened to all three channels: the interpolated colour agrees between device
and reference to about `5e-08` per channel, the fetched corner is
bit-identical, the hit point agrees to `5.1e-05`, and the radiance clamp
mirrors exactly. With the interpolation applied the interior comparison still
reports 200 mismatches against a bound of 41.

Every part of the reflection path is accounted for. The remaining suspect is
the diffuse bounce, which casts eight rays per pixel and whose hits are
neither reported nor excluded, unlike the reflection's single ray. That is
inference rather than measurement -- the bounce has not been probed -- and it
is the next step.

It carries a design consequence: excluding a pixel when any of eight rays
diverges is a few per cent of the frame rather than a quarter of one per cent,
and whether that remains a comparison worth having is a judgement call, not a
measurement.

## The measured blocker

`divergent-bounces=9877` of 41,981 interior pixels: **23.5% have at least one
of their eight diffuse-bounce rays hitting different geometry on the device
than in the reference.** The reflection's single ray disagrees on 0.28%.

The same run passes at 41 mismatches, because with no attribute varying across
a surface a divergent ray reads the same flat per-object albedo and shades
identically. The disagreement is total and invisible until a per-hit attribute
is introduced.

Texture fetch at a hit, interpolated attributes and multi-sample reflections
are therefore not blocked by a defect in this renderer. They are blocked by
two intersectors sampling eight stochastic directions and not agreeing about a
quarter of the pixels. The three ways forward -- exclude a quarter of the
frame, compare the bounce statistically as a stochastic estimator, or have the
reference trace against the device's own structure -- are judgements about
what this contract is for.

## Exclusion ruled out

Excluding pixels whose bounce diverges removes `hdr-excluded=13689` pixels --
exactly the lit-pixel count. Every lit pixel in the frame has at least one of
its eight bounce rays hitting different geometry on the two sides. What
survives the exclusion is background, agreeing to `2e-05` because nothing was
traced there.

So a per-hit attribute cannot be verified by excluding the divergence, and it
cannot be verified by widening a count either. It needs either a statistical
comparison of the bounce -- which `contract.indirect_accumulation` already
demonstrates for histories -- or a reference that traces against the device's
own acceleration structure. The reflection half of the path is measured clean
to `5e-08` and is not the obstacle.

## The tile comparison is not sensitive enough, and why

A 16x16 tile-mean comparison was built and measured. It sees through the
sampling noise exactly as intended -- 7 of 192 tiles mismatch with and without
a per-hit attribute, and the max tile error moves by 0.4% -- but multiplying
the reflection's hit albedo by 1.15 also leaves it unchanged and passing.

The ray-traced terms are about 2% of the frame. A 15% error inside one is 0.3%
frame-wide, under any tile bound loose enough to admit eight-ray sampling
noise. The per-pixel count at 1/1000 did catch it; loosening that count to
admit the noise removed the only sensitive bound.

The comparison must therefore be over the **isolated term**. The oracle already
renders `unreflectedHdr` and `unindirectHdr`, and the device can now render
with `EnvironmentReflectionDisabled` or `EnvironmentShadowsDisabled` set, so
both sides can produce a with-and-without pair. Differencing gives the term
alone, where a 15% error is 15%, and tile means over that difference keep the
noise averaged while restoring the sensitivity.

## Resolved: per-hit attributes are landed and verified

The section above proposed differencing the with-and-without pair and taking
tile means over it. That was necessary and not sufficient: with 16x16 tiles the
whole reflection term peaks at 0.114 while the part a geometry hit contributes
peaks at 0.0022, so a 1.15x hit albedo still moved a tile mean by only 3e-4 and
still passed. Geometry hits here are 200 pixels in 786432 -- sparse and strong,
and a 256-pixel mean spreads each one out until it is gone.

The mean is taken over the hit pixels instead. Both sides report what their ray
found in the reactive plane's spare lanes, so that is a set they agree on
rather than a guess, and on it the same error is 15%.

The interpolated vertex colour is landed. Three mutations fail and the clean
build passes, with a full build each time so the SPIR-V is re-embedded:

| mutation | signed mean | magnitude-weighted mean |
| --- | --- | --- |
| none | 0.41% | 2.90% |
| hit albedo x1.15 | 15.4% | 15.0% |
| barycentric weights permuted | 0.80% | 8.35% |
| vertex colour read one float over | 23.9% | 15.5% |

Both statistics are gated at 5% because they fail to different things: the
signed mean is nearly blind to a permutation, which leaves the average where it
was.

### The bounce was the blocker, and it is compared as a term

Measured, not inferred: of the 200 interior mismatches the interpolation
produced, **200** were pixels whose diffuse bounce hit different geometry on
the two sides, and of the 659 HDR mismatches, **659**. The reflection's single
deterministic ray disagreed on none.

Excluding those pixels was tried and is confirmed vacuous -- it removes
`hdr-excluded=13689`, exactly the lit-pixel count, leaving background that
agrees to 2.2e-05 because nothing was traced there. So the frame is split by
what kind of quantity each part is:

- the deterministic frame, compared per pixel against a new shadowed
  no-bounce reference, all 49152 pixels, interior agreeing to 0.0092 with zero
  mismatches;
- the diffuse bounce, differenced out of both sides and compared as tile
  means, 78 carrying tiles peaking at 0.142 and agreeing to 0.0028.

Nothing is excluded and no bound is loosened. This is the "statistical
comparison of the bounce" that the sections above named as one of the three
ways forward.

### Still not measured

Hit *position* is only coarsely observed. A 1000-unit displacement is caught
(417%) and a 1.5x distance error is caught (36.8%), but a 1.05x distance error
is bit-identical: fog begins at 2.0, the reflected hits sit below it, so
`vfFog` returns exactly zero either side of a small displacement and the point
lamp contributes negligibly there. Gross attribute errors are caught; a
few-per-cent error in `t` is not. What would settle it is a reflected hit
placed beyond the fog near distance.

Texture fetch at a hit and more than one sample per pixel are no longer
blocked: `vfHitTexCoord` exists alongside `vfHitVertexColor`, and the
instrument that verifies one verifies the other.
