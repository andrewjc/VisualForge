#include "renderer_core/EngineReflection.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace vf::renderer;

namespace {

[[nodiscard]] float Length3(const std::array<float, 3>& value)
{
    return std::sqrt(value[0] * value[0] + value[1] * value[1] +
        value[2] * value[2]);
}

[[nodiscard]] float Dot3(
    const std::array<float, 3>& a,
    const std::array<float, 3>& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// The GPU records use raw float arrays so their layout matches the shader
// block exactly, which a std::array would not guarantee.
void SetVec4(float (&destination)[4], const float x, const float y,
    const float z, const float w) noexcept
{
    destination[0] = x;
    destination[1] = y;
    destination[2] = z;
    destination[3] = w;
}

reflect::ReflectionSurface FlatSurface()
{
    reflect::ReflectionSurface surface{};
    surface.position = {3.0f, -2.0f, 11.0f};
    surface.geometricNormal = {0.0f, 0.0f, 1.0f};
    surface.shadingNormal = {0.0f, 0.0f, 1.0f};
    // Looking straight down at the surface.
    surface.viewDirection = {0.0f, 0.0f, 1.0f};
    surface.baseColor = {0.80f, 0.55f, 0.20f};
    surface.roughness = 0.10f;
    surface.metalness = 0.0f;
    return surface;
}

}

TEST_CASE("P19_f0_separates_dielectrics_from_metals", "[phase19][reflect]")
{
    // A dielectric reflects about 4% of light at normal incidence whatever
    // its colour; a metal reflects its own base colour and has no diffuse
    // term. Treating them with one rule makes gold grey or plastic gold.
    const std::array<float, 3> gold{1.00f, 0.77f, 0.34f};

    const auto dielectric = reflect::ComputeF0(gold, 0.0f);
    CHECK(dielectric[0] == Catch::Approx(reflect::kDielectricF0));
    CHECK(dielectric[1] == Catch::Approx(reflect::kDielectricF0));
    CHECK(dielectric[2] == Catch::Approx(reflect::kDielectricF0));

    const auto metal = reflect::ComputeF0(gold, 1.0f);
    CHECK(metal[0] == Catch::Approx(gold[0]));
    CHECK(metal[1] == Catch::Approx(gold[1]));
    CHECK(metal[2] == Catch::Approx(gold[2]));

    // Captured materials do author intermediate metalness, so the two
    // endpoints interpolate rather than snapping at a threshold.
    const auto partial = reflect::ComputeF0(gold, 0.5f);
    CHECK(partial[0] == Catch::Approx(
        (reflect::kDielectricF0 + gold[0]) * 0.5f));

    // Fresnel rises to one at grazing incidence for every material. A
    // reflection that stays at F0 edge-on loses the rim that makes a wet or
    // polished surface read as one.
    const auto head = reflect::FresnelSchlick(dielectric, 1.0f);
    const auto grazing = reflect::FresnelSchlick(dielectric, 0.0f);
    CHECK(head[0] == Catch::Approx(reflect::kDielectricF0));
    CHECK(grazing[0] == Catch::Approx(1.0f));
    CHECK(grazing[0] > head[0]);
}

TEST_CASE("P19_mirror_direction_reflects_about_the_normal",
    "[phase19][reflect]")
{
    const std::array<float, 3> normal{0.0f, 0.0f, 1.0f};
    // Arriving at 45 degrees.
    const auto root = 1.0f / std::sqrt(2.0f);
    const std::array<float, 3> view{root, 0.0f, root};

    const auto mirror = reflect::MirrorDirection(view, normal);
    CHECK(mirror[0] == Catch::Approx(-root));
    CHECK(mirror[1] == Catch::Approx(0.0f));
    CHECK(mirror[2] == Catch::Approx(root));
    CHECK(Length3(mirror) == Catch::Approx(1.0f));

    // The angle of reflection equals the angle of incidence.
    CHECK(Dot3(mirror, normal) == Catch::Approx(Dot3(view, normal)));

    // Straight on reflects straight back.
    const auto straight = reflect::MirrorDirection(normal, normal);
    CHECK(straight[2] == Catch::Approx(1.0f));
}

TEST_CASE("P19_sampling_is_deterministic_and_stays_above_the_surface",
    "[phase19][reflect]")
{
    // The GPU and this oracle must walk the same sequence, or the fixture
    // compares two different sets of rays and reports the difference as a
    // reflection error.
    const auto first = reflect::SampleSequence(37, 91, 5, 0);
    const auto again = reflect::SampleSequence(37, 91, 5, 0);
    CHECK(first[0] == again[0]);
    CHECK(first[1] == again[1]);
    CHECK(first[0] >= 0.0f);
    CHECK(first[0] < 1.0f);
    CHECK(first[1] >= 0.0f);
    CHECK(first[1] < 1.0f);

    // Different pixels, frames, and samples must not walk the same sequence,
    // or a whole tile shares one direction and the reflection bands.
    CHECK((first != reflect::SampleSequence(38, 91, 5, 0)));
    CHECK((first != reflect::SampleSequence(37, 92, 5, 0)));
    CHECK((first != reflect::SampleSequence(37, 91, 6, 0)));
    CHECK((first != reflect::SampleSequence(37, 91, 5, 1)));

    // A mirror surface samples its mirror direction exactly, whatever the
    // sample is: a zero-roughness lobe is a delta, not the limit of a
    // sampling scheme.
    auto mirror = FlatSurface();
    mirror.roughness = 0.0f;
    std::array<float, 3> direction{};
    REQUIRE(reflect::SampleReflectionDirection(mirror, {0.37f, 0.81f},
        direction));
    const auto exact = reflect::MirrorDirection(
        mirror.viewDirection, mirror.shadingNormal);
    CHECK(direction[0] == Catch::Approx(exact[0]).margin(1.0e-6));
    CHECK(direction[1] == Catch::Approx(exact[1]).margin(1.0e-6));
    CHECK(direction[2] == Catch::Approx(exact[2]).margin(1.0e-6));

    // Every accepted rough sample stays in the hemisphere around the shading
    // normal and stays unit length.
    auto rough = FlatSurface();
    rough.roughness = 0.45f;
    auto accepted = 0;
    for (std::uint32_t index = 0; index < 256; ++index) {
        const auto sample = reflect::SampleSequence(11, 13, 2, index);
        std::array<float, 3> sampled{};
        if (!reflect::SampleReflectionDirection(rough, sample, sampled)) {
            continue;
        }
        ++accepted;
        CHECK(Dot3(sampled, rough.shadingNormal) > 0.0f);
        CHECK(Length3(sampled) == Catch::Approx(1.0f).margin(1.0e-4));
    }
    // A rejection scheme that rejects everything is not a sampler.
    CHECK(accepted > 128);

    // A rougher lobe spreads further from the mirror direction than a
    // smoother one. Without this the roughness input does nothing.
    const auto spread = [](const float roughness) {
        auto surface = FlatSurface();
        surface.roughness = roughness;
        const auto centre = reflect::MirrorDirection(
            surface.viewDirection, surface.shadingNormal);
        auto total = 0.0f;
        auto count = 0;
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::array<float, 3> sampled{};
            if (!reflect::SampleReflectionDirection(surface,
                    reflect::SampleSequence(3, 7, 1, index), sampled)) {
                continue;
            }
            total += 1.0f - Dot3(sampled, centre);
            ++count;
        }
        return count > 0 ? total / static_cast<float>(count) : 0.0f;
    };
    CHECK(spread(0.50f) > spread(0.10f));
}

TEST_CASE("P19_two_sided_hits_flip_the_normal_toward_the_ray",
    "[phase19][reflect]")
{
    // A ray that finds the back of a two-sided surface must shade against a
    // normal facing it. Without the flip every dot product goes negative and
    // the reflection resolves to black on exactly the surfaces two-sided
    // rendering exists to support.
    const std::array<float, 3> normal{0.0f, 0.0f, 1.0f};
    const std::array<float, 3> fromBehind{0.0f, 0.0f, 1.0f};
    const std::array<float, 3> fromFront{0.0f, 0.0f, -1.0f};

    const auto flipped = reflect::OrientHitNormal(normal, fromBehind, true);
    CHECK(flipped[2] == Catch::Approx(-1.0f));
    CHECK(Dot3(flipped, fromBehind) < 0.0f);

    const auto kept = reflect::OrientHitNormal(normal, fromFront, true);
    CHECK(kept[2] == Catch::Approx(1.0f));

    // A single-sided surface keeps its normal whatever hit it. Flipping it
    // would make a back-face hit look like a valid front-face one, hiding a
    // ray that leaked through geometry.
    const auto single = reflect::OrientHitNormal(normal, fromBehind, false);
    CHECK(single[2] == Catch::Approx(1.0f));
}

TEST_CASE("P19_rays_leave_the_surface_bounded_and_carry_a_cone",
    "[phase19][reflect]")
{
    const auto surface = FlatSurface();
    reflect::ReflectionPolicy policy{};
    policy.maximumDistance = 512.0f;
    policy.pixelSpreadRadians = 1.0e-3f;

    reflect::ReflectionRay ray{};
    REQUIRE(reflect::BuildReflectionRay(surface, policy, {0.25f, 0.5f}, ray)
        == reflect::ReflectError::None);

    // The origin is offset along the geometric normal by the same rule the
    // shadow ray uses, so a reflection cannot start inside its own surface.
    const auto expected = accel::OffsetRayOrigin(
        surface.position, surface.geometricNormal, 1.0f);
    CHECK(ray.origin == expected);
    CHECK(ray.maximumDistance == Catch::Approx(policy.maximumDistance));
    CHECK(Length3(ray.direction) == Catch::Approx(1.0f).margin(1.0e-4));

    // A rougher surface starts with a wider cone: the lobe it represents is
    // wider, so the texels it should read are coarser from the first hit.
    auto rough = surface;
    rough.roughness = 0.50f;
    reflect::ReflectionRay roughRay{};
    REQUIRE(reflect::BuildReflectionRay(rough, policy, {0.25f, 0.5f},
        roughRay) == reflect::ReflectError::None);
    CHECK(roughRay.cone.spreadAngle > ray.cone.spreadAngle);

    // The pixel footprint contributes even to a mirror, or a minified
    // reflection would always read mip 0 and alias.
    reflect::ReflectionPolicy noSpread{policy};
    noSpread.pixelSpreadRadians = 0.0f;
    auto mirror = surface;
    mirror.roughness = 0.0f;
    reflect::ReflectionRay withPixel{};
    reflect::ReflectionRay withoutPixel{};
    REQUIRE(reflect::BuildReflectionRay(mirror, policy, {0.5f, 0.5f},
        withPixel) == reflect::ReflectError::None);
    REQUIRE(reflect::BuildReflectionRay(mirror, noSpread, {0.5f, 0.5f},
        withoutPixel) == reflect::ReflectError::None);
    CHECK(withPixel.cone.spreadAngle > withoutPixel.cone.spreadAngle);

    // Degenerate inputs are refused, not normalized into something plausible.
    auto broken = surface;
    broken.shadingNormal = {0.0f, 0.0f, 0.0f};
    reflect::ReflectionRay rejected{};
    CHECK(reflect::BuildReflectionRay(broken, policy, {0.5f, 0.5f}, rejected)
        == reflect::ReflectError::DegenerateNormal);

    auto backwards = surface;
    backwards.roughness = -0.1f;
    CHECK(reflect::BuildReflectionRay(backwards, policy, {0.5f, 0.5f},
        rejected) == reflect::ReflectError::InvalidRoughness);
}

TEST_CASE("P19_cone_growth_drives_mip_selection", "[phase19][reflect]")
{
    // Hit shaders have no implicit derivatives, so the footprint has to be
    // carried explicitly. A cone that does not grow selects mip 0 at every
    // distance and aliases the moment a reflection is minified.
    reflect::RayCone cone{};
    cone.width = 0.0f;
    cone.spreadAngle = 0.01f;

    const auto near = reflect::PropagateCone(cone, 10.0f);
    const auto far = reflect::PropagateCone(cone, 100.0f);
    CHECK(near.width == Catch::Approx(0.10f));
    CHECK(far.width == Catch::Approx(1.00f));
    // A straight segment does not change the spread itself.
    CHECK(far.spreadAngle == Catch::Approx(cone.spreadAngle));

    reflect::HitFootprint footprint{};
    footprint.worldArea = 4.0f;
    footprint.uvArea = 1.0f;
    footprint.textureWidth = 1024;
    footprint.textureHeight = 1024;
    footprint.mipCount = 11;

    float nearLevel = 0.0f;
    float farLevel = 0.0f;
    REQUIRE(reflect::SelectMipLevel(near, footprint, nearLevel) ==
        reflect::ReflectError::None);
    REQUIRE(reflect::SelectMipLevel(far, footprint, farLevel) ==
        reflect::ReflectError::None);
    // The level itself, not only how it moves. Checking differences alone
    // would accept an inverted texel-density ratio, which reads mip 0 where
    // it should read the coarsest level and aliases on exactly the dense
    // textures the footprint exists to tame.
    CHECK(nearLevel == Catch::Approx(std::log2(0.10f * 0.5f * 1024.0f)));
    // Ten times the footprint is log2(10) higher in the chain.
    CHECK(farLevel - nearLevel == Catch::Approx(std::log2(10.0f)));
    CHECK(farLevel > nearLevel);

    // Denser texels on the same triangle read a higher level for the same
    // cone, because one cone covers more of them.
    auto dense = footprint;
    dense.textureWidth = 2048;
    dense.textureHeight = 2048;
    float denseLevel = 0.0f;
    REQUIRE(reflect::SelectMipLevel(near, dense, denseLevel) ==
        reflect::ReflectError::None);
    CHECK(denseLevel - nearLevel == Catch::Approx(1.0f));

    // A degenerate triangle has no texel density to speak of and is refused
    // rather than producing an infinite level.
    auto degenerate = footprint;
    degenerate.worldArea = 0.0f;
    float ignored = 0.0f;
    CHECK(reflect::SelectMipLevel(near, degenerate, ignored) ==
        reflect::ReflectError::InvalidFootprint);

    auto noTexture = footprint;
    noTexture.textureWidth = 0;
    CHECK(reflect::SelectMipLevel(near, noTexture, ignored) ==
        reflect::ReflectError::InvalidTexture);
}

TEST_CASE("P19_roughness_cutoff_is_policy_not_material", "[phase19][reflect]")
{
    reflect::ReflectionPolicy policy{};
    policy.roughnessCutoff = 0.40f;

    auto smooth = FlatSurface();
    smooth.roughness = 0.20f;
    auto rough = FlatSurface();
    rough.roughness = 0.60f;

    CHECK(reflect::TracesReflection(smooth, policy));
    CHECK_FALSE(reflect::TracesReflection(rough, policy));

    // The same material traces or does not purely by policy. If the cutoff
    // lived in the material, a quality preset would have to rewrite captured
    // material records to take effect.
    reflect::ReflectionPolicy generous{policy};
    generous.roughnessCutoff = 0.90f;
    CHECK(reflect::TracesReflection(rough, generous));
}

TEST_CASE("P19_missed_rays_resolve_by_captured_environment",
    "[phase19][reflect]")
{
    // Substituting an exterior sky indoors is a light leak, so an interior
    // with no probe resolves to nothing and says so rather than inventing a
    // colour that the comparison would then have to accept.
    lighting::GpuEnvironmentV1 exterior{};
    exterior.flagsAndCount[0] = lighting::EnvironmentPresent;
    lighting::GpuEnvironmentV1 interior{};
    interior.flagsAndCount[0] =
        lighting::EnvironmentPresent | lighting::EnvironmentInterior;
    lighting::GpuEnvironmentV1 absent{};

    CHECK(reflect::ResolveMiss(exterior, false) ==
        reflect::ReflectionSource::Environment);
    CHECK(reflect::ResolveMiss(interior, false) ==
        reflect::ReflectionSource::Unresolved);
    // A captured probe is valid indoors and is preferred there.
    CHECK(reflect::ResolveMiss(interior, true) ==
        reflect::ReflectionSource::Probe);
    CHECK(reflect::ResolveMiss(absent, false) ==
        reflect::ReflectionSource::Unresolved);

    // An unresolved miss contributes nothing, which is visible in the frame
    // rather than hidden inside a plausible grey.
    const std::array<float, 3> probe{0.20f, 0.30f, 0.45f};
    const auto nothing = reflect::EvaluateMissRadiance(
        interior, {0.0f, 0.0f, 1.0f}, probe, false);
    CHECK(nothing[0] == Catch::Approx(0.0f));
    CHECK(nothing[1] == Catch::Approx(0.0f));
    CHECK(nothing[2] == Catch::Approx(0.0f));

    const auto probed = reflect::EvaluateMissRadiance(
        interior, {0.0f, 0.0f, 1.0f}, probe, true);
    CHECK(probed[0] == Catch::Approx(probe[0]));

    // Exterior sky brightens toward the sun and never goes negative under a
    // direction pointing away from it.
    lighting::GpuEnvironmentV1 sky{exterior};
    sky.sunDirectionAndFogFar[0] = 0.0f;
    sky.sunDirectionAndFogFar[1] = 0.0f;
    sky.sunDirectionAndFogFar[2] = -1.0f;
    sky.sunColorAndIntensity[0] = 1.0f;
    sky.sunColorAndIntensity[1] = 0.9f;
    sky.sunColorAndIntensity[2] = 0.8f;
    sky.sunColorAndIntensity[3] = 2.0f;
    sky.ambientAndFogNear[0] = 0.10f;
    sky.ambientAndFogNear[1] = 0.12f;
    sky.ambientAndFogNear[2] = 0.16f;
    const auto toward = reflect::EvaluateMissRadiance(
        sky, {0.0f, 0.0f, 1.0f}, probe, false);
    const auto away = reflect::EvaluateMissRadiance(
        sky, {0.0f, 0.0f, -1.0f}, probe, false);
    CHECK(toward[0] > away[0]);
    CHECK(away[0] >= 0.0f);
}

TEST_CASE("P19_history_survives_only_its_own_epoch", "[phase19][reflect]")
{
    // Reusing a history across a camera cut smears the previous scene across
    // the new one for as long as the history survives; rejecting it costs one
    // frame of convergence.
    reflect::ReflectionHistoryKey key{};
    key.cameraEpoch = 9;
    key.width = 1920;
    key.height = 1080;
    key.viewId = 4;

    CHECK_FALSE(reflect::ResetHistory(key, key));

    auto cut = key;
    cut.cameraEpoch = 10;
    CHECK(reflect::ResetHistory(key, cut));

    auto resized = key;
    resized.width = 1280;
    CHECK(reflect::ResetHistory(key, resized));

    auto otherHeight = key;
    otherHeight.height = 720;
    CHECK(reflect::ResetHistory(key, otherHeight));

    // A different view is a different image, not a moved camera.
    auto otherView = key;
    otherView.viewId = 5;
    CHECK(reflect::ResetHistory(key, otherView));

    // A history that was never established is not reusable.
    CHECK(reflect::ResetHistory({}, key));
}

TEST_CASE("P19_reflections_trace_geometry_and_fall_back_by_policy",
    "[phase19][reflect]")
{
    // A flat surface at the origin looking straight down, with a lit panel
    // directly above it. The mirror direction points at the panel, so a
    // traced reflection must find it and a skipped one must not.
    reflect::ReflectionSurface surface{};
    surface.position = {0.0f, 0.0f, 0.0f};
    surface.geometricNormal = {0.0f, 0.0f, 1.0f};
    surface.shadingNormal = {0.0f, 0.0f, 1.0f};
    surface.viewDirection = {0.0f, 0.0f, 1.0f};
    surface.baseColor = {1.0f, 1.0f, 1.0f};
    surface.roughness = 0.0f;
    surface.metalness = 1.0f;

    reflect::ReflectionTriangle panel{};
    panel.a = {-4.0f, -4.0f, 6.0f};
    panel.b = {4.0f, -4.0f, 6.0f};
    panel.c = {0.0f, 4.0f, 6.0f};
    panel.normal = {0.0f, 0.0f, -1.0f};
    panel.albedo = {0.20f, 0.60f, 0.90f};
    const std::array<reflect::ReflectionTriangle, 1> geometry{panel};

    lighting::GpuEnvironmentV1 environment{};
    environment.flagsAndCount[0] = lighting::EnvironmentPresent;
    environment.flagsAndCount[1] = 1;
    SetVec4(environment.ambientAndFogNear, 0.0f, 0.0f, 0.0f, 1.0e9f);
    SetVec4(environment.sunDirectionAndFogFar, 0.0f, 0.0f, -1.0f, 2.0e9f);
    SetVec4(environment.sunColorAndIntensity, 1.0f, 1.0f, 1.0f, 1.0f);
    SetVec4(environment.fogColorAndPower, 0.0f, 0.0f, 0.0f, 1.0f);
    SetVec4(environment.moonColorAndFogMaximum, 0.0f, 0.0f, 0.0f, 0.0f);

    std::array<lighting::GpuLightRecordV1, 1> lights{};
    lights[0].color[0] = 1.0f;
    lights[0].color[1] = 1.0f;
    lights[0].color[2] = 1.0f;
    lights[0].color[3] =
        static_cast<float>(lighting::LightType::Directional);
    // Travelling up, so the panel's downward normal faces it.
    lights[0].direction[2] = 1.0f;

    reflect::ReflectionPolicy policy{};
    policy.roughnessCutoff = 0.65f;
    policy.maximumDistance = 100.0f;

    const std::array<float, 3> probe{0.05f, 0.05f, 0.05f};
    const auto traced = reflect::EvaluateReflection(surface, policy, geometry,
        lights, environment, {0.5f, 0.5f}, probe, false);
    CHECK(traced.source == reflect::ReflectionSource::Geometry);
    CHECK(traced.hitDistance == Catch::Approx(6.0f).margin(1.0e-2));
    // A metal reflects its own base colour, which is white here, so the
    // reflected panel arrives at close to its own lit brightness.
    CHECK(traced.radiance[2] > traced.radiance[0]);
    CHECK(traced.radiance[2] > 0.0f);

    // The panel is only visible because the ray reaches it. Shortening the
    // ray past it must fall through to the environment, not keep the hit.
    auto shortPolicy = policy;
    shortPolicy.maximumDistance = 3.0f;
    const auto missed = reflect::EvaluateReflection(surface, shortPolicy,
        geometry, lights, environment, {0.5f, 0.5f}, probe, false);
    CHECK(missed.source == reflect::ReflectionSource::Environment);

    // Above the cutoff no ray is traced at all, whatever geometry is there.
    auto rough = surface;
    rough.roughness = 0.90f;
    const auto skipped = reflect::EvaluateReflection(rough, policy, geometry,
        lights, environment, {0.5f, 0.5f}, probe, false);
    CHECK(skipped.source == reflect::ReflectionSource::Skipped);

    // A dielectric reflects far less than a metal at normal incidence. If
    // Fresnel were not applied the two would return the same radiance and the
    // authored F0 would do nothing.
    auto dielectric = surface;
    dielectric.metalness = 0.0f;
    const auto dull = reflect::EvaluateReflection(dielectric, policy,
        geometry, lights, environment, {0.5f, 0.5f}, probe, false);
    CHECK(dull.source == reflect::ReflectionSource::Geometry);
    CHECK(dull.radiance[2] < traced.radiance[2] * 0.25f);

    // A ray that starts on the surface must not re-hit it. The panel is
    // moved to the surface's own plane; a reflection that finds it is
    // reflecting the surface in itself.
    auto coplanar = panel;
    coplanar.a = {-4.0f, -4.0f, 0.0f};
    coplanar.b = {4.0f, -4.0f, 0.0f};
    coplanar.c = {0.0f, 4.0f, 0.0f};
    const std::array<reflect::ReflectionTriangle, 1> selfPlane{coplanar};
    const auto notItself = reflect::EvaluateReflection(surface, policy,
        selfPlane, lights, environment, {0.5f, 0.5f}, probe, false);
    CHECK(notItself.source != reflect::ReflectionSource::Geometry);
}

TEST_CASE("P19_reflections_can_be_switched_off_for_a_frame",
    "[phase19][reflection]")
{
    // The diffuse bounce has had a frame switch since phase 20, and the
    // comment on it argues that every ray-traced term needs one: without it
    // the terms arrive together and none can be measured alone. Reflections
    // did not have one, which is why the mirrored frame's 96 ms could be
    // attributed to the bounce and to nothing else.
    //
    // Exactly nothing rather than a small residue, for the same reason the
    // bounce switch gives nothing: a term that is almost off still moves every
    // pixel it touches, and an isolation built on it measures the remainder.
    reflect::ReflectionSurface surface{};
    surface.position = {0.0f, 0.0f, 0.0f};
    surface.geometricNormal = {0.0f, 0.0f, 1.0f};
    surface.shadingNormal = {0.0f, 0.0f, 1.0f};
    surface.viewDirection = {0.0f, 0.0f, 1.0f};
    surface.baseColor = {1.0f, 1.0f, 1.0f};
    surface.roughness = 0.0f;
    surface.metalness = 1.0f;

    reflect::ReflectionPolicy policy{};
    lighting::GpuEnvironmentV1 environment{};
    environment.flagsAndCount[0] = lighting::EnvironmentPresent;
    environment.ambientAndFogNear[0] = 1.0f;
    environment.ambientAndFogNear[1] = 1.0f;
    environment.ambientAndFogNear[2] = 1.0f;
    environment.fogColorAndPower[3] = 1.0f;
    environment.sunDirectionAndFogFar[3] = 1000.0f;

    const std::array<float, 2> sample{0.5f, 0.5f};
    const std::array<float, 3> probe{2.0f, 2.0f, 2.0f};
    const auto lit = reflect::EvaluateReflection(surface, policy, {}, {},
        environment, sample, probe, true);
    // The frame reflects something, or switching it off proves nothing.
    CHECK((lit.radiance[0] > 0.0f || lit.radiance[1] > 0.0f ||
        lit.radiance[2] > 0.0f));

    environment.flagsAndCount[0] |= lighting::EnvironmentReflectionDisabled;
    const auto dark = reflect::EvaluateReflection(surface, policy, {}, {},
        environment, sample, probe, true);
    CHECK(dark.radiance[0] == 0.0f);
    CHECK(dark.radiance[1] == 0.0f);
    CHECK(dark.radiance[2] == 0.0f);
}

TEST_CASE("P19_a_reflection_reports_which_geometry_it_found",
    "[phase19][reflection]")
{
    // A radiance comparison between an oracle and a hardware intersector
    // cannot distinguish "the two shaded the same hit differently" from "the
    // two hit different things". Every per-hit attribute this phase wants --
    // an interpolated vertex colour, a texture fetch, more than one sample --
    // makes the second case visible, and each one stalled on the same
    // question. Reporting what was hit is what lets those pixels be named and
    // excluded rather than absorbed by a wider bound.
    reflect::ReflectionTriangle near{};
    near.a = {-1.0f, -1.0f, 1.0f};
    near.b = {1.0f, -1.0f, 1.0f};
    near.c = {0.0f, 2.0f, 1.0f};
    near.normal = {0.0f, 0.0f, -1.0f};
    near.twoSided = true;
    near.objectIndex = 7;
    auto far = near;
    far.a[2] = 5.0f;
    far.b[2] = 5.0f;
    far.c[2] = 5.0f;
    far.objectIndex = 9;
    const std::array<reflect::ReflectionTriangle, 2> triangles{far, near};

    reflect::ReflectionRay ray{};
    ray.origin = {0.0f, 0.0f, 0.0f};
    ray.direction = {0.0f, 0.0f, 1.0f};
    ray.maximumDistance = 100.0f;
    const auto hit = reflect::TraceReflection(triangles, ray);
    REQUIRE(hit.hit);
    // The nearer triangle, and its identity travels with the hit rather than
    // being recovered from the distance by the caller.
    CHECK(hit.objectIndex == 7u);

    // A ray that finds nothing reports no geometry, so "found object zero"
    // and "found nothing" cannot be confused.
    reflect::ReflectionRay away{};
    away.origin = {0.0f, 0.0f, 0.0f};
    away.direction = {0.0f, 0.0f, -1.0f};
    away.maximumDistance = 100.0f;
    const auto missed = reflect::TraceReflection(triangles, away);
    CHECK_FALSE(missed.hit);
    CHECK(missed.objectIndex == 0u);
}

TEST_CASE("P19_a_reflection_hit_interpolates_the_vertex_colour",
    "[phase19][reflection]")
{
    // A ray query reports the primitive and its barycentrics, and the shader
    // now reads the three corners the ray actually struck rather than one
    // colour for the whole object. The oracle has to do the same or the two
    // disagree everywhere a surface is not a single flat colour -- which is
    // most of them.
    reflect::ReflectionTriangle triangle{};
    triangle.a = {0.0f, 0.0f, 1.0f};
    triangle.b = {1.0f, 0.0f, 1.0f};
    triangle.c = {0.0f, 1.0f, 1.0f};
    triangle.normal = {0.0f, 0.0f, -1.0f};
    triangle.albedo = {1.0f, 1.0f, 1.0f};
    triangle.twoSided = true;
    triangle.vertexColor = {{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f}}};
    const std::array<reflect::ReflectionTriangle, 1> triangles{triangle};

    // Straight at the first corner: its own colour, undiluted.
    reflect::ReflectionRay atA{};
    atA.origin = {0.01f, 0.01f, 0.0f};
    atA.direction = {0.0f, 0.0f, 1.0f};
    atA.maximumDistance = 10.0f;
    const auto hitA = reflect::TraceReflection(triangles, atA);
    REQUIRE(hitA.hit);
    CHECK(hitA.albedo[0] > 0.9f);
    CHECK(hitA.albedo[2] < 0.1f);

    // And at the third corner, the other end of the same interpolation.
    reflect::ReflectionRay atC{};
    atC.origin = {0.01f, 0.97f, 0.0f};
    atC.direction = {0.0f, 0.0f, 1.0f};
    atC.maximumDistance = 10.0f;
    const auto hitC = reflect::TraceReflection(triangles, atC);
    REQUIRE(hitC.hit);
    CHECK(hitC.albedo[2] > 0.9f);
    CHECK(hitC.albedo[0] < 0.1f);
}
