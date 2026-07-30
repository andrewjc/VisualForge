#include "renderer_core/EngineDeformation.h"

#include "renderer_trace/Crc32.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace vf::renderer::deform {

namespace {

constexpr float kWeightTolerance = 1.0e-4f;

std::size_t AlignUp(
    const std::size_t value,
    const std::size_t alignment) noexcept
{
    if (alignment == 0 || value >
        std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

bool CheckedMultiply(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result) noexcept
{
    if (left != 0 && right >
        std::numeric_limits<std::size_t>::max() / left) {
        result = std::numeric_limits<std::size_t>::max();
        return false;
    }
    result = left * right;
    return true;
}

bool CheckedAdd(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result) noexcept
{
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        result = std::numeric_limits<std::size_t>::max();
        return false;
    }
    result = left + right;
    return true;
}

bool Finite(const float* values, const std::size_t count) noexcept
{
    return std::all_of(values, values + count,
        [](const float value) { return std::isfinite(value); });
}

bool ValidBone(const BoneTransformV1& bone) noexcept
{
    if (!Finite(bone.rows, 12)) return false;
    const double determinant =
        static_cast<double>(bone.rows[0]) *
            (static_cast<double>(bone.rows[5]) * bone.rows[10] -
             static_cast<double>(bone.rows[6]) * bone.rows[9]) -
        static_cast<double>(bone.rows[1]) *
            (static_cast<double>(bone.rows[4]) * bone.rows[10] -
             static_cast<double>(bone.rows[6]) * bone.rows[8]) +
        static_cast<double>(bone.rows[2]) *
            (static_cast<double>(bone.rows[4]) * bone.rows[9] -
             static_cast<double>(bone.rows[5]) * bone.rows[8]);
    return std::isfinite(determinant) && std::abs(determinant) > 1.0e-10;
}

std::array<double, 3> TransformPoint(
    const BoneTransformV1& bone,
    const std::array<double, 3>& point) noexcept
{
    std::array<double, 3> output{};
    for (std::size_t row = 0; row < 3; ++row) {
        output[row] = static_cast<double>(bone.rows[row * 4 + 0]) * point[0] +
            static_cast<double>(bone.rows[row * 4 + 1]) * point[1] +
            static_cast<double>(bone.rows[row * 4 + 2]) * point[2] +
            static_cast<double>(bone.rows[row * 4 + 3]);
    }
    return output;
}

void ExpandBounds(
    DeformBounds& bounds,
    const std::array<float, 3>& point,
    const bool first) noexcept
{
    for (std::size_t axis = 0; axis < 3; ++axis) {
        bounds.minimum[axis] = first
            ? point[axis] : std::min(bounds.minimum[axis], point[axis]);
        bounds.maximum[axis] = first
            ? point[axis] : std::max(bounds.maximum[axis], point[axis]);
    }
}

std::array<double, 3> ApplyPose(
    const DeformationPacket& packet,
    const std::span<const BoneTransformV1> palette,
    const std::size_t vertexIndex,
    const std::array<double, 3>& morphed,
    const float windTime) noexcept
{
    const auto& influence = packet.vertices[vertexIndex];
    double total = 0.0;
    for (std::size_t slot = 0; slot < kInfluencesPerVertex; ++slot) {
        total += influence.weights[slot];
    }
    std::array<double, 3> skinned{};
    for (std::size_t slot = 0; slot < kInfluencesPerVertex; ++slot) {
        const auto weight = influence.weights[slot] / total;
        if (weight == 0.0) continue;
        const auto contribution = TransformPoint(
            palette[influence.bones[slot]], morphed);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            skinned[axis] += weight * contribution[axis];
        }
    }
    const auto& wind = packet.header.wind;
    if (wind.amplitude != 0.0f && influence.flexibility != 0.0f) {
        const auto phase = static_cast<double>(wind.frequency) * windTime +
            wind.direction[0] * morphed[0] + wind.direction[1] * morphed[1] +
            wind.direction[2] * morphed[2];
        const auto displacement = static_cast<double>(influence.flexibility) *
            wind.amplitude * std::sin(phase);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            skinned[axis] += displacement * wind.direction[axis];
        }
    }
    return skinned;
}

}

DynamicRing::DynamicRing(
    const std::uint64_t capacity,
    const std::uint64_t alignment) noexcept
    : capacity_(capacity)
    , alignment_(alignment == 0 ? 1 : alignment)
{}

bool DynamicRing::Overlaps(
    const std::uint64_t offset,
    const std::uint64_t size) const noexcept
{
    return std::any_of(inFlight_.begin(), inFlight_.end(),
        [offset, size](const Range& range) {
            return offset < range.offset + range.size &&
                range.offset < offset + size;
        });
}

bool DynamicRing::Allocate(
    const std::uint64_t size,
    const std::uint64_t timelineValue,
    RingAllocation& allocation) noexcept
{
    allocation = {};
    if (size == 0 || size > capacity_) return false;
    const auto aligned = (size + alignment_ - 1) & ~(alignment_ - 1);
    if (aligned > capacity_) return false;
    for (std::uint32_t attempt = 0; attempt < 2; ++attempt) {
        const auto offset = attempt == 0 ? cursor_ : 0;
        if (offset + aligned > capacity_) continue;
        if (Overlaps(offset, aligned)) continue;
        try {
            inFlight_.push_back(Range{offset, aligned, timelineValue});
        } catch (...) {
            return false;
        }
        cursor_ = offset + aligned;
        if (cursor_ >= capacity_) cursor_ = 0;
        allocation = RingAllocation{offset, size};
        return true;
    }
    return false;
}

void DynamicRing::Retire(const std::uint64_t completedValue) noexcept
{
    inFlight_.erase(
        std::remove_if(inFlight_.begin(), inFlight_.end(),
            [completedValue](const Range& range) {
                return range.timelineValue <= completedValue;
            }),
        inFlight_.end());
}

std::uint64_t DynamicRing::InFlightBytes() const noexcept
{
    std::uint64_t total = 0;
    for (const auto& range : inFlight_) {
        total += range.size;
    }
    return total;
}

std::uint64_t DynamicRing::Capacity() const noexcept
{
    return capacity_;
}

DeformError TopologyRegistry::Observe(
    const DeformationPacket& packet) noexcept
{
    const auto validation = ValidateDeformationPacket(packet);
    if (validation != DeformError::None) return validation;
    const Entry observed{
        packet.header.generation,
        static_cast<std::uint32_t>(packet.vertices.size()),
        static_cast<std::uint32_t>(packet.bones.size()),
        static_cast<std::uint32_t>(packet.morphDeltas.size()),
    };
    const auto existing = entries_.find(packet.header.topologyId);
    if (existing == entries_.end()) {
        try {
            entries_.emplace(packet.header.topologyId, observed);
        } catch (...) {
            return DeformError::AllocationFailure;
        }
        return DeformError::None;
    }
    if (observed.generation < existing->second.generation) {
        return DeformError::GenerationMismatch;
    }
    if (observed.generation == existing->second.generation) {
        if (observed.vertexCount != existing->second.vertexCount ||
            observed.boneCount != existing->second.boneCount ||
            observed.morphDeltaCount != existing->second.morphDeltaCount) {
            return DeformError::GenerationMismatch;
        }
        return DeformError::None;
    }
    existing->second = observed;
    return DeformError::None;
}

std::size_t TopologyRegistry::TrackedCount() const noexcept
{
    return entries_.size();
}

DeformError ValidateDeformationPacket(
    const DeformationPacket& packet) noexcept
{
    const auto& header = packet.header;
    if (header.magic != kDeformPacketMagic ||
        header.versionMajor != kDeformPacketVersionMajor ||
        header.versionMinor > kDeformPacketVersionMinor ||
        header.endianMarker != kDeformPacketEndian ||
        header.topologyId == 0 || header.generation == 0 ||
        header.captureThreadId == 0 || header.renderThreadId == 0 ||
        packet.vertices.empty() ||
        packet.vertices.size() > kMaximumDeformVertices ||
        packet.bones.empty() ||
        packet.bones.size() > kMaximumDeformBones ||
        packet.morphTargets.size() > kMaximumMorphTargets ||
        packet.morphDeltas.size() > kMaximumMorphDeltas) {
        return DeformError::InvalidIdentity;
    }
    if (header.captureThreadId != header.renderThreadId) {
        return DeformError::WrongThread;
    }
    if (header.flags != 0 || header.reserved0 != 0 ||
        std::any_of(std::begin(header.reserved), std::end(header.reserved),
            [](const std::uint64_t value) { return value != 0; })) {
        return DeformError::InvalidFlags;
    }
    if (packet.previousBones.size() != packet.bones.size()) {
        return DeformError::TopologyMismatch;
    }
    for (const auto& bone : packet.bones) {
        if (!ValidBone(bone)) return DeformError::InvalidMatrix;
    }
    for (const auto& bone : packet.previousBones) {
        if (!ValidBone(bone)) return DeformError::InvalidMatrix;
    }
    for (const auto& vertex : packet.vertices) {
        if (!Finite(vertex.weights, kInfluencesPerVertex) ||
            !std::isfinite(vertex.flexibility) ||
            vertex.flexibility < 0.0f ||
            !Finite(vertex.reserved, 3) ||
            std::any_of(std::begin(vertex.reserved),
                std::end(vertex.reserved),
                [](const float value) { return value != 0.0f; })) {
            return DeformError::InvalidWeights;
        }
        float total = 0.0f;
        for (std::size_t slot = 0; slot < kInfluencesPerVertex; ++slot) {
            if (vertex.weights[slot] < 0.0f) {
                return DeformError::InvalidWeights;
            }
            total += vertex.weights[slot];
        }
        if (total <= kWeightTolerance) return DeformError::InvalidWeights;
        for (std::size_t slot = 0; slot < kInfluencesPerVertex; ++slot) {
            if (vertex.weights[slot] == 0.0f) continue;
            if (vertex.bones[slot] >= packet.bones.size()) {
                return DeformError::InvalidBoneIndex;
            }
        }
    }
    for (const auto& target : packet.morphTargets) {
        if (!std::isfinite(target.weight) ||
            !std::isfinite(target.previousWeight) ||
            target.weight < 0.0f || target.weight > 1.0f ||
            target.previousWeight < 0.0f || target.previousWeight > 1.0f) {
            return DeformError::InvalidMorph;
        }
        const auto end = static_cast<std::uint64_t>(target.firstDelta) +
            target.deltaCount;
        if (end > packet.morphDeltas.size()) {
            return DeformError::InvalidMorph;
        }
    }
    for (const auto& delta : packet.morphDeltas) {
        if (delta.vertexIndex >= packet.vertices.size() ||
            !Finite(delta.delta, 4) || delta.delta[3] != 0.0f ||
            std::any_of(std::begin(delta.reserved), std::end(delta.reserved),
                [](const std::uint32_t value) { return value != 0; })) {
            return DeformError::InvalidMorph;
        }
    }
    const auto& wind = header.wind;
    if (!Finite(wind.direction, 4) || wind.direction[3] != 0.0f ||
        !std::isfinite(wind.amplitude) || wind.amplitude < 0.0f ||
        !std::isfinite(wind.frequency) || wind.frequency < 0.0f ||
        !std::isfinite(wind.time) || !std::isfinite(wind.previousTime)) {
        return DeformError::InvalidWind;
    }
    return DeformError::None;
}

DeformError EncodeDeformationPacket(
    const DeformationPacket& packet,
    std::vector<std::byte>& bytes) noexcept
{
    bytes.clear();
    const auto validation = ValidateDeformationPacket(packet);
    if (validation != DeformError::None) return validation;
    try {
        auto header = packet.header;
        header.headerSize = sizeof(DeformPacketHeaderV1);
        header.vertexCount = static_cast<std::uint32_t>(
            packet.vertices.size());
        header.boneCount = static_cast<std::uint32_t>(packet.bones.size());
        header.morphTargetCount = static_cast<std::uint32_t>(
            packet.morphTargets.size());
        header.morphDeltaCount = static_cast<std::uint32_t>(
            packet.morphDeltas.size());

        std::size_t cursor = AlignUp(sizeof(header), alignof(DeformVertexV1));
        std::size_t vertexBytes{};
        std::size_t boneBytes{};
        std::size_t targetBytes{};
        std::size_t deltaBytes{};
        if (cursor == std::numeric_limits<std::size_t>::max() ||
            !CheckedMultiply(packet.vertices.size(), sizeof(DeformVertexV1),
                vertexBytes) ||
            !CheckedMultiply(packet.bones.size(), sizeof(BoneTransformV1),
                boneBytes) ||
            !CheckedMultiply(packet.morphTargets.size(),
                sizeof(MorphTargetV1), targetBytes) ||
            !CheckedMultiply(packet.morphDeltas.size(), sizeof(MorphDeltaV1),
                deltaBytes)) {
            return DeformError::AllocationFailure;
        }
        header.verticesOffset = static_cast<std::uint32_t>(cursor);
        if (!CheckedAdd(cursor, vertexBytes, cursor)) {
            return DeformError::AllocationFailure;
        }
        header.bonesOffset = static_cast<std::uint32_t>(cursor);
        if (!CheckedAdd(cursor, boneBytes, cursor)) {
            return DeformError::AllocationFailure;
        }
        header.previousBonesOffset = static_cast<std::uint32_t>(cursor);
        if (!CheckedAdd(cursor, boneBytes, cursor)) {
            return DeformError::AllocationFailure;
        }
        header.morphTargetsOffset = packet.morphTargets.empty()
            ? 0 : static_cast<std::uint32_t>(cursor);
        if (!CheckedAdd(cursor, targetBytes, cursor)) {
            return DeformError::AllocationFailure;
        }
        header.morphDeltasOffset = packet.morphDeltas.empty()
            ? 0 : static_cast<std::uint32_t>(cursor);
        if (!CheckedAdd(cursor, deltaBytes, cursor) ||
            cursor > std::numeric_limits<std::uint32_t>::max()) {
            return DeformError::AllocationFailure;
        }
        header.totalSize = static_cast<std::uint32_t>(cursor);
        bytes.assign(cursor, std::byte{0});
        std::memcpy(bytes.data() + header.verticesOffset,
            packet.vertices.data(), vertexBytes);
        std::memcpy(bytes.data() + header.bonesOffset,
            packet.bones.data(), boneBytes);
        std::memcpy(bytes.data() + header.previousBonesOffset,
            packet.previousBones.data(), boneBytes);
        if (!packet.morphTargets.empty()) {
            std::memcpy(bytes.data() + header.morphTargetsOffset,
                packet.morphTargets.data(), targetBytes);
        }
        if (!packet.morphDeltas.empty()) {
            std::memcpy(bytes.data() + header.morphDeltasOffset,
                packet.morphDeltas.data(), deltaBytes);
        }
        header.payloadCrc32 = trace::Crc32(
            std::span<const std::byte>{bytes}.subspan(sizeof(header)));
        std::memcpy(bytes.data(), &header, sizeof(header));
        return DeformError::None;
    } catch (...) {
        bytes.clear();
        return DeformError::AllocationFailure;
    }
}

DeformError DecodeDeformationPacket(
    const std::span<const std::byte> bytes,
    DeformationPacket& packet) noexcept
{
    packet = {};
    if (bytes.size() < sizeof(DeformPacketHeaderV1)) {
        return DeformError::TruncatedHeader;
    }
    DeformPacketHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kDeformPacketMagic) return DeformError::BadMagic;
    if (header.versionMajor != kDeformPacketVersionMajor ||
        header.versionMinor > kDeformPacketVersionMinor) {
        return DeformError::UnsupportedVersion;
    }
    if (header.endianMarker != kDeformPacketEndian) {
        return DeformError::WrongEndian;
    }
    if (header.headerSize != sizeof(header)) {
        return DeformError::SectionOutOfBounds;
    }
    if (header.totalSize != bytes.size()) return DeformError::SizeMismatch;
    if (header.vertexCount == 0 ||
        header.vertexCount > kMaximumDeformVertices ||
        header.boneCount == 0 || header.boneCount > kMaximumDeformBones ||
        header.morphTargetCount > kMaximumMorphTargets ||
        header.morphDeltaCount > kMaximumMorphDeltas) {
        return DeformError::SectionOutOfBounds;
    }
    std::size_t cursor = AlignUp(sizeof(header), alignof(DeformVertexV1));
    std::size_t vertexBytes{};
    std::size_t boneBytes{};
    std::size_t targetBytes{};
    std::size_t deltaBytes{};
    if (cursor == std::numeric_limits<std::size_t>::max() ||
        !CheckedMultiply(header.vertexCount, sizeof(DeformVertexV1),
            vertexBytes) ||
        !CheckedMultiply(header.boneCount, sizeof(BoneTransformV1),
            boneBytes) ||
        !CheckedMultiply(header.morphTargetCount, sizeof(MorphTargetV1),
            targetBytes) ||
        !CheckedMultiply(header.morphDeltaCount, sizeof(MorphDeltaV1),
            deltaBytes)) {
        return DeformError::SectionOutOfBounds;
    }
    if (header.verticesOffset != cursor) {
        return header.verticesOffset % alignof(DeformVertexV1) != 0
            ? DeformError::MisalignedSection
            : DeformError::SectionOutOfBounds;
    }
    cursor += vertexBytes;
    if (header.bonesOffset != cursor) {
        return DeformError::SectionOutOfBounds;
    }
    cursor += boneBytes;
    if (header.previousBonesOffset != cursor) {
        return DeformError::SectionOutOfBounds;
    }
    cursor += boneBytes;
    if (header.morphTargetsOffset !=
        (header.morphTargetCount == 0 ? 0u
            : static_cast<std::uint32_t>(cursor))) {
        return DeformError::SectionOutOfBounds;
    }
    cursor += targetBytes;
    if (header.morphDeltasOffset !=
        (header.morphDeltaCount == 0 ? 0u
            : static_cast<std::uint32_t>(cursor))) {
        return DeformError::SectionOutOfBounds;
    }
    cursor += deltaBytes;
    if (cursor != bytes.size()) return DeformError::SizeMismatch;
    if (!std::all_of(bytes.begin() + sizeof(header),
            bytes.begin() + header.verticesOffset,
            [](const std::byte value) { return value == std::byte{0}; })) {
        return DeformError::NonZeroPadding;
    }
    if (trace::Crc32(bytes.subspan(sizeof(header))) != header.payloadCrc32) {
        return DeformError::ChecksumMismatch;
    }
    try {
        DeformationPacket candidate{};
        candidate.header = header;
        candidate.vertices.resize(header.vertexCount);
        candidate.bones.resize(header.boneCount);
        candidate.previousBones.resize(header.boneCount);
        std::memcpy(candidate.vertices.data(),
            bytes.data() + header.verticesOffset, vertexBytes);
        std::memcpy(candidate.bones.data(),
            bytes.data() + header.bonesOffset, boneBytes);
        std::memcpy(candidate.previousBones.data(),
            bytes.data() + header.previousBonesOffset, boneBytes);
        if (header.morphTargetCount != 0) {
            candidate.morphTargets.resize(header.morphTargetCount);
            std::memcpy(candidate.morphTargets.data(),
                bytes.data() + header.morphTargetsOffset, targetBytes);
        }
        if (header.morphDeltaCount != 0) {
            candidate.morphDeltas.resize(header.morphDeltaCount);
            std::memcpy(candidate.morphDeltas.data(),
                bytes.data() + header.morphDeltasOffset, deltaBytes);
        }
        const auto validation = ValidateDeformationPacket(candidate);
        if (validation != DeformError::None) return validation;
        packet = std::move(candidate);
        return DeformError::None;
    } catch (...) {
        packet = {};
        return DeformError::AllocationFailure;
    }
}

DeformError EvaluateDeformation(
    const DeformationPacket& packet,
    const std::span<const std::array<float, 3>> baseVertices,
    DeformationResult& result) noexcept
{
    result = {};
    const auto validation = ValidateDeformationPacket(packet);
    if (validation != DeformError::None) return validation;
    if (baseVertices.size() != packet.vertices.size()) {
        return DeformError::InvalidBase;
    }
    for (const auto& vertex : baseVertices) {
        if (!std::all_of(vertex.begin(), vertex.end(),
                [](const float value) { return std::isfinite(value); })) {
            return DeformError::InvalidBase;
        }
    }
    try {
        result.current.resize(baseVertices.size());
        result.previous.resize(baseVertices.size());
        // Morph accumulation happens in bind space, then skinning, then
        // wind. The previous pose runs the same order with previous inputs.
        std::vector<std::array<double, 3>> morphed(baseVertices.size());
        std::vector<std::array<double, 3>> previousMorphed(
            baseVertices.size());
        for (std::size_t index = 0; index < baseVertices.size(); ++index) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                morphed[index][axis] = baseVertices[index][axis];
                previousMorphed[index][axis] = baseVertices[index][axis];
            }
        }
        for (const auto& target : packet.morphTargets) {
            for (std::uint32_t element = 0; element < target.deltaCount;
                 ++element) {
                const auto& delta =
                    packet.morphDeltas[target.firstDelta + element];
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    morphed[delta.vertexIndex][axis] +=
                        static_cast<double>(target.weight) *
                        delta.delta[axis];
                    previousMorphed[delta.vertexIndex][axis] +=
                        static_cast<double>(target.previousWeight) *
                        delta.delta[axis];
                }
            }
        }
        double motion = 0.0;
        for (std::size_t index = 0; index < baseVertices.size(); ++index) {
            float total = 0.0f;
            for (std::size_t slot = 0; slot < kInfluencesPerVertex; ++slot) {
                total += packet.vertices[index].weights[slot];
            }
            if (std::abs(total - 1.0f) > kWeightTolerance) {
                ++result.normalizedVertices;
            }
            const auto current = ApplyPose(packet, packet.bones, index,
                morphed[index], packet.header.wind.time);
            const auto previous = ApplyPose(packet, packet.previousBones,
                index, previousMorphed[index],
                packet.header.wind.previousTime);
            double squared = 0.0;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                result.current[index][axis] =
                    static_cast<float>(current[axis]);
                result.previous[index][axis] =
                    static_cast<float>(previous[axis]);
                const auto difference = current[axis] - previous[axis];
                squared += difference * difference;
            }
            motion = std::max(motion, std::sqrt(squared));
            ExpandBounds(result.bounds, result.current[index], index == 0);
            ExpandBounds(result.previousBounds, result.previous[index],
                index == 0);
        }
        // The mirrored bounds must cover both poses so motion never leaves
        // the region the renderer reserved for the object.
        for (std::size_t axis = 0; axis < 3; ++axis) {
            result.bounds.minimum[axis] = std::min(
                result.bounds.minimum[axis],
                result.previousBounds.minimum[axis]);
            result.bounds.maximum[axis] = std::max(
                result.bounds.maximum[axis],
                result.previousBounds.maximum[axis]);
        }
        result.motionMagnitude = static_cast<float>(motion);
        return DeformError::None;
    } catch (...) {
        result = {};
        return DeformError::AllocationFailure;
    }
}

const char* ToString(const DeformError error) noexcept
{
    switch (error) {
    case DeformError::None: return "none";
    case DeformError::TruncatedHeader: return "truncated header";
    case DeformError::BadMagic: return "bad magic";
    case DeformError::UnsupportedVersion: return "unsupported version";
    case DeformError::WrongEndian: return "wrong endian";
    case DeformError::SizeMismatch: return "size mismatch";
    case DeformError::ChecksumMismatch: return "checksum mismatch";
    case DeformError::SectionOutOfBounds: return "section out of bounds";
    case DeformError::MisalignedSection: return "misaligned section";
    case DeformError::NonZeroPadding: return "nonzero padding";
    case DeformError::WrongThread: return "wrong thread";
    case DeformError::InvalidIdentity: return "invalid identity";
    case DeformError::InvalidFlags: return "invalid flags";
    case DeformError::InvalidWeights: return "invalid weights";
    case DeformError::InvalidBoneIndex: return "invalid bone index";
    case DeformError::InvalidMatrix: return "invalid matrix";
    case DeformError::InvalidMorph: return "invalid morph";
    case DeformError::InvalidWind: return "invalid wind";
    case DeformError::InvalidBase: return "invalid base stream";
    case DeformError::TopologyMismatch: return "topology mismatch";
    case DeformError::GenerationMismatch: return "generation mismatch";
    case DeformError::AllocationFailure: return "allocation failure";
    }
    return "unknown";
}

}
