#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::texture {

constexpr std::uint32_t kTexturePacketMagic = 0x58544656u; // "VFTX"
constexpr std::uint16_t kTexturePacketVersionMajor = 1;
constexpr std::uint16_t kTexturePacketVersionMinor = 0;

// Values intentionally match DXGI_FORMAT so the capture boundary does not
// need a lossy engine- or D3D-specific translation table.
enum class TextureFormat : std::uint32_t
{
    Unknown = 0,
    R8G8B8A8Typeless = 27,
    R8G8B8A8Unorm = 28,
    R8G8B8A8UnormSrgb = 29,
    R8G8Typeless = 48,
    R8G8Unorm = 49,
    R8Typeless = 60,
    R8Unorm = 61,
    BC1Typeless = 70,
    BC1Unorm = 71,
    BC1UnormSrgb = 72,
    BC2Typeless = 73,
    BC2Unorm = 74,
    BC2UnormSrgb = 75,
    BC3Typeless = 76,
    BC3Unorm = 77,
    BC3UnormSrgb = 78,
    BC4Typeless = 79,
    BC4Unorm = 80,
    BC4Snorm = 81,
    BC5Typeless = 82,
    BC5Unorm = 83,
    BC5Snorm = 84,
    B8G8R8A8Unorm = 87,
    B8G8R8A8Typeless = 90,
    B8G8R8A8UnormSrgb = 91,
    BC6HTypeless = 94,
    BC6HUf16 = 95,
    BC6HSf16 = 96,
    BC7Typeless = 97,
    BC7Unorm = 98,
    BC7UnormSrgb = 99,
};

enum class TextureFamily : std::uint8_t
{
    Unknown,
    R8,
    RG8,
    RGBA8,
    BGRA8,
    BC1,
    BC2,
    BC3,
    BC4,
    BC5,
    BC6H,
    BC7,
};

enum class TextureDimension : std::uint32_t
{
    Texture2D,
    Texture2DArray,
    Cube,
};

enum class TextureFilter : std::uint32_t
{
    Nearest,
    Linear,
};

enum class TextureAddressMode : std::uint32_t
{
    Wrap,
    Mirror,
    Clamp,
    Border,
    MirrorOnce,
};

enum class TextureCompareOp : std::uint32_t
{
    Never,
    Less,
    Equal,
    LessOrEqual,
    Greater,
    NotEqual,
    GreaterOrEqual,
    Always,
};

enum class TexturePacketError : std::uint8_t
{
    None,
    TruncatedHeader,
    BadMagic,
    UnsupportedVersion,
    SizeMismatch,
    InvalidResource,
    UnsupportedFormat,
    IllegalViewFormat,
    InvalidDimension,
    InvalidExtent,
    InvalidMipRange,
    InvalidSampler,
    InvalidSubresource,
    InvalidPitch,
    MisalignedSection,
    SectionOutOfBounds,
    ChecksumMismatch,
    AllocationFailure,
    UnsupportedSampling,
};

struct TextureFormatInfo
{
    TextureFamily family{TextureFamily::Unknown};
    std::uint32_t blockWidth{1};
    std::uint32_t blockHeight{1};
    std::uint32_t bytesPerBlock{};
    bool compressed{};
    bool srgb{};
    bool signedData{};
};

struct TextureFootprint
{
    std::uint32_t rowBytes{};
    std::uint32_t rowCount{};
    std::uint64_t byteSize{};
};

struct TextureSamplerDesc
{
    TextureFilter minFilter{TextureFilter::Linear};
    TextureFilter magFilter{TextureFilter::Linear};
    TextureFilter mipFilter{TextureFilter::Linear};
    TextureAddressMode addressU{TextureAddressMode::Wrap};
    TextureAddressMode addressV{TextureAddressMode::Wrap};
    TextureAddressMode addressW{TextureAddressMode::Wrap};
    float mipLodBias{};
    float maxAnisotropy{1.0f};
    float minLod{};
    float maxLod{1000.0f};
    std::uint32_t anisotropyEnable{};
    std::uint32_t comparisonEnable{};
    TextureCompareOp compareOp{TextureCompareOp::Always};
    std::uint32_t reserved{};
    float borderColor[4]{};

    friend bool operator==(const TextureSamplerDesc&,
        const TextureSamplerDesc&) = default;
};

struct TextureSubresource
{
    std::uint32_t mipLevel{};
    std::uint32_t arrayLayer{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t rowPitch{};
    std::uint32_t slicePitch{};
    std::vector<std::byte> bytes;
};

struct CapturedTexture
{
    std::uint64_t resourceId{};
    std::uint32_t generation{};
    TextureDimension dimension{TextureDimension::Texture2D};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{1};
    std::uint32_t arrayLayers{1};
    std::uint32_t mipLevels{1};
    TextureFormat resourceFormat{TextureFormat::Unknown};
    TextureFormat viewFormat{TextureFormat::Unknown};
    std::uint32_t residentBaseMip{};
    std::uint32_t residentMipCount{1};
    TextureSamplerDesc sampler{};
    std::vector<TextureSubresource> subresources;
};

struct alignas(8) TexturePacketHeaderV1
{
    std::uint32_t magic{kTexturePacketMagic};
    std::uint16_t versionMajor{kTexturePacketVersionMajor};
    std::uint16_t versionMinor{kTexturePacketVersionMinor};
    std::uint32_t headerSize{sizeof(TexturePacketHeaderV1)};
    std::uint32_t totalSize{};
    std::uint64_t resourceId{};
    std::uint32_t generation{};
    TextureDimension dimension{TextureDimension::Texture2D};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{1};
    std::uint32_t arrayLayers{1};
    std::uint32_t mipLevels{1};
    TextureFormat resourceFormat{TextureFormat::Unknown};
    TextureFormat viewFormat{TextureFormat::Unknown};
    std::uint32_t residentBaseMip{};
    std::uint32_t residentMipCount{1};
    std::uint32_t subresourceCount{};
    std::uint32_t subresourcesOffset{};
    std::uint32_t payloadOffset{};
    std::uint32_t payloadSize{};
    std::uint32_t payloadCrc32{};
    std::uint32_t reserved32{};
    TextureSamplerDesc sampler{};
    std::uint32_t samplerPadding{};
    std::uint64_t reserved[4]{};
};

struct TextureSubresourceV1
{
    std::uint32_t mipLevel{};
    std::uint32_t arrayLayer{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t rowPitch{};
    std::uint32_t slicePitch{};
    std::uint32_t payloadOffset{};
    std::uint32_t payloadSize{};
};

enum class FallbackTextureRole : std::uint8_t
{
    White,
    Black,
    FlatNormal,
    NeutralMask,
};

struct SampledColor
{
    float r{};
    float g{};
    float b{};
    float a{1.0f};
};

[[nodiscard]] TexturePacketError ResolveTextureFormat(
    TextureFormat resourceFormat,
    TextureFormat viewFormat,
    TextureFormatInfo& info) noexcept;
[[nodiscard]] TexturePacketError ComputeTextureFootprint(
    TextureFormat viewFormat,
    std::uint32_t width,
    std::uint32_t height,
    TextureFootprint& footprint) noexcept;
[[nodiscard]] TexturePacketError NormalizeSampler(
    const TextureSamplerDesc& input,
    TextureSamplerDesc& normalized) noexcept;
[[nodiscard]] TexturePacketError RepackTextureRows(
    TextureFormat viewFormat,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t sourceRowPitch,
    std::span<const std::byte> source,
    std::vector<std::byte>& tightlyPacked) noexcept;
// A frame's worth of textures, framed as one payload. A scene draws from many
// materials and the renderer took exactly one texture per frame, so every
// surface sampled the same fallback. Engine textures differ in size and in
// format, so they cannot share one array image: they travel as a list and are
// bound through a descriptor array, and a material carries its position in
// this list. Order is therefore part of the contract, not an implementation
// detail.
inline constexpr std::uint32_t kTextureLibraryMagic = 0x4C544656u; // "VFTL"
inline constexpr std::uint16_t kTextureLibraryVersionMajor = 1;
inline constexpr std::uint16_t kTextureLibraryVersionMinor = 0;

[[nodiscard]] TexturePacketError EncodeTextureLibrary(
    std::span<const CapturedTexture> textures,
    std::vector<std::byte>& bytes) noexcept;
// The same encoding from textures the caller already holds elsewhere.
//
// A library is a hundred and thirty megabytes of texel data. Gathering it into
// a contiguous vector first copies all of it purely to satisfy the span, and
// the encoder then copies it again -- so the value overload costs one whole
// extra pass over the library every time a single texture is added. A null
// entry is rejected as InvalidResource rather than skipped, so a caller cannot
// silently shorten its own library and shift every index after the hole.
[[nodiscard]] TexturePacketError EncodeTextureLibrary(
    std::span<const CapturedTexture* const> textures,
    std::vector<std::byte>& bytes) noexcept;
[[nodiscard]] TexturePacketError DecodeTextureLibrary(
    std::span<const std::byte> bytes,
    std::vector<CapturedTexture>& textures) noexcept;

[[nodiscard]] TexturePacketError EncodeCapturedTexture(
    const CapturedTexture& texture,
    std::vector<std::byte>& bytes) noexcept;
[[nodiscard]] TexturePacketError DecodeCapturedTexture(
    std::span<const std::byte> bytes,
    CapturedTexture& texture) noexcept;
[[nodiscard]] TexturePacketError SampleTexture2D(
    const CapturedTexture& texture,
    float u,
    float v,
    float lod,
    SampledColor& color) noexcept;
[[nodiscard]] TexturePacketError SampleTexture2DArray(
    const CapturedTexture& texture,
    float u,
    float v,
    std::uint32_t arrayLayer,
    float lod,
    SampledColor& color) noexcept;
[[nodiscard]] TexturePacketError SampleTextureCube(
    const CapturedTexture& texture,
    float directionX,
    float directionY,
    float directionZ,
    float lod,
    SampledColor& color) noexcept;
[[nodiscard]] CapturedTexture MakeFallbackTexture(
    FallbackTextureRole role);
[[nodiscard]] const char* ToString(TexturePacketError error) noexcept;

static_assert(sizeof(TextureSamplerDesc) == 72);
static_assert(sizeof(TextureSubresourceV1) == 32);
static_assert(sizeof(TexturePacketHeaderV1) == 200);

}
