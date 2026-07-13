#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::mesh {

constexpr std::uint32_t kMeshPacketMagic = 0x534D4656u; // "VFMS"
constexpr std::uint16_t kMeshPacketVersionMajor = 1;
constexpr std::uint16_t kMeshPacketVersionMinor = 0;

enum class MeshUsage : std::uint32_t
{
    Immutable,
    Dynamic
};

enum class MeshPacketError : std::uint8_t
{
    None,
    TruncatedHeader,
    BadMagic,
    UnsupportedVersion,
    SizeMismatch,
    InvalidResource,
    InvalidUsage,
    InvalidLayout,
    InvalidVertexPayload,
    MisalignedSection,
    SectionOutOfBounds,
    ChecksumMismatch,
    InvalidDrawRange,
    VertexOutOfRange,
    NonFiniteVertex,
    DegenerateTopology,
    InvalidExtent,
    AllocationFailure,
    RasterPacketRejected
};

struct alignas(8) MeshPacketHeaderV1
{
    std::uint32_t magic{kMeshPacketMagic};
    std::uint16_t versionMajor{kMeshPacketVersionMajor};
    std::uint16_t versionMinor{kMeshPacketVersionMinor};
    std::uint32_t headerSize{sizeof(MeshPacketHeaderV1)};
    std::uint32_t totalSize{};
    std::uint64_t resourceId{};
    std::uint32_t generation{};
    MeshUsage usage{MeshUsage::Immutable};
    std::uint64_t vertexDesc{};
    std::uint32_t stride{};
    std::uint32_t vertexCount{};
    std::uint32_t indexCount{};
    std::uint32_t firstIndex{};
    std::uint32_t drawIndexCount{};
    std::int32_t baseVertex{};
    std::uint32_t vertexBytesOffset{};
    std::uint32_t vertexBytesSize{};
    std::uint32_t indicesOffset{};
    std::uint32_t indicesSize{};
    std::uint32_t payloadCrc32{};
    std::uint32_t reserved32{};
    std::uint64_t reserved[3]{};
};

struct CapturedMesh
{
    std::uint64_t resourceId{};
    std::uint32_t generation{};
    MeshUsage usage{MeshUsage::Immutable};
    std::uint64_t vertexDesc{};
    std::uint32_t stride{};
    std::uint32_t vertexCount{};
    std::vector<std::byte> vertexBytes;
    std::vector<std::uint16_t> indices;
    std::uint32_t firstIndex{};
    std::uint32_t indexCount{};
    std::int32_t baseVertex{};
};

struct MeshBounds
{
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
};

struct MeshTranslationReport
{
    MeshBounds sourceBounds{};
    std::uint32_t translatedVertexCount{};
    std::uint32_t translatedIndexCount{};
    std::uint32_t sourceAttributeCount{};
    bool clockwise{};
};

[[nodiscard]] MeshPacketError EncodeCapturedMesh(
    const CapturedMesh& mesh,
    std::vector<std::byte>& bytes) noexcept;
[[nodiscard]] MeshPacketError DecodeCapturedMesh(
    std::span<const std::byte> bytes,
    CapturedMesh& mesh) noexcept;
[[nodiscard]] MeshPacketError TranslateCapturedMesh(
    const CapturedMesh& mesh,
    std::uint32_t width,
    std::uint32_t height,
    std::vector<std::byte>& rasterPacket,
    MeshTranslationReport& report) noexcept;
[[nodiscard]] const char* ToString(MeshPacketError error) noexcept;

static_assert(sizeof(MeshPacketHeaderV1) == 112);

}
