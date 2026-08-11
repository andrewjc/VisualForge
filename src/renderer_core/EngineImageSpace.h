#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vf::renderer::imagespace {

// The ledger is versioned because it is a compatibility contract, not a
// settings block: a build that maps one more effect is a build whose frames
// are not comparable with the previous one, and a difference view that does
// not say so attributes the change to the renderer.
inline constexpr std::uint32_t kLedgerVersion = 1u;

// Vanilla's image-space work, in schedule order. Listed rather than counted,
// because an effect nobody enumerated is an effect nobody decided about, and
// that is the same failure as an unknown dispatch with none of the visibility.
enum class EffectId : std::uint32_t
{
    Downsample = 0,
    AmbientOcclusion = 1,
    Volumetrics = 2,
    DepthOfField = 3,
    MotionBlur = 4,
    RadialBlur = 5,
    Bloom = 6,
    Exposure = 7,
    ToneMap = 8,
    ColorGrading = 9,
    FilmGrain = 10,
    Vignette = 11,
    LensFlare = 12,
    Refraction = 13,
    Underwater = 14,
    Fxaa = 15,
    Taa = 16,
    Upscale = 17,
    Cinematic = 18,
    Count = 19,
};

// What happens to a vanilla image-space dispatch under takeover.
enum class Disposition : std::uint8_t
{
    // Vulkan produces the equivalent and vanilla's dispatch is suppressed.
    Suppressed = 0,
    // Vanilla keeps it, deliberately, with a named owner.
    Retained = 1,
    // Recognised and out of scope. A decision, which is not the same thing as
    // a gap, and the two need different responses.
    Unsupported = 2,
    // Not mapped. Blocks suppression: a dispatch nobody classified writes
    // somewhere nobody accounted for.
    Unknown = 3,
};

enum class ImageSpaceError : std::uint8_t
{
    None = 0,
    UnknownDispatch,
    IncompleteLedger,
    DuplicateEntry,
    UnsupportedLedgerVersion,
    RetainedWithoutOwner,
    DoubleAcquire,
    ReturnWithoutAcquire,
    CrossFrameReturn,
    LeakedBorrow,
    BorrowedWrite,
    DepthFormatMismatch,
    DepthExtentMismatch,
    DepthConventionMismatch,
    ResidualWorldDraw,
    CaptureBeforeComposite,
};

struct LedgerEntry
{
    EffectId id{};
    Disposition disposition{Disposition::Unknown};
    // Whoever keeps a retained effect must be nameable. A retained path with
    // no owner is indistinguishable from one that was forgotten.
    std::uint64_t ownerId{};
};

// The image-space ledger, complete and versioned. Completeness is the point:
// a partial ledger reports no unknowns simply because the unmapped effects
// were never listed.
struct SuppressionLedger
{
    std::uint32_t version{kLedgerVersion};
    std::uint32_t unknown{};
    std::uint32_t suppressed{};
    std::uint32_t retained{};
    std::uint32_t unsupported{};

    [[nodiscard]] bool MaySuppress() const noexcept
    {
        return unknown == 0;
    }
};

[[nodiscard]] ImageSpaceError ValidateLedger(
    std::span<const LedgerEntry> entries,
    std::uint32_t version,
    SuppressionLedger& ledger) noexcept;

// A dispatch this renderer has never seen. Classified as unknown rather than
// guessed at: "probably equivalent" is not a class.
[[nodiscard]] Disposition ClassifyDispatch(
    EffectId id,
    std::span<const LedgerEntry> entries) noexcept;

enum class BorrowState : std::uint8_t
{
    Unowned = 0,
    Acquired = 1,
    Returned = 2,
};

struct BorrowRecord
{
    std::uint64_t resourceId{};
    std::uint64_t frameAcquired{};
    BorrowState state{BorrowState::Unowned};
    bool writable{};
};

// Targets borrowed from the engine for one frame. The engine decides when it
// is done with them and reuses them next frame, so a borrow that outlives its
// frame hands this renderer a surface something else is already drawing into.
class BorrowLedger
{
public:
    [[nodiscard]] ImageSpaceError Acquire(
        std::uint64_t resourceId,
        std::uint64_t frameIndex,
        bool writable) noexcept;

    [[nodiscard]] ImageSpaceError Return(
        std::uint64_t resourceId,
        std::uint64_t frameIndex) noexcept;

    // Writing to a target borrowed read-only changes what vanilla draws next,
    // and the damage surfaces somewhere unrelated to this renderer.
    [[nodiscard]] ImageSpaceError ValidateWrite(
        std::uint64_t resourceId) const noexcept;

    // Any borrow still outstanding at the frame boundary. This blocks the next
    // frame rather than being tidied up, because tidying it up hides a leak
    // that only shows itself as corruption under load.
    [[nodiscard]] ImageSpaceError EndFrame(std::uint64_t frameIndex) noexcept;

    [[nodiscard]] std::uint32_t Outstanding() const noexcept;
    [[nodiscard]] bool BlocksNextFrame() const noexcept;
    void Reset() noexcept;

private:
    static constexpr std::size_t kCapacity = 16;

    [[nodiscard]] BorrowRecord* Find(std::uint64_t resourceId) noexcept;
    [[nodiscard]] const BorrowRecord* Find(
        std::uint64_t resourceId) const noexcept;

    std::array<BorrowRecord, kCapacity> records_{};
    std::size_t count_{};
    bool blocked_{};
};

struct DepthContract
{
    std::uint32_t format{};
    std::uint32_t sampleCount{1};
    std::uint32_t width{};
    std::uint32_t height{};
    // Reversed-Z is not a detail. Handing depth written one way to readers
    // that expect the other makes fog, depth of field and decals all wrong by
    // an amount that reads as a bias setting rather than as an inverted range.
    bool reversedZ{};
    bool stencilRequired{};
};

[[nodiscard]] ImageSpaceError ValidateDepthHandoff(
    const DepthContract& produced,
    const DepthContract& expected) noexcept;

struct ExposureHandoff
{
    float vanillaExposure{};
    float vulkanDefault{};
    bool vanillaEstablished{};
};

// Crossing from vanilla post to Vulkan post adopts the exposure vanilla had
// reached. Restarting at a default makes the scene visibly brighten or darken
// at the moment takeover engages, which reads as takeover being wrong rather
// than as an adaptation starting over.
[[nodiscard]] float AdoptExposure(const ExposureHandoff& handoff) noexcept;

// Everything a temporal history depends on. Any change makes the stored
// history describe a different image, and reusing it produces ghosting that
// looks like a resolve bug.
struct HistoryKey
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t ledgerVersion{};
    std::uint32_t upscaleNumerator{1};
    std::uint32_t upscaleDenominator{1};
    std::uint64_t takeoverEpoch{};
};

[[nodiscard]] bool HistorySurvives(
    const HistoryKey& previous,
    const HistoryKey& current) noexcept;

enum class CaptureSource : std::uint8_t
{
    PreUiWorld = 0,
    FinalComposite = 1,
};

// A screenshot captures what the user saw. Taking the pre-UI world silently
// drops the HUD, and every screenshot then arrives as a bug report about
// missing user interface.
[[nodiscard]] CaptureSource SelectCaptureSource(bool includeUi) noexcept;

[[nodiscard]] ImageSpaceError ValidateCapture(
    CaptureSource source,
    bool compositeComplete) noexcept;

// What D3D11 is still allowed to do once Vulkan owns world and image space.
// Named operations only: a whitelist by category would readmit the world
// draws this phase exists to remove.
enum class D3dOperation : std::uint32_t
{
    UiDraw = 0,
    VideoBlit = 1,
    MiddlewareDraw = 2,
    BridgeComposite = 3,
    SwapChainPresent = 4,
    WorldDraw = 5,
    ImageSpaceDispatch = 6,
    Count = 7,
};

[[nodiscard]] bool Whitelisted(D3dOperation operation) noexcept;

struct D3dOperationRecord
{
    D3dOperation operation{};
    std::uint32_t count{};
};

[[nodiscard]] ImageSpaceError ValidateResidualD3d(
    std::span<const D3dOperationRecord> records) noexcept;

[[nodiscard]] bool ResizeRequiresReacquire(
    std::uint32_t previousWidth,
    std::uint32_t previousHeight,
    std::uint32_t width,
    std::uint32_t height) noexcept;

[[nodiscard]] const char* ToString(ImageSpaceError error) noexcept;
[[nodiscard]] const char* ToString(EffectId id) noexcept;
[[nodiscard]] const char* ToString(Disposition disposition) noexcept;

}
