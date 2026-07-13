#include "renderer_core/EngineMesh.h"

#include "renderer_api/RasterPacket.h"
#include "renderer_core/EngineVertex.h"
#include "renderer_trace/Crc32.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace vf::renderer::mesh {

namespace {

constexpr std::uint64_t kDiagnosticMaterialSalt = 0x0710'0000'0000'0000ull;

std::size_t AlignUp(const std::size_t value, const std::size_t alignment) noexcept
{
    return (value + alignment - 1) & ~(alignment - 1);
}

bool HasRange(
    const std::span<const std::byte> bytes,
    const std::uint32_t offset,
    const std::uint32_t size) noexcept
{
    return offset <= bytes.size() && size <= bytes.size() - offset;
}

MeshPacketError ValidateSource(
    const CapturedMesh& mesh,
    EngineVertexLayout& layout) noexcept
{
    if (mesh.resourceId == 0 || mesh.generation == 0 ||
        mesh.vertexCount == 0 || mesh.stride == 0) {
        return MeshPacketError::InvalidResource;
    }
    if (mesh.usage != MeshUsage::Immutable && mesh.usage != MeshUsage::Dynamic) {
        return MeshPacketError::InvalidUsage;
    }
    if (ParseEngineVertexLayout(mesh.vertexDesc, layout) !=
        VertexLayoutError::None || layout.stride != mesh.stride) {
        return MeshPacketError::InvalidLayout;
    }
    if (mesh.vertexCount >
        std::numeric_limits<std::size_t>::max() / mesh.stride ||
        mesh.vertexBytes.size() !=
            static_cast<std::size_t>(mesh.vertexCount) * mesh.stride) {
        return MeshPacketError::InvalidVertexPayload;
    }
    if (mesh.firstIndex > mesh.indices.size() ||
        mesh.indexCount > mesh.indices.size() - mesh.firstIndex ||
        mesh.indexCount == 0 || mesh.indexCount % 3 != 0) {
        return MeshPacketError::InvalidDrawRange;
    }
    return MeshPacketError::None;
}

template <class T>
void Append(
    std::vector<std::byte>& bytes,
    const std::span<const T> values,
    std::uint32_t& offset)
{
    const auto aligned = AlignUp(bytes.size(), 8);
    bytes.resize(aligned);
    offset = static_cast<std::uint32_t>(aligned);
    const auto prior = bytes.size();
    bytes.resize(prior + values.size_bytes());
    if (!values.empty()) {
        std::memcpy(bytes.data() + prior, values.data(), values.size_bytes());
    }
}

}

MeshPacketError EncodeCapturedMesh(
    const CapturedMesh& mesh,
    std::vector<std::byte>& bytes) noexcept
{
    bytes.clear();
    EngineVertexLayout layout;
    const auto validation = ValidateSource(mesh, layout);
    if (validation != MeshPacketError::None &&
        validation != MeshPacketError::VertexOutOfRange) {
        return validation;
    }
    if (mesh.vertexBytes.size() > std::numeric_limits<std::uint32_t>::max() ||
        mesh.indices.size() >
            std::numeric_limits<std::uint32_t>::max() / sizeof(std::uint16_t)) {
        return MeshPacketError::AllocationFailure;
    }
    try {
        MeshPacketHeaderV1 header{};
        header.resourceId = mesh.resourceId;
        header.generation = mesh.generation;
        header.usage = mesh.usage;
        header.vertexDesc = mesh.vertexDesc;
        header.stride = mesh.stride;
        header.vertexCount = mesh.vertexCount;
        header.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
        header.firstIndex = mesh.firstIndex;
        header.drawIndexCount = mesh.indexCount;
        header.baseVertex = mesh.baseVertex;

        bytes.resize(sizeof(header));
        Append(bytes, std::span{mesh.vertexBytes}, header.vertexBytesOffset);
        header.vertexBytesSize = static_cast<std::uint32_t>(mesh.vertexBytes.size());
        Append(bytes, std::span{mesh.indices}, header.indicesOffset);
        header.indicesSize = static_cast<std::uint32_t>(
            mesh.indices.size() * sizeof(std::uint16_t));
        if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
            bytes.clear();
            return MeshPacketError::AllocationFailure;
        }
        header.totalSize = static_cast<std::uint32_t>(bytes.size());
        header.payloadCrc32 = vf::renderer::trace::Crc32(
            std::span{bytes}.subspan(header.headerSize));
        std::memcpy(bytes.data(), &header, sizeof(header));
        return MeshPacketError::None;
    } catch (...) {
        bytes.clear();
        return MeshPacketError::AllocationFailure;
    }
}

MeshPacketError DecodeCapturedMesh(
    const std::span<const std::byte> bytes,
    CapturedMesh& mesh) noexcept
{
    mesh = {};
    if (bytes.size() < sizeof(MeshPacketHeaderV1)) {
        return MeshPacketError::TruncatedHeader;
    }
    MeshPacketHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kMeshPacketMagic) return MeshPacketError::BadMagic;
    if (header.versionMajor != kMeshPacketVersionMajor ||
        header.versionMinor > kMeshPacketVersionMinor) {
        return MeshPacketError::UnsupportedVersion;
    }
    if (header.headerSize < sizeof(header) ||
        header.headerSize > header.totalSize || header.totalSize != bytes.size()) {
        return MeshPacketError::SizeMismatch;
    }
    if (header.vertexBytesOffset % 8 != 0 || header.indicesOffset % 8 != 0) {
        return MeshPacketError::MisalignedSection;
    }
    if (!HasRange(bytes, header.vertexBytesOffset, header.vertexBytesSize) ||
        !HasRange(bytes, header.indicesOffset, header.indicesSize) ||
        header.vertexBytesOffset < header.headerSize ||
        header.indicesOffset < header.headerSize) {
        return MeshPacketError::SectionOutOfBounds;
    }
    const auto verticesEnd = static_cast<std::size_t>(header.vertexBytesOffset) +
        header.vertexBytesSize;
    const auto indicesEnd = static_cast<std::size_t>(header.indicesOffset) +
        header.indicesSize;
    if (static_cast<std::size_t>(header.vertexBytesOffset) < indicesEnd &&
        static_cast<std::size_t>(header.indicesOffset) < verticesEnd) {
        return MeshPacketError::SectionOutOfBounds;
    }
    if (header.indicesSize !=
        static_cast<std::uint64_t>(header.indexCount) * sizeof(std::uint16_t)) {
        return MeshPacketError::SizeMismatch;
    }
    if (vf::renderer::trace::Crc32(bytes.subspan(header.headerSize)) !=
        header.payloadCrc32) {
        return MeshPacketError::ChecksumMismatch;
    }

    try {
        mesh.resourceId = header.resourceId;
        mesh.generation = header.generation;
        mesh.usage = header.usage;
        mesh.vertexDesc = header.vertexDesc;
        mesh.stride = header.stride;
        mesh.vertexCount = header.vertexCount;
        mesh.firstIndex = header.firstIndex;
        mesh.indexCount = header.drawIndexCount;
        mesh.baseVertex = header.baseVertex;
        mesh.vertexBytes.assign(
            bytes.begin() + header.vertexBytesOffset,
            bytes.begin() + header.vertexBytesOffset + header.vertexBytesSize);
        mesh.indices.resize(header.indexCount);
        if (header.indicesSize != 0) {
            std::memcpy(
                mesh.indices.data(), bytes.data() + header.indicesOffset,
                header.indicesSize);
        }
        EngineVertexLayout layout;
        return ValidateSource(mesh, layout);
    } catch (...) {
        mesh = {};
        return MeshPacketError::AllocationFailure;
    }
}

MeshPacketError TranslateCapturedMesh(
    const CapturedMesh& mesh,
    const std::uint32_t width,
    const std::uint32_t height,
    std::vector<std::byte>& rasterPacket,
    MeshTranslationReport& report) noexcept
{
    rasterPacket.clear();
    report = {};
    if (width == 0 || height == 0 ||
        width > vf::renderer::raster::kMaximumExtent ||
        height > vf::renderer::raster::kMaximumExtent) {
        return MeshPacketError::InvalidExtent;
    }
    EngineVertexLayout layout;
    const auto validation = ValidateSource(mesh, layout);
    if (validation != MeshPacketError::None) return validation;

    try {
        std::vector<std::uint32_t> remap(mesh.vertexCount,
            std::numeric_limits<std::uint32_t>::max());
        std::vector<DecodedEngineVertex> decoded;
        std::vector<std::uint32_t> translatedIndices;
        translatedIndices.reserve(mesh.indexCount);
        for (std::uint32_t ordinal = 0; ordinal < mesh.indexCount; ++ordinal) {
            const auto sourceIndex = static_cast<std::int64_t>(
                mesh.indices[mesh.firstIndex + ordinal]) + mesh.baseVertex;
            if (sourceIndex < 0 || sourceIndex >= mesh.vertexCount) {
                return MeshPacketError::VertexOutOfRange;
            }
            auto& mapped = remap[static_cast<std::size_t>(sourceIndex)];
            if (mapped == std::numeric_limits<std::uint32_t>::max()) {
                DecodedEngineVertex vertex{};
                const auto decodedResult = DecodeEngineVertex(
                    layout, mesh.vertexBytes,
                    static_cast<std::size_t>(sourceIndex), vertex);
                if (decodedResult == VertexDecodeError::NonFiniteValue) {
                    return MeshPacketError::NonFiniteVertex;
                }
                if (decodedResult != VertexDecodeError::None ||
                    !std::isfinite(vertex.position[0]) ||
                    !std::isfinite(vertex.position[1]) ||
                    !std::isfinite(vertex.position[2])) {
                    return MeshPacketError::InvalidVertexPayload;
                }
                mapped = static_cast<std::uint32_t>(decoded.size());
                decoded.push_back(vertex);
            }
            translatedIndices.push_back(mapped);
        }
        if (decoded.empty()) return MeshPacketError::InvalidDrawRange;

        report.sourceBounds.minimum = {
            decoded[0].position[0], decoded[0].position[1], decoded[0].position[2]};
        report.sourceBounds.maximum = report.sourceBounds.minimum;
        for (const auto& vertex : decoded) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                report.sourceBounds.minimum[axis] = std::min(
                    report.sourceBounds.minimum[axis], vertex.position[axis]);
                report.sourceBounds.maximum[axis] = std::max(
                    report.sourceBounds.maximum[axis], vertex.position[axis]);
            }
        }

        std::array<std::size_t, 3> axes{0, 1, 2};
        std::sort(axes.begin(), axes.end(), [&](const auto first, const auto second) {
            const auto firstRange = report.sourceBounds.maximum[first] -
                report.sourceBounds.minimum[first];
            const auto secondRange = report.sourceBounds.maximum[second] -
                report.sourceBounds.minimum[second];
            return firstRange > secondRange;
        });
        const auto range0 = report.sourceBounds.maximum[axes[0]] -
            report.sourceBounds.minimum[axes[0]];
        const auto range1 = report.sourceBounds.maximum[axes[1]] -
            report.sourceBounds.minimum[axes[1]];
        const auto scale = std::max(range0, range1);
        if (!(scale > 0.0f) || !std::isfinite(scale)) {
            return MeshPacketError::DegenerateTopology;
        }

        vf::renderer::raster::DecodedPacket packet{};
        packet.header.frameIndex = mesh.generation;
        packet.header.width = width;
        packet.header.height = height;
        packet.header.viewportWidth = static_cast<float>(width);
        packet.header.viewportHeight = static_cast<float>(height);
        packet.header.scissorWidth = width;
        packet.header.scissorHeight = height;
        packet.header.indexType = decoded.size() <=
            std::numeric_limits<std::uint16_t>::max()
            ? vf::renderer::raster::IndexType::Uint16
            : vf::renderer::raster::IndexType::Uint32;
        packet.indices = translatedIndices;
        packet.vertices.reserve(decoded.size());
        const auto colorAttribute = layout.Find(VertexSemantic::Color) != nullptr;
        const auto normalAttribute = layout.Find(VertexSemantic::Normal) != nullptr;
        for (const auto& source : decoded) {
            vf::renderer::raster::RasterVertexV3 vertex{};
            const auto center0 = (report.sourceBounds.minimum[axes[0]] +
                report.sourceBounds.maximum[axes[0]]) * 0.5f;
            const auto center1 = (report.sourceBounds.minimum[axes[1]] +
                report.sourceBounds.maximum[axes[1]]) * 0.5f;
            vertex.position[0] = (source.position[axes[0]] - center0) /
                scale * 1.6f;
            vertex.position[1] = (source.position[axes[1]] - center1) /
                scale * 1.6f;
            const auto depthRange = report.sourceBounds.maximum[axes[2]] -
                report.sourceBounds.minimum[axes[2]];
            vertex.position[2] = depthRange > 0.0f
                ? 0.15f + 0.7f *
                    (source.position[axes[2]] - report.sourceBounds.minimum[axes[2]]) /
                    depthRange
                : 0.5f;
            if (colorAttribute) {
                std::copy_n(source.color.begin(), 3, vertex.color);
            } else if (normalAttribute) {
                for (std::size_t component = 0; component < 3; ++component) {
                    vertex.color[component] = source.normal[component] * 0.5f + 0.5f;
                }
            } else {
                vertex.color[0] = vertex.position[0] * 0.35f + 0.5f;
                vertex.color[1] = vertex.position[1] * 0.35f + 0.5f;
                vertex.color[2] = 0.85f;
            }
            if (layout.Find(VertexSemantic::TexCoord0) != nullptr) {
                vertex.texCoord[0] = source.texCoord0[0];
                vertex.texCoord[1] = source.texCoord0[1];
            }
            packet.vertices.push_back(vertex);
        }

        auto winding = vf::renderer::raster::TriangleWinding::Degenerate;
        for (std::size_t index = 0; index < packet.indices.size(); index += 3) {
            winding = vf::renderer::raster::ClassifyTriangle(
                packet.vertices[packet.indices[index]],
                packet.vertices[packet.indices[index + 1]],
                packet.vertices[packet.indices[index + 2]]);
            if (winding != vf::renderer::raster::TriangleWinding::Degenerate) break;
        }
        if (winding == vf::renderer::raster::TriangleWinding::Degenerate) {
            return MeshPacketError::DegenerateTopology;
        }
        report.clockwise = winding ==
            vf::renderer::raster::TriangleWinding::Clockwise;
        const auto materialId = mesh.resourceId ^ kDiagnosticMaterialSalt;
        packet.materials.push_back(vf::renderer::raster::RasterMaterialV1{
            materialId,
            vf::renderer::raster::kPhase6ShaderLayoutHash,
            {1.0f, 1.0f, 1.0f, 1.0f},
        });
        packet.draws.push_back(vf::renderer::raster::RasterDrawV1{
            materialId,
            0,
            static_cast<std::uint32_t>(packet.indices.size()),
            0,
            report.clockwise
                ? vf::renderer::raster::FrontFace::Clockwise
                : vf::renderer::raster::FrontFace::CounterClockwise,
            vf::renderer::raster::DepthCompare::LessOrEqual,
            0,
        });
        const auto encoded = vf::renderer::raster::EncodePacket(
            packet, rasterPacket);
        if (!encoded) return MeshPacketError::RasterPacketRejected;
        report.translatedVertexCount =
            static_cast<std::uint32_t>(packet.vertices.size());
        report.translatedIndexCount =
            static_cast<std::uint32_t>(packet.indices.size());
        report.sourceAttributeCount =
            static_cast<std::uint32_t>(layout.attributes.size());
        return MeshPacketError::None;
    } catch (...) {
        rasterPacket.clear();
        return MeshPacketError::AllocationFailure;
    }
}

const char* ToString(const MeshPacketError error) noexcept
{
    switch (error) {
    case MeshPacketError::None: return "none";
    case MeshPacketError::TruncatedHeader: return "truncated-header";
    case MeshPacketError::BadMagic: return "bad-magic";
    case MeshPacketError::UnsupportedVersion: return "unsupported-version";
    case MeshPacketError::SizeMismatch: return "size-mismatch";
    case MeshPacketError::InvalidResource: return "invalid-resource";
    case MeshPacketError::InvalidUsage: return "invalid-usage";
    case MeshPacketError::InvalidLayout: return "invalid-layout";
    case MeshPacketError::InvalidVertexPayload: return "invalid-vertex-payload";
    case MeshPacketError::MisalignedSection: return "misaligned-section";
    case MeshPacketError::SectionOutOfBounds: return "section-out-of-bounds";
    case MeshPacketError::ChecksumMismatch: return "checksum-mismatch";
    case MeshPacketError::InvalidDrawRange: return "invalid-draw-range";
    case MeshPacketError::VertexOutOfRange: return "vertex-out-of-range";
    case MeshPacketError::NonFiniteVertex: return "non-finite-vertex";
    case MeshPacketError::DegenerateTopology: return "degenerate-topology";
    case MeshPacketError::InvalidExtent: return "invalid-extent";
    case MeshPacketError::AllocationFailure: return "allocation-failure";
    case MeshPacketError::RasterPacketRejected: return "raster-packet-rejected";
    }
    return "unknown";
}

}
