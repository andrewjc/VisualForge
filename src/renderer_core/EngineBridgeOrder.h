#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::bridge {

// The layers that compose after the Vulkan world enters D3D11. The order here
// is the documented composition order, and it is not a preference: Scaleform
// expects to draw over a finished world, a video layer replaces the world
// entirely while it plays, and an external overlay must sit above everything
// or it cannot be read.
enum class LayerId : std::uint32_t
{
    // The Vulkan world and post output, handed over at the pre-Scaleform
    // boundary. Everything below composites on top of it.
    VulkanWorld = 0,
    Bink = 1,
    Scaleform = 2,
    RetainedMiddleware = 3,
    ImGui = 4,
    ExternalOverlay = 5,
    Count = 6,
};

// What this renderer does about a D3D draw issued after the handoff. Every
// one must be classified: an unclassified draw is a draw nobody owns, and it
// lands somewhere in the composition by accident.
enum class DrawOwnership : std::uint8_t
{
    // Composited by this renderer in a known layer.
    Owned = 0,
    // Left to vanilla deliberately, with a named owner and a resource
    // contract, so the decision is visible rather than looking like a gap.
    Retained = 1,
    // Recognised and explicitly out of scope.
    Excluded = 2,
    // Not recognised. This prevents arming: a draw nobody owns lands
    // somewhere in the composition by accident, and the accident is
    // reproducible only on the machine that has that middleware installed.
    Unclassified = 3,
};

enum class BridgeError : std::uint8_t
{
    None,
    UnknownLayer,
    OrderViolation,
    UnclassifiedDraw,
    InvalidExtent,
    TicketUnavailable,
    ColorSpaceMismatch,
};

// Which target the handoff writes into. Selecting the post-UI target would
// put the world on top of the menu, which is not a subtle failure but is a
// very easy one to introduce by picking whichever target is bound.
enum class TargetSelection : std::uint8_t
{
    PreUi = 0,
    PostUi = 1,
};

enum class ColorSpace : std::uint8_t
{
    Srgb = 0,
    LinearScRgb = 1,
    Rec2020Pq = 2,
};

// A retained layer's owner and contract. A retained path with no named owner
// is indistinguishable from something forgotten.
struct RetainedContract
{
    LayerId layer{};
    std::uint64_t ownerId{};
    bool ownsTarget{};
    bool ownsDepth{};
};

struct LayerEntry
{
    LayerId id{};
    DrawOwnership ownership{DrawOwnership::Owned};
    bool present{true};
};

struct CompositionLedger
{
    std::vector<LayerId> owned;
    std::vector<LayerId> retained;
    std::vector<LayerId> excluded;
    std::uint32_t unclassified{};

    [[nodiscard]] bool MayArm() const noexcept
    {
        return unclassified == 0;
    }
};

// A bridge ticket is permission to write into the engine's target for one
// frame. Without one, nothing is written and the frame stays vanilla; there
// is no partial state where half the world arrived.
struct BridgeTicket
{
    std::uint64_t frameIndex{};
    bool acquired{};
};

struct Viewport
{
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

[[nodiscard]] CompositionLedger ClassifyLayers(
    std::span<const LayerEntry> layers) noexcept;

// The composition order, checked rather than assumed.
[[nodiscard]] BridgeError ValidateComposition(
    std::span<const LayerEntry> layers) noexcept;

// Where the world is handed over. Always the pre-UI target: the post-UI one
// would put the world on top of the menu.
[[nodiscard]] TargetSelection SelectHandoffTarget() noexcept;

// A retained path must name an owner. One without is indistinguishable from
// something forgotten, and forgotten paths are found by users rather than by
// tests.
[[nodiscard]] BridgeError ValidateRetained(
    const RetainedContract& contract) noexcept;

// Vulkan output is premultiplied. Compositing it as straight alpha darkens
// every edge of every UI element drawn over it, which reads as a UI problem
// rather than as a handoff problem.
[[nodiscard]] std::array<float, 4> PrepareForComposition(
    const std::array<float, 4>& source,
    bool sourceIsPremultiplied) noexcept;

// The conversion between what Vulkan produced and what the engine's target
// expects. A mismatch that is silently accepted shifts every colour in the
// frame by an amount that looks like a grading change.
[[nodiscard]] BridgeError ValidateColorSpace(
    ColorSpace produced,
    ColorSpace expected) noexcept;

// The letterboxed region for a world image inside a target of a different
// aspect. Stretching instead would change the field of view without anything
// saying so.
[[nodiscard]] BridgeError ComputeLetterbox(
    std::uint32_t worldWidth,
    std::uint32_t worldHeight,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight,
    Viewport& viewport) noexcept;

// Whether the world should be composited at all this frame. A video layer
// replaces the world while it plays: compositing underneath it wastes the
// frame, and compositing over it hides the video.
[[nodiscard]] bool WorldIsVisible(
    std::span<const LayerEntry> layers) noexcept;

// A skipped Vulkan frame, or a ticket that could not be acquired, yields a
// complete vanilla frame. Never a partial one: half a world over a menu is
// worse than no world at all, and it is the state that gets reported as a
// crash.
[[nodiscard]] bool ShouldCompositeWorld(
    const BridgeTicket& ticket,
    bool vulkanFrameProduced,
    std::span<const LayerEntry> layers) noexcept;

[[nodiscard]] const char* ToString(BridgeError error) noexcept;
[[nodiscard]] const char* ToString(LayerId layer) noexcept;

}
