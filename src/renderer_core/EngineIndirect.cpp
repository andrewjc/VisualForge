#include "renderer_core/EngineIndirect.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vf::renderer::gi {

namespace {

constexpr float kPi = 3.14159265358979323846f;

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

// Duff et al., the same construction every other pass uses. One surface must
// not end up with two different tangent frames.
void OrthonormalBasis(
    const std::array<float, 3>& normal,
    std::array<float, 3>& tangent,
    std::array<float, 3>& bitangent) noexcept
{
    const auto sign = std::copysign(1.0f, normal[2]);
    const auto a = -1.0f / (sign + normal[2]);
    const auto b = normal[0] * normal[1] * a;
    tangent = {1.0f + sign * normal[0] * normal[0] * a, sign * b,
        -sign * normal[0]};
    bitangent = {b, sign + normal[1] * normal[1] * a, -normal[1]};
}

}

IndirectError SampleDiffuseDirection(
    const std::array<float, 3>& normal,
    const std::array<float, 2>& sample,
    std::array<float, 3>& direction) noexcept
{
    direction = {};
    auto axis = normal;
    if (!Normalize(axis)) return IndirectError::DegenerateNormal;

    // Malley's method: a uniform point on the disc lifted to the hemisphere
    // is cosine distributed. The cosine is carried by the distribution rather
    // than multiplied in afterwards, so no ray is spent near the horizon
    // where it would contribute almost nothing.
    const auto u1 = std::clamp(sample[0], 0.0f, 0.999999f);
    const auto u2 = std::clamp(sample[1], 0.0f, 0.999999f);
    const auto radius = std::sqrt(u1);
    const auto phi = 2.0f * kPi * u2;
    const auto x = radius * std::cos(phi);
    const auto y = radius * std::sin(phi);
    const auto z = std::sqrt(std::max(0.0f, 1.0f - u1));

    std::array<float, 3> tangent{};
    std::array<float, 3> bitangent{};
    OrthonormalBasis(axis, tangent, bitangent);
    direction = {
        tangent[0] * x + bitangent[0] * y + axis[0] * z,
        tangent[1] * x + bitangent[1] * y + axis[1] * z,
        tangent[2] * x + bitangent[2] * y + axis[2] * z};
    if (!Normalize(direction)) return IndirectError::DegenerateNormal;
    return IndirectError::None;
}

std::array<float, 3> ClampRadiance(
    const std::array<float, 3>& radiance,
    const IndirectRules& rules) noexcept
{
    std::array<float, 3> bounded{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto value = radiance[channel];
        // A failed path, not an infinitely bright one. Carrying a NaN into
        // the accumulator poisons the pixel permanently.
        if (!std::isfinite(value)) {
            bounded[channel] = 0.0f;
            continue;
        }
        bounded[channel] = std::clamp(value, 0.0f, rules.radianceClamp);
    }
    return bounded;
}

std::array<float, 3> SeparateIndirect(
    const std::array<float, 3>& total,
    const std::array<float, 3>& direct) noexcept
{
    std::array<float, 3> indirect{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        // Never negative: a direct term larger than the total is two
        // measurements disagreeing, not a negative light, and subtracting
        // past zero would darken the surface below its own albedo.
        indirect[channel] = std::max(0.0f, total[channel] - direct[channel]);
    }
    return indirect;
}

ReprojectionResult Reproject(
    const SurfaceSample& current,
    const SurfaceSample& previous,
    const std::array<float, 2>& motion,
    const std::uint32_t x,
    const std::uint32_t y,
    const std::uint32_t width,
    const std::uint32_t height,
    const IndirectRules& rules,
    const reflect::ReflectionHistoryKey& previousEpoch,
    const reflect::ReflectionHistoryKey& currentEpoch) noexcept
{
    ReprojectionResult result{};
    // A camera cut invalidates every pixel at once, whatever the surfaces
    // say, so the epoch is tested before anything geometric.
    if (reflect::ResetHistory(previousEpoch, currentEpoch)) {
        result.reason = RejectReason::Epoch;
        return result;
    }
    if (width == 0 || height == 0) {
        result.reason = RejectReason::OffScreen;
        return result;
    }
    if (!std::isfinite(motion[0]) || !std::isfinite(motion[1])) {
        result.reason = RejectReason::OffScreen;
        return result;
    }
    const auto sourceX =
        static_cast<std::int64_t>(x) + static_cast<std::int64_t>(motion[0]);
    const auto sourceY =
        static_cast<std::int64_t>(y) + static_cast<std::int64_t>(motion[1]);
    if (sourceX < 0 || sourceY < 0 ||
        sourceX >= static_cast<std::int64_t>(width) ||
        sourceY >= static_cast<std::int64_t>(height)) {
        result.reason = RejectReason::OffScreen;
        return result;
    }
    result.sourceX = static_cast<std::uint32_t>(sourceX);
    result.sourceY = static_cast<std::uint32_t>(sourceY);

    // Relative, not absolute. At a thousand units a pixel's depth changes by
    // several units between frames without the surface moving at all, and a
    // fixed epsilon rejects every distant pixel forever.
    const auto reference = std::max(std::abs(current.depth), 1.0e-4f);
    if (!std::isfinite(current.depth) || !std::isfinite(previous.depth) ||
        std::abs(current.depth - previous.depth) / reference >
            rules.depthTolerance) {
        result.reason = RejectReason::Depth;
        return result;
    }

    auto currentNormal = current.geometricNormal;
    auto previousNormal = previous.geometricNormal;
    if (!Normalize(currentNormal) || !Normalize(previousNormal) ||
        Dot(currentNormal, previousNormal) < rules.normalCosineTolerance) {
        result.reason = RejectReason::Normal;
        return result;
    }

    // Object and material are separate tests. Two objects can share a
    // material, and one object can change material without moving, so a
    // single identity check would miss one case or the other.
    if (rules.requireObjectMatch && current.objectId != previous.objectId) {
        result.reason = RejectReason::Object;
        return result;
    }
    if (rules.requireMaterialMatch &&
        current.materialId != previous.materialId) {
        result.reason = RejectReason::Material;
        return result;
    }
    return result;
}

HistorySample Accumulate(
    const HistorySample& history,
    const std::array<float, 3>& sample,
    const RejectReason reason,
    const QualityPreset& preset) noexcept
{
    HistorySample updated{};
    // A rejected sample resets. Blending it in is exactly the trail the gate
    // forbids: the previous scene stays visible, fading, for as long as the
    // history is allowed to be.
    if (reason != RejectReason::Accepted || history.length == 0) {
        updated.length = 1;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            updated.mean[channel] = sample[channel];
            updated.secondMoment[channel] =
                sample[channel] * sample[channel];
        }
        return updated;
    }

    const auto cap = std::max<std::uint32_t>(1, preset.maximumHistoryLength);
    updated.length = std::min(history.length + 1, cap);
    // An exponential moving average whose weight is one over the current
    // length: identical to a true average while the history is short, and a
    // fixed-rate filter once it is capped. That is what keeps a converged
    // pixel converged without freezing it against real change.
    const auto weight = 1.0f / static_cast<float>(updated.length);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        updated.mean[channel] = history.mean[channel] +
            (sample[channel] - history.mean[channel]) * weight;
        const auto squared = sample[channel] * sample[channel];
        updated.secondMoment[channel] = history.secondMoment[channel] +
            (squared - history.secondMoment[channel]) * weight;
    }
    return updated;
}

std::array<float, 3> Variance(const HistorySample& history) noexcept
{
    std::array<float, 3> variance{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        // Negative only through float cancellation, never physically.
        variance[channel] = std::max(0.0f,
            history.secondMoment[channel] -
                history.mean[channel] * history.mean[channel]);
    }
    return variance;
}

IndirectError MapToTraceResolution(
    const std::uint32_t x,
    const std::uint32_t y,
    const std::uint32_t width,
    const std::uint32_t height,
    const QualityPreset& preset,
    std::uint32_t& traceX,
    std::uint32_t& traceY,
    std::uint32_t& traceWidth,
    std::uint32_t& traceHeight) noexcept
{
    traceX = 0;
    traceY = 0;
    traceWidth = 0;
    traceHeight = 0;
    if (width == 0 || height == 0 || x >= width || y >= height) {
        return IndirectError::InvalidResolution;
    }
    if (!preset.halfResolution) {
        traceX = x;
        traceY = y;
        traceWidth = width;
        traceHeight = height;
        return IndirectError::None;
    }
    // Rounded up, so an odd extent still covers its last column and row.
    // Rounding down leaves those pixels reading a sample that was never
    // traced, which reads as a hard edge along two sides of the frame.
    traceWidth = (width + 1) / 2;
    traceHeight = (height + 1) / 2;
    traceX = std::min(x / 2, traceWidth - 1);
    traceY = std::min(y / 2, traceHeight - 1);
    return IndirectError::None;
}

std::array<float, 3> EvaluateIndirect(
    const SurfaceSample& surface,
    const IndirectRules& rules,
    const QualityPreset& preset,
    const std::span<const reflect::ReflectionTriangle> geometry,
    const std::span<const lighting::GpuLightRecordV1> lights,
    const lighting::GpuEnvironmentV1& environment,
    const std::uint32_t pixelX,
    const std::uint32_t pixelY,
    const std::uint32_t frameIndex,
    IndirectSource& source) noexcept
{
    source = IndirectSource::Skipped;
    std::array<float, 3> total{};
    if (preset.raysPerPixel == 0) return total;
    // No captured lighting means no light to bounce. Phase 17 established
    // that such a frame leaves the albedo alone, and a bounce that returned
    // the hit surface's albedo would invent light the capture never saw.
    if ((environment.flagsAndCount[0] & lighting::EnvironmentPresent) == 0) {
        return total;
    }
    // Switched off for the frame. Exactly nothing rather than a small
    // residue: a term that is almost off still moves every pixel it touches,
    // and an isolation built on it measures the remainder rather than the
    // term it meant to remove.
    if ((environment.flagsAndCount[0] &
        lighting::EnvironmentIndirectDisabled) != 0) {
        return total;
    }

    auto normal = surface.geometricNormal;
    if (!Normalize(normal)) return total;

    std::uint32_t contributing = 0;
    auto sawGeometry = false;
    auto sawEnvironment = false;

    for (std::uint32_t ray = 0; ray < preset.raysPerPixel; ++ray) {
        const auto pair =
            reflect::SampleSequence(pixelX, pixelY, frameIndex, ray);
        std::array<float, 3> direction{};
        if (SampleDiffuseDirection(normal, pair, direction) !=
            IndirectError::None) {
            continue;
        }

        reflect::ReflectionRay bounce{};
        bounce.origin =
            accel::OffsetRayOrigin(surface.position, normal, 1.0f);
        bounce.direction = direction;
        bounce.minimumDistance = 0.0f;
        bounce.maximumDistance = accel::kDirectionalShadowDistance;

        const auto hit = reflect::TraceReflection(geometry, bounce);
        std::array<float, 3> radiance{};
        if (hit.hit) {
            // The bounce surface is shadowed by the same geometry the direct
            // pass shadows against. Skipping it would bounce light off a
            // surface that is in shadow, so a room would brighten from a
            // wall the sun cannot reach.
            std::array<float, reflect::kMaximumReflectionLights> shadow{};
            const auto count =
                std::min<std::size_t>(lights.size(), shadow.size());
            for (std::size_t index = 0; index < count; ++index) {
                const auto shadowRay = accel::ShadowRayForLight(
                    lights[index], hit.position, hit.normal);
                reflect::ReflectionRay toLight{};
                toLight.origin = shadowRay.origin;
                toLight.direction = shadowRay.direction;
                toLight.minimumDistance = shadowRay.minimumDistance;
                toLight.maximumDistance = shadowRay.maximumDistance;
                shadow[index] = (shadowRay.maximumDistance > 0.0f &&
                    reflect::TraceReflection(geometry, toLight).hit)
                    ? 0.0f : 1.0f;
            }
            // Shaded through the same function the raster pass uses, so an
            // indirect bounce cannot disagree with the surface it came from.
            radiance = lighting::ShadeSurfaceGpu(environment, lights,
                hit.albedo, hit.position, hit.normal,
                std::span<const float>{shadow.data(), count});
            sawGeometry = true;
        } else {
            // Nothing was hit. Indoors that means nothing is known, and the
            // exterior sky must not stand in: that substitution is the light
            // leak this rule exists to prevent.
            radiance = reflect::EvaluateMissRadiance(environment, direction,
                {0.0f, 0.0f, 0.0f}, false);
            if (reflect::ResolveMiss(environment, false) ==
                reflect::ReflectionSource::Environment) {
                sawEnvironment = true;
            }
        }

        const auto bounded = ClampRadiance(radiance, rules);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            total[channel] += bounded[channel];
        }
        ++contributing;
    }

    if (contributing == 0) {
        source = IndirectSource::Unresolved;
        return {};
    }
    // The cosine-weighted estimator's weight is already one: the distribution
    // carries the cosine and the Lambertian denominator cancels it, so the
    // mean of the samples is the integral.
    const auto weight = 1.0f / static_cast<float>(contributing);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        total[channel] *= weight * surface.albedo[channel];
    }
    source = sawGeometry ? IndirectSource::Geometry
        : sawEnvironment ? IndirectSource::Environment
                         : IndirectSource::Unresolved;
    if (source == IndirectSource::Unresolved) return {};
    return total;
}

const char* ToString(const IndirectError error) noexcept
{
    switch (error) {
    case IndirectError::None: return "none";
    case IndirectError::DegenerateNormal: return "degenerate normal";
    case IndirectError::NonFiniteSource: return "non-finite source";
    case IndirectError::InvalidResolution: return "invalid resolution";
    case IndirectError::InvalidHistory: return "invalid history";
    }
    return "unknown";
}

const char* ToString(const RejectReason reason) noexcept
{
    switch (reason) {
    case RejectReason::Accepted: return "accepted";
    case RejectReason::Epoch: return "epoch";
    case RejectReason::OffScreen: return "off screen";
    case RejectReason::Depth: return "depth";
    case RejectReason::Normal: return "normal";
    case RejectReason::Object: return "object";
    case RejectReason::Material: return "material";
    }
    return "unknown";
}

GpuIndirectPixelV1 BuildGpuIndirectPixel(
    const SurfaceSample& surface,
    const std::array<float, 2>& motion,
    const std::array<float, 3>& radiance) noexcept
{
    GpuIndirectPixelV1 record{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        record.normal[axis] = surface.geometricNormal[axis];
        record.radiance[axis] = radiance[axis];
    }
    record.depth = surface.depth;
    record.motion[0] = motion[0];
    record.motion[1] = motion[1];
    // Split rather than narrowed. Two objects whose low words agree would
    // otherwise reproject into each other.
    record.objectId[0] = static_cast<std::uint32_t>(surface.objectId);
    record.objectId[1] =
        static_cast<std::uint32_t>(surface.objectId >> 32);
    record.materialId[0] = static_cast<std::uint32_t>(surface.materialId);
    record.materialId[1] =
        static_cast<std::uint32_t>(surface.materialId >> 32);
    return record;
}

GpuIndirectHistoryV1 BuildGpuIndirectHistory(
    const HistorySample& history) noexcept
{
    GpuIndirectHistoryV1 record{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        record.mean[channel] = history.mean[channel];
        record.secondMoment[channel] = history.secondMoment[channel];
    }
    record.samples = history.length;
    return record;
}

HistorySample ReadGpuIndirectHistory(
    const GpuIndirectHistoryV1& record) noexcept
{
    HistorySample history{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        history.mean[channel] = record.mean[channel];
        history.secondMoment[channel] = record.secondMoment[channel];
    }
    history.length = record.samples;
    return history;
}

}
