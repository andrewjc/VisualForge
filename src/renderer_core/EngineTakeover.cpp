#include "renderer_core/EngineTakeover.h"

namespace vf::renderer::takeover {

namespace {

// Whether this frame's evidence is clean enough to spend a recovery frame on.
// Only the conditions that describe the backend's own health count: a frame
// denied because a video is loading says nothing about whether the fault that
// put us in recovery has passed.
[[nodiscard]] bool EvidenceIsQuiet(const FrameEvidence& evidence) noexcept
{
    return !evidence.fenceTimedOut && !evidence.backendFaulted &&
        !evidence.workInFlight && evidence.backendLoaded;
}

}

TakeoverPermit EvaluatePermit(
    const FrameEvidence& evidence,
    const std::uint64_t frameIndex,
    const ArmingRules& rules) noexcept
{
    TakeoverPermit permit{};
    permit.frameIndex = frameIndex;
    // One frame, always. A permit that outlives its frame is a decision made
    // against evidence nobody re-checked.
    permit.expiryFrame = frameIndex + 1;
    permit.buildSignature = evidence.buildSignature;

    // Every predicate is evaluated. Short-circuiting would report the first
    // reason only, and fixing that one would just reveal the next: one rebuild
    // per reason, on a renderer whose rebuild is not cheap.
    if (evidence.mode != RendererMode::Takeover) {
        permit.denied |= DenyModeNotTakeover;
    }
    if (!evidence.backendLoaded) permit.denied |= DenyBackendNotLoaded;
    if (!evidence.capabilitiesSatisfied) permit.denied |= DenyCapabilityMissing;
    // Takeover is build-signature specific. Every offset this renderer reads
    // was measured against one build, and a different one turns them into
    // reads of whatever now occupies the address.
    if (evidence.buildSignature != evidence.expectedBuildSignature) {
        permit.denied |= DenyBuildSignatureMismatch;
    }
    // An unknown writer to a world target is the one that matters most: its
    // output is part of the world nobody classified, so suppressing vanilla
    // removes something this renderer never knew to draw.
    if (evidence.unknownWorldWriters != 0) {
        permit.denied |= DenyUnknownWorldWriter;
    }
    if (evidence.unknownGeometry != 0) permit.denied |= DenyUnknownGeometry;
    if (evidence.unknownMaterials != 0) permit.denied |= DenyUnknownMaterial;
    // A resource still being generated would be sampled at whatever it
    // contains so far, which is a texture that is subtly wrong rather than
    // obviously missing.
    if (evidence.incompleteResources != 0) {
        permit.denied |= DenyIncompleteResources;
    }
    if (evidence.backendLagFrames > rules.maximumBackendLagFrames) {
        permit.denied |= DenyBackendLag;
    }
    if (evidence.fenceTimedOut) permit.denied |= DenyFenceTimeout;
    if (evidence.backendFaulted) permit.denied |= DenyBackendFault;
    // During a load the world is being replaced underneath the capture, so
    // whatever was classified a frame ago describes a scene that no longer
    // exists.
    if (evidence.loadingTransition) permit.denied |= DenyLoadingTransition;
    if (!evidence.hooksInstalled) permit.denied |= DenyHooksNotInstalled;
    // Hooks removed, or a mode change, while GPU work is outstanding. Arming
    // over it would suppress vanilla for a frame whose Vulkan half is about to
    // be abandoned.
    if (evidence.workInFlight) permit.denied |= DenyWorkInFlight;

    permit.granted = permit.denied == DenyNone;
    return permit;
}

bool VanillaRestorableThisFrame(const FramePhase phase) noexcept
{
    // The dividing line of the whole phase. Up to suppression the vanilla
    // world submission is still ahead of the engine and abandoning takeover
    // costs nothing. From suppression onwards those draws have been discarded
    // and the engine has walked past them: there is no call that puts them
    // back, so the frame can only be completed by Vulkan or by a previous
    // Vulkan frame.
    return phase == FramePhase::BeforeArming ||
        phase == FramePhase::AfterArmingBeforeSuppression;
}

TakeoverController::TakeoverController(const ArmingRules& rules) noexcept
    : rules_(rules)
{
}

TakeoverPermit TakeoverController::BeginFrame(
    const std::uint64_t frameIndex,
    const FrameEvidence& evidence) noexcept
{
    // Once per frame. A second call inside the same frame returns the first
    // answer whatever the evidence looks like by then: two consumers
    // disagreeing about whether this frame was taken over is precisely how a
    // frame gets drawn half by each renderer.
    if (hasCurrentFrame_ && currentFrame_ == frameIndex) return permit_;

    currentFrame_ = frameIndex;
    hasCurrentFrame_ = true;

    if (recoveryFrames_ != 0) {
        // Spend a recovery frame only on a frame that was itself quiet.
        // Counting faulty frames would let a continuously broken backend
        // re-arm on schedule, fault, and alternate forever.
        if (EvidenceIsQuiet(evidence)) {
            --recoveryFrames_;
        } else {
            recoveryFrames_ = rules_.cleanFramesBeforeRearm;
        }
        permit_ = TakeoverPermit{};
        permit_.frameIndex = frameIndex;
        permit_.expiryFrame = frameIndex + 1;
        permit_.buildSignature = evidence.buildSignature;
        permit_.denied = DenyFaultRecovery;
        return permit_;
    }

    permit_ = EvaluatePermit(evidence, frameIndex, rules_);
    return permit_;
}

FrameOutcome TakeoverController::ReportFault(
    const FaultKind kind,
    const FramePhase phase) noexcept
{
    FrameOutcome outcome{};
    outcome.fault = kind;

    const auto armed = permit_.Grants(currentFrame_);
    // Vanilla was never suppressed on a frame that was not armed, and it is
    // still ahead of the engine before suppression. Either way the frame is
    // finished by vanilla, whole.
    const auto suppressed = armed && !VanillaRestorableThisFrame(phase);

    // Out of takeover for the frames that follow, whichever way this one is
    // salvaged. A fault that recurs every frame produces a picture that
    // alternates between two renderers, which is harder to diagnose than a
    // picture that is simply vanilla.
    recoveryFrames_ = rules_.cleanFramesBeforeRearm;
    permit_ = TakeoverPermit{};
    permit_.frameIndex = currentFrame_;
    permit_.expiryFrame = currentFrame_ + 1;
    permit_.denied = DenyFaultRecovery;

    if (!suppressed) {
        outcome.presentation = FramePresentation::Vanilla;
        outcome.worldSuppressed = false;
        outcome.presentedFrame = currentFrame_;
        return outcome;
    }

    outcome.worldSuppressed = true;
    if (hasLastGood_) {
        // A whole earlier frame. It is stale by one frame or more, which is
        // visible as a hitch; the alternative is a frame missing its world,
        // which is visible as a crash report.
        outcome.presentation = FramePresentation::LastGoodVulkan;
        outcome.presentedFrame = lastGoodFrame_;
        return outcome;
    }
    // Nothing has ever completed. Issuing no present leaves the display
    // holding what it already had, which is still one whole frame.
    outcome.presentation = FramePresentation::HoldPrevious;
    outcome.presentedFrame = 0;
    return outcome;
}

FrameOutcome TakeoverController::CompleteFrame(
    const bool vulkanOutputComplete) noexcept
{
    FrameOutcome outcome{};
    if (!permit_.Grants(currentFrame_)) {
        // Not armed, or the permit was invalidated mid-frame by a lifecycle
        // event. Vanilla drew the world and finished the frame.
        outcome.presentation = FramePresentation::Vanilla;
        outcome.presentedFrame = currentFrame_;
        return outcome;
    }

    outcome.worldSuppressed = true;
    if (!vulkanOutputComplete) {
        // Clean run, unfinished image. Recording it as good would let a later
        // fault present an incomplete frame, which is worse than presenting an
        // older one because it looks current.
        if (hasLastGood_) {
            outcome.presentation = FramePresentation::LastGoodVulkan;
            outcome.presentedFrame = lastGoodFrame_;
            return outcome;
        }
        outcome.presentation = FramePresentation::HoldPrevious;
        return outcome;
    }

    hasLastGood_ = true;
    lastGoodFrame_ = currentFrame_;
    outcome.presentation = FramePresentation::Vulkan;
    outcome.presentedFrame = currentFrame_;
    return outcome;
}

void TakeoverController::NotifyLifecycle(const LifecycleEvent event) noexcept
{
    static_cast<void>(event);
    // Immediately, not at the next frame boundary. Hooks removed while work is
    // in flight, a device reset, or a load do not wait for the frame to end,
    // and a permit that survives one of them authorises suppression against
    // resources that no longer describe anything.
    permit_ = TakeoverPermit{};
    permit_.frameIndex = currentFrame_;
    permit_.expiryFrame = currentFrame_ + 1;
    permit_.denied = DenyWorkInFlight;
    // Whatever completed before the event described the old world, so it is no
    // longer a frame worth falling back to.
    hasLastGood_ = false;
    lastGoodFrame_ = 0;
    recoveryFrames_ = rules_.cleanFramesBeforeRearm;
}

bool TakeoverController::HasLastGood() const noexcept
{
    return hasLastGood_;
}

std::uint64_t TakeoverController::LastGoodFrame() const noexcept
{
    return lastGoodFrame_;
}

std::uint32_t TakeoverController::RecoveryFramesRemaining() const noexcept
{
    return recoveryFrames_;
}

const TakeoverPermit& TakeoverController::CurrentPermit() const noexcept
{
    return permit_;
}

void WorldCaptureMatrix::Record(
    const SceneClass sceneClass,
    const std::uint32_t unknownWorldWriters,
    const std::uint32_t missingVisibleClasses) noexcept
{
    const auto index = static_cast<std::size_t>(sceneClass);
    if (index >= cells_.size()) return;
    auto& cell = cells_[index];
    cell.observed = true;
    // The latest sweep replaces the previous one rather than accumulating, so
    // a class fixed and re-run reports as covered without repeating the whole
    // matrix.
    cell.unknownWorldWriters = unknownWorldWriters;
    cell.missingVisibleClasses = missingVisibleClasses;
}

bool WorldCaptureMatrix::Covered(const SceneClass sceneClass) const noexcept
{
    const auto index = static_cast<std::size_t>(sceneClass);
    if (index >= cells_.size()) return false;
    const auto& cell = cells_[index];
    // Observed is not covered. A scene walked through with an unclassified
    // world writer still has part of its world drawn by something nobody
    // accounted for, and a missing visible class looks fine precisely because
    // the thing that would have shown the problem was not there.
    return cell.observed && cell.unknownWorldWriters == 0 &&
        cell.missingVisibleClasses == 0;
}

bool WorldCaptureMatrix::Complete() const noexcept
{
    return Outstanding() == 0;
}

std::uint32_t WorldCaptureMatrix::Outstanding() const noexcept
{
    std::uint32_t outstanding = 0;
    for (std::size_t index = 0; index < cells_.size(); ++index) {
        if (!Covered(static_cast<SceneClass>(index))) ++outstanding;
    }
    return outstanding;
}

SceneClass WorldCaptureMatrix::FirstOutstanding() const noexcept
{
    for (std::size_t index = 0; index < cells_.size(); ++index) {
        const auto sceneClass = static_cast<SceneClass>(index);
        if (!Covered(sceneClass)) return sceneClass;
    }
    return SceneClass::Count;
}

const char* ToString(const FaultKind kind) noexcept
{
    switch (kind) {
    case FaultKind::None: return "none";
    case FaultKind::FenceTimeout: return "fence timeout";
    case FaultKind::DeviceLost: return "device lost";
    case FaultKind::SubmitFailed: return "submit failed";
    case FaultKind::AcquireFailed: return "acquire failed";
    case FaultKind::PresentFailed: return "present failed";
    case FaultKind::ResourceExhausted: return "resource exhausted";
    case FaultKind::UnknownWorldWriter: return "unknown world writer";
    }
    return "unknown";
}

const char* ToString(const FramePresentation presentation) noexcept
{
    switch (presentation) {
    case FramePresentation::Vanilla: return "vanilla";
    case FramePresentation::Vulkan: return "vulkan";
    case FramePresentation::LastGoodVulkan: return "last good vulkan";
    case FramePresentation::HoldPrevious: return "hold previous";
    }
    return "unknown";
}

const char* ToString(const SceneClass sceneClass) noexcept
{
    switch (sceneClass) {
    case SceneClass::Interior: return "interior";
    case SceneClass::Exterior: return "exterior";
    case SceneClass::DenseSettlement: return "dense settlement";
    case SceneClass::Combat: return "combat";
    case SceneClass::FirstPerson: return "first person";
    case SceneClass::ThirdPerson: return "third person";
    case SceneClass::Terrain: return "terrain";
    case SceneClass::Water: return "water";
    case SceneClass::Weather: return "weather";
    case SceneClass::Loading: return "loading";
    case SceneClass::PipBoy: return "pip-boy";
    case SceneClass::Vats: return "vats";
    case SceneClass::Dialogue: return "dialogue";
    case SceneClass::Map: return "map";
    case SceneClass::Terminal: return "terminal";
    case SceneClass::PowerArmor: return "power armor";
    case SceneClass::Count: return "count";
    }
    return "unknown";
}

const char* DenyReasonName(const std::uint32_t singleBit) noexcept
{
    switch (singleBit) {
    case DenyNone: return "none";
    case DenyModeNotTakeover: return "mode not takeover";
    case DenyBackendNotLoaded: return "backend not loaded";
    case DenyCapabilityMissing: return "capability missing";
    case DenyBuildSignatureMismatch: return "build signature mismatch";
    case DenyUnknownWorldWriter: return "unknown world writer";
    case DenyUnknownGeometry: return "unknown geometry";
    case DenyUnknownMaterial: return "unknown material";
    case DenyIncompleteResources: return "incomplete resources";
    case DenyBackendLag: return "backend lag";
    case DenyFenceTimeout: return "fence timeout";
    case DenyBackendFault: return "backend fault";
    case DenyLoadingTransition: return "loading transition";
    case DenyHooksNotInstalled: return "hooks not installed";
    case DenyWorkInFlight: return "work in flight";
    case DenyFaultRecovery: return "fault recovery";
    default: return "unknown";
    }
}

}
