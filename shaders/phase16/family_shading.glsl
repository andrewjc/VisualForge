// Mirrors the Phase 16 CPU translator's shading rules. Every specialized
// behaviour arrives as data in the family record, so the twenty-one engine
// material families share eight pipeline classes rather than growing a
// technique permutation each.

const float kVfHorizonEpsilon = 1.0e-3;

// Duff et al.'s branchless orthonormal basis. The sign trick is what keeps
// the denominator away from zero for every input normal.
//
// This is a *generated* frame. The engine uses the mesh's authored tangents,
// which no capture supplies yet, so a tangent-space normal will not match
// vanilla until a per-vertex tangent capture exists. The construction is
// deterministic and mirrored on the CPU side, so the mirror agrees with
// itself; it does not yet agree with Bethesda.
void vfOrthonormalBasis(vec3 n, out vec3 t, out vec3 b)
{
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = vec3(c, s + n.y * n.y * a, -n.y);
}


// Parallax occlusion mapping: the texture coordinate is moved along the view
// direction until it lands where the height field would actually be seen,
// rather than where the flat surface is.
//
// The step count varies with the angle because that is where the cost and the
// artefact both are: a surface seen face-on needs almost none, and one seen
// edge-on steps a long way across the height field between samples and
// staircases if it takes too few. The record carries the range, so a material
// that needs more gets more.
//
// This is the textbook march, mirrored exactly on the CPU side, so the two
// agree with each other. Whether the convention agrees with Bethesda's own
// shader is not something offline work can answer -- the depth a given
// `parallaxScale` produces is a choice, and only a captured frame settles it.
vec2 vfParallaxOffset(
    GpuFamilyRecordV1 record,
    sampler2D heightMap,
    vec2 texCoord,
    vec3 geometric,
    vec3 toViewer)
{
    if (!vfHasFeature(record, kVfFeatureParallaxOcclusion)) return texCoord;
    vec3 tangent;
    vec3 bitangent;
    vfOrthonormalBasis(geometric, tangent, bitangent);
    // The view direction in the surface's own frame. Its z is the cosine
    // against the surface, and a ray travelling along the surface has no
    // depth to march through.
    vec3 view = vec3(dot(toViewer, tangent), dot(toViewer, bitangent),
        dot(toViewer, geometric));
    if (!(view.z > 1.0e-4)) return texCoord;

    float minimumSteps = max(1.0, record.wetnessHigh.z);
    float maximumSteps = max(minimumSteps, record.wetnessHigh.w);
    float steps = mix(maximumSteps, minimumSteps, clamp(view.z, 0.0, 1.0));
    float layerDepth = 1.0 / steps;
    vec2 scaledCoord = texCoord * record.parallax.zw;
    // The total distance the coordinate may travel, which is what
    // `parallaxScale` means: a scale of zero leaves a flat surface.
    vec2 sweep = (view.xy / view.z) * record.parallax.x;
    vec2 stepDelta = sweep * layerDepth;

    float currentDepth = 0.0;
    vec2 current = scaledCoord;
    float sampled = textureLod(heightMap, current, 0.0).r + record.parallax.y;
    // Bounded by the step count rather than by the exit condition alone: a
    // height field that never rises above the ray would otherwise march
    // forever.
    for (float step = 0.0; step < maximumSteps; step += 1.0) {
        if (sampled <= currentDepth) break;
        current -= stepDelta;
        currentDepth += layerDepth;
        sampled = textureLod(heightMap, current, 0.0).r + record.parallax.y;
    }

    // The intersection is between the last two samples, not at either of
    // them. Taking the nearer one is plain parallax mapping and steps
    // visibly; interpolating is what makes it occlusion mapping.
    vec2 previous = current + stepDelta;
    float afterDepth = sampled - currentDepth;
    float beforeDepth = textureLod(heightMap, previous, 0.0).r +
        record.parallax.y - currentDepth + layerDepth;
    float span = beforeDepth - afterDepth;
    float weight = abs(span) > 1.0e-6 ? beforeDepth / span : 0.0;
    vec2 marched = mix(current, previous, clamp(weight, 0.0, 1.0));
    // Back out of the height field's own UV scale, so the offset applies to
    // the coordinate the surface is shaded with.
    vec2 unscaled = record.parallax.zw;
    return vec2(
        unscaled.x != 0.0 ? marched.x / unscaled.x : texCoord.x,
        unscaled.y != 0.0 ? marched.y / unscaled.y : texCoord.y);
}

// Model-space normals must never pass through the tangent-normal path. The
// two decodes read a different number of channels *and* land in different
// spaces: a model-space texel is already absolute, while a tangent-space
// texel stores its Z along the surface normal and has to be rotated into the
// surface frame before it means anything. Treating the second as the first
// points every normal away from the surface.
vec3 vfDecodeNormal(
    GpuFamilyRecordV1 record,
    vec4 sampled,
    vec3 geometric,
    float faceSign)
{
    if (vfNormalEncoding(record) == kVfNormalModelSpace) {
        // Absolute, so a two-sided back face flips it like any other normal.
        return (sampled.xyz * 2.0 - 1.0) * faceSign;
    }
    // Two-channel tangent-space storage reconstructs Z from the unit length
    // it was compressed against. The frame is built from the already
    // face-signed geometric normal, so no second flip is needed.
    vec2 xy = sampled.xy * 2.0 - 1.0;
    float z = sqrt(max(0.0, 1.0 - dot(xy, xy)));
    vec3 tangent;
    vec3 bitangent;
    vfOrthonormalBasis(geometric, tangent, bitangent);
    return tangent * xy.x + bitangent * xy.y + geometric * z;
}

// Mirrors visibility::ResolveShadingFrame's horizon lift: a shading normal
// below the geometric horizon is lifted onto it rather than being allowed to
// light the surface from behind.
vec3 vfLiftToHorizon(vec3 geometric, vec3 shading)
{
    float alignment = dot(geometric, shading);
    if (alignment <= kVfHorizonEpsilon) {
        shading += geometric * (kVfHorizonEpsilon - alignment);
    }
    float length2 = dot(shading, shading);
    return length2 > 0.0 ? shading * inversesqrt(length2) : geometric;
}

// Tint is a declaration, not an observation: a captured tint colour alone
// never tints, which is what keeps an ordinary surface from picking up a
// stale palette value.
vec3 vfApplyTint(GpuFamilyRecordV1 record, vec3 albedo)
{
    return vfTintEnabled(record) ? albedo * record.tintColor.rgb : albedo;
}

// Emission is authorized by a glow map, own-emit, external emittance, or the
// glow-map family. A bright albedo never becomes emission on its own.
vec3 vfEmission(GpuFamilyRecordV1 record, vec3 glowSample)
{
    if ((record.emissionFlags & kVfEmissionEnabled) == 0u) {
        return vec3(0.0);
    }
    // Externally driven emission is owned by the reference, not the
    // material, so the material asserts no colour of its own here.
    if ((record.emissionFlags & kVfEmissionExternal) != 0u) {
        return vec3(0.0);
    }
    vec3 emission = record.emissionColor.rgb;
    if ((record.emissionFlags & kVfEmissionGlowMap) != 0u) {
        emission *= glowSample;
    }
    return emission;
}

// The roughness a class contributes on top of the object's own. Skin and
// hair are the two classes whose lobe the general specular/smoothness
// evaluator does not describe, so they are the only ones that adjust it.
float vfClassRoughness(GpuFamilyRecordV1 record, float roughness)
{
    uint shaderClass = vfShaderClass(record);
    if (shaderClass == kVfShaderClassSkin) {
        // Subsurface rolloff widens the effective lobe.
        return clamp(roughness + record.subsurface.x * 0.5, 0.0, 1.0);
    }
    if (shaderClass == kVfShaderClassHair &&
        vfHasFeature(record, kVfFeatureAnisotropy)) {
        // An anisotropic lobe is narrower along the fibre than the isotropic
        // roughness would suggest.
        return clamp(roughness * 0.5, 0.0, 1.0);
    }
    return roughness;
}
