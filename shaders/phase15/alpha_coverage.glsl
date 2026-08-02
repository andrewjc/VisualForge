// Mirrors vf::renderer::visibility::EvaluateCoverage exactly, including the
// order in which the dither gate and the alpha test are applied. The depth
// prepass and the color pass both call this, which is what makes their
// silhouettes identical by construction rather than by coincidence.

const float kVfDitherLevels = 16.0;

struct VfCoverage
{
    bool covered;
    // The opacity the surface actually has, which is what the G-buffer
    // stores. It is NOT the sampled texture alpha: an opaque surface has
    // opacity one no matter what its base texture carries in that channel,
    // and a cutout fragment that survives the test is fully opaque.
    float coverage;
};

float vfDitherThreshold(uvec2 pixel)
{
    // 4x4 Bayer holding each of the sixteen levels once, matching
    // visibility::DitherThreshold.
    const uint kBayer[16] = uint[16](
        0u, 8u, 2u, 10u,
        12u, 4u, 14u, 6u,
        3u, 11u, 1u, 9u,
        15u, 7u, 13u, 5u);
    uint x = pixel.x % 4u;
    uint y = pixel.y % 4u;
    return float(kBayer[y * 4u + x]) / kVfDitherLevels;
}

VfCoverage vfEvaluateCoverage(
    GpuVisibilityRecordV1 record,
    float surfaceAlpha,
    uvec2 pixel)
{
    VfCoverage result = VfCoverage(false, 0.0);
    uint alphaClass = vfAlphaClass(record);
    if (alphaClass == kVfAlphaClassUnclassified) {
        return result;
    }
    // An opaque surface never consults alpha, even a zero one. Its base
    // texture is free to carry a mask or a height in that channel.
    if (alphaClass == kVfAlphaClassOpaque) {
        return VfCoverage(true, 1.0);
    }
    if (isinf(surfaceAlpha) || isnan(surfaceAlpha)) {
        return result;
    }
    // The fade gate is a property of the pixel, not of the pass.
    if ((record.alphaFlags & kVfDitherFade) != 0u &&
        !(record.alphaFade > vfDitherThreshold(pixel))) {
        return result;
    }
    float effective = clamp(surfaceAlpha * record.alphaConstant, 0.0, 1.0);
    if (alphaClass == kVfAlphaClassBlended) {
        return VfCoverage(true, effective);
    }
    // Alpha-to-coverage has no partial coverage to give at one sample per
    // pixel, so it resolves exactly like a plain alpha test and needs no
    // separate branch here. The engine keeps a fragment whose alpha reaches
    // the reference, and that fragment is then fully opaque.
    result.covered = effective >= record.alphaReference;
    result.coverage = result.covered ? 1.0 : 0.0;
    return result;
}

// A back face of a two-sided surface is shaded from the side the viewer is
// actually looking at, so both normals flip with it.
float vfFaceSign(GpuVisibilityRecordV1 record, bool frontFacing)
{
    return (vfFaceMode(record) == kVfFaceModeTwoSided && !frontFacing)
        ? -1.0 : 1.0;
}
