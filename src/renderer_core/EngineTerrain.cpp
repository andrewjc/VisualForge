#include "renderer_core/EngineTerrain.h"

#include "renderer_trace/Crc32.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <utility>

namespace vf::renderer::terrain {

namespace {

constexpr float kWeightTolerance = 1.0e-4f;
constexpr float kNormalTolerance = 1.0e-3f;
constexpr double kSeamTolerance = 1.0e-3;
// Mirrors the guard in shaders/phase14/terrain.frag.
constexpr float kMinimumWeightTotal = 1.0e-6f;

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

TerrainError ValidateLayers(const TerrainPacket& packet) noexcept
{
    if (packet.layers.empty() ||
        packet.layers.size() > kMaximumTerrainLayers) {
        return TerrainError::InvalidLayer;
    }
    std::vector<std::uint32_t> slices;
    slices.reserve(packet.layers.size());
    for (const auto& layer : packet.layers) {
        if ((layer.flags & ~kKnownLayerFlags) != 0) {
            return TerrainError::InvalidFlags;
        }
        if (layer.textureId == 0) return TerrainError::InvalidIdentity;
        if (!Finite(layer.uvScale, 2) ||
            layer.uvScale[0] <= 0.0f || layer.uvScale[1] <= 0.0f) {
            return TerrainError::InvalidLayer;
        }
        if (!std::isfinite(layer.roughness) || layer.roughness < 0.0f ||
            layer.roughness > 1.0f) {
            return TerrainError::InvalidLayer;
        }
        if (!std::isfinite(layer.normalStrength) ||
            layer.normalStrength < 0.0f) {
            return TerrainError::InvalidLayer;
        }
        // Two layers cannot occupy one texture-array slice; sampling would
        // silently read the wrong terrain texture.
        if (std::find(slices.begin(), slices.end(), layer.arraySlice) !=
            slices.end()) {
            return TerrainError::InvalidLayer;
        }
        slices.push_back(layer.arraySlice);
    }
    return TerrainError::None;
}

TerrainError ValidateCell(
    const TerrainPacket& packet,
    const TerrainCellV1& cell) noexcept
{
    if ((cell.flags & ~kKnownCellFlags) != 0) return TerrainError::InvalidFlags;
    if (cell.vertexCount == 0 || cell.indexCount == 0 ||
        cell.indexCount % 3 != 0) {
        return TerrainError::InvalidCell;
    }
    if (cell.vertexCount > packet.vertices.size() ||
        cell.firstVertex > packet.vertices.size() - cell.vertexCount) {
        return TerrainError::SectionOutOfBounds;
    }
    if (cell.indexCount > packet.indices.size() ||
        cell.firstIndex > packet.indices.size() - cell.indexCount) {
        return TerrainError::SectionOutOfBounds;
    }
    if (!std::isfinite(cell.originX) || !std::isfinite(cell.originY) ||
        !std::isfinite(cell.originZ)) {
        return TerrainError::InvalidCell;
    }
    // Full-resolution exterior cells sit exactly on the engine's grid. LOD
    // geometry spans several cells, so it is exempt from the stride check
    // rather than being forced into a guessed stride.
    if ((cell.flags & CellIsLodGeometry) == 0) {
        if (cell.originX != static_cast<double>(cell.gridX) * kCellWorldSize ||
            cell.originY != static_cast<double>(cell.gridY) * kCellWorldSize) {
            return TerrainError::InvalidCell;
        }
    }
    if (!std::isfinite(cell.boundsMinimumZ) ||
        !std::isfinite(cell.boundsMaximumZ) ||
        cell.boundsMaximumZ < cell.boundsMinimumZ) {
        return TerrainError::InvalidCell;
    }
    if (!std::isfinite(cell.lodMorphStart) ||
        !std::isfinite(cell.lodMorphEnd) ||
        cell.lodMorphStart < 0.0f ||
        cell.lodMorphEnd <= cell.lodMorphStart) {
        return TerrainError::InvalidLod;
    }
    if (cell.layerSlotCount == 0 ||
        cell.layerSlotCount > kLandChannelsPerVertex) {
        return TerrainError::LayerSlotMismatch;
    }
    for (std::uint32_t slot = 0; slot < kLandChannelsPerVertex; ++slot) {
        if (slot >= cell.layerSlotCount) {
            if (cell.layerSlots[slot] != 0) {
                return TerrainError::LayerSlotMismatch;
            }
            continue;
        }
        if (cell.layerSlots[slot] >= packet.layers.size()) {
            return TerrainError::MissingLayer;
        }
        for (std::uint32_t other = 0; other < slot; ++other) {
            if (cell.layerSlots[other] == cell.layerSlots[slot]) {
                return TerrainError::LayerSlotMismatch;
            }
        }
    }
    return TerrainError::None;
}

TerrainError ValidateCellVertices(
    const TerrainPacket& packet,
    const TerrainCellV1& cell) noexcept
{
    for (std::uint32_t offset = 0; offset < cell.vertexCount; ++offset) {
        const auto& vertex = packet.vertices[cell.firstVertex + offset];
        if (!Finite(vertex.position, 3) || !Finite(vertex.normal, 4) ||
            !Finite(vertex.color, 4) ||
            !Finite(vertex.channels, kLandChannelsPerVertex)) {
            return TerrainError::InvalidCell;
        }
        // Positions are cell relative on purpose. An absolute world position
        // here is the swimming-terrain defect, not a coordinate convention.
        for (std::size_t axis = 0; axis < 2; ++axis) {
            if (vertex.position[axis] < 0.0f ||
                vertex.position[axis] > static_cast<float>(kCellWorldSize)) {
                return TerrainError::VertexOutOfCell;
            }
        }
        if (vertex.position[2] < cell.boundsMinimumZ ||
            vertex.position[2] > cell.boundsMaximumZ) {
            return TerrainError::VertexOutOfCell;
        }
        const auto length = std::sqrt(
            vertex.normal[0] * vertex.normal[0] +
            vertex.normal[1] * vertex.normal[1] +
            vertex.normal[2] * vertex.normal[2]);
        if (std::abs(length - 1.0f) > kNormalTolerance) {
            return TerrainError::InvalidNormal;
        }
        float declared = 0.0f;
        for (std::uint32_t slot = 0; slot < kLandChannelsPerVertex; ++slot) {
            const auto weight = vertex.channels[slot];
            if (slot < cell.layerSlotCount) {
                if (weight < 0.0f || weight > 1.0f) {
                    return TerrainError::InvalidWeights;
                }
                declared += weight;
                continue;
            }
            // Land data outside the cell's declared slots is a channel we
            // have not classified. It is rejected rather than interpreted.
            if (weight != 0.0f) {
                return TerrainError::UnclassifiedLandChannel;
            }
        }
        if (declared <= kWeightTolerance) return TerrainError::InvalidWeights;
    }
    return TerrainError::None;
}

TerrainError ValidateCellIndices(
    const TerrainPacket& packet,
    const TerrainCellV1& cell) noexcept
{
    const auto first = cell.firstVertex;
    const auto last = cell.firstVertex + cell.vertexCount;
    for (std::uint32_t offset = 0; offset < cell.indexCount; ++offset) {
        const auto index = packet.indices[cell.firstIndex + offset];
        if (index >= packet.vertices.size()) {
            return TerrainError::IndexOutOfRange;
        }
        // A cell that references another cell's vertices would batch two
        // cells into one draw and lose its diagnostic identity.
        if (index < first || index >= last) {
            return TerrainError::IndexOutOfCell;
        }
    }
    return TerrainError::None;
}

double WorldX(const TerrainCellV1& cell, const LandscapeVertexV1& v) noexcept
{
    return cell.originX + static_cast<double>(v.position[0]);
}

double WorldY(const TerrainCellV1& cell, const LandscapeVertexV1& v) noexcept
{
    return cell.originY + static_cast<double>(v.position[1]);
}

struct SeamEntry
{
    std::size_t cellIndex{};
    double height{};
    std::uint32_t lodLevel{};
};

// One place where captured land channels become a normalized layer blend, so
// the packet evaluator and the reference rasterizer cannot drift apart from
// each other or from the terrain fragment shader.
struct LayerBlend
{
    std::array<float, kLandChannelsPerVertex> weights{};
    std::array<std::uint32_t, kLandChannelsPerVertex> arraySlices{};
    float roughness{};
    std::uint64_t dominantTextureId{};
    // True when the captured channels did not already sum to one.
    bool normalized{};
};

LayerBlend BlendLayers(
    const TerrainPacket& packet,
    const TerrainCellV1& cell,
    const std::span<const float, kLandChannelsPerVertex> channels) noexcept
{
    LayerBlend blend{};
    blend.arraySlices.fill(std::numeric_limits<std::uint32_t>::max());
    float total = 0.0f;
    for (std::uint32_t slot = 0; slot < cell.layerSlotCount; ++slot) {
        total += channels[slot];
    }
    blend.normalized = std::abs(total - 1.0f) > kWeightTolerance;
    // The shader divides by the same guarded total, so a degenerate blend
    // resolves identically on both sides instead of producing a NaN.
    total = std::max(total, kMinimumWeightTotal);
    double roughness = 0.0;
    float dominantWeight = -1.0f;
    for (std::uint32_t slot = 0; slot < cell.layerSlotCount; ++slot) {
        const auto weight = channels[slot] / total;
        const auto& layer = packet.layers[cell.layerSlots[slot]];
        blend.weights[slot] = weight;
        blend.arraySlices[slot] = layer.arraySlice;
        roughness += static_cast<double>(weight) * layer.roughness;
        if (weight > dominantWeight) {
            dominantWeight = weight;
            blend.dominantTextureId = layer.textureId;
        }
    }
    blend.roughness = std::clamp(static_cast<float>(roughness), 0.0f, 1.0f);
    return blend;
}

struct CellCoordinate
{
    std::int32_t gridX{};
    std::int32_t gridY{};
    std::uint32_t quadrant{};
    std::uint32_t lodLevel{};

    [[nodiscard]] bool operator==(const CellCoordinate&) const noexcept
        = default;
};

}

TerrainError ValidateTerrainPacket(const TerrainPacket& packet) noexcept
{
    if (packet.header.frameId == 0 || packet.header.viewId == 0) {
        return TerrainError::InvalidIdentity;
    }
    if (packet.header.captureThreadId == 0 ||
        packet.header.captureThreadId != packet.header.renderThreadId) {
        return TerrainError::WrongThread;
    }
    if (packet.cells.empty() || packet.cells.size() > kMaximumTerrainCells) {
        return TerrainError::InvalidCell;
    }
    if (packet.vertices.empty() ||
        packet.vertices.size() > kMaximumTerrainVertices) {
        return TerrainError::SectionOutOfBounds;
    }
    if (packet.indices.empty() ||
        packet.indices.size() > kMaximumTerrainIndices) {
        return TerrainError::SectionOutOfBounds;
    }
    const auto layerResult = ValidateLayers(packet);
    if (layerResult != TerrainError::None) return layerResult;

    std::vector<std::uint64_t> cellIds;
    // Compared as an exact tuple rather than a hash so two distinct cells can
    // never be rejected as duplicates by a collision.
    std::vector<CellCoordinate> coordinates;
    cellIds.reserve(packet.cells.size());
    coordinates.reserve(packet.cells.size());
    std::size_t vertexCursor = 0;
    std::size_t indexCursor = 0;
    for (const auto& cell : packet.cells) {
        const auto cellResult = ValidateCell(packet, cell);
        if (cellResult != TerrainError::None) return cellResult;
        if (std::find(cellIds.begin(), cellIds.end(), cell.cellId) !=
            cellIds.end()) {
            return TerrainError::DuplicateCell;
        }
        cellIds.push_back(cell.cellId);
        const CellCoordinate coordinate{
            cell.gridX, cell.gridY, cell.quadrant, cell.lodLevel};
        if (std::find(coordinates.begin(), coordinates.end(), coordinate) !=
            coordinates.end()) {
            return TerrainError::DuplicateCell;
        }
        coordinates.push_back(coordinate);
        // Cells tile their sections contiguously so no captured vertex is
        // orphaned and no two cells share storage.
        if (cell.firstVertex != vertexCursor ||
            cell.firstIndex != indexCursor) {
            return TerrainError::SectionOutOfBounds;
        }
        vertexCursor += cell.vertexCount;
        indexCursor += cell.indexCount;

        const auto vertexResult = ValidateCellVertices(packet, cell);
        if (vertexResult != TerrainError::None) return vertexResult;
        const auto indexResult = ValidateCellIndices(packet, cell);
        if (indexResult != TerrainError::None) return indexResult;
    }
    if (vertexCursor != packet.vertices.size() ||
        indexCursor != packet.indices.size()) {
        return TerrainError::SectionOutOfBounds;
    }
    return TerrainError::None;
}

std::uint32_t ResolveArraySlice(
    const TerrainPacket& packet,
    const TerrainCellV1& cell,
    const std::uint32_t slot) noexcept
{
    if (slot >= cell.layerSlotCount) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    const auto layerIndex = cell.layerSlots[slot];
    if (layerIndex >= packet.layers.size()) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return packet.layers[layerIndex].arraySlice;
}

float LodBlend(const TerrainCellV1& cell, const float distance) noexcept
{
    if (!std::isfinite(distance) || distance <= cell.lodMorphStart) {
        return 0.0f;
    }
    if (distance >= cell.lodMorphEnd) return 1.0f;
    const auto span = cell.lodMorphEnd - cell.lodMorphStart;
    if (span <= 0.0f) return 1.0f;
    return (distance - cell.lodMorphStart) / span;
}

TerrainError EvaluateTerrain(
    const TerrainPacket& packet,
    TerrainEvaluation& evaluation) noexcept
{
    evaluation = {};
    const auto validation = ValidateTerrainPacket(packet);
    if (validation != TerrainError::None) return validation;
    try {
        evaluation.cells.resize(packet.cells.size());
        evaluation.samples.resize(packet.vertices.size());
        std::map<std::pair<double, double>, std::vector<SeamEntry>> seams;

        for (std::size_t cellIndex = 0; cellIndex < packet.cells.size();
             ++cellIndex) {
            const auto& cell = packet.cells[cellIndex];
            auto& evaluated = evaluation.cells[cellIndex];
            evaluated.cellId = cell.cellId;
            evaluated.lodLevel = cell.lodLevel;
            bool first = true;
            for (std::uint32_t offset = 0; offset < cell.vertexCount;
                 ++offset) {
                const auto vertexIndex = cell.firstVertex + offset;
                const auto& vertex = packet.vertices[vertexIndex];
                const std::array<double, 3> world{
                    WorldX(cell, vertex),
                    WorldY(cell, vertex),
                    cell.originZ + static_cast<double>(vertex.position[2])};
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    evaluated.worldMinimum[axis] = first ? world[axis]
                        : std::min(evaluated.worldMinimum[axis], world[axis]);
                    evaluated.worldMaximum[axis] = first ? world[axis]
                        : std::max(evaluated.worldMaximum[axis], world[axis]);
                }
                first = false;
                evaluation.maximumLocalMagnitude = std::max({
                    evaluation.maximumLocalMagnitude,
                    std::abs(vertex.position[0]),
                    std::abs(vertex.position[1])});

                const auto blend = BlendLayers(packet, cell,
                    std::span<const float, kLandChannelsPerVertex>{
                        vertex.channels, kLandChannelsPerVertex});
                if (blend.normalized) ++evaluation.normalizedVertices;
                auto& sample = evaluation.samples[vertexIndex];
                sample.activeSlots = cell.layerSlotCount;
                sample.weights = blend.weights;
                sample.arraySlices = blend.arraySlices;
                sample.roughness = blend.roughness;
                const auto length = std::sqrt(
                    vertex.normal[0] * vertex.normal[0] +
                    vertex.normal[1] * vertex.normal[1] +
                    vertex.normal[2] * vertex.normal[2]);
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    sample.normal[axis] = vertex.normal[axis] / length;
                }

                seams[{world[0], world[1]}].push_back(
                    SeamEntry{cellIndex, world[2], cell.lodLevel});
            }
        }

        // Cracks appear where two cells meet and disagree on height, so the
        // comparison runs on exact double world positions rather than on the
        // float vertex stream.
        for (const auto& [position, entries] : seams) {
            static_cast<void>(position);
            for (std::size_t left = 0; left < entries.size(); ++left) {
                for (std::size_t right = left + 1; right < entries.size();
                     ++right) {
                    if (entries[left].cellIndex == entries[right].cellIndex) {
                        continue;
                    }
                    const auto crossesLod =
                        entries[left].lodLevel != entries[right].lodLevel;
                    ++evaluation.seamChecks;
                    if (crossesLod) ++evaluation.lodSeamChecks;
                    const auto gap = std::abs(
                        entries[left].height - entries[right].height);
                    evaluation.maximumSeamGap = std::max(
                        evaluation.maximumSeamGap, static_cast<float>(gap));
                    if (gap > kSeamTolerance) {
                        ++evaluation.seamMismatches;
                        if (crossesLod) ++evaluation.lodSeamMismatches;
                    }
                }
            }
        }
        return TerrainError::None;
    } catch (...) {
        evaluation = {};
        return TerrainError::AllocationFailure;
    }
}

TerrainError EncodeTerrainPacket(
    const TerrainPacket& packet,
    std::vector<std::byte>& bytes) noexcept
{
    bytes.clear();
    const auto validation = ValidateTerrainPacket(packet);
    if (validation != TerrainError::None) return validation;
    try {
        auto header = packet.header;
        header.magic = kTerrainPacketMagic;
        header.versionMajor = kTerrainPacketVersionMajor;
        header.versionMinor = kTerrainPacketVersionMinor;
        header.endianMarker = kTerrainPacketEndian;
        header.headerSize = sizeof(TerrainPacketHeaderV1);
        header.layerCount = static_cast<std::uint32_t>(packet.layers.size());
        header.cellCount = static_cast<std::uint32_t>(packet.cells.size());
        header.vertexCount = static_cast<std::uint32_t>(
            packet.vertices.size());
        header.indexCount = static_cast<std::uint32_t>(packet.indices.size());

        std::size_t cursor = AlignUp(sizeof(header), alignof(LandscapeLayerV1));
        std::size_t layerBytes{};
        std::size_t cellBytes{};
        std::size_t vertexBytes{};
        std::size_t indexBytes{};
        if (cursor == std::numeric_limits<std::size_t>::max() ||
            !CheckedMultiply(packet.layers.size(), sizeof(LandscapeLayerV1),
                layerBytes) ||
            !CheckedMultiply(packet.cells.size(), sizeof(TerrainCellV1),
                cellBytes) ||
            !CheckedMultiply(packet.vertices.size(),
                sizeof(LandscapeVertexV1), vertexBytes) ||
            !CheckedMultiply(packet.indices.size(), sizeof(std::uint32_t),
                indexBytes)) {
            return TerrainError::AllocationFailure;
        }
        header.layersOffset = static_cast<std::uint32_t>(cursor);
        if (!CheckedAdd(cursor, layerBytes, cursor)) {
            return TerrainError::AllocationFailure;
        }
        header.cellsOffset = static_cast<std::uint32_t>(cursor);
        if (!CheckedAdd(cursor, cellBytes, cursor)) {
            return TerrainError::AllocationFailure;
        }
        header.verticesOffset = static_cast<std::uint32_t>(cursor);
        if (!CheckedAdd(cursor, vertexBytes, cursor)) {
            return TerrainError::AllocationFailure;
        }
        header.indicesOffset = static_cast<std::uint32_t>(cursor);
        if (!CheckedAdd(cursor, indexBytes, cursor) ||
            cursor > std::numeric_limits<std::uint32_t>::max()) {
            return TerrainError::AllocationFailure;
        }
        header.totalSize = static_cast<std::uint32_t>(cursor);
        header.reserved0 = 0;
        header.reserved[0] = 0;
        header.reserved[1] = 0;

        bytes.assign(cursor, std::byte{0});
        std::memcpy(bytes.data() + header.layersOffset,
            packet.layers.data(), layerBytes);
        std::memcpy(bytes.data() + header.cellsOffset,
            packet.cells.data(), cellBytes);
        std::memcpy(bytes.data() + header.verticesOffset,
            packet.vertices.data(), vertexBytes);
        std::memcpy(bytes.data() + header.indicesOffset,
            packet.indices.data(), indexBytes);
        header.payloadCrc32 = trace::Crc32(
            std::span<const std::byte>{bytes}.subspan(sizeof(header)));
        std::memcpy(bytes.data(), &header, sizeof(header));
        return TerrainError::None;
    } catch (...) {
        bytes.clear();
        return TerrainError::AllocationFailure;
    }
}

TerrainError DecodeTerrainPacket(
    const std::span<const std::byte> bytes,
    TerrainPacket& packet) noexcept
{
    packet = {};
    if (bytes.size() < sizeof(TerrainPacketHeaderV1)) {
        return TerrainError::TruncatedHeader;
    }
    TerrainPacketHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kTerrainPacketMagic) return TerrainError::BadMagic;
    if (header.versionMajor != kTerrainPacketVersionMajor ||
        header.versionMinor > kTerrainPacketVersionMinor) {
        return TerrainError::UnsupportedVersion;
    }
    if (header.endianMarker != kTerrainPacketEndian) {
        return TerrainError::WrongEndian;
    }
    if (header.headerSize != sizeof(header)) {
        return TerrainError::SectionOutOfBounds;
    }
    if (header.totalSize != bytes.size()) return TerrainError::SizeMismatch;
    if (header.layerCount == 0 || header.layerCount > kMaximumTerrainLayers ||
        header.cellCount == 0 || header.cellCount > kMaximumTerrainCells ||
        header.vertexCount == 0 ||
        header.vertexCount > kMaximumTerrainVertices ||
        header.indexCount == 0 ||
        header.indexCount > kMaximumTerrainIndices) {
        return TerrainError::SectionOutOfBounds;
    }
    std::size_t cursor = AlignUp(sizeof(header), alignof(LandscapeLayerV1));
    std::size_t layerBytes{};
    std::size_t cellBytes{};
    std::size_t vertexBytes{};
    std::size_t indexBytes{};
    if (cursor == std::numeric_limits<std::size_t>::max() ||
        !CheckedMultiply(header.layerCount, sizeof(LandscapeLayerV1),
            layerBytes) ||
        !CheckedMultiply(header.cellCount, sizeof(TerrainCellV1), cellBytes) ||
        !CheckedMultiply(header.vertexCount, sizeof(LandscapeVertexV1),
            vertexBytes) ||
        !CheckedMultiply(header.indexCount, sizeof(std::uint32_t),
            indexBytes)) {
        return TerrainError::SectionOutOfBounds;
    }
    if (header.layersOffset != cursor) {
        return header.layersOffset % alignof(LandscapeLayerV1) != 0
            ? TerrainError::MisalignedSection
            : TerrainError::SectionOutOfBounds;
    }
    cursor += layerBytes;
    if (header.cellsOffset != cursor) {
        return header.cellsOffset % alignof(TerrainCellV1) != 0
            ? TerrainError::MisalignedSection
            : TerrainError::SectionOutOfBounds;
    }
    cursor += cellBytes;
    if (header.verticesOffset != cursor) {
        return header.verticesOffset % alignof(LandscapeVertexV1) != 0
            ? TerrainError::MisalignedSection
            : TerrainError::SectionOutOfBounds;
    }
    cursor += vertexBytes;
    if (header.indicesOffset != cursor) {
        return header.indicesOffset % alignof(std::uint32_t) != 0
            ? TerrainError::MisalignedSection
            : TerrainError::SectionOutOfBounds;
    }
    cursor += indexBytes;
    if (cursor != bytes.size()) return TerrainError::SizeMismatch;
    if (!std::all_of(bytes.begin() + sizeof(header),
            bytes.begin() + header.layersOffset,
            [](const std::byte value) { return value == std::byte{0}; })) {
        return TerrainError::NonZeroPadding;
    }
    if (trace::Crc32(bytes.subspan(sizeof(header))) != header.payloadCrc32) {
        return TerrainError::ChecksumMismatch;
    }
    try {
        TerrainPacket candidate{};
        candidate.header = header;
        candidate.layers.resize(header.layerCount);
        candidate.cells.resize(header.cellCount);
        candidate.vertices.resize(header.vertexCount);
        candidate.indices.resize(header.indexCount);
        std::memcpy(candidate.layers.data(),
            bytes.data() + header.layersOffset, layerBytes);
        std::memcpy(candidate.cells.data(),
            bytes.data() + header.cellsOffset, cellBytes);
        std::memcpy(candidate.vertices.data(),
            bytes.data() + header.verticesOffset, vertexBytes);
        std::memcpy(candidate.indices.data(),
            bytes.data() + header.indicesOffset, indexBytes);
        const auto validation = ValidateTerrainPacket(candidate);
        if (validation != TerrainError::None) return validation;
        packet = std::move(candidate);
        return TerrainError::None;
    } catch (...) {
        packet = {};
        return TerrainError::AllocationFailure;
    }
}

TerrainError BuildGpuTerrainCells(
    const TerrainPacket& packet,
    const std::span<const double, 3> cameraOrigin,
    std::vector<GpuTerrainCellV1>& records) noexcept
{
    records.clear();
    const auto validation = ValidateTerrainPacket(packet);
    if (validation != TerrainError::None) return validation;
    try {
        records.resize(packet.cells.size());
        for (std::size_t index = 0; index < packet.cells.size(); ++index) {
            const auto& cell = packet.cells[index];
            auto& record = records[index];
            record.cellId[0] = static_cast<std::uint32_t>(cell.cellId);
            record.cellId[1] = static_cast<std::uint32_t>(cell.cellId >> 32);
            record.lodLevel = cell.lodLevel;
            record.layerSlotCount = cell.layerSlotCount;
            // The subtraction happens in double; only the residual, which is
            // small by construction, is narrowed to float.
            record.cameraRelativeOrigin[0] = static_cast<float>(
                cell.originX - cameraOrigin[0]);
            record.cameraRelativeOrigin[1] = static_cast<float>(
                cell.originY - cameraOrigin[1]);
            record.cameraRelativeOrigin[2] = static_cast<float>(
                cell.originZ - cameraOrigin[2]);
            record.cameraRelativeOrigin[3] = 0.0f;
            std::copy(std::begin(cell.layerSlots), std::end(cell.layerSlots),
                std::begin(record.layerIndices));
        }
        return TerrainError::None;
    } catch (...) {
        records.clear();
        return TerrainError::AllocationFailure;
    }
}

namespace {

struct ShadedVertex
{
    std::array<float, 4> clip{};
    std::array<float, 3> screen{};
    float inverseW{};
    std::array<float, 3> normal{};
    std::array<float, 4> color{};
    std::array<float, 2> local{};
    std::array<float, kLandChannelsPerVertex> channels{};
};

float EdgeFunction(
    const std::array<float, 3>& a,
    const std::array<float, 3>& b,
    const float x,
    const float y) noexcept
{
    return (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
}

void WriteIdentity(
    const std::uint64_t value,
    std::uint32_t (&target)[2]) noexcept
{
    target[0] = static_cast<std::uint32_t>(value);
    target[1] = static_cast<std::uint32_t>(value >> 32);
}

}

TerrainError InitializeTerrainReference(
    const TerrainViewport& target,
    scene::GBufferImage& image) noexcept
{
    image = {};
    if (target.width == 0 || target.height == 0) {
        return TerrainError::InvalidCell;
    }
    try {
        image.width = target.width;
        image.height = target.height;
        image.pixels.resize(
            static_cast<std::size_t>(target.width) * target.height);
        for (auto& pixel : image.pixels) {
            // Matches the backend's shared world clear so uncovered pixels
            // agree with the mirrored G-buffer exactly.
            pixel.albedo[0] = 0.01f;
            pixel.albedo[1] = 0.021f;
            pixel.albedo[2] = 0.04f;
            pixel.albedo[3] = 1.0f;
            pixel.geometricNormalRoughness[3] = 1.0f;
            pixel.shadingNormalDepth[3] = 1.0f;
        }
        return TerrainError::None;
    } catch (...) {
        image = {};
        return TerrainError::AllocationFailure;
    }
}

TerrainError ComposeReferenceTerrainGBuffer(
    const TerrainPacket& packet,
    const view::ViewRecordV1& view,
    const texture::CapturedTexture& layerTextures,
    const TerrainViewport& target,
    scene::GBufferImage& image) noexcept
{
    const auto validation = ValidateTerrainPacket(packet);
    if (validation != TerrainError::None) return validation;
    if (target.width == 0 || target.height == 0 ||
        image.width != target.width || image.height != target.height ||
        image.pixels.size() !=
            static_cast<std::size_t>(target.width) * target.height) {
        return TerrainError::InvalidCell;
    }
    view::GpuViewConstantsV1 constants{};
    if (view::BuildGpuViewConstants(&view, 0, constants) !=
        view::ViewError::None) {
        return TerrainError::InvalidIdentity;
    }
    std::vector<GpuTerrainCellV1> records;
    const std::span<const double, 3> cameraOrigin{
        view.cameraRelativeOrigin, 3};
    const auto build = BuildGpuTerrainCells(packet, cameraOrigin, records);
    if (build != TerrainError::None) return build;

    try {
        std::vector<ShadedVertex> shaded(packet.vertices.size());
        for (std::size_t cellIndex = 0; cellIndex < packet.cells.size();
             ++cellIndex) {
            const auto& cell = packet.cells[cellIndex];
            const auto& record = records[cellIndex];
            for (std::uint32_t offset = 0; offset < cell.vertexCount;
                 ++offset) {
                const auto vertexIndex = cell.firstVertex + offset;
                const auto& source = packet.vertices[vertexIndex];
                auto& vertex = shaded[vertexIndex];
                const float position[4]{
                    record.cameraRelativeOrigin[0] + source.position[0],
                    record.cameraRelativeOrigin[1] + source.position[1],
                    record.cameraRelativeOrigin[2] + source.position[2],
                    1.0f};
                for (std::size_t row = 0; row < 4; ++row) {
                    float accumulated = 0.0f;
                    for (std::size_t column = 0; column < 4; ++column) {
                        accumulated +=
                            constants.viewProjectionRows[row * 4 + column] *
                            position[column];
                    }
                    vertex.clip[row] = accumulated;
                }
                if (!(vertex.clip[3] > 0.0f)) {
                    return TerrainError::InvalidCell;
                }
                vertex.inverseW = 1.0f / vertex.clip[3];
                const std::array<float, 3> ndc{
                    vertex.clip[0] * vertex.inverseW,
                    vertex.clip[1] * vertex.inverseW,
                    vertex.clip[2] * vertex.inverseW};
                vertex.screen = {
                    target.x + (ndc[0] * 0.5f + 0.5f) * target.viewportWidth,
                    target.y + (ndc[1] * 0.5f + 0.5f) * target.viewportHeight,
                    target.minDepth +
                        ndc[2] * (target.maxDepth - target.minDepth)};
                std::copy_n(source.normal, 3, vertex.normal.begin());
                std::copy_n(source.color, 4, vertex.color.begin());
                vertex.local = {source.position[0], source.position[1]};
                std::copy_n(source.channels, kLandChannelsPerVertex,
                    vertex.channels.begin());
            }
        }

        for (std::size_t cellIndex = 0; cellIndex < packet.cells.size();
             ++cellIndex) {
            const auto& cell = packet.cells[cellIndex];
            for (std::uint32_t local = 0; local < cell.indexCount;
                 local += 3) {
                const auto& a = shaded[
                    packet.indices[cell.firstIndex + local]];
                const auto& b = shaded[
                    packet.indices[cell.firstIndex + local + 1]];
                const auto& c = shaded[
                    packet.indices[cell.firstIndex + local + 2]];
                const auto area = EdgeFunction(
                    a.screen, b.screen, c.screen[0], c.screen[1]);
                // A positive-height Vulkan viewport flips winding in
                // framebuffer space, so back faces have positive area here.
                if (area >= 0.0f) continue;
                const auto minX = std::max<std::int32_t>(0,
                    static_cast<std::int32_t>(std::floor(std::min(
                        {a.screen[0], b.screen[0], c.screen[0]}))));
                const auto minY = std::max<std::int32_t>(0,
                    static_cast<std::int32_t>(std::floor(std::min(
                        {a.screen[1], b.screen[1], c.screen[1]}))));
                const auto maxX = std::min<std::int32_t>(
                    static_cast<std::int32_t>(target.width) - 1,
                    static_cast<std::int32_t>(std::ceil(std::max(
                        {a.screen[0], b.screen[0], c.screen[0]}))));
                const auto maxY = std::min<std::int32_t>(
                    static_cast<std::int32_t>(target.height) - 1,
                    static_cast<std::int32_t>(std::ceil(std::max(
                        {a.screen[1], b.screen[1], c.screen[1]}))));
                for (auto y = minY; y <= maxY; ++y) {
                    for (auto x = minX; x <= maxX; ++x) {
                        const auto sampleX = static_cast<float>(x) + 0.5f;
                        const auto sampleY = static_cast<float>(y) + 0.5f;
                        auto weightA = EdgeFunction(
                            b.screen, c.screen, sampleX, sampleY) / area;
                        auto weightB = EdgeFunction(
                            c.screen, a.screen, sampleX, sampleY) / area;
                        auto weightC = EdgeFunction(
                            a.screen, b.screen, sampleX, sampleY) / area;
                        constexpr float edgeTolerance = -1.0e-6f;
                        if (weightA < edgeTolerance ||
                            weightB < edgeTolerance ||
                            weightC < edgeTolerance) {
                            continue;
                        }
                        const auto depth = weightA * a.screen[2] +
                            weightB * b.screen[2] + weightC * c.screen[2];
                        auto& destination = image.pixels[
                            static_cast<std::size_t>(y) * image.width +
                            static_cast<std::size_t>(x)];
                        if (depth >= destination.shadingNormalDepth[3]) {
                            continue;
                        }
                        // Attributes are perspective correct, exactly as the
                        // rasterizer interpolates them.
                        const auto inverseW = weightA * a.inverseW +
                            weightB * b.inverseW + weightC * c.inverseW;
                        if (!(inverseW > 0.0f)) continue;
                        weightA = weightA * a.inverseW / inverseW;
                        weightB = weightB * b.inverseW / inverseW;
                        weightC = weightC * c.inverseW / inverseW;

                        std::array<float, kLandChannelsPerVertex> channels{};
                        for (std::size_t slot = 0;
                             slot < kLandChannelsPerVertex; ++slot) {
                            channels[slot] = weightA * a.channels[slot] +
                                weightB * b.channels[slot] +
                                weightC * c.channels[slot];
                        }
                        std::array<float, 2> localUv{
                            weightA * a.local[0] + weightB * b.local[0] +
                                weightC * c.local[0],
                            weightA * a.local[1] + weightB * b.local[1] +
                                weightC * c.local[1]};
                        std::array<float, 4> color{};
                        for (std::size_t channel = 0; channel < 4; ++channel) {
                            color[channel] = weightA * a.color[channel] +
                                weightB * b.color[channel] +
                                weightC * c.color[channel];
                        }
                        std::array<float, 3> normal{};
                        for (std::size_t axis = 0; axis < 3; ++axis) {
                            normal[axis] = weightA * a.normal[axis] +
                                weightB * b.normal[axis] +
                                weightC * c.normal[axis];
                        }
                        const auto normalLength = std::sqrt(
                            normal[0] * normal[0] + normal[1] * normal[1] +
                            normal[2] * normal[2]);
                        if (normalLength > 0.0f) {
                            for (auto& component : normal) {
                                component /= normalLength;
                            }
                        }

                        const auto blend = BlendLayers(packet, cell,
                            std::span<const float, kLandChannelsPerVertex>{
                                channels});
                        std::array<float, 3> albedo{};
                        for (std::uint32_t slot = 0;
                             slot < cell.layerSlotCount; ++slot) {
                            const auto& layer =
                                packet.layers[cell.layerSlots[slot]];
                            const auto u = localUv[0] /
                                static_cast<float>(kCellWorldSize) *
                                layer.uvScale[0];
                            const auto v = localUv[1] /
                                static_cast<float>(kCellWorldSize) *
                                layer.uvScale[1];
                            texture::SampledColor sampled{};
                            if (texture::SampleTexture2DArray(layerTextures,
                                    u, v, blend.arraySlices[slot], 0.0f,
                                    sampled) !=
                                texture::TexturePacketError::None) {
                                return TerrainError::MissingLayer;
                            }
                            albedo[0] += blend.weights[slot] * sampled.r;
                            albedo[1] += blend.weights[slot] * sampled.g;
                            albedo[2] += blend.weights[slot] * sampled.b;
                        }

                        for (std::size_t channel = 0; channel < 3; ++channel) {
                            const auto shadedChannel =
                                color[channel] * albedo[channel];
                            destination.albedo[channel] = std::clamp(
                                shadedChannel, 0.0f, 1.0f);
                            destination.geometricNormalRoughness[channel] =
                                normal[channel];
                            destination.shadingNormalDepth[channel] =
                                normal[channel];
                        }
                        destination.albedo[3] = std::clamp(
                            color[3], 0.0f, 1.0f);
                        destination.geometricNormalRoughness[3] =
                            blend.roughness;
                        destination.shadingNormalDepth[3] = depth;
                        WriteIdentity(cell.cellId, destination.objectId);
                        WriteIdentity(blend.dominantTextureId,
                            destination.materialId);
                    }
                }
            }
        }
        return TerrainError::None;
    } catch (...) {
        return TerrainError::AllocationFailure;
    }
}

TerrainError TerrainResidency::Load(const TerrainPacket& packet) noexcept
{
    const auto validation = ValidateTerrainPacket(packet);
    if (validation != TerrainError::None) return validation;
    try {
        for (const auto& cell : packet.cells) {
            const auto footprint =
                static_cast<std::uint64_t>(cell.vertexCount) *
                    sizeof(LandscapeVertexV1) +
                static_cast<std::uint64_t>(cell.indexCount) *
                    sizeof(std::uint32_t);
            auto& resident = resident_[cell.cellId];
            // Reloading a resident cell replaces its footprint; it does not
            // accumulate, which is what a residency leak would look like.
            residentBytes_ = residentBytes_ - resident + footprint;
            resident = footprint;
        }
        peakBytes_ = std::max(peakBytes_, residentBytes_);
        return TerrainError::None;
    } catch (...) {
        return TerrainError::AllocationFailure;
    }
}

TerrainError TerrainResidency::Unload(const std::uint64_t cellId) noexcept
{
    const auto entry = resident_.find(cellId);
    if (entry == resident_.end()) return TerrainError::UnknownCell;
    residentBytes_ -= entry->second;
    resident_.erase(entry);
    return TerrainError::None;
}

std::size_t TerrainResidency::ResidentCells() const noexcept
{
    return resident_.size();
}

std::uint64_t TerrainResidency::ResidentBytes() const noexcept
{
    return residentBytes_;
}

std::uint64_t TerrainResidency::PeakBytes() const noexcept
{
    return peakBytes_;
}

const char* ToString(const TerrainError error) noexcept
{
    switch (error) {
    case TerrainError::None: return "none";
    case TerrainError::TruncatedHeader: return "truncated header";
    case TerrainError::BadMagic: return "bad magic";
    case TerrainError::UnsupportedVersion: return "unsupported version";
    case TerrainError::WrongEndian: return "wrong endian";
    case TerrainError::SizeMismatch: return "size mismatch";
    case TerrainError::ChecksumMismatch: return "checksum mismatch";
    case TerrainError::SectionOutOfBounds: return "section out of bounds";
    case TerrainError::MisalignedSection: return "misaligned section";
    case TerrainError::NonZeroPadding: return "non-zero padding";
    case TerrainError::WrongThread: return "wrong thread";
    case TerrainError::InvalidIdentity: return "invalid identity";
    case TerrainError::InvalidFlags: return "invalid flags";
    case TerrainError::InvalidLayer: return "invalid layer";
    case TerrainError::MissingLayer: return "missing layer";
    case TerrainError::LayerSlotMismatch: return "layer slot mismatch";
    case TerrainError::InvalidWeights: return "invalid weights";
    case TerrainError::UnclassifiedLandChannel:
        return "unclassified land channel";
    case TerrainError::InvalidNormal: return "invalid normal";
    case TerrainError::InvalidLod: return "invalid lod";
    case TerrainError::InvalidCell: return "invalid cell";
    case TerrainError::DuplicateCell: return "duplicate cell";
    case TerrainError::VertexOutOfCell: return "vertex out of cell";
    case TerrainError::IndexOutOfRange: return "index out of range";
    case TerrainError::IndexOutOfCell: return "index out of cell";
    case TerrainError::UnknownCell: return "unknown cell";
    case TerrainError::AllocationFailure: return "allocation failure";
    }
    return "unknown";
}

}
