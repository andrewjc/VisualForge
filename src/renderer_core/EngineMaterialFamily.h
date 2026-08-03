#pragma once

#include "SceneShaderLayout.generated.h"
#include "renderer_core/EngineMaterial.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::material {

// The engine binds a ten-slot shader texture set (`BSShaderData 0x1AC..1D8`).
// Role IDs 0..7 are recorded; 8 and 9 carry no recorded role, so an authored
// slot there is counted as unclassified rather than assigned a meaning.
inline constexpr std::uint32_t kShaderTextureSlots = 10;
inline constexpr std::uint32_t kWetnessControls = 6;

// Lighting feature IDs, recorded verbatim from the material ABI. The engine
// uses -1 for "none"; anything else this build does not know is Unknown and
// must never be folded into Default.
enum class MaterialFamily : std::uint8_t
{
    Default = 0,
    EnvironmentMap = 1,
    GlowMap = 2,
    Parallax = 3,
    Face = 4,
    SkinTint = 5,
    HairTint = 6,
    ParallaxOcclusion = 7,
    Landscape = 8,
    LodLandscape = 9,
    Snow = 10,
    MultiLayerParallax = 11,
    TreeAnimation = 12,
    LodObjects = 13,
    MultiIndexSnow = 14,
    LodObjectsHd = 15,
    Eye = 16,
    Cloud = 17,
    LodLandscapeNoise = 18,
    LodLandscapeBlend = 19,
    Dismemberment = 20,
    None = 21,
    Unknown = 22,
};

// The complete 64-bit shader property flag map. Recorded in full because a
// partial copy invites reading a neighbouring bit by accident.
namespace PropertyFlag {

inline constexpr std::uint64_t Specular = 1ull << 0;
inline constexpr std::uint64_t Skinned = 1ull << 1;
inline constexpr std::uint64_t TemporaryRefraction = 1ull << 2;
inline constexpr std::uint64_t VertexAlpha = 1ull << 3;
inline constexpr std::uint64_t GreyscaleToPaletteColor = 1ull << 4;
inline constexpr std::uint64_t GreyscaleToPaletteAlpha = 1ull << 5;
inline constexpr std::uint64_t Falloff = 1ull << 6;
inline constexpr std::uint64_t EnvironmentMap = 1ull << 7;
inline constexpr std::uint64_t RgbFalloff = 1ull << 8;
inline constexpr std::uint64_t CastShadows = 1ull << 9;
inline constexpr std::uint64_t Face = 1ull << 10;
inline constexpr std::uint64_t UiMaskRectangles = 1ull << 11;
inline constexpr std::uint64_t ModelSpaceNormals = 1ull << 12;
inline constexpr std::uint64_t RefractionClamp = 1ull << 13;
inline constexpr std::uint64_t MultiTextureLandscape = 1ull << 14;
inline constexpr std::uint64_t Refraction = 1ull << 15;
inline constexpr std::uint64_t RefractionFalloff = 1ull << 16;
inline constexpr std::uint64_t EyeReflection = 1ull << 17;
inline constexpr std::uint64_t HairTint = 1ull << 18;
inline constexpr std::uint64_t ScreenDoorAlphaFade = 1ull << 19;
inline constexpr std::uint64_t LocalMapClear = 1ull << 20;
inline constexpr std::uint64_t FaceGenRgbTint = 1ull << 21;
inline constexpr std::uint64_t OwnEmit = 1ull << 22;
inline constexpr std::uint64_t ProjectedUv = 1ull << 23;
inline constexpr std::uint64_t MultipleTextures = 1ull << 24;
inline constexpr std::uint64_t Tessellate = 1ull << 25;
inline constexpr std::uint64_t Decal = 1ull << 26;
inline constexpr std::uint64_t DynamicDecal = 1ull << 27;
inline constexpr std::uint64_t CharacterLight = 1ull << 28;
inline constexpr std::uint64_t ExternalEmittance = 1ull << 29;
inline constexpr std::uint64_t SoftEffect = 1ull << 30;
inline constexpr std::uint64_t ZBufferTest = 1ull << 31;
inline constexpr std::uint64_t ZBufferWrite = 1ull << 32;
inline constexpr std::uint64_t LodLandscape = 1ull << 33;
inline constexpr std::uint64_t LodObjects = 1ull << 34;
inline constexpr std::uint64_t NoFade = 1ull << 35;
inline constexpr std::uint64_t TwoSided = 1ull << 36;
inline constexpr std::uint64_t VertexColors = 1ull << 37;
inline constexpr std::uint64_t GlowMap = 1ull << 38;
inline constexpr std::uint64_t TransformChanged = 1ull << 39;
inline constexpr std::uint64_t DismembermentMeatCuff = 1ull << 40;
inline constexpr std::uint64_t Tint = 1ull << 41;
inline constexpr std::uint64_t VertexLighting = 1ull << 42;
inline constexpr std::uint64_t UniformScale = 1ull << 43;
inline constexpr std::uint64_t FitSlope = 1ull << 44;
inline constexpr std::uint64_t Billboard = 1ull << 45;
inline constexpr std::uint64_t LodLandBlend = 1ull << 46;
inline constexpr std::uint64_t Dismemberment = 1ull << 47;
inline constexpr std::uint64_t Wireframe = 1ull << 48;
inline constexpr std::uint64_t WeaponBlood = 1ull << 49;
inline constexpr std::uint64_t HideOnLocalMap = 1ull << 50;
inline constexpr std::uint64_t PremultipliedAlpha = 1ull << 51;
inline constexpr std::uint64_t VatsTarget = 1ull << 52;
inline constexpr std::uint64_t AnisotropicLighting = 1ull << 53;
inline constexpr std::uint64_t SkewSpecularAlpha = 1ull << 54;
inline constexpr std::uint64_t MenuScreen = 1ull << 55;
inline constexpr std::uint64_t MultiLayerParallax = 1ull << 56;
inline constexpr std::uint64_t AlphaTest = 1ull << 57;
inline constexpr std::uint64_t InvertedFadePattern = 1ull << 58;
inline constexpr std::uint64_t VatsTargetDrawAll = 1ull << 59;
inline constexpr std::uint64_t PipBoyScreen = 1ull << 60;
inline constexpr std::uint64_t TreeAnimation = 1ull << 61;
inline constexpr std::uint64_t EffectLighting = 1ull << 62;
inline constexpr std::uint64_t RefractionWritesDepth = 1ull << 63;

}

// Recorded texture-role IDs are base 0, normal 1, glow 2, height 3,
// environment 4, wrinkles 5, multilayer 6, and backlight-mask/smooth-spec 7.
// Role 7 is genuinely overloaded, which is why a slot's role is resolved per
// family instead of read from a single flat table.
enum class MaterialSlotRole : std::uint8_t
{
    Unused,
    BaseColor,
    Normal,
    GlowMap,
    Height,
    Environment,
    Wrinkles,
    MultiLayer,
    BacklightMask,
    SmoothSpec,
};

// Broad pipeline classes. Twenty-one families collapse into these; everything
// else that varies rides along as feature data, so pipeline count does not
// grow with content.
enum class ShaderClass : std::uint8_t
{
    Unknown,
    Standard,
    Skin,
    Hair,
    Eye,
    Parallax,
    MultiLayer,
    Terrain,
    Lod,
};

enum class FamilyError : std::uint8_t
{
    None,
    InvalidIdentity,
    NonFiniteSource,
    InvalidEyeTransform,
    InvalidParallaxRange,
    InvalidLayer,
    MissingRequiredSlot,
};

struct FamilySlotCapture
{
    std::uint64_t resourceId{};
    std::uint32_t generation{};
    bool authored{};
};

// What a captured lighting material offers. Scalars carry engine defaults
// that are valid, so a capture that never observed a field is not mistaken
// for one that observed a degenerate value.
struct FamilyCapture
{
    std::uint64_t materialId{};
    std::uint32_t generation{};
    std::uint32_t revision{};
    std::uint32_t staticRevision{};
    std::int32_t featureId{-1};
    std::uint64_t propertyFlags{};
    std::uint32_t baseTechniqueId{};
    std::array<FamilySlotCapture, kShaderTextureSlots> slots{};

    std::array<float, 3> emitColor{};
    float emitScale{1.0f};
    std::array<float, 3> tintColor{1.0f, 1.0f, 1.0f};

    float subsurfaceRolloff{};
    float rimPower{};
    float backlightPower{};

    std::array<float, 3> eyeCenter{};
    float eyeRadius{0.5f};
    float eyeIrisScale{1.0f};

    float parallaxScale{};
    float parallaxBias{};
    std::array<float, 2> parallaxUvScale{1.0f, 1.0f};
    std::uint32_t parallaxMinSteps{4};
    std::uint32_t parallaxMaxSteps{16};

    float layerThickness{};
    float layerRefraction{1.0f};

    std::array<float, kWetnessControls> wetness{};
};

struct FamilyFeatures
{
    bool subsurface{};
    bool rim{};
    bool backlight{};
    bool anisotropy{};
    bool parallaxOffset{};
    bool parallaxOcclusion{};
    bool multiLayer{};
    bool eye{};
    bool environment{};
    bool snow{};
    bool multiIndex{};
    bool wetness{};
    bool landscape{};
    bool treeAnimation{};
    bool dismemberment{};
    bool meatCuff{};
    bool reducedDetail{};
    bool sky{};
};

struct FamilyEmission
{
    bool enabled{};
    bool usesGlowMap{};
    bool externallyDriven{};
    std::array<float, 3> color{};
};

struct FamilyTint
{
    bool enabled{};
    std::array<float, 3> color{};
};

struct FamilyPalette
{
    bool colorFromGreyscale{};
    bool alphaFromGreyscale{};
};

struct FamilySubsurface
{
    float rolloff{};
    float rimPower{};
    float backlightPower{};
};

struct FamilyEye
{
    std::array<float, 3> center{};
    float radius{};
    float irisScale{};
    bool reflects{};
};

struct FamilyParallax
{
    float scale{};
    float bias{};
    std::array<float, 2> uvScale{};
    std::uint32_t minimumSteps{};
    std::uint32_t maximumSteps{};
};

struct FamilyLayer
{
    float thickness{};
    float refraction{};
};

struct FamilyWetness
{
    std::array<float, kWetnessControls> controls{};
};

struct FamilySlot
{
    MaterialSlotRole role{MaterialSlotRole::Unused};
    std::uint64_t resourceId{};
    std::uint32_t generation{};
};

// Why a descriptor looks the way it does, so an unexpected render can be
// traced to the capture that produced it rather than guessed at.
struct FamilyDiagnostic
{
    std::int32_t capturedFeatureId{-1};
    std::uint64_t capturedFlags{};
    std::uint32_t unclassifiedAuthoredSlots{};
    std::uint32_t baseTechniqueId{};
};

struct FamilyDescriptor
{
    std::uint64_t materialId{};
    std::uint32_t generation{};
    std::uint32_t revision{};
    std::uint32_t staticRevision{};
    MaterialFamily family{MaterialFamily::Unknown};
    ShaderClass shaderClass{ShaderClass::Unknown};
    MaterialProvenance provenance{MaterialProvenance::Missing};
    MaterialNormalEncoding normalEncoding{
        MaterialNormalEncoding::TangentSpaceBc5};
    bool usedFallback{};
    std::array<FamilySlot, kShaderTextureSlots> slots{};
    FamilyFeatures features{};
    FamilyEmission emission{};
    FamilyTint tint{};
    FamilyPalette palette{};
    FamilySubsurface subsurface{};
    FamilyEye eye{};
    FamilyParallax parallax{};
    FamilyLayer layer{};
    FamilyWetness wetness{};
    FamilyDiagnostic diagnostic{};
};

// GPU feature bits, mirrored in scene_layout.glsl. Declared here rather than
// derived from the struct order so a reordered field cannot silently change
// what a bit means.
enum GpuFamilyFeature : std::uint32_t
{
    GpuFeatureSubsurface = 1u << 0,
    GpuFeatureRim = 1u << 1,
    GpuFeatureBacklight = 1u << 2,
    GpuFeatureAnisotropy = 1u << 3,
    GpuFeatureParallaxOffset = 1u << 4,
    GpuFeatureParallaxOcclusion = 1u << 5,
    GpuFeatureMultiLayer = 1u << 6,
    GpuFeatureEye = 1u << 7,
    GpuFeatureEnvironment = 1u << 8,
    GpuFeatureSnow = 1u << 9,
    GpuFeatureMultiIndex = 1u << 10,
    GpuFeatureWetness = 1u << 11,
    GpuFeatureLandscape = 1u << 12,
    GpuFeatureTreeAnimation = 1u << 13,
    GpuFeatureDismemberment = 1u << 14,
    GpuFeatureMeatCuff = 1u << 15,
    GpuFeatureReducedDetail = 1u << 16,
    GpuFeatureSky = 1u << 17,
    // Whether slot 1 actually resolved to a bound normal map. Without it the
    // shader cannot tell an unbound slot from one holding a flat texture,
    // and would replace the object's shading normal with a decoded constant.
    GpuFeatureNormalMap = 1u << 18,
};

enum GpuFamilyEmission : std::uint32_t
{
    GpuEmissionEnabled = 1u << 0,
    GpuEmissionGlowMap = 1u << 1,
    GpuEmissionExternal = 1u << 2,
};

enum GpuFamilyPalette : std::uint32_t
{
    GpuPaletteColor = 1u << 0,
    GpuPaletteAlpha = 1u << 1,
};

struct alignas(16) GpuFamilyRecordV1
{
    // Family in the low byte, shader class in the next, normal encoding in
    // the third. One word so the shader reads a single dword for dispatch.
    std::uint32_t familyPacked{};
    std::uint32_t featureFlags{};
    std::uint32_t emissionFlags{};
    std::uint32_t paletteFlags{};
    float emissionColor[4]{};
    // w carries the tint enable, so a zero tint colour and an absent tint
    // are distinguishable.
    float tintColor[4]{};
    float subsurface[4]{};
    float parallax[4]{};
    float eyeCenterRadius[4]{};
    float layerAndEye[4]{};
    float wetnessLow[4]{};
    float wetnessHigh[4]{};
};

[[nodiscard]] MaterialFamily ClassifyMaterialFamily(
    std::int32_t featureId) noexcept;
// Inverse of the classification, for tests and for round-tripping a captured
// family back into the engine's own numbering.
[[nodiscard]] std::int32_t FeatureIdOf(MaterialFamily family) noexcept;
[[nodiscard]] ShaderClass ShaderClassOf(MaterialFamily family) noexcept;
[[nodiscard]] FamilyError TranslateMaterialFamily(
    const FamilyCapture& capture,
    FamilyDescriptor& descriptor) noexcept;
// A static change is one that invalidates a descriptor set: a rebound
// texture, a different family, a different slot layout. Everything else is a
// dynamic value update, which is what keeps a rain transition from rebuilding
// descriptors every frame.
[[nodiscard]] bool RequiresDescriptorRebuild(
    const FamilyDescriptor& previous,
    const FamilyDescriptor& next) noexcept;
[[nodiscard]] bool RequiresDynamicUpdate(
    const FamilyDescriptor& previous,
    const FamilyDescriptor& next) noexcept;
inline constexpr std::uint32_t kFamilyPacketMagic = 0x4D46'4656u; // "VFFM"
inline constexpr std::uint16_t kFamilyPacketVersionMajor = 1;
inline constexpr std::uint16_t kFamilyPacketVersionMinor = 0;
inline constexpr std::uint32_t kFamilyPacketEndian = 0x01020304u;
inline constexpr std::uint32_t kMaximumFamilyRecords = 65'536;

enum class FamilyPacketError : std::uint8_t
{
    None,
    TruncatedHeader,
    BadMagic,
    UnsupportedVersion,
    WrongEndian,
    SizeMismatch,
    ChecksumMismatch,
    SectionOutOfBounds,
    MisalignedSection,
    NonZeroPadding,
    TooManyRecords,
    InvalidIdentity,
    DuplicateRecord,
    UnclassifiedRecord,
    NonFiniteValue,
    InvalidSlotRole,
    AllocationFailure,
};

// The wire form of one resolved family. It is laid out explicitly rather
// than serialized from FamilyDescriptor, so a compiler's packing choice can
// never change what a captured artifact means.
struct alignas(16) FamilySlotV1
{
    std::uint64_t resourceId{};
    std::uint32_t generation{};
    std::uint8_t role{};
    std::uint8_t reserved0[3]{};
};

struct alignas(16) FamilyRecordV1
{
    std::uint64_t materialId{};
    std::uint64_t objectId{};
    std::uint32_t generation{};
    std::uint32_t revision{};
    std::uint32_t staticRevision{};
    std::uint32_t featureFlags{};
    std::uint32_t emissionFlags{};
    std::uint32_t paletteFlags{};
    std::uint8_t family{};
    std::uint8_t shaderClass{};
    std::uint8_t provenance{};
    std::uint8_t normalEncoding{};
    std::uint8_t usedFallback{};
    std::uint8_t reserved0[3]{};
    std::int32_t capturedFeatureId{};
    std::uint32_t baseTechniqueId{};
    std::uint32_t unclassifiedAuthoredSlots{};
    std::uint32_t reserved1{};
    std::uint64_t capturedFlags{};
    std::uint64_t reserved2{};
    float emissionColor[4]{};
    float tintColor[4]{};
    float subsurface[4]{};
    float parallax[4]{};
    float eyeCenterRadius[4]{};
    float layerAndEye[4]{};
    float wetnessLow[4]{};
    float wetnessHigh[4]{};
    FamilySlotV1 slots[kShaderTextureSlots]{};
};

struct alignas(8) FamilyPacketHeaderV1
{
    std::uint32_t magic{kFamilyPacketMagic};
    std::uint16_t versionMajor{kFamilyPacketVersionMajor};
    std::uint16_t versionMinor{kFamilyPacketVersionMinor};
    std::uint32_t headerSize{sizeof(FamilyPacketHeaderV1)};
    std::uint32_t totalSize{};
    std::uint32_t payloadCrc32{};
    std::uint32_t endianMarker{kFamilyPacketEndian};
    std::uint64_t frameId{};
    std::uint64_t viewId{};
    std::uint32_t recordCount{};
    std::uint32_t recordsOffset{};
    // Two reserved words, which round the header to 64 bytes and leave room
    // for a later minor to append without moving the prefix.
    std::uint64_t reserved0{};
    std::uint64_t reserved1{};
};

struct FamilyPacket
{
    FamilyPacketHeaderV1 header{};
    std::vector<FamilyRecordV1> records;
};

[[nodiscard]] FamilyRecordV1 MakeFamilyRecord(
    const FamilyDescriptor& descriptor,
    std::uint64_t objectId) noexcept;
[[nodiscard]] FamilyPacketError ValidateFamilyPacket(
    const FamilyPacket& packet) noexcept;
[[nodiscard]] FamilyPacketError EncodeFamilyPacket(
    const FamilyPacket& packet,
    std::vector<std::byte>& bytes) noexcept;
[[nodiscard]] FamilyPacketError DecodeFamilyPacket(
    std::span<const std::byte> bytes,
    FamilyPacket& packet) noexcept;
// An object with no captured family resolves to the ordinary lit surface,
// so every consumer sees one rule and the binding is always valid. This is
// the same shape as scene::ResolveVisibility.
[[nodiscard]] FamilyRecordV1 ResolveFamilyRecord(
    const FamilyPacket& packet,
    std::uint64_t objectId) noexcept;
[[nodiscard]] GpuFamilyRecordV1 BuildFamilyGpuRecord(
    const FamilyRecordV1& record) noexcept;
[[nodiscard]] GpuFamilyRecordV1 BuildFamilyGpuRecord(
    const FamilyDescriptor& descriptor) noexcept;
[[nodiscard]] const char* ToString(FamilyPacketError error) noexcept;
[[nodiscard]] const char* ToString(FamilyError error) noexcept;
[[nodiscard]] const char* ToString(MaterialFamily family) noexcept;

static_assert(sizeof(FamilySlotV1) == 16);
static_assert(sizeof(FamilyRecordV1) == 368);
static_assert(offsetof(FamilyRecordV1, emissionColor) == 80);
static_assert(offsetof(FamilyRecordV1, tintColor) == 96);
static_assert(offsetof(FamilyRecordV1, wetnessHigh) == 192);
static_assert(offsetof(FamilyRecordV1, slots) == 208);
static_assert(sizeof(FamilyPacketHeaderV1) == 64);
static_assert(offsetof(FamilyPacketHeaderV1, recordCount) == 40);
static_assert(offsetof(FamilyPacketHeaderV1, recordsOffset) == 44);
static_assert(sizeof(GpuFamilyRecordV1) == 144);
static_assert(sizeof(GpuFamilyRecordV1) == scene::kGpuFamilyRecordSize);
static_assert(offsetof(GpuFamilyRecordV1, emissionColor) == 16);
static_assert(offsetof(GpuFamilyRecordV1, tintColor) == 32);
static_assert(offsetof(GpuFamilyRecordV1, subsurface) == 48);
static_assert(offsetof(GpuFamilyRecordV1, parallax) == 64);
static_assert(offsetof(GpuFamilyRecordV1, eyeCenterRadius) == 80);
static_assert(offsetof(GpuFamilyRecordV1, layerAndEye) == 96);
static_assert(offsetof(GpuFamilyRecordV1, wetnessLow) == 112);
static_assert(offsetof(GpuFamilyRecordV1, wetnessHigh) == 128);

}
