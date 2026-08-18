# Phase 21 — Decals, sorted transparency, and particles

Status: CPU contract complete; GPU composite pass verified against a
suppressed-composite baseline; decal projection and live capture gate pending

## Implemented slice

`EngineTransparency` (`src/renderer_core/EngineTransparency.h/.cpp`, namespace
`vf::renderer::blend`), mirrored by `shaders/phase21/transparency.glsl`, with
the composite recorded by `VulkanRasterRenderer`.

- **Four blend modes kept distinct.** Straight, premultiplied, additive and
  multiply each have their own factors. Collapsing premultiplied into straight
  is the classic error and it looks like a slightly wrong alpha rather than
  like the wrong equation.
- **A sort key that is layered *and* total.** Two draws at one depth cannot
  swap between frames, because a key that ties leaves the order to whatever
  the sort happened to do with equal elements — which is stable in one build
  and not in the next.
- **Blended geometry tests depth and refuses to write it.** Writing depth from
  a transparent surface hides everything behind it, and the symptom is missing
  geometry rather than wrong blending.
- **Soft-particle fade in view space**, dissolve with a *width* rather than a
  bare threshold, and a reactive mask that accounts for the additive branch by
  taking the larger of the draw's alpha and its luminance.
- **Refraction reads the target as it stood before any refractive draw.** A
  refractive surface that samples the live target sees whichever refractive
  surfaces were drawn before it, so two panes of glass show each other and the
  image depends on draw order rather than on depth.

## The composite is isolated against a suppressed-composite baseline

The additive layer is measured against the same packet rendered with
`abi::RasterFrameSuppressTransparentComposite` and nothing else changed.

The previous baseline removed the transparent *table* instead. That is not the
same frame: a blended draw is excluded from the acceleration structure exactly
because the table names it, so the baseline reflected a quad the render it was
compared against had excluded. The additive layer then appeared to darken 212
pixels it never touched. It stayed hidden for as long as ray-traced
reflections shaded black whatever they hit, and surfaced the moment they
carried a real albedo.

The flag rides on the submitted copy alone. The additive, decal and reordered
renders all start from the shared baseline request, and each exists precisely
to composite something — inheriting it made every one of them draw no layer.

## Deferred within this phase

Stated gaps, not silent ones:

- **Decals are composited but not projected.** `TransparentDrawRecordV1`
  carries the projection volume, and the contract measures `decal-covered` and
  `decal-clipped`, but the shader does not yet project a decal onto the
  surface it reaches. `vfProjectDecal` was written and removed because it had
  no caller; the projection now exists in the packet, so the caller is what is
  missing rather than the data.
- **Particles are not a separate path.** They composite through the same
  blended draws as everything else, with no simulation, no sorting by system,
  and no GPU particle buffer.
- **No live capture.** The transparent table is a fixture. Which engine draws
  are blended, in what order, and under which blend mode is not yet read from
  Fallout 4.

## Promotion decision

Not promoted. The composite is verified against a device baseline, which is
what the previous cycle could not do; the projection and the live capture are
both outstanding.
