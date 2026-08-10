#include "renderer_core/EngineWater.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace vf::renderer;

namespace {

water::WaterMaterialV1 BaseWater()
{
    water::WaterMaterialV1 material{};
    material.materialId = 0x2200'0000'0000'0001ull;
    material.surfaceClass = water::TransmissiveClass::Water;
    // Three layers at different speeds and scales. One layer reads as a
    // single sliding texture however it is tuned.
    material.layers[0] = {{0.05f, 0.02f}, 1.0f, 1.0f};
    material.layers[1] = {{-0.03f, 0.04f}, 2.0f, 0.6f};
    material.layers[2] = {{0.01f, -0.05f}, 4.0f, 0.3f};
    material.shallowColor = {0.20f, 0.45f, 0.40f};
    material.deepColor = {0.02f, 0.08f, 0.14f};
    material.fogColor = {0.05f, 0.15f, 0.20f};
    material.siltColor = {0.30f, 0.28f, 0.20f};
    material.depthRange = 200.0f;
    material.shorelineDepth = 12.0f;
    material.fogDensity = 0.01f;
    material.siltDensity = 0.0f;
    material.fresnelBias = 0.02f;
    material.sparklePower = 64.0f;
    return material;
}

}

TEST_CASE("P22_water_animates_from_three_independent_layers", "[phase22][water]")
{
    // Three layers at different speeds is what stops water reading as a
    // single sliding texture, so the count is part of the contract rather
    // than a quality setting that can be turned down to one.
    const auto material = BaseWater();
    CHECK(water::ValidateMaterial(material) == water::WaterError::None);

    std::array<float, 3> first{};
    std::array<float, 3> later{};
    REQUIRE(water::EvaluateNormal(material, {10.0f, 4.0f}, 0.0f, first));
    REQUIRE(water::EvaluateNormal(material, {10.0f, 4.0f}, 1.7f, later));
    CHECK(first != later);

    // Always normalized and always facing up: a water normal that tips past
    // the horizon lights the surface from beneath it.
    for (const auto time : {0.0f, 0.5f, 3.25f, 11.0f}) {
        std::array<float, 3> normal{};
        REQUIRE(water::EvaluateNormal(material, {3.0f, -7.0f}, time, normal));
        const auto length = std::sqrt(normal[0] * normal[0] +
            normal[1] * normal[1] + normal[2] * normal[2]);
        CHECK(length == Catch::Approx(1.0f).margin(1.0e-4));
        CHECK(normal[2] > 0.0f);
    }

    // Each layer contributes: silencing the third must change the result, or
    // the third layer is decoration and the animation is really two layers.
    auto quieter = material;
    quieter.layers[2].amplitude = 0.0f;
    std::array<float, 3> withThird{};
    std::array<float, 3> withoutThird{};
    REQUIRE(water::EvaluateNormal(material, {1.0f, 1.0f}, 2.0f, withThird));
    REQUIRE(water::EvaluateNormal(quieter, {1.0f, 1.0f}, 2.0f, withoutThird));
    CHECK(withThird != withoutThird);

    // A layer that scrolls nowhere at every scale is a captured
    // contradiction, not still water: still water has a flat normal, which
    // the amplitudes express.
    auto frozen = material;
    for (auto& layer : frozen.layers) layer.scale = 0.0f;
    CHECK(water::ValidateMaterial(frozen) == water::WaterError::InvalidScroll);
}

TEST_CASE("P22_water_colour_follows_depth_and_fades_at_the_shore",
    "[phase22][water]")
{
    const auto material = BaseWater();

    // Shallow at the surface, deep far down, and monotonic between: a
    // non-monotonic blend puts a bright band at a depth with nothing there.
    const auto surface = water::EvaluateWaterColor(material, 0.0f);
    const auto mid = water::EvaluateWaterColor(material, 100.0f);
    const auto deep = water::EvaluateWaterColor(material, 400.0f);
    CHECK(surface[1] == Catch::Approx(material.shallowColor[1]));
    CHECK(deep[1] == Catch::Approx(material.deepColor[1]));
    CHECK(mid[1] < surface[1]);
    CHECK(mid[1] > deep[1]);

    // Beyond the range it stays at the deep colour rather than continuing
    // past it into a colour the capture never authored.
    const auto deeper = water::EvaluateWaterColor(material, 4000.0f);
    CHECK(deeper[1] == Catch::Approx(deep[1]));

    // Silt lifts the colour when it is authored, and does nothing when it is
    // not, which is what keeps clear water clear.
    auto silty = material;
    silty.siltDensity = 0.8f;
    const auto muddy = water::EvaluateWaterColor(silty, 100.0f);
    CHECK(muddy[0] > mid[0]);

    // Water that ends abruptly draws a hard line across the sand.
    CHECK(water::ShorelineCoverage(material, 0.0f) == Catch::Approx(0.0f));
    CHECK(water::ShorelineCoverage(material, 100.0f) == Catch::Approx(1.0f));
    const auto edge = water::ShorelineCoverage(material, 6.0f);
    CHECK(edge > 0.0f);
    CHECK(edge < 1.0f);
    CHECK(water::ShorelineCoverage(material, 9.0f) > edge);

    // Without an authored shoreline the surface does not fade at all, rather
    // than fading over some invented distance.
    auto hard = material;
    hard.shorelineDepth = 0.0f;
    CHECK(water::ShorelineCoverage(hard, 0.01f) == Catch::Approx(1.0f));
}

TEST_CASE("P22_missing_transmission_uses_a_documented_default_not_the_image",
    "[phase22][water]")
{
    // Inferring an index of refraction from the rendered image produces a
    // different answer every frame the camera moves, which is
    // indistinguishable from a flickering material. A documented physical
    // constant is used instead, and the fact that it was used is reported.
    auto captured = BaseWater();
    captured.indexOfRefraction = 1.31f;
    captured.hasTransmissionMetadata = true;
    float ior = 0.0f;
    bool fromCapture = false;
    REQUIRE(water::ResolveIndexOfRefraction(captured, ior, fromCapture) ==
        water::WaterError::None);
    CHECK(ior == Catch::Approx(1.31f));
    CHECK(fromCapture);

    auto bare = BaseWater();
    REQUIRE(water::ResolveIndexOfRefraction(bare, ior, fromCapture) ==
        water::WaterError::MissingTransmission);
    CHECK(ior == Catch::Approx(water::kWaterIor));
    CHECK_FALSE(fromCapture);

    auto glass = BaseWater();
    glass.surfaceClass = water::TransmissiveClass::Glass;
    REQUIRE(water::ResolveIndexOfRefraction(glass, ior, fromCapture) ==
        water::WaterError::MissingTransmission);
    CHECK(ior == Catch::Approx(water::kGlassIor));

    // A class with no documented default borrows nothing and keeps its
    // vanilla path: a wrong constant looks deliberate and is very hard to
    // trace back to a missing capture.
    auto unknown = BaseWater();
    unknown.surfaceClass = water::TransmissiveClass::Unknown;
    CHECK(water::ResolveIndexOfRefraction(unknown, ior, fromCapture) ==
        water::WaterError::MissingTransmission);
    CHECK(ior == Catch::Approx(0.0f));

    // A captured value that is physically impossible is refused rather than
    // clamped into plausibility.
    auto broken = BaseWater();
    broken.hasTransmissionMetadata = true;
    broken.indexOfRefraction = 0.5f;
    CHECK(water::ResolveIndexOfRefraction(broken, ior, fromCapture) ==
        water::WaterError::InvalidIor);

    // Fresnel rises to one at grazing incidence whatever the index is, which
    // is what makes a water surface mirror-like at a shallow angle.
    const auto head = water::FresnelReflectance(captured, 1.0f);
    const auto grazing = water::FresnelReflectance(captured, 0.0f);
    CHECK(grazing == Catch::Approx(1.0f));
    CHECK(head < 0.1f);
    CHECK(grazing > head);
}

TEST_CASE("P22_reflection_and_refraction_selection_is_policy",
    "[phase22][water]")
{
    const auto material = BaseWater();
    water::WaterPolicy policy{};
    policy.tracingAvailable = true;
    policy.screenSpaceAvailable = true;
    policy.tracedReflectionCutoff = 0.4f;

    CHECK(water::SelectReflection(material, policy, 0.1f) ==
        water::ReflectionMode::Traced);
    // Too rough to resolve with a traced ray, exactly as the reflection pass
    // decides; the screen-space path still has something to show.
    CHECK(water::SelectReflection(material, policy, 0.9f) ==
        water::ReflectionMode::ScreenSpace);

    water::WaterPolicy noTracing{};
    noTracing.tracingAvailable = false;
    CHECK(water::SelectReflection(material, noTracing, 0.1f) ==
        water::ReflectionMode::ScreenSpace);

    water::WaterPolicy nothing{};
    nothing.tracingAvailable = false;
    nothing.screenSpaceAvailable = false;
    CHECK(water::SelectReflection(material, nothing, 0.1f) ==
        water::ReflectionMode::None);
    // Nothing captured what is behind the surface, and saying so beats
    // inventing a background.
    CHECK(water::SelectRefraction(material, nothing) ==
        water::RefractionSource::Unavailable);

    CHECK(water::SelectRefraction(material, policy) ==
        water::RefractionSource::Traced);
    CHECK(water::SelectRefraction(material, noTracing) ==
        water::RefractionSource::ScreenSpace);

    // The same material picks differently under a different policy without
    // being rewritten, which is the whole point of keeping this out of the
    // material record.
    CHECK(water::SelectReflection(material, policy, 0.1f) !=
        water::SelectReflection(material, noTracing, 0.1f));
}

TEST_CASE("P22_the_reflection_plane_and_underwater_transition_are_exact",
    "[phase22][water]")
{
    water::ReflectionPlane plane{};
    plane.normal = {0.0f, 0.0f, 1.0f};
    plane.height = 50.0f;

    std::array<float, 3> mirrored{};
    REQUIRE(water::ReflectAboutPlane(plane, {3.0f, -4.0f, 70.0f}, mirrored) ==
        water::WaterError::None);
    CHECK(mirrored[0] == Catch::Approx(3.0f));
    CHECK(mirrored[1] == Catch::Approx(-4.0f));
    CHECK(mirrored[2] == Catch::Approx(30.0f));

    // A point on the plane is its own mirror, so a surface never reflects
    // itself to somewhere else.
    REQUIRE(water::ReflectAboutPlane(plane, {1.0f, 1.0f, 50.0f}, mirrored) ==
        water::WaterError::None);
    CHECK(mirrored[2] == Catch::Approx(50.0f));

    // Reflecting about a guessed plane puts the mirrored world somewhere that
    // looks deliberate and is wrong, so a degenerate one is refused.
    water::ReflectionPlane degenerate{};
    degenerate.normal = {0.0f, 0.0f, 0.0f};
    CHECK(water::ReflectAboutPlane(degenerate, {0.0f, 0.0f, 0.0f}, mirrored)
        == water::WaterError::DegeneratePlane);

    // Above, below, and the crossing between them. A crossing invalidates
    // every history that assumed the other medium, so it is reported rather
    // than left for a consumer to detect by comparing states.
    const auto above = water::EvaluateUnderwater(plane, {0.0f, 0.0f, 60.0f},
        {0.0f, 0.0f, 62.0f});
    CHECK_FALSE(above.submerged);
    CHECK_FALSE(above.crossedThisFrame);

    const auto below = water::EvaluateUnderwater(plane, {0.0f, 0.0f, 40.0f},
        {0.0f, 0.0f, 38.0f});
    CHECK(below.submerged);
    CHECK(below.depth == Catch::Approx(10.0f));
    CHECK_FALSE(below.crossedThisFrame);

    const auto diving = water::EvaluateUnderwater(plane, {0.0f, 0.0f, 40.0f},
        {0.0f, 0.0f, 60.0f});
    CHECK(diving.submerged);
    CHECK(diving.crossedThisFrame);

    const auto surfacing = water::EvaluateUnderwater(plane,
        {0.0f, 0.0f, 60.0f}, {0.0f, 0.0f, 40.0f});
    CHECK_FALSE(surfacing.submerged);
    CHECK(surfacing.crossedThisFrame);

    // At the boundary the depth is zero, so a transition can be driven
    // continuously instead of snapping.
    const auto boundary = water::EvaluateUnderwater(plane,
        {0.0f, 0.0f, 50.0f}, {0.0f, 0.0f, 50.0f});
    CHECK(boundary.depth == Catch::Approx(0.0f));
}

TEST_CASE("P22_underwater_fog_absorbs_with_distance", "[phase22][water]")
{
    const auto material = BaseWater();
    const std::array<float, 3> incoming{1.0f, 1.0f, 1.0f};

    // Nothing at zero distance: a surface touching the camera is not fogged.
    const auto none = water::UnderwaterFog(material, incoming, 0.0f);
    CHECK(none[0] == Catch::Approx(incoming[0]));

    // Monotonic toward the fog colour, never past it: overshooting would
    // brighten distant water above the colour the capture authored.
    const auto near = water::UnderwaterFog(material, incoming, 50.0f);
    const auto far = water::UnderwaterFog(material, incoming, 500.0f);
    CHECK(near[0] < incoming[0]);
    CHECK(far[0] < near[0]);
    CHECK(far[0] >= std::min(material.fogColor[0], incoming[0]));

    // At extreme distance it is the fog colour and stays there.
    const auto extreme = water::UnderwaterFog(material, incoming, 100000.0f);
    CHECK(extreme[0] == Catch::Approx(material.fogColor[0]).margin(1.0e-3));

    // Without density authored the water does not absorb, rather than
    // absorbing at some invented rate.
    auto clear = material;
    clear.fogDensity = 0.0f;
    const auto unfogged = water::UnderwaterFog(clear, incoming, 500.0f);
    CHECK(unfogged[0] == Catch::Approx(incoming[0]));

    // A negative distance is behind the camera, not negative fog.
    const auto behind = water::UnderwaterFog(material, incoming, -10.0f);
    CHECK(behind[0] == Catch::Approx(incoming[0]));
}

TEST_CASE("P22_reflection_and_transmission_sum_to_one", "[phase22][water]")
{
    // The split is a partition, not two independently tuned terms. Anything
    // else either creates light at grazing angles -- where Fresnel approaches
    // one and a separately authored transmission is still adding -- or loses
    // it head-on, and both read as the water being the wrong colour rather
    // than as the split being wrong.
    water::WaterMaterialV1 material{};
    material.materialId = 0x2200'0000'0000'0001ull;
    material.surfaceClass = water::TransmissiveClass::Water;
    material.indexOfRefraction = water::kWaterIor;
    material.depthRange = 10.0f;
    material.shallowColor = {0.10f, 0.30f, 0.35f};
    material.deepColor = {0.02f, 0.08f, 0.12f};

    const std::array<float, 3> reflected{1.0f, 1.0f, 1.0f};
    const std::array<float, 3> refracted{1.0f, 1.0f, 1.0f};

    // With no fog, an equal reflection and transmission must come back
    // unchanged at every angle: the two weights are a partition of one.
    for (const auto cosine : {0.0f, 0.15f, 0.5f, 0.85f, 1.0f}) {
        INFO(cosine);
        const auto shaded = water::ShadeWater(
            material, reflected, refracted, cosine, 0.0f);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            CHECK(shaded[channel] == Catch::Approx(1.0f).margin(1.0e-5));
        }
    }

    // Head-on, water reflects very little: the transmitted side dominates.
    const std::array<float, 3> sky{1.0f, 0.0f, 0.0f};
    const std::array<float, 3> bed{0.0f, 0.0f, 1.0f};
    const auto straightOn = water::ShadeWater(material, sky, bed, 1.0f, 0.0f);
    CHECK(straightOn[2] > 0.9f);
    CHECK(straightOn[0] < 0.1f);

    // At a grazing angle it reflects almost everything, which is what makes a
    // lake mirror the sky at a distance and show its bed underfoot.
    const auto grazing = water::ShadeWater(material, sky, bed, 0.0f, 0.0f);
    CHECK(grazing[0] > 0.9f);
    CHECK(grazing[2] < 0.1f);

    // Fog applies to the transmitted side only. Fogging the reflection would
    // dim the sky by the depth of the water under it, which is wrong at every
    // depth and most obviously wrong where the water is deepest.
    material.fogDensity = 0.5f;
    material.fogColor = {0.0f, 1.0f, 0.0f};
    const auto fogged = water::ShadeWater(material, sky, bed, 1.0f, 4.0f);
    CHECK(fogged[1] > 0.5f);
    const auto foggedGrazing =
        water::ShadeWater(material, sky, bed, 0.0f, 4.0f);
    CHECK(foggedGrazing[0] > 0.9f);
    CHECK(foggedGrazing[1] < 0.1f);
}
