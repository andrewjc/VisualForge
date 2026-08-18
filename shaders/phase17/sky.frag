#version 460
#extension GL_EXT_scalar_block_layout : require

#include "../phase10/view_transform.glsl"
#include "../phase11/scene_layout.glsl"
#include "../phase15/alpha_coverage.glsl"
#include "../phase17/lighting.glsl"
#include "../phase16/family_shading.glsl"
#include "../phase19/reflection.glsl"

layout(location = 0) in vec2 skyNdc;

layout(location = 0) out vec4 skyRadiance;

// The sky is what a ray that leaves the world sees, and the frame already has
// a function for that: `vfMissRadiance` is what a reflection ray uses when it
// hits nothing. Writing a second sky here would let the sky the camera sees
// and the sky reflected in a window disagree, which is a class of bug that
// takes a long time to notice and is impossible to argue about once it has
// happened. There is one sky.
//
// With no captured environment `vfMissRadiance` returns zero, which is what
// the attachment was cleared to, so a frame that captured no weather looks
// exactly as it did before this pass existed.
void main()
{
    // The pixel's ray, recovered from clip space. The camera sits at the
    // origin of the space the frame is shaded in, so a point on the far plane
    // is the direction to it.
    vec4 clip = vec4(skyNdc, 1.0, 1.0);
    vec4 farPoint = vec4(
        dot(viewConstants.record.inverseViewProjectionRows[0], clip),
        dot(viewConstants.record.inverseViewProjectionRows[1], clip),
        dot(viewConstants.record.inverseViewProjectionRows[2], clip),
        dot(viewConstants.record.inverseViewProjectionRows[3], clip));
    vec3 direction = vec3(0.0, 0.0, 1.0);
    if (abs(farPoint.w) > 1.0e-12) {
        direction = farPoint.xyz / farPoint.w;
    }
    // No probe: a probe is a captured cube around a place in the world, and
    // the camera is not standing inside one.
    skyRadiance = vec4(
        vfMissRadiance(sceneEnvironment.record, direction, vec3(0.0), false),
        1.0);
}
