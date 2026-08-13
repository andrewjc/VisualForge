#include "renderer_core/EngineTakeover.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace vf::renderer;

namespace {

// Evidence that arms. Every test starts from a frame that would be taken over
// and spoils exactly one thing, so a denial is attributable to that one thing
// rather than to whatever else the fixture happened to leave unset.
takeover::FrameEvidence Clean()
{
    takeover::FrameEvidence evidence{};
    evidence.mode = RendererMode::Takeover;
    evidence.buildSignature = 0x1'11'221'0ull;
    evidence.expectedBuildSignature = 0x1'11'221'0ull;
    evidence.backendLoaded = true;
    evidence.capabilitiesSatisfied = true;
    evidence.hooksInstalled = true;
    return evidence;
}

}

TEST_CASE("P25_arming_requires_every_predicate", "[phase25][takeover]")
{
    const takeover::ArmingRules rules{};

    const auto granted = takeover::EvaluatePermit(Clean(), 100, rules);
    CHECK(granted.granted);
    CHECK(granted.denied == takeover::DenyNone);
    CHECK(granted.Grants(100));

    // Each predicate alone denies, and the permit says which. A boolean that
    // only reports "no" turns every arming investigation into a bisection of
    // the arming code, which is the code least likely to be at fault.
    struct Spoiler
    {
        const char* name;
        void (*apply)(takeover::FrameEvidence&);
        std::uint32_t reason;
    };

    const std::array<Spoiler, 13> spoilers{{
        {"mode", [](takeover::FrameEvidence& e) {
            e.mode = RendererMode::Mirror;
        }, takeover::DenyModeNotTakeover},
        {"backend", [](takeover::FrameEvidence& e) {
            e.backendLoaded = false;
        }, takeover::DenyBackendNotLoaded},
        {"capability", [](takeover::FrameEvidence& e) {
            e.capabilitiesSatisfied = false;
        }, takeover::DenyCapabilityMissing},
        {"build signature", [](takeover::FrameEvidence& e) {
            e.buildSignature = 0x1'10'163'0ull;
        }, takeover::DenyBuildSignatureMismatch},
        {"unknown world writer", [](takeover::FrameEvidence& e) {
            e.unknownWorldWriters = 1;
        }, takeover::DenyUnknownWorldWriter},
        {"unknown geometry", [](takeover::FrameEvidence& e) {
            e.unknownGeometry = 3;
        }, takeover::DenyUnknownGeometry},
        {"unknown material", [](takeover::FrameEvidence& e) {
            e.unknownMaterials = 2;
        }, takeover::DenyUnknownMaterial},
        {"incomplete resources", [](takeover::FrameEvidence& e) {
            e.incompleteResources = 7;
        }, takeover::DenyIncompleteResources},
        {"backend lag", [](takeover::FrameEvidence& e) {
            e.backendLagFrames = 9;
        }, takeover::DenyBackendLag},
        {"fence timeout", [](takeover::FrameEvidence& e) {
            e.fenceTimedOut = true;
        }, takeover::DenyFenceTimeout},
        {"backend fault", [](takeover::FrameEvidence& e) {
            e.backendFaulted = true;
        }, takeover::DenyBackendFault},
        {"loading transition", [](takeover::FrameEvidence& e) {
            e.loadingTransition = true;
        }, takeover::DenyLoadingTransition},
        {"hooks", [](takeover::FrameEvidence& e) {
            e.hooksInstalled = false;
        }, takeover::DenyHooksNotInstalled},
    }};

    for (const auto& spoiler : spoilers) {
        INFO(spoiler.name);
        auto evidence = Clean();
        spoiler.apply(evidence);
        const auto permit = takeover::EvaluatePermit(evidence, 100, rules);
        CHECK_FALSE(permit.granted);
        CHECK((permit.denied & spoiler.reason) == spoiler.reason);
        CHECK_FALSE(permit.Grants(100));
    }

    // Reasons accumulate rather than short-circuiting. Reporting the first one
    // found means fixing it only reveals the next, one rebuild at a time.
    auto several = Clean();
    several.backendLoaded = false;
    several.unknownGeometry = 4;
    several.fenceTimedOut = true;
    const auto multiple = takeover::EvaluatePermit(several, 100, rules);
    CHECK((multiple.denied & takeover::DenyBackendNotLoaded) != 0);
    CHECK((multiple.denied & takeover::DenyUnknownGeometry) != 0);
    CHECK((multiple.denied & takeover::DenyFenceTimeout) != 0);

    // Lag is bounded, not forbidden. A backend one frame behind is normal; one
    // far behind is producing images for a camera that has moved on, and that
    // reads as input latency rather than as a renderer that is behind.
    auto lagging = Clean();
    lagging.backendLagFrames = rules.maximumBackendLagFrames;
    CHECK(takeover::EvaluatePermit(lagging, 100, rules).granted);
    lagging.backendLagFrames = rules.maximumBackendLagFrames + 1;
    CHECK_FALSE(takeover::EvaluatePermit(lagging, 100, rules).granted);
}

TEST_CASE("P25_the_frame_decision_is_taken_once_and_expires_with_its_frame",
    "[phase25][takeover]")
{
    takeover::TakeoverController controller{};
    const auto permit = controller.BeginFrame(10, Clean());
    REQUIRE(permit.granted);

    // Evidence that changes mid-frame does not change the decision. Two
    // consumers in the same frame disagreeing about whether it was taken over
    // is exactly how a half-vanilla frame gets built.
    auto spoiled = Clean();
    spoiled.unknownWorldWriters = 5;
    spoiled.backendFaulted = true;
    const auto again = controller.BeginFrame(10, spoiled);
    CHECK(again.granted);
    CHECK(again.denied == takeover::DenyNone);
    CHECK(again.frameIndex == 10);

    // And the permit is for its own frame only. A yes carried into the next
    // frame is a yes based on evidence nobody re-checked.
    CHECK(permit.Grants(10));
    CHECK_FALSE(permit.Grants(11));
    CHECK_FALSE(permit.Grants(9));
    CHECK(permit.expiryFrame == 11);

    // The next frame is decided afresh, and now the spoiled evidence counts.
    const auto next = controller.BeginFrame(11, spoiled);
    CHECK_FALSE(next.granted);
    CHECK((next.denied & takeover::DenyUnknownWorldWriter) != 0);
}

TEST_CASE("P25_no_fault_at_any_phase_produces_a_half_vanilla_frame",
    "[phase25][takeover]")
{
    // The gate for the whole phase: fault injection at every frame phase must
    // leave a frame that is entirely one thing or entirely the other.
    constexpr std::array<takeover::FramePhase, 5> phases{
        takeover::FramePhase::BeforeArming,
        takeover::FramePhase::AfterArmingBeforeSuppression,
        takeover::FramePhase::AfterSuppression,
        takeover::FramePhase::AfterSubmission,
        takeover::FramePhase::AtPresent};
    constexpr std::array<takeover::FaultKind, 6> kinds{
        takeover::FaultKind::FenceTimeout,
        takeover::FaultKind::DeviceLost,
        takeover::FaultKind::SubmitFailed,
        takeover::FaultKind::AcquireFailed,
        takeover::FaultKind::PresentFailed,
        takeover::FaultKind::ResourceExhausted};

    for (const auto phase : phases) {
        for (const auto kind : kinds) {
            INFO(takeover::ToString(kind));
            takeover::TakeoverController controller{};
            REQUIRE(controller.BeginFrame(1, Clean()).granted);
            const auto outcome = controller.ReportFault(kind, phase);
            CHECK(outcome.IsWholeFrame());
            CHECK(outcome.fault == kind);
        }
    }

    // Before the world submission is suppressed, vanilla finishes the frame:
    // its draws have not been discarded yet.
    CHECK(takeover::VanillaRestorableThisFrame(
        takeover::FramePhase::BeforeArming));
    CHECK(takeover::VanillaRestorableThisFrame(
        takeover::FramePhase::AfterArmingBeforeSuppression));
    // After it, they are gone and the engine has walked past them. Re-issuing
    // is not on the table, so the only whole frame left is a previous one.
    CHECK_FALSE(takeover::VanillaRestorableThisFrame(
        takeover::FramePhase::AfterSuppression));
    CHECK_FALSE(takeover::VanillaRestorableThisFrame(
        takeover::FramePhase::AfterSubmission));
    CHECK_FALSE(takeover::VanillaRestorableThisFrame(
        takeover::FramePhase::AtPresent));

    takeover::TakeoverController early{};
    REQUIRE(early.BeginFrame(1, Clean()).granted);
    const auto restored = early.ReportFault(
        takeover::FaultKind::SubmitFailed,
        takeover::FramePhase::AfterArmingBeforeSuppression);
    CHECK(restored.presentation == takeover::FramePresentation::Vanilla);
    CHECK_FALSE(restored.worldSuppressed);
}

TEST_CASE("P25_a_fault_after_suppression_presents_the_last_completed_output",
    "[phase25][takeover]")
{
    takeover::TakeoverController controller{};

    // Nothing has ever completed, so there is no last good output to fall back
    // on. Holding what the display already has is still a whole frame; drawing
    // half of this one is not.
    REQUIRE(controller.BeginFrame(1, Clean()).granted);
    CHECK_FALSE(controller.HasLastGood());
    const auto first = controller.ReportFault(
        takeover::FaultKind::DeviceLost, takeover::FramePhase::AfterSubmission);
    CHECK(first.presentation == takeover::FramePresentation::HoldPrevious);
    CHECK(first.worldSuppressed);
    CHECK(first.presentedFrame == 0);
    CHECK(first.IsWholeFrame());

    // A clean frame establishes one.
    takeover::TakeoverController settled{};
    REQUIRE(settled.BeginFrame(20, Clean()).granted);
    const auto good = settled.CompleteFrame(true);
    CHECK(good.presentation == takeover::FramePresentation::Vulkan);
    CHECK(good.presentedFrame == 20);
    CHECK(settled.HasLastGood());
    CHECK(settled.LastGoodFrame() == 20);

    // A frame that faults after suppression now has something whole to show.
    REQUIRE(settled.BeginFrame(21, Clean()).granted);
    const auto held = settled.ReportFault(
        takeover::FaultKind::PresentFailed, takeover::FramePhase::AtPresent);
    CHECK(held.presentation == takeover::FramePresentation::LastGoodVulkan);
    CHECK(held.presentedFrame == 20);
    CHECK(held.IsWholeFrame());

    // A Vulkan frame that did not actually complete is not recorded as good,
    // however clean the run through the frame looked. Presenting an
    // unfinished image later is worse than presenting an older one, because
    // it looks current.
    takeover::TakeoverController partial{};
    REQUIRE(partial.BeginFrame(30, Clean()).granted);
    const auto incomplete = partial.CompleteFrame(false);
    CHECK(incomplete.presentation == takeover::FramePresentation::HoldPrevious);
    CHECK_FALSE(partial.HasLastGood());
}

TEST_CASE("P25_a_fault_restores_vanilla_next_frame_and_re_arms_only_after_quiet",
    "[phase25][takeover]")
{
    takeover::ArmingRules rules{};
    rules.cleanFramesBeforeRearm = 3;
    takeover::TakeoverController controller{rules};

    REQUIRE(controller.BeginFrame(1, Clean()).granted);
    const auto faulted = controller.ReportFault(
        takeover::FaultKind::DeviceLost, takeover::FramePhase::AfterSuppression);
    CHECK(faulted.IsWholeFrame());

    // The next frame is vanilla even though the evidence is spotless: the
    // fault has not been outlived yet. Re-arming immediately produces a fault
    // every frame and a picture that alternates, which is harder to diagnose
    // than staying off.
    const auto immediately = controller.BeginFrame(2, Clean());
    CHECK_FALSE(immediately.granted);
    CHECK((immediately.denied & takeover::DenyFaultRecovery) != 0);
    CHECK(controller.RecoveryFramesRemaining() == 2);

    CHECK_FALSE(controller.BeginFrame(3, Clean()).granted);
    CHECK_FALSE(controller.BeginFrame(4, Clean()).granted);
    CHECK(controller.RecoveryFramesRemaining() == 0);
    CHECK(controller.BeginFrame(5, Clean()).granted);

    // A frame that is not clean during recovery restarts the count rather than
    // spending it. Counting frames that were themselves faulty would let a
    // continuously broken backend re-arm on schedule.
    takeover::TakeoverController restarting{rules};
    REQUIRE(restarting.BeginFrame(1, Clean()).granted);
    static_cast<void>(restarting.ReportFault(
        takeover::FaultKind::SubmitFailed,
        takeover::FramePhase::AfterSuppression));
    CHECK_FALSE(restarting.BeginFrame(2, Clean()).granted);
    auto dirty = Clean();
    dirty.backendFaulted = true;
    CHECK_FALSE(restarting.BeginFrame(3, dirty).granted);
    CHECK(restarting.RecoveryFramesRemaining() == 3);
}

TEST_CASE("P25_lifecycle_events_invalidate_the_standing_permit",
    "[phase25][takeover]")
{
    constexpr std::array<takeover::LifecycleEvent, 5> events{
        takeover::LifecycleEvent::ModeTransition,
        takeover::LifecycleEvent::SaveLoad,
        takeover::LifecycleEvent::HookRemoval,
        takeover::LifecycleEvent::DeviceReset,
        takeover::LifecycleEvent::SwapchainResize};

    for (const auto event : events) {
        takeover::TakeoverController controller{};
        REQUIRE(controller.BeginFrame(7, Clean()).granted);

        // Immediately, not at the next frame boundary: the boundary may be on
        // the far side of the event, and hooks removed while work is in flight
        // do not wait for it.
        controller.NotifyLifecycle(event);
        CHECK_FALSE(controller.CurrentPermit().granted);
        CHECK_FALSE(controller.CurrentPermit().Grants(7));

        // Whatever was in flight is not presented as this frame's output. It
        // was produced against resources the event has already invalidated.
        const auto outcome = controller.CompleteFrame(true);
        CHECK(outcome.presentation != takeover::FramePresentation::Vulkan);
        CHECK(outcome.IsWholeFrame());
    }

    // Work in flight denies arming directly too, so a hook removal that
    // arrives between frames is not merely tidied up after.
    auto inFlight = Clean();
    inFlight.workInFlight = true;
    const auto permit =
        takeover::EvaluatePermit(inFlight, 7, takeover::ArmingRules{});
    CHECK_FALSE(permit.granted);
    CHECK((permit.denied & takeover::DenyWorkInFlight) != 0);
}

TEST_CASE("P25_the_world_capture_matrix_is_complete_only_when_every_class_is_clean",
    "[phase25][takeover]")
{
    takeover::WorldCaptureMatrix matrix{};
    CHECK_FALSE(matrix.Complete());
    CHECK(matrix.Outstanding() ==
        static_cast<std::uint32_t>(takeover::SceneClass::Count));

    for (std::uint32_t index = 0;
        index < static_cast<std::uint32_t>(takeover::SceneClass::Count);
        ++index) {
        matrix.Record(static_cast<takeover::SceneClass>(index), 0, 0);
    }
    CHECK(matrix.Complete());
    CHECK(matrix.Outstanding() == 0);

    // An observed class with an unknown world-target writer is not covered.
    // Counting it would let the matrix report completeness for a scene whose
    // world is partly drawn by something nobody classified.
    matrix.Record(takeover::SceneClass::PowerArmor, 1, 0);
    CHECK_FALSE(matrix.Covered(takeover::SceneClass::PowerArmor));
    CHECK_FALSE(matrix.Complete());
    CHECK(matrix.Outstanding() == 1);
    CHECK(matrix.FirstOutstanding() == takeover::SceneClass::PowerArmor);

    // A missing visible class counts the same way: the frame looked fine
    // because something simply was not there to notice.
    matrix.Record(takeover::SceneClass::PowerArmor, 0, 0);
    CHECK(matrix.Complete());
    matrix.Record(takeover::SceneClass::Vats, 0, 2);
    CHECK_FALSE(matrix.Complete());
    CHECK(matrix.FirstOutstanding() == takeover::SceneClass::Vats);

    // Recording a class clean again clears it, so a re-run of the matrix after
    // a fix does not need the whole sweep repeated to report progress.
    matrix.Record(takeover::SceneClass::Vats, 0, 0);
    CHECK(matrix.Complete());
}
