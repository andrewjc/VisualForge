# Phase 20 — Diffuse GI, temporal reconstruction, and denoising

Status: CPU contract and one-bounce GPU vertical complete; temporal
reconstruction and spatial filtering outstanding; live capture gate pending

## Implemented slice

`EngineIndirect` (`src/renderer_core/EngineIndirect.h/.cpp`, namespace
`vf::renderer::gi`), mirrored branch for branch by
`shaders/phase20/indirect.glsl`.

- **Cosine-weighted hemisphere sampling by Malley's method.** The diffuse
  integral already carries a cosine; sampling uniformly and multiplying it in
  spends most rays near the horizon where they contribute least. The test
  checks the *mean cosine* is 2/3 rather than merely that directions point
  the right way — a uniform hemisphere averages 1/2, so the check
  distinguishes the two distributions instead of both passing.
- **Indirect excludes direct, and never goes negative.** Counting the same
  light twice makes every interior bloom, and the error grows with how well
  lit the room is, so it reads as a lighting bug rather than as double
  counting. A direct term larger than the total is two measurements
  disagreeing, not a negative light.
- **Fireflies are clamped, not discarded.** Discarding biases the mean
  darker; clamping only bounds the variance. A non-finite sample is a failed
  path, not an infinitely bright one — carrying a NaN into an accumulator
  poisons that pixel permanently.
- **No captured lighting means nothing to bounce.** Phase 17 established that
  such a frame leaves the albedo alone, and a bounce that returned the hit
  surface's albedo would invent light the capture never saw. This was found
  by the unlit fixture failing at 0.5 error once the shader started tracing.
- **Indoors with no geometry resolves to nothing.** The exterior sky must not
  stand in for an unknown bounce; that substitution is the same light leak
  the reflection miss rule prevents.
- **A bounce surface is shadowed like any other.** Skipping it would bounce
  light off a wall the sun cannot reach, brightening a room from a surface
  that is in shadow.

## Temporal reconstruction

- **Rejection names its reason.** "The camera cut" and "this pixel is newly
  visible" need different responses and are indistinguishable in a boolean.
  The epoch is tested first, because a cut invalidates every pixel at once
  whatever the surfaces say.
- **Depth is compared relatively.** At a thousand units a pixel's depth
  changes by several units between frames without the surface moving, and a
  fixed epsilon rejects every distant pixel forever. The test pins this by
  accepting the same *relative* change at ten times the distance.
- **Object and material are separate tests.** Two objects can share a
  material, and one object can change material without moving, so a single
  identity check misses one case or the other. Both are policy and can be
  relaxed together without touching the geometric checks.
- **A rejected sample resets rather than blends.** Blending it is exactly the
  trail the gate forbids: the previous scene stays visible, fading, for as
  long as the history is allowed to be.
- **Mean and second moment are both accumulated**, because variance is what a
  denoiser needs and cannot be recovered from the mean alone. A history that
  reported zero variance for noisy input would let a filter stop filtering
  exactly where it is needed.
- **Half-resolution mapping rounds up.** Rounding down leaves the last column
  and row reading a sample that was never traced, which appears as a hard
  edge along two sides of the frame.

## Correctness apart from quality

`IndirectRules` holds everything that changes the answer; `QualityPreset`
holds everything that changes only how long it takes to get there. The plan
requires a preset change not to alter what is captured or how a surface is
identified, and the split is what makes that checkable: the history-length
test asserts that a longer history converges to the *same* value.

## Fixture

`vf_packet_replay --render-family-scene --indirect`. The reference and the
shader trace the **same eight rays per pixel** walking the **same sequence**,
so the two integrate one set of directions rather than two estimates of one
integral.

The bounce is isolated by a fourth reference render — reflections on,
indirect off — differenced against the full one, exactly as the unshadowed
pass isolates the shadow term. Measuring the two ray-traced terms together
would let either do nothing unnoticed.

| Metric | Value |
| --- | ---: |
| Pixels the bounce changes | 13,689 |
| Largest bounce change | 0.153 |
| Shadow-interior pixels compared | 41,981 |
| Shadow-interior mismatches | 12 (bound 41) |
| Validation errors | 0 |

**The interior bound is on how many pixels may disagree, not how far one may
move.** Indirect light is a stochastic estimator, and a ray-triangle hit
decision at an edge is not bit-identical between this oracle's
Möller-Trumbore and the hardware intersector. With eight rays a single
flipped ray moves a pixel by an eighth of the radiance behind it. That is the
same rule the Phase 11 silhouette comparison uses and for the same reason;
without ray-traced terms the interior is still required to agree exactly.

## Mutation evidence

| Mutation | Result |
| --- | --- |
| Uniform hemisphere instead of cosine-weighted | `P20_diffuse_sampling_is_cosine_weighted_and_deterministic` fails the mean-cosine check |
| Rejected samples blend instead of reset | `P20_accumulation_converges_and_resets_without_trailing` fails, 3 assertions |
| GPU drops the indirect term | `contract.indirect_scene_frame` fails with 9,877 interior mismatches against a bound of 41 |

## Artifacts

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `artifacts/phase-20/indirect-debug.ppm` | 147,471 | `54E00A85E573CCD843279BC1AE28962C3BD7DC73D8B7259D7D15CDDAFC5E74E2` |
| `artifacts/phase-20/indirect-release.ppm` | 147,471 | `54E00A85E573CCD843279BC1AE28962C3BD7DC73D8B7259D7D15CDDAFC5E74E2` |
| `artifacts/phase-20/indirect-debug.vfgbuf` | 3,145,728 | `C45B999A900125AA6936CE88269659D035127ACFD83B9597CF7B23112AD9F9D9` |
| `artifacts/phase-20/indirect-release.vfgbuf` | 3,145,728 | `C45B999A900125AA6936CE88269659D035127ACFD83B9597CF7B23112AD9F9D9` |

Debug and Release are byte-identical. 286/286 tests pass in both.

The Phase 19 and Phase 20 colour artifacts are **the same image**, because
indirect light is traced for every lit surface the device can trace for, not
only in this fixture. The two differ in what they *measure*, not in what they
draw.

**This phase re-baselined the Phase 17, 18, and 19 colour artifacts.** Adding
a term to every lit surface changes every lit frame. Their contracts still
pass; the G-buffer artifacts are untouched, because the G-buffer carries
albedo and normals rather than radiance.

| Artifact | New SHA-256 |
| --- | --- |
| `artifacts/phase-17/lit-debug.ppm` | `4B718DF68BFB7B3125E83C0ED8D9A55DD85143A5ACB8193323AA5FF28C57E280` |
| `artifacts/phase-18/shadowed-debug.ppm` | `74C5F25C7BF52CC7A459E7E0FD03635945E2C14A2878B499D347FC9C899C146B` |
| `artifacts/phase-19/reflected-debug.ppm` | `54E00A85E573CCD843279BC1AE28962C3BD7DC73D8B7259D7D15CDDAFC5E74E2` |

## Not in this phase

Named rather than implied by omission:

- **No history resources on the GPU.** `Reproject`, `Accumulate`, `Variance`
  and the epoch rules are implemented and tested, but nothing stores a
  per-pixel history yet, so the shader traces eight rays fresh every frame.
  The plan's convergence and ghosting gate cannot be claimed until it does.
- **No spatial filtering.** The accumulated variance exists to drive one and
  currently drives nothing.
- **Emissive importance sampling stays disabled**, as the plan requires,
  until calibrated area and radiance data passes its own tests.
- **No motion vectors captured.** `Reproject` takes them as input and is
  tested against synthetic ones; the engine's own motion vectors are not
  captured yet.
- **Half resolution is implemented and tested but not wired**: the shader
  traces at full resolution.
