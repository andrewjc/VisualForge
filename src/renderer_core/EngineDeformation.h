#pragma once

#include "DeformShaderLayout.generated.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace vf::renderer::deform {

inline constexpr std::uint32_t kDeformPacketMagic = 0x46444656u; // "VFDF"
inline constexpr std::uint16_t kDeformPacketVersionMajor = 1;
inline constexpr std::uint16_t kDeformPacketVersionMinor = 0;
inline constexpr std::uint32_t kDeformPacketEndian = 0x01020304u;
inline constexpr std::uint32_t kInfluencesPerVertex = 4;
inline constexpr std::uint32_t kMaximumDeformBones = 1'024;
inline constexpr std::uint32_t kMaximumDeformVertices = 262'144;
inline constexpr std::uint32_t kMaximumMorphTargets = 256;
inline constexpr std::uint32_t kMaximumMorphDeltas = 1'048'576;

enum class DeformError : std::uint8_t
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
    WrongThread,
    InvalidIdentity,
    InvalidFlags,
    InvalidWeights,
    InvalidBoneIndex,
    InvalidMatrix,
    InvalidMorph,
    InvalidWind,
    InvalidBase,
    TopologyMismatch,
    GenerationMismatch,
    AllocationFailure,
};

struct alignas(16) DeformVertexV1
{
    std::uint32_t bones[kInfluencesPerVertex]{};
    float weights[kInfluencesPerVertex]{};
    float flexibility{};
    float reserved[3]{};
};

struct alignas(16) BoneTransformV1
{
    // Row-major affine 3x4. The fourth row is always (0, 0, 0, 1).
    float rows[12]{};
};

struct alignas(16) MorphTargetV1
{
    std::uint32_t firstDelta{};
    std::uint32_t deltaCount{};
    float weight{};
    float previousWeight{};
};

struct alignas(16) MorphDeltaV1
{
    std::uint32_t vertexIndex{};
    std::uint32_t reserved[3]{};
    float delta[4]{};
};

struct alignas(16) WindParametersV1
{
    float direction[4]{};
    float amplitude{};
    float frequency{};
    float time{};
    float previousTime{};
};

struct alignas(16) DeformPacketHeaderV1
{
    std::uint32_t magic{kDeformPacketMagic};
    std::uint16_t versionMajor{kDeformPacketVersionMajor};
    std::uint16_t versionMinor{kDeformPacketVersionMinor};
    std::uint32_t headerSize{};
    std::uint32_t totalSize{};
    std::uint32_t payloadCrc32{};
    std::uint32_t endianMarker{kDeformPacketEndian};
    std::uint64_t topologyId{};
    std::uint64_t captureSequence{};
    std::uint32_t captureThreadId{};
    std::uint32_t renderThreadId{};
    std::uint32_t generation{};
    std::uint32_t flags{};
    std::uint32_t vertexCount{};
    std::uint32_t boneCount{};
    std::uint32_t morphTargetCount{};
    std::uint32_t morphDeltaCount{};
    std::uint32_t verticesOffset{};
    std::uint32_t bonesOffset{};
    std::uint32_t previousBonesOffset{};
    std::uint32_t morphTargetsOffset{};
    std::uint32_t morphDeltasOffset{};
    std::uint32_t reserved0{};
    WindParametersV1 wind{};
    std::uint64_t reserved[2]{};
};

struct DeformationPacket
{
    DeformPacketHeaderV1 header{};
    std::vector<DeformVertexV1> vertices;
    std::vector<BoneTransformV1> bones;
    std::vector<BoneTransformV1> previousBones;
    std::vector<MorphTargetV1> morphTargets;
    std::vector<MorphDeltaV1> morphDeltas;
};

struct DeformBounds
{
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
};

struct DeformationResult
{
    std::vector<std::array<float, 3>> current;
    std::vector<std::array<float, 3>> previous;
    DeformBounds bounds{};
    DeformBounds previousBounds{};
    // Vertices whose supplied influence weights did not already sum to one
    // and were normalized explicitly rather than silently scaling geometry.
    std::uint32_t normalizedVertices{};
    float motionMagnitude{};
};

struct RingAllocation
{
    std::uint64_t offset{};
    std::uint64_t size{};
};

// Dynamic deformation output is written through a ring so a frame never
// overwrites bytes the GPU is still reading.
class DynamicRing
{
public:
    DynamicRing(std::uint64_t capacity, std::uint64_t alignment) noexcept;

    [[nodiscard]] bool Allocate(
        std::uint64_t size,
        std::uint64_t timelineValue,
        RingAllocation& allocation) noexcept;
    void Retire(std::uint64_t completedValue) noexcept;
    [[nodiscard]] std::uint64_t InFlightBytes() const noexcept;
    [[nodiscard]] std::uint64_t Capacity() const noexcept;

private:
    struct Range
    {
        std::uint64_t offset{};
        std::uint64_t size{};
        std::uint64_t timelineValue{};
    };

    [[nodiscard]] bool Overlaps(
        std::uint64_t offset,
        std::uint64_t size) const noexcept;

    std::uint64_t capacity_{};
    std::uint64_t alignment_{};
    std::uint64_t cursor_{};
    std::vector<Range> inFlight_;
};

// Fixed topology updates in place; a changed vertex or bone count requires a
// new generation so stale deformation state can never be reused.
class TopologyRegistry
{
public:
    [[nodiscard]] DeformError Observe(
        const DeformationPacket& packet) noexcept;
    [[nodiscard]] std::size_t TrackedCount() const noexcept;

private:
    struct Entry
    {
        std::uint32_t generation{};
        std::uint32_t vertexCount{};
        std::uint32_t boneCount{};
        std::uint32_t morphDeltaCount{};
    };

    std::map<std::uint64_t, Entry> entries_;
};

[[nodiscard]] DeformError ValidateDeformationPacket(
    const DeformationPacket& packet) noexcept;
[[nodiscard]] DeformError EncodeDeformationPacket(
    const DeformationPacket& packet,
    std::vector<std::byte>& bytes) noexcept;
[[nodiscard]] DeformError DecodeDeformationPacket(
    std::span<const std::byte> bytes,
    DeformationPacket& packet) noexcept;
[[nodiscard]] DeformError EvaluateDeformation(
    const DeformationPacket& packet,
    std::span<const std::array<float, 3>> baseVertices,
    DeformationResult& result) noexcept;
[[nodiscard]] const char* ToString(DeformError error) noexcept;

static_assert(sizeof(DeformVertexV1) == 48);
static_assert(sizeof(DeformVertexV1) == kGpuDeformVertexSize);
static_assert(sizeof(BoneTransformV1) == 48);
static_assert(sizeof(BoneTransformV1) == kGpuBoneTransformSize);
static_assert(sizeof(MorphTargetV1) == 16);
static_assert(sizeof(MorphDeltaV1) == 32);
static_assert(sizeof(WindParametersV1) == 32);
static_assert(sizeof(DeformPacketHeaderV1) == 144);
static_assert(offsetof(DeformPacketHeaderV1, wind) == 96);

}
