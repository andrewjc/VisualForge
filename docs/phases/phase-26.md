# Phase 26 — World-and-post suppression ledger and acceptance matrix

Status: CPU contract complete; GPU post ownership and live promotion pending

## Implemented slice

`EngineImageSpace` (`src/renderer_core/EngineImageSpace.h/.cpp`).

- **The ledger is complete and versioned, and completeness is checked before
  unknowns.** An effect nobody listed is an effect nobody decided about, and a
  partial ledger reports zero unknowns precisely because the unmapped ones were
  never enumerated — a zero that means the opposite of what it looks like.
- **Duplicates are refused.** That is how a ledger reaches the right count with
  the wrong contents.
- **Retained effects must name an owner**, and a newer ledger version is
  refused rather than silently accepted, because it describes frames this build
  cannot be compared against.
- **Borrowed targets are returned in the frame that took them.** The engine
  reuses them next frame, so a late return releases a surface something else
  has already started drawing into. An outstanding borrow **blocks the next
  frame instead of being tidied away**, because tidying hides a leak that only
  appears as corruption under load.
- **Depth handoff checks the reversed-Z convention**, not just format and
  extent. Depth written reversed and read as standard makes fog, depth of field
  and decals all wrong by an amount that reads as a bias setting.
- **Exposure is adopted from vanilla across the handoff** rather than restarted
  at a default, and history is invalidated by extent, ledger version, upscale
  ratio or takeover epoch.
- **A capture takes the final composite**, not the pre-UI world, and not before
  the composite completes.
- **Residual D3D is whitelisted by named operation, never by category** — a
  world draw and a UI draw are the same category of call.

## Deferred within this phase

- **No GPU post ownership.** The Vulkan renderer does not yet own any
  image-space effect the engine would otherwise run, so the ledger describes a
  handoff that has not happened.
- **The ledger is not populated from Fallout 4.** Which image-space effects the
  engine actually enables is a phase 23 live-capture question and is unanswered.
- **No acceptance matrix from a live run.**

## Promotion decision

Not promoted. This phase depends on phase 23's live capture and phase 24's
handoff ordering, neither of which is complete.
