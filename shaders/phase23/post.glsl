#ifndef VF_PHASE23_POST_GLSL
#define VF_PHASE23_POST_GLSL

// The post chain's output stage, mirrored from renderer_core/EnginePostChain.
// The replay compares the device's tone-mapped image against the same
// functions on the host, so a change to one that is not made to the other is
// a test failure rather than a shift in every frame's contrast that nobody
// attributes to this file.

// Reinhard, applied after exposure. Monotonic everywhere and asymptotic to
// one, so a brighter input is never darker out; a non-monotonic curve produces
// banding that reads as a precision problem rather than as a curve problem.
vec3 vfToneMap(vec3 hdr, float exposure)
{
    float scale = (isinf(exposure) || isnan(exposure) || !(exposure > 0.0))
        ? 1.0 : exposure;
    vec3 scaled = hdr * scale;
    vec3 mapped;
    for (int channel = 0; channel < 3; ++channel) {
        float value = scaled[channel];
        // Non-finite or non-positive collapses to black rather than to a
        // clamped one: a NaN carried forward spreads through every later
        // stage, and a negative value would map to a negative output.
        mapped[channel] = (isinf(value) || isnan(value) || value <= 0.0)
            ? 0.0 : value / (1.0 + value);
    }
    return mapped;
}

// The sRGB transfer function, piecewise exactly as the standard defines it.
// The linear segment near black is not an approximation that can be dropped:
// without it the darkest few codes are visibly wrong, which reads as a black
// level problem.
vec3 vfLinearToSrgb(vec3 value)
{
    vec3 safe = max(value, vec3(0.0));
    bvec3 lower = lessThanEqual(safe, vec3(0.0031308));
    vec3 low = safe * 12.92;
    vec3 high = 1.055 * pow(safe, vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lower);
}

// How much of a pixel contributes to bloom, mirrored from post::BloomWeight.
//
// The knee is not a refinement of the threshold, it is the whole point of it.
// A bare threshold makes bloom switch on the instant a highlight crosses it,
// so any moving specular flickers as it drifts across the boundary -- and that
// reads as a temporal instability somewhere else in the chain rather than as
// this comparison.
float vfBloomWeight(float threshold, float knee, float luminance)
{
    if (isnan(luminance) || isinf(luminance)) return 0.0;
    if (!(knee > 0.0)) {
        return luminance > threshold ? 1.0 : 0.0;
    }
    float lower = threshold - knee;
    float upper = threshold + knee;
    if (luminance <= lower) return 0.0;
    if (luminance >= upper) return 1.0;
    float t = (luminance - lower) / (upper - lower);
    // Smoothstep rather than linear, so the derivative is continuous at both
    // ends and the transition has no visible corner where the ramp meets the
    // flat regions on either side.
    return clamp(t * t * (3.0 - 2.0 * t), 0.0, 1.0);
}

// The luminance bloom is thresholded against. Rec. 709, matching the host.
float vfLuminance(vec3 colour)
{
    return dot(colour, vec3(0.2126, 0.7152, 0.0722));
}

#endif
