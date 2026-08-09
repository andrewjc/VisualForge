#include "renderer_core/EngineTransparency.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace vf::renderer::blend {

namespace {

[[nodiscard]] bool Finite(const float value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] float Dot(
    const std::array<float, 3>& a,
    const std::array<float, 3>& b) noexcept
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

[[nodiscard]] bool Normalize(std::array<float, 3>& value) noexcept
{
    const auto lengthSquared = Dot(value, value);
    if (!(lengthSquared > 0.0f) || !std::isfinite(lengthSquared)) {
        return false;
    }
    const auto inverse = 1.0f / std::sqrt(lengthSquared);
    for (auto& component : value) component *= inverse;
    return true;
}

// Composition order between kinds of effect. A decal belongs on the surface
// it marks and must land before anything volumetric drifts over it; smoke and
// fire sit above everything they obscure. This is a layer, not a distance,
// and depth alone would put a distant decal behind nearby smoke.
[[nodiscard]] std::uint32_t LayerOf(const EffectDomain domain) noexcept
{
    switch (domain) {
    case EffectDomain::Decal: return 0;
    case EffectDomain::Blood: return 1;
    case EffectDomain::Refractive: return 2;
    case EffectDomain::GeneralBlended: return 3;
    case EffectDomain::Particle: return 4;
    case EffectDomain::Smoke: return 5;
    case EffectDomain::Fire: return 6;
    case EffectDomain::Unsupported: return 7;
    }
    return 7;
}

}

TransparencyError ValidateDraw(const TransparentDrawV1& draw) noexcept
{
    if (draw.drawId == 0 || draw.materialId == 0) {
        return TransparencyError::InvalidDomain;
    }
    if (draw.domain > EffectDomain::Unsupported) {
        return TransparencyError::InvalidDomain;
    }
    if (draw.blend > BlendMode::Multiply) {
        return TransparencyError::InvalidBlend;
    }
    if (!Finite(draw.sortDepth) || !Finite(draw.softFade) ||
        !Finite(draw.dissolve) || !Finite(draw.dissolveFalloff)) {
        return TransparencyError::NonFiniteSource;
    }
    if (draw.softFade < 0.0f || draw.dissolve < 0.0f ||
        draw.dissolve > 1.0f || draw.dissolveFalloff < 0.0f ||
        draw.dissolveFalloff > 1.0f) {
        return TransparencyError::InvalidBlend;
    }
    // A blended draw that writes depth occludes the transparent draws behind
    // it, and the layer collapses to whichever was drawn first. An opaque
    // draw is not in the sorted layer and may write.
    if (draw.blend != BlendMode::Opaque && draw.depthWrite) {
        return TransparencyError::InvalidDepthRange;
    }
    return TransparencyError::None;
}

SortKey MakeSortKey(const TransparentDrawV1& draw) noexcept
{
    SortKey key{};
    key.layer = LayerOf(draw.domain);
    key.depth = draw.sortDepth;
    // The identity is what makes the order *total*. Two draws at one depth
    // with no tiebreak swap between frames, and the result reads as a
    // shimmer rather than as a sorting bug.
    key.identity = draw.drawId;
    return key;
}

bool SortsBefore(const SortKey& a, const SortKey& b) noexcept
{
    if (a.layer != b.layer) return a.layer < b.layer;
    // Back to front: the larger depth composites first.
    if (a.depth != b.depth) return a.depth > b.depth;
    return a.identity < b.identity;
}

TransparencyError SortDraws(
    const std::span<const TransparentDrawV1> draws,
    std::vector<std::uint32_t>& order) noexcept
{
    order.clear();
    if (draws.size() > kMaximumTransparentDraws) {
        return TransparencyError::TooManyDraws;
    }
    try {
        order.reserve(draws.size());
        for (std::uint32_t index = 0;
             index < static_cast<std::uint32_t>(draws.size()); ++index) {
            order.push_back(index);
        }
        std::sort(order.begin(), order.end(),
            [draws](const std::uint32_t left, const std::uint32_t right) {
                return SortsBefore(MakeSortKey(draws[left]),
                    MakeSortKey(draws[right]));
            });
    } catch (const std::bad_alloc&) {
        order.clear();
        return TransparencyError::TooManyDraws;
    }
    return TransparencyError::None;
}

std::array<float, 3> Composite(
    const TransparentDrawV1& draw,
    const CompositeSample& sample) noexcept
{
    const auto alpha = std::clamp(sample.source[3], 0.0f, 1.0f);
    std::array<float, 3> result{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto source = sample.source[channel];
        const auto destination = sample.destination[channel];
        switch (draw.blend) {
        case BlendMode::Opaque:
            result[channel] = source;
            break;
        case BlendMode::StraightAlpha:
            result[channel] = source * alpha + destination * (1.0f - alpha);
            break;
        case BlendMode::Premultiplied:
            // Already scaled by its own alpha. Scaling again is the double
            // darkening that shows around every edge.
            result[channel] = source + destination * (1.0f - alpha);
            break;
        case BlendMode::Additive:
            // The destination keeps its full share, which is what makes fire
            // brighten what is behind it rather than replacing it.
            result[channel] = source * alpha + destination;
            break;
        case BlendMode::Multiply:
            // Darkens and can never brighten, which is what a scorch mark
            // needs.
            result[channel] = destination *
                (source * alpha + (1.0f - alpha));
            break;
        }
        if (!std::isfinite(result[channel])) result[channel] = destination;
    }
    return result;
}

float SoftFade(
    const TransparentDrawV1& draw,
    const float sceneDepth,
    const float fragmentDepth) noexcept
{
    if (!(draw.softFade > 0.0f)) return 1.0f;
    if (!Finite(sceneDepth) || !Finite(fragmentDepth)) return 1.0f;
    // Distance in view-space units, not a depth-buffer difference: the latter
    // changes meaning with the near plane, so a particle that looked right at
    // one camera setting would fade differently at another.
    const auto separation = sceneDepth - fragmentDepth;
    if (separation <= 0.0f) return 0.0f;
    return std::clamp(separation / draw.softFade, 0.0f, 1.0f);
}

float DissolveCoverage(
    const TransparentDrawV1& draw,
    const float noise) noexcept
{
    // Nothing authored leaves the effect alone rather than fading it.
    if (!(draw.dissolve > 0.0f)) return 1.0f;
    if (!Finite(noise)) return 1.0f;
    if (!(draw.dissolveFalloff > 0.0f)) {
        // The hard cut, still exact.
        return noise > draw.dissolve ? 1.0f : 0.0f;
    }
    const auto lower = draw.dissolve - draw.dissolveFalloff * 0.5f;
    const auto upper = draw.dissolve + draw.dissolveFalloff * 0.5f;
    if (noise <= lower) return 0.0f;
    if (noise >= upper) return 1.0f;
    return std::clamp((noise - lower) / (upper - lower), 0.0f, 1.0f);
}

float TransparentAlpha(
    const TransparentDrawV1& draw,
    const float sourceAlpha,
    const float sceneDepth,
    const float fragmentDepth,
    const float noise) noexcept
{
    // Multiplicative: two independent reasons to be less present do not each
    // get the whole budget. The source alpha is clamped rather than trusted,
    // because a value above one brightens the effect past what it authored.
    return std::clamp(sourceAlpha, 0.0f, 1.0f) *
        SoftFade(draw, sceneDepth, fragmentDepth) *
        DissolveCoverage(draw, noise);
}

TransparencyError ProjectDecal(
    const DecalProjection& projection,
    const std::array<float, 3>& surfacePosition,
    const std::array<float, 3>& surfaceNormal,
    const std::uint8_t surfaceStencil,
    const TransparentDrawV1& draw,
    float& coverage) noexcept
{
    coverage = 0.0f;
    auto axis = projection.axis;
    if (!Normalize(axis)) return TransparencyError::DegenerateAxis;
    if (!(projection.range > 0.0f) || !(projection.radius > 0.0f) ||
        !Finite(projection.range) || !Finite(projection.radius)) {
        return TransparencyError::InvalidProjection;
    }

    // A surface the engine did not mark as a receiver takes nothing. Without
    // this a decal lands on the sky and on characters walking past it.
    if (draw.stencilReceiverMask != 0 &&
        (surfaceStencil & draw.stencilReceiverMask) !=
            (draw.stencilReference & draw.stencilReceiverMask)) {
        return TransparencyError::None;
    }

    const std::array<float, 3> offset{
        surfacePosition[0] - projection.origin[0],
        surfacePosition[1] - projection.origin[1],
        surfacePosition[2] - projection.origin[2]};
    // Along the axis: outside the range the decal would stretch across every
    // surface behind the one it was meant for.
    const auto along = Dot(offset, axis);
    if (along < 0.0f || along > projection.range) {
        return TransparencyError::None;
    }
    // Perpendicular: outside the radius it is simply not this decal's.
    const std::array<float, 3> lateral{
        offset[0] - axis[0] * along,
        offset[1] - axis[1] * along,
        offset[2] - axis[2] * along};
    const auto distance = std::sqrt(Dot(lateral, lateral));
    if (distance > projection.radius) return TransparencyError::None;

    // Facing away from the projection: a decal wrapping around to the back of
    // a wall is the artefact this prevents.
    auto normal = surfaceNormal;
    if (!Normalize(normal)) return TransparencyError::None;
    const auto facing = -Dot(normal, axis);
    if (facing <= 0.0f) return TransparencyError::None;

    coverage = facing * (1.0f - distance / projection.radius);
    coverage = std::clamp(coverage, 0.0f, 1.0f);
    return TransparencyError::None;
}

bool RefractionReadsPriorTarget(const TransparentDrawV1& draw) noexcept
{
    // Sampling the live target feeds a refracting surface its own output, and
    // the result diverges over a few frames into a smear.
    return draw.domain == EffectDomain::Refractive;
}

float ReactiveMask(
    const TransparentDrawV1& draw,
    const CompositeSample& sample) noexcept
{
    const auto alpha = std::clamp(sample.source[3], 0.0f, 1.0f);
    // How much of the pixel this effect actually decides. An additive draw
    // contributes its full radiance whatever its alpha says, so its influence
    // is not the alpha alone.
    if (draw.blend == BlendMode::Additive) {
        auto brightest = 0.0f;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            if (std::isfinite(sample.source[channel])) {
                brightest = std::max(brightest, sample.source[channel]);
            }
        }
        return std::clamp(std::max(alpha, brightest), 0.0f, 1.0f);
    }
    return alpha;
}

CompatibilityLedger ClassifyCoverage(
    const std::span<const TransparentDrawV1> draws) noexcept
{
    CompatibilityLedger ledger{};
    try {
        for (const auto& draw : draws) {
            if (draw.domain == EffectDomain::Unsupported) {
                // Recorded by identity rather than counted, so a
                // compatibility gap is a list somebody can act on.
                ledger.unsupportedDraws.push_back(draw.drawId);
                continue;
            }
            ++ledger.covered;
        }
    } catch (const std::bad_alloc&) {
        ledger.unsupportedDraws.clear();
    }
    return ledger;
}

const char* ToString(const TransparencyError error) noexcept
{
    switch (error) {
    case TransparencyError::None: return "none";
    case TransparencyError::InvalidDomain: return "invalid domain";
    case TransparencyError::InvalidBlend: return "invalid blend";
    case TransparencyError::NonFiniteSource: return "non-finite source";
    case TransparencyError::InvalidDepthRange: return "invalid depth range";
    case TransparencyError::InvalidProjection: return "invalid projection";
    case TransparencyError::DegenerateAxis: return "degenerate axis";
    case TransparencyError::TooManyDraws: return "too many draws";
    }
    return "unknown";
}

const char* ToString(const EffectDomain domain) noexcept
{
    switch (domain) {
    case EffectDomain::Decal: return "decal";
    case EffectDomain::Blood: return "blood";
    case EffectDomain::Fire: return "fire";
    case EffectDomain::Smoke: return "smoke";
    case EffectDomain::Particle: return "particle";
    case EffectDomain::GeneralBlended: return "general blended";
    case EffectDomain::Refractive: return "refractive";
    case EffectDomain::Unsupported: return "unsupported";
    }
    return "unknown";
}

}
