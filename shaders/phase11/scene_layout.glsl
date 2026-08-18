struct GpuOpaqueObjectV1
{
    uvec2 objectId;
    uvec2 materialId;
    uint drawIndex;
    uint flags;
    float roughness;
    uint passSequence;
    vec4 modelRows[4];
    vec4 previousModelRows[4];
    vec4 boundsMinimum;
    vec4 boundsMaximum;
    vec4 geometricNormal;
    vec4 shadingNormal;
};

struct GpuSceneInstanceV1
{
    uvec2 objectId;
    uint objectIndex;
    uint flags;
    vec4 modelRows[4];
    vec4 previousModelRows[4];
    vec4 parameters;
};

layout(set = 0, binding = 7, std430) readonly buffer OpaqueObjects
{
    GpuOpaqueObjectV1 records[];
} opaqueObjects;

layout(set = 0, binding = 9, std430) readonly buffer SceneInstances
{
    GpuSceneInstanceV1 records[];
} sceneInstances;

// Mirrors vf::renderer::visibility::VisibilityRecordV1 byte for byte. The
// packed word holds the alpha source in its low byte and the alpha class in
// the next, exactly as the C++ record lays them out.
struct GpuVisibilityRecordV1
{
    uvec2 objectId;
    uvec2 materialId;
    uint alphaPacked;
    uint alphaFlags;
    float alphaReference;
    float alphaConstant;
    float alphaFade;
    uint alphaReserved0;
    uint alphaReserved1;
    uint alphaReserved2;
    uint faceMode;
    float modelDeterminant;
    uvec2 reserved;
};

layout(set = 0, binding = 13, std430) readonly buffer SceneVisibility
{
    GpuVisibilityRecordV1 records[];
} sceneVisibility;

// Mirrors vf::renderer::material::GpuFamilyRecordV1 byte for byte. Every
// specialized behaviour a family can request arrives as data in this record,
// which is what lets twenty-one families share eight pipeline classes.
struct GpuFamilyRecordV1
{
    uint familyPacked;
    uint featureFlags;
    uint emissionFlags;
    uint paletteFlags;
    vec4 emissionColor;
    vec4 tintColor;
    // The surface colour a ray-query hit shades with. A tint modulates it
    // and never replaces it: the tint is zero unless one is declared.
    vec4 baseColor;
    vec4 subsurface;
    vec4 parallax;
    vec4 eyeCenterRadius;
    vec4 layerAndEye;
    vec4 wetnessLow;
    vec4 wetnessHigh;
};

layout(set = 0, binding = 14, std430) readonly buffer SceneFamilies
{
    GpuFamilyRecordV1 records[];
} sceneFamilies;

// Mirrors vf::renderer::lighting::GpuLightRecordV1 byte for byte.
struct GpuLightRecordV1
{
    vec4 color;
    vec4 position;
    vec4 direction;
    vec4 attenuation;
    vec4 cone;
};

layout(set = 0, binding = 15, std430) readonly buffer SceneLights
{
    GpuLightRecordV1 records[];
} sceneLights;

// Mirrors vf::renderer::lighting::GpuEnvironmentV1 byte for byte.
struct GpuEnvironmentV1
{
    vec4 ambientAndFogNear;
    vec4 sunDirectionAndFogFar;
    vec4 sunColorAndIntensity;
    vec4 moonDirectionAndIntensity;
    vec4 moonColorAndFogMaximum;
    vec4 fogColorAndPower;
    uvec4 flagsAndCount;
};

layout(set = 0, binding = 16, std430) readonly buffer SceneEnvironment
{
    GpuEnvironmentV1 record;
} sceneEnvironment;

// The colour target as it stood before any refractive draw began. Outside the
// ray-query guard because a refractive surface reads it whether or not the
// device can trace: without ray query the reflection falls back, but the
// refraction is still whatever is behind the surface.
layout(set = 0, binding = 19) uniform sampler2D sceneRefractionSource;

// The frame's material textures, one per library entry, selected per draw
// through scenePush.textureIndex.
//
// A separate binding rather than a widening of binding 1: that one is the
// single base texture the phase 6, 9 and 16 shaders all declare, and each has
// a build-time reflection gate asserting its exact shape. Taking a fresh
// binding leaves every one of those untouched.
//
// Fixed size, and indexed without nonuniformEXT, because the index arrives in
// a push constant: it is therefore *dynamically uniform* across the draw, and
// non-uniform indexing is exactly what nonuniformEXT exists to permit. An
// unsized array would additionally have forced the extension into every
// translation unit that includes this file, several of which include it after
// another header's declarations, where an #extension directive is not valid.
//
// Entries past the frame's library count are never sampled but must still be
// bound, which is what descriptorBindingPartiallyBound is for.
const uint kSceneMaterialTextureCapacity = 256;
layout(set = 0, binding = 20) uniform sampler2D
    sceneMaterialTextures[kSceneMaterialTextureCapacity];

#ifdef VF_RAY_QUERY
// The scene's top level, holding one instance per drawn object in the same
// camera-relative space the fragments are shaded in. Declared only in the
// ray-query variant: the binding exists in the descriptor layout only when
// the device enabled the extension.
layout(set = 0, binding = 17) uniform accelerationStructureEXT sceneTlas;

// One entry per bottom-level geometry: everything a ray-query hit needs that
// the query itself cannot report. A query returns the geometry index and the
// barycentrics and nothing else, so the object it belongs to, where its
// triangle list starts, where its vertices start, and which texture shades it
// are all resolved on the host once per geometry and read back here.
//
// The element offsets are already absolute into the frame's shared upload
// buffer, so the shader needs no per-frame base and cannot get one wrong.
struct GpuGeometryRecordV1
{
    uint objectIndex;
    uint firstIndexElement;
    uint firstVertexFloat;
    uint textureIndex;
    uint flags;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

// Set when the frame's indices are 16-bit, which the shader has to unpack two
// to a word. Both widths occur -- the fixtures encode 16-bit and the live
// mirror emits 32-bit -- and reading one as the other walks a triangle list
// that does not exist.
const uint kVfGeometryIndexIs16Bit = 1u;

layout(set = 0, binding = 18, std430) readonly buffer SceneGeometryObjects
{
    GpuGeometryRecordV1 records[];
} sceneGeometryObjects;

// The frame's index stream, as words. A 16-bit frame packs two indices per
// word and unpacks them with the flag above.
layout(set = 0, binding = 21, std430) readonly buffer SceneIndices
{
    uint words[];
} sceneIndices;

// The frame's vertex stream, as floats. RasterVertexV3 is twelve of them:
// position, colour, texcoord, normal, pad.
layout(set = 0, binding = 22, std430) readonly buffer SceneVertices
{
    float values[];
} sceneVertices;

const uint kVfVertexFloats = 12u;
const uint kVfVertexColorFloat = 3u;
const uint kVfVertexTexCoordFloat = 6u;

uint vfGeometryIndex(GpuGeometryRecordV1 record, uint corner)
{
    uint element = record.firstIndexElement + corner;
    if ((record.flags & kVfGeometryIndexIs16Bit) == 0u) {
        return sceneIndices.words[element];
    }
    uint word = sceneIndices.words[element >> 1u];
    return (element & 1u) != 0u ? (word >> 16u) : (word & 0xFFFFu);
}

// The barycentric-weighted vertex colour at a hit. `bary` is what the query
// reports, which is the weight of the second and third corners; the first
// takes the remainder.
vec3 vfHitVertexColor(GpuGeometryRecordV1 record, uint triangle, vec2 bary)
{
    vec3 weights = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
    vec3 total = vec3(0.0);
    for (uint corner = 0u; corner < 3u; ++corner) {
        uint vertex = vfGeometryIndex(record, triangle * 3u + corner);
        uint base = record.firstVertexFloat + vertex * kVfVertexFloats +
            kVfVertexColorFloat;
        total += weights[corner] * vec3(sceneVertices.values[base],
            sceneVertices.values[base + 1u],
            sceneVertices.values[base + 2u]);
    }
    return total;
}

vec2 vfHitTexCoord(GpuGeometryRecordV1 record, uint triangle, vec2 bary)
{
    vec3 weights = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
    vec2 total = vec2(0.0);
    for (uint corner = 0u; corner < 3u; ++corner) {
        uint vertex = vfGeometryIndex(record, triangle * 3u + corner);
        uint base = record.firstVertexFloat + vertex * kVfVertexFloats +
            kVfVertexTexCoordFloat;
        total += weights[corner] * vec2(sceneVertices.values[base],
            sceneVertices.values[base + 1u]);
    }
    return total;
}

// The material texture at a hit, selected per geometry because the query
// reports no material, and sampled at level zero because a ray query has no
// derivatives to choose one from. The sentinel means the geometry names no
// texture, and white leaves the albedo exactly as it was -- the same rule the
// CPU oracle applies when a triangle carries no texture.
vec3 vfHitTexture(GpuGeometryRecordV1 record, uint triangle, vec2 bary)
{
    if (record.textureIndex == 0xFFFFFFFFu) return vec3(1.0);
    return textureLod(sceneMaterialTextures[record.textureIndex],
        vfHitTexCoord(record, triangle, bary), 0.0).rgb;
}
#endif

const uint kVfLightTypeAmbient = 0u;
const uint kVfLightTypeDirectional = 1u;
const uint kVfLightTypePoint = 2u;
const uint kVfLightTypeSpot = 3u;
const uint kVfEnvironmentInterior = 1u;
const uint kVfEnvironmentPresent = 2u;
// Switches the diffuse bounce off for the frame, so a contract can render the
// same frame twice with only that term changing and attribute the difference
// to it. Without it the bounce arrives alongside every other ray-traced term
// and none of them can be measured alone.
const uint kVfEnvironmentIndirectDisabled = 4u;
// The same switch for the specular bounce. Mirrors
// lighting::EnvironmentReflectionDisabled.
const uint kVfEnvironmentReflectionDisabled = 8u;
// Mirrors lighting::EnvironmentShadowsDisabled.
const uint kVfEnvironmentShadowsDisabled = 16u;

const uint kVfShaderClassStandard = 1u;
const uint kVfShaderClassSkin = 2u;
const uint kVfShaderClassHair = 3u;
const uint kVfShaderClassEye = 4u;
const uint kVfShaderClassParallax = 5u;
const uint kVfShaderClassMultiLayer = 6u;
const uint kVfShaderClassTerrain = 7u;
const uint kVfShaderClassLod = 8u;

const uint kVfFeatureSubsurface = 1u << 0;
const uint kVfFeatureRim = 1u << 1;
const uint kVfFeatureBacklight = 1u << 2;
const uint kVfFeatureAnisotropy = 1u << 3;
const uint kVfFeatureParallaxOffset = 1u << 4;
const uint kVfFeatureParallaxOcclusion = 1u << 5;
const uint kVfFeatureMultiLayer = 1u << 6;
const uint kVfFeatureEye = 1u << 7;
const uint kVfFeatureEnvironment = 1u << 8;
const uint kVfFeatureSnow = 1u << 9;
const uint kVfFeatureMultiIndex = 1u << 10;
const uint kVfFeatureWetness = 1u << 11;
const uint kVfFeatureLandscape = 1u << 12;
const uint kVfFeatureTreeAnimation = 1u << 13;
const uint kVfFeatureDismemberment = 1u << 14;
const uint kVfFeatureMeatCuff = 1u << 15;
const uint kVfFeatureReducedDetail = 1u << 16;
const uint kVfFeatureSky = 1u << 17;
const uint kVfFeatureNormalMap = 1u << 18;

const uint kVfEmissionEnabled = 1u << 0;
const uint kVfEmissionGlowMap = 1u << 1;
const uint kVfEmissionExternal = 1u << 2;

const uint kVfPaletteColor = 1u << 0;
const uint kVfPaletteAlpha = 1u << 1;

const uint kVfNormalTangentSpace = 0u;
const uint kVfNormalModelSpace = 1u;

uint vfMaterialFamily(GpuFamilyRecordV1 record)
{
    return record.familyPacked & 0xFFu;
}

uint vfShaderClass(GpuFamilyRecordV1 record)
{
    return (record.familyPacked >> 8) & 0xFFu;
}

uint vfNormalEncoding(GpuFamilyRecordV1 record)
{
    return (record.familyPacked >> 16) & 0xFFu;
}

bool vfHasFeature(GpuFamilyRecordV1 record, uint feature)
{
    return (record.featureFlags & feature) != 0u;
}

bool vfTintEnabled(GpuFamilyRecordV1 record)
{
    return record.tintColor.w != 0.0;
}

const uint kVfAlphaClassOpaque = 0u;
const uint kVfAlphaClassTested = 1u;
const uint kVfAlphaClassBlended = 2u;
const uint kVfAlphaClassUnclassified = 3u;
const uint kVfFaceModeTwoSided = 2u;
const uint kVfAlphaToCoverage = 1u;
const uint kVfDitherFade = 2u;

uint vfAlphaSource(GpuVisibilityRecordV1 record)
{
    return record.alphaPacked & 0xFFu;
}

uint vfAlphaClass(GpuVisibilityRecordV1 record)
{
    return (record.alphaPacked >> 8) & 0xFFu;
}

uint vfFaceMode(GpuVisibilityRecordV1 record)
{
    return record.faceMode & 0xFFu;
}

layout(push_constant) uniform ScenePushConstants
{
    uint objectIndex;
    uint firstInstance;
    // Non-zero when this draw is refractive. Carried per draw rather than
    // per object because the same mesh can be drawn refractive in one pass
    // and opaque in another.
    uint refractive;
    float indexOfRefraction;
    // The blend mode this draw is being composited with, raw so the shader
    // does not depend on the enum's definition. Opaque never reaches a
    // blended pipeline, so it also reads as "this pixel is entirely mine".
    uint blend;
    // The volume a decal projects into. A range of zero means this draw
    // projects nothing and composites as ordinary blended geometry.
    uint decalReceiverMask;
    uint decalReference;
    float decalRange;
    vec3 decalOrigin;
    float decalRadius;
    vec3 decalAxis;
    // Index into the bound texture array, or 0xFFFFFFFF for "no texture" --
    // which shades flat from the material base colour, exactly as the CPU
    // oracle does for the same sentinel.
    uint textureIndex;
} scenePush;
