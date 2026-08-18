# Phase 27 — Native Vulkan WSI and compatibility-island retirement

Status: CPU contract complete; native WSI implementation and live promotion
pending

## Implemented slice

`EnginePresentation` (`src/renderer_core/EnginePresentation.h/.cpp`).

- **Surface selection is deterministic, not first-available.** First-available
  differs by driver, so the same build produces a different colour on two
  machines and reproduces on neither.
- **HDR requires a capable display *and* a ten-bit-or-wider format.** Eight bits
  stretched across a PQ curve bands every dark gradient and reads as a
  tone-mapping fault rather than as a format choice. It falls back to a whole
  SDR surface rather than to a bad HDR one.
- **FIFO is the only honest fallback**, because the specification guarantees it
  and nothing else.
- **Swapchain recreation is driven by the reported result**, not by a polled
  window size.

## Deferred within this phase

- **No native surface.** Nothing creates a `VkSurfaceKHR` against the game's
  window; presentation still goes through the D3D11 bridge, which is the
  compatibility island this phase exists to retire.
- **The island is not retired.** Retiring it depends on phases 24 and 26: the
  engine's UI must be composed over a Vulkan-owned frame first.
- **No live promotion.** This is the last phase in the plan and is gated by
  every stage before it.

## Promotion decision

Not promoted, and deliberately last. The plan marks native presentation
optional: the renderer is complete without it, and taking the window is the
change with the least room to fail safely.
