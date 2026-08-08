#ifndef VF_PHASE20_TEMPORAL_GLSL
#define VF_PHASE20_TEMPORAL_GLSL

// The temporal half of the pass, mirrored from renderer_core/EngineIndirect.
// The replay compares the device against the same functions on the host, so a
// change to one that is not made to the other is a test failure rather than a
// slow drift in how quickly indirect light settles that nobody attributes to
// this file.

// Mirrors gi::IndirectRules::radianceClamp. One unlucky path survives
// temporal accumulation as a bright dot for seconds; clamped rather than
// discarded, because discarding biases the mean darker while clamping only
// bounds the variance.
const float kVfIndirectClamp = 8.0;

// Mirrors gi::ClampRadiance. A non-finite value is a failed path, not an
// infinitely bright one: carrying it into an accumulator poisons the pixel
// permanently. It lives beside accumulation because bounding what enters the
// accumulator is what it is for.
vec3 vfClampIndirect(vec3 radiance)
{
    vec3 bounded;
    for (int channel = 0; channel < 3; ++channel) {
        float value = radiance[channel];
        bounded[channel] = (isnan(value) || isinf(value))
            ? 0.0 : clamp(value, 0.0, kVfIndirectClamp);
    }
    return bounded;
}

const uint kVfRejectAccepted = 0u;
const uint kVfRejectEpoch = 1u;
const uint kVfRejectOffScreen = 2u;
const uint kVfRejectDepth = 3u;
const uint kVfRejectNormal = 4u;
const uint kVfRejectObject = 5u;
const uint kVfRejectMaterial = 6u;

// The longest history a pixel may accumulate. A quality parameter, not a
// correctness one: longer converges further but responds later.
const uint kVfIndirectMaximumHistory = 32u;
// Relative rather than absolute. An absolute epsilon fails at distance, where
// a pixel's depth changes by metres between frames without the surface moving.
const float kVfIndirectDepthTolerance = 0.05;
const float kVfIndirectNormalCosine = 0.9;

struct VfIndirectHistory
{
    // Mean and second moment together, because variance is what the spatial
    // filter needs and it cannot be recovered from the mean alone.
    vec3 mean;
    vec3 secondMoment;
    uint samples;
};

// Mirrors gi::Reproject. Returns the reason a history sample may not be
// believed, and the pixel it came from when it may. Every gate here is a
// separate reason on purpose: a single "rejected" cannot tell a camera cut
// from a surface that merely turned, and the two have different fixes.
uint vfReprojectIndirect(
    vec3 currentNormal,
    vec3 previousNormal,
    float currentDepth,
    float previousDepth,
    uvec2 currentObjectAndMaterial,
    uvec2 previousObjectAndMaterial,
    vec2 motion,
    uvec2 pixel,
    uvec2 extent,
    bool epochMatches,
    out uvec2 source)
{
    source = pixel;
    // The history epoch first. A frame that reset the history has nothing to
    // reproject from, and testing the surface before the epoch would accept a
    // sample belonging to a scene that no longer exists.
    if (!epochMatches) return kVfRejectEpoch;
    if (extent.x == 0u || extent.y == 0u) return kVfRejectOffScreen;
    if (isnan(motion.x) || isnan(motion.y) ||
        isinf(motion.x) || isinf(motion.y)) {
        return kVfRejectOffScreen;
    }

    // The motion is truncated toward zero and then added, which is what the
    // host does by converting it to an integer first. Flooring the sum instead
    // would agree for positive motion and disagree by one pixel for negative,
    // so half the screen would sample the wrong texel whenever the camera
    // panned the other way -- a difference that looks like a shimmer along
    // edges rather than like an off-by-one.
    float targetX = float(pixel.x) + trunc(motion.x);
    float targetY = float(pixel.y) + trunc(motion.y);
    if (targetX < 0.0 || targetY < 0.0 ||
        targetX >= float(extent.x) || targetY >= float(extent.y)) {
        return kVfRejectOffScreen;
    }
    source = uvec2(uint(targetX), uint(targetY));

    // Relative to the current depth, floored so a surface at the near plane
    // does not divide by nothing. Absolute epsilons fail at distance, where a
    // pixel's depth changes by several units between frames without the
    // surface having moved, and reject every distant pixel forever.
    float reference = max(abs(currentDepth), 1.0e-4);
    if (isnan(currentDepth) || isnan(previousDepth) ||
        isinf(currentDepth) || isinf(previousDepth) ||
        abs(currentDepth - previousDepth) / reference >
            kVfIndirectDepthTolerance) {
        return kVfRejectDepth;
    }

    // A normal that will not normalise is rejected here rather than allowed to
    // produce a NaN dot product that compares false against every threshold
    // and so passes the gate it should fail.
    if (dot(currentNormal, currentNormal) <= 0.0 ||
        dot(previousNormal, previousNormal) <= 0.0) {
        return kVfRejectNormal;
    }
    vec3 a = normalize(currentNormal);
    vec3 b = normalize(previousNormal);
    if (dot(a, b) < kVfIndirectNormalCosine) return kVfRejectNormal;

    // Object and material are separate tests because two objects can share a
    // material and one object can change material without moving.
    if (currentObjectAndMaterial.x != previousObjectAndMaterial.x) {
        return kVfRejectObject;
    }
    if (currentObjectAndMaterial.y != previousObjectAndMaterial.y) {
        return kVfRejectMaterial;
    }
    return kVfRejectAccepted;
}

// Mirrors gi::Accumulate. A rejected sample resets rather than blends:
// blending one in is exactly the trail the gate exists to forbid, where the
// previous scene stays visible and fading for as long as history is allowed.
VfIndirectHistory vfAccumulateIndirect(
    VfIndirectHistory history,
    vec3 sample_,
    uint reason)
{
    VfIndirectHistory updated;
    if (reason != kVfRejectAccepted || history.samples == 0u) {
        updated.mean = sample_;
        updated.secondMoment = sample_ * sample_;
        updated.samples = 1u;
        return updated;
    }
    uint cap = max(1u, kVfIndirectMaximumHistory);
    updated.samples = min(history.samples + 1u, cap);
    // An exponential moving average weighted by one over the current length:
    // identical to a true average while the history is short, and a fixed-rate
    // filter once capped. That is what keeps a converged pixel converged
    // without freezing it against real change.
    float weight = 1.0 / float(updated.samples);
    updated.mean = history.mean + (sample_ - history.mean) * weight;
    vec3 squared = sample_ * sample_;
    updated.secondMoment =
        history.secondMoment + (squared - history.secondMoment) * weight;
    return updated;
}

// Mirrors gi::Variance. Clamped at zero: the difference of two moments can go
// slightly negative through float cancellation, never physically, and a
// negative variance would make the spatial filter widen where it should
// narrow.
vec3 vfIndirectVariance(VfIndirectHistory history)
{
    return max(vec3(0.0),
        history.secondMoment - history.mean * history.mean);
}

#endif
