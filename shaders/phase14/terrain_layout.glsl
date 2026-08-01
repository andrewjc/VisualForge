// Mirrors vf::renderer::terrain::LandscapeLayerV1 byte for byte so the
// captured layer table uploads without translation.
struct GpuLandscapeLayerV1
{
    uvec2 textureId;
    uint arraySlice;
    uint flags;
    vec2 uvScale;
    float roughness;
    float normalStrength;
};

// Derived from TerrainCellV1 on the host. The engine's double-precision cell
// origin becomes a camera-relative float offset here, which is what keeps a
// distant exterior cell free of camera-origin swimming.
struct GpuTerrainCellV1
{
    uvec2 cellId;
    uint lodLevel;
    uint layerSlotCount;
    vec4 cameraRelativeOrigin;
    uvec4 layerIndices0;
    uvec4 layerIndices1;
};

layout(set = 0, binding = 10, std430) readonly buffer TerrainCells
{
    GpuTerrainCellV1 records[];
} terrainCells;

layout(set = 0, binding = 11, std430) readonly buffer TerrainLayers
{
    GpuLandscapeLayerV1 records[];
} terrainLayers;

layout(set = 0, binding = 12) uniform sampler2DArray terrainLayerTextures;

layout(push_constant) uniform TerrainPushConstants
{
    uint cellIndex;
    uint reserved;
} terrainPush;

const float kVfCellWorldSize = 4096.0;
const uint kVfLandChannelsPerVertex = 8u;

uint vfTerrainLayerIndex(GpuTerrainCellV1 cellRecord, uint slot)
{
    return slot < 4u
        ? cellRecord.layerIndices0[slot]
        : cellRecord.layerIndices1[slot - 4u];
}
