#version 460
#extension GL_GOOGLE_include_directive : require

#include "../phase23/post.glsl"

layout(set = 0, binding = 0) uniform sampler2D hdrSource;

layout(push_constant) uniform TonePushConstants
{
    // Applied before the curve, so the whole range shifts rather than the
    // highlights alone. Scaling after tone mapping compresses first and
    // brightens second, which lifts black instead of exposing the image.
    float exposure;
    // Bloom's threshold and the width of the knee around it, and how much of
    // the thresholded highlight is added back. An intensity of zero is an
    // exact identity: the chain has to be able to switch an effect off
    // without changing the image by a single code, or "disabled" and "nearly
    // disabled" are indistinguishable in a capture.
    float bloomThreshold;
    float bloomKnee;
    float bloomIntensity;
} tonePush;

layout(location = 0) in vec2 textureCoordinate;
layout(location = 0) out vec4 outputColor;

void main()
{
    vec4 hdr = texture(hdrSource, textureCoordinate);

    // Bloom is taken from the scene before exposure and the curve, because
    // both are display-side decisions and the highlight that blooms is a
    // property of the scene. Thresholding after tone mapping would make the
    // bloom level depend on the exposure, so an adapting frame would bloom
    // more as it darkened.
    vec3 scene = hdr.rgb;
    if (tonePush.bloomIntensity != 0.0) {
        float weight = vfBloomWeight(tonePush.bloomThreshold,
            tonePush.bloomKnee, vfLuminance(scene));
        scene += scene * (weight * tonePush.bloomIntensity);
    }

    vec3 mapped = vfToneMap(scene, tonePush.exposure);
    outputColor = vec4(vfLinearToSrgb(mapped), clamp(hdr.a, 0.0, 1.0));
}
