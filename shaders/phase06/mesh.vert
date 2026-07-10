#version 460
#extension GL_GOOGLE_include_directive : require

#include "../phase10/view_transform.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 vertexColor;
layout(location = 1) out vec2 vertexTexCoord;

void main()
{
    gl_Position = vfTransformPosition(inPosition);
    vertexColor = inColor;
    vertexTexCoord = inTexCoord;
}
