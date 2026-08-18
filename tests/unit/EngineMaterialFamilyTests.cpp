#include "renderer_core/EngineMaterialFamily.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using namespace vf::renderer;

namespace {

material::FamilyCapture BaseCapture(
    const std::int32_t featureId,
    const std::uint64_t flags = 0)
{
    material::FamilyCapture capture{};
    capture.materialId = 0x1600'0000'0000'0001ull;
    capture.generation = 1;
    capture.featureId = featureId;
    capture.propertyFlags = flags;
    capture.baseTechniqueId = 0x2100;
    return capture;
}

// Every slot authored so a translator's slot decisions are visible rather
// than hidden behind an absent texture.
void AuthorAllSlots(material::FamilyCapture& capture)
{
    for (std::uint32_t slot = 0; slot < material::kShaderTextureSlots;
         ++slot) {
        capture.slots[slot].resourceId = 0x9000ull + slot;
        capture.slots[slot].generation = 1;
        capture.slots[slot].authored = true;
    }
}

[[nodiscard]] material::MaterialSlotRole RoleOf(
    const material::FamilyDescriptor& descriptor,
    const std::uint32_t slot)
{
    return descriptor.slots[slot].role;
}

}

TEST_CASE("P16_every_engine_lighting_feature_id_maps_to_a_declared_family",
    "[phase16][material]")
{
    // The engine's lighting feature IDs are 0..20 with -1 meaning none. They
    // are recorded from the material ABI, so the mapping is a lookup and not
    // a judgement call.
    struct Expectation
    {
        std::int32_t id;
        material::MaterialFamily family;
    };
    const std::array<Expectation, 21> kExpected{{
        {0, material::MaterialFamily::Default},
        {1, material::MaterialFamily::EnvironmentMap},
        {2, material::MaterialFamily::GlowMap},
        {3, material::MaterialFamily::Parallax},
        {4, material::MaterialFamily::Face},
        {5, material::MaterialFamily::SkinTint},
        {6, material::MaterialFamily::HairTint},
        {7, material::MaterialFamily::ParallaxOcclusion},
        {8, material::MaterialFamily::Landscape},
        {9, material::MaterialFamily::LodLandscape},
        {10, material::MaterialFamily::Snow},
        {11, material::MaterialFamily::MultiLayerParallax},
        {12, material::MaterialFamily::TreeAnimation},
        {13, material::MaterialFamily::LodObjects},
        {14, material::MaterialFamily::MultiIndexSnow},
        {15, material::MaterialFamily::LodObjectsHd},
        {16, material::MaterialFamily::Eye},
        {17, material::MaterialFamily::Cloud},
        {18, material::MaterialFamily::LodLandscapeNoise},
        {19, material::MaterialFamily::LodLandscapeBlend},
        {20, material::MaterialFamily::Dismemberment},
    }};
    for (const auto& expectation : kExpected) {
        CHECK(material::ClassifyMaterialFamily(expectation.id) ==
            expectation.family);
    }
    CHECK(material::ClassifyMaterialFamily(-1) ==
        material::MaterialFamily::None);

    // An ID this build has not classified is named as unclassified, never
    // folded into Default. A silent Default would render an unknown family
    // as an ordinary surface and look plausible while being wrong.
    CHECK(material::ClassifyMaterialFamily(21) ==
        material::MaterialFamily::Unknown);
    CHECK(material::ClassifyMaterialFamily(1'000) ==
        material::MaterialFamily::Unknown);
    CHECK(material::ClassifyMaterialFamily(-2) ==
        material::MaterialFamily::Unknown);
}

TEST_CASE("P16_unknown_family_falls_back_explicitly_and_records_provenance",
    "[phase16][material]")
{
    auto capture = BaseCapture(21);
    AuthorAllSlots(capture);
    material::FamilyDescriptor descriptor;
    // A translator must exist for every input. An unknown family resolves to
    // the declared fallback rather than failing the frame, but it must say so.
    REQUIRE(material::TranslateMaterialFamily(capture, descriptor) ==
        material::FamilyError::None);
    CHECK(descriptor.family == material::MaterialFamily::Unknown);
    CHECK(descriptor.usedFallback);
    CHECK(descriptor.provenance ==
        material::MaterialProvenance::CanonicalFallback);
    CHECK(descriptor.diagnostic.capturedFeatureId == 21);
    // The fallback is the ordinary lit surface: base colour and normal only.
    CHECK(RoleOf(descriptor, 0) == material::MaterialSlotRole::BaseColor);
    CHECK(RoleOf(descriptor, 1) == material::MaterialSlotRole::Normal);
    for (std::uint32_t slot = 2; slot < material::kShaderTextureSlots;
         ++slot) {
        CHECK(RoleOf(descriptor, slot) ==
            material::MaterialSlotRole::Unused);
    }
    // A fallback must not invent a specialized lobe.
    CHECK_FALSE(descriptor.features.subsurface);
    CHECK_FALSE(descriptor.features.anisotropy);
    CHECK_FALSE(descriptor.features.parallaxOcclusion);
    CHECK(descriptor.emission.enabled == false);

    auto known = BaseCapture(0);
    AuthorAllSlots(known);
    material::FamilyDescriptor defaulted;
    REQUIRE(material::TranslateMaterialFamily(known, defaulted) ==
        material::FamilyError::None);
    CHECK(defaulted.family == material::MaterialFamily::Default);
    CHECK_FALSE(defaulted.usedFallback);
}

TEST_CASE("P16_texture_slot_seven_is_overloaded_by_family_not_by_guess",
    "[phase16][material]")
{
    // Role 7 is recorded as backlight-mask *and* smooth-spec. Which one it
    // is depends on the family, and reading it as the wrong one silently
    // turns a smoothness map into a rim-light mask.
    const std::array<material::MaterialFamily, 3> kBacklit{{
        material::MaterialFamily::Face,
        material::MaterialFamily::SkinTint,
        material::MaterialFamily::HairTint,
    }};
    for (const auto family : kBacklit) {
        auto capture = BaseCapture(material::FeatureIdOf(family));
        AuthorAllSlots(capture);
        material::FamilyDescriptor descriptor;
        REQUIRE(material::TranslateMaterialFamily(capture, descriptor) ==
            material::FamilyError::None);
        CHECK(RoleOf(descriptor, 7) ==
            material::MaterialSlotRole::BacklightMask);
    }

    auto ordinary = BaseCapture(material::FeatureIdOf(
        material::MaterialFamily::Default));
    AuthorAllSlots(ordinary);
    material::FamilyDescriptor descriptor;
    REQUIRE(material::TranslateMaterialFamily(ordinary, descriptor) ==
        material::FamilyError::None);
    CHECK(RoleOf(descriptor, 7) == material::MaterialSlotRole::SmoothSpec);
}

TEST_CASE("P16_height_and_environment_slots_belong_only_to_their_families",
    "[phase16][material]")
{
    // Slot 3 is height. A family that does not displace must not consume it,
    // or an authored height map silently becomes an input to a surface that
    // has no parallax.
    const std::array<material::MaterialFamily, 3> kDisplaced{{
        material::MaterialFamily::Parallax,
        material::MaterialFamily::ParallaxOcclusion,
        material::MaterialFamily::MultiLayerParallax,
    }};
    for (const auto family : kDisplaced) {
        auto capture = BaseCapture(material::FeatureIdOf(family));
        AuthorAllSlots(capture);
        material::FamilyDescriptor descriptor;
        REQUIRE(material::TranslateMaterialFamily(capture, descriptor) ==
            material::FamilyError::None);
        CHECK(RoleOf(descriptor, 3) == material::MaterialSlotRole::Height);
    }

    auto flat = BaseCapture(material::FeatureIdOf(
        material::MaterialFamily::GlowMap));
    AuthorAllSlots(flat);
    material::FamilyDescriptor glow;
    REQUIRE(material::TranslateMaterialFamily(flat, glow) ==
        material::FamilyError::None);
    CHECK(RoleOf(glow, 3) == material::MaterialSlotRole::Unused);
    CHECK(RoleOf(glow, 2) == material::MaterialSlotRole::GlowMap);

    // Slot 4 is the environment map, and it is claimed by the environment
    // family and by anything carrying the environment-map property flag.
    auto envFamily = BaseCapture(material::FeatureIdOf(
        material::MaterialFamily::EnvironmentMap));
    AuthorAllSlots(envFamily);
    material::FamilyDescriptor environment;
    REQUIRE(material::TranslateMaterialFamily(envFamily, environment) ==
        material::FamilyError::None);
    CHECK(RoleOf(environment, 4) ==
        material::MaterialSlotRole::Environment);

    auto envFlagged = BaseCapture(
        material::FeatureIdOf(material::MaterialFamily::Default),
        material::PropertyFlag::EnvironmentMap);
    AuthorAllSlots(envFlagged);
    material::FamilyDescriptor flagged;
    REQUIRE(material::TranslateMaterialFamily(envFlagged, flagged) ==
        material::FamilyError::None);
    CHECK(RoleOf(flagged, 4) == material::MaterialSlotRole::Environment);

    auto plain = BaseCapture(material::FeatureIdOf(
        material::MaterialFamily::Default));
    AuthorAllSlots(plain);
    material::FamilyDescriptor unflagged;
    REQUIRE(material::TranslateMaterialFamily(plain, unflagged) ==
        material::FamilyError::None);
    CHECK(RoleOf(unflagged, 4) == material::MaterialSlotRole::Unused);
}

TEST_CASE("P16_bright_base_colour_never_becomes_emission_without_the_flag",
    "[phase16][material]")
{
    // The rule this phase exists to enforce. A saturated albedo is common in
    // authored content; treating it as emission makes ordinary surfaces glow.
    auto capture = BaseCapture(material::FeatureIdOf(
        material::MaterialFamily::Default));
    AuthorAllSlots(capture);
    capture.emitColor = {8.0f, 7.5f, 7.0f};
    capture.emitScale = 12.0f;
    material::FamilyDescriptor descriptor;
    REQUIRE(material::TranslateMaterialFamily(capture, descriptor) ==
        material::FamilyError::None);
    CHECK_FALSE(descriptor.emission.enabled);
    CHECK(descriptor.emission.color[0] == Catch::Approx(0.0f));
    CHECK(descriptor.emission.color[1] == Catch::Approx(0.0f));
    CHECK(descriptor.emission.color[2] == Catch::Approx(0.0f));

    // Own emit is the flag that authorizes it.
    auto emissive = capture;
    emissive.propertyFlags = material::PropertyFlag::OwnEmit;
    material::FamilyDescriptor lit;
    REQUIRE(material::TranslateMaterialFamily(emissive, lit) ==
        material::FamilyError::None);
    CHECK(lit.emission.enabled);
    CHECK(lit.emission.color[0] == Catch::Approx(8.0f * 12.0f));

    // So is a glow map, which additionally claims slot 2.
    auto glowMapped = capture;
    glowMapped.propertyFlags = material::PropertyFlag::GlowMap;
    material::FamilyDescriptor glowing;
    REQUIRE(material::TranslateMaterialFamily(glowMapped, glowing) ==
        material::FamilyError::None);
    CHECK(glowing.emission.enabled);
    CHECK(glowing.emission.usesGlowMap);
    CHECK(RoleOf(glowing, 2) == material::MaterialSlotRole::GlowMap);

    // External emittance is driven by the reference, not the material, so it
    // enables emission without asserting a material colour.
    auto external = capture;
    external.propertyFlags = material::PropertyFlag::ExternalEmittance;
    material::FamilyDescriptor driven;
    REQUIRE(material::TranslateMaterialFamily(external, driven) ==
        material::FamilyError::None);
    CHECK(driven.emission.enabled);
    CHECK(driven.emission.externallyDriven);

    // The glow-map *family* emits without needing the property flag; the
    // family is itself the declaration.
    auto glowFamily = BaseCapture(material::FeatureIdOf(
        material::MaterialFamily::GlowMap));
    AuthorAllSlots(glowFamily);
    glowFamily.emitColor = {1.0f, 1.0f, 1.0f};
    glowFamily.emitScale = 1.0f;
    material::FamilyDescriptor familyGlow;
    REQUIRE(material::TranslateMaterialFamily(glowFamily, familyGlow) ==
        material::FamilyError::None);
    CHECK(familyGlow.emission.enabled);
    CHECK(familyGlow.emission.usesGlowMap);
}

TEST_CASE("P16_skin_and_face_resolve_subsurface_rim_and_backlight",
    "[phase16][material]")
{
    auto capture = BaseCapture(
        material::FeatureIdOf(material::MaterialFamily::SkinTint),
        material::PropertyFlag::Face);
    AuthorAllSlots(capture);
    capture.subsurfaceRolloff = 0.35f;
    capture.rimPower = 2.5f;
    capture.backlightPower = 1.75f;
    capture.tintColor = {0.9f, 0.7f, 0.6f};
    material::FamilyDescriptor descriptor;
    REQUIRE(material::TranslateMaterialFamily(capture, descriptor) ==
        material::FamilyError::None);
    CHECK(descriptor.features.subsurface);
    CHECK(descriptor.features.rim);
    CHECK(descriptor.features.backlight);
    CHECK(descriptor.subsurface.rolloff == Catch::Approx(0.35f));
    CHECK(descriptor.subsurface.rimPower == Catch::Approx(2.5f));
    CHECK(descriptor.subsurface.backlightPower == Catch::Approx(1.75f));
    // Skin tint is a tint, so it must reach the descriptor as one.
    CHECK(descriptor.tint.enabled);
    CHECK(descriptor.tint.color[0] == Catch::Approx(0.9f));

    // The face family additionally reads the wrinkle slot.
    auto face = BaseCapture(
        material::FeatureIdOf(material::MaterialFamily::Face));
    AuthorAllSlots(face);
    material::FamilyDescriptor faceDescriptor;
    REQUIRE(material::TranslateMaterialFamily(face, faceDescriptor) ==
        material::FamilyError::None);
    CHECK(RoleOf(faceDescriptor, 5) == material::MaterialSlotRole::Wrinkles);
    CHECK(faceDescriptor.features.subsurface);

    // An ordinary surface gets none of it, whatever its scalars happen to
    // hold. Captured scalars are not a licence to enable a lobe.
    auto ordinary = BaseCapture(
        material::FeatureIdOf(material::MaterialFamily::Default));
    AuthorAllSlots(ordinary);
    ordinary.subsurfaceRolloff = 0.9f;
    ordinary.rimPower = 4.0f;
    ordinary.backlightPower = 3.0f;
    material::FamilyDescriptor plain;
    REQUIRE(material::TranslateMaterialFamily(ordinary, plain) ==
        material::FamilyError::None);
    CHECK_FALSE(plain.features.subsurface);
    CHECK_FALSE(plain.features.rim);
    CHECK_FALSE(plain.features.backlight);
    CHECK(plain.subsurface.rolloff == Catch::Approx(0.0f));
}

TEST_CASE("P16_hair_tint_uses_palette_and_anisotropy_only_when_declared",
    "[phase16][material]")
{
    auto capture = BaseCapture(
        material::FeatureIdOf(material::MaterialFamily::HairTint),
        material::PropertyFlag::HairTint |
            material::PropertyFlag::AnisotropicLighting |
            material::PropertyFlag::GreyscaleToPaletteColor);
    AuthorAllSlots(capture);
    capture.tintColor = {0.25f, 0.15f, 0.10f};
    material::FamilyDescriptor descriptor;
    REQUIRE(material::TranslateMaterialFamily(capture, descriptor) ==
        material::FamilyError::None);
    CHECK(descriptor.features.anisotropy);
    CHECK(descriptor.tint.enabled);
    CHECK(descriptor.palette.colorFromGreyscale);
    CHECK_FALSE(descriptor.palette.alphaFromGreyscale);

    // Greyscale-to-palette alpha is a separate flag and must not be implied
    // by the colour one; conflating them rewrites coverage.
    auto both = capture;
    both.propertyFlags |= material::PropertyFlag::GreyscaleToPaletteAlpha;
    material::FamilyDescriptor paletteAlpha;
    REQUIRE(material::TranslateMaterialFamily(both, paletteAlpha) ==
        material::FamilyError::None);
    CHECK(paletteAlpha.palette.alphaFromGreyscale);

    // Anisotropy is bit 53 and nothing else. Hair without the flag is
    // isotropic, because the engine's own lobe selection says so.
    auto isotropic = BaseCapture(
        material::FeatureIdOf(material::MaterialFamily::HairTint),
        material::PropertyFlag::HairTint);
    AuthorAllSlots(isotropic);
    material::FamilyDescriptor noAniso;
    REQUIRE(material::TranslateMaterialFamily(isotropic, noAniso) ==
        material::FamilyError::None);
    CHECK_FALSE(noAniso.features.anisotropy);
    CHECK(noAniso.tint.enabled);
}

TEST_CASE("P16_eye_family_resolves_its_transform_and_reflection",
    "[phase16][material]")
{
    auto capture = BaseCapture(
        material::FeatureIdOf(material::MaterialFamily::Eye),
        material::PropertyFlag::EyeReflection);
    AuthorAllSlots(capture);
    capture.eyeCenter = {0.0f, 0.0f, 0.55f};
    capture.eyeRadius = 0.42f;
    capture.eyeIrisScale = 1.25f;
    material::FamilyDescriptor descriptor;
    REQUIRE(material::TranslateMaterialFamily(capture, descriptor) ==
        material::FamilyError::None);
    CHECK(descriptor.features.eye);
    CHECK(descriptor.eye.radius == Catch::Approx(0.42f));
    CHECK(descriptor.eye.irisScale == Catch::Approx(1.25f));
    CHECK(descriptor.eye.center[2] == Catch::Approx(0.55f));
    CHECK(descriptor.eye.reflects);
    CHECK(RoleOf(descriptor, 4) == material::MaterialSlotRole::Environment);

    // A zero or negative radius is not a usable eye transform and cannot be
    // silently normalized into one.
    auto degenerate = capture;
    degenerate.eyeRadius = 0.0f;
    material::FamilyDescriptor rejected;
    CHECK(material::TranslateMaterialFamily(degenerate, rejected) ==
        material::FamilyError::InvalidEyeTransform);

    auto negative = capture;
    negative.eyeIrisScale = -1.0f;
    material::FamilyDescriptor alsoRejected;
    CHECK(material::TranslateMaterialFamily(negative, alsoRejected) ==
        material::FamilyError::InvalidEyeTransform);
}

TEST_CASE("P16_parallax_occlusion_carries_scale_bias_and_uv_without_guessing",
    "[phase16][material]")
{
    auto capture = BaseCapture(material::FeatureIdOf(
        material::MaterialFamily::ParallaxOcclusion));
    AuthorAllSlots(capture);
    capture.parallaxScale = 0.045f;
    capture.parallaxBias = -0.5f;
    capture.parallaxUvScale = {2.0f, 3.0f};
    capture.parallaxMinSteps = 8;
    capture.parallaxMaxSteps = 32;
    material::FamilyDescriptor descriptor;
    REQUIRE(material::TranslateMaterialFamily(capture, descriptor) ==
        material::FamilyError::None);
    CHECK(descriptor.features.parallaxOcclusion);
    CHECK(descriptor.parallax.scale == Catch::Approx(0.045f));
    CHECK(descriptor.parallax.bias == Catch::Approx(-0.5f));
    CHECK(descriptor.parallax.uvScale[0] == Catch::Approx(2.0f));
    CHECK(descriptor.parallax.uvScale[1] == Catch::Approx(3.0f));
    CHECK(descriptor.parallax.minimumSteps == 8);
    CHECK(descriptor.parallax.maximumSteps == 32);

    // Plain parallax offsets but does not march, so it must not claim to.
    auto offsetOnly = BaseCapture(
        material::FeatureIdOf(material::MaterialFamily::Parallax));
    AuthorAllSlots(offsetOnly);
    offsetOnly.parallaxScale = 0.02f;
    material::FamilyDescriptor offset;
    REQUIRE(material::TranslateMaterialFamily(offsetOnly, offset) ==
        material::FamilyError::None);
    CHECK(offset.features.parallaxOffset);
    CHECK_FALSE(offset.features.parallaxOcclusion);
    CHECK(offset.parallax.scale == Catch::Approx(0.02f));

    // An inverted or empty step range is a captured contradiction.
    auto inverted = capture;
    inverted.parallaxMinSteps = 40;
    inverted.parallaxMaxSteps = 8;
    material::FamilyDescriptor rejected;
    CHECK(material::TranslateMaterialFamily(inverted, rejected) ==
        material::FamilyError::InvalidParallaxRange);

    auto zeroed = capture;
    zeroed.parallaxMinSteps = 0;
    material::FamilyDescriptor alsoRejected;
    CHECK(material::TranslateMaterialFamily(zeroed, alsoRejected) ==
        material::FamilyError::InvalidParallaxRange);
}

TEST_CASE("P16_multilayer_parallax_binds_its_inner_layer_slot",
    "[phase16][material]")
{
    auto capture = BaseCapture(
        material::FeatureIdOf(material::MaterialFamily::MultiLayerParallax),
        material::PropertyFlag::MultiLayerParallax);
    AuthorAllSlots(capture);
    capture.parallaxScale = 0.03f;
    capture.parallaxMinSteps = 4;
    capture.parallaxMaxSteps = 16;
    capture.layerThickness = 0.6f;
    capture.layerRefraction = 1.33f;
    material::FamilyDescriptor descriptor;
    REQUIRE(material::TranslateMaterialFamily(capture, descriptor) ==
        material::FamilyError::None);
    CHECK(descriptor.features.multiLayer);
    CHECK(RoleOf(descriptor, 6) == material::MaterialSlotRole::MultiLayer);
    CHECK(RoleOf(descriptor, 3) == material::MaterialSlotRole::Height);
    CHECK(descriptor.layer.thickness == Catch::Approx(0.6f));
    CHECK(descriptor.layer.refraction == Catch::Approx(1.33f));

    // A refraction index below one is not physical and is not clamped into
    // plausibility.
    auto invalid = capture;
    invalid.layerRefraction = 0.4f;
    material::FamilyDescriptor rejected;
    CHECK(material::TranslateMaterialFamily(invalid, rejected) ==
        material::FamilyError::InvalidLayer);
}

TEST_CASE("P16_snow_and_wetness_are_dynamic_and_never_churn_descriptors",
    "[phase16][material]")
{
    auto capture = BaseCapture(
        material::FeatureIdOf(material::MaterialFamily::Snow));
    AuthorAllSlots(capture);
    capture.wetness = {0.25f, 0.5f, 0.75f, 1.0f, 0.125f, 0.375f};
    capture.staticRevision = 7;
    capture.revision = 11;
    material::FamilyDescriptor descriptor;
    REQUIRE(material::TranslateMaterialFamily(capture, descriptor) ==
        material::FamilyError::None);
    CHECK(descriptor.features.snow);
    CHECK(descriptor.features.wetness);
    for (std::size_t index = 0; index < descriptor.wetness.controls.size();
         ++index) {
        CHECK(descriptor.wetness.controls[index] ==
            Catch::Approx(capture.wetness[index]));
    }

    // A wetness change moves the dynamic revision only. If it moved the
    // static one, every rain transition would rebuild descriptor sets.
    auto wetter = capture;
    wetter.wetness[0] = 0.9f;
    wetter.revision = 12;
    material::FamilyDescriptor updated;
    REQUIRE(material::TranslateMaterialFamily(wetter, updated) ==
        material::FamilyError::None);
    CHECK(material::RequiresDescriptorRebuild(descriptor, updated) == false);
    CHECK(material::RequiresDynamicUpdate(descriptor, updated));

    // Rebinding a texture is a static change and must rebuild.
    auto rebound = capture;
    rebound.slots[0].resourceId = 0xABCDull;
    rebound.staticRevision = 8;
    material::FamilyDescriptor rebuilt;
    REQUIRE(material::TranslateMaterialFamily(rebound, rebuilt) ==
        material::FamilyError::None);
    CHECK(material::RequiresDescriptorRebuild(descriptor, rebuilt));
}

TEST_CASE("P16_model_space_normals_are_declared_not_inferred",
    "[phase16][material]")
{
    auto tangent = BaseCapture(
        material::FeatureIdOf(material::MaterialFamily::Default));
    AuthorAllSlots(tangent);
    material::FamilyDescriptor descriptor;
    REQUIRE(material::TranslateMaterialFamily(tangent, descriptor) ==
        material::FamilyError::None);
    CHECK(descriptor.normalEncoding ==
        material::MaterialNormalEncoding::TangentSpaceBc5);

    auto modelSpace = BaseCapture(
        material::FeatureIdOf(material::MaterialFamily::Default),
        material::PropertyFlag::ModelSpaceNormals);
    AuthorAllSlots(modelSpace);
    material::FamilyDescriptor model;
    REQUIRE(material::TranslateMaterialFamily(modelSpace, model) ==
        material::FamilyError::None);
    CHECK(model.normalEncoding ==
        material::MaterialNormalEncoding::ModelSpaceRgb);
}

TEST_CASE("P16_lod_families_resolve_without_claiming_full_detail_features",
    "[phase16][material]")
{
    // LOD families must not enable parallax, subsurface, or anisotropy. A
    // distant object paying for a specialized lobe is both wrong and slow.
    const std::array<material::MaterialFamily, 5> kLod{{
        material::MaterialFamily::LodLandscape,
        material::MaterialFamily::LodObjects,
        material::MaterialFamily::LodObjectsHd,
        material::MaterialFamily::LodLandscapeNoise,
        material::MaterialFamily::LodLandscapeBlend,
    }};
    for (const auto family : kLod) {
        auto capture = BaseCapture(material::FeatureIdOf(family));
        AuthorAllSlots(capture);
        capture.parallaxScale = 0.5f;
        capture.subsurfaceRolloff = 0.5f;
        material::FamilyDescriptor descriptor;
        REQUIRE(material::TranslateMaterialFamily(capture, descriptor) ==
            material::FamilyError::None);
        CHECK(descriptor.family == family);
        CHECK_FALSE(descriptor.usedFallback);
        CHECK_FALSE(descriptor.features.parallaxOffset);
        CHECK_FALSE(descriptor.features.parallaxOcclusion);
        CHECK_FALSE(descriptor.features.subsurface);
        CHECK_FALSE(descriptor.features.anisotropy);
        CHECK(descriptor.features.reducedDetail);
    }

    // A cloud is not a reduced-detail surface. It is an ordinary textured
    // one whose sky behaviour belongs to the lighting phase, so it carries a
    // feature bit rather than a pipeline class of its own.
    auto cloud = BaseCapture(
        material::FeatureIdOf(material::MaterialFamily::Cloud));
    AuthorAllSlots(cloud);
    material::FamilyDescriptor sky;
    REQUIRE(material::TranslateMaterialFamily(cloud, sky) ==
        material::FamilyError::None);
    CHECK(sky.features.sky);
    CHECK_FALSE(sky.features.reducedDetail);
    CHECK(sky.shaderClass == material::ShaderClass::Standard);
}

TEST_CASE("P16_dismemberment_carries_its_meat_cuff_without_altering_lighting",
    "[phase16][material]")
{
    auto capture = BaseCapture(
        material::FeatureIdOf(material::MaterialFamily::Dismemberment),
        material::PropertyFlag::Dismemberment |
            material::PropertyFlag::DismembermentMeatCuff);
    AuthorAllSlots(capture);
    material::FamilyDescriptor descriptor;
    REQUIRE(material::TranslateMaterialFamily(capture, descriptor) ==
        material::FamilyError::None);
    CHECK(descriptor.features.dismemberment);
    CHECK(descriptor.features.meatCuff);
    // It is still an ordinary lit surface underneath.
    CHECK(RoleOf(descriptor, 0) == material::MaterialSlotRole::BaseColor);
    CHECK(RoleOf(descriptor, 1) == material::MaterialSlotRole::Normal);
    CHECK_FALSE(descriptor.features.subsurface);
}

TEST_CASE("P16_translation_refuses_non_finite_and_unauthored_required_slots",
    "[phase16][material]")
{
    auto capture = BaseCapture(material::FeatureIdOf(
        material::MaterialFamily::ParallaxOcclusion));
    AuthorAllSlots(capture);
    capture.parallaxMinSteps = 4;
    capture.parallaxMaxSteps = 16;

    auto nonFinite = capture;
    nonFinite.parallaxScale =
        std::numeric_limits<float>::quiet_NaN();
    material::FamilyDescriptor rejected;
    CHECK(material::TranslateMaterialFamily(nonFinite, rejected) ==
        material::FamilyError::NonFiniteSource);

    // A family that requires a slot cannot render without it. Silently
    // dropping to a flat surface would hide a broken texture set.
    auto missingHeight = capture;
    missingHeight.slots[3].authored = false;
    missingHeight.slots[3].resourceId = 0;
    material::FamilyDescriptor missing;
    CHECK(material::TranslateMaterialFamily(missingHeight, missing) ==
        material::FamilyError::MissingRequiredSlot);

    // Base colour is required by every family.
    auto missingBase = BaseCapture(material::FeatureIdOf(
        material::MaterialFamily::Default));
    AuthorAllSlots(missingBase);
    missingBase.slots[0].authored = false;
    missingBase.slots[0].resourceId = 0;
    material::FamilyDescriptor noBase;
    CHECK(material::TranslateMaterialFamily(missingBase, noBase) ==
        material::FamilyError::MissingRequiredSlot);
}

TEST_CASE("P16_family_packet_is_pointer_free_deterministic_and_checksummed",
    "[phase16][material]")
{
    material::FamilyPacket packet;
    packet.header.frameId = 0x1600'0000'0000'0007ull;
    packet.header.viewId = 0x1600'0000'0000'0009ull;
    for (std::int32_t id = 0; id < 3; ++id) {
        auto capture = BaseCapture(id);
        capture.materialId = 0x1600'0000'0000'0001ull +
            static_cast<std::uint64_t>(id);
        AuthorAllSlots(capture);
        material::FamilyDescriptor descriptor;
        REQUIRE(material::TranslateMaterialFamily(capture, descriptor) ==
            material::FamilyError::None);
        packet.records.push_back(material::MakeFamilyRecord(
            descriptor, 0x5000ull + static_cast<std::uint64_t>(id)));
    }
    REQUIRE(material::ValidateFamilyPacket(packet) ==
        material::FamilyPacketError::None);

    std::vector<std::byte> bytes;
    REQUIRE(material::EncodeFamilyPacket(packet, bytes) ==
        material::FamilyPacketError::None);
    CHECK(bytes.size() == sizeof(material::FamilyPacketHeaderV1) +
        packet.records.size() * sizeof(material::FamilyRecordV1));

    material::FamilyPacket decoded;
    REQUIRE(material::DecodeFamilyPacket(bytes, decoded) ==
        material::FamilyPacketError::None);
    CHECK(decoded.records.size() == packet.records.size());
    CHECK(decoded.header.frameId == packet.header.frameId);
    CHECK(decoded.records[2].family ==
        static_cast<std::uint8_t>(material::MaterialFamily::GlowMap));

    // Re-encoding a decoded packet must reproduce the same bytes, or a
    // captured artifact could not be compared against a replayed one.
    std::vector<std::byte> reEncoded;
    REQUIRE(material::EncodeFamilyPacket(decoded, reEncoded) ==
        material::FamilyPacketError::None);
    CHECK(reEncoded == bytes);

    // A flipped payload byte must not decode. Without the checksum a
    // corrupted family table would render as a plausible wrong surface.
    auto corrupted = bytes;
    corrupted[sizeof(material::FamilyPacketHeaderV1) + 4] ^= std::byte{0x40};
    material::FamilyPacket rejected;
    CHECK(material::DecodeFamilyPacket(corrupted, rejected) ==
        material::FamilyPacketError::ChecksumMismatch);

    auto truncated = bytes;
    truncated.resize(bytes.size() - 1);
    CHECK(material::DecodeFamilyPacket(truncated, rejected) ==
        material::FamilyPacketError::SizeMismatch);

    auto badMagic = bytes;
    badMagic[0] ^= std::byte{0xFF};
    CHECK(material::DecodeFamilyPacket(badMagic, rejected) ==
        material::FamilyPacketError::BadMagic);

    // Two records claiming the same object cannot both own it.
    auto duplicated = packet;
    duplicated.records[1].objectId = duplicated.records[0].objectId;
    CHECK(material::ValidateFamilyPacket(duplicated) ==
        material::FamilyPacketError::DuplicateRecord);

    // An unclassified shader class would leave the backend without a
    // pipeline to select, so it is refused rather than defaulted.
    auto unclassified = packet;
    unclassified.records[0].shaderClass =
        static_cast<std::uint8_t>(material::ShaderClass::Unknown);
    CHECK(material::ValidateFamilyPacket(unclassified) ==
        material::FamilyPacketError::UnclassifiedRecord);

    auto nonFinite = packet;
    nonFinite.records[0].emissionColor[1] =
        std::numeric_limits<float>::infinity();
    CHECK(material::ValidateFamilyPacket(nonFinite) ==
        material::FamilyPacketError::NonFiniteValue);

    // The record is the wire format, so its layout is pinned. 384 since the
    // base colour was added: a ray-query hit has no vertex attributes and
    // shades from this record, and the tint it used to read is zero unless a
    // tint is declared.
    CHECK(sizeof(material::FamilyRecordV1) == 384);
    CHECK(sizeof(material::FamilySlotV1) == 16);
    CHECK(sizeof(material::FamilyPacketHeaderV1) == 64);
}

TEST_CASE("P16_gpu_family_record_packs_every_declaration_losslessly",
    "[phase16][material]")
{
    auto capture = BaseCapture(
        material::FeatureIdOf(material::MaterialFamily::HairTint),
        material::PropertyFlag::HairTint |
            material::PropertyFlag::AnisotropicLighting |
            material::PropertyFlag::GreyscaleToPaletteColor |
            material::PropertyFlag::GreyscaleToPaletteAlpha |
            material::PropertyFlag::OwnEmit |
            material::PropertyFlag::ModelSpaceNormals);
    AuthorAllSlots(capture);
    capture.tintColor = {0.25f, 0.15f, 0.10f};
    capture.emitColor = {0.5f, 0.25f, 0.125f};
    capture.emitScale = 2.0f;
    capture.wetness = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    material::FamilyDescriptor descriptor;
    REQUIRE(material::TranslateMaterialFamily(capture, descriptor) ==
        material::FamilyError::None);

    const auto record = material::BuildFamilyGpuRecord(descriptor);
    CHECK((record.familyPacked & 0xFFu) ==
        static_cast<std::uint32_t>(material::MaterialFamily::HairTint));
    CHECK(((record.familyPacked >> 8) & 0xFFu) ==
        static_cast<std::uint32_t>(material::ShaderClass::Hair));
    CHECK(((record.familyPacked >> 16) & 0xFFu) ==
        static_cast<std::uint32_t>(
            material::MaterialNormalEncoding::ModelSpaceRgb));

    CHECK((record.featureFlags & material::GpuFeatureAnisotropy) != 0);
    CHECK((record.featureFlags & material::GpuFeatureSubsurface) == 0);
    CHECK((record.emissionFlags & material::GpuEmissionEnabled) != 0);
    CHECK((record.emissionFlags & material::GpuEmissionGlowMap) == 0);
    CHECK((record.paletteFlags & material::GpuPaletteColor) != 0);
    CHECK((record.paletteFlags & material::GpuPaletteAlpha) != 0);

    CHECK(record.emissionColor[0] == Catch::Approx(1.0f));
    CHECK(record.emissionColor[2] == Catch::Approx(0.25f));
    CHECK(record.tintColor[0] == Catch::Approx(0.25f));
    // The tint enable rides in w, so a zero tint colour and an absent tint
    // stay distinguishable.
    CHECK(record.tintColor[3] == Catch::Approx(1.0f));
    for (std::size_t index = 0; index < 4; ++index) {
        CHECK(record.wetnessLow[index] ==
            Catch::Approx(capture.wetness[index]));
    }
    CHECK(record.wetnessHigh[0] == Catch::Approx(0.5f));
    CHECK(record.wetnessHigh[1] == Catch::Approx(0.6f));

    auto untinted = BaseCapture(
        material::FeatureIdOf(material::MaterialFamily::Default));
    AuthorAllSlots(untinted);
    untinted.tintColor = {0.0f, 0.0f, 0.0f};
    material::FamilyDescriptor plain;
    REQUIRE(material::TranslateMaterialFamily(untinted, plain) ==
        material::FamilyError::None);
    const auto plainRecord = material::BuildFamilyGpuRecord(plain);
    CHECK(plainRecord.tintColor[3] == Catch::Approx(0.0f));

    // The parallax step range is integral and has no float home of its own,
    // so it must survive in the record rather than being dropped.
    auto marched = BaseCapture(material::FeatureIdOf(
        material::MaterialFamily::ParallaxOcclusion));
    AuthorAllSlots(marched);
    marched.parallaxMinSteps = 6;
    marched.parallaxMaxSteps = 24;
    marched.parallaxScale = 0.05f;
    material::FamilyDescriptor pom;
    REQUIRE(material::TranslateMaterialFamily(marched, pom) ==
        material::FamilyError::None);
    const auto pomRecord = material::BuildFamilyGpuRecord(pom);
    CHECK(pomRecord.parallax[0] == Catch::Approx(0.05f));
    CHECK(pomRecord.wetnessHigh[2] == Catch::Approx(6.0f));
    CHECK(pomRecord.wetnessHigh[3] == Catch::Approx(24.0f));
}

TEST_CASE("P16_every_family_reports_a_broad_shader_class_for_pipeline_keys",
    "[phase16][material]")
{
    // The refactor requirement: technique permutations collapse into a small
    // set of broad classes with the rest carried as feature data. If every
    // family had its own class the pipeline count would grow with content.
    std::array<std::uint32_t, 21> featureIds{};
    for (std::int32_t id = 0; id <= 20; ++id) {
        auto capture = BaseCapture(id);
        AuthorAllSlots(capture);
        material::FamilyDescriptor descriptor;
        REQUIRE(material::TranslateMaterialFamily(capture, descriptor) ==
            material::FamilyError::None);
        CHECK(descriptor.shaderClass != material::ShaderClass::Unknown);
        featureIds[static_cast<std::size_t>(id)] =
            static_cast<std::uint32_t>(descriptor.shaderClass);
    }
    std::sort(featureIds.begin(), featureIds.end());
    const auto distinct = static_cast<std::size_t>(
        std::unique(featureIds.begin(), featureIds.end()) -
        featureIds.begin());
    // Twenty-one families, far fewer pipeline classes.
    CHECK(distinct <= 8);
    CHECK(distinct >= 2);
}

TEST_CASE("P19_family_record_carries_a_base_colour_for_untinted_materials",
    "[phase19][family]")
{
    // `reflection.glsl` shades a ray-query hit from the per-object family
    // record, because a ray query has no vertex attributes bound and so has
    // no way to sample the surface's texture. It read `tintColor` -- but
    // `TranslateMaterialFamily` fills the tint only when one is *declared*,
    // by a tinting family or an explicit tint flag, and the enable rides in
    // `tintColor.w` precisely so that an absent tint is distinguishable from
    // a black one.
    //
    // For every ordinary material the tint is therefore zero by construction,
    // and every ray-traced reflection of ordinary geometry shaded black.
    // Measured on the mirror contract: the hit albedo read 0.034 where the
    // object's own declared colour was 6.0, so a reflection carrying the
    // target could not be told from one carrying nothing.
    material::FamilyCapture capture{};
    capture.materialId = 0x4321;
    capture.generation = 1;
    capture.revision = 1;
    capture.staticRevision = 1;
    capture.featureId = material::FeatureIdOf(
        material::MaterialFamily::Default);
    capture.baseColor = {0.25f, 0.5f, 0.75f};
    capture.emitScale = 1.0f;
    capture.eyeRadius = 0.5f;
    capture.eyeIrisScale = 1.0f;
    // The slots every material must declare; without them the translation
    // refuses the capture before it ever reaches the colour.
    capture.slots[0].resourceId = 0x8000'0000'0000'1901ull;
    capture.slots[0].generation = 1;
    capture.slots[0].authored = true;
    capture.slots[1].resourceId = 0x8000'0000'0000'1902ull;
    capture.slots[1].generation = 1;
    capture.slots[1].authored = true;

    material::FamilyDescriptor descriptor{};
    REQUIRE(material::TranslateMaterialFamily(capture, descriptor) ==
        material::FamilyError::None);
    CHECK(descriptor.baseColor[0] == 0.25f);
    CHECK(descriptor.baseColor[1] == 0.5f);
    CHECK(descriptor.baseColor[2] == 0.75f);

    const auto gpu = material::BuildFamilyGpuRecord(descriptor);
    CHECK(gpu.baseColor[0] == 0.25f);
    CHECK(gpu.baseColor[1] == 0.5f);
    CHECK(gpu.baseColor[2] == 0.75f);
    // The tint stays absent, so the shader can still tell "no tint declared"
    // from "tinted black" and modulate only when there is something to
    // modulate by.
    CHECK(gpu.tintColor[3] == 0.0f);
}
