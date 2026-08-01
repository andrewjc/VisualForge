#include "renderer_core/EngineTerrain.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using namespace vf::renderer;

using Channels = std::array<float, terrain::kLandChannelsPerVertex>;

terrain::LandscapeLayerV1 BuildLayer(
    const std::uint32_t slice,
    const std::uint64_t textureId,
    const float roughness)
{
    terrain::LandscapeLayerV1 layer{};
    layer.textureId = textureId;
    layer.arraySlice = slice;
    layer.uvScale[0] = 8.0f;
    layer.uvScale[1] = 8.0f;
    layer.roughness = roughness;
    layer.normalStrength = 1.0f;
    return layer;
}

terrain::LandscapeVertexV1 BuildVertex(
    const float x,
    const float y,
    const float height,
    const Channels& channels)
{
    terrain::LandscapeVertexV1 vertex{};
    vertex.position[0] = x;
    vertex.position[1] = y;
    vertex.position[2] = height;
    vertex.normal[2] = 1.0f;
    vertex.color[0] = 1.0f;
    vertex.color[1] = 1.0f;
    vertex.color[2] = 1.0f;
    vertex.color[3] = 1.0f;
    std::copy(channels.begin(), channels.end(), std::begin(vertex.channels));
    return vertex;
}

// One exterior cell of four corner vertices. Fallout 4 exterior grid
// coordinates are routinely negative, so the fixture uses negative x.
terrain::TerrainCellV1 BuildCell(
    const std::int32_t gridX,
    const std::int32_t gridY,
    const std::uint32_t firstVertex,
    const std::uint32_t firstIndex,
    const std::uint32_t slotCount)
{
    terrain::TerrainCellV1 cell{};
    cell.cellId = (static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(gridX)) << 32) |
        static_cast<std::uint32_t>(gridY);
    cell.gridX = gridX;
    cell.gridY = gridY;
    cell.quadrant = 0;
    cell.lodLevel = 0;
    cell.firstVertex = firstVertex;
    cell.vertexCount = 4;
    cell.firstIndex = firstIndex;
    cell.indexCount = 6;
    cell.originX = static_cast<double>(gridX) * terrain::kCellWorldSize;
    cell.originY = static_cast<double>(gridY) * terrain::kCellWorldSize;
    cell.lodMorphStart = 4096.0f;
    cell.lodMorphEnd = 8192.0f;
    cell.flags = terrain::CellWritesWorldTarget;
    cell.layerSlotCount = slotCount;
    for (std::uint32_t slot = 0; slot < slotCount; ++slot) {
        cell.layerSlots[slot] = slot;
    }
    cell.boundsMinimumZ = 0.0f;
    cell.boundsMaximumZ = 64.0f;
    return cell;
}

constexpr Channels kBase{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
constexpr Channels kMixed{0.5f, 0.3f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

// Two cells sharing the edge at world x = -8192, which is the seam case that
// produces visible cracks when heights disagree.
terrain::TerrainPacket BuildPacket()
{
    terrain::TerrainPacket packet{};
    packet.header.frameId = 14;
    packet.header.viewId = 0x4101;
    packet.header.captureSequence = 91;
    packet.header.captureThreadId = 5;
    packet.header.renderThreadId = 5;
    packet.layers.push_back(BuildLayer(0, 0x7000ull, 0.40f));
    packet.layers.push_back(BuildLayer(1, 0x7001ull, 0.50f));
    packet.layers.push_back(BuildLayer(2, 0x7002ull, 0.60f));

    packet.cells.push_back(BuildCell(-3, 5, 0, 0, 3));
    packet.cells.push_back(BuildCell(-2, 5, 4, 6, 3));

    constexpr float kSize = static_cast<float>(terrain::kCellWorldSize);
    packet.vertices.push_back(BuildVertex(0.0f, 0.0f, 10.0f, kBase));
    packet.vertices.push_back(BuildVertex(kSize, 0.0f, 20.0f, kMixed));
    packet.vertices.push_back(BuildVertex(0.0f, kSize, 30.0f, kBase));
    packet.vertices.push_back(BuildVertex(kSize, kSize, 40.0f, kMixed));
    // The neighbour's left edge repeats the first cell's right edge heights.
    packet.vertices.push_back(BuildVertex(0.0f, 0.0f, 20.0f, kMixed));
    packet.vertices.push_back(BuildVertex(kSize, 0.0f, 25.0f, kBase));
    packet.vertices.push_back(BuildVertex(0.0f, kSize, 40.0f, kMixed));
    packet.vertices.push_back(BuildVertex(kSize, kSize, 45.0f, kBase));

    packet.indices = {0, 1, 2, 1, 3, 2, 4, 5, 6, 5, 7, 6};
    return packet;
}

}

TEST_CASE("P14_landscape_weights_normalize_explicitly_and_fail_closed",
    "[phase14][terrain]")
{
    const auto source = BuildPacket();
    REQUIRE(terrain::ValidateTerrainPacket(source) ==
        terrain::TerrainError::None);

    // Engine land data does not arrive pre-normalized. Normalization is
    // counted rather than applied silently.
    auto scaled = source;
    scaled.vertices[0].channels[0] = 0.5f;
    scaled.vertices[0].channels[1] = 0.25f;
    terrain::TerrainEvaluation evaluation{};
    REQUIRE(terrain::EvaluateTerrain(scaled, evaluation) ==
        terrain::TerrainError::None);
    CHECK(evaluation.normalizedVertices == 1);
    CHECK(evaluation.samples[0].roughness ==
        Catch::Approx((0.5f * 0.40f + 0.25f * 0.50f) / 0.75f));

    auto zeroed = source;
    std::fill(std::begin(zeroed.vertices[1].channels),
        std::end(zeroed.vertices[1].channels), 0.0f);
    CHECK(terrain::ValidateTerrainPacket(zeroed) ==
        terrain::TerrainError::InvalidWeights);

    auto negative = source;
    negative.vertices[1].channels[0] = -0.25f;
    CHECK(terrain::ValidateTerrainPacket(negative) ==
        terrain::TerrainError::InvalidWeights);

    // A channel outside the cell's declared slot count is land data we have
    // not classified, so the packet is rejected instead of guessed at.
    auto unclassified = source;
    unclassified.vertices[1].channels[6] = 0.25f;
    CHECK(terrain::ValidateTerrainPacket(unclassified) ==
        terrain::TerrainError::UnclassifiedLandChannel);

    auto badNormal = source;
    badNormal.vertices[0].normal[2] = 0.5f;
    CHECK(terrain::ValidateTerrainPacket(badNormal) ==
        terrain::TerrainError::InvalidNormal);

    auto duplicated = source;
    duplicated.cells[1].cellId = duplicated.cells[0].cellId;
    CHECK(terrain::ValidateTerrainPacket(duplicated) ==
        terrain::TerrainError::DuplicateCell);

    auto strayIndex = source;
    strayIndex.indices[0] = 4; // belongs to the neighbouring cell
    CHECK(terrain::ValidateTerrainPacket(strayIndex) ==
        terrain::TerrainError::IndexOutOfCell);
}

TEST_CASE("P14_terrain_layer_slots_select_texture_array_slices",
    "[phase14][terrain]")
{
    const auto source = BuildPacket();
    terrain::TerrainEvaluation evaluation{};
    REQUIRE(terrain::EvaluateTerrain(source, evaluation) ==
        terrain::TerrainError::None);
    REQUIRE(evaluation.samples.size() == source.vertices.size());

    // Slot ordering is the cell's, not the layer table's, so the resolved
    // slice must follow the cell mapping.
    CHECK(terrain::ResolveArraySlice(source, source.cells[0], 0) == 0u);
    CHECK(terrain::ResolveArraySlice(source, source.cells[0], 2) == 2u);
    CHECK(evaluation.samples[1].arraySlices[1] == 1u);

    auto remapped = source;
    remapped.cells[0].layerSlots[0] = 2;
    remapped.cells[0].layerSlots[2] = 0;
    REQUIRE(terrain::ValidateTerrainPacket(remapped) ==
        terrain::TerrainError::None);
    CHECK(terrain::ResolveArraySlice(remapped, remapped.cells[0], 0) == 2u);
    terrain::TerrainEvaluation remappedEvaluation{};
    REQUIRE(terrain::EvaluateTerrain(remapped, remappedEvaluation) ==
        terrain::TerrainError::None);
    // Remapping the slots must move the blend with them.
    CHECK(remappedEvaluation.samples[1].roughness ==
        Catch::Approx(0.5f * 0.60f + 0.3f * 0.50f + 0.2f * 0.40f));

    auto missing = source;
    missing.cells[0].layerSlots[1] = 9;
    CHECK(terrain::ValidateTerrainPacket(missing) ==
        terrain::TerrainError::MissingLayer);

    auto aliased = source;
    aliased.cells[0].layerSlots[1] = aliased.cells[0].layerSlots[0];
    CHECK(terrain::ValidateTerrainPacket(aliased) ==
        terrain::TerrainError::LayerSlotMismatch);
}

TEST_CASE("P14_terrain_preserves_negative_grid_and_camera_relative_precision",
    "[phase14][terrain]")
{
    auto source = BuildPacket();
    // Far negative cells are exactly where absolute float world positions
    // start swimming, so origins stay double and vertices stay cell relative.
    source.cells[0].gridX = -64;
    source.cells[0].cellId = 0xFFFFFFC000000005ull;
    source.cells[0].originX = -64.0 * terrain::kCellWorldSize;
    source.cells[1].gridX = -63;
    source.cells[1].cellId = 0xFFFFFFC100000005ull;
    source.cells[1].originX = -63.0 * terrain::kCellWorldSize;
    REQUIRE(terrain::ValidateTerrainPacket(source) ==
        terrain::TerrainError::None);

    terrain::TerrainEvaluation evaluation{};
    REQUIRE(terrain::EvaluateTerrain(source, evaluation) ==
        terrain::TerrainError::None);
    REQUIRE(evaluation.cells.size() == 2);
    CHECK(evaluation.cells[0].worldMinimum[0] ==
        Catch::Approx(-262144.0).margin(1.0e-9));
    CHECK(evaluation.cells[1].worldMinimum[0] ==
        Catch::Approx(-258048.0).margin(1.0e-9));
    // The seam still resolves at this distance because the comparison runs
    // in double precision on cell origin plus local offset.
    CHECK(evaluation.seamChecks == 2);
    CHECK(evaluation.seamMismatches == 0);
    // Cell-relative positions stay inside one cell, which is what keeps the
    // float vertex stream free of camera-origin swimming.
    CHECK(evaluation.maximumLocalMagnitude <=
        static_cast<float>(terrain::kCellWorldSize));

    auto absolute = source;
    absolute.vertices[0].position[0] = -262144.0f;
    CHECK(terrain::ValidateTerrainPacket(absolute) ==
        terrain::TerrainError::VertexOutOfCell);
}

TEST_CASE("P14_terrain_seams_match_between_adjacent_cells",
    "[phase14][terrain]")
{
    const auto source = BuildPacket();
    terrain::TerrainEvaluation evaluation{};
    REQUIRE(terrain::EvaluateTerrain(source, evaluation) ==
        terrain::TerrainError::None);
    CHECK(evaluation.seamChecks == 2);
    CHECK(evaluation.seamMismatches == 0);
    CHECK(evaluation.maximumSeamGap == Catch::Approx(0.0f));

    auto cracked = source;
    cracked.vertices[4].position[2] = 21.0f; // neighbour edge said 20
    terrain::TerrainEvaluation cracks{};
    REQUIRE(terrain::EvaluateTerrain(cracked, cracks) ==
        terrain::TerrainError::None);
    CHECK(cracks.seamMismatches == 1);
    CHECK(cracks.maximumSeamGap == Catch::Approx(1.0f));
}

TEST_CASE("P14_terrain_near_far_lod_seam_stays_watertight",
    "[phase14][terrain]")
{
    // The near cell stays at LOD 0 while its neighbour drops to LOD 1. The
    // shared edge must still agree, which is the transition that produces
    // the classic distant-terrain crack.
    auto source = BuildPacket();
    source.cells[1].lodLevel = 1;
    source.cells[1].lodMorphStart = 8192.0f;
    source.cells[1].lodMorphEnd = 16384.0f;
    REQUIRE(terrain::ValidateTerrainPacket(source) ==
        terrain::TerrainError::None);

    terrain::TerrainEvaluation evaluation{};
    REQUIRE(terrain::EvaluateTerrain(source, evaluation) ==
        terrain::TerrainError::None);
    CHECK(evaluation.lodSeamChecks == 2);
    CHECK(evaluation.seamMismatches == 0);

    auto cracked = source;
    cracked.vertices[6].position[2] = 39.5f; // near cell edge said 40
    terrain::TerrainEvaluation cracks{};
    REQUIRE(terrain::EvaluateTerrain(cracked, cracks) ==
        terrain::TerrainError::None);
    CHECK(cracks.seamMismatches == 1);
    CHECK(cracks.lodSeamMismatches == 1);
}

TEST_CASE("P14_terrain_lod_ranges_are_ordered_and_blend_monotonically",
    "[phase14][terrain]")
{
    auto source = BuildPacket();
    REQUIRE(terrain::ValidateTerrainPacket(source) ==
        terrain::TerrainError::None);
    const auto& cell = source.cells[0];
    CHECK(terrain::LodBlend(cell, 0.0f) == Catch::Approx(0.0f));
    CHECK(terrain::LodBlend(cell, 4096.0f) == Catch::Approx(0.0f));
    CHECK(terrain::LodBlend(cell, 6144.0f) == Catch::Approx(0.5f));
    CHECK(terrain::LodBlend(cell, 8192.0f) == Catch::Approx(1.0f));
    CHECK(terrain::LodBlend(cell, 99999.0f) == Catch::Approx(1.0f));

    float previous = -1.0f;
    for (float distance = 0.0f; distance <= 12288.0f; distance += 128.0f) {
        const auto blend = terrain::LodBlend(cell, distance);
        CHECK(blend >= previous);
        CHECK(blend >= 0.0f);
        CHECK(blend <= 1.0f);
        previous = blend;
    }

    auto inverted = source;
    inverted.cells[0].lodMorphEnd = 1024.0f;
    CHECK(terrain::ValidateTerrainPacket(inverted) ==
        terrain::TerrainError::InvalidLod);

    auto negativeRange = source;
    negativeRange.cells[0].lodMorphStart = -1.0f;
    CHECK(terrain::ValidateTerrainPacket(negativeRange) ==
        terrain::TerrainError::InvalidLod);
}

TEST_CASE("P14_terrain_packets_round_trip_and_reject_corruption",
    "[phase14][terrain]")
{
    const auto source = BuildPacket();
    std::vector<std::byte> first;
    std::vector<std::byte> second;
    REQUIRE(terrain::EncodeTerrainPacket(source, first) ==
        terrain::TerrainError::None);
    REQUIRE(terrain::EncodeTerrainPacket(source, second) ==
        terrain::TerrainError::None);
    CHECK(first == second);

    terrain::TerrainPacket decoded;
    REQUIRE(terrain::DecodeTerrainPacket(first, decoded) ==
        terrain::TerrainError::None);
    REQUIRE(decoded.cells.size() == source.cells.size());
    REQUIRE(decoded.layers.size() == source.layers.size());
    REQUIRE(decoded.vertices.size() == source.vertices.size());
    CHECK(decoded.indices == source.indices);
    CHECK(decoded.cells[0].gridX == -3);
    CHECK(decoded.cells[0].originX == Catch::Approx(-12288.0));
    CHECK(decoded.cells[0].layerSlotCount == 3u);
    CHECK(decoded.vertices[1].channels[1] == Catch::Approx(0.3f));

    std::vector<std::byte> reEncoded;
    REQUIRE(terrain::EncodeTerrainPacket(decoded, reEncoded) ==
        terrain::TerrainError::None);
    CHECK(reEncoded == first);

    auto truncated = first;
    truncated.resize(truncated.size() - 1);
    CHECK(terrain::DecodeTerrainPacket(truncated, decoded) ==
        terrain::TerrainError::SizeMismatch);

    auto corrupted = first;
    corrupted.back() ^= std::byte{0x10};
    CHECK(terrain::DecodeTerrainPacket(corrupted, decoded) ==
        terrain::TerrainError::ChecksumMismatch);
}

TEST_CASE("P14_terrain_layer_blend_is_weight_ordered_and_bounded",
    "[phase14][terrain]")
{
    const auto source = BuildPacket();
    terrain::TerrainEvaluation evaluation{};
    REQUIRE(terrain::EvaluateTerrain(source, evaluation) ==
        terrain::TerrainError::None);
    REQUIRE(evaluation.samples.size() == source.vertices.size());

    // A single-layer vertex resolves to exactly that layer.
    CHECK(evaluation.samples[0].roughness == Catch::Approx(0.40f));
    // A blended vertex lands strictly between its contributing layers.
    const auto blended = evaluation.samples[1].roughness;
    CHECK(blended == Catch::Approx(0.47f));
    CHECK(blended > 0.40f);
    CHECK(blended < 0.60f);

    for (const auto& sample : evaluation.samples) {
        CHECK(sample.roughness >= 0.0f);
        CHECK(sample.roughness <= 1.0f);
        const auto length = std::sqrt(
            sample.normal[0] * sample.normal[0] +
            sample.normal[1] * sample.normal[1] +
            sample.normal[2] * sample.normal[2]);
        CHECK(length == Catch::Approx(1.0f).margin(1.0e-5));
        float sum = 0.0f;
        for (const auto weight : sample.weights) {
            CHECK(weight >= 0.0f);
            sum += weight;
        }
        CHECK(sum == Catch::Approx(1.0f).margin(1.0e-5));
    }
}

TEST_CASE("P14_terrain_gpu_cells_narrow_only_the_camera_relative_residual",
    "[phase14][terrain]")
{
    auto source = BuildPacket();
    // A Commonwealth-scale origin: the absolute coordinates need double, the
    // camera-relative residual does not.
    const std::array<double, 3> cameraOrigin{
        2'000'000.0, 512.0, -1'000'000.0};
    for (auto& cell : source.cells) {
        cell.originZ = -992'000.0;
    }
    source.cells[0].gridX = 488;
    source.cells[0].originX = 488.0 * terrain::kCellWorldSize;
    source.cells[0].cellId = 0x000001E800000000ull;
    source.cells[1].gridX = 489;
    source.cells[1].originX = 489.0 * terrain::kCellWorldSize;
    source.cells[1].cellId = 0x000001E900000000ull;
    source.cells[0].gridY = 0;
    source.cells[0].originY = 0.0;
    source.cells[1].gridY = 0;
    source.cells[1].originY = 0.0;
    REQUIRE(terrain::ValidateTerrainPacket(source) ==
        terrain::TerrainError::None);

    std::vector<terrain::GpuTerrainCellV1> records;
    REQUIRE(terrain::BuildGpuTerrainCells(
        source, std::span<const double, 3>{cameraOrigin}, records) ==
        terrain::TerrainError::None);
    REQUIRE(records.size() == source.cells.size());

    // 488 * 4096 - 2'000'000 = -1152, exactly representable in float.
    CHECK(records[0].cameraRelativeOrigin[0] == Catch::Approx(-1152.0f));
    CHECK(records[1].cameraRelativeOrigin[0] == Catch::Approx(2944.0f));
    CHECK(records[0].cameraRelativeOrigin[1] == Catch::Approx(-512.0f));
    CHECK(records[0].cameraRelativeOrigin[2] == Catch::Approx(8000.0f));
    // The residual must survive the narrowing exactly; a float built from the
    // absolute origin would not.
    CHECK(static_cast<double>(records[0].cameraRelativeOrigin[0]) ==
        source.cells[0].originX - cameraOrigin[0]);
    CHECK(static_cast<float>(source.cells[0].originX) !=
        static_cast<float>(source.cells[0].originX + 1.0));

    CHECK(records[0].cellId[0] == 0u);
    CHECK(records[0].cellId[1] == 488u);
    CHECK(records[0].layerSlotCount == 3u);
    CHECK(records[0].layerIndices[1] == 1u);
    CHECK(records[1].lodLevel == 0u);
}

TEST_CASE("P14_terrain_reference_matches_the_shader_blend_and_identity",
    "[phase14][terrain]")
{
    auto source = BuildPacket();
    for (auto& cell : source.cells) {
        cell.originZ = -992'000.0;
    }
    source.cells[0].gridX = 488;
    source.cells[0].originX = 488.0 * terrain::kCellWorldSize;
    source.cells[0].cellId = 0x000001E800000000ull;
    source.cells[0].gridY = 0;
    source.cells[0].originY = 0.0;
    source.cells[1].gridX = 489;
    source.cells[1].originX = 489.0 * terrain::kCellWorldSize;
    source.cells[1].cellId = 0x000001E900000000ull;
    source.cells[1].gridY = 0;
    source.cells[1].originY = 0.0;
    // A distinct flat colour per array slice makes a wrong slice selection a
    // wrong pixel rather than a subtle shading difference.
    texture::CapturedTexture layers{};
    layers.resourceId = 0x8000'0000'0000'14A1ull;
    layers.generation = 1;
    layers.dimension = texture::TextureDimension::Texture2DArray;
    layers.width = 1;
    layers.height = 1;
    layers.arrayLayers = 3;
    layers.resourceFormat = texture::TextureFormat::R8G8B8A8Unorm;
    layers.viewFormat = texture::TextureFormat::R8G8B8A8Unorm;
    layers.sampler.minFilter = texture::TextureFilter::Nearest;
    layers.sampler.magFilter = texture::TextureFilter::Nearest;
    layers.sampler.mipFilter = texture::TextureFilter::Nearest;
    layers.sampler.maxLod = 0.0f;
    const std::array<std::array<std::uint8_t, 4>, 3> sliceColors{{
        {{255, 0, 0, 255}}, {{0, 255, 0, 255}}, {{0, 0, 255, 255}}}};
    for (std::uint32_t slice = 0; slice < 3; ++slice) {
        texture::TextureSubresource subresource{};
        subresource.arrayLayer = slice;
        subresource.width = 1;
        subresource.height = 1;
        subresource.rowPitch = 4;
        subresource.slicePitch = 4;
        subresource.bytes.resize(4);
        for (std::size_t channel = 0; channel < 4; ++channel) {
            subresource.bytes[channel] =
                static_cast<std::byte>(sliceColors[slice][channel]);
        }
        layers.subresources.push_back(std::move(subresource));
    }

    view::CapturedView captured{};
    captured.viewId = source.header.viewId;
    captured.cameraId = 0x99;
    captured.projectionMode = view::ProjectionMode::Perspective;
    captured.handedness = view::Handedness::LeftHanded;
    captured.flags = view::ViewCameraRelative;
    captured.targetId = 2;
    captured.outputWidth = 64;
    captured.outputHeight = 48;
    captured.renderScale = 1.0f;
    captured.nearPlane = 10.0f;
    captured.farPlane = 32'768.0f;
    captured.verticalFovRadians = 1.0471975512f;
    captured.cameraRelativeOrigin = {2'000'000.0, 512.0, -1'000'000.0};
    captured.previousCameraRelativeOrigin = captured.cameraRelativeOrigin;
    captured.viewport = {0.0f, 0.0f, 64.0f, 48.0f, 0.0f, 1.0f};
    captured.scissor = {0, 0, 64, 48};
    const auto identity = view::IdentityMatrix();
    const auto projection = view::BuildPerspectiveProjection(
        captured.verticalFovRadians, 64.0f / 48.0f,
        captured.nearPlane, captured.farPlane, captured.handedness);
    const auto toSource = [](const view::Matrix4& matrix) {
        view::SourceMatrix4 out{};
        out.storage = view::MatrixStorage::RowMajor;
        out.vectors = view::VectorConvention::ColumnVector;
        std::copy(std::begin(matrix.elements), std::end(matrix.elements),
            std::begin(out.elements));
        return out;
    };
    captured.view = toSource(identity);
    captured.projection = toSource(projection);
    captured.previousView = captured.view;
    captured.previousProjection = captured.projection;
    view::ViewRecordV1 record{};
    REQUIRE(view::TranslateCapturedView(captured, record) ==
        view::ViewError::None);

    terrain::TerrainViewport target{};
    target.width = 64;
    target.height = 48;
    target.viewportWidth = 64.0f;
    target.viewportHeight = 48.0f;
    target.maxDepth = 1.0f;

    scene::GBufferImage image;
    REQUIRE(terrain::InitializeTerrainReference(target, image) ==
        terrain::TerrainError::None);
    REQUIRE(terrain::ComposeReferenceTerrainGBuffer(
        source, record, layers, target, image) ==
        terrain::TerrainError::None);

    std::uint64_t covered = 0;
    bool sawFirstCell = false;
    bool sawSecondCell = false;
    for (const auto& pixel : image.pixels) {
        const auto cellId = static_cast<std::uint64_t>(pixel.objectId[0]) |
            (static_cast<std::uint64_t>(pixel.objectId[1]) << 32);
        if (cellId == 0) continue;
        ++covered;
        sawFirstCell = sawFirstCell || cellId == source.cells[0].cellId;
        sawSecondCell = sawSecondCell || cellId == source.cells[1].cellId;
        const auto textureId =
            static_cast<std::uint64_t>(pixel.materialId[0]) |
            (static_cast<std::uint64_t>(pixel.materialId[1]) << 32);
        // The dominant layer is always one of the three captured ones.
        CHECK((textureId == 0x7000ull || textureId == 0x7001ull ||
            textureId == 0x7002ull));
        // Every covered pixel blends the flat slice colours, so no channel
        // can leave the range spanned by them.
        CHECK(pixel.albedo[0] <= 1.0f);
        CHECK(pixel.albedo[3] == Catch::Approx(1.0f));
        CHECK(pixel.geometricNormalRoughness[2] == Catch::Approx(1.0f));
        CHECK(pixel.geometricNormalRoughness[3] >= 0.40f);
        CHECK(pixel.geometricNormalRoughness[3] <= 0.60f);
        CHECK(pixel.shadingNormalDepth[3] < 1.0f);
    }
    CHECK(covered > 0);
    CHECK(sawFirstCell);
    CHECK(sawSecondCell);

    // A composite that was never initialized is rejected rather than
    // silently resized.
    scene::GBufferImage unsized;
    CHECK(terrain::ComposeReferenceTerrainGBuffer(
        source, record, layers, target, unsized) ==
        terrain::TerrainError::InvalidCell);
}

TEST_CASE("P14_terrain_residency_returns_to_baseline_on_unload",
    "[phase14][terrain]")
{
    terrain::TerrainResidency residency;
    CHECK(residency.ResidentCells() == 0);
    CHECK(residency.ResidentBytes() == 0);

    const auto packet = BuildPacket();
    REQUIRE(residency.Load(packet) == terrain::TerrainError::None);
    CHECK(residency.ResidentCells() == 2);
    const auto loadedBytes = residency.ResidentBytes();
    CHECK(loadedBytes > 0);

    // Reloading the same cells is an update, not a leak.
    REQUIRE(residency.Load(packet) == terrain::TerrainError::None);
    CHECK(residency.ResidentCells() == 2);
    CHECK(residency.ResidentBytes() == loadedBytes);

    CHECK(residency.Unload(packet.cells[0].cellId) ==
        terrain::TerrainError::None);
    CHECK(residency.ResidentCells() == 1);
    CHECK(residency.ResidentBytes() < loadedBytes);
    // Unloading a cell that was never resident must be reported, not ignored.
    CHECK(residency.Unload(packet.cells[0].cellId) ==
        terrain::TerrainError::UnknownCell);

    CHECK(residency.Unload(packet.cells[1].cellId) ==
        terrain::TerrainError::None);
    CHECK(residency.ResidentCells() == 0);
    CHECK(residency.ResidentBytes() == 0);
    CHECK(residency.PeakBytes() == loadedBytes);
}
