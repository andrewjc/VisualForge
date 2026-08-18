#version 460
#extension GL_GOOGLE_include_directive : require
#ifdef VF_RAY_QUERY
// Ray query is a compile-time capability of the module, not a runtime branch:
// a device without it cannot create a pipeline from this SPIR-V at all. The
// build therefore emits two variants of this shader and the backend picks by
// what the device actually enabled.
#extension GL_EXT_ray_query : require
#endif

#include "../phase11/scene_layout.glsl"
#include "../phase15/alpha_coverage.glsl"
#include "../phase17/lighting.glsl"
#include "family_shading.glsl"
#include "../phase19/reflection.glsl"
#include "../phase20/indirect.glsl"
#include "../phase21/transparency.glsl"
#include "../phase22/water.glsl"

// One pipeline for every specialized opaque material family. What differs
// between a skin, a hair, a glow-mapped, and an ordinary surface arrives in
// the family record rather than in a separate technique.

layout(set = 0, binding = 0, std140) uniform MaterialConstants
{
    vec4 baseColor;
} material;

layout(set = 0, binding = 1) uniform sampler2D baseTexture;
layout(set = 0, binding = 2) uniform sampler2D normalTexture;
// The material.s third texture slot. A material that declares
// MaterialSlotRole::GlowMap there uses it as an emission mask; every other
// material leaves it bound to the neutral white the backend supplies, which
// is what makes sampling it unconditionally safe.
layout(set = 0, binding = 3) uniform sampler2D materialSlotTwo;

layout(location = 0) in vec3 vertexColor;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) flat in uint sceneObjectIndex;
layout(location = 3) flat in uint sceneInstanceIndex;
layout(location = 4) in vec3 vertexCameraRelative;
layout(location = 5) in vec3 vertexNormal;

layout(location = 0) out vec4 hdrColor;
layout(location = 1) out vec4 gbufferAlbedo;
layout(location = 2) out vec4 gbufferGeometricNormalRoughness;
layout(location = 3) out vec4 gbufferShadingNormalDepth;
layout(location = 4) out uvec4 gbufferIdentity;
// How much of this pixel a transparent effect decides. Opaque geometry decides
// all of it and says so; a transparent draw says what its blend and its source
// colour make it worth. An upscaler without this reconstructs a particle from
// history that never contained it.
layout(location = 5) out vec4 gbufferReactive;

void main()
{
    GpuOpaqueObjectV1 objectRecord =
        opaqueObjects.records[sceneObjectIndex];
    GpuSceneInstanceV1 instanceRecord =
        sceneInstances.records[sceneInstanceIndex];
    GpuVisibilityRecordV1 visibleRecord =
        sceneVisibility.records[sceneObjectIndex];
    GpuFamilyRecordV1 familyRecord =
        sceneFamilies.records[sceneObjectIndex];

    vec4 sampled = textureLod(baseTexture, vertexTexCoord, 0.0);
    float surfaceAlpha = material.baseColor.a * sampled.a *
        instanceRecord.parameters.a;
    VfCoverage coverage = vfEvaluateCoverage(
        visibleRecord, surfaceAlpha, uvec2(gl_FragCoord.xy));
    if (!coverage.covered) {
        discard;
    }

    // A decal is a projection, not a quad. Its geometry is only the volume it
    // is evaluated over; what it actually covers is decided here, against this
    // fragment's own position and normal. Without it a scorch mark is a card
    // hanging in the air that stays the same size however the wall it is meant
    // for turns away.
    //
    // The stencil receiver test is passed a mask of zero, which is the host
    // contract's own "no receiver restriction" path: this pass has no stencil
    // to read, and the receiver test is applied where the receiver is known.
    // Passing the draw's own reference as if it were the receiver's would make
    // the test compare a value against itself and always agree.
    if (scenePush.decalRange > 0.0) {
        VfDecalProjection projection;
        projection.origin = scenePush.decalOrigin;
        projection.axis = scenePush.decalAxis;
        projection.range = scenePush.decalRange;
        projection.radius = scenePush.decalRadius;
        float projected = vfProjectDecal(projection, vertexCameraRelative,
            vertexNormal, 0u, 0u, 0u);
        if (!(projected > 0.0)) {
            discard;
        }
        coverage.coverage *= projected;
    }

    vec3 shaded = vertexColor * material.baseColor.rgb * sampled.rgb *
        instanceRecord.parameters.rgb;
    shaded = vfApplyTint(familyRecord, shaded);

    float faceSign = vfFaceSign(visibleRecord, gl_FrontFacing);
    // The interpolated per-vertex normal when the mesh carried one, and the
    // object's own axis when it did not. The per-object normal shades every
    // surface of an object as one flat plane, which reads as the lighting
    // being wrong rather than as the normal being an approximation; the
    // length test is what tells a real normal from an absent one, because a
    // packet migrated from before normals existed carries a unit default and
    // an interpolation across a degenerate triangle can carry nothing.
    float vertexNormalLength = length(vertexNormal);
    vec3 geometric = vertexNormalLength > 1.0e-4
        ? normalize(vertexNormal * faceSign)
        : normalize(objectRecord.geometricNormal.xyz * faceSign);

    // The normal texture is decoded according to the declared encoding and
    // then lifted onto the geometric horizon, so a perturbation can never
    // light the surface from behind. Without a bound normal map the object's
    // own shading normal stands, which is what keeps a family-less scene
    // identical to the Phase 11 mirror.
    vec3 shading = objectRecord.shadingNormal.xyz * faceSign;
    if (vfHasFeature(familyRecord, kVfFeatureNormalMap)) {
        vec4 normalSample = textureLod(normalTexture, vertexTexCoord, 0.0);
        vec3 decoded = vfDecodeNormal(
            familyRecord, normalSample, geometric, faceSign);
        float decodedLength2 = dot(decoded, decoded);
        if (decodedLength2 > 0.0) {
            shading = decoded * inversesqrt(decodedLength2);
        }
    }
    shading = vfLiftToHorizon(geometric, shading);

    float roughness = vfClassRoughness(familyRecord, objectRecord.roughness);

    // The material bundle binds base, normal, and smooth/spec only; the
    // engine's glow map is slot 2 of the shader texture set and has no
    // binding here yet. A glow-mapped material therefore emits its declared
    // colour unmodulated rather than borrowing an unrelated channel. The
    // declaration still travels in the record, so the modulation can be
    // added without another capture change.
    // The glow mask, sampled through the same coordinates the base colour
    // used. It was a hardcoded white, so a material declaring a glow map got
    // its declared colour unmasked and the map was carried to the device and
    // never read.
    vec3 emission = vfEmission(familyRecord,
        texture(materialSlotTwo, vertexTexCoord).rgb);
    // The mirrored opaque scene receives captured direct and ambient light,
    // then fog, in linear HDR. Emission is added after shading because it is
    // radiance the surface emits rather than reflects.
    vec3 lit = vfShadeSurface(
        sceneEnvironment.record,
        shaded,
        vertexCameraRelative,
        shading,
        length(vertexCameraRelative));
    // One bounce of specular, traced against the same structure the shadow
    // ray uses. It is added rather than blended because Fresnel has already
    // scaled it by how much of the incoming light this surface reflects.
    vec3 reflection = vec3(0.0);
#ifdef VF_RAY_QUERY
    uint reflectionSource;
    uint reflectionHitObject = 0u;
    uint reflectionHitPrimitive = 0u;
    reflection = vfReflection(
        vertexCameraRelative,
        geometric,
        shading,
        normalize(-vertexCameraRelative),
        shaded,
        roughness,
        vfMetalness(familyRecord),
        vfSampleSequence(uint(gl_FragCoord.x), uint(gl_FragCoord.y),
            0u, 0u),
        vec3(0.0),
        false,
        reflectionSource,
        reflectionHitObject,
        reflectionHitPrimitive);
#endif
    // One bounce of diffuse indirect, traced against the same structure.
    vec3 indirect = vec3(0.0);
#ifdef VF_RAY_QUERY
    uint indirectSource;
    float indirectProbe = 0.0;
    indirect = vfIndirect(vertexCameraRelative, geometric, shaded,
        uint(gl_FragCoord.x), uint(gl_FragCoord.y), 0u, indirectSource,
        indirectProbe);
#endif
    vec3 surface = lit + emission + reflection + indirect;
    // A refractive surface splits between what it reflects and what it lets
    // through, and the two shares are a partition of one. Reading the
    // snapshot rather than the live target is what keeps what shows through
    // it independent of which refractive draws came before.
    if (scenePush.refractive != 0u) {
        VfWaterSurface water;
        water.ior = scenePush.indexOfRefraction;
        water.fresnelBias = 0.02;
        water.fogDensity = 0.0;
        water.fogColor = vec3(0.0);
        water.shallowColor = vec3(0.0);
        water.deepColor = vec3(0.0);
        water.depthRange = 0.0;
        vec2 screenUv = gl_FragCoord.xy / vec2(textureSize(
            sceneRefractionSource, 0));
        vec3 behind = textureLod(sceneRefractionSource, screenUv, 0.0).rgb;
        float cosine = clamp(dot(shading,
            normalize(-vertexCameraRelative)), 0.0, 1.0);
        surface = vfWaterShade(water, surface, behind, cosine, 0.0);
    }
    hdrColor = vec4(surface, coverage.coverage);

    gbufferAlbedo = vec4(
        clamp(shaded, vec3(0.0), vec3(1.0)),
        clamp(coverage.coverage, 0.0, 1.0));
    gbufferGeometricNormalRoughness = vec4(geometric, roughness);
    gbufferShadingNormalDepth = vec4(shading, gl_FragCoord.z);
    gbufferIdentity = uvec4(
        instanceRecord.objectId, objectRecord.materialId);
    // Opaque geometry decides none of this: it is the stable part of the
    // frame and an upscaler reprojects it from history. A blended draw asks
    // the transparency contract, which weighs an additive spark by its
    // radiance rather than by an alpha the spark does not use.
    //
    // The draw's own alpha rather than the resolved coverage: coverage is
    // what the alpha class decided this fragment contributes to the target,
    // and an opaque-classified blended draw has a coverage of one whatever
    // its material says. The mask is about the effect, so it reads the
    // effect's alpha -- which is also what the host contract takes.
    // The reactive mask, and beside it what this pixel's reflection ray
    // found. The three lanes after the mask were reserved and unused; the
    // readback already carries them, so reporting the hit costs no
    // attachment and no ABI.
    //
    // This is what lets a contract compare two intersectors honestly. A
    // radiance difference alone cannot separate "the two shaded the same hit
    // differently" from "the two hit different things", and the second is not
    // a defect in either. With the hit reported, those pixels can be excluded
    // by name and counted, which is the difference between an exclusion and a
    // widened bound.
    float reflectionSourceOut = 0.0;
    float reflectionHitOut = 0.0;
    float reflectionPrimitiveOut = 0.0;
    float indirectProbeOut = 0.0;
#ifdef VF_RAY_QUERY
    reflectionSourceOut = float(reflectionSource);
    reflectionHitOut = float(reflectionHitObject);
    reflectionPrimitiveOut = float(reflectionHitPrimitive);
    indirectProbeOut = indirectProbe;
#endif
    gbufferReactive = vec4(
        scenePush.blend == kVfBlendOpaque
            ? 0.0
            : vfReactiveMask(scenePush.blend,
                  vec4(surface, surfaceAlpha)),
        reflectionSourceOut, reflectionHitOut, indirectProbeOut);
}
