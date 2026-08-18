#include "renderer_core/EngineEnvironmentSource.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace vf::renderer;
using Catch::Matchers::WithinAbs;

namespace {

// The buffer the volumetric lighting shaders declare, at the offsets they
// declare it at. Measured live in Sanctuary rather than invented: the two
// scalars either side of the light are here because a layout that puts the
// direction in the right place by accident would put them in the wrong one.
constexpr std::uint32_t kLightDirOffset = 608;
constexpr std::uint32_t kGodrayBiasOffset = 620;
constexpr std::uint32_t kLightPosOffset = 624;
constexpr std::uint32_t kLightColorOffset = 640;
constexpr std::uint32_t kTargetRaySizeOffset = 652;
constexpr std::uint32_t kVolumeBytes = 736;

[[nodiscard]] shader::ReflectedShader VolumeReflection()
{
    shader::ReflectedShader reflection{};
    shader::ReflectedBuffer buffer{};
    buffer.name = "cbVolume";
    buffer.size = kVolumeBytes;
    buffer.variables.push_back({"g_vLightDir", kLightDirOffset, 12});
    buffer.variables.push_back({"g_fGodrayBias", kGodrayBiasOffset, 4});
    buffer.variables.push_back({"g_vLightPos", kLightPosOffset, 12});
    buffer.variables.push_back({"g_vLightColor", kLightColorOffset, 12});
    buffer.variables.push_back({"g_fTargetRaySize", kTargetRaySizeOffset, 4});
    reflection.buffers.push_back(std::move(buffer));
    return reflection;
}

void WriteFloats(
    std::vector<std::byte>& bytes,
    const std::uint32_t offset,
    const std::initializer_list<float> values)
{
    std::uint32_t cursor = offset;
    for (const float value : values) {
        std::memcpy(bytes.data() + cursor, &value, sizeof(value));
        cursor += static_cast<std::uint32_t>(sizeof(value));
    }
}

// The values the engine actually wrote, captured live.
[[nodiscard]] std::vector<std::byte> VolumeContents()
{
    std::vector<std::byte> bytes(kVolumeBytes, std::byte{0});
    WriteFloats(bytes, kLightDirOffset, {-0.3505f, 0.6911f, -0.6320f});
    WriteFloats(bytes, kGodrayBiasOffset, {0.0015f});
    WriteFloats(bytes, kLightPosOffset, {0.0f, 0.0f, 0.0f});
    WriteFloats(bytes, kLightColorOffset, {0.3490f, 0.3804f, 0.4196f});
    WriteFloats(bytes, kTargetRaySizeOffset, {8.0f});
    return bytes;
}

}

TEST_CASE("the engine's directional light is read by name, not by offset")
{
    SECTION("the sun captured live is recovered from its own declaration")
    {
        lighting::EnvironmentRecordV1 environment{};
        REQUIRE(environment::ReadDirectionalLight(
                    VolumeReflection(), "cbVolume", VolumeContents(), environment)
                == environment::EnvironmentSourceError::None);

        CHECK_THAT(environment.sunDirection[0], WithinAbs(-0.3505, 1.0e-4));
        CHECK_THAT(environment.sunDirection[1], WithinAbs(0.6911, 1.0e-4));
        CHECK_THAT(environment.sunDirection[2], WithinAbs(-0.6320, 1.0e-4));
        CHECK_THAT(environment.sunColor[0], WithinAbs(0.3490, 1.0e-4));
        CHECK_THAT(environment.sunColor[1], WithinAbs(0.3804, 1.0e-4));
        CHECK_THAT(environment.sunColor[2], WithinAbs(0.4196, 1.0e-4));

        // The frame declares it has lighting. Without this the mirror treats a
        // zeroed environment as "ambient zero" and blacks out every surface
        // instead of leaving the albedo alone.
        CHECK((environment.flags & lighting::EnvironmentPresent) != 0);
    }

    SECTION("the field order in the declaration does not matter")
    {
        // Reflection lists variables in whatever order the compiler emitted.
        // Reading by position rather than by name would work on the fixture
        // above and silently swap direction for colour here.
        auto reflection = VolumeReflection();
        auto& variables = reflection.buffers[0].variables;
        std::swap(variables[0], variables[3]);

        lighting::EnvironmentRecordV1 environment{};
        REQUIRE(environment::ReadDirectionalLight(
                    reflection, "cbVolume", VolumeContents(), environment)
                == environment::EnvironmentSourceError::None);
        CHECK_THAT(environment.sunDirection[1], WithinAbs(0.6911, 1.0e-4));
        CHECK_THAT(environment.sunColor[2], WithinAbs(0.4196, 1.0e-4));
    }

    SECTION("the intensity carries the colour's magnitude, not a guess")
    {
        lighting::EnvironmentRecordV1 environment{};
        REQUIRE(environment::ReadDirectionalLight(
                    VolumeReflection(), "cbVolume", VolumeContents(), environment)
                == environment::EnvironmentSourceError::None);
        // The engine publishes a colour and no separate intensity, so the
        // record's intensity is one: scaling the colour twice would light the
        // scene by a number nobody measured.
        CHECK_THAT(environment.sunIntensity, WithinAbs(1.0, 1.0e-6));
    }
}

TEST_CASE("a constant buffer that is not the light is refused")
{
    SECTION("a buffer the reflection does not name is refused")
    {
        lighting::EnvironmentRecordV1 environment{};
        CHECK(environment::ReadDirectionalLight(
                  VolumeReflection(), "cbPass", VolumeContents(), environment)
              == environment::EnvironmentSourceError::BufferNotFound);
    }

    SECTION("contents of a different size than the buffer declares are refused")
    {
        // The engine binds many buffers and the sample is matched to a layout
        // by width. Contents that are the wrong size are a different buffer,
        // and every offset below would then be read out of somebody else's
        // block -- which is exactly the mistake this whole path exists to
        // stop making.
        auto contents = VolumeContents();
        contents.resize(kVolumeBytes + 16);
        lighting::EnvironmentRecordV1 environment{};
        CHECK(environment::ReadDirectionalLight(
                  VolumeReflection(), "cbVolume", contents, environment)
              == environment::EnvironmentSourceError::ContentsMismatched);
    }

    SECTION("a declaration without the direction is refused")
    {
        auto reflection = VolumeReflection();
        auto& variables = reflection.buffers[0].variables;
        variables.erase(variables.begin());
        lighting::EnvironmentRecordV1 environment{};
        CHECK(environment::ReadDirectionalLight(
                  reflection, "cbVolume", VolumeContents(), environment)
              == environment::EnvironmentSourceError::FieldMissing);
    }

    SECTION("a declaration without the colour is refused")
    {
        auto reflection = VolumeReflection();
        auto& variables = reflection.buffers[0].variables;
        variables.erase(variables.begin() + 3);
        lighting::EnvironmentRecordV1 environment{};
        CHECK(environment::ReadDirectionalLight(
                  reflection, "cbVolume", VolumeContents(), environment)
              == environment::EnvironmentSourceError::FieldMissing);
    }

    SECTION("a field narrower than three floats is refused")
    {
        // A name alone is not enough. A direction stored as two floats is not
        // a direction, and reading three from it would take the first word of
        // whatever follows.
        auto reflection = VolumeReflection();
        reflection.buffers[0].variables[0].size = 8;
        lighting::EnvironmentRecordV1 environment{};
        CHECK(environment::ReadDirectionalLight(
                  reflection, "cbVolume", VolumeContents(), environment)
              == environment::EnvironmentSourceError::FieldTruncated);
    }

    SECTION("a field that runs past the end of the buffer is refused")
    {
        auto reflection = VolumeReflection();
        reflection.buffers[0].variables[0].offset = kVolumeBytes - 8;
        lighting::EnvironmentRecordV1 environment{};
        CHECK(environment::ReadDirectionalLight(
                  reflection, "cbVolume", VolumeContents(), environment)
              == environment::EnvironmentSourceError::FieldTruncated);
    }

    SECTION("a field at an offset that is not float-aligned is refused")
    {
        auto reflection = VolumeReflection();
        reflection.buffers[0].variables[0].offset = kLightDirOffset + 1;
        lighting::EnvironmentRecordV1 environment{};
        CHECK(environment::ReadDirectionalLight(
                  reflection, "cbVolume", VolumeContents(), environment)
              == environment::EnvironmentSourceError::FieldTruncated);
    }
}

TEST_CASE("a direction that is not a direction is refused")
{
    SECTION("a non-unit vector is refused")
    {
        // The load-bearing check. Three floats at the right offset are only a
        // direction if they have unit length; without this, any buffer that
        // happens to be the right width would yield a confident sun pointing
        // wherever its bytes happened to point.
        auto contents = VolumeContents();
        WriteFloats(contents, kLightDirOffset, {0.5f, 0.5f, 0.5f});
        lighting::EnvironmentRecordV1 environment{};
        CHECK(environment::ReadDirectionalLight(
                  VolumeReflection(), "cbVolume", contents, environment)
              == environment::EnvironmentSourceError::DirectionNotUnit);
    }

    SECTION("a vector just outside the tolerance is refused")
    {
        auto contents = VolumeContents();
        WriteFloats(contents, kLightDirOffset, {1.01f, 0.0f, 0.0f});
        lighting::EnvironmentRecordV1 environment{};
        CHECK(environment::ReadDirectionalLight(
                  VolumeReflection(), "cbVolume", contents, environment)
              == environment::EnvironmentSourceError::DirectionNotUnit);
    }

    SECTION("a vector just inside the tolerance is accepted")
    {
        // The engine's own value measures 0.99995, so the gate has to admit a
        // float's worth of drift or it would reject the real sun.
        auto contents = VolumeContents();
        WriteFloats(contents, kLightDirOffset, {0.9999f, 0.0f, 0.0f});
        lighting::EnvironmentRecordV1 environment{};
        CHECK(environment::ReadDirectionalLight(
                  VolumeReflection(), "cbVolume", contents, environment)
              == environment::EnvironmentSourceError::None);
    }

    SECTION("a zero vector is refused as degenerate, not as non-unit")
    {
        // An unwritten buffer is all zeros, and that is the single most likely
        // wrong input. It deserves its own answer: "nothing was written here"
        // is a different diagnosis from "this is not a direction".
        auto contents = VolumeContents();
        WriteFloats(contents, kLightDirOffset, {0.0f, 0.0f, 0.0f});
        lighting::EnvironmentRecordV1 environment{};
        CHECK(environment::ReadDirectionalLight(
                  VolumeReflection(), "cbVolume", contents, environment)
              == environment::EnvironmentSourceError::DirectionDegenerate);
    }

    SECTION("a direction with a non-finite component is refused")
    {
        auto contents = VolumeContents();
        WriteFloats(contents, kLightDirOffset,
            {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f});
        lighting::EnvironmentRecordV1 environment{};
        CHECK(environment::ReadDirectionalLight(
                  VolumeReflection(), "cbVolume", contents, environment)
              == environment::EnvironmentSourceError::DirectionDegenerate);
    }
}

TEST_CASE("a colour that cannot be a colour is refused")
{
    SECTION("a negative component is refused")
    {
        // A light that removes energy is not a measurement of anything. It is
        // far more likely to be the wrong four bytes.
        auto contents = VolumeContents();
        WriteFloats(contents, kLightColorOffset, {0.5f, -0.1f, 0.5f});
        lighting::EnvironmentRecordV1 environment{};
        CHECK(environment::ReadDirectionalLight(
                  VolumeReflection(), "cbVolume", contents, environment)
              == environment::EnvironmentSourceError::ColourInvalid);
    }

    SECTION("a non-finite component is refused")
    {
        auto contents = VolumeContents();
        WriteFloats(contents, kLightColorOffset,
            {0.5f, std::numeric_limits<float>::infinity(), 0.5f});
        lighting::EnvironmentRecordV1 environment{};
        CHECK(environment::ReadDirectionalLight(
                  VolumeReflection(), "cbVolume", contents, environment)
              == environment::EnvironmentSourceError::ColourInvalid);
    }

    SECTION("a black sun is accepted, because midnight is a measurement")
    {
        auto contents = VolumeContents();
        WriteFloats(contents, kLightColorOffset, {0.0f, 0.0f, 0.0f});
        lighting::EnvironmentRecordV1 environment{};
        CHECK(environment::ReadDirectionalLight(
                  VolumeReflection(), "cbVolume", contents, environment)
              == environment::EnvironmentSourceError::None);
    }
}

TEST_CASE("the light is selected from among buffers that share a width")
{
    // The engine binds thousands of constant buffers and a sample is matched
    // to a declaration by width, so several candidates can present themselves
    // for one layout. Picking the first would be picking whichever bound
    // earliest, which is how the wrong 752-byte block was read for weeks.

    SECTION("the candidate whose bytes are a direction is chosen")
    {
        auto decoy = VolumeContents();
        WriteFloats(decoy, kLightDirOffset, {12.5f, -3.25f, 900.0f});
        // Named, not temporaries: a span into a vector that dies at the end of
        // the expression that made it points at freed memory.
        const auto real = VolumeContents();
        const std::vector<std::span<const std::byte>> candidates{decoy, real};

        lighting::EnvironmentRecordV1 environment{};
        std::size_t chosen = 0;
        REQUIRE(environment::SelectDirectionalLight(
                    VolumeReflection(), "cbVolume", candidates, environment, chosen)
                == environment::EnvironmentSourceError::None);
        CHECK(chosen == 1);
        CHECK_THAT(environment.sunDirection[1], WithinAbs(0.6911, 1.0e-4));
    }

    SECTION("candidates that agree are accepted, because that is one sun")
    {
        const auto same = VolumeContents();
        const std::vector<std::span<const std::byte>> candidates{same, same};
        lighting::EnvironmentRecordV1 environment{};
        std::size_t chosen = 0;
        CHECK(environment::SelectDirectionalLight(
                  VolumeReflection(), "cbVolume", candidates, environment, chosen)
              == environment::EnvironmentSourceError::None);
    }

    SECTION("candidates that disagree are refused rather than picked between")
    {
        // Two blocks of the same width that both hold a unit vector are two
        // different things, and no rule here can say which is the sun. Taking
        // either would be a coin toss reported as a measurement.
        auto other = VolumeContents();
        WriteFloats(other, kLightDirOffset, {0.0f, 0.0f, 1.0f});
        const auto real = VolumeContents();
        const std::vector<std::span<const std::byte>> candidates{real, other};

        lighting::EnvironmentRecordV1 environment{};
        std::size_t chosen = 0;
        CHECK(environment::SelectDirectionalLight(
                  VolumeReflection(), "cbVolume", candidates, environment, chosen)
              == environment::EnvironmentSourceError::Ambiguous);
    }

    SECTION("no candidate at all is refused, not defaulted")
    {
        const std::vector<std::span<const std::byte>> candidates{};
        lighting::EnvironmentRecordV1 environment{};
        std::size_t chosen = 0;
        CHECK(environment::SelectDirectionalLight(
                  VolumeReflection(), "cbVolume", candidates, environment, chosen)
              == environment::EnvironmentSourceError::NoCandidate);
    }

    SECTION("candidates that all fail report why the last one failed")
    {
        // A caller that only learns "nothing worked" cannot tell a frame with
        // no volumetric pass from a frame whose buffer was the wrong width.
        auto zeroed = VolumeContents();
        WriteFloats(zeroed, kLightDirOffset, {0.0f, 0.0f, 0.0f});
        const std::vector<std::span<const std::byte>> candidates{zeroed};
        lighting::EnvironmentRecordV1 environment{};
        std::size_t chosen = 0;
        CHECK(environment::SelectDirectionalLight(
                  VolumeReflection(), "cbVolume", candidates, environment, chosen)
              == environment::EnvironmentSourceError::DirectionDegenerate);
    }

    SECTION("a selection with nothing acceptable leaves the environment untouched")
    {
        // Distinct from the ambiguous case below, which refuses from inside
        // the loop before anything is written. This path runs to the end with
        // nothing accepted, and the record it would otherwise copy out is a
        // zeroed one -- which is not "no lighting", it is a black sun that
        // would shade every surface to nothing.
        auto zeroed = VolumeContents();
        WriteFloats(zeroed, kLightDirOffset, {0.0f, 0.0f, 0.0f});
        const std::vector<std::span<const std::byte>> candidates{zeroed};

        lighting::EnvironmentRecordV1 environment{};
        environment.sunDirection[0] = 7.0f;
        environment.sunIntensity = 3.0f;
        std::size_t chosen = 0;
        REQUIRE(environment::SelectDirectionalLight(
                    VolumeReflection(), "cbVolume", candidates, environment, chosen)
                != environment::EnvironmentSourceError::None);
        CHECK_THAT(environment.sunDirection[0], WithinAbs(7.0, 1.0e-6));
        CHECK_THAT(environment.sunIntensity, WithinAbs(3.0, 1.0e-6));
        CHECK((environment.flags & lighting::EnvironmentPresent) == 0);
    }

    SECTION("a refused selection leaves the environment untouched")
    {
        auto other = VolumeContents();
        WriteFloats(other, kLightDirOffset, {0.0f, 0.0f, 1.0f});
        const auto real = VolumeContents();
        const std::vector<std::span<const std::byte>> candidates{real, other};

        lighting::EnvironmentRecordV1 environment{};
        environment.sunDirection[0] = 7.0f;
        std::size_t chosen = 0;
        REQUIRE(environment::SelectDirectionalLight(
                    VolumeReflection(), "cbVolume", candidates, environment, chosen)
                != environment::EnvironmentSourceError::None);
        // Untouched means untouched: a half-written environment carrying one
        // candidate's sun and another's flag is worse than none.
        CHECK_THAT(environment.sunDirection[0], WithinAbs(7.0, 1.0e-6));
        CHECK((environment.flags & lighting::EnvironmentPresent) == 0);
    }
}

TEST_CASE("the environment read from the engine is a valid light packet")
{
    // The mirror's whole reason for reading this is to send it to the backend,
    // and it travels as a light packet. A record that cannot be encoded is a
    // measurement that never arrives.

    SECTION("it encodes without being adjusted by the caller")
    {
        lighting::EnvironmentRecordV1 environment{};
        REQUIRE(environment::ReadDirectionalLight(
                    VolumeReflection(), "cbVolume", VolumeContents(), environment)
                == environment::EnvironmentSourceError::None);

        lighting::LightPacket packet{};
        packet.environment = environment;
        std::vector<std::byte> bytes;
        CHECK(lighting::EncodeLightPacket(packet, bytes)
              == lighting::LightPacketError::None);
        CHECK(!bytes.empty());
    }

    SECTION("fog is switched off rather than invented")
    {
        // The engine publishes no fog distances in this buffer. The packet
        // requires an ordered range, so one is supplied -- but with a maximum
        // of zero, which is what makes it "no fog" instead of "fog over a
        // range nobody measured". EvaluateFog scales by the maximum, so this
        // contributes exactly nothing.
        lighting::EnvironmentRecordV1 environment{};
        REQUIRE(environment::ReadDirectionalLight(
                    VolumeReflection(), "cbVolume", VolumeContents(), environment)
                == environment::EnvironmentSourceError::None);

        CHECK(environment.fogFar > environment.fogNear);
        CHECK_THAT(environment.fogMaximum, WithinAbs(0.0, 1.0e-6));
        CHECK_THAT(lighting::EvaluateFog(environment, 0.0f), WithinAbs(0.0, 1.0e-6));
        CHECK_THAT(lighting::EvaluateFog(environment, 1.0e6f), WithinAbs(0.0, 1.0e-6));
    }

    SECTION("the ambient term is zero, because none was measured")
    {
        // Sun only. An ambient invented to stop shadowed faces going black
        // would be a number nobody measured lighting the whole scene.
        lighting::EnvironmentRecordV1 environment{};
        REQUIRE(environment::ReadDirectionalLight(
                    VolumeReflection(), "cbVolume", VolumeContents(), environment)
                == environment::EnvironmentSourceError::None);
        CHECK_THAT(environment.ambient[0], WithinAbs(0.0, 1.0e-6));
        CHECK_THAT(environment.ambient[1], WithinAbs(0.0, 1.0e-6));
        CHECK_THAT(environment.ambient[2], WithinAbs(0.0, 1.0e-6));
    }
}

TEST_CASE("the measured sun shades differently along different normals")
{
    // The end of the chain, checked deterministically because the live frame
    // dump races the harness's conversion and cannot be relied on to hold a
    // frame with geometry in it.
    //
    // This is the property the whole exercise is for. A sun that reaches the
    // backend but shades every normal identically is indistinguishable, on
    // screen, from no sun at all -- which is exactly what the mirror looked
    // like while every vertex carried the same default normal.
    lighting::EnvironmentRecordV1 record{};
    REQUIRE(environment::ReadDirectionalLight(
                VolumeReflection(), "cbVolume", VolumeContents(), record)
            == environment::EnvironmentSourceError::None);

    lighting::LightRecordV1 sun{};
    REQUIRE(environment::MakeDirectionalLight(record, 0x5000'0000'0000'0001ull, sun)
            == environment::EnvironmentSourceError::None);

    const auto gpu = lighting::BuildGpuEnvironment(record, 1);
    lighting::GpuLightRecordV1 gpuSun{};
    REQUIRE(lighting::BuildGpuLight(sun, {0.0, 0.0, 0.0}, gpuSun)
            == lighting::LightError::None);
    const std::array<lighting::GpuLightRecordV1, 1> gpuLights{gpuSun};
    const std::array<float, 3> albedo{1.0f, 1.0f, 1.0f};
    const std::array<float, 3> position{0.0f, 0.0f, 0.0f};

    // The sun is (-0.3505, 0.6911, -0.6320). A surface facing into it and one
    // facing away from it cannot shade the same.
    const std::array<float, 3> facing{-0.3505f, 0.6911f, -0.6320f};
    const std::array<float, 3> away{0.3505f, -0.6911f, 0.6320f};
    const auto lit = lighting::ShadeSurfaceGpu(gpu, gpuLights, albedo, position, facing);
    const auto unlit = lighting::ShadeSurfaceGpu(gpu, gpuLights, albedo, position, away);

    auto difference = 0.0f;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        difference = std::max(difference, std::abs(lit[channel] - unlit[channel]));
    }
    CHECK(difference > 0.1f);

    // And the one facing the sun is the brighter of the two, which is what
    // says the direction was not read with its sign flipped -- a mistake no
    // "the picture changed" check would catch.
    CHECK(lit[0] + lit[1] + lit[2] > unlit[0] + unlit[1] + unlit[2]);
}

TEST_CASE("P17_the_moon_becomes_a_light_like_the_sun_does",
    "[phase17][environment]")
{
    // The moon's direction, colour and intensity are captured, validated,
    // blended and uploaded into the environment record -- and nothing ever
    // read them. Surface shading evaluates the ambient term and the light
    // list, and only the sun was ever turned into a list entry, so every
    // night scene was lit by ambient alone. The environment's own moon
    // fields are provenance; a body delivered only there shades nothing and
    // looks exactly like no moon, which is the same trap the sun fell into.
    lighting::EnvironmentRecordV1 record{};
    record.flags = lighting::EnvironmentPresent;
    record.ambient[2] = 0.08f;
    record.sunDirection[2] = -1.0f;
    record.sunColor[0] = 1.0f;
    record.sunColor[1] = 1.0f;
    record.sunColor[2] = 1.0f;
    record.sunIntensity = 0.0f;
    record.moonDirection[2] = -1.0f;
    record.moonColor[0] = 0.4f;
    record.moonColor[1] = 0.45f;
    record.moonColor[2] = 0.9f;
    record.moonIntensity = 0.25f;
    record.fogNear = 1.0f;
    record.fogFar = 1000.0f;
    record.fogMaximum = 1.0f;

    lighting::LightRecordV1 moon{};
    REQUIRE(environment::MakeCelestialLight(record,
                environment::CelestialBody::Moon,
                0x5000'0000'0000'0002ull, moon) ==
        environment::EnvironmentSourceError::None);
    CHECK(moon.type ==
        static_cast<std::uint8_t>(lighting::LightType::Directional));
    CHECK(moon.intensity == 0.25f);
    CHECK(moon.color[0] == 0.4f);
    CHECK(moon.color[2] == 0.9f);
    // Negated for the same reason the sun is: the record stores the direction
    // light travels, and the engine's vector points toward the body.
    CHECK(moon.direction[2] == 1.0f);

    // And the sun still resolves through the same function, so the two cannot
    // drift apart.
    lighting::LightRecordV1 sun{};
    REQUIRE(environment::MakeCelestialLight(record,
                environment::CelestialBody::Sun,
                0x5000'0000'0000'0001ull, sun) ==
        environment::EnvironmentSourceError::None);
    CHECK(sun.color[0] == 1.0f);
    CHECK(sun.intensity == 0.0f);
}
