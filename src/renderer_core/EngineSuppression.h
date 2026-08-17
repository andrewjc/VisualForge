#pragma once

#include <cstdint>

// The draw-time half of phase 25. `EngineTakeover` decides once per frame
// whether the frame may be taken over; this decides, for one draw, whether
// that decision applies to it.
//
// Separate from the controller because it runs inside the draw hook, on the
// engine's own thread, thousands of times a frame. It is a pure function of
// three booleans so that it can be tested exhaustively and so the hook does no
// work beyond reading them.
namespace vf::renderer::suppression {

enum class DrawDisposition : std::uint8_t
{
    // Forwarded to D3D unchanged. The default in every ambiguous case: a draw
    // that reaches vanilla costs performance, and a draw that does not reach
    // either renderer is missing from the picture.
    Vanilla = 0,
    // Dropped. Vulkan owns this draw for this frame.
    Suppressed = 1,
};

struct DrawContext
{
    // The frame's permit grants takeover, evaluated once at the frame boundary
    // and not re-derived here.
    bool permitGrants{};
    // This draw writes the world target -- in practice, the main scene depth
    // is bound. The interface does not, which is what makes the suppression
    // world-only rather than total.
    bool writesWorldTarget{};
    // Something is reproducing the world this frame. Without it, suppression
    // removes the world and puts nothing in its place.
    bool worldReproduced{};
};

[[nodiscard]] DrawDisposition ClassifyDraw(const DrawContext& context) noexcept;

// Whether a frame's presentation is whole, given whether its world draws were
// suppressed and whether Vulkan output reached the display. The same property
// `takeover::FrameOutcome::IsWholeFrame` states for the frame as a whole,
// restated here so the draw path can assert it without owning a controller.
[[nodiscard]] bool PresentationIsWhole(
    bool worldSuppressed,
    bool vulkanPresented) noexcept;

}
