#ifndef VF_PHASE22_WATER_GLSL
#define VF_PHASE22_WATER_GLSL

// The water and glass contract, mirrored from renderer_core/EngineWater.cpp.
// The two are compared by the water replay, so a change to one that is not
// made to the other is a test failure rather than a rendering difference
// somebody notices later.

// Index of refraction. Water and glass are far enough apart that using one
// for the other moves the grazing-angle reflection by an amount that reads as
// the Fresnel term being wrong rather than as the wrong material constant.
const float kVfWaterIor = 1.333;
const float kVfGlassIor = 1.52;

struct VfWaterSurface
{
    // Index of refraction, already resolved from the capture or the class
    // default by the host. Zero means neither was usable.
    float ior;
    float fresnelBias;
    float fogDensity;
    vec3 fogColor;
    vec3 shallowColor;
    vec3 deepColor;
    float depthRange;
};

// Schlick, with F0 derived from the index of refraction rather than from the
// authored bias when the capture gave a usable one. The bias is what a capture
// carries when it has no index at all, and preferring it over a real index
// would make every surface reflect like the one that had none.
float vfWaterFresnel(VfWaterSurface surface, float cosine)
{
    float f0 = surface.fresnelBias;
    if (surface.ior > 1.0) {
        float ratio = (surface.ior - 1.0) / (surface.ior + 1.0);
        f0 = ratio * ratio;
    }
    f0 = clamp(f0, 0.0, 1.0);
    float clamped = clamp(cosine, 0.0, 1.0);
    float complement = 1.0 - clamped;
    float squared = complement * complement;
    return clamp(f0 + (1.0 - f0) * squared * squared * complement, 0.0, 1.0);
}

// Beer-Lambert. Transmittance falls exponentially with distance and never
// reaches zero, so the result approaches the fog colour without overshooting
// past it into a colour the capture never authored -- which a linear fade to
// a clamped endpoint does at the far end of a long sight line.
vec3 vfUnderwaterFog(
    VfWaterSurface surface,
    vec3 incoming,
    float distanceThroughWater)
{
    if (!(surface.fogDensity > 0.0) || !(distanceThroughWater > 0.0) ||
        isinf(distanceThroughWater) || isnan(distanceThroughWater)) {
        return incoming;
    }
    float transmittance = exp(-surface.fogDensity * distanceThroughWater);
    return surface.fogColor + (incoming - surface.fogColor) * transmittance;
}

// Shallow to deep over the authored range. Clamped rather than extrapolated:
// past the range the colour stays the deep one instead of continuing into a
// colour the material never described.
vec3 vfWaterColor(VfWaterSurface surface, float depth)
{
    if (!(surface.depthRange > 0.0) || isinf(depth) || isnan(depth)) {
        return surface.shallowColor;
    }
    float t = clamp(depth / surface.depthRange, 0.0, 1.0);
    return mix(surface.shallowColor, surface.deepColor, t);
}

// The split between what is reflected and what is transmitted. They sum to
// one by construction rather than by being tuned separately: any other split
// either creates light at grazing angles or loses it.
vec3 vfWaterShade(
    VfWaterSurface surface,
    vec3 reflected,
    vec3 refracted,
    float cosine,
    float distanceThroughWater)
{
    float fresnel = vfWaterFresnel(surface, cosine);
    vec3 transmitted =
        vfUnderwaterFog(surface, refracted, distanceThroughWater);
    return reflected * fresnel + transmitted * (1.0 - fresnel);
}

#endif
