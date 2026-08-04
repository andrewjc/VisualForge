#include "renderer_core/EngineLighting.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace vf::renderer;

namespace {

lighting::LightCapture BaseLight(
    const lighting::LightType type,
    const std::uint64_t id = 0x1700'0000'0000'0001ull)
{
    lighting::LightCapture capture{};
    capture.lightId = id;
    capture.type = type;
    capture.diffuse = {1.0f, 0.95f, 0.90f};
    capture.dimmer = 1.0f;
    capture.radius = 1024.0f;
    capture.constantAttenuation = 1.0f;
    capture.linearAttenuation = 0.0f;
    capture.quadraticAttenuation = 0.0f;
    capture.direction = {0.0f, 1.0f, 0.0f};
    capture.position = {100.0, 200.0, 300.0};
    capture.innerConeRadians = 0.3f;
    capture.outerConeRadians = 0.6f;
    capture.spotExponent = 1.0f;
    return capture;
}

}

TEST_CASE("P17_light_types_mirror_the_engine_class_taxonomy",
    "[phase17][lighting]")
{
    // The engine has exactly four concrete NiLight subclasses; shadow
    // casting is a property of BSShadowLight, which wraps a light rather
    // than being a fifth type. Modelling it as a type would make "a point
    // light that casts shadows" inexpressible.
    CHECK(static_cast<std::uint8_t>(lighting::LightType::Ambient) == 0);
    CHECK(static_cast<std::uint8_t>(lighting::LightType::Directional) == 1);
    CHECK(static_cast<std::uint8_t>(lighting::LightType::Point) == 2);
    CHECK(static_cast<std::uint8_t>(lighting::LightType::Spot) == 3);

    auto shadowed = BaseLight(lighting::LightType::Point);
    shadowed.castsShadows = true;
    lighting::LightRecordV1 record{};
    REQUIRE(lighting::TranslateLight(shadowed, record) ==
        lighting::LightError::None);
    CHECK(record.type == static_cast<std::uint8_t>(lighting::LightType::Point));
    CHECK((record.flags & lighting::LightCastsShadows) != 0);

    // The shadow term does not exist in this phase. It must be declared
    // unavailable so a parity metric can mask it, rather than silently
    // comparing a lit mirror against a shadowed vanilla frame and reporting
    // the difference as error.
    CHECK(lighting::ShadowTermAvailable() == false);
}

TEST_CASE("P17_attenuation_falls_off_and_never_amplifies",
    "[phase17][lighting]")
{
    auto capture = BaseLight(lighting::LightType::Point);
    capture.constantAttenuation = 1.0f;
    capture.linearAttenuation = 0.002f;
    capture.quadraticAttenuation = 0.000004f;
    lighting::LightRecordV1 record{};
    REQUIRE(lighting::TranslateLight(capture, record) ==
        lighting::LightError::None);

    const auto near = lighting::EvaluateAttenuation(record, 0.0f);
    const auto mid = lighting::EvaluateAttenuation(record, 400.0f);
    const auto far = lighting::EvaluateAttenuation(record, 900.0f);
    CHECK(near == Catch::Approx(1.0f));
    CHECK(mid < near);
    CHECK(far < mid);
    CHECK(far > 0.0f);
    // Beyond the captured radius the light contributes nothing at all, so a
    // light list can be culled by radius without changing the image.
    CHECK(lighting::EvaluateAttenuation(record, 1024.0f) ==
        Catch::Approx(0.0f));
    CHECK(lighting::EvaluateAttenuation(record, 5000.0f) ==
        Catch::Approx(0.0f));

    // A directional light has no falloff; distance is meaningless for it.
    lighting::LightRecordV1 sun{};
    REQUIRE(lighting::TranslateLight(
        BaseLight(lighting::LightType::Directional), sun) ==
        lighting::LightError::None);
    CHECK(lighting::EvaluateAttenuation(sun, 0.0f) == Catch::Approx(1.0f));
    CHECK(lighting::EvaluateAttenuation(sun, 100000.0f) ==
        Catch::Approx(1.0f));

    // All-zero attenuation coefficients would divide by zero.
    auto degenerate = capture;
    degenerate.constantAttenuation = 0.0f;
    degenerate.linearAttenuation = 0.0f;
    degenerate.quadraticAttenuation = 0.0f;
    lighting::LightRecordV1 rejected{};
    CHECK(lighting::TranslateLight(degenerate, rejected) ==
        lighting::LightError::InvalidAttenuation);

    auto negative = capture;
    negative.linearAttenuation = -1.0f;
    CHECK(lighting::TranslateLight(negative, rejected) ==
        lighting::LightError::InvalidAttenuation);

    auto badRadius = capture;
    badRadius.radius = 0.0f;
    CHECK(lighting::TranslateLight(badRadius, rejected) ==
        lighting::LightError::InvalidRange);
}

TEST_CASE("P17_spot_cone_is_monotonic_between_its_declared_angles",
    "[phase17][lighting]")
{
    auto capture = BaseLight(lighting::LightType::Spot);
    capture.innerConeRadians = 0.25f;
    capture.outerConeRadians = 0.55f;
    lighting::LightRecordV1 record{};
    REQUIRE(lighting::TranslateLight(capture, record) ==
        lighting::LightError::None);

    CHECK(lighting::EvaluateCone(record, 0.0f) == Catch::Approx(1.0f));
    CHECK(lighting::EvaluateCone(record, 0.20f) == Catch::Approx(1.0f));
    const auto middle = lighting::EvaluateCone(record, 0.40f);
    CHECK(middle > 0.0f);
    CHECK(middle < 1.0f);
    CHECK(lighting::EvaluateCone(record, 0.55f) == Catch::Approx(0.0f));
    CHECK(lighting::EvaluateCone(record, 1.20f) == Catch::Approx(0.0f));
    // Monotonic across the falloff, or a spot edge would band.
    auto previous = 1.0f;
    for (int step = 0; step <= 20; ++step) {
        const auto angle = 0.25f + 0.30f * static_cast<float>(step) / 20.0f;
        const auto value = lighting::EvaluateCone(record, angle);
        CHECK(value <= previous + 1.0e-6f);
        previous = value;
    }

    // A cone whose inner angle exceeds its outer is a captured
    // contradiction, not something to reorder silently.
    auto inverted = capture;
    inverted.innerConeRadians = 0.8f;
    lighting::LightRecordV1 rejected{};
    CHECK(lighting::TranslateLight(inverted, rejected) ==
        lighting::LightError::InvalidCone);

    // Only a spot has a cone; anything else ignores the angle entirely.
    lighting::LightRecordV1 point{};
    REQUIRE(lighting::TranslateLight(
        BaseLight(lighting::LightType::Point), point) ==
        lighting::LightError::None);
    CHECK(lighting::EvaluateCone(point, 3.0f) == Catch::Approx(1.0f));
}

TEST_CASE("P17_colour_and_intensity_convert_without_inventing_energy",
    "[phase17][lighting]")
{
    auto capture = BaseLight(lighting::LightType::Point);
    capture.diffuse = {0.5f, 0.25f, 0.125f};
    capture.dimmer = 2.0f;
    lighting::LightRecordV1 record{};
    REQUIRE(lighting::TranslateLight(capture, record) ==
        lighting::LightError::None);
    // The dimmer scales radiance; it is not folded into the colour, because
    // a consumer that wants the authored colour must still be able to read
    // it back.
    CHECK(record.color[0] == Catch::Approx(0.5f));
    CHECK(record.color[1] == Catch::Approx(0.25f));
    CHECK(record.color[2] == Catch::Approx(0.125f));
    CHECK(record.intensity == Catch::Approx(2.0f));

    const auto radiance = lighting::EvaluateRadiance(record);
    CHECK(radiance[0] == Catch::Approx(1.0f));
    CHECK(radiance[1] == Catch::Approx(0.5f));
    CHECK(radiance[2] == Catch::Approx(0.25f));

    // A negative dimmer would subtract light from the scene.
    auto negative = capture;
    negative.dimmer = -0.5f;
    lighting::LightRecordV1 rejected{};
    CHECK(lighting::TranslateLight(negative, rejected) ==
        lighting::LightError::InvalidIntensity);

    auto nonFinite = capture;
    nonFinite.diffuse[1] = std::numeric_limits<float>::quiet_NaN();
    CHECK(lighting::TranslateLight(nonFinite, rejected) ==
        lighting::LightError::NonFiniteSource);

    // A zero-radiance light is legal but must be reported as contributing
    // nothing, so it can be culled deterministically.
    auto dark = capture;
    dark.dimmer = 0.0f;
    lighting::LightRecordV1 unlit{};
    REQUIRE(lighting::TranslateLight(dark, unlit) ==
        lighting::LightError::None);
    CHECK_FALSE(lighting::Contributes(unlit));
    CHECK(lighting::Contributes(record));
}

TEST_CASE("P17_light_positions_are_camera_relative_in_double_then_narrowed",
    "[phase17][lighting]")
{
    // Same rule the terrain boundary uses: world coordinates reach the
    // hundreds of thousands, so the subtraction happens in double and only
    // the small residual becomes float. Narrowing first loses metres.
    auto capture = BaseLight(lighting::LightType::Point);
    capture.position = {2'000'123.5, -1'000'456.25, 512.75};
    const std::array<double, 3> cameraOrigin{
        2'000'000.0, -1'000'000.0, 500.0};
    lighting::LightRecordV1 record{};
    REQUIRE(lighting::TranslateLight(capture, record) ==
        lighting::LightError::None);
    lighting::GpuLightRecordV1 gpu{};
    REQUIRE(lighting::BuildGpuLight(record, cameraOrigin, gpu) ==
        lighting::LightError::None);
    CHECK(gpu.position[0] == Catch::Approx(123.5f));
    CHECK(gpu.position[1] == Catch::Approx(-456.25f));
    CHECK(gpu.position[2] == Catch::Approx(12.75f));

    // A light too far from the camera to narrow without losing precision is
    // refused rather than silently swimming.
    auto distant = record;
    distant.position[0] = 1.0e12;
    lighting::GpuLightRecordV1 rejected{};
    CHECK(lighting::BuildGpuLight(distant, cameraOrigin, rejected) ==
        lighting::LightError::PositionOutOfRange);
}

TEST_CASE("P17_light_list_overflow_is_deterministic_and_reported",
    "[phase17][lighting]")
{
    // A frame with more lights than the list can hold must drop the same
    // ones every time, or the image flickers between frames that captured
    // the same scene.
    lighting::LightSet set{};
    for (std::uint32_t index = 0; index < lighting::kMaximumActiveLights + 8;
         ++index) {
        auto capture = BaseLight(lighting::LightType::Point,
            0x1700'0000'0000'0001ull + index);
        // Brighter lights are more important; the order they arrive in is
        // deliberately the reverse of their importance.
        capture.dimmer = static_cast<float>(index + 1);
        capture.position = {static_cast<double>(index) * 10.0, 0.0, 0.0};
        lighting::LightRecordV1 record{};
        REQUIRE(lighting::TranslateLight(capture, record) ==
            lighting::LightError::None);
        set.lights.push_back(record);
    }

    lighting::LightSelection first{};
    lighting::LightSelection second{};
    const std::array<double, 3> origin{0.0, 0.0, 0.0};
    REQUIRE(lighting::SelectActiveLights(set, origin, first) ==
        lighting::LightError::None);
    REQUIRE(lighting::SelectActiveLights(set, origin, second) ==
        lighting::LightError::None);
    CHECK(first.selected.size() == lighting::kMaximumActiveLights);
    CHECK(first.droppedCount == 8);
    // Deterministic: the same input selects the same lights, in the same
    // order, every time.
    CHECK(first.selected == second.selected);
    // Dropping is reported, never silent.
    CHECK(first.overflowed);

    lighting::LightSet small{};
    small.lights.push_back(set.lights.front());
    lighting::LightSelection fits{};
    REQUIRE(lighting::SelectActiveLights(small, origin, fits) ==
        lighting::LightError::None);
    CHECK_FALSE(fits.overflowed);
    CHECK(fits.droppedCount == 0);
}

TEST_CASE("P17_environment_state_selects_interior_and_exterior_without_stale",
    "[phase17][lighting]")
{
    lighting::EnvironmentCapture capture{};
    capture.interior = false;
    capture.ambient = {0.20f, 0.22f, 0.30f};
    capture.sunDirection = {0.0f, -0.6f, -0.8f};
    capture.sunColor = {1.0f, 0.95f, 0.85f};
    capture.sunIntensity = 3.0f;
    capture.fog.nearDistance = 500.0f;
    capture.fog.farDistance = 20000.0f;
    capture.fog.color = {0.55f, 0.60f, 0.68f};
    capture.fog.power = 1.0f;
    capture.fog.maximum = 1.0f;

    lighting::EnvironmentRecordV1 exterior{};
    REQUIRE(lighting::TranslateEnvironment(capture, exterior) ==
        lighting::LightError::None);
    CHECK_FALSE((exterior.flags & lighting::EnvironmentInterior) != 0);
    // The sun direction is normalized once, here, so no consumer has to.
    const auto length = std::sqrt(
        exterior.sunDirection[0] * exterior.sunDirection[0] +
        exterior.sunDirection[1] * exterior.sunDirection[1] +
        exterior.sunDirection[2] * exterior.sunDirection[2]);
    CHECK(length == Catch::Approx(1.0f));

    // An interior has no sun. Carrying the exterior's sun into an interior
    // is exactly the stale-state bug the gate names, so translation zeroes
    // it rather than trusting the capture.
    auto interiorCapture = capture;
    interiorCapture.interior = true;
    lighting::EnvironmentRecordV1 interior{};
    REQUIRE(lighting::TranslateEnvironment(interiorCapture, interior) ==
        lighting::LightError::None);
    CHECK((interior.flags & lighting::EnvironmentInterior) != 0);
    CHECK(interior.sunIntensity == Catch::Approx(0.0f));

    // A zero-length sun direction cannot be normalized and is not guessed.
    auto degenerate = capture;
    degenerate.sunDirection = {0.0f, 0.0f, 0.0f};
    lighting::EnvironmentRecordV1 rejected{};
    CHECK(lighting::TranslateEnvironment(degenerate, rejected) ==
        lighting::LightError::InvalidDirection);

    auto invertedFog = capture;
    invertedFog.fog.farDistance = 100.0f;
    CHECK(lighting::TranslateEnvironment(invertedFog, rejected) ==
        lighting::LightError::InvalidFogRange);
}

TEST_CASE("P17_fog_is_monotonic_bounded_and_respects_its_maximum",
    "[phase17][lighting]")
{
    lighting::EnvironmentCapture capture{};
    capture.ambient = {0.1f, 0.1f, 0.1f};
    capture.sunDirection = {0.0f, 0.0f, -1.0f};
    capture.sunIntensity = 1.0f;
    capture.fog.nearDistance = 1000.0f;
    capture.fog.farDistance = 9000.0f;
    capture.fog.color = {0.5f, 0.5f, 0.6f};
    capture.fog.power = 1.0f;
    capture.fog.maximum = 0.75f;
    lighting::EnvironmentRecordV1 record{};
    REQUIRE(lighting::TranslateEnvironment(capture, record) ==
        lighting::LightError::None);

    CHECK(lighting::EvaluateFog(record, 0.0f) == Catch::Approx(0.0f));
    CHECK(lighting::EvaluateFog(record, 1000.0f) == Catch::Approx(0.0f));
    const auto middle = lighting::EvaluateFog(record, 5000.0f);
    CHECK(middle > 0.0f);
    CHECK(middle < 0.75f);
    // Saturates at the captured maximum, never at one, or distant geometry
    // would vanish into fog the engine never applied.
    CHECK(lighting::EvaluateFog(record, 9000.0f) == Catch::Approx(0.75f));
    CHECK(lighting::EvaluateFog(record, 100000.0f) == Catch::Approx(0.75f));

    auto previous = -1.0f;
    for (int step = 0; step <= 32; ++step) {
        const auto distance = 9000.0f * static_cast<float>(step) / 32.0f;
        const auto value = lighting::EvaluateFog(record, distance);
        CHECK(value >= previous - 1.0e-6f);
        CHECK(value >= 0.0f);
        CHECK(value <= 0.75f + 1.0e-6f);
        previous = value;
    }
}

TEST_CASE("P17_weather_transition_blends_without_overshooting",
    "[phase17][lighting]")
{
    lighting::EnvironmentCapture fromCapture{};
    fromCapture.ambient = {0.2f, 0.2f, 0.2f};
    fromCapture.sunDirection = {0.0f, 0.0f, -1.0f};
    fromCapture.sunColor = {1.0f, 1.0f, 1.0f};
    fromCapture.sunIntensity = 4.0f;
    fromCapture.fog.nearDistance = 1000.0f;
    fromCapture.fog.farDistance = 20000.0f;
    fromCapture.fog.color = {0.6f, 0.6f, 0.7f};
    fromCapture.fog.power = 1.0f;
    fromCapture.fog.maximum = 1.0f;
    auto toCapture = fromCapture;
    toCapture.ambient = {0.05f, 0.05f, 0.07f};
    toCapture.sunIntensity = 0.5f;
    toCapture.fog.farDistance = 4000.0f;
    toCapture.fog.color = {0.3f, 0.3f, 0.32f};

    lighting::EnvironmentRecordV1 from{};
    lighting::EnvironmentRecordV1 to{};
    REQUIRE(lighting::TranslateEnvironment(fromCapture, from) ==
        lighting::LightError::None);
    REQUIRE(lighting::TranslateEnvironment(toCapture, to) ==
        lighting::LightError::None);

    lighting::EnvironmentRecordV1 blended{};
    REQUIRE(lighting::BlendEnvironment(from, to, 0.0f, blended) ==
        lighting::LightError::None);
    CHECK(blended.sunIntensity == Catch::Approx(4.0f));
    REQUIRE(lighting::BlendEnvironment(from, to, 1.0f, blended) ==
        lighting::LightError::None);
    CHECK(blended.sunIntensity == Catch::Approx(0.5f));
    REQUIRE(lighting::BlendEnvironment(from, to, 0.5f, blended) ==
        lighting::LightError::None);
    CHECK(blended.sunIntensity == Catch::Approx(2.25f));
    CHECK(blended.fogFar == Catch::Approx(12000.0f));

    // A transition factor outside its range is a captured contradiction, not
    // something to clamp into plausibility.
    lighting::EnvironmentRecordV1 rejected{};
    CHECK(lighting::BlendEnvironment(from, to, -0.1f, rejected) ==
        lighting::LightError::InvalidTransition);
    CHECK(lighting::BlendEnvironment(from, to, 1.1f, rejected) ==
        lighting::LightError::InvalidTransition);

    // Blending an interior with an exterior has no meaning: the engine cuts
    // between them, it does not cross-fade, and a blend would invent a state
    // the game never shows.
    auto interiorCapture = fromCapture;
    interiorCapture.interior = true;
    lighting::EnvironmentRecordV1 interior{};
    REQUIRE(lighting::TranslateEnvironment(interiorCapture, interior) ==
        lighting::LightError::None);
    CHECK(lighting::BlendEnvironment(interior, to, 0.5f, rejected) ==
        lighting::LightError::IncompatibleEnvironments);
}

TEST_CASE("P17_direct_lighting_is_unshadowed_and_says_so",
    "[phase17][lighting]")
{
    lighting::LightRecordV1 sun{};
    auto capture = BaseLight(lighting::LightType::Directional);
    capture.direction = {0.0f, 0.0f, -1.0f};
    capture.diffuse = {1.0f, 1.0f, 1.0f};
    capture.dimmer = 1.0f;
    REQUIRE(lighting::TranslateLight(capture, sun) ==
        lighting::LightError::None);

    lighting::SurfacePoint surface{};
    surface.position = {0.0f, 0.0f, 0.0f};
    surface.normal = {0.0f, 0.0f, 1.0f};

    const auto facing = lighting::EvaluateDirect(sun, surface);
    CHECK(facing.shadowed == false);
    CHECK(facing.available == false);
    CHECK(facing.radiance[0] == Catch::Approx(1.0f));

    // A surface facing away receives nothing; the cosine term is not
    // allowed to go negative and subtract light.
    lighting::SurfacePoint away{};
    away.position = {0.0f, 0.0f, 0.0f};
    away.normal = {0.0f, 0.0f, -1.0f};
    const auto behind = lighting::EvaluateDirect(sun, away);
    CHECK(behind.radiance[0] == Catch::Approx(0.0f));

    // Grazing incidence falls off with the cosine rather than cutting off.
    lighting::SurfacePoint grazing{};
    grazing.position = {0.0f, 0.0f, 0.0f};
    grazing.normal = {0.7071068f, 0.0f, 0.7071068f};
    const auto edge = lighting::EvaluateDirect(sun, grazing);
    CHECK(edge.radiance[0] == Catch::Approx(0.7071068f).margin(1.0e-5));
}

TEST_CASE("P17_light_packet_is_pointer_free_deterministic_and_checksummed",
    "[phase17][lighting]")
{
    lighting::LightPacket packet;
    packet.header.frameId = 0x1700'0000'0000'0007ull;
    packet.header.viewId = 0x1700'0000'0000'0009ull;
    for (std::uint32_t index = 0; index < 4; ++index) {
        auto capture = BaseLight(
            index == 0 ? lighting::LightType::Directional
                       : lighting::LightType::Point,
            0x1700'0000'0000'0100ull + index);
        lighting::LightRecordV1 record{};
        REQUIRE(lighting::TranslateLight(capture, record) ==
            lighting::LightError::None);
        packet.lights.push_back(record);
    }
    lighting::EnvironmentCapture environment{};
    environment.ambient = {0.2f, 0.2f, 0.25f};
    environment.sunDirection = {0.0f, -1.0f, -1.0f};
    environment.sunColor = {1.0f, 1.0f, 1.0f};
    environment.sunIntensity = 2.0f;
    environment.fog.nearDistance = 100.0f;
    environment.fog.farDistance = 10000.0f;
    environment.fog.color = {0.5f, 0.5f, 0.5f};
    environment.fog.power = 1.0f;
    environment.fog.maximum = 1.0f;
    REQUIRE(lighting::TranslateEnvironment(environment, packet.environment) ==
        lighting::LightError::None);
    REQUIRE(lighting::ValidateLightPacket(packet) ==
        lighting::LightPacketError::None);

    std::vector<std::byte> bytes;
    REQUIRE(lighting::EncodeLightPacket(packet, bytes) ==
        lighting::LightPacketError::None);
    lighting::LightPacket decoded;
    REQUIRE(lighting::DecodeLightPacket(bytes, decoded) ==
        lighting::LightPacketError::None);
    CHECK(decoded.lights.size() == packet.lights.size());
    CHECK(decoded.header.frameId == packet.header.frameId);

    std::vector<std::byte> reEncoded;
    REQUIRE(lighting::EncodeLightPacket(decoded, reEncoded) ==
        lighting::LightPacketError::None);
    CHECK(reEncoded == bytes);

    auto corrupted = bytes;
    corrupted[sizeof(lighting::LightPacketHeaderV1) + 8] ^= std::byte{0x20};
    lighting::LightPacket rejected;
    CHECK(lighting::DecodeLightPacket(corrupted, rejected) ==
        lighting::LightPacketError::ChecksumMismatch);

    auto truncated = bytes;
    truncated.resize(bytes.size() - 1);
    CHECK(lighting::DecodeLightPacket(truncated, rejected) ==
        lighting::LightPacketError::SizeMismatch);

    auto duplicated = packet;
    duplicated.lights[1].lightId = duplicated.lights[0].lightId;
    CHECK(lighting::ValidateLightPacket(duplicated) ==
        lighting::LightPacketError::DuplicateLight);

    CHECK(sizeof(lighting::LightRecordV1) == 112);
    CHECK(sizeof(lighting::EnvironmentRecordV1) == 112);
    CHECK(sizeof(lighting::LightPacketHeaderV1) == 64);
}
