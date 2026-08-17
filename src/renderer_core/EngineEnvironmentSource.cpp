#include "EngineEnvironmentSource.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace vf::renderer::environment {
namespace {

[[nodiscard]] const shader::ReflectedBuffer* FindBuffer(
    const shader::ReflectedShader& reflection,
    const std::string_view name) noexcept
{
    for (const auto& buffer : reflection.buffers) {
        if (buffer.name == name) {
            return &buffer;
        }
    }
    return nullptr;
}

// By name, never by position. Reflection lists variables in whatever order the
// compiler emitted them, so an index that works for one shader silently reads
// a different field in the next.
[[nodiscard]] const shader::ReflectedVariable* FindVariable(
    const shader::ReflectedBuffer& buffer,
    const std::string_view name) noexcept
{
    for (const auto& variable : buffer.variables) {
        if (variable.name == name) {
            return &variable;
        }
    }
    return nullptr;
}

constexpr std::uint32_t kVectorBytes = 3 * sizeof(float);

// Reads three floats, refusing anything that would read outside the field the
// declaration describes or outside the buffer that holds it.
[[nodiscard]] bool ReadVector(
    const shader::ReflectedVariable& variable,
    std::span<const std::byte> contents,
    float (&values)[3]) noexcept
{
    if (variable.size < kVectorBytes) {
        return false;
    }
    if (variable.offset % sizeof(float) != 0) {
        return false;
    }
    if (variable.offset > contents.size() ||
        contents.size() - variable.offset < kVectorBytes) {
        return false;
    }
    std::memcpy(values, contents.data() + variable.offset, kVectorBytes);
    return true;
}

}

EnvironmentSourceError ReadDirectionalLight(
    const shader::ReflectedShader& reflection,
    const std::string_view bufferName,
    std::span<const std::byte> contents,
    lighting::EnvironmentRecordV1& environment) noexcept
{
    const auto* const buffer = FindBuffer(reflection, bufferName);
    if (buffer == nullptr) {
        return EnvironmentSourceError::BufferNotFound;
    }
    // A sample is matched to a layout by width, so contents of another size
    // are another buffer. Reading them at these offsets is the exact mistake
    // this path exists to stop making.
    if (buffer->size != contents.size()) {
        return EnvironmentSourceError::ContentsMismatched;
    }

    const auto* const directionField = FindVariable(*buffer, kDirectionFieldName);
    const auto* const colourField = FindVariable(*buffer, kColourFieldName);
    if (directionField == nullptr || colourField == nullptr) {
        return EnvironmentSourceError::FieldMissing;
    }

    float direction[3]{};
    float colour[3]{};
    if (!ReadVector(*directionField, contents, direction) ||
        !ReadVector(*colourField, contents, colour)) {
        return EnvironmentSourceError::FieldTruncated;
    }

    for (const float component : direction) {
        if (!std::isfinite(component)) {
            return EnvironmentSourceError::DirectionDegenerate;
        }
    }
    const auto lengthSquared = direction[0] * direction[0] +
        direction[1] * direction[1] + direction[2] * direction[2];
    if (lengthSquared <= 0.0f) {
        return EnvironmentSourceError::DirectionDegenerate;
    }
    // The check that makes this a measurement. Three floats at the right
    // offset are a direction only if they have unit length; without it any
    // buffer of the right width yields a confident sun pointing wherever its
    // bytes happen to point.
    if (std::abs(std::sqrt(lengthSquared) - 1.0f) > kDirectionTolerance) {
        return EnvironmentSourceError::DirectionNotUnit;
    }

    for (const float component : colour) {
        if (!std::isfinite(component) || component < 0.0f) {
            return EnvironmentSourceError::ColourInvalid;
        }
    }

    environment.sunDirection[0] = direction[0];
    environment.sunDirection[1] = direction[1];
    environment.sunDirection[2] = direction[2];
    environment.sunDirection[3] = 0.0f;
    environment.sunColor[0] = colour[0];
    environment.sunColor[1] = colour[1];
    environment.sunColor[2] = colour[2];
    // The engine publishes a colour and no separate intensity. Scaling by
    // anything but one would light the scene by a number nobody measured.
    environment.sunIntensity = 1.0f;

    // This buffer carries no fog. The packet requires an ordered range, so
    // one is supplied -- with a maximum of zero, which is what makes it "no
    // fog" rather than "fog over distances nobody measured". EvaluateFog
    // scales by the maximum, so this contributes exactly nothing at any
    // depth. The range itself is the smallest ordered pair that satisfies the
    // invariant and is never reached by the evaluation.
    environment.fogNear = 0.0f;
    environment.fogFar = 1.0f;
    environment.fogPower = 1.0f;
    environment.fogMaximum = 0.0f;

    // Sun only. Nothing in this block is an ambient term, and inventing one
    // to stop shadowed faces going black would light the whole scene by a
    // number nobody measured. Left at zero until an ambient is found.
    environment.ambient[0] = 0.0f;
    environment.ambient[1] = 0.0f;
    environment.ambient[2] = 0.0f;

    environment.flags |= lighting::EnvironmentPresent;
    return EnvironmentSourceError::None;
}

EnvironmentSourceError MakeDirectionalLight(
    const lighting::EnvironmentRecordV1& environment,
    const std::uint64_t lightId,
    lighting::LightRecordV1& light) noexcept
{
    // An identity of zero is refused by the packet, and a caller that passed
    // one would otherwise learn about it much further downstream.
    if (lightId == 0) {
        return EnvironmentSourceError::FieldMissing;
    }
    if ((environment.flags & lighting::EnvironmentPresent) == 0) {
        return EnvironmentSourceError::NoCandidate;
    }

    // Re-checked rather than assumed. This function is reachable with any
    // record, including one a caller assembled itself, and a light built from
    // a direction that is not one shades the whole scene from nowhere.
    float lengthSquared = 0.0f;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto component = environment.sunDirection[axis];
        if (!std::isfinite(component)) {
            return EnvironmentSourceError::DirectionDegenerate;
        }
        lengthSquared += component * component;
    }
    if (lengthSquared <= 0.0f) {
        return EnvironmentSourceError::DirectionDegenerate;
    }
    if (std::abs(std::sqrt(lengthSquared) - 1.0f) > kDirectionTolerance) {
        return EnvironmentSourceError::DirectionNotUnit;
    }

    light = {};
    light.lightId = lightId;
    light.type = static_cast<std::uint8_t>(lighting::LightType::Directional);
    light.intensity = environment.sunIntensity;
    // Negated. LightRecordV1 stores the direction light travels --
    // EvaluateDirectGpu computes toLight as -direction -- while the engine's
    // vector points toward the light. Measured rather than assumed: passing
    // it through unchanged lit exactly the surfaces facing away from the sun
    // and left the ones facing it black, which is the one error a "the
    // picture changed" check cannot catch, because it changes just as much.
    for (std::size_t axis = 0; axis < 3; ++axis) {
        light.direction[axis] = -environment.sunDirection[axis];
        light.color[axis] = environment.sunColor[axis];
    }
    light.color[3] = 1.0f;
    light.direction[3] = 0.0f;
    // Zero radius and no falloff, matching what BuildLightRecord does for any
    // light that is not positional. Neither is consulted for a directional
    // light -- the attenuation is fixed at one because distance to a sun is
    // meaningless -- so this is the codebase's convention rather than a value
    // any input can distinguish. An earlier float maximum here read as though
    // the radius mattered and was reachable, and it is neither.
    light.radius = 0.0f;
    light.constantAttenuation = 1.0f;
    return EnvironmentSourceError::None;
}

EnvironmentSourceError SelectDirectionalLight(
    const shader::ReflectedShader& reflection,
    const std::string_view bufferName,
    std::span<const std::span<const std::byte>> candidates,
    lighting::EnvironmentRecordV1& environment,
    std::size_t& chosen) noexcept
{
    lighting::EnvironmentRecordV1 accepted{};
    std::size_t acceptedIndex = 0;
    auto found = false;
    // The failure of the last candidate, kept so a caller that gets nothing
    // learns why rather than only that. "No volumetric pass this frame" and
    // "the buffer was the wrong width" need different answers. Starting at
    // NoCandidate is also what answers an empty list: an explicit guard for
    // that case returned the same value by a second route and no input could
    // tell the two apart.
    auto lastError = EnvironmentSourceError::NoCandidate;

    for (std::size_t index = 0; index < candidates.size(); ++index) {
        lighting::EnvironmentRecordV1 candidate{};
        const auto result = ReadDirectionalLight(
            reflection, bufferName, candidates[index], candidate);
        if (result != EnvironmentSourceError::None) {
            lastError = result;
            continue;
        }
        if (!found) {
            accepted = candidate;
            acceptedIndex = index;
            found = true;
            continue;
        }
        // Two suns are not a sun. Compared on direction because that is what
        // the geometry is shaded by; a colour that drifted between two writes
        // of the same block is still one light.
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (std::abs(accepted.sunDirection[axis] -
                    candidate.sunDirection[axis]) > kDirectionTolerance) {
                return EnvironmentSourceError::Ambiguous;
            }
        }
    }

    if (!found) {
        return lastError;
    }
    // Written only now. A half-filled environment carrying one candidate's
    // sun and another's flag is worse than none at all.
    environment = accepted;
    chosen = acceptedIndex;
    return EnvironmentSourceError::None;
}

const char* ToString(const EnvironmentSourceError error) noexcept
{
    switch (error) {
    case EnvironmentSourceError::None: return "none";
    case EnvironmentSourceError::BufferNotFound: return "buffer-not-found";
    case EnvironmentSourceError::ContentsMismatched: return "contents-mismatched";
    case EnvironmentSourceError::FieldMissing: return "field-missing";
    case EnvironmentSourceError::FieldTruncated: return "field-truncated";
    case EnvironmentSourceError::DirectionNotUnit: return "direction-not-unit";
    case EnvironmentSourceError::DirectionDegenerate: return "direction-degenerate";
    case EnvironmentSourceError::ColourInvalid: return "colour-invalid";
    case EnvironmentSourceError::NoCandidate: return "no-candidate";
    case EnvironmentSourceError::Ambiguous: return "ambiguous";
    }
    return "unknown";
}

}
