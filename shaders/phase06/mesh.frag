#version 460

layout(set = 0, binding = 0, std140) uniform MaterialConstants
{
    vec4 baseColor;
} material;

layout(set = 0, binding = 1) uniform sampler2D baseTexture;

layout(location = 0) in vec3 vertexColor;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 0) out vec4 hdrColor;

void main()
{
    vec4 sampled = textureLod(baseTexture, vertexTexCoord, 0.0);
    hdrColor = vec4(
        vertexColor * material.baseColor.rgb * sampled.rgb,
        material.baseColor.a * sampled.a);
}
