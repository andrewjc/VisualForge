#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene_layout.glsl"
#include "../phase15/alpha_coverage.glsl"

// Deliberately no discard, which is what keeps early fragment tests sound.
// The opaque raster class is validated before a frame is armed, so every
// object reaching this shader is opaque and its coverage is unconditionally
// one; the coverage call is here to source the stored opacity from the one
// rule rather than to make a decision.
layout(early_fragment_tests) in;

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
    // Per-instance material data modulates only its own instance.
    vec3 shaded = vertexColor * material.baseColor.rgb * sampled.rgb *
        instanceRecord.parameters.rgb;
    float surfaceAlpha = material.baseColor.a * sampled.a *
        instanceRecord.parameters.a;
    // An opaque surface's base texture may carry a mask or a height in its
    // alpha channel. Folding that into the stored opacity would let unrelated
    // authored data silently make a solid surface transparent.
    float alpha = vfEvaluateCoverage(
        visibleRecord, surfaceAlpha, uvec2(gl_FragCoord.xy)).coverage;
    hdrColor = vec4(shaded, alpha);

    gbufferAlbedo = vec4(
        clamp(shaded, vec3(0.0), vec3(1.0)), clamp(alpha, 0.0, 1.0));
    // The interpolated per-vertex normal when the mesh carried one, and the
    // object's own axis when it did not. The per-object normal shades every
    // surface of an object as one flat plane; the length test is what tells
    // a real normal from an absent one.
    float vertexNormalLength = length(vertexNormal);
    bool haveVertexNormal = vertexNormalLength > 1.0e-4;
    vec3 resolvedNormal = haveVertexNormal
        ? vertexNormal / vertexNormalLength
        : objectRecord.geometricNormal.xyz;
    gbufferGeometricNormalRoughness = vec4(
        resolvedNormal, objectRecord.roughness);
    gbufferShadingNormalDepth = vec4(
        haveVertexNormal ? resolvedNormal : objectRecord.shadingNormal.xyz,
        gl_FragCoord.z);
    gbufferIdentity = uvec4(
        instanceRecord.objectId, objectRecord.materialId);
    gbufferReactive = vec4(0.0, 0.0, 0.0, 0.0);
}
