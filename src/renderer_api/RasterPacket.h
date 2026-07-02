#pragma once

#include "RasterShaderLayout.generated.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::raster {

constexpr std::uint32_t kPacketMagic = 0x54504656u; // "VFPT"
constexpr std::uint16_t kPacketVersionMajor = 1;
constexpr std::uint16_t kPacketPhase6VersionMinor = 0;
// Texture coordinates joined the vertex here.
constexpr std::uint16_t kPacketTexCoordVersionMinor = 1;
// And the shading normal here. A new field is a new minor version: encoding
// forty-eight-byte vertices under the old number hands a reader a stride it
// will walk at thirty-two, which is not a decode failure but a scene of
// garbage.
constexpr std::uint16_t kPacketVertexNormalVersionMinor = 2;
constexpr std::uint16_t kPacketVersionMinor = kPacketVertexNormalVersionMinor;
constexpr std::uint32_t kMaximumExtent = 16384;

enum class IndexType : std::uint32_t
{
    Uint16 = 2,
    Uint32 = 4
};

// Which space a packet's vertex positions are already in. Winding is
// classified from the XY signed area, which is only a winding once the
// vertices lie in the screen plane; camera-relative world geometry acquires
// its winding from the view transform, and any real mesh contains triangles
// standing edge-on in XY that a screen-plane rule would reject outright.
enum class VertexSpace : std::uint32_t
{
    ScreenNdc = 0,
    CameraRelativeWorld = 1,
};

enum class FrontFace : std::uint32_t
{
    CounterClockwise,
    Clockwise
};

enum class DepthCompare : std::uint32_t
{
    Less,
    LessOrEqual,
    Always
};

enum class TriangleWinding : std::uint8_t
{
    Degenerate,
    CounterClockwise,
    Clockwise
};

enum class PacketError : std::uint8_t
{
    None,
    NotImplemented,
    TruncatedHeader,
    BadMagic,
    UnsupportedVersion,
    SizeMismatch,
    InvalidExtent,
    InvalidViewport,
    InvalidScissor,
    InvalidIndexType,
    InvalidVertexSpace,
    MisalignedSection,
    SectionOutOfBounds,
    InvalidDrawRange,
    IndexOutOfRange,
    MissingMaterial,
    ShaderLayoutMismatch,
    DegenerateTriangle,
    AllocationFailure,
    DuplicateResource,
    InvalidVertex
};

struct alignas(8) PacketHeaderV1
{
    std::uint32_t magic{kPacketMagic};
    std::uint16_t versionMajor{kPacketVersionMajor};
    std::uint16_t versionMinor{kPacketVersionMinor};
    std::uint32_t headerSize{sizeof(PacketHeaderV1)};
    std::uint32_t totalSize{};
    std::uint64_t frameIndex{};
    std::uint32_t width{};
    std::uint32_t height{};
    float viewportX{};
    float viewportY{};
    float viewportWidth{};
    float viewportHeight{};
    float viewportMinDepth{};
    float viewportMaxDepth{1.0f};
    std::int32_t scissorX{};
    std::int32_t scissorY{};
    std::uint32_t scissorWidth{};
    std::uint32_t scissorHeight{};
    IndexType indexType{IndexType::Uint16};
    std::uint32_t vertexCount{};
    std::uint32_t indexCount{};
    std::uint32_t drawCount{};
    std::uint32_t materialCount{};
    std::uint32_t verticesOffset{};
    std::uint32_t indicesOffset{};
    std::uint32_t drawsOffset{};
    std::uint32_t materialsOffset{};
    std::uint64_t shaderLayoutHash{kPhase6ShaderLayoutHash};
    // Which space the vertex positions are already in. Carved out of the
    // former reserved block, so the header size does not move and every
    // packet encoded before this field existed keeps its exact meaning.
    VertexSpace vertexSpace{VertexSpace::ScreenNdc};
    std::uint32_t reserved0{};
    std::uint64_t reserved[3]{};
};

struct RasterVertexV1
{
    float position[3]{};
    float color[3]{};
};

struct RasterVertexV2
{
    float position[3]{};
    float color[3]{};
    float texCoord[2]{};

    RasterVertexV2() = default;
    RasterVertexV2(const RasterVertexV1& source) noexcept;
};

// The shading normal, per vertex. Without it every surface shades as though it
// faced the same way, which reads as a lighting fault rather than as absent
// data -- and it makes the shadow, reflection and indirect phases meaningless
// on captured geometry, because every one of them starts from N.
//
// Padded to sixteen-byte alignment rather than packed to thirty-six, so the
// stride matches what the vertex shader reads without a per-attribute offset
// that has to be kept in step by hand.
struct alignas(16) RasterVertexV3
{
    float position[3]{};
    float color[3]{};
    float texCoord[2]{};
    // Defaults to +Z rather than to zero. A zero normal is not "no lighting":
    // it is a division by zero wherever the shading normalises it, and the
    // result is a NaN that spreads through the whole frame.
    float normal[3]{0.0f, 0.0f, 1.0f};
    float pad{};

    RasterVertexV3() = default;
    RasterVertexV3(const RasterVertexV1& source) noexcept;
    RasterVertexV3(const RasterVertexV2& source) noexcept;
};

struct alignas(16) RasterMaterialV1
{
    std::uint64_t resourceId{};
    std::uint64_t shaderLayoutHash{kPhase6ShaderLayoutHash};
    float baseColor[4]{1.0f, 1.0f, 1.0f, 1.0f};
};

struct alignas(8) RasterDrawV1
{
    std::uint64_t materialId{};
    std::uint32_t firstIndex{};
    std::uint32_t indexCount{};
    std::int32_t vertexOffset{};
    FrontFace frontFace{FrontFace::CounterClockwise};
    DepthCompare depthCompare{DepthCompare::Less};
    std::uint32_t reserved{};
};

struct PacketResult
{
    PacketError error{PacketError::None};
    std::size_t byteOffset{};
    std::uint64_t resourceId{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == PacketError::None;
    }
};

struct DecodedPacket
{
    PacketHeaderV1 header{};
    std::vector<RasterVertexV3> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<RasterDrawV1> draws;
    std::vector<RasterMaterialV1> materials;
};

struct SyntheticPacketOptions
{
    std::uint32_t width{96};
    std::uint32_t height{64};
    std::uint64_t frameIndex{1};
    IndexType indexType{IndexType::Uint16};
    FrontFace frontFace{FrontFace::CounterClockwise};
    DepthCompare depthCompare{DepthCompare::Less};
    bool reverseWinding{};
    bool includeOccludedTriangle{};
};

class MaterialRegistry
{
public:
    explicit MaterialRegistry(
        std::span<const RasterMaterialV1> materials) noexcept;
    [[nodiscard]] const RasterMaterialV1* Resolve(
        std::uint64_t resourceId) const noexcept;
    [[nodiscard]] std::size_t IndexOf(
        std::uint64_t resourceId) const noexcept;
    [[nodiscard]] bool HasDuplicateIds() const noexcept;

private:
    std::span<const RasterMaterialV1> materials_;
};

[[nodiscard]] std::vector<std::byte> BuildSyntheticPacket(
    const SyntheticPacketOptions& options = {});
[[nodiscard]] PacketResult EncodePacket(
    const DecodedPacket& packet,
    std::vector<std::byte>& bytes) noexcept;
[[nodiscard]] PacketResult DecodePacket(
    std::span<const std::byte> bytes,
    DecodedPacket& decoded) noexcept;
[[nodiscard]] TriangleWinding ClassifyTriangle(
    const RasterVertexV1& a,
    const RasterVertexV1& b,
    const RasterVertexV1& c) noexcept;
[[nodiscard]] TriangleWinding ClassifyTriangle(
    const RasterVertexV3& a,
    const RasterVertexV3& b,
    const RasterVertexV3& c) noexcept;
[[nodiscard]] const char* ToString(PacketError error) noexcept;

class ExtentGeneration
{
public:
    [[nodiscard]] bool Update(std::uint32_t width, std::uint32_t height) noexcept;
    [[nodiscard]] std::uint64_t Generation() const noexcept;
    [[nodiscard]] std::uint32_t Width() const noexcept;
    [[nodiscard]] std::uint32_t Height() const noexcept;

private:
    std::uint32_t width_{};
    std::uint32_t height_{};
    std::uint64_t generation_{};
};

struct UploadAllocation
{
    std::size_t offset{};
    std::size_t size{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return size != 0;
    }
};

class UploadArena
{
public:
    explicit UploadArena(std::size_t capacity);
    [[nodiscard]] UploadAllocation Allocate(
        std::size_t size,
        std::size_t alignment) noexcept;
    void Reset() noexcept;
    [[nodiscard]] std::size_t Used() const noexcept;
    [[nodiscard]] std::size_t Capacity() const noexcept;

private:
    std::size_t capacity_{};
    std::size_t used_{};
};

// The smallest maxMemoryAllocationSize any conformant Vulkan implementation is
// permitted to report. Used as the floor when a device reports nothing, and as
// the floor a device is not allowed to undercut.
constexpr std::uint64_t kMinimumDeviceAllocationBytes = 1ull << 30;

// The largest packet the renderer will accept, given what the device it is
// uploading to says it can allocate at once. A packet has to fit in a single
// device allocation, so the device's own maximum is the only ceiling here that
// is neither arbitrary nor a guess -- and the ceiling has to exist, because it
// is applied before the packet is decoded and the decoder would otherwise
// reserve host memory on the strength of a caller-supplied count.
[[nodiscard]] std::uint64_t MaximumPacketBytes(
    std::uint64_t reportedDeviceAllocationBytes) noexcept;

static_assert(sizeof(RasterVertexV1) == 24);
static_assert(sizeof(RasterVertexV2) == 32);
static_assert(sizeof(RasterVertexV3) == 48);
static_assert(alignof(RasterVertexV3) == 16);
static_assert(sizeof(RasterMaterialV1) == 32);
static_assert(sizeof(RasterDrawV1) == 32);

}
