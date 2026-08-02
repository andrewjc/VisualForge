#include "renderer_core/EngineVisibility.h"

#include <algorithm>
#include <cmath>

namespace vf::renderer::visibility {

namespace {

// A shading normal is never allowed to sit exactly on the horizon, because a
// grazing frame produces a zero-length reflection basis.
constexpr float kHorizonEpsilon = 1.0e-3f;
constexpr float kMaximumCoverageScale = 8.0f;
constexpr int kCoverageBisectionSteps = 40;

// Ordered 4x4 Bayer thresholds, holding each of the sixteen levels once.
constexpr std::array<std::array<std::uint32_t, kDitherExtent>, kDitherExtent>
    kBayer{{
        {{0, 8, 2, 10}},
        {{12, 4, 14, 6}},
        {{3, 11, 1, 9}},
        {{15, 7, 13, 5}},
    }};

bool Finite(const std::array<float, 3>& value) noexcept
{
    return std::all_of(value.begin(), value.end(),
        [](const float component) { return std::isfinite(component); });
}

bool UnitRange(const float value) noexcept
{
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

float Length(const std::array<float, 3>& value) noexcept
{
    return std::sqrt(value[0] * value[0] + value[1] * value[1] +
        value[2] * value[2]);
}

bool Normalize(std::array<float, 3>& value) noexcept
{
    const auto length = Length(value);
    if (!std::isfinite(length) || length <= 0.0f) return false;
    for (auto& component : value) component /= length;
    return true;
}

float Dot(
    const std::array<float, 3>& left,
    const std::array<float, 3>& right) noexcept
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

bool ValidMipLevel(const AlphaMipLevel& level) noexcept
{
    if (level.width == 0 || level.height == 0) return false;
    const auto texels = static_cast<std::size_t>(level.width) * level.height;
    if (level.alpha.size() != texels) return false;
    return std::all_of(level.alpha.begin(), level.alpha.end(),
        [](const float value) {
            return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
        });
}

}

VisibilityError ClassifyAlphaState(
    const AlphaPropertyCapture& capture,
    const AlphaSource observedSource,
    AlphaStateV1& state) noexcept
{
    state = {};
    if (!UnitRange(capture.fade)) return VisibilityError::InvalidFade;
    // Alpha-to-coverage without an alpha test is a state combination this
    // project has not observed in the engine. It is refused, not guessed at.
    if (capture.alphaToCoverage && !capture.testEnabled) {
        return VisibilityError::UnclassifiedAlpha;
    }
    // A test that consults no alpha source could never discard, so the
    // capture disagrees with itself.
    if (capture.testEnabled && observedSource == AlphaSource::None) {
        return VisibilityError::UnclassifiedAlpha;
    }

    state.constantAlpha = 1.0f;
    state.fade = capture.fade;
    state.flags = (capture.alphaToCoverage ? AlphaToCoverage : 0u) |
        (capture.ditherFade ? DitherFade : 0u);
    if (capture.blendEnabled || capture.testEnabled) {
        state.reference =
            static_cast<float>(capture.testReference) / 255.0f;
        state.source = observedSource;
        state.classification = capture.blendEnabled
            ? AlphaClass::Blended : AlphaClass::Tested;
    } else {
        state.classification = AlphaClass::Opaque;
        state.source = AlphaSource::None;
    }
    return VisibilityError::None;
}

float DitherThreshold(
    const std::uint32_t pixelX,
    const std::uint32_t pixelY) noexcept
{
    const auto cell = kBayer[pixelY % kDitherExtent][pixelX % kDitherExtent];
    return static_cast<float>(cell) /
        static_cast<float>(kDitherExtent * kDitherExtent);
}

CoverageResult EvaluateCoverage(
    const AlphaStateV1& state,
    const float sampledAlpha,
    const CoverageContext& context) noexcept
{
    CoverageResult result{};
    if (state.classification == AlphaClass::Unclassified) return result;
    if (state.classification == AlphaClass::Opaque) {
        result.covered = true;
        result.coverage = 1.0f;
        return result;
    }
    if (!std::isfinite(sampledAlpha)) return result;
    // The fade gate is a property of the pixel, not of the pass, which is
    // what keeps the depth prepass and the color pass in agreement.
    if ((state.flags & DitherFade) != 0 &&
        !(state.fade > DitherThreshold(context.pixelX, context.pixelY))) {
        return result;
    }
    const auto effective = std::clamp(
        sampledAlpha * state.constantAlpha, 0.0f, 1.0f);
    if (state.classification == AlphaClass::Blended) {
        result.covered = true;
        result.coverage = effective;
        return result;
    }
    // Single-sample rendering has no partial coverage to give, so it resolves
    // exactly like a plain alpha test.
    if ((state.flags & AlphaToCoverage) != 0 && context.sampleCount > 1) {
        const auto samples = static_cast<float>(std::min(
            context.sampleCount, kMaximumCoverageSamples));
        result.coverage = std::clamp(
            std::round(effective * samples) / samples, 0.0f, 1.0f);
        result.covered = result.coverage > 0.0f;
        return result;
    }
    // The engine keeps a fragment whose alpha reaches the reference.
    result.covered = effective >= state.reference;
    result.coverage = result.covered ? 1.0f : 0.0f;
    return result;
}

float AlphaCoverage(
    const AlphaMipLevel& level,
    const float reference,
    const float scale) noexcept
{
    if (level.alpha.empty()) return 0.0f;
    const auto covered = std::count_if(level.alpha.begin(), level.alpha.end(),
        [reference, scale](const float value) {
            return value * scale >= reference;
        });
    return static_cast<float>(covered) /
        static_cast<float>(level.alpha.size());
}

VisibilityError ComputeAlphaCoverageScales(
    const std::span<const AlphaMipLevel> chain,
    const float reference,
    std::vector<float>& scales) noexcept
{
    scales.clear();
    if (!UnitRange(reference)) return VisibilityError::InvalidCutoff;
    if (chain.empty() || chain.size() > kMaximumAlphaMipLevels) {
        return VisibilityError::InvalidMipChain;
    }
    if (!std::all_of(chain.begin(), chain.end(), ValidMipLevel)) {
        return VisibilityError::InvalidMipChain;
    }
    try {
        scales.assign(chain.size(), 1.0f);
        const auto target = AlphaCoverage(chain.front(), reference, 1.0f);
        for (std::size_t level = 1; level < chain.size(); ++level) {
            // Coverage is monotonic in the scale, so bisection finds the
            // smallest scale that reaches the target.
            float low = 0.0f;
            float high = kMaximumCoverageScale;
            for (int step = 0; step < kCoverageBisectionSteps; ++step) {
                const auto middle = 0.5f * (low + high);
                if (AlphaCoverage(chain[level], reference, middle) < target) {
                    low = middle;
                } else {
                    high = middle;
                }
            }
            const auto highCoverage = AlphaCoverage(
                chain[level], reference, high);
            const auto lowCoverage = AlphaCoverage(
                chain[level], reference, low);
            // On a tie prefer the scale that does not lose coverage; a cutout
            // that thins is more visible than one that thickens.
            scales[level] = std::abs(lowCoverage - target) <
                std::abs(highCoverage - target) ? low : high;
        }
        return VisibilityError::None;
    } catch (...) {
        scales.clear();
        return VisibilityError::AllocationFailure;
    }
}

VisibilityError ResolveShadingFrame(
    const FaceMode faceMode,
    const bool backFacing,
    const float modelDeterminant,
    const ShadingFrameInput& input,
    ShadingFrame& frame) noexcept
{
    frame = {};
    if (faceMode < FaceMode::FrontOnly || faceMode > FaceMode::TwoSided) {
        return VisibilityError::InvalidFaceMode;
    }
    if (!std::isfinite(modelDeterminant) || modelDeterminant == 0.0f) {
        return VisibilityError::InvalidDeterminant;
    }
    if (!Finite(input.geometricNormal) || !Finite(input.shadingNormal) ||
        !Finite(input.tangent) || !Finite(input.bitangent)) {
        return VisibilityError::InvalidNormalFrame;
    }
    auto geometric = input.geometricNormal;
    auto shading = input.shadingNormal;
    auto tangent = input.tangent;
    auto bitangent = input.bitangent;
    if (!Normalize(geometric) || !Normalize(shading) ||
        !Normalize(tangent) || !Normalize(bitangent)) {
        return VisibilityError::InvalidNormalFrame;
    }

    // Only a two-sided surface is ever shaded from its back; a back face of a
    // single-sided surface is culled instead.
    frame.flipped = faceMode == FaceMode::TwoSided && backFacing;
    if (frame.flipped) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            geometric[axis] = -geometric[axis];
            shading[axis] = -shading[axis];
        }
    }

    const auto alignment = Dot(geometric, shading);
    if (alignment <= kHorizonEpsilon) {
        // Project the shading normal onto the geometric horizon rather than
        // letting it light the surface from behind.
        for (std::size_t axis = 0; axis < 3; ++axis) {
            shading[axis] += geometric[axis] * (kHorizonEpsilon - alignment);
        }
        if (!Normalize(shading)) return VisibilityError::InvalidNormalFrame;
        frame.liftedToHorizon = true;
    }

    // A mirroring transform reverses the tangent basis handedness; correcting
    // the bitangent keeps normal maps on the side they were authored for.
    frame.mirrored = modelDeterminant < 0.0f;
    if (frame.mirrored) {
        for (auto& component : bitangent) component = -component;
    }
    frame.geometricNormal = geometric;
    frame.shadingNormal = shading;
    frame.tangent = tangent;
    frame.bitangent = bitangent;
    return VisibilityError::None;
}

raster::FrontFace EffectiveFrontFace(
    const raster::FrontFace declared,
    const float modelDeterminant) noexcept
{
    if (!std::isfinite(modelDeterminant) || modelDeterminant >= 0.0f) {
        return declared;
    }
    return declared == raster::FrontFace::CounterClockwise
        ? raster::FrontFace::Clockwise
        : raster::FrontFace::CounterClockwise;
}

VisibilityError ValidateVisibilityRecord(
    const VisibilityRecordV1& record) noexcept
{
    if (record.objectId == 0 || record.materialId == 0) {
        return VisibilityError::InvalidIdentity;
    }
    if ((record.alpha.flags & ~kKnownVisibilityFlags) != 0 ||
        record.alpha.reserved0 != 0 ||
        !std::all_of(std::begin(record.alpha.reserved1),
            std::end(record.alpha.reserved1),
            [](const std::uint32_t value) { return value == 0; }) ||
        !std::all_of(std::begin(record.reserved0),
            std::end(record.reserved0),
            [](const std::uint8_t value) { return value == 0; }) ||
        record.reserved1 != 0) {
        return VisibilityError::InvalidFlags;
    }
    if (!UnitRange(record.alpha.reference) ||
        !UnitRange(record.alpha.constantAlpha)) {
        return VisibilityError::InvalidCutoff;
    }
    if (!UnitRange(record.alpha.fade)) return VisibilityError::InvalidFade;
    if (record.faceMode < FaceMode::FrontOnly ||
        record.faceMode > FaceMode::TwoSided) {
        return VisibilityError::InvalidFaceMode;
    }
    if (!std::isfinite(record.modelDeterminant) ||
        record.modelDeterminant == 0.0f) {
        return VisibilityError::InvalidDeterminant;
    }
    if (record.alpha.classification == AlphaClass::Unclassified ||
        (record.alpha.classification == AlphaClass::Tested &&
            record.alpha.source == AlphaSource::None)) {
        return VisibilityError::UnclassifiedAlpha;
    }
    return VisibilityError::None;
}

VisibilityError ValidateOpaqueRasterClass(
    const VisibilityRecordV1& record) noexcept
{
    const auto validation = ValidateVisibilityRecord(record);
    if (validation != VisibilityError::None) return validation;
    // Sorted transparency is a classified class this phase does not render.
    // It is refused here rather than rendered incorrectly as a cutout.
    if (record.alpha.classification == AlphaClass::Blended) {
        return VisibilityError::BlendedNotSupported;
    }
    return VisibilityError::None;
}

const char* ToString(const VisibilityError error) noexcept
{
    switch (error) {
    case VisibilityError::None: return "none";
    case VisibilityError::InvalidIdentity: return "invalid identity";
    case VisibilityError::InvalidFlags: return "invalid flags";
    case VisibilityError::InvalidCutoff: return "invalid cutoff";
    case VisibilityError::InvalidFade: return "invalid fade";
    case VisibilityError::InvalidFaceMode: return "invalid face mode";
    case VisibilityError::InvalidDeterminant: return "invalid determinant";
    case VisibilityError::InvalidNormalFrame: return "invalid normal frame";
    case VisibilityError::InvalidMipChain: return "invalid mip chain";
    case VisibilityError::UnclassifiedAlpha: return "unclassified alpha";
    case VisibilityError::BlendedNotSupported:
        return "blended class not supported";
    case VisibilityError::AllocationFailure: return "allocation failure";
    }
    return "unknown";
}

}
