#include "renderer_core/EngineImageSpace.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using namespace vf::renderer;

namespace {

// A complete ledger: every image-space effect vanilla can dispatch, mapped.
// Completeness is the fixture's job because it is the property most easily
// lost, and a partial ledger reports zero unknowns for the wrong reason.
std::vector<imagespace::LedgerEntry> CompleteLedger()
{
    std::vector<imagespace::LedgerEntry> entries;
    for (std::uint32_t index = 0;
        index < static_cast<std::uint32_t>(imagespace::EffectId::Count);
        ++index) {
        imagespace::LedgerEntry entry{};
        entry.id = static_cast<imagespace::EffectId>(index);
        entry.disposition = imagespace::Disposition::Suppressed;
        entries.push_back(entry);
    }
    return entries;
}

imagespace::DepthContract Depth()
{
    imagespace::DepthContract contract{};
    contract.format = 45;
    contract.sampleCount = 1;
    contract.width = 1920;
    contract.height = 1080;
    contract.reversedZ = true;
    return contract;
}

}

TEST_CASE("P26_an_unmapped_image_space_dispatch_blocks_suppression",
    "[phase26][imagespace]")
{
    imagespace::SuppressionLedger ledger{};
    auto entries = CompleteLedger();
    REQUIRE(imagespace::ValidateLedger(
        entries, imagespace::kLedgerVersion, ledger) ==
        imagespace::ImageSpaceError::None);
    CHECK(ledger.MaySuppress());
    CHECK(ledger.suppressed ==
        static_cast<std::uint32_t>(imagespace::EffectId::Count));

    // A dispatch nobody classified writes somewhere nobody accounted for.
    // "Probably equivalent" is not a class, so it is not treated as one.
    entries[static_cast<std::size_t>(imagespace::EffectId::RadialBlur)]
        .disposition = imagespace::Disposition::Unknown;
    imagespace::SuppressionLedger blocked{};
    CHECK(imagespace::ValidateLedger(
        entries, imagespace::kLedgerVersion, blocked) ==
        imagespace::ImageSpaceError::UnknownDispatch);
    CHECK_FALSE(blocked.MaySuppress());
    CHECK(blocked.unknown == 1);

    // An effect nobody listed is an effect nobody decided about, and a partial
    // ledger reports no unknowns simply because they were never enumerated.
    auto partial = CompleteLedger();
    partial.pop_back();
    imagespace::SuppressionLedger incomplete{};
    CHECK(imagespace::ValidateLedger(
        partial, imagespace::kLedgerVersion, incomplete) ==
        imagespace::ImageSpaceError::IncompleteLedger);

    // Listing one effect twice is how a ledger reaches the right count with
    // the wrong contents.
    auto duplicated = CompleteLedger();
    duplicated.back().id = imagespace::EffectId::Bloom;
    imagespace::SuppressionLedger repeated{};
    CHECK(imagespace::ValidateLedger(
        duplicated, imagespace::kLedgerVersion, repeated) ==
        imagespace::ImageSpaceError::DuplicateEntry);

    // A retained effect must name its owner, for the same reason a retained
    // draw must: one without is indistinguishable from something forgotten.
    auto retained = CompleteLedger();
    retained[static_cast<std::size_t>(imagespace::EffectId::LensFlare)]
        .disposition = imagespace::Disposition::Retained;
    imagespace::SuppressionLedger anonymous{};
    CHECK(imagespace::ValidateLedger(
        retained, imagespace::kLedgerVersion, anonymous) ==
        imagespace::ImageSpaceError::RetainedWithoutOwner);
    retained[static_cast<std::size_t>(imagespace::EffectId::LensFlare)]
        .ownerId = 0x2600'0000'0000'0001ull;
    imagespace::SuppressionLedger named{};
    CHECK(imagespace::ValidateLedger(
        retained, imagespace::kLedgerVersion, named) ==
        imagespace::ImageSpaceError::None);
    CHECK(named.retained == 1);
    CHECK(named.MaySuppress());

    // The ledger is a versioned compatibility contract. A newer one describes
    // frames this build cannot be compared against, and accepting it silently
    // attributes the difference to the renderer.
    imagespace::SuppressionLedger future{};
    CHECK(imagespace::ValidateLedger(
        CompleteLedger(), imagespace::kLedgerVersion + 1, future) ==
        imagespace::ImageSpaceError::UnsupportedLedgerVersion);

    CHECK(imagespace::ClassifyDispatch(imagespace::EffectId::Bloom,
        CompleteLedger()) == imagespace::Disposition::Suppressed);
    const std::array<imagespace::LedgerEntry, 0> nothing{};
    CHECK(imagespace::ClassifyDispatch(imagespace::EffectId::Bloom, nothing) ==
        imagespace::Disposition::Unknown);
}

TEST_CASE("P26_a_borrowed_target_is_returned_in_the_frame_that_took_it",
    "[phase26][imagespace]")
{
    imagespace::BorrowLedger ledger{};
    CHECK(ledger.Acquire(0x11, 5, false) ==
        imagespace::ImageSpaceError::None);
    CHECK(ledger.Outstanding() == 1);

    // Acquiring twice means two owners believe they hold it, and the second
    // one's return releases it while the first is still using it.
    CHECK(ledger.Acquire(0x11, 5, false) ==
        imagespace::ImageSpaceError::DoubleAcquire);

    // Writing to a target borrowed read-only changes what vanilla draws next,
    // and the damage surfaces somewhere unrelated to this renderer.
    CHECK(ledger.ValidateWrite(0x11) ==
        imagespace::ImageSpaceError::BorrowedWrite);

    CHECK(ledger.Return(0x11, 5) == imagespace::ImageSpaceError::None);
    CHECK(ledger.Outstanding() == 0);
    CHECK(ledger.EndFrame(5) == imagespace::ImageSpaceError::None);
    CHECK_FALSE(ledger.BlocksNextFrame());

    // Returning something never acquired means the identity being tracked is
    // not the identity being used.
    CHECK(ledger.Return(0x22, 5) ==
        imagespace::ImageSpaceError::ReturnWithoutAcquire);

    // The engine reuses its targets next frame. A borrow returned a frame late
    // releases a surface something else has already started drawing into.
    imagespace::BorrowLedger late{};
    REQUIRE(late.Acquire(0x33, 7, true) == imagespace::ImageSpaceError::None);
    CHECK(late.Return(0x33, 8) ==
        imagespace::ImageSpaceError::CrossFrameReturn);

    // An outstanding borrow at the frame boundary blocks the next frame rather
    // than being tidied away. Tidying it hides a leak that only appears as
    // corruption under load, on somebody else's machine.
    imagespace::BorrowLedger leaked{};
    REQUIRE(leaked.Acquire(0x44, 9, true) == imagespace::ImageSpaceError::None);
    CHECK(leaked.EndFrame(9) == imagespace::ImageSpaceError::LeakedBorrow);
    CHECK(leaked.BlocksNextFrame());
    CHECK(leaked.Outstanding() == 1);

    // A writable borrow may be written; that is what the flag is for.
    imagespace::BorrowLedger writable{};
    REQUIRE(writable.Acquire(0x55, 1, true) ==
        imagespace::ImageSpaceError::None);
    CHECK(writable.ValidateWrite(0x55) == imagespace::ImageSpaceError::None);
    CHECK(writable.ValidateWrite(0x56) ==
        imagespace::ImageSpaceError::ReturnWithoutAcquire);
}

TEST_CASE("P26_depth_handoff_checks_the_convention_not_just_the_format",
    "[phase26][imagespace]")
{
    CHECK(imagespace::ValidateDepthHandoff(Depth(), Depth()) ==
        imagespace::ImageSpaceError::None);

    auto other = Depth();
    other.format = 20;
    CHECK(imagespace::ValidateDepthHandoff(Depth(), other) ==
        imagespace::ImageSpaceError::DepthFormatMismatch);

    auto scaled = Depth();
    scaled.width = 1280;
    CHECK(imagespace::ValidateDepthHandoff(Depth(), scaled) ==
        imagespace::ImageSpaceError::DepthExtentMismatch);

    auto multisampled = Depth();
    multisampled.sampleCount = 4;
    CHECK(imagespace::ValidateDepthHandoff(Depth(), multisampled) ==
        imagespace::ImageSpaceError::DepthFormatMismatch);

    // The one that does not announce itself. Depth written reversed and read
    // as standard makes fog, depth of field and decals all wrong by an amount
    // that reads as a bias setting rather than as an inverted range, so it is
    // refused rather than converted quietly.
    auto standard = Depth();
    standard.reversedZ = false;
    CHECK(imagespace::ValidateDepthHandoff(Depth(), standard) ==
        imagespace::ImageSpaceError::DepthConventionMismatch);

    // Stencil the readers need but the producer did not write is the same kind
    // of failure: the reads succeed and return whatever was there.
    auto needsStencil = Depth();
    needsStencil.stencilRequired = true;
    CHECK(imagespace::ValidateDepthHandoff(Depth(), needsStencil) ==
        imagespace::ImageSpaceError::DepthFormatMismatch);
}

TEST_CASE("P26_exposure_and_history_survive_the_handoff_or_are_reset",
    "[phase26][imagespace]")
{
    // Adopting vanilla's exposure rather than restarting at a default. The
    // default makes the scene visibly brighten or darken the moment takeover
    // engages, which reads as takeover being wrong.
    imagespace::ExposureHandoff handoff{};
    handoff.vanillaExposure = 0.35f;
    handoff.vulkanDefault = 1.0f;
    handoff.vanillaEstablished = true;
    CHECK(imagespace::AdoptExposure(handoff) == Catch::Approx(0.35f));

    // With nothing established there is nothing to adopt.
    handoff.vanillaEstablished = false;
    CHECK(imagespace::AdoptExposure(handoff) == Catch::Approx(1.0f));

    // A non-finite reading is not adopted either: one bad frame would
    // otherwise pin exposure for as long as the adaptation takes to recover.
    handoff.vanillaEstablished = true;
    handoff.vanillaExposure =
        std::numeric_limits<float>::quiet_NaN();
    CHECK(imagespace::AdoptExposure(handoff) == Catch::Approx(1.0f));

    imagespace::HistoryKey key{};
    key.width = 1920;
    key.height = 1080;
    key.ledgerVersion = imagespace::kLedgerVersion;
    key.takeoverEpoch = 4;
    CHECK(imagespace::HistorySurvives(key, key));

    // Every one of these changes what the stored history describes. Reusing it
    // produces ghosting that looks like a resolve bug rather than like history
    // from a different image.
    auto resized = key;
    resized.width = 2560;
    CHECK_FALSE(imagespace::HistorySurvives(key, resized));

    auto reledgered = key;
    reledgered.ledgerVersion = imagespace::kLedgerVersion + 1;
    CHECK_FALSE(imagespace::HistorySurvives(key, reledgered));

    auto rescaled = key;
    rescaled.upscaleNumerator = 2;
    rescaled.upscaleDenominator = 3;
    CHECK_FALSE(imagespace::HistorySurvives(key, rescaled));

    // A takeover that stopped and started again produced its history under a
    // different renderer.
    auto rearmed = key;
    rearmed.takeoverEpoch = 5;
    CHECK_FALSE(imagespace::HistorySurvives(key, rearmed));

    CHECK(imagespace::ResizeRequiresReacquire(1920, 1080, 2560, 1440));
    CHECK_FALSE(imagespace::ResizeRequiresReacquire(1920, 1080, 1920, 1080));
    CHECK(imagespace::ResizeRequiresReacquire(0, 0, 1920, 1080));
}

TEST_CASE("P26_a_capture_takes_what_the_user_saw", "[phase26][imagespace]")
{
    // Capturing the pre-UI world silently drops the HUD, and every screenshot
    // then arrives as a bug report about missing user interface.
    CHECK(imagespace::SelectCaptureSource(true) ==
        imagespace::CaptureSource::FinalComposite);
    CHECK(imagespace::SelectCaptureSource(false) ==
        imagespace::CaptureSource::PreUiWorld);

    // And the composite has to have happened. Reading the target early yields
    // whatever the bridge had written so far, which is a partial frame saved
    // to disk and looks like a renderer fault.
    CHECK(imagespace::ValidateCapture(
        imagespace::CaptureSource::FinalComposite, true) ==
        imagespace::ImageSpaceError::None);
    CHECK(imagespace::ValidateCapture(
        imagespace::CaptureSource::FinalComposite, false) ==
        imagespace::ImageSpaceError::CaptureBeforeComposite);

    // The pre-UI world is readable before the composite: that is the only
    // moment it exists in isolation.
    CHECK(imagespace::ValidateCapture(
        imagespace::CaptureSource::PreUiWorld, false) ==
        imagespace::ImageSpaceError::None);
}

TEST_CASE("P26_no_world_or_image_space_work_survives_in_d3d",
    "[phase26][imagespace]")
{
    // Named operations only. A whitelist by category would readmit exactly the
    // draws this phase exists to remove, because a world draw and a UI draw
    // are the same category of call.
    CHECK(imagespace::Whitelisted(imagespace::D3dOperation::UiDraw));
    CHECK(imagespace::Whitelisted(imagespace::D3dOperation::VideoBlit));
    CHECK(imagespace::Whitelisted(imagespace::D3dOperation::MiddlewareDraw));
    CHECK(imagespace::Whitelisted(imagespace::D3dOperation::BridgeComposite));
    CHECK(imagespace::Whitelisted(imagespace::D3dOperation::SwapChainPresent));
    CHECK_FALSE(imagespace::Whitelisted(imagespace::D3dOperation::WorldDraw));
    CHECK_FALSE(imagespace::Whitelisted(
        imagespace::D3dOperation::ImageSpaceDispatch));

    const std::array<imagespace::D3dOperationRecord, 3> clean{{
        {imagespace::D3dOperation::UiDraw, 412},
        {imagespace::D3dOperation::BridgeComposite, 1},
        {imagespace::D3dOperation::SwapChainPresent, 1}}};
    CHECK(imagespace::ValidateResidualD3d(clean) ==
        imagespace::ImageSpaceError::None);

    // A count of zero is not a violation: an operation that did not happen
    // this frame still appears in a per-frame tally.
    const std::array<imagespace::D3dOperationRecord, 2> absent{{
        {imagespace::D3dOperation::WorldDraw, 0},
        {imagespace::D3dOperation::ImageSpaceDispatch, 0}}};
    CHECK(imagespace::ValidateResidualD3d(absent) ==
        imagespace::ImageSpaceError::None);

    const std::array<imagespace::D3dOperationRecord, 2> residual{{
        {imagespace::D3dOperation::UiDraw, 412},
        {imagespace::D3dOperation::WorldDraw, 1}}};
    CHECK(imagespace::ValidateResidualD3d(residual) ==
        imagespace::ImageSpaceError::ResidualWorldDraw);

    const std::array<imagespace::D3dOperationRecord, 1> dispatch{{
        {imagespace::D3dOperation::ImageSpaceDispatch, 3}}};
    CHECK(imagespace::ValidateResidualD3d(dispatch) ==
        imagespace::ImageSpaceError::ResidualWorldDraw);
}
