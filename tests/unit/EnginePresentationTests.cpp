#include "renderer_core/EnginePresentation.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace vf::renderer;

namespace {

wsi::SurfaceState Surface()
{
    wsi::SurfaceState state{};
    state.format = wsi::SurfaceFormat::Bgra8Srgb;
    state.colorSpace = wsi::ColorSpace::SrgbNonlinear;
    state.presentMode = wsi::PresentMode::Fifo;
    state.width = 1920;
    state.height = 1080;
    state.displayId = 1;
    return state;
}

}

TEST_CASE("P27_surface_selection_is_deterministic_not_first_available",
    "[phase27][wsi]")
{
    // First-available differs by driver, so the same build produces a
    // different colour on two machines and the difference reproduces on
    // neither. The order of the candidate list must not decide the outcome.
    const std::array<wsi::SurfaceCandidate, 3> sdr{{
        {wsi::SurfaceFormat::Rgba8Srgb, wsi::ColorSpace::SrgbNonlinear},
        {wsi::SurfaceFormat::Bgra8Srgb, wsi::ColorSpace::SrgbNonlinear},
        {wsi::SurfaceFormat::Rgb10A2Unorm, wsi::ColorSpace::Hdr10St2084}}};
    const std::array<wsi::SurfaceCandidate, 3> reordered{{
        {wsi::SurfaceFormat::Rgb10A2Unorm, wsi::ColorSpace::Hdr10St2084},
        {wsi::SurfaceFormat::Bgra8Srgb, wsi::ColorSpace::SrgbNonlinear},
        {wsi::SurfaceFormat::Rgba8Srgb, wsi::ColorSpace::SrgbNonlinear}}};

    wsi::DisplayPolicy policy{};
    const auto first = wsi::SelectSurface(sdr, policy);
    const auto second = wsi::SelectSurface(reordered, policy);
    CHECK(first.error == wsi::PresentationError::None);
    CHECK(first.candidate.format == second.candidate.format);
    CHECK(first.candidate.colorSpace == second.candidate.colorSpace);
    CHECK(first.candidate.colorSpace == wsi::ColorSpace::SrgbNonlinear);
    CHECK_FALSE(first.hdrActive);

    // HDR asked for and available.
    policy.hdrRequested = true;
    policy.hdrCapable = true;
    const auto hdr = wsi::SelectSurface(sdr, policy);
    CHECK(hdr.error == wsi::PresentationError::None);
    CHECK(hdr.candidate.colorSpace == wsi::ColorSpace::Hdr10St2084);
    CHECK(hdr.hdrActive);

    // Ten bits at minimum for an ST 2084 encoding. Eight bits across a PQ
    // curve bands visibly in every dark gradient, and the banding reads as a
    // tone-mapping fault rather than as a format that cannot carry the range.
    const std::array<wsi::SurfaceCandidate, 2> narrowHdr{{
        {wsi::SurfaceFormat::Bgra8Srgb, wsi::ColorSpace::SrgbNonlinear},
        {wsi::SurfaceFormat::Bgra8Srgb, wsi::ColorSpace::Hdr10St2084}}};
    const auto narrow = wsi::SelectSurface(narrowHdr, policy);
    CHECK(narrow.error == wsi::PresentationError::HdrWithoutWideFormat);
    // And it falls back to a whole SDR surface rather than to a bad HDR one.
    CHECK(narrow.candidate.colorSpace == wsi::ColorSpace::SrgbNonlinear);
    CHECK_FALSE(narrow.hdrActive);

    // Requested on a display that cannot show it. Presenting PQ to an SDR
    // panel washes the whole image out in a way that looks like a gamma bug.
    policy.hdrCapable = false;
    const auto refused = wsi::SelectSurface(sdr, policy);
    CHECK(refused.error == wsi::PresentationError::HdrWithoutCapableDisplay);
    CHECK_FALSE(refused.hdrActive);
    CHECK(refused.candidate.colorSpace == wsi::ColorSpace::SrgbNonlinear);

    const std::array<wsi::SurfaceCandidate, 0> nothing{};
    CHECK(wsi::SelectSurface(nothing, wsi::DisplayPolicy{}).error ==
        wsi::PresentationError::NoSuitableSurface);
}

TEST_CASE("P27_present_mode_falls_back_to_the_one_the_spec_guarantees",
    "[phase27][wsi]")
{
    const std::array<wsi::PresentMode, 4> all{
        wsi::PresentMode::Immediate, wsi::PresentMode::Mailbox,
        wsi::PresentMode::FifoRelaxed, wsi::PresentMode::Fifo};

    wsi::PresentModeRequest request{};
    // Vsync without the latency: mailbox replaces the queued image rather than
    // queueing another, so a fast frame does not wait behind a stale one.
    CHECK(wsi::SelectPresentMode(all, request).mode == wsi::PresentMode::Mailbox);

    // Relaxed FIFO is a different contract, not a free upgrade, so it is only
    // chosen when the caller has said it will accept a tear on a late frame.
    request.tolerateLateTearing = true;
    const std::array<wsi::PresentMode, 2> noMailbox{
        wsi::PresentMode::FifoRelaxed, wsi::PresentMode::Fifo};
    CHECK(wsi::SelectPresentMode(noMailbox, request).mode ==
        wsi::PresentMode::FifoRelaxed);
    request.tolerateLateTearing = false;
    CHECK(wsi::SelectPresentMode(noMailbox, request).mode ==
        wsi::PresentMode::Fifo);

    request.vsync = false;
    CHECK(wsi::SelectPresentMode(all, request).mode ==
        wsi::PresentMode::Immediate);

    // FIFO is always supported by the specification, which is what makes it
    // the only honest fallback: anything else can be absent on a driver
    // nobody tested against.
    const std::array<wsi::PresentMode, 1> only{wsi::PresentMode::Fifo};
    CHECK(wsi::SelectPresentMode(only, request).mode == wsi::PresentMode::Fifo);

    const std::array<wsi::PresentMode, 0> none{};
    CHECK(wsi::SelectPresentMode(none, request).error ==
        wsi::PresentationError::NoSupportedPresentMode);
}

TEST_CASE("P27_acquire_and_present_failures_resolve_without_spinning",
    "[phase27][wsi]")
{
    // Suboptimal is not an error. The image is presentable; recreating before
    // showing it drops a frame for a condition the specification calls
    // acceptable, and the dropped frames read as a stutter.
    CHECK(wsi::ResolveStatus(wsi::SwapchainStatus::Suboptimal, false) ==
        wsi::SwapchainAction::PresentThenRecreate);

    // Out of date is. Presenting it is undefined, so the frame is abandoned
    // and the chain rebuilt.
    CHECK(wsi::ResolveStatus(wsi::SwapchainStatus::OutOfDate, false) ==
        wsi::SwapchainAction::RecreateSwapchain);

    // A lost surface outlives the swap chain built on it, so rebuilding only
    // the chain fails again immediately and the loop spins.
    CHECK(wsi::ResolveStatus(wsi::SwapchainStatus::SurfaceLost, false) ==
        wsi::SwapchainAction::RecreateSurface);
    CHECK(wsi::ResolveStatus(wsi::SwapchainStatus::DeviceLost, false) ==
        wsi::SwapchainAction::RecreateDevice);
    CHECK(wsi::ResolveStatus(wsi::SwapchainStatus::Ok, false) ==
        wsi::SwapchainAction::Present);

    // Minimized wins over every recreate. A zero-extent swap chain is invalid,
    // so a loop that keeps trying to build one spins for as long as the window
    // stays down, which is measured as the game hanging.
    for (const auto status : {wsi::SwapchainStatus::Ok,
        wsi::SwapchainStatus::Suboptimal, wsi::SwapchainStatus::OutOfDate,
        wsi::SwapchainStatus::SurfaceLost, wsi::SwapchainStatus::Timeout}) {
        CHECK(wsi::ResolveStatus(status, true) ==
            wsi::SwapchainAction::SkipFrame);
    }
    // Except device loss, which has to be handled wherever the window is.
    CHECK(wsi::ResolveStatus(wsi::SwapchainStatus::DeviceLost, true) ==
        wsi::SwapchainAction::RecreateDevice);

    CHECK(wsi::ResolveStatus(wsi::SwapchainStatus::Timeout, false) ==
        wsi::SwapchainAction::SkipFrame);
}

TEST_CASE("P27_a_display_or_format_change_rebuilds_and_a_resize_alone_does_too",
    "[phase27][wsi]")
{
    CHECK_FALSE(wsi::TransitionRequiresRecreate(Surface(), Surface()));

    auto resized = Surface();
    resized.width = 2560;
    CHECK(wsi::TransitionRequiresRecreate(Surface(), resized));

    auto hdr = Surface();
    hdr.colorSpace = wsi::ColorSpace::Hdr10St2084;
    hdr.format = wsi::SurfaceFormat::Rgb10A2Unorm;
    CHECK(wsi::TransitionRequiresRecreate(Surface(), hdr));

    // Dragged to another monitor. The images are still the right size and the
    // right format, and they are for the wrong display.
    auto moved = Surface();
    moved.displayId = 2;
    CHECK(wsi::TransitionRequiresRecreate(Surface(), moved));

    auto paced = Surface();
    paced.presentMode = wsi::PresentMode::Mailbox;
    CHECK(wsi::TransitionRequiresRecreate(Surface(), paced));

    // Minimized: zero extent is not a size to rebuild at, it is a reason not
    // to.
    auto minimized = Surface();
    minimized.width = 0;
    minimized.height = 0;
    CHECK(wsi::TransitionRequiresRecreate(Surface(), minimized));
}

TEST_CASE("P27_pacing_yields_and_never_spins_or_idles_the_device",
    "[phase27][wsi]")
{
    // Nothing established: present immediately rather than waiting out a
    // budget measured from a frame that never happened.
    const auto opening = wsi::Pace(wsi::PacingState{}, 100.0, 1.0 / 60.0);
    CHECK(opening.present);
    CHECK(opening.sleepSeconds == Catch::Approx(0.0));
    CHECK(opening.next.established);

    // A frame that arrives early waits for the remainder of the budget, by
    // yielding. A spin burns a core to hit a deadline the scheduler would have
    // hit anyway, and it is measured as the renderer being expensive.
    wsi::PacingState state{};
    state.established = true;
    state.lastPresentSeconds = 100.0;
    const auto early = wsi::Pace(state, 100.008, 1.0 / 60.0);
    CHECK(early.sleepSeconds == Catch::Approx(1.0 / 60.0 - 0.008).margin(1e-6));
    CHECK(early.present);

    // A frame that is already late does not wait at all, and does not try to
    // claw the time back by presenting early next frame either: catch-up
    // pacing converts one long frame into several short ones and the result
    // is judder rather than a single hitch.
    const auto late = wsi::Pace(state, 100.5, 1.0 / 60.0);
    CHECK(late.sleepSeconds == Catch::Approx(0.0));
    CHECK(late.next.lastPresentSeconds == Catch::Approx(100.5));

    // No target: nothing to pace to.
    CHECK(wsi::Pace(state, 100.001, 0.0).sleepSeconds == Catch::Approx(0.0));

    // The gate, stated as a property rather than trusted. Zero busy-waits and
    // zero routine device-idle calls, on every path.
    for (const auto now : {100.0, 100.001, 100.008, 100.5, 99.0}) {
        for (const auto target : {0.0, 1.0 / 60.0, 1.0 / 144.0}) {
            const auto decision = wsi::Pace(state, now, target);
            CHECK_FALSE(decision.busyWait);
            CHECK_FALSE(decision.deviceIdle);
            CHECK(decision.sleepSeconds >= 0.0);
        }
    }
}

TEST_CASE("P27_exclusive_fullscreen_is_opt_in_and_yields_to_overlays",
    "[phase27][wsi]")
{
    // Exclusive mode takes the display mode away from everything else on the
    // machine and blanks any overlay that is not a display-driver one, which
    // reaches the user as the game having broken their capture software.
    CHECK(wsi::SelectFullscreenPolicy(false, false) ==
        wsi::FullscreenPolicy::Borderless);
    CHECK(wsi::SelectFullscreenPolicy(true, false) ==
        wsi::FullscreenPolicy::ExclusiveOptIn);
    CHECK(wsi::SelectFullscreenPolicy(true, true) ==
        wsi::FullscreenPolicy::Borderless);
    CHECK(wsi::SelectFullscreenPolicy(false, true) ==
        wsi::FullscreenPolicy::Borderless);
}

TEST_CASE("P27_the_compatibility_island_retires_only_when_no_reader_remains",
    "[phase27][wsi]")
{
    const std::array<wsi::ConsumerRecord, 3> resolved{{
        {0x01, wsi::ConsumerKind::EngineReader,
            wsi::ResolutionKind::NativeBackend},
        {0x02, wsi::ConsumerKind::Middleware,
            wsi::ResolutionKind::ImportedLayer},
        {0x03, wsi::ConsumerKind::Capture,
            wsi::ResolutionKind::ScopedFacade}}};
    const auto clean = wsi::AuditConsumers(resolved);
    CHECK(clean.MayRetireD3d());
    CHECK(clean.resolved == 3);

    // A reader nobody found keeps reading a resource nobody creates, which is
    // an access violation at whatever moment it next runs rather than at the
    // moment the island was retired.
    const std::array<wsi::ConsumerRecord, 2> outstanding{{
        {0x01, wsi::ConsumerKind::EngineReader,
            wsi::ResolutionKind::NativeBackend},
        {0x04, wsi::ConsumerKind::Plugin, wsi::ResolutionKind::Unresolved}}};
    const auto blocked = wsi::AuditConsumers(outstanding);
    CHECK_FALSE(blocked.MayRetireD3d());
    CHECK(blocked.unresolved == 1);
    CHECK(blocked.firstUnresolvedOwner == 0x04);

    // An entry the audit found but could not attribute blocks it too, even
    // with a resolution attached: the resolution is for something nobody has
    // identified.
    const std::array<wsi::ConsumerRecord, 1> unattributed{{
        {0x05, wsi::ConsumerKind::Unknown,
            wsi::ResolutionKind::NativeBackend}}};
    const auto anonymous = wsi::AuditConsumers(unattributed);
    CHECK_FALSE(anonymous.MayRetireD3d());
    CHECK(anonymous.unattributed == 1);

    // An empty audit is not proof of an empty island, but it is the only state
    // in which nothing is outstanding, so it retires. The audit's coverage is
    // the fixture's problem, not this function's.
    const std::array<wsi::ConsumerRecord, 0> nothing{};
    CHECK(wsi::AuditConsumers(nothing).MayRetireD3d());
}

TEST_CASE("P27_switching_between_native_and_takeover_migrates_nothing",
    "[phase27][wsi]")
{
    // Presentation policy is independent of scene and frame-graph semantics.
    // A migration here would make the fallback mode unusable as a fallback,
    // which is the whole reason Takeover stays supported.
    constexpr std::array<RendererMode, 6> modes{
        RendererMode::Disabled, RendererMode::Off, RendererMode::Observe,
        RendererMode::Mirror, RendererMode::Takeover, RendererMode::Native};

    for (const auto from : modes) {
        for (const auto to : modes) {
            CHECK_FALSE(wsi::SwitchRequiresMigration(from, to));
        }
    }
}
