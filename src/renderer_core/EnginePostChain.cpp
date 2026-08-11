#include "renderer_core/EnginePostChain.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace vf::renderer::post {

namespace {

// The van der Corput radical inverse in a given base. Two coprime bases give
// a Halton sequence, which covers the pixel evenly rather than clustering the
// way an uncorrelated random pair does at small sample counts.
[[nodiscard]] float RadicalInverse(
    std::uint32_t index,
    const std::uint32_t base) noexcept
{
    auto result = 0.0f;
    auto fraction = 1.0f / static_cast<float>(base);
    while (index > 0) {
        result += static_cast<float>(index % base) * fraction;
        index /= base;
        fraction /= static_cast<float>(base);
    }
    return result;
}

}

ChainLedger ClassifyChain(const std::span<const EffectEntry> entries) noexcept
{
    ChainLedger ledger{};
    try {
        for (const auto& entry : entries) {
            switch (entry.coverage) {
            case Coverage::Covered:
                ledger.covered.push_back(entry.id);
                break;
            case Coverage::Retained:
                ledger.retained.push_back(entry.id);
                break;
            case Coverage::Unsupported:
                // A decision, not a gap. Keeping the two apart is the whole
                // point: "we know and chose not to" and "we have no idea"
                // need different responses.
                ledger.unsupported.push_back(entry.id);
                break;
            case Coverage::Unknown:
                ++ledger.unknown;
                break;
            }
        }
    } catch (const std::bad_alloc&) {
        ++ledger.unknown;
    }
    return ledger;
}

PostError ValidateOrder(const std::span<const EffectEntry> entries) noexcept
{
    // The enumerator order *is* the schedule. A disabled effect still holds
    // its place, so enabling one cannot reorder the chain and a settings
    // change cannot silently become a rendering change.
    std::uint32_t previous = 0;
    auto first = true;
    for (const auto& entry : entries) {
        const auto current = static_cast<std::uint32_t>(entry.id);
        if (current >= static_cast<std::uint32_t>(EffectId::Count)) {
            return PostError::UnknownEffect;
        }
        if (!first && current <= previous) return PostError::OrderViolation;
        previous = current;
        first = false;
    }
    return PostError::None;
}

bool MayAlias(const TransientImage& a, const TransientImage& b) noexcept
{
    // A borrowed image belongs to the engine, which decides when it is done
    // with it. Aliasing over it would hand its memory away while it is still
    // in use somewhere this renderer cannot see.
    if (a.borrowed || b.borrowed) return false;
    if (a.resourceId == b.resourceId) return false;
    // Touching at a single pass is still an overlap: the pass that ends one
    // range and the pass that begins the other are the same pass, and both
    // are live inside it.
    return a.lastPass < b.firstPass || b.lastPass < a.firstPass;
}

PostError ValidateAliasing(
    const std::span<const TransientImage> images,
    const std::span<const std::array<std::uint64_t, 2>> aliasPairs) noexcept
{
    for (const auto& pair : aliasPairs) {
        const TransientImage* first = nullptr;
        const TransientImage* second = nullptr;
        for (const auto& image : images) {
            if (image.resourceId == pair[0]) first = &image;
            if (image.resourceId == pair[1]) second = &image;
        }
        if (first == nullptr || second == nullptr) {
            return PostError::UnknownEffect;
        }
        if (!MayAlias(*first, *second)) return PostError::AliasOverlap;
    }
    return PostError::None;
}

PostError ValidateBorrowedUsage(
    const TransientImage& image,
    const bool written) noexcept
{
    // Writing to a borrowed image changes what vanilla draws next, and the
    // damage appears somewhere unrelated to this renderer.
    if (image.borrowed && written) return PostError::BorrowedWrite;
    return PostError::None;
}

ExposureState AdaptExposure(
    const ExposureState& previous,
    const float targetExposure,
    const float deltaSeconds,
    const bool resetHistory,
    const ExposureRules& rules) noexcept
{
    ExposureState state{};
    state.established = true;
    // Bounded, so a black frame does not drive exposure to infinity and a
    // white one does not drive it to zero.
    const auto target = std::isfinite(targetExposure)
        ? std::clamp(targetExposure, rules.minimum, rules.maximum)
        : previous.value;

    // Nothing established, or a cut. Adapting across a cut shows the previous
    // scene's brightness for as long as the adaptation takes, which reads as
    // the new scene being wrong rather than as a stale exposure.
    if (!previous.established || resetHistory) {
        state.value = target;
        return state;
    }
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f) {
        state.value = previous.value;
        return state;
    }

    // Asymmetric because eyes are: dark to bright is fast, the reverse is
    // slow, and one rate for both reads as a lag in whichever direction is
    // wrong. Exponential approach, so it converges without overshooting.
    const auto rate = target > previous.value
        ? rules.brightenRate : rules.darkenRate;
    const auto blend = 1.0f - std::exp(-std::max(0.0f, rate) * deltaSeconds);
    state.value = previous.value + (target - previous.value) * blend;
    state.value = std::clamp(state.value, rules.minimum, rules.maximum);
    return state;
}

float Luminance(const std::array<float, 3>& colour) noexcept
{
    // A non-finite channel yields no luminance rather than a NaN: a NaN
    // reaching the threshold compares false against both ends of the knee and
    // the pixel silently takes whichever branch fell through.
    for (const auto channel : colour) {
        if (!std::isfinite(channel)) return 0.0f;
    }
    return colour[0] * 0.2126f + colour[1] * 0.7152f + colour[2] * 0.0722f;
}

float BloomWeight(const BloomRules& rules, const float luminance) noexcept
{
    if (!std::isfinite(luminance)) return 0.0f;
    // A bare threshold makes bloom pop on as a highlight crosses it, which
    // reads as flicker on any moving specular.
    if (!(rules.knee > 0.0f)) {
        return luminance > rules.threshold ? 1.0f : 0.0f;
    }
    const auto lower = rules.threshold - rules.knee;
    const auto upper = rules.threshold + rules.knee;
    if (luminance <= lower) return 0.0f;
    if (luminance >= upper) return 1.0f;
    const auto t = (luminance - lower) / (upper - lower);
    // Smoothstep rather than linear, so the derivative is continuous at both
    // ends and the transition has no visible corner.
    return std::clamp(t * t * (3.0f - 2.0f * t), 0.0f, 1.0f);
}

JitterState AdvanceJitter(
    const JitterState& state,
    const std::uint32_t width,
    const std::uint32_t height) noexcept
{
    JitterState next{};
    next.width = width;
    next.height = height;
    // A sequence carried across a resize samples positions that no longer
    // exist, so it restarts rather than continuing into nothing.
    if (state.width != width || state.height != height) {
        next.index = 0;
        return next;
    }
    next.index = state.index + 1;
    return next;
}

std::array<float, 2> JitterOffset(const JitterState& state) noexcept
{
    // Halton bases two and three: coprime, so the pair covers the pixel
    // evenly instead of clustering the way an uncorrelated random pair does
    // at the small sample counts a temporal resolve actually gets.
    const auto index = state.index + 1;
    return {RadicalInverse(index, 2) - 0.5f,
        RadicalInverse(index, 3) - 0.5f};
}

std::array<float, 2> MotionToPrevious(
    const std::array<float, 2>& currentPixel,
    const std::array<float, 2>& previousPixel) noexcept
{
    // From where a pixel is now to where it was. Adding the vector to the
    // current position lands on the previous one, which is the property every
    // consumer relies on; the opposite sign smears the other way and looks
    // like a different defect entirely.
    return {previousPixel[0] - currentPixel[0],
        previousPixel[1] - currentPixel[1]};
}

std::array<float, 3> ToneMap(
    const std::array<float, 3>& hdr,
    const float exposure) noexcept
{
    const auto scale = std::isfinite(exposure) && exposure > 0.0f
        ? exposure : 1.0f;
    std::array<float, 3> display{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto value = hdr[channel] * scale;
        if (!std::isfinite(value) || value <= 0.0f) {
            display[channel] = 0.0f;
            continue;
        }
        // Reinhard: monotonic everywhere and asymptotic to one, so a brighter
        // input is never darker out. A non-monotonic curve produces banding
        // that reads as a precision problem rather than as a curve problem.
        display[channel] = value / (1.0f + value);
    }
    return display;
}

std::array<float, 3> ApplyOutputTransform(
    const std::array<float, 3>& display,
    const OutputFormat format) noexcept
{
    std::array<float, 3> output{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto value = std::isfinite(display[channel])
            ? std::max(0.0f, display[channel]) : 0.0f;
        switch (format) {
        case OutputFormat::ScRgbLinear:
            // Exactly the identity, so a display path that needs no
            // conversion performs none.
            output[channel] = display[channel];
            break;
        case OutputFormat::Srgb8:
            output[channel] = value <= 0.0031308f
                ? value * 12.92f
                : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
            break;
        case OutputFormat::Rec2020Pq: {
            // SMPTE ST 2084, normalized so one is the peak the curve encodes.
            constexpr float m1 = 2610.0f / 16384.0f;
            constexpr float m2 = 128.0f * 2523.0f / 4096.0f;
            constexpr float c1 = 3424.0f / 4096.0f;
            constexpr float c2 = 32.0f * 2413.0f / 4096.0f;
            constexpr float c3 = 32.0f * 2392.0f / 4096.0f;
            const auto powered = std::pow(value, m1);
            output[channel] = std::pow(
                (c1 + c2 * powered) / (1.0f + c3 * powered), m2);
            break;
        }
        }
        if (!std::isfinite(output[channel])) output[channel] = 0.0f;
    }
    return output;
}

bool IsIdentityWhenDisabled(
    const EffectId id,
    const std::array<float, 3>& sample,
    const std::array<float, 3>& result) noexcept
{
    static_cast<void>(id);
    // Exact, not approximate. Anything else means turning an effect off still
    // changes the image, and every A/B comparison after that measures two
    // differences at once without saying so.
    for (std::size_t channel = 0; channel < 3; ++channel) {
        if (!(sample[channel] == result[channel])) return false;
    }
    return true;
}

bool ResizeInvalidatesHistory(
    const std::uint32_t previousWidth,
    const std::uint32_t previousHeight,
    const std::uint32_t width,
    const std::uint32_t height) noexcept
{
    // A history with no extent was never established, so there is nothing to
    // reuse and nothing to reject.
    if (previousWidth == 0 || previousHeight == 0) return true;
    return previousWidth != width || previousHeight != height;
}

const char* ToString(const PostError error) noexcept
{
    switch (error) {
    case PostError::None: return "none";
    case PostError::UnknownEffect: return "unknown effect";
    case PostError::OrderViolation: return "order violation";
    case PostError::AliasOverlap: return "alias overlap";
    case PostError::BorrowedWrite: return "borrowed write";
    case PostError::InvalidExtent: return "invalid extent";
    case PostError::NonFiniteSource: return "non-finite source";
    case PostError::InvalidHistory: return "invalid history";
    }
    return "unknown";
}

const char* ToString(const EffectId id) noexcept
{
    switch (id) {
    case EffectId::Exposure: return "exposure";
    case EffectId::Bloom: return "bloom";
    case EffectId::Volumetrics: return "volumetrics";
    case EffectId::MotionBlur: return "motion blur";
    case EffectId::TemporalResolve: return "temporal resolve";
    case EffectId::ToneMap: return "tone map";
    case EffectId::ColorGradingLut: return "colour grading lut";
    case EffectId::Sharpen: return "sharpen";
    case EffectId::FinalTransform: return "final transform";
    case EffectId::Count: return "count";
    }
    return "unknown";
}

}
