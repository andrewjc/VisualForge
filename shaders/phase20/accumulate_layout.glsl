#ifndef VF_PHASE20_ACCUMULATE_LAYOUT_GLSL
#define VF_PHASE20_ACCUMULATE_LAYOUT_GLSL

// Mirrors gi::GpuIndirectPixelV1, gi::GpuIndirectHistoryV1 and
// gi::GpuIndirectResultV1 byte for byte. A stride that disagrees with the host
// struct neither fails to compile nor trips validation: the pass simply reads
// and writes every pixel at the wrong address, and the frame then looks like
// the filter is wrong rather than like a layout is stale.

struct GpuIndirectPixelV1
{
    // std430 rounds a vec3 up to four floats, so the depth rides in the slot
    // the padding would otherwise waste.
    vec3 normal;
    float depth;
    vec3 radiance;
    float radiancePad;
    vec2 motion;
    // Both halves of each identity. Narrowed to thirty-two bits, two objects
    // whose low words agreed would reproject into each other.
    uvec2 objectId;
    uvec2 materialId;
    uvec2 reserved;
};

struct GpuIndirectHistoryV1
{
    vec3 mean;
    float meanPad;
    vec3 secondMoment;
    uint samples;
};

struct GpuIndirectResultV1
{
    vec3 mean;
    // Which gate rejected the incoming history, so a frame that will not
    // converge reads as the reason rather than as "the filter is slow".
    uint reason;
    vec3 variance;
    uint samples;
};

layout(set = 0, binding = 0, std430) readonly buffer IndirectCurrent
{
    GpuIndirectPixelV1 records[];
} indirectCurrent;

layout(set = 0, binding = 1, std430) readonly buffer IndirectPrevious
{
    GpuIndirectPixelV1 records[];
} indirectPrevious;

layout(set = 0, binding = 2, std430) readonly buffer IndirectHistory
{
    GpuIndirectHistoryV1 records[];
} indirectHistory;

layout(set = 0, binding = 3, std430) writeonly buffer IndirectResults
{
    GpuIndirectResultV1 records[];
} indirectResults;

layout(push_constant) uniform IndirectPush
{
    uint width;
    uint height;
    uint pixelCount;
    // A camera cut invalidates every pixel at once whatever the surfaces say,
    // and no per-pixel record can carry that.
    uint epochMatches;
} indirectPush;

#endif
