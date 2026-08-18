#pragma once

#include "EngineLighting.h"
#include "ShaderReflection.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace vf::renderer::environment {

// Turns a constant buffer the engine wrote into the mirror's environment
// record, using the layout the engine's own shaders declare for it.
//
// This is the join between two measurements that are each useless alone. The
// reflection says a field called `g_vLightDir` sits at byte 608 of a 736-byte
// block, but not what is in it. A sampled buffer says what bytes 608 through
// 620 hold, but not what they mean. Reading a light needs both.
//
// Every refusal below exists because the alternative is a scene lit by
// numbers nobody checked. A wrong direction is not a visible error the way a
// wrong mesh is -- it looks exactly like a plausible sun in the wrong place,
// so the checks have to be arithmetic rather than visual.

enum class EnvironmentSourceError : std::uint8_t
{
    None,
    // The reflection carries no buffer under that name.
    BufferNotFound,
    // The contents are not the size the declaration says the buffer is, so
    // they are a different buffer and every offset would land in it.
    ContentsMismatched,
    // The declaration has no field under the required name.
    FieldMissing,
    // The field exists but is too narrow, misaligned, or runs off the end.
    FieldTruncated,
    // Three floats that are not a unit vector are not a direction.
    DirectionNotUnit,
    // Zero, or not finite. An unwritten buffer is all zeros, which is the most
    // likely wrong input and deserves its own diagnosis.
    DirectionDegenerate,
    // Negative or not finite. A light that removes energy is far more likely
    // to be the wrong four bytes than a real measurement.
    ColourInvalid,
    // Nothing was offered. A frame with no volumetric pass reaches this, and
    // it is a different answer from a frame whose buffer would not decode.
    NoCandidate,
    // More than one candidate decoded, and they disagree. Two blocks of the
    // same width that both hold a unit vector are two different things, and
    // nothing here can say which is the sun.
    Ambiguous,
};

// The names the volumetric lighting shaders use. Held here so a caller cannot
// invent its own spelling of a field the engine defined.
inline constexpr std::string_view kDirectionFieldName = "g_vLightDir";
inline constexpr std::string_view kColourFieldName = "g_vLightColor";

// How far from unit length a direction may measure and still be believed. The
// engine's own value measures 0.99995, so a gate any tighter would reject the
// real sun; any looser and (0.5, 0.5, 0.5) starts to qualify.
inline constexpr float kDirectionTolerance = 1.0e-3f;

// Writes the directional light into `environment`, leaving it untouched on
// any refusal. Sets EnvironmentPresent only on success.
[[nodiscard]] EnvironmentSourceError ReadDirectionalLight(
    const shader::ReflectedShader& reflection,
    std::string_view bufferName,
    std::span<const std::byte> contents,
    lighting::EnvironmentRecordV1& environment) noexcept;

// Builds the sun as a light the shading will actually evaluate.
//
// The environment record has sunDirection and sunColor fields, but nothing
// reads them: ShadeSurfaceGpu shades from the ambient term plus the light
// list, and a sun delivered only as environment metadata contributes exactly
// nothing. On screen that is indistinguishable from no sun at all, which is
// why it has to be a light record and not just a field.
//
// `direction` is passed through as the engine wrote it. LightRecordV1 stores
// the direction light travels -- EvaluateDirectGpu negates it to face the
// surface -- and every sample measured has a consistently negative Z in this
// Z-up world, so the engine's vector already points downward from the sky.
// Which body in the environment record a light is built from.
//
// The record carries both, and for a long time only one of them was ever
// turned into a light. Naming them apart is what stops a second body from
// being captured, validated, blended, uploaded and then quietly evaluated by
// nothing -- which is exactly what happened to the moon.
enum class CelestialBody : std::uint8_t
{
    Sun,
    Moon,
};

// Builds a directional light from one of the environment's celestial bodies.
[[nodiscard]] EnvironmentSourceError MakeCelestialLight(
    const lighting::EnvironmentRecordV1& environment,
    CelestialBody body,
    std::uint64_t lightId,
    lighting::LightRecordV1& light) noexcept;

[[nodiscard]] EnvironmentSourceError MakeDirectionalLight(

    const lighting::EnvironmentRecordV1& environment,
    std::uint64_t lightId,
    lighting::LightRecordV1& light) noexcept;

// Chooses the light among several buffers that share the declared width.
//
// A sample is matched to a declaration by width, so more than one buffer can
// present itself for one layout -- which is how the wrong 752-byte block was
// read for as long as it was. The checks in ReadDirectionalLight are the
// discriminator: a block that is not the light almost never holds a unit
// vector at the declared offset.
//
// Candidates that decode and agree are one sun seen more than once, which is
// expected and accepted. Candidates that decode and disagree are refused:
// picking either would be a coin toss reported as a measurement. `chosen` is
// the index that supplied the answer, and `environment` is left untouched
// unless the result is None.
[[nodiscard]] EnvironmentSourceError SelectDirectionalLight(
    const shader::ReflectedShader& reflection,
    std::string_view bufferName,
    std::span<const std::span<const std::byte>> candidates,
    lighting::EnvironmentRecordV1& environment,
    std::size_t& chosen) noexcept;

[[nodiscard]] const char* ToString(EnvironmentSourceError error) noexcept;

}
