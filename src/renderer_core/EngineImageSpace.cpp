#include "renderer_core/EngineImageSpace.h"

#include <cmath>

namespace vf::renderer::imagespace {

ImageSpaceError ValidateLedger(
    const std::span<const LedgerEntry> entries,
    const std::uint32_t version,
    SuppressionLedger& ledger) noexcept
{
    ledger = SuppressionLedger{};
    ledger.version = version;

    // A newer ledger describes a frame this build cannot be compared against.
    // Accepting it silently attributes the difference to the renderer instead
    // of to the mapping that changed.
    if (version > kLedgerVersion) {
        return ImageSpaceError::UnsupportedLedgerVersion;
    }

    constexpr auto kCount = static_cast<std::size_t>(EffectId::Count);
    std::array<bool, kCount> seen{};

    for (const auto& entry : entries) {
        const auto index = static_cast<std::size_t>(entry.id);
        if (index >= kCount) return ImageSpaceError::UnknownDispatch;
        // Listing one effect twice is how a ledger reaches the right count
        // with the wrong contents, and the count is what gets checked.
        if (seen[index]) return ImageSpaceError::DuplicateEntry;
        seen[index] = true;

        switch (entry.disposition) {
        case Disposition::Suppressed:
            ++ledger.suppressed;
            break;
        case Disposition::Retained:
            if (entry.ownerId == 0) {
                return ImageSpaceError::RetainedWithoutOwner;
            }
            ++ledger.retained;
            break;
        case Disposition::Unsupported:
            ++ledger.unsupported;
            break;
        case Disposition::Unknown:
            ++ledger.unknown;
            break;
        }
    }

    // Completeness before unknowns. An effect nobody listed is an effect
    // nobody decided about, and a partial ledger reports zero unknowns
    // precisely because the unmapped ones were never enumerated.
    for (const auto observed : seen) {
        if (!observed) return ImageSpaceError::IncompleteLedger;
    }
    if (ledger.unknown != 0) return ImageSpaceError::UnknownDispatch;
    return ImageSpaceError::None;
}

Disposition ClassifyDispatch(
    const EffectId id,
    const std::span<const LedgerEntry> entries) noexcept
{
    for (const auto& entry : entries) {
        if (entry.id == id) return entry.disposition;
    }
    // Not mapped. "Probably equivalent" is not a class, so an unlisted
    // dispatch is unknown rather than assumed harmless.
    return Disposition::Unknown;
}

BorrowRecord* BorrowLedger::Find(const std::uint64_t resourceId) noexcept
{
    for (std::size_t index = 0; index < count_; ++index) {
        if (records_[index].resourceId == resourceId) return &records_[index];
    }
    return nullptr;
}

const BorrowRecord* BorrowLedger::Find(
    const std::uint64_t resourceId) const noexcept
{
    for (std::size_t index = 0; index < count_; ++index) {
        if (records_[index].resourceId == resourceId) return &records_[index];
    }
    return nullptr;
}

ImageSpaceError BorrowLedger::Acquire(
    const std::uint64_t resourceId,
    const std::uint64_t frameIndex,
    const bool writable) noexcept
{
    if (auto* existing = Find(resourceId); existing != nullptr) {
        // Two owners each believing they hold it. The second one's return
        // releases the target while the first is still drawing into it.
        if (existing->state == BorrowState::Acquired) {
            return ImageSpaceError::DoubleAcquire;
        }
        existing->state = BorrowState::Acquired;
        existing->frameAcquired = frameIndex;
        existing->writable = writable;
        return ImageSpaceError::None;
    }
    if (count_ >= kCapacity) {
        // Out of slots means borrows are outliving their frames somewhere;
        // treating it as a leak is the honest reading.
        blocked_ = true;
        return ImageSpaceError::LeakedBorrow;
    }
    auto& record = records_[count_++];
    record.resourceId = resourceId;
    record.frameAcquired = frameIndex;
    record.state = BorrowState::Acquired;
    record.writable = writable;
    return ImageSpaceError::None;
}

ImageSpaceError BorrowLedger::Return(
    const std::uint64_t resourceId,
    const std::uint64_t frameIndex) noexcept
{
    auto* record = Find(resourceId);
    if (record == nullptr || record->state != BorrowState::Acquired) {
        // The identity being tracked is not the identity being used.
        return ImageSpaceError::ReturnWithoutAcquire;
    }
    // The engine reuses its targets next frame. A borrow returned a frame late
    // releases a surface something else has already started drawing into.
    if (record->frameAcquired != frameIndex) {
        return ImageSpaceError::CrossFrameReturn;
    }
    record->state = BorrowState::Returned;
    return ImageSpaceError::None;
}

ImageSpaceError BorrowLedger::ValidateWrite(
    const std::uint64_t resourceId) const noexcept
{
    const auto* record = Find(resourceId);
    if (record == nullptr || record->state != BorrowState::Acquired) {
        return ImageSpaceError::ReturnWithoutAcquire;
    }
    // Writing to a read-only borrow changes what vanilla draws next, and the
    // damage surfaces somewhere unrelated to this renderer.
    if (!record->writable) return ImageSpaceError::BorrowedWrite;
    return ImageSpaceError::None;
}

ImageSpaceError BorrowLedger::EndFrame(const std::uint64_t frameIndex) noexcept
{
    static_cast<void>(frameIndex);
    auto leaked = false;
    for (std::size_t index = 0; index < count_; ++index) {
        if (records_[index].state == BorrowState::Acquired) leaked = true;
    }
    if (!leaked) return ImageSpaceError::None;
    // Blocked rather than tidied. Releasing it here hides a leak that only
    // shows itself as corruption under load, on somebody else's machine.
    blocked_ = true;
    return ImageSpaceError::LeakedBorrow;
}

std::uint32_t BorrowLedger::Outstanding() const noexcept
{
    std::uint32_t outstanding = 0;
    for (std::size_t index = 0; index < count_; ++index) {
        if (records_[index].state == BorrowState::Acquired) ++outstanding;
    }
    return outstanding;
}

bool BorrowLedger::BlocksNextFrame() const noexcept
{
    return blocked_;
}

void BorrowLedger::Reset() noexcept
{
    records_ = {};
    count_ = 0;
    blocked_ = false;
}

ImageSpaceError ValidateDepthHandoff(
    const DepthContract& produced,
    const DepthContract& expected) noexcept
{
    if (produced.format != expected.format ||
        produced.sampleCount != expected.sampleCount) {
        return ImageSpaceError::DepthFormatMismatch;
    }
    // Readers that test stencil against depth this renderer wrote without one
    // still succeed; they simply read whatever was there.
    if (expected.stencilRequired && !produced.stencilRequired) {
        return ImageSpaceError::DepthFormatMismatch;
    }
    if (produced.width != expected.width ||
        produced.height != expected.height) {
        return ImageSpaceError::DepthExtentMismatch;
    }
    // The one that does not announce itself. Depth written reversed and read
    // as standard makes fog, depth of field and decals all wrong by an amount
    // that reads as a bias setting rather than as an inverted range, so it is
    // refused rather than converted quietly.
    if (produced.reversedZ != expected.reversedZ) {
        return ImageSpaceError::DepthConventionMismatch;
    }
    return ImageSpaceError::None;
}

float AdoptExposure(const ExposureHandoff& handoff) noexcept
{
    // Nothing established, or a reading that is not a number. One bad frame
    // adopted would pin exposure for as long as the adaptation takes to climb
    // back, which looks like the takeover having broken exposure.
    if (!handoff.vanillaEstablished ||
        !std::isfinite(handoff.vanillaExposure)) {
        return handoff.vulkanDefault;
    }
    // Otherwise carry vanilla's value across. Restarting at a default makes
    // the scene visibly brighten or darken the moment takeover engages.
    return handoff.vanillaExposure;
}

bool HistorySurvives(
    const HistoryKey& previous,
    const HistoryKey& current) noexcept
{
    // Any of these changes what the stored history describes. Reusing it
    // produces ghosting that reads as a resolve bug rather than as history
    // belonging to a different image.
    return previous.width == current.width &&
        previous.height == current.height &&
        previous.ledgerVersion == current.ledgerVersion &&
        previous.upscaleNumerator == current.upscaleNumerator &&
        previous.upscaleDenominator == current.upscaleDenominator &&
        previous.takeoverEpoch == current.takeoverEpoch;
}

CaptureSource SelectCaptureSource(const bool includeUi) noexcept
{
    // A screenshot captures what the user saw. Taking the pre-UI world
    // silently drops the HUD, and every screenshot then arrives as a bug
    // report about missing user interface.
    return includeUi ? CaptureSource::FinalComposite
        : CaptureSource::PreUiWorld;
}

ImageSpaceError ValidateCapture(
    const CaptureSource source,
    const bool compositeComplete) noexcept
{
    // Reading the final target early yields whatever the bridge had written so
    // far: a partial frame saved to disk, which looks like a renderer fault.
    if (source == CaptureSource::FinalComposite && !compositeComplete) {
        return ImageSpaceError::CaptureBeforeComposite;
    }
    return ImageSpaceError::None;
}

bool Whitelisted(const D3dOperation operation) noexcept
{
    // Named operations only. A whitelist by category would readmit exactly the
    // draws this phase exists to remove, because a world draw and a UI draw
    // are the same category of call.
    switch (operation) {
    case D3dOperation::UiDraw:
    case D3dOperation::VideoBlit:
    case D3dOperation::MiddlewareDraw:
    case D3dOperation::BridgeComposite:
    case D3dOperation::SwapChainPresent:
        return true;
    case D3dOperation::WorldDraw:
    case D3dOperation::ImageSpaceDispatch:
    case D3dOperation::Count:
        return false;
    }
    return false;
}

ImageSpaceError ValidateResidualD3d(
    const std::span<const D3dOperationRecord> records) noexcept
{
    for (const auto& record : records) {
        // A zero count is not a violation: an operation that did not happen
        // this frame still occupies a row in a per-frame tally.
        if (record.count == 0) continue;
        if (!Whitelisted(record.operation)) {
            return ImageSpaceError::ResidualWorldDraw;
        }
    }
    return ImageSpaceError::None;
}

bool ResizeRequiresReacquire(
    const std::uint32_t previousWidth,
    const std::uint32_t previousHeight,
    const std::uint32_t width,
    const std::uint32_t height) noexcept
{
    // Nothing was ever acquired at zero extent, so there is nothing to keep.
    if (previousWidth == 0 || previousHeight == 0) return true;
    return previousWidth != width || previousHeight != height;
}

const char* ToString(const ImageSpaceError error) noexcept
{
    switch (error) {
    case ImageSpaceError::None: return "none";
    case ImageSpaceError::UnknownDispatch: return "unknown dispatch";
    case ImageSpaceError::IncompleteLedger: return "incomplete ledger";
    case ImageSpaceError::DuplicateEntry: return "duplicate entry";
    case ImageSpaceError::UnsupportedLedgerVersion:
        return "unsupported ledger version";
    case ImageSpaceError::RetainedWithoutOwner:
        return "retained without owner";
    case ImageSpaceError::DoubleAcquire: return "double acquire";
    case ImageSpaceError::ReturnWithoutAcquire:
        return "return without acquire";
    case ImageSpaceError::CrossFrameReturn: return "cross frame return";
    case ImageSpaceError::LeakedBorrow: return "leaked borrow";
    case ImageSpaceError::BorrowedWrite: return "borrowed write";
    case ImageSpaceError::DepthFormatMismatch: return "depth format mismatch";
    case ImageSpaceError::DepthExtentMismatch: return "depth extent mismatch";
    case ImageSpaceError::DepthConventionMismatch:
        return "depth convention mismatch";
    case ImageSpaceError::ResidualWorldDraw: return "residual world draw";
    case ImageSpaceError::CaptureBeforeComposite:
        return "capture before composite";
    }
    return "unknown";
}

const char* ToString(const EffectId id) noexcept
{
    switch (id) {
    case EffectId::Downsample: return "downsample";
    case EffectId::AmbientOcclusion: return "ambient occlusion";
    case EffectId::Volumetrics: return "volumetrics";
    case EffectId::DepthOfField: return "depth of field";
    case EffectId::MotionBlur: return "motion blur";
    case EffectId::RadialBlur: return "radial blur";
    case EffectId::Bloom: return "bloom";
    case EffectId::Exposure: return "exposure";
    case EffectId::ToneMap: return "tone map";
    case EffectId::ColorGrading: return "colour grading";
    case EffectId::FilmGrain: return "film grain";
    case EffectId::Vignette: return "vignette";
    case EffectId::LensFlare: return "lens flare";
    case EffectId::Refraction: return "refraction";
    case EffectId::Underwater: return "underwater";
    case EffectId::Fxaa: return "fxaa";
    case EffectId::Taa: return "taa";
    case EffectId::Upscale: return "upscale";
    case EffectId::Cinematic: return "cinematic";
    case EffectId::Count: return "count";
    }
    return "unknown";
}

const char* ToString(const Disposition disposition) noexcept
{
    switch (disposition) {
    case Disposition::Suppressed: return "suppressed";
    case Disposition::Retained: return "retained";
    case Disposition::Unsupported: return "unsupported";
    case Disposition::Unknown: return "unknown";
    }
    return "unknown";
}

}
