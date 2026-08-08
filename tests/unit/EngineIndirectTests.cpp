#include "renderer_core/EngineIndirect.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace vf::renderer;

namespace {

[[nodiscard]] float Dot3(
    const std::array<float, 3>& a,
    const std::array<float, 3>& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

[[nodiscard]] float Length3(const std::array<float, 3>& value)
{
    return std::sqrt(Dot3(value, value));
}

void SetVec4(float (&destination)[4], const float x, const float y,
    const float z, const float w) noexcept
{
    destination[0] = x;
    destination[1] = y;
    destination[2] = z;
    destination[3] = w;
}

gi::SurfaceSample BaseSurface()
{
    gi::SurfaceSample surface{};
    surface.position = {0.0f, 0.0f, 0.0f};
    surface.geometricNormal = {0.0f, 0.0f, 1.0f};
    surface.albedo = {0.6f, 0.5f, 0.4f};
    surface.depth = 100.0f;
    surface.objectId = 0x3000'0000'0000'0001ull;
    surface.materialId = 0x3000'0000'0000'0002ull;
    return surface;
}

reflect::ReflectionHistoryKey BaseEpoch()
{
    reflect::ReflectionHistoryKey key{};
    key.cameraEpoch = 4;
    key.width = 128;
    key.height = 64;
    key.viewId = 2;
    return key;
}

}

TEST_CASE("P20_diffuse_sampling_is_cosine_weighted_and_deterministic",
    "[phase20][gi]")
{
    // The diffuse integral already carries a cosine. Sampling uniformly and
    // multiplying by it spends most rays near the horizon where they
    // contribute least, so the distribution has to carry the cosine instead.
    const std::array<float, 3> normal{0.0f, 0.0f, 1.0f};

    std::array<float, 3> first{};
    std::array<float, 3> again{};
    REQUIRE(gi::SampleDiffuseDirection(normal, {0.31f, 0.72f}, first) ==
        gi::IndirectError::None);
    REQUIRE(gi::SampleDiffuseDirection(normal, {0.31f, 0.72f}, again) ==
        gi::IndirectError::None);
    CHECK(first == again);

    // Every sample is in the hemisphere and unit length, or the integral is
    // being taken over the wrong domain.
    auto below = 0;
    auto total = 0.0f;
    constexpr int kSamples = 512;
    for (int index = 0; index < kSamples; ++index) {
        const auto pair = reflect::SampleSequence(5, 9, 1,
            static_cast<std::uint32_t>(index));
        std::array<float, 3> direction{};
        REQUIRE(gi::SampleDiffuseDirection(normal, pair, direction) ==
            gi::IndirectError::None);
        const auto cosine = Dot3(direction, normal);
        if (cosine <= 0.0f) ++below;
        total += cosine;
        CHECK(Length3(direction) == Catch::Approx(1.0f).margin(1.0e-4));
    }
    CHECK(below == 0);
    // A cosine-weighted hemisphere has a mean cosine of 2/3. A uniform one
    // would average 1/2, so this distinguishes the two rather than merely
    // checking the directions point the right way.
    CHECK(total / kSamples == Catch::Approx(2.0f / 3.0f).margin(0.05));

    // A degenerate normal has no hemisphere to sample.
    std::array<float, 3> ignored{};
    CHECK(gi::SampleDiffuseDirection({0.0f, 0.0f, 0.0f}, {0.5f, 0.5f},
        ignored) == gi::IndirectError::DegenerateNormal);
}

TEST_CASE("P20_indirect_excludes_direct_and_bounds_fireflies", "[phase20][gi]")
{
    // Indirect must exclude what the direct pass already added. Counting the
    // same light twice makes every interior bloom, and the error grows with
    // how well lit the room is, so it looks like a lighting bug rather than
    // double counting.
    const std::array<float, 3> total{1.0f, 0.8f, 0.6f};
    const std::array<float, 3> direct{0.4f, 0.5f, 0.6f};
    const auto indirect = gi::SeparateIndirect(total, direct);
    CHECK(indirect[0] == Catch::Approx(0.6f));
    CHECK(indirect[1] == Catch::Approx(0.3f));
    CHECK(indirect[2] == Catch::Approx(0.0f));

    // Never negative: a direct term larger than the total is a measurement
    // disagreement, not a negative light, and subtracting past zero would
    // darken the surface below its own albedo.
    const auto floored = gi::SeparateIndirect({0.1f, 0.1f, 0.1f},
        {0.5f, 0.5f, 0.5f});
    CHECK(floored[0] == Catch::Approx(0.0f));

    // One unlucky path that survives temporal accumulation shows as a bright
    // dot for seconds. Clamped rather than discarded: discarding biases the
    // mean darker, clamping only bounds the variance.
    gi::IndirectRules rules{};
    rules.radianceClamp = 4.0f;
    const auto bounded = gi::ClampRadiance({100.0f, 0.5f, 2.0f}, rules);
    CHECK(bounded[0] == Catch::Approx(4.0f));
    CHECK(bounded[1] == Catch::Approx(0.5f));
    CHECK(bounded[2] == Catch::Approx(2.0f));

    // Non-finite radiance is a failed path, not an infinitely bright one.
    const auto broken = gi::ClampRadiance(
        {std::numeric_limits<float>::infinity(), 0.0f,
         std::numeric_limits<float>::quiet_NaN()}, rules);
    CHECK(broken[0] == Catch::Approx(0.0f));
    CHECK(broken[2] == Catch::Approx(0.0f));
}

TEST_CASE("P20_history_is_refused_for_a_named_reason", "[phase20][gi]")
{
    // "The camera cut" and "this pixel is newly visible" need different
    // responses and are indistinguishable in a boolean, so the rejection
    // names itself.
    const auto current = BaseSurface();
    auto previous = current;
    const auto epoch = BaseEpoch();
    gi::IndirectRules rules{};

    const auto accepted = gi::Reproject(current, previous, {0.0f, 0.0f},
        10, 20, 128, 64, rules, epoch, epoch);
    CHECK(accepted.reason == gi::RejectReason::Accepted);
    CHECK(accepted.sourceX == 10);
    CHECK(accepted.sourceY == 20);

    // Motion moves where the sample is read from.
    const auto moved = gi::Reproject(current, previous, {-4.0f, 3.0f},
        10, 20, 128, 64, rules, epoch, epoch);
    CHECK(moved.reason == gi::RejectReason::Accepted);
    CHECK(moved.sourceX == 6);
    CHECK(moved.sourceY == 23);

    // A camera cut invalidates every pixel at once, whatever the surfaces say.
    auto cut = epoch;
    cut.cameraEpoch = 5;
    CHECK(gi::Reproject(current, previous, {0.0f, 0.0f}, 10, 20, 128, 64,
        rules, epoch, cut).reason == gi::RejectReason::Epoch);

    // Motion that leaves the frame has no history to read.
    CHECK(gi::Reproject(current, previous, {-40.0f, 0.0f}, 10, 20, 128, 64,
        rules, epoch, epoch).reason == gi::RejectReason::OffScreen);

    // Depth is compared relatively. An absolute epsilon fails at distance,
    // where a pixel's depth changes by metres between frames without moving.
    auto deeper = previous;
    deeper.depth = current.depth * 1.5f;
    CHECK(gi::Reproject(current, deeper, {0.0f, 0.0f}, 10, 20, 128, 64,
        rules, epoch, epoch).reason == gi::RejectReason::Depth);
    auto nudged = previous;
    nudged.depth = current.depth * 1.01f;
    CHECK(gi::Reproject(current, nudged, {0.0f, 0.0f}, 10, 20, 128, 64,
        rules, epoch, epoch).reason == gi::RejectReason::Accepted);
    // The same relative change at ten times the distance is still accepted,
    // which a fixed epsilon would have refused.
    auto far = current;
    far.depth = 1000.0f;
    auto farPrevious = far;
    farPrevious.depth = 1010.0f;
    CHECK(gi::Reproject(far, farPrevious, {0.0f, 0.0f}, 10, 20, 128, 64,
        rules, epoch, epoch).reason == gi::RejectReason::Accepted);

    auto turned = previous;
    turned.geometricNormal = {1.0f, 0.0f, 0.0f};
    CHECK(gi::Reproject(current, turned, {0.0f, 0.0f}, 10, 20, 128, 64,
        rules, epoch, epoch).reason == gi::RejectReason::Normal);

    // Object and material are separate tests: two objects can share a
    // material, and one object can change material without moving.
    auto other = previous;
    other.objectId = 0x3000'0000'0000'00FFull;
    CHECK(gi::Reproject(current, other, {0.0f, 0.0f}, 10, 20, 128, 64,
        rules, epoch, epoch).reason == gi::RejectReason::Object);
    auto repainted = previous;
    repainted.materialId = 0x3000'0000'0000'00FEull;
    CHECK(gi::Reproject(current, repainted, {0.0f, 0.0f}, 10, 20, 128, 64,
        rules, epoch, epoch).reason == gi::RejectReason::Material);

    // Both checks are policy and can be relaxed together without touching
    // the geometric ones.
    gi::IndirectRules relaxed{};
    relaxed.requireObjectMatch = false;
    relaxed.requireMaterialMatch = false;
    CHECK(gi::Reproject(current, other, {0.0f, 0.0f}, 10, 20, 128, 64,
        relaxed, epoch, epoch).reason == gi::RejectReason::Accepted);
}

TEST_CASE("P20_accumulation_converges_and_resets_without_trailing",
    "[phase20][gi]")
{
    gi::QualityPreset preset{};
    preset.maximumHistoryLength = 8;

    // A stationary pixel converges on its mean and its variance falls.
    gi::HistorySample history{};
    for (int frame = 0; frame < 64; ++frame) {
        history = gi::Accumulate(history, {1.0f, 1.0f, 1.0f},
            gi::RejectReason::Accepted, preset);
    }
    CHECK(history.mean[0] == Catch::Approx(1.0f).margin(1.0e-3));
    CHECK(history.length == preset.maximumHistoryLength);
    const auto settled = gi::Variance(history);
    CHECK(settled[0] == Catch::Approx(0.0f).margin(1.0e-3));

    // Alternating samples keep a real variance, which is what a denoiser
    // needs; a history that reports zero variance for noisy input would let
    // the filter stop filtering exactly where it is needed.
    gi::HistorySample noisy{};
    for (int frame = 0; frame < 64; ++frame) {
        const auto value = (frame % 2) == 0 ? 0.0f : 2.0f;
        noisy = gi::Accumulate(noisy, {value, value, value},
            gi::RejectReason::Accepted, preset);
    }
    CHECK(gi::Variance(noisy)[0] > 0.1f);

    // A rejected sample resets rather than blends. Blending a rejected
    // sample is the trail the gate forbids: the old scene stays visible,
    // fading, for as long as the history is long.
    const auto reset = gi::Accumulate(history, {0.0f, 0.0f, 0.0f},
        gi::RejectReason::Epoch, preset);
    CHECK(reset.length == 1);
    CHECK(reset.mean[0] == Catch::Approx(0.0f));
    CHECK(gi::Variance(reset)[0] == Catch::Approx(0.0f));

    // The cap is a quality knob: a longer history converges further and
    // responds later, and changing it must not change what a converged pixel
    // converges to.
    gi::QualityPreset longer{};
    longer.maximumHistoryLength = 64;
    gi::HistorySample slow{};
    for (int frame = 0; frame < 256; ++frame) {
        slow = gi::Accumulate(slow, {1.0f, 1.0f, 1.0f},
            gi::RejectReason::Accepted, longer);
    }
    CHECK(slow.mean[0] == Catch::Approx(history.mean[0]).margin(1.0e-3));
    CHECK(slow.length == longer.maximumHistoryLength);
}

TEST_CASE("P20_half_resolution_mapping_is_explicit", "[phase20][gi]")
{
    // A full-resolution pixel reading the wrong low-resolution sample smears
    // indirect light across every edge in the frame, so the mapping is stated
    // rather than assumed to be a shift.
    gi::QualityPreset full{};
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    REQUIRE(gi::MapToTraceResolution(9, 5, 128, 64, full, x, y, width,
        height) == gi::IndirectError::None);
    CHECK(x == 9);
    CHECK(y == 5);
    CHECK(width == 128);
    CHECK(height == 64);

    gi::QualityPreset half{};
    half.halfResolution = true;
    REQUIRE(gi::MapToTraceResolution(9, 5, 128, 64, half, x, y, width,
        height) == gi::IndirectError::None);
    CHECK(x == 4);
    CHECK(y == 2);
    CHECK(width == 64);
    CHECK(height == 32);

    // An odd extent still covers every pixel: rounding down would leave the
    // last column and row reading a sample that was never traced.
    REQUIRE(gi::MapToTraceResolution(126, 62, 127, 63, half, x, y, width,
        height) == gi::IndirectError::None);
    CHECK(width == 64);
    CHECK(height == 32);
    CHECK(x < width);
    CHECK(y < height);

    // A pixel outside the frame has no sample at all.
    CHECK(gi::MapToTraceResolution(200, 5, 128, 64, half, x, y, width,
        height) == gi::IndirectError::InvalidResolution);
    CHECK(gi::MapToTraceResolution(9, 5, 0, 64, half, x, y, width, height)
        == gi::IndirectError::InvalidResolution);
}

TEST_CASE("P20_indirect_light_bounces_once_and_never_leaks_indoors",
    "[phase20][gi]")
{
    // A lit panel above a surface should bounce light down onto it. If the
    // hemisphere sample or the shading is wrong the surface stays black, and
    // if the environment leaks indoors it brightens with no geometry to
    // justify it.
    auto surface = BaseSurface();
    surface.albedo = {1.0f, 1.0f, 1.0f};

    reflect::ReflectionTriangle panel{};
    panel.a = {-8.0f, -8.0f, 6.0f};
    panel.b = {8.0f, -8.0f, 6.0f};
    panel.c = {0.0f, 8.0f, 6.0f};
    panel.normal = {0.0f, 0.0f, -1.0f};
    panel.albedo = {0.9f, 0.2f, 0.2f};
    panel.twoSided = true;
    const std::array<reflect::ReflectionTriangle, 1> geometry{panel};

    lighting::GpuEnvironmentV1 interior{};
    interior.flagsAndCount[0] =
        lighting::EnvironmentPresent | lighting::EnvironmentInterior;
    interior.flagsAndCount[1] = 1;
    SetVec4(interior.ambientAndFogNear, 0.0f, 0.0f, 0.0f, 1.0e9f);
    SetVec4(interior.sunDirectionAndFogFar, 0.0f, 0.0f, -1.0f, 2.0e9f);
    SetVec4(interior.sunColorAndIntensity, 1.0f, 1.0f, 1.0f, 1.0f);
    SetVec4(interior.fogColorAndPower, 0.0f, 0.0f, 0.0f, 1.0f);
    SetVec4(interior.moonColorAndFogMaximum, 0.0f, 0.0f, 0.0f, 0.0f);

    std::array<lighting::GpuLightRecordV1, 1> lights{};
    SetVec4(lights[0].color, 2.0f, 2.0f, 2.0f,
        static_cast<float>(lighting::LightType::Directional));
    // Travelling up, so the panel's downward face is lit.
    SetVec4(lights[0].direction, 0.0f, 0.0f, 1.0f, 0.0f);

    gi::IndirectRules rules{};
    gi::QualityPreset preset{};
    preset.raysPerPixel = 64;

    gi::IndirectSource source{};
    const auto bounced = gi::EvaluateIndirect(surface, rules, preset,
        geometry, lights, interior, 11, 7, 3, source);
    CHECK(source == gi::IndirectSource::Geometry);
    // The panel is red, so the bounce arriving below it is red.
    CHECK(bounced[0] > bounced[1]);
    CHECK(bounced[0] > 0.0f);

    // Indoors with no geometry there is nothing to bounce from, and the
    // exterior sky must not stand in for it. That substitution is the light
    // leak this rule exists to prevent.
    gi::IndirectSource emptySource{};
    const auto leaked = gi::EvaluateIndirect(surface, rules, preset, {},
        lights, interior, 11, 7, 3, emptySource);
    CHECK(emptySource == gi::IndirectSource::Unresolved);
    CHECK(leaked[0] == Catch::Approx(0.0f));

    // The same surface outdoors picks the sky up, because outdoors there is
    // a sky for a ray that escapes to have come from.
    lighting::GpuEnvironmentV1 exterior{interior};
    exterior.flagsAndCount[0] = lighting::EnvironmentPresent;
    gi::IndirectSource skySource{};
    const auto sky = gi::EvaluateIndirect(surface, rules, preset, {},
        lights, exterior, 11, 7, 3, skySource);
    CHECK(skySource == gi::IndirectSource::Environment);
    CHECK(sky[0] >= 0.0f);

    // Zero rays traces nothing and says so rather than returning black as
    // though it had looked.
    gi::QualityPreset none{};
    none.raysPerPixel = 0;
    gi::IndirectSource skipped{};
    static_cast<void>(gi::EvaluateIndirect(surface, rules, none, geometry,
        lights, interior, 11, 7, 3, skipped));
    CHECK(skipped == gi::IndirectSource::Skipped);
}

TEST_CASE("P20_the_environment_can_switch_the_bounce_off", "[phase20][gi]")
{
    // Isolating a ray-traced term means rendering the same frame twice with
    // only that term changing. Every other term already has such a switch on
    // the device; the bounce did not, which is why the reflection contract
    // could not tell a reflection from the indirect light that arrives with
    // it -- both moved together and neither could be measured alone.
    lighting::GpuEnvironmentV1 environment{};
    environment.flagsAndCount[0] = lighting::EnvironmentPresent;
    environment.ambientAndFogNear[0] = 0.25f;
    environment.ambientAndFogNear[1] = 0.25f;
    environment.ambientAndFogNear[2] = 0.25f;

    gi::SurfaceSample surface{};
    surface.albedo = {0.8f, 0.8f, 0.8f};

    gi::IndirectRules rules{};
    gi::QualityPreset preset{};
    preset.raysPerPixel = 4;

    const std::array<reflect::ReflectionTriangle, 0> geometry{};
    const std::array<lighting::GpuLightRecordV1, 0> lights{};
    gi::IndirectSource source{};
    // With the switch set the bounce is exactly nothing, not merely small.
    // A term that is "almost off" still moves every pixel it touches, and an
    // isolation built on it measures the remainder rather than the term.
    environment.flagsAndCount[0] |= lighting::EnvironmentIndirectDisabled;
    const auto unlit = gi::EvaluateIndirect(surface, rules, preset, geometry,
        lights, environment, 0, 0, 0, source);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        CHECK(unlit[channel] == 0.0f);
    }

    // And the flag is not confused with the ones beside it: a frame that
    // merely has no environment must not read as one that switched the
    // bounce off, or the two cases would be indistinguishable in a capture.
    CHECK((lighting::EnvironmentIndirectDisabled &
        (lighting::EnvironmentInterior | lighting::EnvironmentPresent)) == 0u);
    static_cast<void>(source);
}

TEST_CASE("P20_gpu_indirect_records_carry_what_reprojection_needs")
{
    // The temporal half of the pass runs on the device over per-pixel records,
    // so the records have to carry everything Reproject and Accumulate read
    // and nothing they do not. A record missing one of these turns a gate off
    // silently: the shader still compiles, the pass still runs, and the frame
    // grows a trail behind moving geometry that reads as the filter being too
    // slow rather than as a field that was never uploaded.
    SECTION("the layouts match what std430 will read them as")
    {
        // A stride that disagrees with the host struct neither fails to
        // compile nor trips validation. Every pixel is simply read at the
        // wrong address.
        CHECK(sizeof(gi::GpuIndirectPixelV1) == 64);
        CHECK(alignof(gi::GpuIndirectPixelV1) == 16);
        CHECK(sizeof(gi::GpuIndirectHistoryV1) == 32);
        CHECK(alignof(gi::GpuIndirectHistoryV1) == 16);
        CHECK(sizeof(gi::GpuIndirectResultV1) == 32);
        CHECK(alignof(gi::GpuIndirectResultV1) == 16);
        // std430 rounds a vec3 up to four floats, so every member that the
        // shader reads as a vec3 must start on a four-float boundary.
        CHECK(offsetof(gi::GpuIndirectPixelV1, normal) % 16 == 0);
        CHECK(offsetof(gi::GpuIndirectPixelV1, radiance) % 16 == 0);
        CHECK(offsetof(gi::GpuIndirectHistoryV1, mean) % 16 == 0);
        CHECK(offsetof(gi::GpuIndirectHistoryV1, secondMoment) % 16 == 0);
        CHECK(offsetof(gi::GpuIndirectResultV1, mean) % 16 == 0);
        CHECK(offsetof(gi::GpuIndirectResultV1, variance) % 16 == 0);
    }

    SECTION("a surface sample survives the trip through the record")
    {
        gi::SurfaceSample surface{};
        surface.geometricNormal = {0.0f, 0.6f, 0.8f};
        surface.depth = 1234.5f;
        // Both halves of each identity must arrive. A record that narrowed
        // them to thirty-two bits would let two objects whose low words agree
        // reproject into each other, which is a smear across the seam between
        // them and nowhere else.
        surface.objectId = 0x1122'3344'5566'7788ull;
        surface.materialId = 0x99AA'BBCC'DDEE'FF00ull;
        const std::array<float, 2> motion{-3.5f, 7.25f};
        const std::array<float, 3> radiance{0.25f, 0.5f, 0.75f};

        const auto record = gi::BuildGpuIndirectPixel(surface, motion,
            radiance);
        CHECK(record.normal[0] == surface.geometricNormal[0]);
        CHECK(record.normal[1] == surface.geometricNormal[1]);
        CHECK(record.normal[2] == surface.geometricNormal[2]);
        CHECK(record.depth == surface.depth);
        CHECK(record.motion[0] == motion[0]);
        CHECK(record.motion[1] == motion[1]);
        CHECK(record.radiance[0] == radiance[0]);
        CHECK(record.radiance[2] == radiance[2]);
        const auto object =
            static_cast<std::uint64_t>(record.objectId[0]) |
            (static_cast<std::uint64_t>(record.objectId[1]) << 32);
        const auto material =
            static_cast<std::uint64_t>(record.materialId[0]) |
            (static_cast<std::uint64_t>(record.materialId[1]) << 32);
        CHECK(object == surface.objectId);
        CHECK(material == surface.materialId);
    }

    SECTION("a history record round-trips through the device form")
    {
        gi::HistorySample history{};
        history.mean = {0.125f, 0.25f, 0.5f};
        history.secondMoment = {0.5f, 1.0f, 2.0f};
        history.length = 17;
        const auto record = gi::BuildGpuIndirectHistory(history);
        const auto restored = gi::ReadGpuIndirectHistory(record);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            INFO(channel);
            CHECK(restored.mean[channel] == history.mean[channel]);
            CHECK(restored.secondMoment[channel] ==
                history.secondMoment[channel]);
        }
        CHECK(restored.length == history.length);
    }
}
