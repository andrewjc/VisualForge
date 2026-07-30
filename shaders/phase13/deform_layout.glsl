// Mirrors raster::RasterVertexV3 byte for byte. A stride that disagrees with
// the host struct does not fail to compile and does not fail validation: the
// compute pass simply writes each vertex at the wrong address, and the result
// reads as deformation being wrong rather than as a layout being stale.
struct GpuRasterVertexV3
{
    float position[3];
    float color[3];
    float texCoord[2];
    float normal[3];
    float pad;
};

struct GpuDeformVertexV1
{
    uvec4 bones;
    vec4 weights;
    vec4 flexibility;
};

struct GpuBoneTransformV1
{
    vec4 rows[3];
};

struct GpuMorphTargetV1
{
    uint firstDelta;
    uint deltaCount;
    float weight;
    float previousWeight;
};

struct GpuMorphDeltaV1
{
    uvec4 vertexIndex;
    vec4 delta;
};

layout(set = 0, binding = 0, std430) readonly buffer BaseVertices
{
    GpuRasterVertexV3 records[];
} baseVertices;

layout(set = 0, binding = 1, std430) readonly buffer DeformVertices
{
    GpuDeformVertexV1 records[];
} deformVertices;

layout(set = 0, binding = 2, std430) readonly buffer Bones
{
    GpuBoneTransformV1 records[];
} bones;

layout(set = 0, binding = 3, std430) readonly buffer PreviousBones
{
    GpuBoneTransformV1 records[];
} previousBones;

layout(set = 0, binding = 4, std430) readonly buffer MorphTargets
{
    GpuMorphTargetV1 records[];
} morphTargets;

layout(set = 0, binding = 5, std430) readonly buffer MorphDeltas
{
    GpuMorphDeltaV1 records[];
} morphDeltas;

layout(set = 0, binding = 6, std430) buffer DeformedVertices
{
    GpuRasterVertexV3 records[];
} deformedVertices;

layout(set = 0, binding = 7, std430) buffer PreviousPositions
{
    vec4 records[];
} previousPositions;

layout(push_constant) uniform DeformPushConstants
{
    uint vertexCount;
    uint morphTargetCount;
    float amplitude;
    float frequency;
    float time;
    float previousTime;
    uint reserved0;
    uint reserved1;
    vec4 direction;
} deformPush;
