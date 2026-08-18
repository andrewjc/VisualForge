#include "temporal.glsl"

// Mirrors vf::renderer::gi branch for branch. Every rule exists once on each
// side and nowhere else, so the mirror and its oracle cannot drift by
// interpretation.

// Mirrors gi::QualityPreset::raysPerPixel for the fixture. The oracle uses
// the same count and walks the same sequence, so the two integrate the same
// set of directions rather than two different estimates of one integral.
const uint kVfIndirectRays = 8u;
const uint kVfIndirectSkipped = 0u;
const uint kVfIndirectGeometry = 1u;
const uint kVfIndirectEnvironment = 2u;
const uint kVfIndirectUnresolved = 3u;

// Malley's method: a uniform point on the disc lifted to the hemisphere is
// cosine distributed. The cosine is carried by the distribution rather than
// multiplied in afterwards, so no ray is spent near the horizon where it
// would contribute almost nothing. Mirrors gi::SampleDiffuseDirection.
bool vfSampleDiffuseDirection(vec3 normal, vec2 xi, out vec3 direction)
{
    direction = vec3(0.0);
    float lengthSquared = dot(normal, normal);
    if (!(lengthSquared > 0.0) || isinf(lengthSquared)) return false;
    vec3 axis = normal * (1.0 / sqrt(lengthSquared));

    float u1 = clamp(xi.x, 0.0, 0.999999);
    float u2 = clamp(xi.y, 0.0, 0.999999);
    float radius = sqrt(u1);
    float phi = 6.28318530718 * u2;
    float z = sqrt(max(0.0, 1.0 - u1));

    vec3 tangent;
    vec3 bitangent;
    vfOrthonormalBasis(axis, tangent, bitangent);
    direction = normalize(tangent * (radius * cos(phi)) +
        bitangent * (radius * sin(phi)) + axis * z);
    return dot(direction, direction) > 0.0;
}

#ifdef VF_RAY_QUERY
// One bounce of diffuse indirect light, traced against the same top level the
// shadow and reflection passes use. Mirrors gi::EvaluateIndirect.
vec3 vfIndirect(
    vec3 position,
    vec3 geometricNormal,
    vec3 albedo,
    uint pixelX,
    uint pixelY,
    uint frameIndex,
    out uint source)
{
    source = kVfIndirectSkipped;
    vec3 total = vec3(0.0);
    if (kVfIndirectRays == 0u) return total;

    GpuEnvironmentV1 environment = sceneEnvironment.record;
    // No captured lighting means no light to bounce. Phase 17 established
    // that such a frame leaves the albedo alone, and a bounce that returns
    // the hit surface's albedo would invent light the capture never saw.
    if ((environment.flagsAndCount.x & kVfEnvironmentIndirectDisabled) != 0u) {
        return vec3(0.0);
    }
    if ((environment.flagsAndCount.x & kVfEnvironmentPresent) == 0u) {
        return total;
    }
    vec3 normal = normalize(geometricNormal);
    vec3 origin = vfOffsetRayOrigin(position, normal, 1.0);

    uint contributing = 0u;
    bool sawGeometry = false;
    bool sawEnvironment = false;

    for (uint ray = 0u; ray < kVfIndirectRays; ++ray) {
        vec2 xi = vfSampleSequence(pixelX, pixelY, frameIndex, ray);
        vec3 direction;
        if (!vfSampleDiffuseDirection(normal, xi, direction)) continue;

        rayQueryEXT query;
        rayQueryInitializeEXT(query, sceneTlas, gl_RayFlagsOpaqueEXT, 0xFFu,
            origin, 0.0, direction, kVfDirectionalShadowDistance);
        while (rayQueryProceedEXT(query)) {}

        vec3 radiance;
        if (rayQueryGetIntersectionTypeEXT(query, true) ==
            gl_RayQueryCommittedIntersectionNoneEXT) {
            // Nothing hit. Indoors that means nothing is known, and the
            // exterior sky must not stand in: that substitution is the light
            // leak this rule exists to prevent.
            radiance = vfMissRadiance(environment, direction, vec3(0.0),
                false);
            if (vfResolveMiss(environment, false) ==
                kVfReflectionEnvironment) {
                sawEnvironment = true;
            }
        } else {
            uint geometry =
                rayQueryGetIntersectionGeometryIndexEXT(query, true);
            uint objectIndex = geometry < sceneGeometryObjects.records.length()
                ? sceneGeometryObjects.records[geometry] : 0u;
            float distance = rayQueryGetIntersectionTEXT(query, true);
            vec3 hitPosition = origin + direction * distance;
            vec3 hitNormal = vfOrientHitNormal(
                opaqueObjects.records[objectIndex].geometricNormal.xyz,
                direction, true);
            // The bounce surface's own colour, tinted only where a tint is
            // declared. Reading the tint alone made every ordinary material
            // bounce black, so the diffuse indirect term contributed nothing
            // on exactly the surfaces that carry a room's light.
            vec3 hitAlbedo = vfApplyTint(sceneFamilies.records[objectIndex],
                sceneFamilies.records[objectIndex].baseColor.rgb);

            // Shaded through the same function the raster pass uses, which
            // shadows the bounce surface as well: a room must not brighten
            // from a wall the sun cannot reach.
            radiance = vfShadeSurface(environment, hitAlbedo, hitPosition,
                hitNormal, length(hitPosition));
            sawGeometry = true;
        }

        total += vfClampIndirect(radiance);
        ++contributing;
    }

    if (contributing == 0u) {
        source = kVfIndirectUnresolved;
        return vec3(0.0);
    }
    // The cosine-weighted estimator's weight is one: the distribution carries
    // the cosine and the Lambertian denominator cancels it, so the mean of the
    // samples is the integral.
    total *= albedo / float(contributing);
    source = sawGeometry ? kVfIndirectGeometry
        : (sawEnvironment ? kVfIndirectEnvironment : kVfIndirectUnresolved);
    return source == kVfIndirectUnresolved ? vec3(0.0) : total;
}
#endif

