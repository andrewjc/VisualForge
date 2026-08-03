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
