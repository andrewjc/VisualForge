#include "renderer_core/EngineWater.h"

#include <algorithm>
#include <cmath>

namespace vf::renderer::water {

namespace {

constexpr float kTwoPi = 6.28318530718f;

[[nodiscard]] bool Finite(const float value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool Finite(const std::array<float, 3>& value) noexcept
{
    return std::all_of(value.begin(), value.end(),
        [](const float entry) { return std::isfinite(entry); });
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

}

WaterError ValidateMaterial(const WaterMaterialV1& material) noexcept
{
    if (material.materialId == 0) return WaterError::InvalidIdentity;
    if (material.surfaceClass > TransmissiveClass::Unknown) {
        return WaterError::InvalidIdentity;
    }
    if (!Finite(material.shallowColor) || !Finite(material.deepColor) ||
        !Finite(material.fogColor) || !Finite(material.siltColor) ||
        !Finite(material.depthRange) || !Finite(material.shorelineDepth) ||
        !Finite(material.fogDensity) || !Finite(material.siltDensity) ||
        !Finite(material.fresnelBias) || !Finite(material.sparklePower)) {
        return WaterError::NonFiniteSource;
    }
    if (material.depthRange < 0.0f || material.shorelineDepth < 0.0f ||
        material.fogDensity < 0.0f || material.siltDensity < 0.0f) {
        return WaterError::InvalidDepthRange;
    }
    // A layer with no scale samples one texel forever, whatever it scrolls
    // over. Still water is expressed by zero amplitude, not by a zero scale,
    // so a zero scale is a captured contradiction rather than a calm day.
    for (const auto& layer : material.layers) {
        if (!Finite(layer.scroll[0]) || !Finite(layer.scroll[1]) ||
            !Finite(layer.scale) || !Finite(layer.amplitude)) {
            return WaterError::NonFiniteSource;
        }
        if (!(layer.scale > 0.0f) || layer.amplitude < 0.0f) {
            return WaterError::InvalidScroll;
        }
    }
    return WaterError::None;
}

bool EvaluateNormal(
    const WaterMaterialV1& material,
    const std::array<float, 2>& position,
    const float time,
    std::array<float, 3>& normal) noexcept
{
    normal = {0.0f, 0.0f, 1.0f};
    if (!Finite(position[0]) || !Finite(position[1]) || !Finite(time)) {
        return false;
    }
    // Each layer contributes a gradient at its own scale and speed. Summing
    // gradients rather than normals is what lets three layers combine into
    // one surface instead of three surfaces fighting.
    float slopeX = 0.0f;
    float slopeY = 0.0f;
    for (const auto& layer : material.layers) {
        if (!(layer.scale > 0.0f) || !(layer.amplitude > 0.0f)) continue;
        const auto u =
            (position[0] + layer.scroll[0] * time) * layer.scale;
        const auto v =
            (position[1] + layer.scroll[1] * time) * layer.scale;
        slopeX += layer.amplitude * std::cos(u * kTwoPi) * layer.scale;
        slopeY += layer.amplitude * std::cos(v * kTwoPi) * layer.scale;
    }
    // The surface is a height field, so its normal is (-dh/dx, -dh/dy, 1).
    // Building it this way keeps the z component positive by construction: a
    // water normal that tips past the horizon lights the surface from
    // beneath it.
    normal = {-slopeX, -slopeY, 1.0f};
    return Normalize(normal);
}

std::array<float, 3> EvaluateWaterColor(
    const WaterMaterialV1& material,
    const float depthBelow) noexcept
{
    auto blend = 0.0f;
    if (material.depthRange > 0.0f && Finite(depthBelow)) {
        // Clamped at both ends: past the range it stays deep rather than
        // continuing into a colour the capture never authored.
        blend = std::clamp(depthBelow / material.depthRange, 0.0f, 1.0f);
    } else if (Finite(depthBelow) && depthBelow > 0.0f) {
        blend = 1.0f;
    }
    std::array<float, 3> colour{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        colour[channel] = material.shallowColor[channel] +
            (material.deepColor[channel] - material.shallowColor[channel]) *
                blend;
    }
    // Silt lifts the colour toward its own; unauthored, it does nothing,
    // which is what keeps clear water clear.
    const auto silt = std::clamp(material.siltDensity, 0.0f, 1.0f);
    if (silt > 0.0f) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            colour[channel] += (material.siltColor[channel] -
                colour[channel]) * silt * blend;
        }
    }
    return colour;
}

float ShorelineCoverage(
    const WaterMaterialV1& material,
    const float depthBelow) noexcept
{
    // Unauthored means the surface does not fade at all, rather than fading
    // over some invented distance.
    if (!(material.shorelineDepth > 0.0f)) return 1.0f;
    if (!Finite(depthBelow) || depthBelow <= 0.0f) return 0.0f;
    return std::clamp(depthBelow / material.shorelineDepth, 0.0f, 1.0f);
}

WaterError ResolveIndexOfRefraction(
    const WaterMaterialV1& material,
    float& ior,
    bool& fromCapture) noexcept
{
    ior = 0.0f;
    fromCapture = false;
    if (material.hasTransmissionMetadata) {
        // Physically impossible values are refused rather than clamped into
        // plausibility: a clamp hides a broken capture behind a number that
        // looks deliberate.
        if (!Finite(material.indexOfRefraction) ||
            material.indexOfRefraction < 1.0f ||
            material.indexOfRefraction > 4.0f) {
            return WaterError::InvalidIor;
        }
        ior = material.indexOfRefraction;
        fromCapture = true;
        return WaterError::None;
    }
    // A documented physical constant, never a value inferred from the
    // rendered image: inference produces a different answer every frame the
    // camera moves, which is indistinguishable from a flickering material.
    switch (material.surfaceClass) {
    case TransmissiveClass::Water:
        ior = kWaterIor;
        break;
    case TransmissiveClass::Glass:
        ior = kGlassIor;
        break;
    case TransmissiveClass::Unknown:
        // Borrows nothing. A wrong constant looks deliberate and is very hard
        // to trace back to a missing capture.
        ior = 0.0f;
        break;
    }
    return WaterError::MissingTransmission;
}

float FresnelReflectance(
    const WaterMaterialV1& material,
    const float cosine) noexcept
{
    float ior = 0.0f;
    bool fromCapture = false;
    const auto resolved = ResolveIndexOfRefraction(material, ior, fromCapture);
    auto f0 = material.fresnelBias;
    if (resolved != WaterError::InvalidIor && ior > 1.0f) {
        const auto ratio = (ior - 1.0f) / (ior + 1.0f);
        f0 = ratio * ratio;
    }
    f0 = std::clamp(f0, 0.0f, 1.0f);
    const auto clamped = std::clamp(cosine, 0.0f, 1.0f);
    const auto complement = 1.0f - clamped;
    const auto squared = complement * complement;
    return std::clamp(f0 + (1.0f - f0) * squared * squared * complement,
        0.0f, 1.0f);
}

ReflectionMode SelectReflection(
    const WaterMaterialV1& material,
    const WaterPolicy& policy,
    const float roughness) noexcept
{
    static_cast<void>(material);
    // Above the cutoff a traced ray costs more than it resolves, exactly as
    // the reflection pass decides. Falling back rather than tracing anyway
    // keeps the cost bounded without losing the reflection entirely.
    if (policy.tracingAvailable && std::isfinite(roughness) &&
        roughness <= policy.tracedReflectionCutoff) {
        return ReflectionMode::Traced;
    }
    if (policy.screenSpaceAvailable) return ReflectionMode::ScreenSpace;
    return ReflectionMode::None;
}

RefractionSource SelectRefraction(
    const WaterMaterialV1& material,
    const WaterPolicy& policy) noexcept
{
    static_cast<void>(material);
    if (policy.tracingAvailable) return RefractionSource::Traced;
    if (policy.screenSpaceAvailable) return RefractionSource::ScreenSpace;
    // Nothing captured what is behind the surface. Saying so beats inventing
    // a background that a comparison would then have to accept.
    return RefractionSource::Unavailable;
}

WaterError ReflectAboutPlane(
    const ReflectionPlane& plane,
    const std::array<float, 3>& position,
    std::array<float, 3>& mirrored) noexcept
{
    mirrored = position;
    auto normal = plane.normal;
    if (!Normalize(normal) || !Finite(plane.height) || !Finite(position)) {
        return WaterError::DegeneratePlane;
    }
    // Signed distance to the plane, then twice that back along the normal. A
    // point on the plane is its own mirror, so a surface never reflects
    // itself to somewhere else.
    const auto distance = Dot(position, normal) - plane.height;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        mirrored[axis] = position[axis] - 2.0f * distance * normal[axis];
    }
    return WaterError::None;
}

UnderwaterState EvaluateUnderwater(
    const ReflectionPlane& plane,
    const std::array<float, 3>& cameraPosition,
    const std::array<float, 3>& previousCameraPosition) noexcept
{
    UnderwaterState state{};
    auto normal = plane.normal;
    if (!Normalize(normal) || !Finite(cameraPosition) ||
        !Finite(previousCameraPosition) || !Finite(plane.height)) {
        return state;
    }
    const auto current = Dot(cameraPosition, normal) - plane.height;
    const auto previous =
        Dot(previousCameraPosition, normal) - plane.height;
    state.submerged = current < 0.0f;
    // Zero at the boundary, so a transition can be driven continuously
    // instead of snapping between two states.
    state.depth = state.submerged ? -current : 0.0f;
    // A crossing invalidates every history that assumed the other medium, so
    // it is reported rather than left for a consumer to detect by comparing
    // two states it may not have kept.
    state.crossedThisFrame = (current < 0.0f) != (previous < 0.0f);
    return state;
}

std::array<float, 3> UnderwaterFog(
    const WaterMaterialV1& material,
    const std::array<float, 3>& incoming,
    const float distanceThroughWater) noexcept
{
    if (!(material.fogDensity > 0.0f) || !Finite(distanceThroughWater) ||
        distanceThroughWater <= 0.0f || !Finite(incoming)) {
        return incoming;
    }
    // Beer-Lambert: transmittance falls exponentially with distance and never
    // reaches zero, so the result approaches the fog colour without ever
    // overshooting past it into a colour the capture never authored.
    const auto transmittance =
        std::exp(-material.fogDensity * distanceThroughWater);
    std::array<float, 3> result{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        result[channel] = material.fogColor[channel] +
            (incoming[channel] - material.fogColor[channel]) * transmittance;
    }
    return result;
}

std::array<float, 3> ShadeWater(
    const WaterMaterialV1& material,
    const std::array<float, 3>& reflected,
    const std::array<float, 3>& refracted,
    const float cosine,
    const float distanceThroughWater) noexcept
{
    const auto fresnel = FresnelReflectance(material, cosine);
    const auto transmitted =
        UnderwaterFog(material, refracted, distanceThroughWater);
    std::array<float, 3> result{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        result[channel] = reflected[channel] * fresnel +
            transmitted[channel] * (1.0f - fresnel);
    }
    return result;
}

const char* ToString(const WaterError error) noexcept
{
    switch (error) {
    case WaterError::None: return "none";
    case WaterError::InvalidIdentity: return "invalid identity";
    case WaterError::NonFiniteSource: return "non-finite source";
    case WaterError::InvalidDepthRange: return "invalid depth range";
    case WaterError::DegeneratePlane: return "degenerate plane";
    case WaterError::InvalidScroll: return "invalid scroll";
    case WaterError::InvalidIor: return "invalid index of refraction";
    case WaterError::MissingTransmission: return "missing transmission";
    }
    return "unknown";
}

const char* ToString(const TransmissiveClass surfaceClass) noexcept
{
    switch (surfaceClass) {
    case TransmissiveClass::Water: return "water";
    case TransmissiveClass::Glass: return "glass";
    case TransmissiveClass::Unknown: return "unknown";
    }
    return "unknown";
}

}
