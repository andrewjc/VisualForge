#pragma once

#include "TerrainShaderLayout.generated.h"
#include "renderer_core/EngineScene.h"
#include "renderer_core/EngineTexture.h"
#include "renderer_core/EngineView.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace vf::renderer::terrain {

inline constexpr std::uint32_t kTerrainPacketMagic = 0x52544656u; // "VFTR"
inline constexpr std::uint16_t kTerrainPacketVersionMajor = 1;
inline constexpr std::uint16_t kTerrainPacketVersionMinor = 0;
inline constexpr std::uint32_t kTerrainPacketEndian = 0x01020304u;
// The engine vertex carries landscape data as two UNorm byte quads
// (VertexSemantic::Landscape0 and Landscape1), so a captured vertex has
// exactly eight blend channels regardless of how many the cell declares.
inline constexpr std::uint32_t kLandChannelsPerVertex = 8;
inline constexpr double kCellWorldSize = 4096.0;
inline constexpr std::uint32_t kMaximumTerrainLayers = 256;
inline constexpr std::uint32_t kMaximumTerrainCells = 8'192;
inline constexpr std::uint32_t kMaximumTerrainVertices = 4'194'304;
inline constexpr std::uint32_t kMaximumTerrainIndices = 16'777'216;

enum CellFlag : std::uint32_t
{
    CellWritesWorldTarget = 1u << 0,
    CellIsLodGeometry = 1u << 1,
};

inline constexpr std::uint32_t kKnownCellFlags =
    CellWritesWorldTarget | CellIsLodGeometry;

enum LayerFlag : std::uint32_t
{
    LayerIsBase = 1u << 0,
    LayerHasNormalMap = 1u << 1,
};

inline constexpr std::uint32_t kKnownLayerFlags =
    LayerIsBase | LayerHasNormalMap;

enum class TerrainError : std::uint8_t
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
    InvalidLayer,
    MissingLayer,
    LayerSlotMismatch,
    InvalidWeights,
    UnclassifiedLandChannel,
    InvalidNormal,
    InvalidLod,
    InvalidCell,
    DuplicateCell,
    VertexOutOfCell,
    IndexOutOfRange,
    IndexOutOfCell,
    UnknownCell,
    AllocationFailure,
};

struct alignas(16) LandscapeLayerV1
{
    std::uint64_t textureId{};
    std::uint32_t arraySlice{};
    std::uint32_t flags{};
    float uvScale[2]{1.0f, 1.0f};
    float roughness{1.0f};
    float normalStrength{1.0f};
};

struct alignas(16) LandscapeVertexV1
{
    // Cell relative. Absolute world position is the cell origin plus this,
    // evaluated in double, so distant cells never lose float precision.
    float position[3]{};
    float reserved0{};
    float normal[4]{0.0f, 0.0f, 1.0f, 0.0f};
    float color[4]{1.0f, 1.0f, 1.0f, 1.0f};
    float channels[kLandChannelsPerVertex]{};
};

struct alignas(16) TerrainCellV1
{
    std::uint64_t cellId{};
    std::int32_t gridX{};
    std::int32_t gridY{};
    std::uint32_t quadrant{};
    // The engine's own LOD decision for this cell. It is recorded, never
    // recomputed, so mirrored terrain cannot disagree with vanilla culling.
    std::uint32_t lodLevel{};
    std::uint32_t firstVertex{};
    std::uint32_t vertexCount{};
    std::uint32_t firstIndex{};
    std::uint32_t indexCount{};
    double originX{};
    double originY{};
    // Height origin. Vertex heights are relative to it for the same reason
    // planar positions are relative to originX/originY.
    double originZ{};
    float lodMorphStart{};
    float lodMorphEnd{};
    std::uint32_t flags{CellWritesWorldTarget};
    // Channels beyond this count are land data we have not classified.
    std::uint32_t layerSlotCount{};
    // Slot to layer-table index. Slot order is the cell's, not the table's.
    std::uint32_t layerSlots[kLandChannelsPerVertex]{};
    float boundsMinimumZ{};
    float boundsMaximumZ{};
    std::uint64_t reserved{};
};

struct alignas(8) TerrainPacketHeaderV1
{
    std::uint32_t magic{kTerrainPacketMagic};
    std::uint16_t versionMajor{kTerrainPacketVersionMajor};
    std::uint16_t versionMinor{kTerrainPacketVersionMinor};
    std::uint32_t headerSize{sizeof(TerrainPacketHeaderV1)};
    std::uint32_t totalSize{};
    std::uint32_t payloadCrc32{};
    std::uint32_t endianMarker{kTerrainPacketEndian};
    std::uint64_t frameId{};
    std::uint64_t viewId{};
    std::uint64_t captureSequence{};
    std::uint32_t captureThreadId{};
    std::uint32_t renderThreadId{};
    std::uint32_t layerCount{};
    std::uint32_t cellCount{};
    std::uint32_t vertexCount{};
    std::uint32_t indexCount{};
    std::uint32_t layersOffset{};
    std::uint32_t cellsOffset{};
    std::uint32_t verticesOffset{};
    std::uint32_t indicesOffset{};
    std::uint32_t flags{};
    std::uint32_t reserved0{};
    std::uint64_t reserved[2]{};
};

struct TerrainPacket
{
    TerrainPacketHeaderV1 header{};
    std::vector<LandscapeLayerV1> layers;
    std::vector<TerrainCellV1> cells;
    std::vector<LandscapeVertexV1> vertices;
    std::vector<std::uint32_t> indices;
};

struct TerrainSample
{
    std::array<float, kLandChannelsPerVertex> weights{};
    std::array<std::uint32_t, kLandChannelsPerVertex> arraySlices{};
    std::array<float, 3> normal{0.0f, 0.0f, 1.0f};
    float roughness{};
    std::uint32_t activeSlots{};
};

struct EvaluatedCell
{
    std::uint64_t cellId{};
    std::array<double, 3> worldMinimum{};
    std::array<double, 3> worldMaximum{};
    std::uint32_t lodLevel{};
};

struct TerrainEvaluation
{
    std::vector<EvaluatedCell> cells;
    std::vector<TerrainSample> samples;
    // Vertices whose captured channels did not already sum to one and were
    // normalized explicitly rather than silently darkening the blend.
    std::uint32_t normalizedVertices{};
    std::uint32_t seamChecks{};
    std::uint32_t seamMismatches{};
    // Seam checks where the two cells disagree on LOD level, which is where
    // near/far terrain cracks appear.
    std::uint32_t lodSeamChecks{};
    std::uint32_t lodSeamMismatches{};
    float maximumSeamGap{};
    float maximumLocalMagnitude{};
};

// The GPU record derived from a captured cell. The engine's double cell
// origin becomes a camera-relative float offset here; nothing downstream
// ever sees an absolute world coordinate in single precision.
struct alignas(16) GpuTerrainCellV1
{
    std::uint32_t cellId[2]{};
    std::uint32_t lodLevel{};
    std::uint32_t layerSlotCount{};
    float cameraRelativeOrigin[4]{};
    std::uint32_t layerIndices[kLandChannelsPerVertex]{};
};

// Where a mirrored terrain frame lands. Kept separate from the raster packet
// header because terrain carries its own vertex stream.
struct TerrainViewport
{
    std::uint32_t width{};
    std::uint32_t height{};
    float x{};
    float y{};
    float viewportWidth{};
    float viewportHeight{};
    float minDepth{};
    float maxDepth{1.0f};
};

// Terrain streams in and out by cell. Residency is tracked explicitly so a
// phase gate can prove the exterior returns to its baseline footprint.
class TerrainResidency
{
public:
    [[nodiscard]] TerrainError Load(const TerrainPacket& packet) noexcept;
    [[nodiscard]] TerrainError Unload(std::uint64_t cellId) noexcept;
    [[nodiscard]] std::size_t ResidentCells() const noexcept;
    [[nodiscard]] std::uint64_t ResidentBytes() const noexcept;
    [[nodiscard]] std::uint64_t PeakBytes() const noexcept;

private:
    std::map<std::uint64_t, std::uint64_t> resident_;
    std::uint64_t residentBytes_{};
    std::uint64_t peakBytes_{};
};

[[nodiscard]] TerrainError ValidateTerrainPacket(
    const TerrainPacket& packet) noexcept;
[[nodiscard]] TerrainError EncodeTerrainPacket(
    const TerrainPacket& packet,
    std::vector<std::byte>& bytes) noexcept;
[[nodiscard]] TerrainError DecodeTerrainPacket(
    std::span<const std::byte> bytes,
    TerrainPacket& packet) noexcept;
[[nodiscard]] TerrainError EvaluateTerrain(
    const TerrainPacket& packet,
    TerrainEvaluation& evaluation) noexcept;
[[nodiscard]] std::uint32_t ResolveArraySlice(
    const TerrainPacket& packet,
    const TerrainCellV1& cell,
    std::uint32_t slot) noexcept;
[[nodiscard]] float LodBlend(
    const TerrainCellV1& cell,
    float distance) noexcept;
// Camera origin comes from the captured view so the offset is computed in
// double and only the small residual is narrowed to float.
[[nodiscard]] TerrainError BuildGpuTerrainCells(
    const TerrainPacket& packet,
    std::span<const double, 3> cameraOrigin,
    std::vector<GpuTerrainCellV1>& records) noexcept;
// Clears `image` to the shared world clear so a terrain-only reference and a
// scene reference start from identical uncovered pixels.
[[nodiscard]] TerrainError InitializeTerrainReference(
    const TerrainViewport& target,
    scene::GBufferImage& image) noexcept;
// Mirrors the terrain fragment shader exactly, including perspective-correct
// attribute interpolation, so a GPU disagreement is a real defect and not a
// difference in interpolation rules. Terrain shares the render pass and depth
// buffer with the scene pass, so this composites into an already-sized image
// rather than owning it.
[[nodiscard]] TerrainError ComposeReferenceTerrainGBuffer(
    const TerrainPacket& packet,
    const view::ViewRecordV1& view,
    const texture::CapturedTexture& layerTextures,
    const TerrainViewport& target,
    scene::GBufferImage& image) noexcept;
[[nodiscard]] const char* ToString(TerrainError error) noexcept;

static_assert(sizeof(GpuTerrainCellV1) == 64);
static_assert(sizeof(GpuTerrainCellV1) == kGpuTerrainCellSize);
static_assert(offsetof(GpuTerrainCellV1, cameraRelativeOrigin) == 16);
static_assert(offsetof(GpuTerrainCellV1, layerIndices) == 32);
static_assert(sizeof(LandscapeLayerV1) == kGpuLandscapeLayerSize);
static_assert(sizeof(LandscapeVertexV1) == kGpuLandscapeVertexSize);
static_assert(sizeof(LandscapeLayerV1) == 32);
static_assert(sizeof(LandscapeVertexV1) == 80);
static_assert(offsetof(LandscapeVertexV1, normal) == 16);
static_assert(offsetof(LandscapeVertexV1, channels) == 48);
static_assert(sizeof(TerrainCellV1) == 128);
static_assert(offsetof(TerrainCellV1, originX) == 40);
static_assert(offsetof(TerrainCellV1, originZ) == 56);
static_assert(offsetof(TerrainCellV1, layerSlots) == 80);
static_assert(sizeof(TerrainPacketHeaderV1) == 112);

}
