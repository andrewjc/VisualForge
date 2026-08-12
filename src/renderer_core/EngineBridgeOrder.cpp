#include "renderer_core/EngineBridgeOrder.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace vf::renderer::bridge {

CompositionLedger ClassifyLayers(
    const std::span<const LayerEntry> layers) noexcept
{
    CompositionLedger ledger{};
    try {
        for (const auto& layer : layers) {
            switch (layer.ownership) {
            case DrawOwnership::Owned:
                ledger.owned.push_back(layer.id);
                break;
            case DrawOwnership::Retained:
                ledger.retained.push_back(layer.id);
                break;
            case DrawOwnership::Excluded:
                ledger.excluded.push_back(layer.id);
                break;
            case DrawOwnership::Unclassified:
                // A draw nobody owns lands somewhere in the composition by
                // accident, and the accident reproduces only on a machine
                // that has that middleware installed.
                ++ledger.unclassified;
                break;
            }
        }
    } catch (const std::bad_alloc&) {
        ++ledger.unclassified;
    }
    return ledger;
}

BridgeError ValidateComposition(
    const std::span<const LayerEntry> layers) noexcept
{
    // The enumerator order is the composition order. Checking it rather than
    // trusting the caller is what stops an overlay ending up underneath the
    // menu it is meant to sit over.
    std::uint32_t previous = 0;
    auto first = true;
    for (const auto& layer : layers) {
        const auto current = static_cast<std::uint32_t>(layer.id);
        if (current >= static_cast<std::uint32_t>(LayerId::Count)) {
            return BridgeError::UnknownLayer;
        }
        if (!first && current <= previous) return BridgeError::OrderViolation;
        previous = current;
        first = false;
    }
    return BridgeError::None;
}

TargetSelection SelectHandoffTarget() noexcept
{
    // Always pre-UI. The post-UI target would put the world on top of the
    // menu, which is an easy mistake to make by binding whichever target
    // happens to be current at the hook.
    return TargetSelection::PreUi;
}

BridgeError ValidateRetained(const RetainedContract& contract) noexcept
{
    if (static_cast<std::uint32_t>(contract.layer) >=
        static_cast<std::uint32_t>(LayerId::Count)) {
        return BridgeError::UnknownLayer;
    }
    // A retained path with no named owner is indistinguishable from something
    // forgotten, and forgotten paths are found by users rather than by tests.
    if (contract.ownerId == 0) return BridgeError::UnclassifiedDraw;
    return BridgeError::None;
}

std::array<float, 4> PrepareForComposition(
    const std::array<float, 4>& source,
    const bool sourceIsPremultiplied) noexcept
{
    const auto alpha = std::clamp(source[3], 0.0f, 1.0f);
    if (sourceIsPremultiplied) return {source[0], source[1], source[2], alpha};
    // Converted rather than assumed compatible. Compositing a straight-alpha
    // source as premultiplied darkens every edge of every element drawn over
    // it, which reads as a user-interface problem and gets investigated in
    // the wrong place.
    return {source[0] * alpha, source[1] * alpha, source[2] * alpha, alpha};
}

BridgeError ValidateColorSpace(
    const ColorSpace produced,
    const ColorSpace expected) noexcept
{
    // A mismatch silently accepted shifts every colour in the frame by an
    // amount that looks like a grading change rather than a conversion bug.
    return produced == expected
        ? BridgeError::None : BridgeError::ColorSpaceMismatch;
}

BridgeError ComputeLetterbox(
    const std::uint32_t worldWidth,
    const std::uint32_t worldHeight,
    const std::uint32_t targetWidth,
    const std::uint32_t targetHeight,
    Viewport& viewport) noexcept
{
    viewport = {};
    if (worldWidth == 0 || worldHeight == 0 || targetWidth == 0 ||
        targetHeight == 0) {
        return BridgeError::InvalidExtent;
    }
    // Fit inside, preserving aspect. Stretching would change the field of
    // view without anything saying so, and the change is small enough to be
    // mistaken for a camera setting.
    const auto worldAspect =
        static_cast<double>(worldWidth) / static_cast<double>(worldHeight);
    const auto targetAspect =
        static_cast<double>(targetWidth) / static_cast<double>(targetHeight);

    if (worldAspect > targetAspect) {
        // Wider than the target: the width fills and the height centres.
        viewport.width = targetWidth;
        viewport.height = static_cast<std::uint32_t>(
            std::lround(static_cast<double>(targetWidth) / worldAspect));
        viewport.height = std::min(viewport.height, targetHeight);
    } else {
        viewport.height = targetHeight;
        viewport.width = static_cast<std::uint32_t>(
            std::lround(static_cast<double>(targetHeight) * worldAspect));
        viewport.width = std::min(viewport.width, targetWidth);
    }
    viewport.x = (targetWidth - viewport.width) / 2;
    viewport.y = (targetHeight - viewport.height) / 2;
    return BridgeError::None;
}

bool WorldIsVisible(const std::span<const LayerEntry> layers) noexcept
{
    // A video layer replaces the world while it plays. Compositing underneath
    // it wastes the frame and compositing over it hides the video; neither is
    // a blend, so the world simply does not appear.
    for (const auto& layer : layers) {
        if (layer.id == LayerId::Bink && layer.present) return false;
    }
    return true;
}

bool ShouldCompositeWorld(
    const BridgeTicket& ticket,
    const bool vulkanFrameProduced,
    const std::span<const LayerEntry> layers) noexcept
{
    // No ticket, no frame, or a video playing: a complete vanilla frame,
    // never a partial one. Half a world over a menu is worse than no world at
    // all, and it is the state that gets reported as a crash.
    if (!ticket.acquired) return false;
    // A frame Vulkan never produced would composite a stale image, which is
    // worse than compositing none because it looks current.
    if (!vulkanFrameProduced) return false;
    return WorldIsVisible(layers);
}

const char* ToString(const BridgeError error) noexcept
{
    switch (error) {
    case BridgeError::None: return "none";
    case BridgeError::UnknownLayer: return "unknown layer";
    case BridgeError::OrderViolation: return "order violation";
    case BridgeError::UnclassifiedDraw: return "unclassified draw";
    case BridgeError::InvalidExtent: return "invalid extent";
    case BridgeError::TicketUnavailable: return "ticket unavailable";
    case BridgeError::ColorSpaceMismatch: return "colour space mismatch";
    }
    return "unknown";
}

const char* ToString(const LayerId layer) noexcept
{
    switch (layer) {
    case LayerId::VulkanWorld: return "vulkan world";
    case LayerId::Bink: return "bink";
    case LayerId::Scaleform: return "scaleform";
    case LayerId::RetainedMiddleware: return "retained middleware";
    case LayerId::ImGui: return "imgui";
    case LayerId::ExternalOverlay: return "external overlay";
    case LayerId::Count: return "count";
    }
    return "unknown";
}

}
