#version 460
#extension GL_GOOGLE_include_directive : require

#include "terrain_layout.glsl"

layout(early_fragment_tests) in;

layout(location = 0) in vec3 vertexNormal;
layout(location = 1) in vec4 vertexColor;
layout(location = 2) in vec4 vertexChannels0;
layout(location = 3) in vec4 vertexChannels1;
layout(location = 4) in vec2 vertexLocal;
layout(location = 5) flat in uint terrainCellIndex;

layout(location = 0) out vec4 hdrColor;
layout(location = 1) out vec4 gbufferAlbedo;
layout(location = 2) out vec4 gbufferGeometricNormalRoughness;
layout(location = 3) out vec4 gbufferShadingNormalDepth;
layout(location = 4) out uvec4 gbufferIdentity;
// How much of this pixel a transparent effect decides. Opaque geometry decides
// none of it: it is the stable part of the frame, the part an upscaler can
// reproject from history, and marking it reactive would tell the upscaler to
// distrust the whole image. Written rather than left to the clear value,
// because an attachment a shader does not write holds an undefined result
// rather than a zero.
layout(location = 5) out vec4 gbufferReactive;

void main()
{
    GpuTerrainCellV1 cellRecord = terrainCells.records[terrainCellIndex];
    float channels[8] = float[8](
        vertexChannels0.x, vertexChannels0.y,
        vertexChannels0.z, vertexChannels0.w,
        vertexChannels1.x, vertexChannels1.y,
        vertexChannels1.z, vertexChannels1.w);

    // Captured land data is not pre-normalized, so the blend divides by the
    // declared total instead of assuming it sums to one.
    float total = 0.0;
    for (uint slot = 0u; slot < cellRecord.layerSlotCount; ++slot) {
        total += channels[slot];
    }
    total = max(total, 1.0e-6);

    vec3 albedo = vec3(0.0);
    float roughness = 0.0;
    float dominantWeight = -1.0;
    uvec2 dominantTextureId = uvec2(0u);
    for (uint slot = 0u; slot < cellRecord.layerSlotCount; ++slot) {
        float weight = channels[slot] / total;
        GpuLandscapeLayerV1 layer =
            terrainLayers.records[vfTerrainLayerIndex(cellRecord, slot)];
        vec2 uv = vertexLocal / kVfCellWorldSize * layer.uvScale;
        vec4 sampled = textureLod(terrainLayerTextures,
            vec3(uv, float(layer.arraySlice)), 0.0);
        albedo += weight * sampled.rgb;
        roughness += weight * layer.roughness;
        if (weight > dominantWeight) {
            dominantWeight = weight;
            dominantTextureId = layer.textureId;
        }
    }

    vec3 normal = normalize(vertexNormal);
    vec3 shaded = vertexColor.rgb * albedo;
    hdrColor = vec4(shaded, vertexColor.a);

    gbufferAlbedo = vec4(
        clamp(shaded, vec3(0.0), vec3(1.0)), clamp(vertexColor.a, 0.0, 1.0));
    gbufferGeometricNormalRoughness = vec4(
        normal, clamp(roughness, 0.0, 1.0));
    gbufferShadingNormalDepth = vec4(normal, gl_FragCoord.z);
    // Terrain identity is the cell plus the layer that dominates the blend,
    // which is what makes a wrong layer selection visible in the G-buffer.
    gbufferIdentity = uvec4(cellRecord.cellId, dominantTextureId);
    gbufferReactive = vec4(0.0, 0.0, 0.0, 0.0);
}
