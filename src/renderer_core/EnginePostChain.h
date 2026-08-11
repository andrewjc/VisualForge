#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::post {

// An image-space effect the engine schedules. The order here is the order the
// chain runs in, and it is not arbitrary: a lookup table authored for display
// values applied before tone mapping reads a wildly wrong entry, and bloom
// gathered after tone mapping gathers from values that have already been
// compressed.
enum class EffectId : std::uint32_t
{
    Exposure = 0,
    Bloom = 1,
    Volumetrics = 2,
    MotionBlur = 3,
    TemporalResolve = 4,
    ToneMap = 5,
    ColorGradingLut = 6,
    Sharpen = 7,
    FinalTransform = 8,
    Count = 9,
};

// What this renderer does about an effect the engine asked for.
enum class Coverage : std::uint8_t
{
    // Implemented in Vulkan.
    Covered = 0,
    // Deliberately left to the vanilla chain, recorded so the decision is
    // visible rather than looking like an omission.
    Retained = 1,
    // Recognised and explicitly not supported.
    Unsupported = 2,
    // Not recognised at all. This one prevents arming: an effect that
    // disappears silently is a frame that is wrong in a way nobody sees.
    Unknown = 3,
};

enum class PostError : std::uint8_t
{
    None,
    UnknownEffect,
    OrderViolation,
    AliasOverlap,
    BorrowedWrite,
    InvalidExtent,
    NonFiniteSource,
    InvalidHistory,
};

// A transient image the chain uses. Two of them may share memory only when
// their live ranges do not overlap; aliasing overlapping ranges corrupts
// silently, and the corruption looks like a shader bug in whichever pass
// reads second.
struct TransientImage
{
    std::uint64_t resourceId{};
    std::uint32_t firstPass{};
    std::uint32_t lastPass{};
    std::uint32_t width{};
    std::uint32_t height{};
    // A borrowed image belongs to the engine. It may be read, never written:
    // writing to it changes what vanilla draws next and the damage appears
    // somewhere unrelated.
    bool borrowed{};
};

struct ExposureState
{
    float value{1.0f};
    bool established{};
};

struct ExposureRules
{
    // Per-second rates. Adaptation is asymmetric because eyes are: going from
    // dark to bright is fast, the reverse is slow, and one rate for both
    // reads as a lag in one direction.
    float brightenRate{3.0f};
    float darkenRate{1.0f};
    float minimum{0.03f};
    float maximum{8.0f};
};

struct BloomRules
{
    float threshold{1.0f};
    // The width over which a pixel starts contributing. A bare threshold
    // makes bloom pop on as a highlight crosses it, which reads as flicker on
    // any moving specular.
    float knee{0.5f};
    float intensity{0.05f};
};

// Jitter for temporal resolve. It must be deterministic, must cover the pixel
// evenly, and must restart when the target changes size: a sequence carried
// across a resize samples positions that no longer exist.
struct JitterState
{
    std::uint32_t index{};
    std::uint32_t width{};
    std::uint32_t height{};
};

enum class OutputFormat : std::uint8_t
{
    Srgb8 = 0,
    Rec2020Pq = 1,
    ScRgbLinear = 2,
};

struct EffectEntry
{
    EffectId id{};
    Coverage coverage{Coverage::Covered};
    bool enabled{true};
};

struct ChainLedger
{
    std::vector<EffectId> covered;
    std::vector<EffectId> retained;
    std::vector<EffectId> unsupported;
    std::uint32_t unknown{};
    // An unknown effect prevents the frame being armed at all. Disappearing
    // silently would leave a frame that is wrong in a way nobody sees.
    [[nodiscard]] bool MayArm() const noexcept { return unknown == 0; }
};

[[nodiscard]] ChainLedger ClassifyChain(
    std::span<const EffectEntry> entries) noexcept;

// The order effects must run in. Checked rather than assumed, because the
// consequences of two of these swapping are subtle enough to be mistaken for
// a shader bug.
[[nodiscard]] PostError ValidateOrder(
    std::span<const EffectEntry> entries) noexcept;

// Whether two transients may share memory.
[[nodiscard]] bool MayAlias(
    const TransientImage& a,
    const TransientImage& b) noexcept;

[[nodiscard]] PostError ValidateAliasing(
    std::span<const TransientImage> images,
    std::span<const std::array<std::uint64_t, 2>> aliasPairs) noexcept;

// A borrowed image may be read by any pass and written by none.
[[nodiscard]] PostError ValidateBorrowedUsage(
    const TransientImage& image,
    bool written) noexcept;

// Exposure adapts toward the target over time, and resets outright on a cut:
// adapting across a cut shows the previous scene's brightness for as long as
// the adaptation takes, which reads as the new scene being wrong.
[[nodiscard]] ExposureState AdaptExposure(
    const ExposureState& previous,
    float targetExposure,
    float deltaSeconds,
    bool resetHistory,
    const ExposureRules& rules) noexcept;

// The luminance bloom is thresholded against. Rec. 709, because which
// luminance is a rule rather than a detail: a single channel or an unweighted
// mean makes a saturated blue highlight bloom at a different level from a
// green one of the same brightness, which reads as a colour cast that appears
// only on bright edges.
[[nodiscard]] float Luminance(const std::array<float, 3>& colour) noexcept;

// How much of a pixel contributes to bloom.
[[nodiscard]] float BloomWeight(
    const BloomRules& rules,
    float luminance) noexcept;

// The next jitter offset, in pixels, within [-0.5, 0.5].
[[nodiscard]] JitterState AdvanceJitter(
    const JitterState& state,
    std::uint32_t width,
    std::uint32_t height) noexcept;
[[nodiscard]] std::array<float, 2> JitterOffset(
    const JitterState& state) noexcept;

// The convention, stated once and tested: a motion vector points from where
// a pixel is *now* to where it *was*. A sign error smears in the opposite
// direction, which looks like a different defect entirely and gets diagnosed
// as one.
[[nodiscard]] std::array<float, 2> MotionToPrevious(
    const std::array<float, 2>& currentPixel,
    const std::array<float, 2>& previousPixel) noexcept;

// Tone mapping, then the grading table, then the output transform. The lookup
// table is authored in display space, so applying it to HDR values reads an
// entry that has nothing to do with the colour.
[[nodiscard]] std::array<float, 3> ToneMap(
    const std::array<float, 3>& hdr,
    float exposure) noexcept;
[[nodiscard]] std::array<float, 3> ApplyOutputTransform(
    const std::array<float, 3>& display,
    OutputFormat format) noexcept;

// A disabled effect must be an *exact* identity, not approximately one.
// Anything else means turning an effect off still changes the image, and
// every A/B comparison after that measures two differences at once.
[[nodiscard]] bool IsIdentityWhenDisabled(
    EffectId id,
    const std::array<float, 3>& sample,
    const std::array<float, 3>& result) noexcept;

// A resize invalidates every history: they hold samples at positions that no
// longer exist.
[[nodiscard]] bool ResizeInvalidatesHistory(
    std::uint32_t previousWidth,
    std::uint32_t previousHeight,
    std::uint32_t width,
    std::uint32_t height) noexcept;

[[nodiscard]] const char* ToString(PostError error) noexcept;
[[nodiscard]] const char* ToString(EffectId id) noexcept;

}
