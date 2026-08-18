# Phase 24 — Scaleform, Bink, Flex, overlay, and final bridge ordering

Status: CPU contract complete; GPU handoff and live capture gate pending

## Implemented slice

`EngineBridgeOrder` (`src/renderer_core/EngineBridgeOrder.h/.cpp`, namespace
`vf::renderer::bridgeorder`).

- **Every layer after the handoff is classified**, an *unclassified* draw
  prevents arming, and a retained layer must name an owner. A layer nobody owns
  is a layer nobody restores.
- **A checked composition order** that keeps the world underneath and an
  external overlay on top.
- **The handoff is always to the pre-UI target.** Handing off to the post-UI
  one puts the world over the menu, which is not a subtle failure.
- **Video replaces the world rather than blending with it.**
- **Premultiplied output is converted rather than assumed**, and a colour-space
  mismatch is refused rather than silently accepted.
- **Aspect-preserving letterboxing** for video whose aspect differs from the
  target's.

## What the live investigation established

The bridge itself is already GPU-only. The CPU round-trip that remains exists
because `VulkanInteropBridge` and `VulkanRasterRenderer` each call
`vkCreateDevice`, so the image the raster renderer produces is not on the
device the bridge shares from. Removing the round-trip means one Vulkan device
for the backend, and that is the prerequisite for the pre-Scaleform handoff
rather than a separate optimisation.

## Deferred within this phase

- **One Vulkan device for the backend.** Until the bridge and the raster
  renderer share a device, the handoff cannot be a GPU-side image share.
- **No Scaleform, Bink or Flex hooks.** The ordering contract is exercised
  with described layers, not with layers read from the engine.
- **No live handoff.** Nothing yet hands a Vulkan-rendered world to the
  engine's own UI composition.

## Promotion decision

Not promoted. This phase is a stage-6 prerequisite and gates the ordering that
phases 25 and 26 depend on.
