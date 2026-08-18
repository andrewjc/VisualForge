#pragma once

#include "renderer_core/EngineAcceleration.h"
#include "renderer_core/EngineLighting.h"
#include "renderer_core/EngineMaterialFamily.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::reflect {

// Where a reflection ray's radiance came from. Recorded rather than folded
// into the colour, because "black because nothing was hit" and "black because
// the sky is black" are different failures and only one of them is a bug.
enum class ReflectionSource : std::uint8_t
{
    // The ray was never traced: the surface is rougher than the policy's
    // cutoff, so the environment stands in for a lobe too wide to resolve.
    Skipped = 0,
    Geometry = 1,
    Environment = 2,
    Probe = 3,
    // Traced, missed, and no environment or probe was captured. The frame
    // says so instead of inventing a colour.
    Unresolved = 4,
};

enum class ReflectError : std::uint8_t
{
    None,
    NonFiniteSource,
    DegenerateNormal,
    DegenerateView,
    InvalidRoughness,
    InvalidFootprint,
    InvalidTexture,
};

// Dielectrics reflect about 4% at normal incidence; metals reflect their own
// base colour and have no diffuse term at all. The plan asks for authored
// F0/roughness, and this is the one place the two are separated.
inline constexpr float kDielectricF0 = 0.04f;

// The reflection oracle shadows a hit through a fixed buffer rather than
// allocating per pixel. A frame carrying more lights than this shades the
// remainder of a reflection unshadowed, which shows up in the comparison
// rather than silently.
inline constexpr std::size_t kMaximumReflectionLights = 64;

// Below this a lobe is narrow enough that one ray resolves it. Above it the
// lobe is wider than the reflection pass can resolve within its budget, and
// the environment stands in. This is a *policy* number and deliberately does
// not live in the material: the plan requires performance policy to be
// separable from material semantics.
struct ReflectionPolicy
{
    float roughnessCutoff{0.65f};
    float maximumDistance{4096.0f};
    // A ray shorter than this cannot leave the surface it started on.
    float minimumDistance{0.0f};
    // Declared and honoured by nothing, on either side.
    //
    // This is stated here because the phase document claimed the oracle
    // honoured it and only the shader traced once, and that was not true:
    // `EvaluateReflection` takes one sample and averages nothing, and no
    // caller loops. A field the contract carries and no evaluation reads is
    // exactly the shape of a declaration that looks implemented.
    //
    // Wiring it is not just a loop. Both sides would draw from
    // `SampleSequence`, which is mirrored and would agree, but each extra
    // sample is another traced ray, and a ray-triangle decision at an edge is
    // not bit-identical between this oracle and the hardware intersector.
    // Averaging N of them multiplies the pixels where the two legitimately
    // disagree, against bounds that were set for one. That is the same wall
    // the per-hit attribute work ran into, and it wants the same thing first:
    // a comparison that can exclude pixels whose hit geometry differs between
    // the two, the way the silhouette comparison already excludes edges.
    std::uint32_t samplesPerPixel{1};
    // Growth added to a cone's spread by the pixel footprint itself, in
    // radians per pixel. Zero would make every mip selection pick mip 0 and
    // alias the moment the reflection is minified.
    float pixelSpreadRadians{0.0f};
};

// A ray cone carries the footprint a ray represents, so a hit can pick a mip
// without derivatives. Hit shaders have no implicit derivatives at all, which
// is why the footprint has to be propagated explicitly.
struct RayCone
{
    float width{};
    float spreadAngle{};
};

struct ReflectionRay
{
    std::array<float, 3> origin{};
    std::array<float, 3> direction{};
    float minimumDistance{};
    float maximumDistance{};
    RayCone cone{};
};

// The surface a reflection ray leaves from.
struct ReflectionSurface
{
    std::array<float, 3> position{};
    // Offsetting uses the geometric normal; a shading normal can point into
    // the surface and would push the origin below it.
    std::array<float, 3> geometricNormal{0.0f, 0.0f, 1.0f};
    std::array<float, 3> shadingNormal{0.0f, 0.0f, 1.0f};
    std::array<float, 3> viewDirection{0.0f, 0.0f, 1.0f};
    std::array<float, 3> baseColor{1.0f, 1.0f, 1.0f};
    float roughness{0.0f};
    float metalness{0.0f};
    bool twoSided{};
};

// Everything a hit needs to choose a mip. Kept separate from the material so
// the same footprint rule serves any textured hit.
struct HitFootprint
{
    // Areas of the hit triangle, in world units squared and UV squared.
    float worldArea{};
    float uvArea{};
    std::uint32_t textureWidth{};
    std::uint32_t textureHeight{};
    std::uint32_t mipCount{1};
};

struct ReflectionResult
{
    std::array<float, 3> radiance{};
    ReflectionSource source{ReflectionSource::Unresolved};
    float hitDistance{};
    // The geometry this reflection found, or zero when it found none. Paired
    // with `source`, it is what a contract compares to decide whether the two
    // intersectors agree about the ray, before comparing what they shaded.
    std::uint32_t hitObjectIndex{};
    std::uint32_t hitPrimitiveIndex{};
};

// History identity. A reflection history is only valid for the epoch that
// produced it; reusing one across a camera cut or a resolution change smears
// the old frame across the new one, which is the ghosting the gate forbids.
struct ReflectionHistoryKey
{
    std::uint64_t cameraEpoch{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t viewId{};

    [[nodiscard]] friend bool operator==(
        const ReflectionHistoryKey&, const ReflectionHistoryKey&) = default;
};

// Fresnel reflectance at normal incidence. Metalness selects between the
// dielectric constant and the authored base colour rather than blending two
// unrelated models, and a partially metallic value interpolates because
// captured materials do author intermediate values.
[[nodiscard]] std::array<float, 3> ComputeF0(
    const std::array<float, 3>& baseColor,
    float metalness) noexcept;

// Schlick, evaluated against the *half-vector* cosine when one exists. The
// caller passes the cosine it actually has; using the surface cosine for a
// rough lobe darkens grazing reflections that should brighten.
[[nodiscard]] std::array<float, 3> FresnelSchlick(
    const std::array<float, 3>& f0,
    float cosine) noexcept;

// Perfect mirror direction. Separated from the sampled form so a zero
// roughness surface is exact rather than the limit of a sampling scheme.
[[nodiscard]] std::array<float, 3> MirrorDirection(
    const std::array<float, 3>& viewDirection,
    const std::array<float, 3>& normal) noexcept;

// Deterministic low-discrepancy pair for a pixel, frame, and sample index.
// The same inputs must produce the same pair on the CPU oracle and the GPU,
// or the fixture would be comparing two different sequences and calling the
// difference error.
[[nodiscard]] std::array<float, 2> SampleSequence(
    std::uint32_t pixelX,
    std::uint32_t pixelY,
    std::uint32_t frameIndex,
    std::uint32_t sampleIndex) noexcept;

// GGX visible-normal sampling. Returns false when the sampled half-vector
// produces a direction below the surface: such a sample is rejected rather
// than clamped, because clamping piles rejected directions onto the horizon
// and draws a bright ring around every rough surface at grazing angles.
[[nodiscard]] bool SampleReflectionDirection(
    const ReflectionSurface& surface,
    const std::array<float, 2>& sample,
    std::array<float, 3>& direction) noexcept;

// A hit on the back of a two-sided surface flips the normal. Without the
// flip the shading normal points away from the ray that found it, every dot
// product goes negative, and the reflection resolves to black on exactly the
// surfaces two-sided rendering exists to support.
[[nodiscard]] std::array<float, 3> OrientHitNormal(
    const std::array<float, 3>& normal,
    const std::array<float, 3>& rayDirection,
    bool twoSided) noexcept;

// Builds the ray, including the offset origin, the bounded distance, and the
// cone the hit will use to pick a mip.
[[nodiscard]] ReflectError BuildReflectionRay(
    const ReflectionSurface& surface,
    const ReflectionPolicy& policy,
    const std::array<float, 2>& sample,
    ReflectionRay& ray) noexcept;

// Cone growth along the ray. Width grows by the spread over the distance
// travelled; the spread itself does not change on a straight segment.
[[nodiscard]] RayCone PropagateCone(
    const RayCone& cone,
    float distance) noexcept;

// The mip a hit should sample, from the cone's width at the hit and the
// triangle's texel density. Returns the unclamped level; the caller clamps to
// the texture's mip count so a level beyond the chain is visible as a clamp
// rather than an out-of-range read.
[[nodiscard]] ReflectError SelectMipLevel(
    const RayCone& coneAtHit,
    const HitFootprint& footprint,
    float& level) noexcept;

// The lobe is wider than one ray can resolve. Policy, not material.
[[nodiscard]] bool TracesReflection(
    const ReflectionSurface& surface,
    const ReflectionPolicy& policy) noexcept;

// What a missed ray resolves to. An interior with no captured environment has
// no sky to fall back on, and saying so is the point: substituting an
// exterior sky indoors is the light leak this rule exists to prevent.
[[nodiscard]] ReflectionSource ResolveMiss(
    const lighting::GpuEnvironmentV1& environment,
    bool probeAvailable) noexcept;

[[nodiscard]] std::array<float, 3> EvaluateMissRadiance(
    const lighting::GpuEnvironmentV1& environment,
    const std::array<float, 3>& direction,
    const std::array<float, 3>& probeRadiance,
    bool probeAvailable) noexcept;

// One occluder triangle for the reflection tracer, carrying what a hit needs
// to be shaded. Positions are camera relative, matching the lights and the
// space the top-level structure is built in.
struct ReflectionTriangle
{
    std::array<float, 3> a{};
    std::array<float, 3> b{};
    std::array<float, 3> c{};
    // The object's own geometric normal, not a per-vertex one: the ray query
    // can recover which geometry it hit but has no vertex attributes bound,
    // so both sides read the same per-object value or they cannot agree.
    std::array<float, 3> normal{0.0f, 0.0f, 1.0f};
    std::array<float, 3> albedo{1.0f, 1.0f, 1.0f};
    std::uint32_t primitiveIndex{};
    // Which drawn geometry this triangle belongs to, carried so a hit can be
    // *identified* and not only shaded.
    //
    // Two intersectors do not agree bit for bit about which side of an edge a
    // ray passed, and the disagreement is invisible while both sides shade a
    // hit to the same value. Any per-hit attribute makes it visible, which is
    // what stopped the interpolated vertex colour, the texture fetch at a hit
    // and the multi-sample reflection from landing. Comparing what each side
    // *hit* lets those pixels be excluded from a radiance comparison by name,
    // rather than by widening a bound until they fit.
    std::uint32_t objectIndex{};
    bool twoSided{};
};

struct ReflectionHit
{
    bool hit{};
    float distance{};
    // The triangle.s own geometry, so a caller can report what was hit.
    std::uint32_t objectIndex{};
    // And which triangle of it. Object identity alone is too coarse: two
    // intersectors that disagree about an edge *within* one object report the
    // same object and different barycentrics, which is exactly the case an
    // interpolated attribute exposes.
    std::uint32_t primitiveIndex{};
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 3> albedo{};
};

// Closest hit, Möller-Trumbore, mirroring the ray query's committed
// intersection. Nearest rather than any-hit: a reflection shows the first
// surface along the ray, and an arbitrary one would show whichever triangle
// the traversal happened to reach first.
[[nodiscard]] ReflectionHit TraceReflection(
    std::span<const ReflectionTriangle> triangles,
    const ReflectionRay& ray) noexcept;

// The complete reflection for one surface: policy, ray, trace, shade or fall
// back, and the Fresnel the surface owes. Mirrored branch for branch by
// `vfReflection` in shaders/phase19/reflection.glsl.
[[nodiscard]] ReflectionResult EvaluateReflection(
    const ReflectionSurface& surface,
    const ReflectionPolicy& policy,
    std::span<const ReflectionTriangle> triangles,
    std::span<const lighting::GpuLightRecordV1> lights,
    const lighting::GpuEnvironmentV1& environment,
    const std::array<float, 2>& sample,
    const std::array<float, 3>& probeRadiance,
    bool probeAvailable) noexcept;

// A history from a different epoch, view, or size is not a history of this
// frame. Rejecting it costs one frame of convergence; accepting it smears the
// previous scene across the new one for as long as the history survives.
[[nodiscard]] bool ResetHistory(
    const ReflectionHistoryKey& previous,
    const ReflectionHistoryKey& current) noexcept;

[[nodiscard]] const char* ToString(ReflectError error) noexcept;
[[nodiscard]] const char* ToString(ReflectionSource source) noexcept;

}
