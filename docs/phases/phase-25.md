# Phase 25 — Classified world-only suppression and fail-open recovery

Status: CPU contract complete; world suppression implemented and live-verified;
`TakeoverController` wiring and the world-capture matrix pending

## Implemented slice

`EngineTakeover` (`src/renderer_core/EngineTakeover.h/.cpp`) and
`EngineSuppression` (`src/renderer_core/EngineSuppression.h/.cpp`), with the
live half in `src/EngineWorldSuppression.{h,cpp}` and the draw classification
in `EngineDrawCapture`.

- **Thirteen arming predicates, each denying on its own and *accumulating*
  rather than short-circuiting**, so one investigation names every reason
  instead of costing one rebuild per reason.
- **The decision is an immutable `TakeoverPermit` carrying its evidence and
  expiring with its own frame.** A second `BeginFrame` in the same frame
  returns the first answer whatever the evidence says by then: two consumers
  disagreeing about whether the frame was taken over is exactly how a frame
  gets drawn half by each renderer.
- **Fault injection at all five frame phases by six fault kinds yields a whole
  frame every time.** Before suppression, vanilla finishes the frame; after it,
  those draws are gone and the engine has walked past them, so the frame is
  completed by the last completed Vulkan output, or by holding what the display
  already had when none exists.
- **A fault latches out of takeover**, and a recovery frame is spent only on a
  frame that was itself quiet, so a continuously broken backend cannot re-arm
  on schedule and alternate.
- **Lifecycle events invalidate the permit immediately**, not at the next frame
  boundary, and drop the last-good frame with it.
- **`PresentationIsWhole(worldSuppressed, vulkanPresented)`** is the rule that
  a suppressed world must be a presented one.

## Live suppression

`VISUALFORGE_SUPPRESS_WORLD` is default-on and is exercised by every live
capture through `Invoke-LiveCapture.ps1 -SuppressWorldDraws`. With it the
engine's own world draws are classified and dropped, which is what makes the
mirror's frame cost measurable rather than measured alongside a second
renderer drawing the same scene.

## Deferred within this phase

- **`TakeoverController` is not wired into the plugin.** Suppression runs in
  measurement mode; the permit machinery is tested and unreferenced by the
  live path, so the arming predicates are not what decides whether a live
  frame suppresses.
- **`WorldCaptureMatrix` is not populated from a live run.** It makes the gate
  machine-checkable — observed is not covered, and a class with an unknown
  world-target writer or a missing visible class stays outstanding and named —
  but nothing yet fills it from captured draws.

## Promotion decision

Suppression is live and load-bearing for measurement. Authoritative takeover is
not: that needs the controller wired and the capture matrix satisfied.
