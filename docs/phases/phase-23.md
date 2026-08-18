# Phase 23 — Vulkan image-space and post chain

Status: CPU contract complete; GPU post chain and live capture gate pending

## Implemented slice

`EnginePostChain` (`src/renderer_core/EnginePostChain.h/.cpp`, namespace
`vf::renderer::post`), with `shaders/phase23/post.glsl` carrying the shared
rules.

- **An effect ledger where an *unknown* effect prevents arming and an
  explicitly unsupported one does not.** The distinction is the whole point: an
  effect nobody has classified is an effect nobody has decided about, and
  arming past it means rendering a frame that silently lost something.
- **A chain order that is checked rather than assumed**, and that a disabled
  effect still holds its place in. Removing a disabled effect from the order
  makes the order depend on the settings, so two machines with different
  settings run different chains and neither is wrong by its own check.
- **Transient aliasing that refuses touching lifetimes** and never aliases a
  borrowed image. Two effects whose lifetimes touch at a single point are the
  case that looks safe and is not.
- **Borrowed images may be read and never written.**
- **Asymmetric exposure adaptation that resets outright on a cut.** Adapting
  across a cut is what makes a scene change look like a camera iris.
- **A bloom knee**, and **Halton jitter that restarts on resize** so the
  sequence cannot carry a sub-pixel offset computed for a different extent.

## Deferred within this phase

- **No GPU post chain.** Tone mapping and bloom are rendered and compared
  against the oracle by the phase 16 family contract, but the chain itself —
  ordering, aliasing, borrowing, exposure adaptation — has no device pass.
  The CPU contract is the phase's only evidence for those.
- **No motion vectors.** A stated motion model exists; nothing produces a
  motion vector image, so temporal effects that would consume one cannot be
  exercised.
- **No live capture.** Which image-space effects Fallout 4 actually enables,
  in what order, is not yet read from the engine. Phase 26 owns the ledger
  that decides which of them the Vulkan renderer takes over.

## Promotion decision

Not promoted.
