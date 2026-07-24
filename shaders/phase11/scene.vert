#version 460
#extension GL_GOOGLE_include_directive : require

#include "../phase10/view_transform.glsl"
#include "scene_layout.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec3 inNormal;

layout(location = 0) out vec3 vertexColor;
layout(location = 1) out vec2 vertexTexCoord;
layout(location = 2) flat out uint sceneObjectIndex;
layout(location = 3) flat out uint sceneInstanceIndex;
// The lighting pass needs the shaded point in the same camera-relative space
// the light positions were uploaded in.
layout(location = 4) out vec3 vertexCameraRelative;
// The per-vertex shading normal, rotated into the same camera-relative space.
// Everything downstream starts from N, so a per-object normal makes every
// surface of an object shade as one flat plane -- which reads as the lighting
// being broken rather than as the normal being an approximation.
layout(location = 5) out vec3 vertexNormal;

void main()
{
    uint instanceIndex = scenePush.firstInstance + uint(gl_InstanceIndex);
    GpuSceneInstanceV1 instanceRecord = sceneInstances.records[instanceIndex];
    vec4 localPosition = vec4(inPosition, 1.0);
    vec3 cameraRelativePosition = vec3(
        dot(instanceRecord.modelRows[0], localPosition),
        dot(instanceRecord.modelRows[1], localPosition),
        dot(instanceRecord.modelRows[2], localPosition));
    gl_Position = vfTransformPosition(cameraRelativePosition);
    vertexColor = inColor;
    vertexTexCoord = inTexCoord;
    sceneObjectIndex = scenePush.objectIndex;
    sceneInstanceIndex = instanceIndex;
    vertexCameraRelative = cameraRelativePosition;
    // Rotated by the model's upper 3x3. Exact for the rigid and
    // uniformly scaled placements the draw stream admits -- it rejects
    // mirrored and singular transforms outright -- and an approximation
    // under non-uniform scale, where the strictly correct form is the
    // inverse transpose.
    vertexNormal = vec3(
        dot(instanceRecord.modelRows[0].xyz, inNormal),
        dot(instanceRecord.modelRows[1].xyz, inNormal),
        dot(instanceRecord.modelRows[2].xyz, inNormal));
}
