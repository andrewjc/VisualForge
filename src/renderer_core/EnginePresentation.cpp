#include "renderer_core/EnginePresentation.h"

#include <cmath>

namespace vf::renderer::wsi {

namespace {

// How much range a format can carry through the transfer function chosen for
// it. Ranked rather than compared for equality, so the selection can prefer
// without enumerating every pair.
[[nodiscard]] std::uint32_t FormatRank(const SurfaceFormat format) noexcept
{
    switch (format) {
    case SurfaceFormat::Rgba16Sfloat: return 4;
    case SurfaceFormat::Rgb10A2Unorm: return 3;
    case SurfaceFormat::Bgra8Srgb: return 2;
    case SurfaceFormat::Rgba8Srgb: return 1;
    case SurfaceFormat::Unknown: return 0;
    }
    return 0;
}

// Ten bits at minimum for an ST 2084 encoding. Eight bits stretched across a
// PQ curve bands visibly in every dark gradient, and the banding is read as a
// tone-mapping fault rather than as a format that cannot carry the range.
[[nodiscard]] bool CarriesWideRange(const SurfaceFormat format) noexcept
{
    return format == SurfaceFormat::Rgb10A2Unorm ||
        format == SurfaceFormat::Rgba16Sfloat;
}

[[nodiscard]] bool IsHdr(const ColorSpace colorSpace) noexcept
{
    return colorSpace == ColorSpace::Hdr10St2084 ||
        colorSpace == ColorSpace::ExtendedSrgbLinear;
}

[[nodiscard]] bool Supports(
    const std::span<const PresentMode> supported,
    const PresentMode mode) noexcept
{
    for (const auto candidate : supported) {
        if (candidate == mode) return true;
    }
    return false;
}

}

SurfaceSelection SelectSurface(
    const std::span<const SurfaceCandidate> candidates,
    const DisplayPolicy& policy) noexcept
{
    SurfaceSelection selection{};

    // Two passes with an explicit preference, never "the first one that
    // works". First-available differs by driver, so the same build produces a
    // different colour on two machines and reproduces on neither.
    const SurfaceCandidate* bestHdr = nullptr;
    const SurfaceCandidate* bestSdr = nullptr;
    auto sawHdrColorSpace = false;

    for (const auto& candidate : candidates) {
        if (candidate.format == SurfaceFormat::Unknown ||
            candidate.colorSpace == ColorSpace::Unknown) {
            continue;
        }
        if (IsHdr(candidate.colorSpace)) {
            sawHdrColorSpace = true;
            if (!CarriesWideRange(candidate.format)) continue;
            if (bestHdr == nullptr ||
                FormatRank(candidate.format) > FormatRank(bestHdr->format)) {
                bestHdr = &candidate;
            }
            continue;
        }
        if (bestSdr == nullptr ||
            FormatRank(candidate.format) > FormatRank(bestSdr->format)) {
            bestSdr = &candidate;
        }
    }

    if (policy.hdrRequested) {
        if (!policy.hdrCapable) {
            // Presenting a PQ signal to a panel that cannot show it washes the
            // whole image out in a way that looks like a gamma bug.
            selection.error = PresentationError::HdrWithoutCapableDisplay;
        } else if (bestHdr != nullptr) {
            selection.candidate = *bestHdr;
            selection.hdrActive = true;
            return selection;
        } else if (sawHdrColorSpace) {
            selection.error = PresentationError::HdrWithoutWideFormat;
        } else {
            selection.error = PresentationError::HdrWithoutCapableDisplay;
        }
    }

    if (bestSdr == nullptr) {
        // Nothing usable at all. Reported rather than falling through to a
        // zeroed candidate that would be created and then present garbage.
        selection.candidate = SurfaceCandidate{};
        if (selection.error == PresentationError::None) {
            selection.error = PresentationError::NoSuitableSurface;
        }
        return selection;
    }

    // A whole SDR surface rather than a bad HDR one. Half-configured HDR is
    // worse than no HDR: the picture is wrong and nothing says why.
    selection.candidate = *bestSdr;
    selection.hdrActive = false;
    return selection;
}

PresentModeSelection SelectPresentMode(
    const std::span<const PresentMode> supported,
    const PresentModeRequest& request) noexcept
{
    PresentModeSelection selection{};
    if (supported.empty()) {
        selection.error = PresentationError::NoSupportedPresentMode;
        return selection;
    }

    if (!request.vsync && Supports(supported, PresentMode::Immediate)) {
        selection.mode = PresentMode::Immediate;
        return selection;
    }

    if (request.vsync) {
        // Mailbox replaces the queued image rather than queueing another, so a
        // fast frame does not wait behind a stale one. That is vsync without
        // the latency vsync usually costs.
        if (Supports(supported, PresentMode::Mailbox)) {
            selection.mode = PresentMode::Mailbox;
            return selection;
        }
    }

    // Relaxed FIFO tears on a late frame. That is a different contract, not a
    // free upgrade, so it is taken only when the caller has said so.
    if (request.tolerateLateTearing &&
        Supports(supported, PresentMode::FifoRelaxed)) {
        selection.mode = PresentMode::FifoRelaxed;
        return selection;
    }

    if (Supports(supported, PresentMode::Fifo)) {
        selection.mode = PresentMode::Fifo;
        return selection;
    }

    // FIFO is required by the specification, so reaching here means the
    // reported set is not one this renderer should trust.
    selection.error = PresentationError::NoSupportedPresentMode;
    selection.mode = PresentMode::Fifo;
    return selection;
}

bool TransitionRequiresRecreate(
    const SurfaceState& previous,
    const SurfaceState& next) noexcept
{
    // Every field here changes what the images mean rather than how many of
    // them there are, and the display identity is included because images that
    // are the right size and format can still be for the wrong monitor.
    return previous.format != next.format ||
        previous.colorSpace != next.colorSpace ||
        previous.presentMode != next.presentMode ||
        previous.width != next.width ||
        previous.height != next.height ||
        previous.displayId != next.displayId;
}

SwapchainAction ResolveStatus(
    const SwapchainStatus status,
    const bool minimized) noexcept
{
    // Device loss is handled wherever the window is: it is not a property of
    // the surface, and deferring it while minimized leaves every later call
    // failing against a dead device.
    if (status == SwapchainStatus::DeviceLost) {
        return SwapchainAction::RecreateDevice;
    }
    // Otherwise minimized wins over every recreate. A zero-extent swap chain
    // is invalid, so a loop that keeps trying to build one spins for as long
    // as the window stays down, which is measured as the game hanging.
    if (minimized) return SwapchainAction::SkipFrame;

    switch (status) {
    case SwapchainStatus::Ok:
        return SwapchainAction::Present;
    case SwapchainStatus::Suboptimal:
        // Not an error. The image is presentable; rebuilding before showing it
        // drops a frame for a condition the specification calls acceptable,
        // and those dropped frames read as a stutter.
        return SwapchainAction::PresentThenRecreate;
    case SwapchainStatus::OutOfDate:
        return SwapchainAction::RecreateSwapchain;
    case SwapchainStatus::SurfaceLost:
        // A lost surface outlives the swap chain built on it, so rebuilding
        // only the chain fails again immediately and the loop spins.
        return SwapchainAction::RecreateSurface;
    case SwapchainStatus::DeviceLost:
        return SwapchainAction::RecreateDevice;
    case SwapchainStatus::Timeout:
        return SwapchainAction::SkipFrame;
    }
    return SwapchainAction::SkipFrame;
}

PacingDecision Pace(
    const PacingState& state,
    const double nowSeconds,
    const double targetFrameSeconds) noexcept
{
    PacingDecision decision{};
    decision.next = state;
    decision.next.established = true;
    decision.next.lastPresentSeconds = nowSeconds;

    if (!std::isfinite(nowSeconds)) {
        decision.next = state;
        return decision;
    }

    // Nothing established, or no target: present rather than waiting out a
    // budget measured from a frame that never happened.
    if (!state.established || !std::isfinite(targetFrameSeconds) ||
        targetFrameSeconds <= 0.0) {
        return decision;
    }

    const auto elapsed = nowSeconds - state.lastPresentSeconds;
    // Exponential, so one long frame does not move the estimate far enough to
    // pace the next several against it.
    decision.next.smoothedFrameSeconds = state.smoothedFrameSeconds > 0.0
        ? state.smoothedFrameSeconds * 0.9 + elapsed * 0.1
        : elapsed;

    const auto remaining = targetFrameSeconds - elapsed;
    if (remaining <= 0.0) {
        // Already late. No catch-up: pacing the next frames short to reclaim
        // the time converts one long frame into several uneven ones, and
        // judder is more visible than a single hitch.
        return decision;
    }

    // Yielded, never spun. A spin burns a core to hit a deadline the scheduler
    // would have hit anyway, and it is measured as the renderer being
    // expensive rather than as the pacing being crude.
    decision.sleepSeconds = remaining;
    return decision;
}

FullscreenPolicy SelectFullscreenPolicy(
    const bool exclusiveRequested,
    const bool overlaysPresent) noexcept
{
    // Opt-in, and it yields. Exclusive mode takes the display mode away from
    // everything else on the machine and blanks any overlay that is not a
    // display-driver one, which reaches the user as the game having broken
    // their capture software.
    if (exclusiveRequested && !overlaysPresent) {
        return FullscreenPolicy::ExclusiveOptIn;
    }
    return FullscreenPolicy::Borderless;
}

AuditResult AuditConsumers(
    const std::span<const ConsumerRecord> consumers) noexcept
{
    AuditResult result{};
    for (const auto& consumer : consumers) {
        // Unattributed first: a resolution attached to something nobody has
        // identified resolves nothing, because the thing it names may not be
        // the thing that reads.
        if (consumer.kind == ConsumerKind::Unknown) {
            ++result.unattributed;
            if (result.firstUnresolvedOwner == 0) {
                result.firstUnresolvedOwner = consumer.ownerId;
            }
            continue;
        }
        if (consumer.resolution == ResolutionKind::Unresolved) {
            ++result.unresolved;
            if (result.firstUnresolvedOwner == 0) {
                result.firstUnresolvedOwner = consumer.ownerId;
            }
            continue;
        }
        ++result.resolved;
    }
    return result;
}

bool SwitchRequiresMigration(
    const RendererMode from,
    const RendererMode to) noexcept
{
    static_cast<void>(from);
    static_cast<void>(to);
    // Presentation policy is independent of scene and frame-graph semantics.
    // Nothing persisted describes how the image reached the screen, so nothing
    // persisted has to change when that changes. A migration here would make
    // the fallback mode unusable as a fallback, which is the whole reason
    // Takeover stays supported.
    return false;
}

const char* ToString(const PresentationError error) noexcept
{
    switch (error) {
    case PresentationError::None: return "none";
    case PresentationError::NoSuitableSurface: return "no suitable surface";
    case PresentationError::HdrWithoutCapableDisplay:
        return "hdr without capable display";
    case PresentationError::HdrWithoutWideFormat:
        return "hdr without wide format";
    case PresentationError::NoSupportedPresentMode:
        return "no supported present mode";
    case PresentationError::UnresolvedD3dConsumer:
        return "unresolved d3d consumer";
    case PresentationError::ZeroExtent: return "zero extent";
    }
    return "unknown";
}

const char* ToString(const PresentMode mode) noexcept
{
    switch (mode) {
    case PresentMode::Immediate: return "immediate";
    case PresentMode::Mailbox: return "mailbox";
    case PresentMode::FifoRelaxed: return "fifo relaxed";
    case PresentMode::Fifo: return "fifo";
    }
    return "unknown";
}

const char* ToString(const SwapchainAction action) noexcept
{
    switch (action) {
    case SwapchainAction::Present: return "present";
    case SwapchainAction::PresentThenRecreate:
        return "present then recreate";
    case SwapchainAction::RecreateSwapchain: return "recreate swapchain";
    case SwapchainAction::RecreateSurface: return "recreate surface";
    case SwapchainAction::RecreateDevice: return "recreate device";
    case SwapchainAction::SkipFrame: return "skip frame";
    }
    return "unknown";
}

}
