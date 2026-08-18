#include "renderer_core/EngineMaterialFile.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace vf::renderer;

namespace {

void U8(std::vector<std::byte>& bytes, const std::uint8_t value)
{
    bytes.push_back(static_cast<std::byte>(value));
}

void U32(std::vector<std::byte>& bytes, const std::uint32_t value)
{
    for (std::size_t shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
}

void F32(std::vector<std::byte>& bytes, const float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    U32(bytes, bits);
}

void Tag(std::vector<std::byte>& bytes, const std::string& text)
{
    for (const auto character : text) {
        bytes.push_back(static_cast<std::byte>(character));
    }
}

// Length-prefixed and NUL-terminated, with the length counting the
// terminator, which is how the format writes every path.
void Str(std::vector<std::byte>& bytes, const std::string& text)
{
    U32(bytes, static_cast<std::uint32_t>(text.size() + 1));
    Tag(bytes, text);
    U8(bytes, 0);
}

struct ShaderOptions
{
    bool glowmap{};
    bool environmentMapping{};
    bool hair{};
    bool skinTint{};
    bool faceGen{};
    bool tree{};
    bool environmentMappingEye{};
    bool emitEnabled{};
    bool grayscaleToPaletteColor{};
    std::string greyscaleTexture;
    std::string displacementTexture;
};

std::vector<std::byte> BuildShaderMaterial(const ShaderOptions& options)
{
    std::vector<std::byte> bytes;
    Tag(bytes, "BGSM");
    U32(bytes, 2);
    U32(bytes, 3);                       // tileFlags
    F32(bytes, 0.0f); F32(bytes, 0.0f);  // uOffset, vOffset
    F32(bytes, 1.0f); F32(bytes, 1.0f);  // uScale, vScale
    F32(bytes, 1.0f);                    // alpha
    U8(bytes, 0);                        // alphaBlend
    U32(bytes, 6); U32(bytes, 7);        // alphaSrc, alphaDst
    U8(bytes, 128);                      // alphaTestRef
    U8(bytes, 1);                        // alphaTest
    U8(bytes, 1); U8(bytes, 1);          // zBufferWrite, zBufferTest
    U8(bytes, 0); U8(bytes, 0);          // ssr, wetnessControlSsr
    U8(bytes, 0);                        // decal
    U8(bytes, 1);                        // twoSided
    U8(bytes, 0); U8(bytes, 0);          // decalNoFade, nonOccluder
    U8(bytes, 0); U8(bytes, 0);          // refraction, refractionFalloff
    F32(bytes, 0.0f);                    // refractionPower
    U8(bytes, options.environmentMapping ? 1 : 0);
    F32(bytes, 1.0f);                    // environmentMappingMaskScale
    U8(bytes, options.grayscaleToPaletteColor ? 1 : 0);

    Str(bytes, "textures/diffuse_d.dds");
    Str(bytes, "textures/normal_n.dds");
    Str(bytes, "textures/spec_s.dds");
    Str(bytes, options.greyscaleTexture);
    Str(bytes, "");
    Str(bytes, "");
    Str(bytes, "");
    Str(bytes, "");
    Str(bytes, options.displacementTexture);

    U8(bytes, 0);                        // enableEditorAlphaRef
    U8(bytes, 0);                        // rimLighting
    F32(bytes, 0.0f); F32(bytes, 0.0f);  // rimPower, backlightPower
    U8(bytes, 0);                        // subsurfaceLighting
    F32(bytes, 0.0f);                    // subsurfaceLightingRolloff
    U8(bytes, 1);                        // specularEnabled
    F32(bytes, 1.0f); F32(bytes, 1.0f); F32(bytes, 1.0f);  // specularColor
    F32(bytes, 1.0f);                    // specularMult
    F32(bytes, 0.51f);                   // smoothness
    F32(bytes, 5.0f);                    // fresnelPower
    for (int index = 0; index < 6; ++index) F32(bytes, 1.0f);  // wetness
    Str(bytes, "");                      // rootMaterialPath
    U8(bytes, 0);                        // anisoLighting
    U8(bytes, options.emitEnabled ? 1 : 0);
    if (options.emitEnabled) {
        F32(bytes, 1.0f); F32(bytes, 0.5f); F32(bytes, 0.25f);
    }
    F32(bytes, 1.0f);                    // emittanceMult
    U8(bytes, 0);                        // modelSpaceNormals
    U8(bytes, 0);                        // externalEmittance
    U8(bytes, 0);                        // backLighting
    U8(bytes, 1);                        // receiveShadows
    U8(bytes, 0);                        // hideSecret
    U8(bytes, 1);                        // castShadows
    U8(bytes, 0);                        // dissolveFade
    U8(bytes, 0);                        // assumeShadowmask
    U8(bytes, options.glowmap ? 1 : 0);
    U8(bytes, 0);                        // environmentMappingWindow
    U8(bytes, options.environmentMappingEye ? 1 : 0);
    U8(bytes, options.hair ? 1 : 0);
    F32(bytes, 0.0f); F32(bytes, 0.0f); F32(bytes, 0.0f);  // hairTintColor
    U8(bytes, options.tree ? 1 : 0);
    U8(bytes, options.faceGen ? 1 : 0);
    U8(bytes, options.skinTint ? 1 : 0);
    U8(bytes, 0);                        // tessellate
    for (int index = 0; index < 5; ++index) F32(bytes, 0.0f);  // version < 3
    F32(bytes, 1.0f);                    // grayscaleToPaletteScale
    U8(bytes, 0);                        // skewSpecularAlpha
    return bytes;
}

std::vector<std::byte> BuildEffectMaterial()
{
    std::vector<std::byte> bytes;
    Tag(bytes, "BGEM");
    U32(bytes, 2);
    U32(bytes, 3);
    F32(bytes, 0.0f); F32(bytes, 0.0f);
    F32(bytes, 1.0f); F32(bytes, 1.0f);
    F32(bytes, 1.0f);
    U8(bytes, 1);                        // alphaBlend
    U32(bytes, 6); U32(bytes, 7);
    U8(bytes, 128);
    U8(bytes, 0);
    U8(bytes, 0); U8(bytes, 1);
    U8(bytes, 0); U8(bytes, 0);
    U8(bytes, 0);
    U8(bytes, 1);                        // twoSided
    U8(bytes, 0); U8(bytes, 0);
    U8(bytes, 0); U8(bytes, 0);
    F32(bytes, 0.0f);
    U8(bytes, 0);
    F32(bytes, 1.0f);
    U8(bytes, 0);

    Str(bytes, "textures/effect_d.dds");
    Str(bytes, "");
    Str(bytes, "");
    Str(bytes, "");
    Str(bytes, "");
    U8(bytes, 0);                        // bloodEnabled
    U8(bytes, 1);                        // effectLightingEnabled
    U8(bytes, 1);                        // falloffEnabled
    U8(bytes, 0);                        // falloffColorEnabled
    U8(bytes, 1);                        // grayscaleToPaletteAlpha
    U8(bytes, 0);                        // softEnabled
    F32(bytes, 1.0f); F32(bytes, 1.0f); F32(bytes, 1.0f);  // baseColor
    F32(bytes, 1.0f);                    // baseColorScale
    F32(bytes, 0.0f); F32(bytes, 1.0f);  // falloff angles
    F32(bytes, 0.0f); F32(bytes, 1.0f);  // falloff opacities
    F32(bytes, 1.0f);                    // lightingInfluence
    U8(bytes, 0);                        // envmapMinLOD
    F32(bytes, 100.0f);                  // softDepth
    return bytes;
}

}  // namespace

TEST_CASE("P16_a_shader_material_is_consumed_exactly", "[phase16][material]")
{
    ShaderOptions options{};
    options.environmentMapping = true;
    const auto bytes = BuildShaderMaterial(options);
    materialfile::MaterialFile material;
    REQUIRE(materialfile::ParseMaterialFile(bytes, material) ==
        materialfile::MaterialFileError::None);
    CHECK(material.kind == materialfile::MaterialFileKind::Shader);
    CHECK(material.version == 2);
    CHECK(material.alphaTest);
    CHECK(material.alphaTestReference == 128);
    CHECK(material.twoSided);
    CHECK(material.environmentMapping);
    CHECK(material.specularEnabled);
    CHECK(material.textures[0] == "textures/diffuse_d.dds");
    CHECK(material.textures[1] == "textures/normal_n.dds");
    CHECK(material.textures[3].empty());

    // A layout that is wrong but self-consistent reads every field and stops
    // somewhere else, so landing exactly on the end is the check that
    // separates a correct layout from a plausible one. One extra byte has to
    // fail for that check to be doing anything.
    auto extended = bytes;
    extended.push_back(std::byte{0});
    materialfile::MaterialFile ignored;
    CHECK(materialfile::ParseMaterialFile(extended, ignored) ==
        materialfile::MaterialFileError::TrailingBytes);

    auto shortened = bytes;
    shortened.pop_back();
    CHECK(materialfile::ParseMaterialFile(shortened, ignored) ==
        materialfile::MaterialFileError::Truncated);
}

TEST_CASE("P16_emission_moves_the_rest_of_the_record",
    "[phase16][material]")
{
    // The emissive colour is present only when emission is declared. It is
    // the one place this format is not a flat record, so reading it
    // unconditionally shifts every field after it by twelve bytes -- which
    // the exact-consumption check catches, and which a field-by-field reader
    // that stopped at the last field it cared about would not.
    ShaderOptions options{};
    options.emitEnabled = true;
    options.glowmap = true;
    const auto bytes = BuildShaderMaterial(options);
    materialfile::MaterialFile material;
    REQUIRE(materialfile::ParseMaterialFile(bytes, material) ==
        materialfile::MaterialFileError::None);
    CHECK(material.emitEnabled);
    CHECK(material.glowmap);
    CHECK(materialfile::ClassifyMaterialFile(material) ==
        material::MaterialFamily::GlowMap);
}

TEST_CASE("P16_a_material_is_classified_by_what_it_declares",
    "[phase16][material]")
{
    const auto family = [](const ShaderOptions& options) {
        materialfile::MaterialFile material;
        REQUIRE(materialfile::ParseMaterialFile(
            BuildShaderMaterial(options), material) ==
            materialfile::MaterialFileError::None);
        return materialfile::ClassifyMaterialFile(material);
    };

    CHECK(family({}) == material::MaterialFamily::Default);

    ShaderOptions environment{};
    environment.environmentMapping = true;
    CHECK(family(environment) == material::MaterialFamily::EnvironmentMap);

    ShaderOptions glow{};
    glow.glowmap = true;
    CHECK(family(glow) == material::MaterialFamily::GlowMap);

    // A hair material also declares environment mapping, and an eye material
    // declares both that and face generation. The specific reading has to win
    // or the corpus reports almost everything as one family.
    ShaderOptions hair{};
    hair.hair = true;
    hair.environmentMapping = true;
    CHECK(family(hair) == material::MaterialFamily::HairTint);

    ShaderOptions eye{};
    eye.environmentMappingEye = true;
    eye.environmentMapping = true;
    eye.faceGen = true;
    CHECK(family(eye) == material::MaterialFamily::Eye);

    ShaderOptions skin{};
    skin.skinTint = true;
    skin.faceGen = true;
    CHECK(family(skin) == material::MaterialFamily::SkinTint);

    ShaderOptions tree{};
    tree.tree = true;
    CHECK(family(tree) == material::MaterialFamily::TreeAnimation);
}

TEST_CASE("P16_an_effect_material_is_consumed_exactly",
    "[phase16][material]")
{
    const auto bytes = BuildEffectMaterial();
    materialfile::MaterialFile material;
    REQUIRE(materialfile::ParseMaterialFile(bytes, material) ==
        materialfile::MaterialFileError::None);
    CHECK(material.kind == materialfile::MaterialFileKind::Effect);
    CHECK(material.alphaBlend);
    CHECK(material.twoSided);
    CHECK(material.effectLightingEnabled);
    CHECK(material.falloffEnabled);
    CHECK(material.grayscaleToPaletteAlpha);
    CHECK(material.effectTextures[0] == "textures/effect_d.dds");
    // Not folded into Default: an effect material is not a lighting shader,
    // and reporting it as one would claim the lighting path handles it.
    CHECK(materialfile::ClassifyMaterialFile(material) ==
        material::MaterialFamily::None);
}

TEST_CASE("P16_a_material_file_refuses_what_it_cannot_read",
    "[phase16][material]")
{
    materialfile::MaterialFile material;
    auto bytes = BuildShaderMaterial({});
    bytes[0] = static_cast<std::byte>('X');
    CHECK(materialfile::ParseMaterialFile(bytes, material) ==
        materialfile::MaterialFileError::BadMagic);

    // Later versions moved and added fields. Reading one with this layout
    // would consume it to a different place, and the version is the honest
    // reason to refuse rather than the trailing-byte count.
    auto newer = BuildShaderMaterial({});
    newer[4] = static_cast<std::byte>(20);
    CHECK(materialfile::ParseMaterialFile(newer, material) ==
        materialfile::MaterialFileError::UnsupportedVersion);

    const std::vector<std::byte> stub(4, std::byte{0});
    CHECK(materialfile::ParseMaterialFile(stub, material) ==
        materialfile::MaterialFileError::TruncatedHeader);
}
