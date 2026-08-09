#ifndef VF_PHASE21_TRANSPARENCY_GLSL
#define VF_PHASE21_TRANSPARENCY_GLSL

// The blend contract, mirrored from renderer_core/EngineTransparency.cpp. The
// two are compared pixel for pixel by the transparency replay, so a change to
// one that is not made to the other is a test failure rather than a rendering
// difference somebody notices later.

// Must match blend::BlendMode. The composite is a switch on this value, so a
// renumbering that is not mirrored silently blends every surface with the
// wrong rule.
const uint kVfBlendOpaque = 0u;
const uint kVfBlendStraightAlpha = 1u;
const uint kVfBlendPremultiplied = 2u;
const uint kVfBlendAdditive = 3u;
const uint kVfBlendMultiply = 4u;

// One transparent draw's blend state, as the scene packet carries it.
struct VfTransparentDraw
{
    uint blend;
    float softFade;
    float dissolve;
    float dissolveFalloff;
};

vec3 vfComposite(
    VfTransparentDraw draw,
    vec4 source,
    vec3 destination)
{
    float alpha = clamp(source.a, 0.0, 1.0);
    vec3 result;
    if (draw.blend == kVfBlendOpaque) {
        result = source.rgb;
    } else if (draw.blend == kVfBlendStraightAlpha) {
        result = source.rgb * alpha + destination * (1.0 - alpha);
    } else if (draw.blend == kVfBlendPremultiplied) {
        // Already scaled by its own alpha. Scaling again is the double
        // darkening that shows around every edge.
        result = source.rgb + destination * (1.0 - alpha);
    } else if (draw.blend == kVfBlendAdditive) {
        // The destination keeps its full share, which is what makes fire
        // brighten what is behind it rather than replacing it.
        result = source.rgb * alpha + destination;
    } else if (draw.blend == kVfBlendMultiply) {
        // Darkens and can never brighten, which is what a scorch mark needs.
        result = destination * (source.rgb * alpha + (1.0 - alpha));
    } else {
        // An unrecognised mode leaves the destination alone. Falling through
        // to a blend would put an effect on screen under a rule nobody chose.
        result = destination;
    }
    // A non-finite channel keeps what was already there. One bad particle
    // otherwise turns the pixel white and the whole bloom chain follows it.
    bvec3 finite = bvec3(
        !isinf(result.x) && !isnan(result.x),
        !isinf(result.y) && !isnan(result.y),
        !isinf(result.z) && !isnan(result.z));
    return mix(destination, result, vec3(finite));
}

// Distance in view-space units, not a depth-buffer difference: the latter is
// non-linear, so a fade tuned at one camera setting would fade differently at
// another.
float vfSoftFade(VfTransparentDraw draw, float sceneDepth, float fragmentDepth)
{
    if (!(draw.softFade > 0.0)) return 1.0;
    if (isinf(sceneDepth) || isnan(sceneDepth) ||
        isinf(fragmentDepth) || isnan(fragmentDepth)) {
        return 1.0;
    }
    float separation = sceneDepth - fragmentDepth;
    if (separation <= 0.0) return 0.0;
    return clamp(separation / draw.softFade, 0.0, 1.0);
}

float vfDissolveCoverage(VfTransparentDraw draw, float noise)
{
    // Nothing authored leaves the effect alone rather than fading it.
    if (!(draw.dissolve > 0.0)) return 1.0;
    if (isinf(noise) || isnan(noise)) return 1.0;
    if (!(draw.dissolveFalloff > 0.0)) {
        // The hard cut, still exact.
        return noise > draw.dissolve ? 1.0 : 0.0;
    }
    float lower = draw.dissolve - draw.dissolveFalloff * 0.5;
    float upper = draw.dissolve + draw.dissolveFalloff * 0.5;
    if (noise <= lower) return 0.0;
    if (noise >= upper) return 1.0;
    return clamp((noise - lower) / (upper - lower), 0.0, 1.0);
}

// The alpha a transparent fragment actually contributes: its own, narrowed by
// the soft fade and the dissolve. Applied to alpha rather than to colour, so
// an additive effect fades out instead of merely dimming toward its own hue.
float vfTransparentAlpha(
    VfTransparentDraw draw,
    float sourceAlpha,
    float sceneDepth,
    float fragmentDepth,
    float noise)
{
    return clamp(sourceAlpha, 0.0, 1.0) *
        vfSoftFade(draw, sceneDepth, fragmentDepth) *
        vfDissolveCoverage(draw, noise);
}

// A decal's projection volume, mirrored from transparency::DecalProjection.
// A box along an axis rather than a sphere, because a decal is projected from
// somewhere onto something and both the direction and the reach matter.
struct VfDecalProjection
{
    vec3 origin;
    vec3 axis;
    float range;
    float radius;
};

// Mirrors transparency::ProjectDecal. How much of this surface the decal
// covers, and zero for every reason it should not be here at all.
//
// The three rejections test different things and each is an artefact somebody
// would report: past the range a decal stretches across every surface behind
// the one it was meant for; outside the radius it bleeds onto its neighbours;
// facing away it wraps around to the back of the wall.
float vfProjectDecal(
    VfDecalProjection projection,
    vec3 surfacePosition,
    vec3 surfaceNormal,
    uint surfaceStencil,
    uint stencilReceiverMask,
    uint stencilReference)
{
    if (dot(projection.axis, projection.axis) <= 0.0) return 0.0;
    if (!(projection.range > 0.0) || !(projection.radius > 0.0) ||
        isinf(projection.range) || isinf(projection.radius) ||
        isnan(projection.range) || isnan(projection.radius)) {
        return 0.0;
    }
    vec3 axis = normalize(projection.axis);

    // A surface the engine did not mark as a receiver takes nothing. Without
    // this a decal lands on the sky and on characters walking past it.
    if (stencilReceiverMask != 0u &&
        (surfaceStencil & stencilReceiverMask) !=
            (stencilReference & stencilReceiverMask)) {
        return 0.0;
    }

    vec3 offset = surfacePosition - projection.origin;
    float along = dot(offset, axis);
    if (along < 0.0 || along > projection.range) return 0.0;

    vec3 lateral = offset - axis * along;
    float lateralDistance = sqrt(dot(lateral, lateral));
    if (lateralDistance > projection.radius) return 0.0;

    if (dot(surfaceNormal, surfaceNormal) <= 0.0) return 0.0;
    float facing = -dot(normalize(surfaceNormal), axis);
    if (facing <= 0.0) return 0.0;

    // Strongest head on and at the centre, fading to nothing at the rim. The
    // facing term is what stops a decal on a grazing surface reading as a
    // smear rather than as a mark.
    return clamp(facing * (1.0 - lateralDistance / projection.radius),
        0.0, 1.0);
}

// Mirrors transparency::ReactiveMask. How much of a pixel this effect actually
// decides, which is what an upscaler needs in order to stop treating a
// particle as if it were the geometry behind it.
//
// Not the alpha alone: an additive draw contributes its full radiance whatever
// its alpha says, so a bright additive spark with an alpha of zero still owns
// its pixel. Taking the alpha there would tell the upscaler to reconstruct the
// spark from history that never contained it.
float vfReactiveMask(uint blend, vec4 source)
{
    float alpha = clamp(source.a, 0.0, 1.0);
    if (blend == kVfBlendAdditive) {
        float brightest = 0.0;
        for (int channel = 0; channel < 3; ++channel) {
            float value = source[channel];
            if (!isnan(value) && !isinf(value)) {
                brightest = max(brightest, value);
            }
        }
        return clamp(max(alpha, brightest), 0.0, 1.0);
    }
    return alpha;
}

#endif
