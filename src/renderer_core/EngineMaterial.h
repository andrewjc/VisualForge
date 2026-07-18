#pragma once

#include "MaterialShaderLayout.generated.h"
#include "renderer_core/EngineTexture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace vf::renderer::material {

enum class MaterialProvenance : std::uint8_t
{
    Missing,
    CanonicalFallback,
    MaterialDefault,
    TextureSet,
    RootMaterial,
    RuntimeMaterial,
    ShaderProperty,
};

enum class MaterialTextureRole : std::uint8_t
{
    BaseColor,
    Normal,
    SmoothSpec,
};

enum class MaterialColorSpace : std::uint8_t
{
    Linear,
    Srgb,
};

enum class MaterialAlphaSemantic : std::uint8_t
{
    Opaque,
    Coverage,
};

enum class MaterialNormalEncoding : std::uint8_t
{
    TangentSpaceBc5,
    ModelSpaceRgb,
};

enum class MaterialError : std::uint8_t
{
    None,
    InvalidIdentity,
    InvalidRevision,
    InvalidProvenance,
    AmbiguousSource,
    NonFiniteSource,
    InvalidTexture,
    InvalidTextureRole,
    InvalidColorSpace,
    InvalidAlphaSemantic,
    InvalidNormalEncoding,
    InvalidTransferLut,
    MissingDescriptor,
    DuplicateDescriptor,
    AllocationFailure,
};

struct ScalarSource
{
    float value{};
    MaterialProvenance provenance{MaterialProvenance::Missing};
};

struct Vector2Source
{
    std::array<float, 2> value{};
    MaterialProvenance provenance{MaterialProvenance::Missing};
};

struct Vector3Source
{
    std::array<float, 3> value{};
    MaterialProvenance provenance{MaterialProvenance::Missing};
};

struct TextureSource
{
    MaterialTextureRole role{MaterialTextureRole::BaseColor};
    std::uint64_t resourceId{};
    std::uint32_t generation{};
    texture::TextureFormat viewFormat{texture::TextureFormat::Unknown};
    MaterialProvenance provenance{MaterialProvenance::Missing};
};

struct MaterialCapture
{
    std::uint64_t materialId{};
    std::uint32_t generation{};
    std::uint32_t revision{};
    std::uint32_t staticRevision{};
    MaterialAlphaSemantic alphaSemantic{MaterialAlphaSemantic::Opaque};
    MaterialNormalEncoding normalEncoding{
        MaterialNormalEncoding::TangentSpaceBc5};
    std::vector<TextureSource> textures;
    std::vector<ScalarSource> alpha;
    std::vector<ScalarSource> alphaCutoff;
    std::vector<ScalarSource> smoothness;
    std::vector<ScalarSource> specularScale;
    std::vector<ScalarSource> fresnelPower;
    std::vector<Vector2Source> uvOffset;
    std::vector<Vector2Source> uvScale;
    std::vector<Vector3Source> specularColor;
};

struct ResolvedScalar
{
    float raw{};
    float value{};
    MaterialProvenance provenance{MaterialProvenance::Missing};
    bool clamped{};
};

struct ResolvedVector2
{
    std::array<float, 2> raw{};
    std::array<float, 2> value{};
    MaterialProvenance provenance{MaterialProvenance::Missing};
    bool clamped{};
};

struct ResolvedVector3
{
    std::array<float, 3> raw{};
    std::array<float, 3> value{};
    MaterialProvenance provenance{MaterialProvenance::Missing};
    bool clamped{};
};

struct ResolvedTexture
{
    MaterialTextureRole role{MaterialTextureRole::BaseColor};
    std::uint64_t resourceId{};
    std::uint32_t generation{};
    texture::TextureFormat viewFormat{texture::TextureFormat::Unknown};
    MaterialColorSpace colorSpace{MaterialColorSpace::Linear};
    MaterialProvenance provenance{MaterialProvenance::Missing};
    bool authored{};
};

struct CanonicalMaterial
{
    std::uint64_t materialId{};
    std::uint32_t generation{};
    std::uint32_t revision{};
    std::uint32_t staticRevision{};
    MaterialAlphaSemantic alphaSemantic{MaterialAlphaSemantic::Opaque};
    MaterialNormalEncoding normalEncoding{
        MaterialNormalEncoding::TangentSpaceBc5};
    std::array<ResolvedTexture, 3> textures{};
    ResolvedScalar alpha{};
    ResolvedScalar alphaCutoff{};
    ResolvedScalar smoothness{};
    ResolvedScalar specularScale{};
    ResolvedScalar fresnelPower{};
    ResolvedVector2 uvOffset{};
    ResolvedVector2 uvScale{};
    ResolvedVector3 specularColor{};
};

inline constexpr std::uint32_t kMaterialTransferVersion = 1;
inline constexpr std::uint32_t kMaterialTransferSampleCount = 16;

struct MaterialTransferLut
{
    std::uint32_t version{kMaterialTransferVersion};
    std::array<float, kMaterialTransferSampleCount>
        smoothnessToPerceptualRoughness{};
    std::array<float, kMaterialTransferSampleCount>
        specularWeight{};
};

struct SmoothSpecSample
{
    float specularWeight{1.0f};
    float smoothnessWeight{1.0f};
};

struct MaterialSurfaceInput
{
    texture::SampledColor baseColor{1.0f, 1.0f, 1.0f, 1.0f};
    texture::SampledColor normal{0.5f, 0.5f, 1.0f, 1.0f};
    texture::SampledColor smoothSpec{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 3> vertexColor{1.0f, 1.0f, 1.0f};
    std::array<float, 3> geometricNormal{0.0f, 0.0f, 1.0f};
    std::array<float, 3> tangent{1.0f, 0.0f, 0.0f};
    std::array<float, 3> bitangent{0.0f, 1.0f, 0.0f};
};

struct MaterialSurface
{
    std::array<float, 3> baseColor{};
    std::array<float, 3> normal{0.0f, 0.0f, 1.0f};
    std::array<float, 3> specularF0{0.04f, 0.04f, 0.04f};
    float opacity{1.0f};
    float smoothness{0.5f};
    float perceptualRoughness{0.5f};
    float alphaRoughness{0.25f};
    float fresnelPower{5.0f};
    bool discarded{};
};

struct GgxLighting
{
    std::array<float, 3> diffuse{};
    std::array<float, 3> specular{};
    std::array<float, 3> combined{};
};

[[nodiscard]] MaterialError TranslateMaterial(
    const MaterialCapture& capture,
    CanonicalMaterial& material) noexcept;
[[nodiscard]] std::array<float, 2> TransformMaterialUv(
    const CanonicalMaterial& material,
    std::array<float, 2> uv) noexcept;
[[nodiscard]] SmoothSpecSample DecodeSmoothSpec(
    const texture::SampledColor& sample) noexcept;
[[nodiscard]] MaterialTransferLut MakeDefaultMaterialTransferLut() noexcept;
[[nodiscard]] MaterialError ValidateMaterialTransferLut(
    const MaterialTransferLut& lut) noexcept;
[[nodiscard]] MaterialError EvaluateMaterialSurface(
    const CanonicalMaterial& material,
    const MaterialSurfaceInput& input,
    const MaterialTransferLut& transfer,
    MaterialSurface& surface) noexcept;
[[nodiscard]] GgxLighting EvaluateGgxDirect(
    const MaterialSurface& surface,
    std::array<float, 3> viewDirection,
    std::array<float, 3> lightDirection,
    std::array<float, 3> radiance) noexcept;

enum class DescriptorError : std::uint8_t
{
    None,
    InvalidResource,
    DuplicateResource,
};

class BindlessTextureTable
{
public:
    [[nodiscard]] DescriptorError Register(
        std::uint64_t resourceId,
        std::uint32_t generation,
        std::uint32_t descriptorIndex);
    [[nodiscard]] std::optional<std::uint32_t> Resolve(
        std::uint64_t resourceId,
        std::uint32_t generation) const noexcept;

private:
    struct Key
    {
        std::uint64_t resourceId{};
        std::uint32_t generation{};
        [[nodiscard]] bool operator<(const Key& other) const noexcept;
    };
    std::map<Key, std::uint32_t> descriptors_;
};

struct MaterialGpuRecords
{
    GpuMaterialStaticV1 staticRecord{};
    GpuMaterialDynamicV1 dynamicRecord{};
};

inline constexpr std::uint32_t kGpuMaterialHasBaseColor = 1u << 0;
inline constexpr std::uint32_t kGpuMaterialHasNormal = 1u << 1;
inline constexpr std::uint32_t kGpuMaterialHasSmoothSpec = 1u << 2;
inline constexpr std::uint32_t kGpuMaterialModelSpaceNormal = 1u << 3;
inline constexpr std::uint32_t kGpuMaterialAlphaCoverage = 1u << 4;

[[nodiscard]] MaterialError BuildMaterialGpuRecords(
    const CanonicalMaterial& material,
    const BindlessTextureTable& textures,
    std::uint32_t transferVersion,
    MaterialGpuRecords& records) noexcept;

struct MaterialUpdatePlan
{
    bool writeStatic{};
    bool writeDynamic{};
    bool writeDescriptors{};
};

class MaterialRevisionTracker
{
public:
    [[nodiscard]] MaterialUpdatePlan PlanAndCommit(
        const MaterialGpuRecords& records) noexcept;

private:
    MaterialGpuRecords current_{};
    bool currentValid_{};
};

inline constexpr std::uint32_t kMaterialPacketMagic = 0x544D4656u; // "VFMT"
inline constexpr std::uint16_t kMaterialPacketVersionMajor = 1;
inline constexpr std::uint16_t kMaterialPacketVersionMinor = 0;

enum class MaterialPacketError : std::uint8_t
{
    None,
    TruncatedHeader,
    BadMagic,
    UnsupportedVersion,
    SizeMismatch,
    MisalignedSection,
    SectionOutOfBounds,
    ChecksumMismatch,
    InvalidMaterial,
    TexturePacketFailed,
    TextureMismatch,
    AllocationFailure,
    UnsupportedTransferVersion,
};

struct alignas(8) MaterialPacketHeaderV1
{
    std::uint32_t magic{kMaterialPacketMagic};
    std::uint16_t versionMajor{kMaterialPacketVersionMajor};
    std::uint16_t versionMinor{kMaterialPacketVersionMinor};
    std::uint32_t headerSize{sizeof(MaterialPacketHeaderV1)};
    std::uint32_t totalSize{};
    std::uint32_t payloadCrc32{};
    std::uint32_t recordOffset{};
    std::uint32_t recordSize{};
    std::uint32_t transferVersion{kMaterialTransferVersion};
    std::uint32_t textureOffsets[3]{};
    std::uint32_t textureSizes[3]{};
    std::uint64_t reserved[3]{};
};

struct TextureBindingPacketV1
{
    std::uint64_t resourceId{};
    std::uint32_t generation{};
    texture::TextureFormat viewFormat{texture::TextureFormat::Unknown};
    std::uint32_t role{};
    std::uint32_t colorSpace{};
    std::uint32_t provenance{};
    std::uint32_t authored{};
};

struct ResolvedScalarPacketV1
{
    float raw{};
    float value{};
    std::uint32_t provenance{};
    std::uint32_t flags{};
};

struct ResolvedVector2PacketV1
{
    float raw[2]{};
    float value[2]{};
    std::uint32_t provenance{};
    std::uint32_t flags{};
};

struct ResolvedVector3PacketV1
{
    float raw[3]{};
    float value[3]{};
    std::uint32_t provenance{};
    std::uint32_t flags{};
};

struct alignas(8) MaterialRecordV1
{
    std::uint64_t materialId{};
    std::uint32_t generation{};
    std::uint32_t revision{};
    std::uint32_t staticRevision{};
    std::uint32_t alphaSemantic{};
    std::uint32_t normalEncoding{};
    std::uint32_t reserved0{};
    TextureBindingPacketV1 textures[3]{};
    ResolvedScalarPacketV1 scalars[5]{};
    ResolvedVector2PacketV1 uvOffset{};
    ResolvedVector2PacketV1 uvScale{};
    ResolvedVector3PacketV1 specularColor{};
};

struct MaterialReplayBundle
{
    CanonicalMaterial material;
    std::uint32_t transferVersion{kMaterialTransferVersion};
    std::array<texture::CapturedTexture, 3> textures;
};

[[nodiscard]] MaterialPacketError EncodeMaterialReplayBundle(
    const MaterialReplayBundle& bundle,
    std::vector<std::byte>& bytes) noexcept;
[[nodiscard]] MaterialPacketError DecodeMaterialReplayBundle(
    std::span<const std::byte> bytes,
    MaterialReplayBundle& bundle) noexcept;
[[nodiscard]] const char* ToString(MaterialPacketError error) noexcept;

[[nodiscard]] const char* ToString(MaterialError error) noexcept;

static_assert(sizeof(MaterialPacketHeaderV1) == 80);
static_assert(sizeof(TextureBindingPacketV1) == 32);
static_assert(sizeof(ResolvedScalarPacketV1) == 16);
static_assert(sizeof(ResolvedVector2PacketV1) == 24);
static_assert(sizeof(ResolvedVector3PacketV1) == 32);
static_assert(sizeof(MaterialRecordV1) == 288);

}
