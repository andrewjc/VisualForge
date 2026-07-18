#version 460
#extension GL_GOOGLE_include_directive : require

#include "material_eval.glsl"

struct GpuMaterialStaticV1
{
    uvec2 materialId;
    uint staticRevision;
    uint flags;
    uvec4 textureIndices;
    vec2 uvScale;
    vec2 uvOffset;
    vec3 specularColor;
    float reserved;
};

struct GpuMaterialDynamicV1
{
    uvec2 materialId;
    uint materialRevision;
    uint staticRevision;
    vec4 parameters;
    float alphaCutoff;
    uint transferVersion;
    uvec2 reserved;
};

layout(set = 0, binding = 0, std140) uniform LegacyMaterialConstants
{
    vec4 baseColor;
} legacyMaterial;

layout(set = 0, binding = 1) uniform sampler2D baseTexture;
layout(set = 0, binding = 2) uniform sampler2D normalTexture;
layout(set = 0, binding = 3) uniform sampler2D smoothSpecTexture;

layout(set = 0, binding = 4, std430) readonly buffer StaticMaterials
{
    GpuMaterialStaticV1 records[];
} staticMaterials;

layout(set = 0, binding = 5, std430) readonly buffer DynamicMaterials
{
    GpuMaterialDynamicV1 records[];
} dynamicMaterials;

layout(location = 0) in vec3 vertexColor;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 0) out vec4 hdrColor;

const uint VF_HAS_NORMAL = 1u << 1;
const uint VF_MODEL_SPACE_NORMAL = 1u << 3;
const uint VF_ALPHA_COVERAGE = 1u << 4;

void main()
{
    GpuMaterialStaticV1 staticRecord = staticMaterials.records[0];
    GpuMaterialDynamicV1 dynamicRecord = dynamicMaterials.records[0];
    vec2 uv = vertexTexCoord * staticRecord.uvScale + staticRecord.uvOffset;
    vec4 sampledBase = textureLod(baseTexture, uv, 0.0);
    vec2 sampledSmoothSpec = textureLod(smoothSpecTexture, uv, 0.0).rg;
    vec3 baseColor = sampledBase.rgb * vertexColor * legacyMaterial.baseColor.rgb;
    float opacity = clamp(sampledBase.a * dynamicRecord.parameters.x *
        legacyMaterial.baseColor.a, 0.0, 1.0);
    if ((staticRecord.flags & VF_ALPHA_COVERAGE) != 0u &&
        opacity < dynamicRecord.alphaCutoff) discard;

    vec3 normal = vec3(0.0, 0.0, 1.0);
    if ((staticRecord.flags & VF_HAS_NORMAL) != 0u) {
        vec3 sampledNormal = textureLod(normalTexture, uv, 0.0).rgb;
        if ((staticRecord.flags & VF_MODEL_SPACE_NORMAL) != 0u) {
            normal = normalize(sampledNormal * 2.0 - 1.0);
        } else {
            vec2 xy = sampledNormal.rg * 2.0 - 1.0;
            normal = normalize(vec3(xy,
                sqrt(max(0.0, 1.0 - dot(xy, xy)))));
        }
    }
    float smoothness = clamp(
        dynamicRecord.parameters.y * sampledSmoothSpec.g, 0.0, 1.0);
    float perceptualRoughness = max(0.045, 1.0 - smoothness);
    float alphaRoughness = max(0.002025,
        perceptualRoughness * perceptualRoughness);
    vec3 specularF0 = clamp(staticRecord.specularColor *
        dynamicRecord.parameters.z * sampledSmoothSpec.r,
        vec3(0.0), vec3(0.99));
    vec3 direct = vfEvaluateGgx(baseColor, normal, specularF0,
        alphaRoughness, dynamicRecord.parameters.w,
        vec3(0.0, 0.0, 1.0),
        normalize(vec3(-0.45, 0.55, 1.0)), vec3(4.0));
    hdrColor = vec4(direct + baseColor * 0.035, opacity);
}
