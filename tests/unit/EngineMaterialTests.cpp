#include "renderer_core/EngineMaterial.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace {

using namespace vf::renderer::material;
using vf::renderer::texture::SampledColor;
using vf::renderer::texture::TextureFormat;

MaterialCapture BaseCapture()
{
    MaterialCapture capture{};
    capture.materialId = 0x9000'0000'0000'0042ull;
    capture.generation = 3;
    capture.revision = 7;
    capture.staticRevision = 2;
    return capture;
}

TextureSource Texture(
    const MaterialTextureRole role,
    const std::uint64_t id,
    const TextureFormat format,
    const MaterialProvenance provenance = MaterialProvenance::TextureSet)
{
    return {role, id, 1, format, provenance};
}

CanonicalMaterial Translate(const MaterialCapture& capture)
{
    CanonicalMaterial material{};
    const auto result = TranslateMaterial(capture, material);
    INFO(ToString(result));
    REQUIRE(result == MaterialError::None);
    return material;
}

}

TEST_CASE("phase9 authored source precedence preserves provenance", "[phase9][material]")
{
    auto capture = BaseCapture();
    capture.smoothness = {
        {0.05f, MaterialProvenance::Missing},
        {0.10f, MaterialProvenance::CanonicalFallback},
        {0.20f, MaterialProvenance::MaterialDefault},
        {0.30f, MaterialProvenance::TextureSet},
        {0.40f, MaterialProvenance::RootMaterial},
        {0.50f, MaterialProvenance::RuntimeMaterial},
        {0.60f, MaterialProvenance::ShaderProperty},
    };
    const auto material = Translate(capture);
    CHECK(material.smoothness.raw == 0.60f);
    CHECK(material.smoothness.value == 0.60f);
    CHECK(material.smoothness.provenance ==
        MaterialProvenance::ShaderProperty);

    capture.smoothness.push_back(
        {0.70f, MaterialProvenance::ShaderProperty});
    CanonicalMaterial rejected{};
    CHECK(TranslateMaterial(capture, rejected) ==
        MaterialError::AmbiguousSource);
}

TEST_CASE("phase9 semantic roles own texture color space", "[phase9][material]")
{
    auto capture = BaseCapture();
    capture.textures = {
        Texture(MaterialTextureRole::BaseColor, 101,
            TextureFormat::BC1UnormSrgb),
        Texture(MaterialTextureRole::Normal, 102,
            TextureFormat::BC5Unorm),
        Texture(MaterialTextureRole::SmoothSpec, 103,
            TextureFormat::BC5Unorm),
    };
    const auto material = Translate(capture);
    CHECK(material.textures[0].colorSpace == MaterialColorSpace::Srgb);
    CHECK(material.textures[1].colorSpace == MaterialColorSpace::Linear);
    CHECK(material.textures[2].colorSpace == MaterialColorSpace::Linear);
    CHECK(material.textures[0].authored);
    CHECK(material.textures[1].authored);
    CHECK(material.textures[2].authored);

    auto invalidBase = capture;
    invalidBase.textures[0].viewFormat = TextureFormat::BC1Unorm;
    CanonicalMaterial rejected{};
    CHECK(TranslateMaterial(invalidBase, rejected) ==
        MaterialError::InvalidColorSpace);
    auto invalidData = capture;
    invalidData.textures[1].viewFormat = TextureFormat::BC1UnormSrgb;
    CHECK(TranslateMaterial(invalidData, rejected) ==
        MaterialError::InvalidColorSpace);

    const auto decoded = DecodeSmoothSpec(
        SampledColor{0.25f, 0.75f, 0.5f, 0.125f});
    CHECK(decoded.specularWeight == 0.25f);
    CHECK(decoded.smoothnessWeight == 0.75f);
}

TEST_CASE("phase9 missing maps resolve to canonical semantic fallbacks", "[phase9][material]")
{
    const auto material = Translate(BaseCapture());
    for (std::size_t index = 0; index < material.textures.size(); ++index) {
        INFO(index);
        CHECK(material.textures[index].resourceId != 0);
        CHECK(material.textures[index].generation == 1);
        CHECK(material.textures[index].provenance ==
            MaterialProvenance::CanonicalFallback);
        CHECK_FALSE(material.textures[index].authored);
    }
    CHECK(material.textures[0].resourceId != material.textures[1].resourceId);
    CHECK(material.textures[1].resourceId != material.textures[2].resourceId);
}

TEST_CASE("phase9 raw material values survive bounded translation", "[phase9][material]")
{
    auto capture = BaseCapture();
    capture.alpha = {{1.5f, MaterialProvenance::RuntimeMaterial}};
    capture.alphaCutoff = {{-0.25f, MaterialProvenance::RootMaterial}};
    capture.smoothness = {{1.4f, MaterialProvenance::RootMaterial}};
    capture.specularScale = {{-2.0f, MaterialProvenance::RootMaterial}};
    capture.fresnelPower = {{32.0f, MaterialProvenance::RootMaterial}};
    capture.uvScale = {{{2048.0f, -2048.0f},
        MaterialProvenance::RootMaterial}};
    capture.specularColor = {{{-0.5f, 0.5f, 2.0f},
        MaterialProvenance::RootMaterial}};
    const auto material = Translate(capture);
    CHECK(material.alpha.raw == 1.5f);
    CHECK(material.alpha.value == 1.0f);
    CHECK(material.alpha.clamped);
    CHECK(material.alphaCutoff.value == 0.0f);
    CHECK(material.smoothness.raw == 1.4f);
    CHECK(material.smoothness.value == 1.0f);
    CHECK(material.specularScale.value == 0.0f);
    CHECK(material.fresnelPower.value == 16.0f);
    CHECK(material.uvScale.raw[0] == 2048.0f);
    CHECK(material.uvScale.value == std::array<float, 2>{1024.0f, -1024.0f});
    CHECK(material.specularColor.value ==
        std::array<float, 3>{0.0f, 0.5f, 1.0f});

    capture.smoothness = {{std::numeric_limits<float>::quiet_NaN(),
        MaterialProvenance::RootMaterial}};
    CanonicalMaterial rejected{};
    CHECK(TranslateMaterial(capture, rejected) ==
        MaterialError::NonFiniteSource);
}

TEST_CASE("phase9 UV alpha and normal semantics are explicit", "[phase9][material]")
{
    auto capture = BaseCapture();
    capture.alphaSemantic = MaterialAlphaSemantic::Coverage;
    capture.normalEncoding = MaterialNormalEncoding::TangentSpaceBc5;
    capture.textures.push_back(Texture(
        MaterialTextureRole::Normal, 200, TextureFormat::BC5Unorm));
    capture.alphaCutoff = {{0.5f, MaterialProvenance::RootMaterial}};
    capture.uvScale = {{{2.0f, -3.0f}, MaterialProvenance::RootMaterial}};
    capture.uvOffset = {{{0.25f, 0.5f}, MaterialProvenance::RootMaterial}};
    const auto material = Translate(capture);
    CHECK(TransformMaterialUv(material, {0.5f, 0.25f}) ==
        std::array<float, 2>{1.25f, -0.25f});

    auto transfer = MakeDefaultMaterialTransferLut();
    MaterialSurfaceInput input{};
    input.baseColor = {0.8f, 0.4f, 0.2f, 0.4f};
    input.normal = {1.0f, 0.5f, 0.5f, 1.0f};
    MaterialSurface surface{};
    REQUIRE(EvaluateMaterialSurface(
        material, input, transfer, surface) == MaterialError::None);
    CHECK(surface.discarded);
    CHECK(surface.normal[0] > 0.99f);
    CHECK(std::abs(surface.normal[2]) < 0.001f);

    auto modelCapture = capture;
    modelCapture.normalEncoding = MaterialNormalEncoding::ModelSpaceRgb;
    const auto modelMaterial = Translate(modelCapture);
    input.normal = {0.5f, 1.0f, 0.5f, 1.0f};
    REQUIRE(EvaluateMaterialSurface(
        modelMaterial, input, transfer, surface) == MaterialError::None);
    CHECK(surface.normal[1] > 0.99f);

    const auto missingNormal = Translate(BaseCapture());
    input.geometricNormal = {0.0f, 0.6f, 0.8f};
    REQUIRE(EvaluateMaterialSurface(
        missingNormal, input, transfer, surface) == MaterialError::None);
    CHECK(surface.normal == input.geometricNormal);
}

TEST_CASE("phase9 reflected GPU records preserve exact offsets", "[phase9][material]")
{
    CHECK(kPhase9MaterialLayoutHash == 0xF97A'3578'9BC8'4031ull);
    CHECK(sizeof(GpuMaterialStaticV1) == 64);
    CHECK(offsetof(GpuMaterialStaticV1, textureIndices) == 16);
    CHECK(offsetof(GpuMaterialStaticV1, uvScale) == 32);
    CHECK(offsetof(GpuMaterialStaticV1, specularColor) == 48);
    CHECK(sizeof(GpuMaterialDynamicV1) == 48);
    CHECK(offsetof(GpuMaterialDynamicV1, alpha) == 16);
    CHECK(offsetof(GpuMaterialDynamicV1, alphaCutoff) == 32);
}

TEST_CASE("phase9 dynamic material revision avoids descriptor churn", "[phase9][material]")
{
    auto material = Translate(BaseCapture());
    BindlessTextureTable table;
    for (std::size_t index = 0; index < material.textures.size(); ++index) {
        REQUIRE(table.Register(material.textures[index].resourceId,
            material.textures[index].generation,
            static_cast<std::uint32_t>(10 + index)) == DescriptorError::None);
    }
    MaterialGpuRecords records{};
    REQUIRE(BuildMaterialGpuRecords(
        material, table, 1, records) == MaterialError::None);
    MaterialRevisionTracker tracker;
    auto plan = tracker.PlanAndCommit(records);
    CHECK(plan.writeStatic);
    CHECK(plan.writeDynamic);
    CHECK(plan.writeDescriptors);

    ++material.revision;
    material.smoothness.raw = 0.8f;
    material.smoothness.value = 0.8f;
    REQUIRE(BuildMaterialGpuRecords(
        material, table, 1, records) == MaterialError::None);
    plan = tracker.PlanAndCommit(records);
    CHECK_FALSE(plan.writeStatic);
    CHECK(plan.writeDynamic);
    CHECK_FALSE(plan.writeDescriptors);

    material.textures[0].resourceId = 0x1234;
    material.textures[0].generation = 2;
    ++material.staticRevision;
    REQUIRE(table.Register(0x1234, 2, 99) == DescriptorError::None);
    REQUIRE(BuildMaterialGpuRecords(
        material, table, 1, records) == MaterialError::None);
    plan = tracker.PlanAndCommit(records);
    CHECK(plan.writeStatic);
    CHECK(plan.writeDynamic);
    CHECK(plan.writeDescriptors);
}

TEST_CASE("phase9 versioned transfer and GGX sweeps stay authored", "[phase9][material]")
{
    auto transfer = MakeDefaultMaterialTransferLut();
    REQUIRE(ValidateMaterialTransferLut(transfer) == MaterialError::None);
    auto invalid = transfer;
    invalid.smoothnessToPerceptualRoughness[8] = 2.0f;
    CHECK(ValidateMaterialTransferLut(invalid) ==
        MaterialError::InvalidTransferLut);

    auto capture = BaseCapture();
    capture.specularColor = {{{0.8f, 0.2f, 0.05f},
        MaterialProvenance::RootMaterial}};
    capture.smoothness = {{0.9f, MaterialProvenance::RootMaterial}};
    auto smooth = Translate(capture);
    MaterialSurfaceInput input{};
    MaterialSurface smoothSurface{};
    REQUIRE(EvaluateMaterialSurface(
        smooth, input, transfer, smoothSurface) == MaterialError::None);

    capture.smoothness = {{0.2f, MaterialProvenance::RootMaterial}};
    auto rough = Translate(capture);
    MaterialSurface roughSurface{};
    REQUIRE(EvaluateMaterialSurface(
        rough, input, transfer, roughSurface) == MaterialError::None);
    CHECK(smoothSurface.alphaRoughness < roughSurface.alphaRoughness);
    CHECK(smoothSurface.specularF0[0] > smoothSurface.specularF0[1]);
    CHECK(smoothSurface.specularF0[1] > smoothSurface.specularF0[2]);

    const auto onAxis = std::array<float, 3>{0.0f, 0.0f, 1.0f};
    const auto radiance = std::array<float, 3>{1.0f, 1.0f, 1.0f};
    const auto smoothOn = EvaluateGgxDirect(
        smoothSurface, onAxis, onAxis, radiance);
    const auto roughOn = EvaluateGgxDirect(
        roughSurface, onAxis, onAxis, radiance);
    CHECK(smoothOn.specular[0] > roughOn.specular[0]);

    const auto offAxis = std::array<float, 3>{0.6f, 0.0f, 0.8f};
    const auto smoothOff = EvaluateGgxDirect(
        smoothSurface, onAxis, offAxis, radiance);
    const auto roughOff = EvaluateGgxDirect(
        roughSurface, onAxis, offAxis, radiance);
    CHECK(roughOff.specular[0] > smoothOff.specular[0]);
}

TEST_CASE("phase9 material replay bundle is pointer-free and checksummed", "[phase9][material]")
{
    MaterialReplayBundle source{};
    source.material = Translate(BaseCapture());
    source.transferVersion = 1;
    source.textures[0] = vf::renderer::texture::MakeFallbackTexture(
        vf::renderer::texture::FallbackTextureRole::White);
    source.textures[1] = vf::renderer::texture::MakeFallbackTexture(
        vf::renderer::texture::FallbackTextureRole::FlatNormal);
    source.textures[2] = vf::renderer::texture::MakeFallbackTexture(
        vf::renderer::texture::FallbackTextureRole::NeutralMask);

    std::vector<std::byte> first;
    std::vector<std::byte> second;
    REQUIRE(EncodeMaterialReplayBundle(source, first) ==
        MaterialPacketError::None);
    REQUIRE(EncodeMaterialReplayBundle(source, second) ==
        MaterialPacketError::None);
    CHECK(first == second);
    REQUIRE(first.size() >= sizeof(MaterialPacketHeaderV1));
    const auto& header = *reinterpret_cast<const MaterialPacketHeaderV1*>(
        first.data());
    CHECK(header.recordOffset == sizeof(MaterialPacketHeaderV1));
    CHECK(header.recordSize == sizeof(MaterialRecordV1));
    CHECK(header.textureOffsets[0] % 8 == 0);
    CHECK(header.textureOffsets[1] % 8 == 0);
    CHECK(header.textureOffsets[2] % 8 == 0);

    MaterialReplayBundle decoded{};
    REQUIRE(DecodeMaterialReplayBundle(first, decoded) ==
        MaterialPacketError::None);
    CHECK(decoded.material.materialId == source.material.materialId);
    CHECK(decoded.material.smoothness.raw == source.material.smoothness.raw);
    CHECK(decoded.material.textures[1].provenance ==
        MaterialProvenance::CanonicalFallback);
    CHECK(decoded.textures[2].resourceId == source.textures[2].resourceId);

    first.back() ^= std::byte{0x20};
    CHECK(DecodeMaterialReplayBundle(first, decoded) ==
        MaterialPacketError::ChecksumMismatch);
}

TEST_CASE("phase9 material replay rejects unimplemented transfer versions", "[phase9][material]")
{
    MaterialReplayBundle source{};
    source.material = Translate(BaseCapture());
    source.textures[0] = vf::renderer::texture::MakeFallbackTexture(
        vf::renderer::texture::FallbackTextureRole::White);
    source.textures[1] = vf::renderer::texture::MakeFallbackTexture(
        vf::renderer::texture::FallbackTextureRole::FlatNormal);
    source.textures[2] = vf::renderer::texture::MakeFallbackTexture(
        vf::renderer::texture::FallbackTextureRole::NeutralMask);

    std::vector<std::byte> bytes;
    source.transferVersion = kMaterialTransferVersion + 1;
    CHECK(EncodeMaterialReplayBundle(source, bytes) ==
        MaterialPacketError::UnsupportedTransferVersion);

    source.transferVersion = kMaterialTransferVersion;
    REQUIRE(EncodeMaterialReplayBundle(source, bytes) ==
        MaterialPacketError::None);
    auto header = MaterialPacketHeaderV1{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    header.transferVersion = kMaterialTransferVersion + 1;
    std::memcpy(bytes.data(), &header, sizeof(header));
    MaterialReplayBundle decoded{};
    CHECK(DecodeMaterialReplayBundle(bytes, decoded) ==
        MaterialPacketError::UnsupportedTransferVersion);
}

TEST_CASE("phase9 material bundle rejects texture identity substitution", "[phase9][material]")
{
    MaterialReplayBundle source{};
    source.material = Translate(BaseCapture());
    source.textures[0] = vf::renderer::texture::MakeFallbackTexture(
        vf::renderer::texture::FallbackTextureRole::FlatNormal);
    source.textures[1] = vf::renderer::texture::MakeFallbackTexture(
        vf::renderer::texture::FallbackTextureRole::White);
    source.textures[2] = vf::renderer::texture::MakeFallbackTexture(
        vf::renderer::texture::FallbackTextureRole::NeutralMask);
    std::vector<std::byte> bytes;
    CHECK(EncodeMaterialReplayBundle(source, bytes) ==
        MaterialPacketError::TextureMismatch);
}
