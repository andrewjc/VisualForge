#include "renderer_core/EngineLighting.h"

#include "renderer_trace/Crc32.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace vf::renderer::lighting {

namespace {

[[nodiscard]] bool Finite(const float value) noexcept
{
    return std::isfinite(value);
}

template <std::size_t N>
[[nodiscard]] bool Finite(const std::array<float, N>& values) noexcept
{
    for (const auto value : values) {
        if (!std::isfinite(value)) return false;
    }
    return true;
}

template <std::size_t N>
[[nodiscard]] bool Finite(const std::array<double, N>& values) noexcept
{
    for (const auto value : values) {
        if (!std::isfinite(value)) return false;
    }
    return true;
}

[[nodiscard]] bool Normalize(std::array<float, 3>& value) noexcept
{
    const auto lengthSquared = value[0] * value[0] + value[1] * value[1] +
        value[2] * value[2];
    if (!(lengthSquared > 0.0f) || !std::isfinite(lengthSquared)) {
        return false;
    }
    const auto inverse = 1.0f / std::sqrt(lengthSquared);
    for (auto& component : value) component *= inverse;
    return true;
}

[[nodiscard]] std::size_t AlignUp(
    const std::size_t value,
    const std::size_t alignment) noexcept
{
    if (alignment == 0 ||
        value > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

}

bool ShadowTermAvailable() noexcept
{
    // Phase 18 introduces ray-traced visibility. Until then every direct
    // evaluation is unshadowed, and says so, so a parity comparison can mask
    // the term instead of attributing the difference to an error.
    return false;
}

LightError TranslateLight(
    const LightCapture& capture,
    LightRecordV1& record) noexcept
{
    record = {};
    if (capture.lightId == 0) return LightError::InvalidIdentity;
    if (capture.type < LightType::Ambient || capture.type > LightType::Spot) {
        return LightError::InvalidType;
    }
    if (!Finite(capture.diffuse) || !Finite(capture.dimmer) ||
        !Finite(capture.radius) || !Finite(capture.constantAttenuation) ||
        !Finite(capture.linearAttenuation) ||
        !Finite(capture.quadraticAttenuation) ||
        !Finite(capture.direction) || !Finite(capture.position) ||
        !Finite(capture.innerConeRadians) ||
        !Finite(capture.outerConeRadians) || !Finite(capture.spotExponent)) {
        return LightError::NonFiniteSource;
    }
    // A negative dimmer would subtract light from the scene.
    if (capture.dimmer < 0.0f) return LightError::InvalidIntensity;

    const auto positional = capture.type == LightType::Point ||
        capture.type == LightType::Spot;
    if (positional) {
        if (!(capture.radius > 0.0f)) return LightError::InvalidRange;
        if (capture.constantAttenuation < 0.0f ||
            capture.linearAttenuation < 0.0f ||
            capture.quadraticAttenuation < 0.0f) {
            return LightError::InvalidAttenuation;
        }
        // All-zero coefficients divide by zero at every distance.
        if (capture.constantAttenuation == 0.0f &&
            capture.linearAttenuation == 0.0f &&
            capture.quadraticAttenuation == 0.0f) {
            return LightError::InvalidAttenuation;
        }
    }

    auto direction = capture.direction;
    if (capture.type == LightType::Directional ||
        capture.type == LightType::Spot) {
        if (!Normalize(direction)) return LightError::InvalidDirection;
    }

    if (capture.type == LightType::Spot) {
        if (!(capture.outerConeRadians > 0.0f) ||
            capture.innerConeRadians < 0.0f ||
            capture.innerConeRadians > capture.outerConeRadians ||
            capture.outerConeRadians >= 3.14159274f) {
            return LightError::InvalidCone;
        }
        if (!(capture.spotExponent > 0.0f)) return LightError::InvalidCone;
    }

    record.lightId = capture.lightId;
    record.type = static_cast<std::uint8_t>(capture.type);
    record.flags =
        (capture.castsShadows ? LightCastsShadows : 0u) |
        (capture.portalStrict ? LightPortalStrict : 0u) |
        (capture.neverFades ? LightNeverFades : 0u);
    // The dimmer scales radiance and is kept separate from the colour, so a
    // consumer can still read back the authored colour.
    for (std::size_t channel = 0; channel < 3; ++channel) {
        record.color[channel] = capture.diffuse[channel];
        record.direction[channel] = direction[channel];
        record.position[channel] = capture.position[channel];
    }
    record.intensity = capture.dimmer;
    record.radius = positional ? capture.radius : 0.0f;
    record.constantAttenuation = capture.constantAttenuation;
    record.linearAttenuation = capture.linearAttenuation;
    record.quadraticAttenuation = capture.quadraticAttenuation;
    // Cones are stored as cosines because that is what a shader compares
    // against a dot product; converting per fragment would be arithmetic
    // repeated for nothing.
    record.innerConeCosine = capture.type == LightType::Spot
        ? std::cos(capture.innerConeRadians) : 1.0f;
    record.outerConeCosine = capture.type == LightType::Spot
        ? std::cos(capture.outerConeRadians) : -1.0f;
    record.spotExponent = capture.spotExponent;
    return LightError::None;
}

float EvaluateAttenuation(
    const LightRecordV1& record,
    const float distance) noexcept
{
    const auto type = static_cast<LightType>(record.type);
    // Distance is meaningless for a light with no position.
    if (type == LightType::Directional || type == LightType::Ambient) {
        return 1.0f;
    }
    if (!std::isfinite(distance) || distance < 0.0f) return 0.0f;
    // Outside the captured radius the light contributes nothing, which is
    // what lets a light list be culled by radius without changing the image.
    if (distance >= record.radius) return 0.0f;
    const auto denominator = record.constantAttenuation +
        record.linearAttenuation * distance +
        record.quadraticAttenuation * distance * distance;
    if (!(denominator > 0.0f)) return 0.0f;
    return std::clamp(1.0f / denominator, 0.0f, 1.0f);
}

float EvaluateCone(
    const LightRecordV1& record,
    const float angleRadians) noexcept
{
    if (static_cast<LightType>(record.type) != LightType::Spot) return 1.0f;
    if (!std::isfinite(angleRadians)) return 0.0f;
    const auto cosine = std::cos(std::abs(angleRadians));
    if (cosine >= record.innerConeCosine) return 1.0f;
    if (cosine <= record.outerConeCosine) return 0.0f;
    const auto span = record.innerConeCosine - record.outerConeCosine;
    if (!(span > 0.0f)) return 0.0f;
    const auto t = (cosine - record.outerConeCosine) / span;
    return std::clamp(std::pow(t, record.spotExponent), 0.0f, 1.0f);
}

std::array<float, 3> EvaluateRadiance(const LightRecordV1& record) noexcept
{
    return {record.color[0] * record.intensity,
        record.color[1] * record.intensity,
        record.color[2] * record.intensity};
}

bool Contributes(const LightRecordV1& record) noexcept
{
    const auto radiance = EvaluateRadiance(record);
    return radiance[0] > 0.0f || radiance[1] > 0.0f || radiance[2] > 0.0f;
}

DirectLighting EvaluateDirect(
    const LightRecordV1& record,
    const SurfacePoint& surface) noexcept
{
    DirectLighting result{};
    result.available = ShadowTermAvailable();
    auto normal = surface.normal;
    if (!Normalize(normal)) return result;

    const auto type = static_cast<LightType>(record.type);
    std::array<float, 3> toLight{};
    auto distance = 0.0f;
    if (type == LightType::Directional) {
        // The record stores the direction the light travels, so the vector
        // toward it is the negation.
        toLight = {-record.direction[0], -record.direction[1],
            -record.direction[2]};
    } else if (type == LightType::Ambient) {
        const auto radiance = EvaluateRadiance(record);
        result.radiance = radiance;
        return result;
    } else {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            toLight[axis] = static_cast<float>(record.position[axis]) -
                surface.position[axis];
        }
        distance = std::sqrt(toLight[0] * toLight[0] +
            toLight[1] * toLight[1] + toLight[2] * toLight[2]);
    }
    if (!Normalize(toLight)) return result;

    // Clamped at zero: a surface facing away receives nothing, and a
    // negative cosine would subtract light from the frame.
    const auto cosine = std::max(0.0f,
        normal[0] * toLight[0] + normal[1] * toLight[1] +
            normal[2] * toLight[2]);
    auto scale = cosine * EvaluateAttenuation(record, distance);
    if (type == LightType::Spot) {
        const auto axisDot = -(record.direction[0] * toLight[0] +
            record.direction[1] * toLight[1] +
            record.direction[2] * toLight[2]);
        scale *= EvaluateCone(record,
            std::acos(std::clamp(axisDot, -1.0f, 1.0f)));
    }
    const auto radiance = EvaluateRadiance(record);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        result.radiance[channel] = radiance[channel] * scale;
    }
    return result;
}

LightError TranslateEnvironment(
    const EnvironmentCapture& capture,
    EnvironmentRecordV1& record) noexcept
{
    record = {};
    if (!Finite(capture.ambient) || !Finite(capture.sunDirection) ||
        !Finite(capture.sunColor) || !Finite(capture.sunIntensity) ||
        !Finite(capture.moonDirection) || !Finite(capture.moonColor) ||
        !Finite(capture.moonIntensity) ||
        !Finite(capture.fog.nearDistance) ||
        !Finite(capture.fog.farDistance) || !Finite(capture.fog.color) ||
        !Finite(capture.fog.power) || !Finite(capture.fog.maximum)) {
        return LightError::NonFiniteSource;
    }
    if (capture.sunIntensity < 0.0f || capture.moonIntensity < 0.0f) {
        return LightError::InvalidIntensity;
    }
    auto sun = capture.sunDirection;
    auto moon = capture.moonDirection;
    if (!Normalize(sun) || !Normalize(moon)) {
        return LightError::InvalidDirection;
    }
    if (!(capture.fog.farDistance > capture.fog.nearDistance) ||
        capture.fog.nearDistance < 0.0f || !(capture.fog.power > 0.0f) ||
        capture.fog.maximum < 0.0f || capture.fog.maximum > 1.0f) {
        return LightError::InvalidFogRange;
    }

    record.flags = capture.interior ? EnvironmentInterior : 0u;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        record.ambient[channel] = capture.ambient[channel];
        record.sunDirection[channel] = sun[channel];
        record.sunColor[channel] = capture.sunColor[channel];
        record.moonDirection[channel] = moon[channel];
        record.moonColor[channel] = capture.moonColor[channel];
        record.fogColor[channel] = capture.fog.color[channel];
    }
    // An interior has no sun or moon. Carrying an exterior's sun into an
    // interior is precisely the stale-state bug the phase gate names, so it
    // is zeroed here rather than trusted from the capture.
    record.sunIntensity = capture.interior ? 0.0f : capture.sunIntensity;
    record.moonIntensity = capture.interior ? 0.0f : capture.moonIntensity;
    record.fogNear = capture.fog.nearDistance;
    record.fogFar = capture.fog.farDistance;
    record.fogPower = capture.fog.power;
    record.fogMaximum = capture.fog.maximum;
    return LightError::None;
}

float EvaluateFog(
    const EnvironmentRecordV1& record,
    const float distance) noexcept
{
    if (!std::isfinite(distance) || distance <= record.fogNear) return 0.0f;
    const auto span = record.fogFar - record.fogNear;
    if (!(span > 0.0f)) return 0.0f;
    const auto t = std::clamp((distance - record.fogNear) / span, 0.0f, 1.0f);
    // Saturates at the captured maximum rather than at one, or distant
    // geometry would disappear into fog the engine never applied.
    return std::clamp(std::pow(t, record.fogPower) * record.fogMaximum,
        0.0f, record.fogMaximum);
}

LightError BlendEnvironment(
    const EnvironmentRecordV1& from,
    const EnvironmentRecordV1& to,
    const float factor,
    EnvironmentRecordV1& blended) noexcept
{
    blended = {};
    if (!std::isfinite(factor) || factor < 0.0f || factor > 1.0f) {
        return LightError::InvalidTransition;
    }
    // The engine cuts between interior and exterior; it does not cross-fade.
    // Blending them would invent a state the game never shows.
    if (((from.flags ^ to.flags) & EnvironmentInterior) != 0) {
        return LightError::IncompatibleEnvironments;
    }
    const auto mix = [factor](const float a, const float b) {
        return a + (b - a) * factor;
    };
    blended.flags = from.flags;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        blended.ambient[channel] =
            mix(from.ambient[channel], to.ambient[channel]);
        blended.sunColor[channel] =
            mix(from.sunColor[channel], to.sunColor[channel]);
        blended.moonColor[channel] =
            mix(from.moonColor[channel], to.moonColor[channel]);
        blended.fogColor[channel] =
            mix(from.fogColor[channel], to.fogColor[channel]);
        blended.sunDirection[channel] =
            mix(from.sunDirection[channel], to.sunDirection[channel]);
        blended.moonDirection[channel] =
            mix(from.moonDirection[channel], to.moonDirection[channel]);
    }
    std::array<float, 3> sun{blended.sunDirection[0], blended.sunDirection[1],
        blended.sunDirection[2]};
    std::array<float, 3> moon{blended.moonDirection[0],
        blended.moonDirection[1], blended.moonDirection[2]};
    // Interpolating two unit vectors shortens them, so they are renormalized
    // rather than left to dim the light in the middle of a transition.
    if (Normalize(sun)) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            blended.sunDirection[axis] = sun[axis];
        }
    }
    if (Normalize(moon)) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            blended.moonDirection[axis] = moon[axis];
        }
    }
    blended.sunIntensity = mix(from.sunIntensity, to.sunIntensity);
    blended.moonIntensity = mix(from.moonIntensity, to.moonIntensity);
    blended.fogNear = mix(from.fogNear, to.fogNear);
    blended.fogFar = mix(from.fogFar, to.fogFar);
    blended.fogPower = mix(from.fogPower, to.fogPower);
    blended.fogMaximum = mix(from.fogMaximum, to.fogMaximum);
    return LightError::None;
}

LightError BuildGpuLight(
    const LightRecordV1& record,
    const std::array<double, 3>& cameraOrigin,
    GpuLightRecordV1& gpu) noexcept
{
    gpu = {};
    if (!Finite(cameraOrigin)) return LightError::NonFiniteSource;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        // The subtraction happens in double; only the small residual is
        // narrowed. Narrowing first would lose whole units at exterior
        // coordinates.
        const auto relative = record.position[axis] - cameraOrigin[axis];
        if (!std::isfinite(relative) ||
            std::abs(relative) > kMaximumCameraRelativeDistance) {
            return LightError::PositionOutOfRange;
        }
        gpu.position[axis] = static_cast<float>(relative);
        gpu.direction[axis] = record.direction[axis];
    }
    const auto radiance = EvaluateRadiance(record);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        gpu.color[channel] = radiance[channel];
    }
    gpu.color[3] = static_cast<float>(record.type);
    gpu.position[3] = record.radius;
    gpu.direction[3] = static_cast<float>(record.flags);
    gpu.attenuation[0] = record.constantAttenuation;
    gpu.attenuation[1] = record.linearAttenuation;
    gpu.attenuation[2] = record.quadraticAttenuation;
    gpu.attenuation[3] = record.radius;
    gpu.cone[0] = record.innerConeCosine;
    gpu.cone[1] = record.outerConeCosine;
    gpu.cone[2] = record.spotExponent;
    return LightError::None;
}

GpuEnvironmentV1 BuildGpuEnvironment(
    const EnvironmentRecordV1& record,
    const std::uint32_t activeLightCount,
    const std::uint32_t indirectRays) noexcept
{
    GpuEnvironmentV1 gpu{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        gpu.ambientAndFogNear[channel] = record.ambient[channel];
        gpu.sunDirectionAndFogFar[channel] = record.sunDirection[channel];
        gpu.sunColorAndIntensity[channel] = record.sunColor[channel];
        gpu.moonDirectionAndIntensity[channel] = record.moonDirection[channel];
        gpu.moonColorAndFogMaximum[channel] = record.moonColor[channel];
        gpu.fogColorAndPower[channel] = record.fogColor[channel];
    }
    gpu.ambientAndFogNear[3] = record.fogNear;
    gpu.sunDirectionAndFogFar[3] = record.fogFar;
    gpu.sunColorAndIntensity[3] = record.sunIntensity;
    gpu.moonDirectionAndIntensity[3] = record.moonIntensity;
    gpu.moonColorAndFogMaximum[3] = record.fogMaximum;
    gpu.fogColorAndPower[3] = record.fogPower;
    gpu.flagsAndCount[0] = record.flags | EnvironmentPresent;
    gpu.flagsAndCount[1] = activeLightCount;
    gpu.flagsAndCount[2] = indirectRays;
    return gpu;
}

std::array<float, 3> EvaluateDirectGpu(
    const GpuLightRecordV1& light,
    const std::array<float, 3>& position,
    const std::array<float, 3>& normal) noexcept
{
    const auto classification = ClassifyGpuLight(light);
    const auto type = static_cast<std::uint32_t>(classification);
    const std::array<float, 3> radiance{
        light.color[0], light.color[1], light.color[2]};
    if (classification == LightType::Ambient) {
        return radiance;
    }
    std::array<float, 3> toLight{};
    auto distance = 0.0f;
    if (type == static_cast<std::uint32_t>(LightType::Directional)) {
        toLight = {-light.direction[0], -light.direction[1],
            -light.direction[2]};
    } else {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            toLight[axis] = light.position[axis] - position[axis];
        }
        distance = std::sqrt(toLight[0] * toLight[0] +
            toLight[1] * toLight[1] + toLight[2] * toLight[2]);
    }
    const auto toLightLength = std::sqrt(toLight[0] * toLight[0] +
        toLight[1] * toLight[1] + toLight[2] * toLight[2]);
    if (!(toLightLength > 0.0f)) return {};
    for (auto& component : toLight) component /= toLightLength;

    const auto cosine = std::max(0.0f,
        normal[0] * toLight[0] + normal[1] * toLight[1] +
            normal[2] * toLight[2]);
    auto attenuation = 1.0f;
    if (type != static_cast<std::uint32_t>(LightType::Directional)) {
        const auto radius = light.attenuation[3];
        if (distance >= radius) {
            attenuation = 0.0f;
        } else {
            const auto denominator = light.attenuation[0] +
                light.attenuation[1] * distance +
                light.attenuation[2] * distance * distance;
            attenuation = denominator > 0.0f
                ? std::clamp(1.0f / denominator, 0.0f, 1.0f) : 0.0f;
        }
    }
    auto scale = cosine * attenuation;
    if (type == static_cast<std::uint32_t>(LightType::Spot)) {
        const auto axisCosine = -(light.direction[0] * toLight[0] +
            light.direction[1] * toLight[1] +
            light.direction[2] * toLight[2]);
        const auto inner = light.cone[0];
        const auto outer = light.cone[1];
        auto cone = 0.0f;
        if (axisCosine >= inner) {
            cone = 1.0f;
        } else if (axisCosine > outer) {
            const auto span = inner - outer;
            cone = span > 0.0f
                ? std::clamp(std::pow((axisCosine - outer) / span,
                      light.cone[2]), 0.0f, 1.0f)
                : 0.0f;
        }
        scale *= cone;
    }
    return {radiance[0] * scale, radiance[1] * scale, radiance[2] * scale};
}

float EvaluateFogGpu(
    const GpuEnvironmentV1& environment,
    const float distance) noexcept
{
    const auto fogNear = environment.ambientAndFogNear[3];
    const auto fogFar = environment.sunDirectionAndFogFar[3];
    const auto fogPower = environment.fogColorAndPower[3];
    const auto fogMaximum = environment.moonColorAndFogMaximum[3];
    if (!std::isfinite(distance) || distance <= fogNear) return 0.0f;
    const auto span = fogFar - fogNear;
    if (!(span > 0.0f)) return 0.0f;
    const auto t = std::clamp((distance - fogNear) / span, 0.0f, 1.0f);
    return std::clamp(std::pow(t, fogPower) * fogMaximum, 0.0f, fogMaximum);
}

std::array<float, 3> ShadeSurfaceGpu(
    const GpuEnvironmentV1& environment,
    const std::span<const GpuLightRecordV1> lights,
    const std::array<float, 3>& albedo,
    const std::array<float, 3>& position,
    const std::array<float, 3>& normal) noexcept
{
    return ShadeSurfaceGpu(
        environment, lights, albedo, position, normal, {});
}

std::array<float, 3> ShadeSurfaceGpu(
    const GpuEnvironmentV1& environment,
    const std::span<const GpuLightRecordV1> lights,
    const std::array<float, 3>& albedo,
    const std::array<float, 3>& position,
    const std::array<float, 3>& normal,
    const std::span<const float> shadow) noexcept
{
    // A frame with no captured lighting leaves the albedo alone, exactly as
    // every phase before this one did.
    if ((environment.flagsAndCount[0] & EnvironmentPresent) == 0) {
        return albedo;
    }
    std::array<float, 3> lit{environment.ambientAndFogNear[0],
        environment.ambientAndFogNear[1], environment.ambientAndFogNear[2]};
    const auto count = std::min<std::size_t>(
        lights.size(), environment.flagsAndCount[1]);
    for (std::size_t index = 0; index < count; ++index) {
        auto direct = EvaluateDirectGpu(lights[index], position, normal);
        // Ambient is not cast from anywhere, so no ray of it can be blocked.
        if (index < shadow.size() &&
            ClassifyGpuLight(lights[index]) != LightType::Ambient) {
            for (auto& channel : direct) channel *= shadow[index];
        }
        for (std::size_t channel = 0; channel < 3; ++channel) {
            lit[channel] += direct[channel];
        }
    }
    std::array<float, 3> shaded{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        shaded[channel] = albedo[channel] * lit[channel];
    }
    const auto fog = EvaluateFogGpu(environment,
        std::sqrt(position[0] * position[0] + position[1] * position[1] +
            position[2] * position[2]));
    for (std::size_t channel = 0; channel < 3; ++channel) {
        shaded[channel] += (environment.fogColorAndPower[channel] -
            shaded[channel]) * fog;
    }
    return shaded;
}

LightError SelectActiveLights(
    const LightSet& set,
    const std::array<double, 3>& cameraOrigin,
    LightSelection& selection) noexcept
{
    selection = {};
    if (set.lights.size() > kMaximumCapturedLights) {
        return LightError::TooManyLights;
    }
    try {
        struct Ranked
        {
            std::uint64_t lightId;
            double score;
            std::size_t order;
        };
        std::vector<Ranked> ranked;
        ranked.reserve(set.lights.size());
        for (std::size_t index = 0; index < set.lights.size(); ++index) {
            const auto& light = set.lights[index];
            if (!Contributes(light)) continue;
            const auto radiance = EvaluateRadiance(light);
            const auto brightness = static_cast<double>(radiance[0]) +
                radiance[1] + radiance[2];
            auto distanceSquared = 0.0;
            const auto type = static_cast<LightType>(light.type);
            if (type == LightType::Point || type == LightType::Spot) {
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const auto delta =
                        light.position[axis] - cameraOrigin[axis];
                    distanceSquared += delta * delta;
                }
            }
            // Brightness falling off with distance, which is the same order
            // a viewer would notice a light disappearing in.
            const auto score = brightness / (1.0 + distanceSquared);
            ranked.push_back({light.lightId, score, index});
        }
        // Ties break on the captured order and then on identity, so the same
        // scene always selects the same lights. A comparison that left ties
        // unresolved would let a static scene flicker.
        std::stable_sort(ranked.begin(), ranked.end(),
            [](const Ranked& a, const Ranked& b) {
                if (a.score != b.score) return a.score > b.score;
                if (a.order != b.order) return a.order < b.order;
                return a.lightId < b.lightId;
            });
        const auto keep = std::min<std::size_t>(
            ranked.size(), kMaximumActiveLights);
        selection.selected.reserve(keep);
        for (std::size_t index = 0; index < keep; ++index) {
            selection.selected.push_back(ranked[index].lightId);
        }
        selection.droppedCount =
            static_cast<std::uint32_t>(ranked.size() - keep);
        selection.overflowed = selection.droppedCount != 0;
        return LightError::None;
    } catch (const std::bad_alloc&) {
        selection = {};
        return LightError::TooManyLights;
    }
}

LightPacketError ValidateLightPacket(const LightPacket& packet) noexcept
{
    if (packet.lights.size() > kMaximumCapturedLights) {
        return LightPacketError::TooManyLights;
    }
    for (std::size_t index = 0; index < packet.lights.size(); ++index) {
        const auto& light = packet.lights[index];
        if (light.lightId == 0) return LightPacketError::InvalidIdentity;
        if (light.type > static_cast<std::uint8_t>(LightType::Spot)) {
            return LightPacketError::InvalidLight;
        }
        for (const auto pad : light.reserved0) {
            if (pad != 0) return LightPacketError::NonZeroPadding;
        }
        if (light.reserved1 != 0) return LightPacketError::NonZeroPadding;
        for (std::size_t other = 0; other < index; ++other) {
            if (packet.lights[other].lightId == light.lightId) {
                return LightPacketError::DuplicateLight;
            }
        }
    }
    if (!(packet.environment.fogFar > packet.environment.fogNear) ||
        packet.environment.reserved0 != 0) {
        return LightPacketError::InvalidEnvironment;
    }
    return LightPacketError::None;
}

LightPacketError EncodeLightPacket(
    const LightPacket& packet,
    std::vector<std::byte>& bytes) noexcept
{
    bytes.clear();
    const auto validation = ValidateLightPacket(packet);
    if (validation != LightPacketError::None) return validation;
    try {
        LightPacketHeaderV1 header{};
        header.frameId = packet.header.frameId;
        header.viewId = packet.header.viewId;
        header.lightCount = static_cast<std::uint32_t>(packet.lights.size());
        const auto lightsOffset =
            AlignUp(sizeof(LightPacketHeaderV1), alignof(LightRecordV1));
        const auto lightBytes =
            packet.lights.size() * sizeof(LightRecordV1);
        const auto environmentOffset =
            AlignUp(lightsOffset + lightBytes, alignof(EnvironmentRecordV1));
        const auto totalSize =
            environmentOffset + sizeof(EnvironmentRecordV1);
        if (totalSize > std::numeric_limits<std::uint32_t>::max()) {
            return LightPacketError::AllocationFailure;
        }
        header.lightsOffset = static_cast<std::uint32_t>(lightsOffset);
        header.environmentOffset =
            static_cast<std::uint32_t>(environmentOffset);
        header.totalSize = static_cast<std::uint32_t>(totalSize);
        bytes.resize(totalSize);
        if (lightBytes != 0) {
            std::memcpy(bytes.data() + lightsOffset, packet.lights.data(),
                lightBytes);
        }
        std::memcpy(bytes.data() + environmentOffset, &packet.environment,
            sizeof(EnvironmentRecordV1));
        header.payloadCrc32 = trace::Crc32(
            std::span<const std::byte>{bytes}.subspan(sizeof(header)));
        std::memcpy(bytes.data(), &header, sizeof(header));
        return LightPacketError::None;
    } catch (const std::bad_alloc&) {
        bytes.clear();
        return LightPacketError::AllocationFailure;
    }
}

LightPacketError DecodeLightPacket(
    const std::span<const std::byte> bytes,
    LightPacket& packet) noexcept
{
    packet = {};
    if (bytes.size() < sizeof(LightPacketHeaderV1)) {
        return LightPacketError::TruncatedHeader;
    }
    LightPacketHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kLightPacketMagic) {
        return LightPacketError::BadMagic;
    }
    if (header.endianMarker != kLightPacketEndian) {
        return LightPacketError::WrongEndian;
    }
    if (header.versionMajor != kLightPacketVersionMajor ||
        header.versionMinor > kLightPacketVersionMinor) {
        return LightPacketError::UnsupportedVersion;
    }
    if (header.headerSize != sizeof(LightPacketHeaderV1) ||
        header.totalSize != bytes.size()) {
        return LightPacketError::SizeMismatch;
    }
    if (header.lightCount > kMaximumCapturedLights) {
        return LightPacketError::TooManyLights;
    }
    if (header.lightsOffset % alignof(LightRecordV1) != 0 ||
        header.environmentOffset % alignof(EnvironmentRecordV1) != 0) {
        return LightPacketError::MisalignedSection;
    }
    const auto lightBytes =
        static_cast<std::size_t>(header.lightCount) * sizeof(LightRecordV1);
    if (header.lightsOffset < sizeof(LightPacketHeaderV1) ||
        header.lightsOffset + lightBytes > bytes.size() ||
        header.environmentOffset + sizeof(EnvironmentRecordV1) >
            bytes.size() ||
        header.environmentOffset < header.lightsOffset + lightBytes) {
        return LightPacketError::SectionOutOfBounds;
    }
    if (trace::Crc32(bytes.subspan(sizeof(header))) != header.payloadCrc32) {
        return LightPacketError::ChecksumMismatch;
    }
    if (header.reserved0 != 0 || header.reserved1 != 0) {
        return LightPacketError::NonZeroPadding;
    }
    try {
        packet.header = header;
        packet.lights.resize(header.lightCount);
        if (lightBytes != 0) {
            std::memcpy(packet.lights.data(),
                bytes.data() + header.lightsOffset, lightBytes);
        }
        std::memcpy(&packet.environment,
            bytes.data() + header.environmentOffset,
            sizeof(EnvironmentRecordV1));
    } catch (const std::bad_alloc&) {
        packet = {};
        return LightPacketError::AllocationFailure;
    }
    const auto validation = ValidateLightPacket(packet);
    if (validation != LightPacketError::None) {
        packet = {};
        return validation;
    }
    return LightPacketError::None;
}

const char* ToString(const LightError error) noexcept
{
    switch (error) {
    case LightError::None: return "None";
    case LightError::InvalidIdentity: return "InvalidIdentity";
    case LightError::InvalidType: return "InvalidType";
    case LightError::InvalidRange: return "InvalidRange";
    case LightError::InvalidAttenuation: return "InvalidAttenuation";
    case LightError::InvalidCone: return "InvalidCone";
    case LightError::InvalidIntensity: return "InvalidIntensity";
    case LightError::InvalidDirection: return "InvalidDirection";
    case LightError::InvalidFogRange: return "InvalidFogRange";
    case LightError::InvalidTransition: return "InvalidTransition";
    case LightError::IncompatibleEnvironments:
        return "IncompatibleEnvironments";
    case LightError::PositionOutOfRange: return "PositionOutOfRange";
    case LightError::NonFiniteSource: return "NonFiniteSource";
    case LightError::TooManyLights: return "TooManyLights";
    }
    return "Unknown";
}

const char* ToString(const LightPacketError error) noexcept
{
    switch (error) {
    case LightPacketError::None: return "None";
    case LightPacketError::TruncatedHeader: return "TruncatedHeader";
    case LightPacketError::BadMagic: return "BadMagic";
    case LightPacketError::UnsupportedVersion: return "UnsupportedVersion";
    case LightPacketError::WrongEndian: return "WrongEndian";
    case LightPacketError::SizeMismatch: return "SizeMismatch";
    case LightPacketError::ChecksumMismatch: return "ChecksumMismatch";
    case LightPacketError::SectionOutOfBounds: return "SectionOutOfBounds";
    case LightPacketError::MisalignedSection: return "MisalignedSection";
    case LightPacketError::NonZeroPadding: return "NonZeroPadding";
    case LightPacketError::TooManyLights: return "TooManyLights";
    case LightPacketError::InvalidIdentity: return "InvalidIdentity";
    case LightPacketError::DuplicateLight: return "DuplicateLight";
    case LightPacketError::InvalidLight: return "InvalidLight";
    case LightPacketError::InvalidEnvironment: return "InvalidEnvironment";
    case LightPacketError::AllocationFailure: return "AllocationFailure";
    }
    return "Unknown";
}

}
