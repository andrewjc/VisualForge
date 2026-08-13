#pragma once

#include "renderer_api/RendererMode.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace vf::renderer::takeover {

// Why a frame was not taken over. These accumulate: a permit carries every
// reason it was denied, not the first one found. One reason at a time turns
// every investigation into a sequence of rebuilds, because fixing the reason
// that happened to be checked first only reveals the next.
inline constexpr std::uint32_t DenyNone = 0u;
inline constexpr std::uint32_t DenyModeNotTakeover = 1u << 0;
inline constexpr std::uint32_t DenyBackendNotLoaded = 1u << 1;
inline constexpr std::uint32_t DenyCapabilityMissing = 1u << 2;
inline constexpr std::uint32_t DenyBuildSignatureMismatch = 1u << 3;
inline constexpr std::uint32_t DenyUnknownWorldWriter = 1u << 4;
inline constexpr std::uint32_t DenyUnknownGeometry = 1u << 5;
inline constexpr std::uint32_t DenyUnknownMaterial = 1u << 6;
inline constexpr std::uint32_t DenyIncompleteResources = 1u << 7;
inline constexpr std::uint32_t DenyBackendLag = 1u << 8;
inline constexpr std::uint32_t DenyFenceTimeout = 1u << 9;
inline constexpr std::uint32_t DenyBackendFault = 1u << 10;
inline constexpr std::uint32_t DenyLoadingTransition = 1u << 11;
inline constexpr std::uint32_t DenyHooksNotInstalled = 1u << 12;
inline constexpr std::uint32_t DenyWorkInFlight = 1u << 13;
inline constexpr std::uint32_t DenyFaultRecovery = 1u << 14;

// What went wrong. The kind matters less than when it arrived, but it is
// carried so a fault that repeats can be told from a fault that moved.
enum class FaultKind : std::uint8_t
{
    None = 0,
    FenceTimeout = 1,
    DeviceLost = 2,
    SubmitFailed = 3,
    AcquireFailed = 4,
    PresentFailed = 5,
    ResourceExhausted = 6,
    UnknownWorldWriter = 7,
};

// Where in the frame a fault arrived. This is the whole phase in one
// enumerator: before the world submission is suppressed, vanilla can still
// finish the frame; after it, those draws are gone and the engine has already
// walked past them, so vanilla is no longer an option this frame.
enum class FramePhase : std::uint8_t
{
    BeforeArming = 0,
    AfterArmingBeforeSuppression = 1,
    AfterSuppression = 2,
    AfterSubmission = 3,
    AtPresent = 4,
};

// What actually reaches the display. There is deliberately no enumerator for
// a partial frame: half a vanilla world and half a Vulkan one is not a
// degraded picture, it is a picture nobody can diagnose.
enum class FramePresentation : std::uint8_t
{
    // Vanilla submitted its world and finished the frame.
    Vanilla = 0,
    // This frame's Vulkan output.
    Vulkan = 1,
    // A previous frame's completed Vulkan output, whole, because this frame's
    // could not be finished after vanilla had already been suppressed.
    LastGoodVulkan = 2,
    // No present is issued and the display keeps what it already had. The
    // fallback when suppression has happened and no Vulkan output has ever
    // completed.
    HoldPrevious = 3,
};

// Something outside the frame loop that makes every standing assumption
// stale. Each invalidates the permit immediately rather than at the next
// frame boundary, because the frame boundary may be on the other side of the
// event.
enum class LifecycleEvent : std::uint8_t
{
    ModeTransition = 0,
    SaveLoad = 1,
    HookRemoval = 2,
    DeviceReset = 3,
    SwapchainResize = 4,
};

// The scene classes the world capture matrix has to cover before takeover is
// credible. Listed rather than counted, so an unvisited one is named in a
// report instead of appearing as a smaller number than expected.
enum class SceneClass : std::uint32_t
{
    Interior = 0,
    Exterior = 1,
    DenseSettlement = 2,
    Combat = 3,
    FirstPerson = 4,
    ThirdPerson = 5,
    Terrain = 6,
    Water = 7,
    Weather = 8,
    Loading = 9,
    PipBoy = 10,
    Vats = 11,
    Dialogue = 12,
    Map = 13,
    Terminal = 14,
    PowerArmor = 15,
    Count = 16,
};

struct ArmingRules
{
    // A backend that has fallen this far behind is producing images for a
    // camera that has moved on. Presenting them is worse than not taking
    // over, because the lag reads as input latency rather than as a renderer
    // that is behind.
    std::uint32_t maximumBackendLagFrames{2};
    // After a fault, this many consecutive clean frames before arming again.
    // Re-arming immediately produces a fault every frame and a picture that
    // alternates, which is harder to diagnose than staying off.
    std::uint32_t cleanFramesBeforeRearm{60};
};

// Everything the arming decision is allowed to look at, gathered before the
// decision rather than sampled during it. Sampling during the decision is how
// two consumers in the same frame end up disagreeing about whether the frame
// was taken over.
struct FrameEvidence
{
    RendererMode mode{RendererMode::Disabled};
    std::uint64_t buildSignature{};
    std::uint64_t expectedBuildSignature{};
    std::uint32_t unknownWorldWriters{};
    std::uint32_t unknownGeometry{};
    std::uint32_t unknownMaterials{};
    std::uint32_t incompleteResources{};
    std::uint32_t backendLagFrames{};
    bool backendLoaded{};
    bool capabilitiesSatisfied{};
    bool fenceTimedOut{};
    bool backendFaulted{};
    bool loadingTransition{};
    bool hooksInstalled{};
    bool workInFlight{};
};

// The decision, frozen. It carries every piece of evidence that produced it
// and the single frame it is valid for, so a consumer cannot re-derive a
// different answer and cannot carry a stale yes into the next frame.
struct TakeoverPermit
{
    std::uint64_t frameIndex{};
    // The first frame this permit no longer grants. Always frameIndex + 1: a
    // permit is for one frame and expires whether or not anyone notices.
    std::uint64_t expiryFrame{};
    std::uint64_t buildSignature{};
    std::uint32_t denied{DenyNone};
    bool granted{};

    [[nodiscard]] bool Grants(const std::uint64_t frame) const noexcept
    {
        return granted && frame >= frameIndex && frame < expiryFrame;
    }
};

struct FrameOutcome
{
    FramePresentation presentation{FramePresentation::Vanilla};
    // Which frame's image reached the display. Zero when nothing was
    // presented.
    std::uint64_t presentedFrame{};
    FaultKind fault{FaultKind::None};
    bool worldSuppressed{};

    // The property the whole phase exists to guarantee. A suppressed world
    // can never be completed by vanilla, and an unsuppressed one can never be
    // completed by Vulkan; anything else is a frame drawn half by each.
    [[nodiscard]] bool IsWholeFrame() const noexcept
    {
        return worldSuppressed
            ? presentation != FramePresentation::Vanilla
            : presentation == FramePresentation::Vanilla;
    }
};

// The arming decision, as a pure function of the evidence. Separate from the
// controller so the predicate can be tested without a frame history and so
// the controller has exactly one place that decides.
[[nodiscard]] TakeoverPermit EvaluatePermit(
    const FrameEvidence& evidence,
    std::uint64_t frameIndex,
    const ArmingRules& rules) noexcept;

// Whether vanilla can still finish the frame after a fault at this phase.
// False from the moment the world submission is suppressed: those draws are
// not re-issuable, the engine has already walked past them.
[[nodiscard]] bool VanillaRestorableThisFrame(FramePhase phase) noexcept;

class TakeoverController
{
public:
    TakeoverController() noexcept = default;
    explicit TakeoverController(const ArmingRules& rules) noexcept;

    // The one decision, taken at Renderer::Begin. Calling it again for the
    // same frame returns the same permit no matter what the evidence says by
    // then, which is what makes the decision per-frame atomic rather than
    // merely computed once by convention.
    [[nodiscard]] TakeoverPermit BeginFrame(
        std::uint64_t frameIndex,
        const FrameEvidence& evidence) noexcept;

    // A fault at a phase of the current frame. Returns the whole frame that
    // can still be delivered, and latches out of takeover for the frames that
    // follow.
    [[nodiscard]] FrameOutcome ReportFault(
        FaultKind kind,
        FramePhase phase) noexcept;

    // No fault. Records the output as the last good one when it completed.
    [[nodiscard]] FrameOutcome CompleteFrame(bool vulkanOutputComplete) noexcept;

    void NotifyLifecycle(LifecycleEvent event) noexcept;

    [[nodiscard]] bool HasLastGood() const noexcept;
    [[nodiscard]] std::uint64_t LastGoodFrame() const noexcept;
    [[nodiscard]] std::uint32_t RecoveryFramesRemaining() const noexcept;
    [[nodiscard]] const TakeoverPermit& CurrentPermit() const noexcept;

private:
    ArmingRules rules_{};
    TakeoverPermit permit_{};
    std::uint64_t lastGoodFrame_{};
    std::uint64_t currentFrame_{};
    std::uint32_t recoveryFrames_{};
    bool hasCurrentFrame_{};
    bool hasLastGood_{};
};

// The live gate, machine-checkable rather than prose. A class is covered only
// when it has been observed with no unknown world-target writer and no
// missing visible class; anything else leaves it outstanding and named.
class WorldCaptureMatrix
{
public:
    void Record(
        SceneClass sceneClass,
        std::uint32_t unknownWorldWriters,
        std::uint32_t missingVisibleClasses) noexcept;

    [[nodiscard]] bool Covered(SceneClass sceneClass) const noexcept;
    [[nodiscard]] bool Complete() const noexcept;
    [[nodiscard]] std::uint32_t Outstanding() const noexcept;
    [[nodiscard]] SceneClass FirstOutstanding() const noexcept;

private:
    struct Cell
    {
        std::uint32_t unknownWorldWriters{};
        std::uint32_t missingVisibleClasses{};
        bool observed{};
    };

    std::array<Cell, static_cast<std::size_t>(SceneClass::Count)> cells_{};
};

[[nodiscard]] const char* ToString(FaultKind kind) noexcept;
[[nodiscard]] const char* ToString(FramePresentation presentation) noexcept;
[[nodiscard]] const char* ToString(SceneClass sceneClass) noexcept;

// The set bits of a deny mask, one name per call site, for the log line that
// actually gets read when takeover does not arm.
[[nodiscard]] const char* DenyReasonName(std::uint32_t singleBit) noexcept;

}
