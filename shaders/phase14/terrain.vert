#version 460
#extension GL_GOOGLE_include_directive : require

#include "../phase10/view_transform.glsl"
#include "terrain_layout.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec4 inChannels0;
layout(location = 4) in vec4 inChannels1;

layout(location = 0) out vec3 vertexNormal;
layout(location = 1) out vec4 vertexColor;
layout(location = 2) out vec4 vertexChannels0;
layout(location = 3) out vec4 vertexChannels1;
layout(location = 4) out vec2 vertexLocal;
layout(location = 5) flat out uint terrainCellIndex;

void main()
{
    GpuTerrainCellV1 cellRecord = terrainCells.records[terrainPush.cellIndex];
    // The vertex stream stays cell relative and the cell origin arrives
    // already camera relative, so no absolute world coordinate is ever
    // materialized in float.
    vec3 cameraRelative = cellRecord.cameraRelativeOrigin.xyz + inPosition;
    gl_Position = vfTransformPosition(cameraRelative);
    vertexNormal = inNormal;
    vertexColor = inColor;
    vertexChannels0 = inChannels0;
    vertexChannels1 = inChannels1;
    vertexLocal = inPosition.xy;
    terrainCellIndex = terrainPush.cellIndex;
}
