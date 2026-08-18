#include "renderer_core/EngineReflection.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vf::renderer::reflect {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// The sun occupies a small solid angle, so a missed ray only sees it when it
// is pointed almost straight at it. A broad falloff would light every escaping
// ray with the sun's full radiance and wash the reflection out.
constexpr float kSunLobeExponent = 64.0f;

[[nodiscard]] bool Finite(const std::array<float, 3>& value) noexcept
{
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
        std::isfinite(value[2]);
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

// Duff et al., branchless. The same construction the Phase 16 normal decode
// uses, so a frame cannot end up with two different tangent frames for one
// surface.
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

// One round of a widely used integer mixer. It is used rather than a counter
// because the sequence has to be reproducible from the pixel alone: a
// reflection pass has no cheap way to carry per-pixel state, and the GPU and
// this oracle must land on the same value from the same inputs.
[[nodiscard]] std::uint32_t Mix(std::uint32_t value) noexcept
{
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    return value;
}

[[nodiscard]] float ToUnitFloat(const std::uint32_t bits) noexcept
{
    // 24 bits keeps the result strictly below one, which matters because a
    // sample of exactly one drives the GGX mapping to infinity.
    return static_cast<float>(bits >> 8) * (1.0f / 16777216.0f);
}

}

std::array<float, 3> ComputeF0(
    const std::array<float, 3>& baseColor,
    const float metalness) noexcept
{
    const auto blend = std::clamp(metalness, 0.0f, 1.0f);
    std::array<float, 3> f0{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        f0[channel] = kDielectricF0 +
            (baseColor[channel] - kDielectricF0) * blend;
    }
    return f0;
}

std::array<float, 3> FresnelSchlick(
    const std::array<float, 3>& f0,
    const float cosine) noexcept
{
    const auto clamped = std::clamp(cosine, 0.0f, 1.0f);
    const auto complement = 1.0f - clamped;
    const auto squared = complement * complement;
    const auto scale = squared * squared * complement;
    std::array<float, 3> fresnel{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        fresnel[channel] = f0[channel] + (1.0f - f0[channel]) * scale;
    }
    return fresnel;
}

std::array<float, 3> MirrorDirection(
    const std::array<float, 3>& viewDirection,
    const std::array<float, 3>& normal) noexcept
{
    const auto cosine = Dot(normal, viewDirection);
    std::array<float, 3> mirror{
        2.0f * cosine * normal[0] - viewDirection[0],
        2.0f * cosine * normal[1] - viewDirection[1],
        2.0f * cosine * normal[2] - viewDirection[2]};
    static_cast<void>(Normalize(mirror));
    return mirror;
}

std::array<float, 2> SampleSequence(
    const std::uint32_t pixelX,
    const std::uint32_t pixelY,
    const std::uint32_t frameIndex,
    const std::uint32_t sampleIndex) noexcept
{
    // Each input is mixed with a distinct odd constant before being combined,
    // so swapping a pixel coordinate for a frame index cannot land on the
    // same state and give a whole tile one direction.
    const auto seed = Mix(pixelX * 0x9E3779B1u) ^
        Mix(pixelY * 0x85EBCA77u) ^
        Mix(frameIndex * 0xC2B2AE3Du) ^
        Mix(sampleIndex * 0x27D4EB2Fu);
    return {ToUnitFloat(Mix(seed)), ToUnitFloat(Mix(seed ^ 0x68BC21EBu))};
}

bool SampleReflectionDirection(
    const ReflectionSurface& surface,
    const std::array<float, 2>& sample,
    std::array<float, 3>& direction) noexcept
{
    auto normal = surface.shadingNormal;
    auto view = surface.viewDirection;
    if (!Normalize(normal) || !Normalize(view)) return false;

    const auto roughness = std::clamp(surface.roughness, 0.0f, 1.0f);
    // A zero-roughness lobe is a delta, not the limit of a sampling scheme,
    // so a mirror is answered exactly rather than approached.
    if (!(roughness > 0.0f)) {
        direction = MirrorDirection(view, normal);
        return Dot(direction, normal) > 0.0f;
    }

    // GGX maps the first sample to the half-vector's polar angle. alpha is
    // roughness squared, which is the parameterization the raster lobe
    // already uses; a different one here would make the reflection disagree
    // with the highlight on the same surface.
    const auto alpha = roughness * roughness;
    const auto u1 = std::clamp(sample[0], 0.0f, 0.999999f);
    const auto u2 = std::clamp(sample[1], 0.0f, 0.999999f);
    const auto tangentSquared = alpha * alpha * u1 / (1.0f - u1);
    const auto cosTheta = 1.0f / std::sqrt(1.0f + tangentSquared);
    const auto sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    const auto phi = 2.0f * kPi * u2;

    std::array<float, 3> tangent{};
    std::array<float, 3> bitangent{};
    OrthonormalBasis(normal, tangent, bitangent);
    const auto x = sinTheta * std::cos(phi);
    const auto y = sinTheta * std::sin(phi);
    std::array<float, 3> half{
        tangent[0] * x + bitangent[0] * y + normal[0] * cosTheta,
        tangent[1] * x + bitangent[1] * y + normal[1] * cosTheta,
        tangent[2] * x + bitangent[2] * y + normal[2] * cosTheta};
    if (!Normalize(half)) return false;

    direction = MirrorDirection(view, half);
    // Below the surface. Rejected rather than clamped: clamping piles every
    // rejected direction onto the horizon and draws a bright ring around the
    // surface at grazing angles.
    return Dot(direction, normal) > 0.0f;
}

std::array<float, 3> OrientHitNormal(
    const std::array<float, 3>& normal,
    const std::array<float, 3>& rayDirection,
    const bool twoSided) noexcept
{
    if (!twoSided) return normal;
    if (Dot(normal, rayDirection) <= 0.0f) return normal;
    return {-normal[0], -normal[1], -normal[2]};
}

ReflectError BuildReflectionRay(
    const ReflectionSurface& surface,
    const ReflectionPolicy& policy,
    const std::array<float, 2>& sample,
    ReflectionRay& ray) noexcept
{
    ray = {};
    if (!Finite(surface.position) || !Finite(surface.geometricNormal) ||
        !Finite(surface.shadingNormal) || !Finite(surface.viewDirection)) {
        return ReflectError::NonFiniteSource;
    }
    if (!std::isfinite(surface.roughness) || surface.roughness < 0.0f ||
        surface.roughness > 1.0f) {
        return ReflectError::InvalidRoughness;
    }
    auto shading = surface.shadingNormal;
    auto geometric = surface.geometricNormal;
    if (!Normalize(shading) || !Normalize(geometric)) {
        return ReflectError::DegenerateNormal;
    }
    auto view = surface.viewDirection;
    if (!Normalize(view)) return ReflectError::DegenerateView;

    ray.origin = accel::OffsetRayOrigin(
        surface.position, surface.geometricNormal, 1.0f);
    ray.minimumDistance = std::max(0.0f, policy.minimumDistance);
    ray.maximumDistance = policy.maximumDistance;
    if (!SampleReflectionDirection(surface, sample, ray.direction)) {
        // A rejected sample still owes this pixel a ray. The lobe's centre is
        // the bounded answer; leaving the direction unset would put a hole in
        // the reflection wherever the sampler happened to reject.
        ray.direction = MirrorDirection(view, shading);
    }
    ray.cone.width = 0.0f;
    // The pixel's own footprint plus the lobe's characteristic width. Without
    // the pixel term a minified mirror reads mip 0 at every distance and
    // aliases; without the roughness term the reflection reads texels finer
    // than the lobe it represents.
    ray.cone.spreadAngle = std::max(0.0f, policy.pixelSpreadRadians) +
        surface.roughness * surface.roughness;
    return ReflectError::None;
}

RayCone PropagateCone(const RayCone& cone, const float distance) noexcept
{
    RayCone grown{cone};
    if (std::isfinite(distance) && distance > 0.0f) {
        grown.width = cone.width + cone.spreadAngle * distance;
    }
    return grown;
}

ReflectError SelectMipLevel(
    const RayCone& coneAtHit,
    const HitFootprint& footprint,
    float& level) noexcept
{
    level = 0.0f;
    if (footprint.textureWidth == 0 || footprint.textureHeight == 0) {
        return ReflectError::InvalidTexture;
    }
    if (!std::isfinite(footprint.worldArea) ||
        !std::isfinite(footprint.uvArea) ||
        !(footprint.worldArea > 0.0f) || !(footprint.uvArea > 0.0f)) {
        return ReflectError::InvalidFootprint;
    }
    if (!std::isfinite(coneAtHit.width) || !(coneAtHit.width > 0.0f)) {
        return ReflectError::InvalidFootprint;
    }
    // Texels the cone covers at the hit: its width in world units, converted
    // to UV by the triangle's own ratio, then to texels. The logarithm of
    // that count is the mip level, which is what a derivative would have
    // produced had a hit shader had one.
    const auto texelDimension = std::sqrt(
        static_cast<float>(footprint.textureWidth) *
        static_cast<float>(footprint.textureHeight));
    const auto density = std::sqrt(footprint.uvArea / footprint.worldArea);
    const auto texels = coneAtHit.width * density * texelDimension;
    if (!(texels > 0.0f) || !std::isfinite(texels)) {
        return ReflectError::InvalidFootprint;
    }
    level = std::log2(texels);
    return ReflectError::None;
}

bool TracesReflection(
    const ReflectionSurface& surface,
    const ReflectionPolicy& policy) noexcept
{
    if (!std::isfinite(surface.roughness)) return false;
    return surface.roughness <= policy.roughnessCutoff;
}

ReflectionSource ResolveMiss(
    const lighting::GpuEnvironmentV1& environment,
    const bool probeAvailable) noexcept
{
    // A captured probe is a local measurement and beats the global
    // environment wherever it exists.
    if (probeAvailable) return ReflectionSource::Probe;
    if ((environment.flagsAndCount[0] & lighting::EnvironmentPresent) == 0) {
        return ReflectionSource::Unresolved;
    }
    // Indoors there is no sky for an escaping ray to have come from.
    // Substituting the exterior one is the light leak this rule prevents.
    if ((environment.flagsAndCount[0] & lighting::EnvironmentInterior) != 0) {
        return ReflectionSource::Unresolved;
    }
    return ReflectionSource::Environment;
}

std::array<float, 3> EvaluateMissRadiance(
    const lighting::GpuEnvironmentV1& environment,
    const std::array<float, 3>& direction,
    const std::array<float, 3>& probeRadiance,
    const bool probeAvailable) noexcept
{
    switch (ResolveMiss(environment, probeAvailable)) {
    case ReflectionSource::Probe:
        return probeRadiance;
    case ReflectionSource::Environment:
        break;
    default:
        // Nothing was captured that could say what the ray saw. Zero is
        // visible in the frame; a plausible grey would be accepted by every
        // comparison that follows.
        return {0.0f, 0.0f, 0.0f};
    }

    auto toSun = std::array<float, 3>{
        -environment.sunDirectionAndFogFar[0],
        -environment.sunDirectionAndFogFar[1],
        -environment.sunDirectionAndFogFar[2]};
    std::array<float, 3> radiance{environment.ambientAndFogNear[0],
        environment.ambientAndFogNear[1], environment.ambientAndFogNear[2]};
    auto ray = direction;
    if (!Normalize(ray) || !Normalize(toSun)) return radiance;

    const auto cosine = std::max(0.0f, Dot(ray, toSun));
    const auto lobe = std::pow(cosine, kSunLobeExponent);
    const auto intensity = environment.sunColorAndIntensity[3];
    for (std::size_t channel = 0; channel < 3; ++channel) {
        radiance[channel] +=
            environment.sunColorAndIntensity[channel] * intensity * lobe;
    }
    return radiance;
}

ReflectionHit TraceReflection(
    const std::span<const ReflectionTriangle> triangles,
    const ReflectionRay& ray) noexcept
{
    ReflectionHit nearest{};
    nearest.distance = ray.maximumDistance;
    auto direction = ray.direction;
    if (!Normalize(direction)) return nearest;

    for (const auto& triangle : triangles) {
        const std::array<float, 3> edge1{
            triangle.b[0] - triangle.a[0], triangle.b[1] - triangle.a[1],
            triangle.b[2] - triangle.a[2]};
        const std::array<float, 3> edge2{
            triangle.c[0] - triangle.a[0], triangle.c[1] - triangle.a[1],
            triangle.c[2] - triangle.a[2]};
        const std::array<float, 3> pvec{
            direction[1] * edge2[2] - direction[2] * edge2[1],
            direction[2] * edge2[0] - direction[0] * edge2[2],
            direction[0] * edge2[1] - direction[1] * edge2[0]};
        const auto determinant = Dot(edge1, pvec);
        // Two-sided by construction: the instance disables triangle culling,
        // so a reflection sees the back of a surface exactly as the ray query
        // does. A culled tracer here would disagree with the mirror on every
        // interior face.
        if (std::abs(determinant) < 1.0e-12f) continue;
        const auto inverse = 1.0f / determinant;
        const std::array<float, 3> tvec{
            ray.origin[0] - triangle.a[0], ray.origin[1] - triangle.a[1],
            ray.origin[2] - triangle.a[2]};
        const auto u = Dot(tvec, pvec) * inverse;
        if (u < 0.0f || u > 1.0f) continue;
        const std::array<float, 3> qvec{
            tvec[1] * edge1[2] - tvec[2] * edge1[1],
            tvec[2] * edge1[0] - tvec[0] * edge1[2],
            tvec[0] * edge1[1] - tvec[1] * edge1[0]};
        const auto v = Dot(direction, qvec) * inverse;
        if (v < 0.0f || u + v > 1.0f) continue;
        const auto distance = Dot(edge2, qvec) * inverse;
        if (distance <= ray.minimumDistance || distance >= nearest.distance) {
            continue;
        }
        nearest.hit = true;
        nearest.distance = distance;
        nearest.position = {ray.origin[0] + direction[0] * distance,
            ray.origin[1] + direction[1] * distance,
            ray.origin[2] + direction[2] * distance};
        nearest.normal =
            OrientHitNormal(triangle.normal, direction, triangle.twoSided);
        nearest.albedo = triangle.albedo;
        nearest.objectIndex = triangle.objectIndex;
        nearest.primitiveIndex = triangle.primitiveIndex;
    }
    return nearest;
}

ReflectionResult EvaluateReflection(
    const ReflectionSurface& surface,
    const ReflectionPolicy& policy,
    const std::span<const ReflectionTriangle> triangles,
    const std::span<const lighting::GpuLightRecordV1> lights,
    const lighting::GpuEnvironmentV1& environment,
    const std::array<float, 2>& sample,
    const std::array<float, 3>& probeRadiance,
    const bool probeAvailable) noexcept
{
    ReflectionResult result{};
    // Switched off for the frame. Exactly nothing rather than a small
    // residue, for the same reason the diffuse switch gives nothing: a term
    // that is almost off still moves every pixel it touches, and an isolation
    // built on it measures the remainder rather than the term it removed.
    if ((environment.flagsAndCount[0] &
        lighting::EnvironmentReflectionDisabled) != 0) {
        result.source = ReflectionSource::Skipped;
        return result;
    }
    auto normal = surface.shadingNormal;
    auto view = surface.viewDirection;
    if (!Normalize(normal) || !Normalize(view)) return result;

    const auto f0 = ComputeF0(surface.baseColor, surface.metalness);
    const auto fresnel = FresnelSchlick(f0, std::max(0.0f, Dot(normal, view)));

    const auto apply = [&fresnel](std::array<float, 3> radiance) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            radiance[channel] *= fresnel[channel];
        }
        return radiance;
    };

    // Rougher than the pass can resolve. The environment stands in for a lobe
    // too wide to trace, which is a policy decision and is reported as one
    // rather than disguised as a traced result.
    if (!TracesReflection(surface, policy)) {
        result.radiance = apply(EvaluateMissRadiance(
            environment, MirrorDirection(view, normal), probeRadiance,
            probeAvailable));
        result.source = ReflectionSource::Skipped;
        return result;
    }

    ReflectionRay ray{};
    if (BuildReflectionRay(surface, policy, sample, ray) !=
        ReflectError::None) {
        result.source = ReflectionSource::Unresolved;
        return result;
    }

    const auto hit = TraceReflection(triangles, ray);
    if (hit.hit) {
        // The hit is shadowed by the same geometry the primary surface is.
        // Skipping it would light the reflection of a surface that is in
        // shadow, and the reflection would be brighter than the thing it
        // reflects -- which is the artefact, not a subtlety.
        std::array<float, kMaximumReflectionLights> shadow{};
        const auto count = std::min<std::size_t>(lights.size(), shadow.size());
        for (std::size_t index = 0; index < count; ++index) {
            const auto shadowRay = accel::ShadowRayForLight(
                lights[index], hit.position, hit.normal);
            ReflectionRay toLight{};
            toLight.origin = shadowRay.origin;
            toLight.direction = shadowRay.direction;
            toLight.minimumDistance = shadowRay.minimumDistance;
            toLight.maximumDistance = shadowRay.maximumDistance;
            shadow[index] = (shadowRay.maximumDistance > 0.0f &&
                TraceReflection(triangles, toLight).hit) ? 0.0f : 1.0f;
        }
        // What the hit surface sends back along the ray. Shaded through the
        // same function the raster pass uses, so a reflection cannot disagree
        // with the surface it is reflecting.
        result.radiance = apply(lighting::ShadeSurfaceGpu(
            environment, lights, hit.albedo, hit.position, hit.normal,
            std::span<const float>{shadow.data(), count}));
        result.source = ReflectionSource::Geometry;
        result.hitObjectIndex = hit.objectIndex;
        result.hitPrimitiveIndex = hit.primitiveIndex;
        result.hitDistance = hit.distance;
        return result;
    }

    result.radiance = apply(EvaluateMissRadiance(
        environment, ray.direction, probeRadiance, probeAvailable));
    result.source = ResolveMiss(environment, probeAvailable);
    result.hitDistance = ray.maximumDistance;
    return result;
}

bool ResetHistory(
    const ReflectionHistoryKey& previous,
    const ReflectionHistoryKey& current) noexcept
{
    // A history with no extent was never established, so there is nothing to
    // reuse and nothing to reject.
    if (previous.width == 0 || previous.height == 0) return true;
    return !(previous == current);
}

const char* ToString(const ReflectError error) noexcept
{
    switch (error) {
    case ReflectError::None: return "none";
    case ReflectError::NonFiniteSource: return "non-finite source";
    case ReflectError::DegenerateNormal: return "degenerate normal";
    case ReflectError::DegenerateView: return "degenerate view";
    case ReflectError::InvalidRoughness: return "invalid roughness";
    case ReflectError::InvalidFootprint: return "invalid footprint";
    case ReflectError::InvalidTexture: return "invalid texture";
    }
    return "unknown";
}

const char* ToString(const ReflectionSource source) noexcept
{
    switch (source) {
    case ReflectionSource::Skipped: return "skipped";
    case ReflectionSource::Geometry: return "geometry";
    case ReflectionSource::Environment: return "environment";
    case ReflectionSource::Probe: return "probe";
    case ReflectionSource::Unresolved: return "unresolved";
    }
    return "unknown";
}

}
