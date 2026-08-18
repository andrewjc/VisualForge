// Mirrors vf::renderer::reflect branch for branch. Every rule exists once on
// each side and nowhere else, so the mirror and its oracle cannot drift by
// interpretation.

// Mirrors vf::renderer::reflect::kDielectricF0. Dielectrics reflect about 4%
// at normal incidence; metals reflect their own base colour.
const float kVfDielectricF0 = 0.04;
// Mirrors kSunLobeExponent. The sun occupies a small solid angle, so a broad
// falloff would light every escaping ray with its full radiance.
const float kVfSunLobeExponent = 64.0;

// Mirrors the defaults in vf::renderer::reflect::ReflectionPolicy. These are
// quality policy, deliberately not material semantics: a preset changes them
// without rewriting a single captured material record.
const float kVfReflectionRoughnessCutoff = 0.65;
const float kVfReflectionMaximumDistance = 4096.0;

const uint kVfReflectionSkipped = 0u;
const uint kVfReflectionGeometry = 1u;
const uint kVfReflectionEnvironment = 2u;
const uint kVfReflectionProbe = 3u;
const uint kVfReflectionUnresolved = 4u;

// The engine authors specular colour and smoothness, not metalness, so there
// is no metalness channel to read. The environment-map feature is the
// closest declared signal a captured material gives for a metal-like
// surface, and it is used as one until a real channel is captured. The
// oracle applies the identical rule, so the approximation is shared rather
// than a place the two sides can disagree.
float vfMetalness(GpuFamilyRecordV1 record)
{
    return vfHasFeature(record, kVfFeatureEnvironment) ? 1.0 : 0.0;
}

vec3 vfComputeF0(vec3 baseColor, float metalness)
{
    return mix(vec3(kVfDielectricF0), baseColor, clamp(metalness, 0.0, 1.0));
}

vec3 vfFresnelSchlick(vec3 f0, float cosine)
{
    float complement = 1.0 - clamp(cosine, 0.0, 1.0);
    float squared = complement * complement;
    return f0 + (vec3(1.0) - f0) * (squared * squared * complement);
}

// `view` points from the surface toward the viewer.
vec3 vfMirrorDirection(vec3 view, vec3 normal)
{
    return normalize(2.0 * dot(normal, view) * normal - view);
}

// Mirrors reflect::Mix. One round of an integer mixer, because a reflection
// pass has no cheap way to carry per-pixel state and both sides must land on
// the same value from the same inputs.
uint vfMixBits(uint value)
{
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    return value;
}

// 24 bits keeps the result strictly below one; a sample of exactly one drives
// the GGX mapping to infinity.
float vfToUnitFloat(uint bits)
{
    return float(bits >> 8) * (1.0 / 16777216.0);
}

vec2 vfSampleSequence(uint pixelX, uint pixelY, uint frameIndex, uint sampleIndex)
{
    uint seed = vfMixBits(pixelX * 0x9E3779B1u) ^
        vfMixBits(pixelY * 0x85EBCA77u) ^
        vfMixBits(frameIndex * 0xC2B2AE3Du) ^
        vfMixBits(sampleIndex * 0x27D4EB2Fu);
    return vec2(vfToUnitFloat(vfMixBits(seed)),
        vfToUnitFloat(vfMixBits(seed ^ 0x68BC21EBu)));
}

// The tangent frame comes from phase16/family_shading.glsl. Defining a
// second one here would let one surface end up with two frames, which is the
// drift the Phase 16 normal decode exists to prevent.

// Returns false when the sampled half-vector produces a direction below the
// surface. Rejected rather than clamped: clamping piles every rejected
// direction onto the horizon and draws a bright ring at grazing angles.
bool vfSampleReflectionDirection(
    vec3 normal, vec3 view, float roughness, vec2 xi, out vec3 direction)
{
    roughness = clamp(roughness, 0.0, 1.0);
    // A zero-roughness lobe is a delta, not the limit of a sampling scheme.
    if (!(roughness > 0.0)) {
        direction = vfMirrorDirection(view, normal);
        return dot(direction, normal) > 0.0;
    }
    float alpha = roughness * roughness;
    float u1 = clamp(xi.x, 0.0, 0.999999);
    float u2 = clamp(xi.y, 0.0, 0.999999);
    float tangentSquared = alpha * alpha * u1 / (1.0 - u1);
    float cosTheta = 1.0 / sqrt(1.0 + tangentSquared);
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = 6.28318530718 * u2;

    vec3 tangent;
    vec3 bitangent;
    vfOrthonormalBasis(normal, tangent, bitangent);
    vec3 halfVector = normalize(tangent * (sinTheta * cos(phi)) +
        bitangent * (sinTheta * sin(phi)) + normal * cosTheta);
    direction = vfMirrorDirection(view, halfVector);
    return dot(direction, normal) > 0.0;
}

// A hit on the back of a two-sided surface flips its normal. Without the flip
// every dot product goes negative and the reflection resolves to black on
// exactly the surfaces two-sided rendering exists to support.
vec3 vfOrientHitNormal(vec3 normal, vec3 rayDirection, bool twoSided)
{
    if (!twoSided) return normal;
    return dot(normal, rayDirection) > 0.0 ? -normal : normal;
}

// Mirrors reflect::PropagateCone. Hit shaders have no implicit derivatives,
// so the footprint is carried explicitly or every hit reads mip 0.
float vfPropagateConeWidth(float width, float spreadAngle, float distance)
{
    return distance > 0.0 ? width + spreadAngle * distance : width;
}

// Mirrors reflect::SelectMipLevel. The texel count a cone covers at the hit,
// logged, which is what a derivative would have produced had there been one.
float vfSelectMipLevel(
    float coneWidth, float worldArea, float uvArea, float texelDimension)
{
    if (!(coneWidth > 0.0) || !(worldArea > 0.0) || !(uvArea > 0.0)) return 0.0;
    return log2(coneWidth * sqrt(uvArea / worldArea) * texelDimension);
}

// Mirrors reflect::ResolveMiss. A captured probe is a local measurement and
// beats the global environment; indoors with neither there is no sky an
// escaping ray could have come from, and substituting the exterior one is the
// light leak this rule prevents.
uint vfResolveMiss(GpuEnvironmentV1 environment, bool probeAvailable)
{
    if (probeAvailable) return kVfReflectionProbe;
    if ((environment.flagsAndCount.x & kVfEnvironmentPresent) == 0u) {
        return kVfReflectionUnresolved;
    }
    if ((environment.flagsAndCount.x & kVfEnvironmentInterior) != 0u) {
        return kVfReflectionUnresolved;
    }
    return kVfReflectionEnvironment;
}

vec3 vfMissRadiance(
    GpuEnvironmentV1 environment,
    vec3 direction,
    vec3 probeRadiance,
    bool probeAvailable)
{
    uint source = vfResolveMiss(environment, probeAvailable);
    if (source == kVfReflectionProbe) return probeRadiance;
    // Nothing captured could say what the ray saw. Zero is visible in the
    // frame; a plausible grey would be accepted by every comparison after it.
    if (source != kVfReflectionEnvironment) return vec3(0.0);

    vec3 radiance = environment.ambientAndFogNear.rgb;
    vec3 toSun = environment.sunDirectionAndFogFar.xyz;
    if (!(dot(toSun, toSun) > 0.0) || !(dot(direction, direction) > 0.0)) {
        return radiance;
    }
    toSun = normalize(-toSun);
    float cosine = max(0.0, dot(normalize(direction), toSun));
    return radiance + environment.sunColorAndIntensity.rgb *
        environment.sunColorAndIntensity.w * pow(cosine, kVfSunLobeExponent);
}

#ifdef VF_RAY_QUERY
// The complete reflection for one surface, mirroring reflect::
// EvaluateReflection. `source` reports which branch produced the radiance,
// because "black because nothing was hit" and "black because the sky is
// black" are different failures and only one of them is a bug.
vec3 vfReflection(
    vec3 position,
    vec3 geometricNormal,
    vec3 shadingNormal,
    vec3 view,
    vec3 baseColor,
    float roughness,
    float metalness,
    vec2 xi,
    vec3 probeRadiance,
    bool probeAvailable,
    out uint source)
{
    GpuEnvironmentV1 environment = sceneEnvironment.record;
    // Switched off for the frame, mirroring reflect::EvaluateReflection.
    // Exactly nothing rather than a small residue: a term that is almost off
    // still moves every pixel it touches.
    if ((environment.flagsAndCount.x & kVfEnvironmentReflectionDisabled)
        != 0u) {
        source = kVfReflectionSkipped;
        return vec3(0.0);
    }
    vec3 normal = normalize(shadingNormal);
    vec3 viewDirection = normalize(view);
    vec3 f0 = vfComputeF0(baseColor, metalness);
    vec3 fresnel = vfFresnelSchlick(f0, max(0.0, dot(normal, viewDirection)));

    // Rougher than the pass can resolve. The environment stands in for a lobe
    // too wide to trace, reported as policy rather than disguised as a hit.
    if (!(roughness <= kVfReflectionRoughnessCutoff)) {
        source = kVfReflectionSkipped;
        return fresnel * vfMissRadiance(environment,
            vfMirrorDirection(viewDirection, normal), probeRadiance,
            probeAvailable);
    }

    vec3 direction;
    if (!vfSampleReflectionDirection(normal, viewDirection, roughness, xi,
            direction)) {
        // A rejected sample still owes this pixel a ray; the lobe's centre is
        // the bounded answer, and leaving it unset would put a hole in the
        // reflection wherever the sampler happened to reject.
        direction = vfMirrorDirection(viewDirection, normal);
    }
    vec3 origin = vfOffsetRayOrigin(position, geometricNormal, 1.0);

    rayQueryEXT query;
    rayQueryInitializeEXT(query, sceneTlas, gl_RayFlagsOpaqueEXT, 0xFFu,
        origin, 0.0, direction, kVfReflectionMaximumDistance);
    while (rayQueryProceedEXT(query)) {}

    if (rayQueryGetIntersectionTypeEXT(query, true) ==
        gl_RayQueryCommittedIntersectionNoneEXT) {
        source = vfResolveMiss(environment, probeAvailable);
        return fresnel * vfMissRadiance(environment, direction, probeRadiance,
            probeAvailable);
    }

    // Which drawn geometry the ray found. The bottom level holds one geometry
    // per drawn instance, and the table maps that back to the object whose
    // records describe it -- a ray query has no vertex attributes bound, so
    // this is the only way both sides can read the same surface.
    uint geometry = rayQueryGetIntersectionGeometryIndexEXT(query, true);
    uint objectIndex = geometry < sceneGeometryObjects.records.length()
        ? sceneGeometryObjects.records[geometry] : 0u;
    float distance = rayQueryGetIntersectionTEXT(query, true);
    vec3 hitPosition = origin + direction * distance;
    vec3 hitNormal = vfOrientHitNormal(
        opaqueObjects.records[objectIndex].geometricNormal.xyz, direction,
        true);
    // The surface colour, tinted only where a tint is actually declared --
    // the same rule `vfApplyTint` applies in the raster pass, so a reflected
    // surface cannot disagree with the surface it reflects.
    //
    // This read `tintColor` alone, which `TranslateMaterialFamily` fills only
    // for a tinting family or an explicit tint flag. Every ordinary material
    // therefore reflected black, which is indistinguishable from a ray that
    // hit nothing at all.
    vec3 hitAlbedo = vfApplyTint(sceneFamilies.records[objectIndex],
        sceneFamilies.records[objectIndex].baseColor.rgb);


    source = kVfReflectionGeometry;
    // Shaded through the same function the raster pass uses, so a reflection
    // cannot disagree with the surface it is reflecting.
    return fresnel * vfShadeSurface(environment, hitAlbedo, hitPosition,
        hitNormal, length(hitPosition));
}
#endif
