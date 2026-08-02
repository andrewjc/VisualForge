#version 460
#extension GL_GOOGLE_include_directive : require

#include "../phase11/scene_layout.glsl"
#include "alpha_coverage.glsl"

// Deliberately no early_fragment_tests. A discarded cutout fragment must not
// have already written depth, or the silhouette punches a hole in whatever
// is behind it.

layout(set = 0, binding = 0, std140) uniform MaterialConstants
{
    vec4 baseColor;
} material;

layout(set = 0, binding = 1) uniform sampler2D baseTexture;

layout(location = 0) in vec3 vertexColor;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) flat in uint sceneObjectIndex;
layout(location = 3) flat in uint sceneInstanceIndex;
layout(location = 5) in vec3 vertexNormal;

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
    GpuOpaqueObjectV1 objectRecord =
        opaqueObjects.records[sceneObjectIndex];
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
    // The stored opacity is the coverage decision's own opacity, never the
    // raw sampled alpha, so what the G-buffer says about a surface can never
    // contradict the test that let the fragment through.
    float alpha = coverage.coverage;

    vec3 shaded = vertexColor * material.baseColor.rgb * sampled.rgb *
        instanceRecord.parameters.rgb;
    hdrColor = vec4(shaded, alpha);

    float faceSign = vfFaceSign(visibleRecord, gl_FrontFacing);
    // The interpolated per-vertex normal when the mesh carried one, and the
    // object's own axis when it did not.
    float vertexNormalLength = length(vertexNormal);
    vec3 geometric = (vertexNormalLength > 1.0e-4
        ? vertexNormal / vertexNormalLength
        : objectRecord.geometricNormal.xyz) * faceSign;
    // Follows the same rule. Taking the vertex normal for the geometric
    // normal and the object record for the shading one leaves the two
    // describing different surfaces on the same pixel.
    vec3 shading = (vertexNormalLength > 1.0e-4
        ? vertexNormal / vertexNormalLength
        : objectRecord.shadingNormal.xyz) * faceSign;

    gbufferAlbedo = vec4(
        clamp(shaded, vec3(0.0), vec3(1.0)), clamp(alpha, 0.0, 1.0));
    gbufferGeometricNormalRoughness = vec4(geometric, objectRecord.roughness);
    gbufferShadingNormalDepth = vec4(shading, gl_FragCoord.z);
    gbufferIdentity = uvec4(
        instanceRecord.objectId, objectRecord.materialId);
    gbufferReactive = vec4(0.0, 0.0, 0.0, 0.0);
}
