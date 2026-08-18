#include "renderer_api/RasterPacket.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace vf::renderer::raster {

namespace {

constexpr std::uint64_t kNearMaterialId = 0x6000'0000'0000'0001ull;
constexpr std::uint64_t kFarMaterialId = 0x6000'0000'0000'0002ull;

[[nodiscard]] std::size_t AlignUp(
    const std::size_t value,
    const std::size_t alignment) noexcept
{
    if (alignment == 0 || value >
        std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

template <class T, std::size_t Extent>
void AppendSection(
    std::vector<std::byte>& bytes,
    const std::span<T, Extent> values,
    std::uint32_t& offset)
{
    const auto aligned = AlignUp(
        bytes.size(), std::max<std::size_t>(8, alignof(T)));
    bytes.resize(aligned);
    offset = static_cast<std::uint32_t>(aligned);
    const auto byteCount = values.size_bytes();
    const auto oldSize = bytes.size();
    bytes.resize(oldSize + byteCount);
    if (byteCount != 0) {
        std::memcpy(bytes.data() + oldSize, values.data(), byteCount);
    }
}

[[nodiscard]] bool IsFinite(const float value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool CheckedByteCount(
    const std::uint32_t count,
    const std::size_t stride,
    std::size_t& result) noexcept
{
    if (count > std::numeric_limits<std::size_t>::max() / stride) {
        return false;
    }
    result = static_cast<std::size_t>(count) * stride;
    return true;
}

[[nodiscard]] PacketResult ValidateSection(
    const PacketHeaderV1& header,
    const std::uint32_t offset,
    const std::size_t size,
    const std::size_t alignment) noexcept
{
    if (offset % alignment != 0) {
        return {PacketError::MisalignedSection, offset};
    }
    if (offset < header.headerSize || offset > header.totalSize ||
        size > static_cast<std::size_t>(header.totalSize - offset)) {
        return {PacketError::SectionOutOfBounds, offset};
    }
    return {};
}

[[nodiscard]] bool SectionsOverlap(
    const std::uint32_t firstOffset,
    const std::size_t firstSize,
    const std::uint32_t secondOffset,
    const std::size_t secondSize) noexcept
{
    const auto firstEnd = static_cast<std::size_t>(firstOffset) + firstSize;
    const auto secondEnd = static_cast<std::size_t>(secondOffset) + secondSize;
    return static_cast<std::size_t>(firstOffset) < secondEnd &&
        static_cast<std::size_t>(secondOffset) < firstEnd;
}

}

std::vector<std::byte> BuildSyntheticPacket(
    const SyntheticPacketOptions& options)
{
    const RasterVertexV1 nearVertices[]{
        {{-0.75f, -0.70f, 0.25f}, {1.0f, 0.0f, 0.0f}},
        {{0.75f, -0.70f, 0.25f}, {0.0f, 1.0f, 0.0f}},
        {{0.0f, 0.75f, 0.25f}, {0.0f, 0.0f, 1.0f}},
    };
    const RasterVertexV1 farVertices[]{
        {{-0.75f, -0.70f, 0.75f}, {1.0f, 0.0f, 1.0f}},
        {{0.75f, -0.70f, 0.75f}, {1.0f, 0.0f, 1.0f}},
        {{0.0f, 0.75f, 0.75f}, {1.0f, 0.0f, 1.0f}},
    };

    std::vector<RasterVertexV3> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<RasterDrawV1> draws;
    std::vector<RasterMaterialV1> materials;
    if (options.includeOccludedTriangle) {
        vertices.insert(vertices.end(), std::begin(farVertices), std::end(farVertices));
        vertices.insert(vertices.end(), std::begin(nearVertices), std::end(nearVertices));
        indices = {0, 1, 2, 3, 4, 5};
        materials.push_back(RasterMaterialV1{
            kFarMaterialId,
            kPhase6ShaderLayoutHash,
            {1.0f, 1.0f, 1.0f, 1.0f}});
        materials.push_back(RasterMaterialV1{
            kNearMaterialId,
            kPhase6ShaderLayoutHash,
            {1.5f, 1.0f, 0.75f, 1.0f}});
        draws.push_back(RasterDrawV1{
            kFarMaterialId, 0, 3, 0, options.frontFace,
            options.depthCompare, 0});
        draws.push_back(RasterDrawV1{
            kNearMaterialId, 3, 3, 0, options.frontFace,
            options.depthCompare, 0});
    } else {
        vertices.assign(std::begin(nearVertices), std::end(nearVertices));
        indices = {0, 1, 2};
        materials.push_back(RasterMaterialV1{
            kNearMaterialId,
            kPhase6ShaderLayoutHash,
            {1.5f, 1.0f, 0.75f, 1.0f}});
        draws.push_back(RasterDrawV1{
            kNearMaterialId, 0, 3, 0, options.frontFace,
            options.depthCompare, 0});
    }
    for (std::size_t vertex = 0; vertex + 2 < vertices.size(); vertex += 3) {
        vertices[vertex].texCoord[0] = 0.0f;
        vertices[vertex].texCoord[1] = 1.0f;
        vertices[vertex + 1].texCoord[0] = 1.0f;
        vertices[vertex + 1].texCoord[1] = 1.0f;
        vertices[vertex + 2].texCoord[0] = 0.5f;
        vertices[vertex + 2].texCoord[1] = 0.0f;
    }
    if (options.reverseWinding) {
        for (std::size_t triangle = 0; triangle < indices.size(); triangle += 3) {
            std::swap(indices[triangle + 1], indices[triangle + 2]);
        }
    }

    PacketHeaderV1 header{};
    header.headerSize = sizeof(header);
    header.frameIndex = options.frameIndex;
    header.width = options.width;
    header.height = options.height;
    header.viewportWidth = static_cast<float>(options.width);
    header.viewportHeight = static_cast<float>(options.height);
    header.scissorWidth = options.width;
    header.scissorHeight = options.height;
    header.indexType = options.indexType;
    header.vertexCount = static_cast<std::uint32_t>(vertices.size());
    header.indexCount = static_cast<std::uint32_t>(indices.size());
    header.drawCount = static_cast<std::uint32_t>(draws.size());
    header.materialCount = static_cast<std::uint32_t>(materials.size());

    std::vector<std::byte> bytes(sizeof(header));
    AppendSection(bytes, std::span{vertices}, header.verticesOffset);
    if (options.indexType == IndexType::Uint16) {
        std::vector<std::uint16_t> narrowIndices(indices.size());
        std::transform(indices.begin(), indices.end(), narrowIndices.begin(),
            [](const std::uint32_t index) {
                return static_cast<std::uint16_t>(index);
            });
        AppendSection(bytes, std::span{narrowIndices}, header.indicesOffset);
    } else {
        AppendSection(bytes, std::span{indices}, header.indicesOffset);
    }
    AppendSection(bytes, std::span{draws}, header.drawsOffset);
    AppendSection(bytes, std::span{materials}, header.materialsOffset);
    header.totalSize = static_cast<std::uint32_t>(bytes.size());
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}

PacketResult EncodePacket(
    const DecodedPacket& packet,
    std::vector<std::byte>& bytes) noexcept
{
    bytes.clear();
    if (packet.vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
        packet.indices.size() > std::numeric_limits<std::uint32_t>::max() ||
        packet.draws.size() > std::numeric_limits<std::uint32_t>::max() ||
        packet.materials.size() > std::numeric_limits<std::uint32_t>::max()) {
        return {PacketError::AllocationFailure, 0};
    }
    try {
        auto header = packet.header;
        header.magic = kPacketMagic;
        header.versionMajor = kPacketVersionMajor;
        // An older minor asked for explicitly is honoured, so a fixture in the
        // previous layout can still be produced and read back. Anything else,
        // including zero from a default-constructed header, encodes current.
        const auto requested = packet.header.versionMinor;
        header.versionMinor =
            requested == kPacketPhase6VersionMinor ||
            requested == kPacketTexCoordVersionMinor
                ? requested : kPacketVersionMinor;
        header.headerSize = sizeof(PacketHeaderV1);
        header.shaderLayoutHash = kPhase6ShaderLayoutHash;
        header.vertexCount = static_cast<std::uint32_t>(packet.vertices.size());
        header.indexCount = static_cast<std::uint32_t>(packet.indices.size());
        header.drawCount = static_cast<std::uint32_t>(packet.draws.size());
        header.materialCount = static_cast<std::uint32_t>(packet.materials.size());

        bytes.resize(sizeof(header));
        // Written in the layout the version names, never in the in-memory one.
        // Encoding forty-eight-byte vertices under an older minor hands the
        // reader a stride it will walk at thirty-two or twenty-four, which is
        // not a decode failure but a scene of garbage.
        if (header.versionMinor == kPacketPhase6VersionMinor) {
            std::vector<RasterVertexV1> legacy(packet.vertices.size());
            for (std::size_t index = 0; index < packet.vertices.size();
                ++index) {
                const auto& source = packet.vertices[index];
                std::memcpy(legacy[index].position, source.position,
                    sizeof(legacy[index].position));
                std::memcpy(legacy[index].color, source.color,
                    sizeof(legacy[index].color));
            }
            AppendSection(bytes, std::span{legacy}, header.verticesOffset);
        } else if (header.versionMinor == kPacketTexCoordVersionMinor) {
            std::vector<RasterVertexV2> previous(packet.vertices.size());
            for (std::size_t index = 0; index < packet.vertices.size();
                ++index) {
                const auto& source = packet.vertices[index];
                std::memcpy(previous[index].position, source.position,
                    sizeof(previous[index].position));
                std::memcpy(previous[index].color, source.color,
                    sizeof(previous[index].color));
                std::memcpy(previous[index].texCoord, source.texCoord,
                    sizeof(previous[index].texCoord));
            }
            AppendSection(bytes, std::span{previous}, header.verticesOffset);
        } else {
            AppendSection(bytes, std::span{packet.vertices},
                header.verticesOffset);
        }
        if (header.indexType == IndexType::Uint16) {
            std::vector<std::uint16_t> narrow(packet.indices.size());
            for (std::size_t index = 0; index < packet.indices.size(); ++index) {
                if (packet.indices[index] >
                    std::numeric_limits<std::uint16_t>::max()) {
                    bytes.clear();
                    return {PacketError::IndexOutOfRange, index};
                }
                narrow[index] = static_cast<std::uint16_t>(packet.indices[index]);
            }
            AppendSection(bytes, std::span{narrow}, header.indicesOffset);
        } else if (header.indexType == IndexType::Uint32) {
            AppendSection(bytes, std::span{packet.indices}, header.indicesOffset);
        } else {
            bytes.clear();
            return {PacketError::InvalidIndexType,
                    offsetof(PacketHeaderV1, indexType)};
        }
        AppendSection(bytes, std::span{packet.draws}, header.drawsOffset);
        AppendSection(bytes, std::span{packet.materials}, header.materialsOffset);
        if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
            bytes.clear();
            return {PacketError::AllocationFailure, 0};
        }
        header.totalSize = static_cast<std::uint32_t>(bytes.size());
        std::memcpy(bytes.data(), &header, sizeof(header));

        DecodedPacket verified;
        const auto result = DecodePacket(bytes, verified);
        if (!result) {
            bytes.clear();
        }
        return result;
    } catch (...) {
        bytes.clear();
        return {PacketError::AllocationFailure, 0};
    }
}

PacketResult DecodePacketHeader(
    const std::span<const std::byte> bytes,
    PacketHeaderV1& header) noexcept
{
    header = {};
    PacketHeaderV1 candidate{};
    if (bytes.size() < sizeof(PacketHeaderV1)) {
        return {PacketError::TruncatedHeader, bytes.size()};
    }
    std::memcpy(&candidate, bytes.data(), sizeof(candidate));
    if (candidate.magic != kPacketMagic) {
        return {PacketError::BadMagic, offsetof(PacketHeaderV1, magic)};
    }
    if (candidate.versionMajor != kPacketVersionMajor ||
        candidate.versionMinor > kPacketVersionMinor) {
        return {PacketError::UnsupportedVersion,
            offsetof(PacketHeaderV1, versionMajor)};
    }
    if (candidate.headerSize < sizeof(PacketHeaderV1) ||
        candidate.headerSize > candidate.totalSize) {
        return {PacketError::TruncatedHeader,
            offsetof(PacketHeaderV1, headerSize)};
    }
    if (candidate.totalSize != bytes.size()) {
        return {PacketError::SizeMismatch,
            offsetof(PacketHeaderV1, totalSize)};
    }
    if (candidate.width == 0 || candidate.height == 0 ||
        candidate.width > kMaximumExtent ||
        candidate.height > kMaximumExtent) {
        return {PacketError::InvalidExtent,
            offsetof(PacketHeaderV1, width)};
    }
    header = candidate;
    return {PacketError::None, 0};
}

PacketResult DecodePacket(
    const std::span<const std::byte> bytes,
    DecodedPacket& decoded) noexcept
{
    decoded = {};
    if (bytes.size() < sizeof(PacketHeaderV1)) {
        return {PacketError::TruncatedHeader, bytes.size()};
    }
    PacketHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kPacketMagic) {
        return {PacketError::BadMagic, offsetof(PacketHeaderV1, magic)};
    }
    if (header.versionMajor != kPacketVersionMajor ||
        header.versionMinor > kPacketVersionMinor) {
        return {PacketError::UnsupportedVersion,
            offsetof(PacketHeaderV1, versionMajor)};
    }
    if (header.headerSize < sizeof(PacketHeaderV1) ||
        header.headerSize > header.totalSize) {
        return {PacketError::TruncatedHeader,
            offsetof(PacketHeaderV1, headerSize)};
    }
    if (header.totalSize != bytes.size()) {
        return {PacketError::SizeMismatch,
            offsetof(PacketHeaderV1, totalSize)};
    }
    if (header.width == 0 || header.height == 0 ||
        header.width > kMaximumExtent || header.height > kMaximumExtent) {
        return {PacketError::InvalidExtent,
            offsetof(PacketHeaderV1, width)};
    }
    const auto viewportRight = header.viewportX + header.viewportWidth;
    const auto viewportBottom = header.viewportY + header.viewportHeight;
    if (!IsFinite(header.viewportX) || !IsFinite(header.viewportY) ||
        !IsFinite(header.viewportWidth) || !IsFinite(header.viewportHeight) ||
        !IsFinite(header.viewportMinDepth) ||
        !IsFinite(header.viewportMaxDepth) ||
        header.viewportX < 0.0f || header.viewportY < 0.0f ||
        header.viewportWidth <= 0.0f || header.viewportHeight <= 0.0f ||
        viewportRight > static_cast<float>(header.width) ||
        viewportBottom > static_cast<float>(header.height) ||
        header.viewportMinDepth < 0.0f ||
        header.viewportMaxDepth > 1.0f ||
        header.viewportMinDepth > header.viewportMaxDepth) {
        return {PacketError::InvalidViewport,
            offsetof(PacketHeaderV1, viewportX)};
    }
    const auto scissorRight = static_cast<std::int64_t>(header.scissorX) +
        header.scissorWidth;
    const auto scissorBottom = static_cast<std::int64_t>(header.scissorY) +
        header.scissorHeight;
    if (header.scissorX < 0 || header.scissorY < 0 ||
        header.scissorWidth == 0 || header.scissorHeight == 0 ||
        scissorRight > header.width || scissorBottom > header.height) {
        return {PacketError::InvalidScissor,
            offsetof(PacketHeaderV1, scissorX)};
    }
    if (header.indexType != IndexType::Uint16 &&
        header.indexType != IndexType::Uint32) {
        return {PacketError::InvalidIndexType,
            offsetof(PacketHeaderV1, indexType)};
    }
    // A space this build does not know is refused rather than assumed to be
    // the screen plane; guessing would silently apply the wrong winding rule.
    if (header.vertexSpace != VertexSpace::ScreenNdc &&
        header.vertexSpace != VertexSpace::CameraRelativeWorld) {
        return {PacketError::InvalidVertexSpace,
            offsetof(PacketHeaderV1, vertexSpace)};
    }
    if (header.shaderLayoutHash != kPhase6ShaderLayoutHash) {
        return {PacketError::ShaderLayoutMismatch,
            offsetof(PacketHeaderV1, shaderLayoutHash)};
    }

    std::size_t vertexBytes{};
    std::size_t indexBytes{};
    std::size_t drawBytes{};
    std::size_t materialBytes{};
    const auto vertexStride =
        header.versionMinor == kPacketPhase6VersionMinor
            ? sizeof(RasterVertexV1)
        : header.versionMinor == kPacketTexCoordVersionMinor
            ? sizeof(RasterVertexV2)
            : sizeof(RasterVertexV3);
    if (!CheckedByteCount(header.vertexCount, vertexStride, vertexBytes) ||
        !CheckedByteCount(header.indexCount,
            static_cast<std::size_t>(header.indexType), indexBytes) ||
        !CheckedByteCount(header.drawCount, sizeof(RasterDrawV1), drawBytes) ||
        !CheckedByteCount(header.materialCount,
            sizeof(RasterMaterialV1), materialBytes) ||
        header.vertexCount == 0 || header.indexCount == 0 ||
        header.drawCount == 0 || header.materialCount == 0) {
        return {PacketError::SectionOutOfBounds, 0};
    }
    const struct Section
    {
        std::uint32_t offset;
        std::size_t size;
        std::size_t alignment;
    } sections[]{
        {header.verticesOffset, vertexBytes,
            header.versionMinor == kPacketPhase6VersionMinor
                ? alignof(RasterVertexV1)
            : header.versionMinor == kPacketTexCoordVersionMinor
                ? alignof(RasterVertexV2)
                : alignof(RasterVertexV3)},
        {header.indicesOffset, indexBytes,
            static_cast<std::size_t>(header.indexType)},
        {header.drawsOffset, drawBytes, alignof(RasterDrawV1)},
        {header.materialsOffset, materialBytes, alignof(RasterMaterialV1)},
    };
    for (const auto& section : sections) {
        const auto result = ValidateSection(
            header, section.offset, section.size, section.alignment);
        if (!result) {
            return result;
        }
    }
    for (std::size_t first = 0; first < std::size(sections); ++first) {
        for (std::size_t second = first + 1; second < std::size(sections); ++second) {
            if (SectionsOverlap(
                sections[first].offset, sections[first].size,
                sections[second].offset, sections[second].size)) {
                return {PacketError::SectionOutOfBounds,
                    sections[second].offset};
            }
        }
    }

    try {
        DecodedPacket candidate;
        candidate.header = header;
        candidate.vertices.resize(header.vertexCount);
        candidate.indices.resize(header.indexCount);
        candidate.draws.resize(header.drawCount);
        candidate.materials.resize(header.materialCount);
        if (header.versionMinor == kPacketPhase6VersionMinor) {
            for (std::size_t index = 0;
                 index < candidate.vertices.size(); ++index) {
                RasterVertexV1 legacy{};
                std::memcpy(&legacy,
                    bytes.data() + header.verticesOffset +
                        index * sizeof(RasterVertexV1), sizeof(legacy));
                candidate.vertices[index] = RasterVertexV3{legacy};
            }
        } else if (header.versionMinor == kPacketTexCoordVersionMinor) {
            for (std::size_t index = 0;
                 index < candidate.vertices.size(); ++index) {
                RasterVertexV2 previous{};
                std::memcpy(&previous,
                    bytes.data() + header.verticesOffset +
                        index * sizeof(RasterVertexV2), sizeof(previous));
                candidate.vertices[index] = RasterVertexV3{previous};
            }
        } else {
            std::memcpy(candidate.vertices.data(),
                bytes.data() + header.verticesOffset, vertexBytes);
        }
        for (std::size_t vertexIndex = 0;
             vertexIndex < candidate.vertices.size(); ++vertexIndex) {
            const auto& vertex = candidate.vertices[vertexIndex];
            bool finite = true;
            for (const auto value : vertex.position) finite &= IsFinite(value);
            for (const auto value : vertex.color) finite &= IsFinite(value);
            for (const auto value : vertex.texCoord) finite &= IsFinite(value);
            if (!finite) {
                return {PacketError::InvalidVertex,
                    header.verticesOffset + vertexIndex * vertexStride};
            }
        }
        std::memcpy(candidate.draws.data(),
            bytes.data() + header.drawsOffset, drawBytes);
        std::memcpy(candidate.materials.data(),
            bytes.data() + header.materialsOffset, materialBytes);
        for (std::uint32_t index = 0; index < header.indexCount; ++index) {
            const auto offset = header.indicesOffset +
                index * static_cast<std::uint32_t>(header.indexType);
            if (header.indexType == IndexType::Uint16) {
                std::uint16_t value{};
                std::memcpy(&value, bytes.data() + offset, sizeof(value));
                candidate.indices[index] = value;
            } else {
                std::memcpy(&candidate.indices[index],
                    bytes.data() + offset, sizeof(std::uint32_t));
            }
        }
        for (std::size_t materialIndex = 0;
             materialIndex < candidate.materials.size(); ++materialIndex) {
            const auto& material = candidate.materials[materialIndex];
            if (material.resourceId == 0 ||
                material.shaderLayoutHash != kPhase6ShaderLayoutHash) {
                return {PacketError::ShaderLayoutMismatch,
                    header.materialsOffset +
                        materialIndex * sizeof(RasterMaterialV1),
                    material.resourceId};
            }
        }
        const MaterialRegistry registry{candidate.materials};
        if (registry.HasDuplicateIds()) {
            return {PacketError::DuplicateResource,
                header.materialsOffset};
        }
        for (std::size_t drawIndex = 0;
             drawIndex < candidate.draws.size(); ++drawIndex) {
            const auto& draw = candidate.draws[drawIndex];
            const auto drawOffset = header.drawsOffset +
                drawIndex * sizeof(RasterDrawV1);
            if (draw.indexCount == 0 || draw.indexCount % 3 != 0 ||
                draw.firstIndex > header.indexCount ||
                draw.indexCount > header.indexCount - draw.firstIndex ||
                (draw.frontFace != FrontFace::CounterClockwise &&
                    draw.frontFace != FrontFace::Clockwise) ||
                (draw.depthCompare != DepthCompare::Less &&
                    draw.depthCompare != DepthCompare::LessOrEqual &&
                    draw.depthCompare != DepthCompare::Always)) {
                return {PacketError::InvalidDrawRange, drawOffset};
            }
            if (registry.Resolve(draw.materialId) == nullptr) {
                return {PacketError::MissingMaterial,
                    drawOffset, draw.materialId};
            }
            for (std::uint32_t local = 0; local < draw.indexCount; ++local) {
                const auto raw = candidate.indices[draw.firstIndex + local];
                const auto adjusted = static_cast<std::int64_t>(raw) +
                    draw.vertexOffset;
                if (adjusted < 0 || adjusted >= header.vertexCount) {
                    return {PacketError::IndexOutOfRange,
                        header.indicesOffset +
                            (draw.firstIndex + local) *
                                static_cast<std::size_t>(header.indexType)};
                }
            }
            // Winding only exists once the vertices lie in the screen plane.
            // Camera-relative world geometry gets its winding from the view
            // transform, and real meshes always contain triangles standing
            // edge-on in XY, so applying the screen-plane rule there would
            // reject an entire captured scene for one such triangle.
            if (header.vertexSpace != VertexSpace::ScreenNdc) continue;
            for (std::uint32_t local = 0; local < draw.indexCount; local += 3) {
                const auto a = static_cast<std::size_t>(
                    static_cast<std::int64_t>(
                        candidate.indices[draw.firstIndex + local]) +
                    draw.vertexOffset);
                const auto b = static_cast<std::size_t>(
                    static_cast<std::int64_t>(
                        candidate.indices[draw.firstIndex + local + 1]) +
                    draw.vertexOffset);
                const auto c = static_cast<std::size_t>(
                    static_cast<std::int64_t>(
                        candidate.indices[draw.firstIndex + local + 2]) +
                    draw.vertexOffset);
                if (ClassifyTriangle(candidate.vertices[a],
                    candidate.vertices[b], candidate.vertices[c]) ==
                    TriangleWinding::Degenerate) {
                    return {PacketError::DegenerateTriangle, drawOffset};
                }
            }
        }
        decoded = std::move(candidate);
        return {};
    } catch (...) {
        decoded = {};
        return {PacketError::AllocationFailure};
    }
}

TriangleWinding ClassifyTriangle(
    const RasterVertexV1& a,
    const RasterVertexV1& b,
    const RasterVertexV1& c) noexcept
{
    const auto twiceArea =
        (b.position[0] - a.position[0]) *
            (c.position[1] - a.position[1]) -
        (b.position[1] - a.position[1]) *
            (c.position[0] - a.position[0]);
    constexpr float epsilon = 1.0e-7f;
    if (!std::isfinite(twiceArea) || std::abs(twiceArea) <= epsilon) {
        return TriangleWinding::Degenerate;
    }
    return twiceArea > 0.0f
        ? TriangleWinding::CounterClockwise
        : TriangleWinding::Clockwise;
}

RasterVertexV2::RasterVertexV2(const RasterVertexV1& source) noexcept
{
    std::memcpy(position, source.position, sizeof(position));
    std::memcpy(color, source.color, sizeof(color));
}

// Both migrations leave the normal at its +Z default rather than at zero. A
// packet written before normals existed has no normal to recover, and a zero
// one is not "no lighting": it is a division by zero wherever the shading
// normalises it, and the NaN spreads through the rest of the frame.
RasterVertexV3::RasterVertexV3(const RasterVertexV1& source) noexcept
{
    std::memcpy(position, source.position, sizeof(position));
    std::memcpy(color, source.color, sizeof(color));
}

RasterVertexV3::RasterVertexV3(const RasterVertexV2& source) noexcept
{
    std::memcpy(position, source.position, sizeof(position));
    std::memcpy(color, source.color, sizeof(color));
    std::memcpy(texCoord, source.texCoord, sizeof(texCoord));
}

TriangleWinding ClassifyTriangle(
    const RasterVertexV3& a,
    const RasterVertexV3& b,
    const RasterVertexV3& c) noexcept
{
    const auto twiceArea =
        (b.position[0] - a.position[0]) *
            (c.position[1] - a.position[1]) -
        (b.position[1] - a.position[1]) *
            (c.position[0] - a.position[0]);
    constexpr float epsilon = 1.0e-7f;
    if (!std::isfinite(twiceArea) || std::abs(twiceArea) <= epsilon) {
        return TriangleWinding::Degenerate;
    }
    return twiceArea > 0.0f
        ? TriangleWinding::CounterClockwise
        : TriangleWinding::Clockwise;
}

const char* ToString(const PacketError error) noexcept
{
    switch (error) {
    case PacketError::None: return "none";
    case PacketError::NotImplemented: return "not-implemented";
    case PacketError::TruncatedHeader: return "truncated-header";
    case PacketError::BadMagic: return "bad-magic";
    case PacketError::UnsupportedVersion: return "unsupported-version";
    case PacketError::SizeMismatch: return "size-mismatch";
    case PacketError::InvalidExtent: return "invalid-extent";
    case PacketError::InvalidViewport: return "invalid-viewport";
    case PacketError::InvalidScissor: return "invalid-scissor";
    case PacketError::InvalidIndexType: return "invalid-index-type";
    case PacketError::InvalidVertexSpace: return "invalid-vertex-space";
    case PacketError::MisalignedSection: return "misaligned-section";
    case PacketError::SectionOutOfBounds: return "section-out-of-bounds";
    case PacketError::InvalidDrawRange: return "invalid-draw-range";
    case PacketError::IndexOutOfRange: return "index-out-of-range";
    case PacketError::MissingMaterial: return "missing-material";
    case PacketError::ShaderLayoutMismatch: return "shader-layout-mismatch";
    case PacketError::DegenerateTriangle: return "degenerate-triangle";
    case PacketError::AllocationFailure: return "allocation-failure";
    case PacketError::DuplicateResource: return "duplicate-resource";
    case PacketError::InvalidVertex: return "invalid-vertex";
    }
    return "unknown";
}

MaterialRegistry::MaterialRegistry(
    const std::span<const RasterMaterialV1> materials) noexcept
    : materials_(materials)
{}

const RasterMaterialV1* MaterialRegistry::Resolve(
    const std::uint64_t resourceId) const noexcept
{
    const auto found = std::find_if(
        materials_.begin(), materials_.end(),
        [resourceId](const RasterMaterialV1& material) {
            return material.resourceId == resourceId;
        });
    return found == materials_.end() ? nullptr : &*found;
}

std::size_t MaterialRegistry::IndexOf(
    const std::uint64_t resourceId) const noexcept
{
    const auto found = Resolve(resourceId);
    return found == nullptr
        ? std::numeric_limits<std::size_t>::max()
        : static_cast<std::size_t>(found - materials_.data());
}

bool MaterialRegistry::HasDuplicateIds() const noexcept
{
    for (std::size_t first = 0; first < materials_.size(); ++first) {
        for (std::size_t second = first + 1;
             second < materials_.size(); ++second) {
            if (materials_[first].resourceId ==
                materials_[second].resourceId) {
                return true;
            }
        }
    }
    return false;
}

bool ExtentGeneration::Update(
    const std::uint32_t width,
    const std::uint32_t height) noexcept
{
    if (width == 0 || height == 0 ||
        width > kMaximumExtent || height > kMaximumExtent ||
        (width == width_ && height == height_)) {
        return false;
    }
    width_ = width;
    height_ = height;
    ++generation_;
    return true;
}

std::uint64_t ExtentGeneration::Generation() const noexcept
{
    return generation_;
}

std::uint32_t ExtentGeneration::Width() const noexcept
{
    return width_;
}

std::uint32_t ExtentGeneration::Height() const noexcept
{
    return height_;
}

UploadArena::UploadArena(const std::size_t capacity)
    : capacity_(capacity)
{}

UploadAllocation UploadArena::Allocate(
    const std::size_t size,
    const std::size_t alignment) noexcept
{
    if (size == 0 || alignment == 0 ||
        (alignment & (alignment - 1)) != 0) {
        return {};
    }
    const auto aligned = AlignUp(used_, alignment);
    if (aligned == std::numeric_limits<std::size_t>::max() ||
        aligned > capacity_ || size > capacity_ - aligned) {
        return {};
    }
    used_ = aligned + size;
    return UploadAllocation{aligned, size};
}

void UploadArena::Reset() noexcept
{
    used_ = 0;
}

std::size_t UploadArena::Used() const noexcept
{
    return used_;
}

std::size_t UploadArena::Capacity() const noexcept
{
    return capacity_;
}

std::uint64_t MaximumPacketBytes(
    const std::uint64_t reportedDeviceAllocationBytes) noexcept
{
    // Below the specification's floor the report is not believed. A device
    // that says it can allocate one byte has either not been queried or is
    // lying, and taking it at its word would reject every frame while looking
    // like a packet format problem.
    // Host addressability is not clamped here. The caller already refuses a
    // packet larger than size_t before consulting this ceiling, and repeating
    // that here would add a branch that cannot be reached on the only word
    // size this renderer builds for -- untestable code reading as a safeguard.
    return std::max(
        reportedDeviceAllocationBytes, kMinimumDeviceAllocationBytes);
}

}
