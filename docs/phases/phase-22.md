# Phase 22 — Water, glass, refraction, and underwater views

Status: CPU contract complete; GPU water/glass pass and live capture gate
pending

## Implemented slice

`EngineWater` (`src/renderer_core/EngineWater.h/.cpp`, namespace
`vf::renderer::water`), mirrored by `shaders/phase22/water.glsl`.

- **Three scrolling normal layers summed as gradients**, not as normals. Summed
  as normals they fight: each pulls the surface toward its own orientation and
  the result is flatter than any one of them. Summed as gradients they combine
  into one surface, which is what a real sum of ripples is.
- **Depth-blended shallow/deep/silt colour that never continues past what was
  authored.** Extrapolating past the authored depth produces colours the artist
  never chose, and it happens exactly where the water is deepest and most
  visible.
- **A reflection plane where a point on it is its own mirror.** The identity is
  the test: a plane that fails it reflects everything to the wrong side by an
  amount that looks like a height offset.
- **An underwater state that reports the crossing** rather than leaving a
  consumer to detect it by comparing camera height to water height, which is
  how two consumers end up disagreeing about whether the camera is submerged.
- **Beer-Lambert fog that approaches the fog colour without overshooting.**

## Deferred within this phase

- **No GPU water pass.** The rules are implemented and mirrored in GLSL, and
  the shader is compiled, but no contract renders a water surface on the
  device and compares it against the oracle. The phase's evidence is the CPU
  contract alone.
- **No planar reflection render.** The reflection plane is computed; nothing
  renders the scene through it into a texture.
- **No live capture.** Water height, the three normal layers, and the shallow
  and deep colours are fixture values, not read from the engine.

## Promotion decision

Not promoted.
