#version 460
#extension GL_GOOGLE_include_directive : require

#include "../phase11/scene_layout.glsl"
#include "alpha_coverage.glsl"

// The depth prepass runs the identical coverage decision as the color pass
// and writes no color, so a cutout silhouette is established once and cannot
// disagree between the two passes.

layout(set = 0, binding = 0, std140) uniform MaterialConstants
{
    vec4 baseColor;
} material;

layout(set = 0, binding = 1) uniform sampler2D baseTexture;

layout(location = 0) in vec3 vertexColor;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) flat in uint sceneObjectIndex;
layout(location = 3) flat in uint sceneInstanceIndex;

void main()
{
    GpuSceneInstanceV1 instanceRecord =
        sceneInstances.records[sceneInstanceIndex];
    GpuVisibilityRecordV1 visibleRecord =
        sceneVisibility.records[sceneObjectIndex];
    vec4 sampled = textureLod(baseTexture, vertexTexCoord, 0.0);
    float surfaceAlpha = material.baseColor.a * sampled.a *
        instanceRecord.parameters.a;
    VfCoverage coverage = vfEvaluateCoverage(
        visibleRecord, surfaceAlpha, uvec2(gl_FragCoord.xy));
    if (!coverage.covered) {
        discard;
    }
}
