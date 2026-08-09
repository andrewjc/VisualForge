#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::blend {

// How a source colour combines with what is already in the target. The
// distinction between straight and premultiplied alpha is not a preference:
// compositing a straight-alpha source as premultiplied darkens every edge
// twice, and the error is invisible in the middle of a sprite and obvious
// around its rim, which is exactly where it is hardest to attribute.
enum class BlendMode : std::uint8_t
{
    Opaque = 0,
    StraightAlpha = 1,
    Premultiplied = 2,
    Additive = 3,
    // The engine's own multiply pass, used by decals that darken rather than
    // cover, such as scorch marks.
    Multiply = 4,
};

// What kind of thing a transparent draw is. The classification decides which
// pass it lands in, and an unclassified draw is not guessed at: it goes on
// the compatibility ledger and keeps its vanilla path.
enum class EffectDomain : std::uint8_t
{
    Decal = 0,
    Blood = 1,
    Fire = 2,
    Smoke = 3,
    Particle = 4,
    GeneralBlended = 5,
    Refractive = 6,
    // Recognised as transparent but not as anything this renderer models.
    Unsupported = 7,
};

enum class TransparencyError : std::uint8_t
{
    None,
    InvalidDomain,
    InvalidBlend,
    NonFiniteSource,
    InvalidDepthRange,
    InvalidProjection,
    DegenerateAxis,
    TooManyDraws,
};

inline constexpr std::uint32_t kMaximumTransparentDraws = 65'536;

// A blended draw as the engine issued it, with everything a compositor needs
// to place it in the right layer at the right time.
struct TransparentDrawV1
{
    std::uint64_t drawId{};
    std::uint64_t materialId{};
    EffectDomain domain{EffectDomain::GeneralBlended};
    BlendMode blend{BlendMode::StraightAlpha};
    // View-space depth of the draw's sort origin. Transparent geometry has no
    // single depth, so the engine's own choice is carried rather than
    // recomputed from bounds that may not match what it sorted by.
    float sortDepth{};
    // Blended geometry tests depth against the opaque scene but must not
    // write it: writing occludes the transparent draws behind it and the
    // layer collapses to whichever happened to be drawn first.
    bool depthTest{true};
    bool depthWrite{};
    // Decals project onto surfaces the engine marked as receivers. Without
    // the mask a decal lands on the sky and on characters walking past it.
    std::uint8_t stencilReceiverMask{};
    std::uint8_t stencilReference{};
    // Softness in view-space units. Zero is a hard particle, which shows the
    // intersection line with whatever it passes through.
    float softFade{};
    // Dissolve threshold and its falloff width, both in [0,1].
    float dissolve{};
    float dissolveFalloff{};
};

// Everything about a pixel the composite needs.
struct CompositeSample
{
    std::array<float, 4> source{};
    std::array<float, 3> destination{};
    // The opaque scene's depth at this pixel, in the same space as sortDepth.
    float sceneDepth{};
    float fragmentDepth{};
};

// A decal's projection frame. It is a box, not a plane: a decal projected
// without a bounded range stretches across every surface behind the one it
// was meant for.
struct DecalProjection
{
    std::array<float, 3> origin{};
    std::array<float, 3> axis{0.0f, 0.0f, -1.0f};
    float range{};
    float radius{};
};

// The sort key a transparent draw is ordered by. Back to front, and *total*:
// two draws at the same depth must have one deterministic order or the frame
// flickers between them from one frame to the next, which reads as a
// shimmering artefact rather than as a sorting bug.
struct SortKey
{
    std::uint32_t layer{};
    float depth{};
    std::uint64_t identity{};

    [[nodiscard]] friend bool operator==(
        const SortKey&, const SortKey&) = default;
};

// Which draws may be composited and which keep their vanilla path. An
// unsupported domain is recorded rather than approximated, because a wrong
// blend is harder to notice than a missing effect and much harder to trace.
struct CompatibilityLedger
{
    std::vector<std::uint64_t> unsupportedDraws;
    std::uint32_t covered{};
};

[[nodiscard]] TransparencyError ValidateDraw(
    const TransparentDrawV1& draw) noexcept;

// Layer first, then depth, then identity. The layer exists because a decal
// must composite before smoke whatever their depths say: they are different
// kinds of thing, not different distances.
[[nodiscard]] SortKey MakeSortKey(const TransparentDrawV1& draw) noexcept;
[[nodiscard]] bool SortsBefore(const SortKey& a, const SortKey& b) noexcept;

// Orders a batch back to front. Stable and total, so the same input always
// produces the same order.
[[nodiscard]] TransparencyError SortDraws(
    std::span<const TransparentDrawV1> draws,
    std::vector<std::uint32_t>& order) noexcept;

// One pixel of one draw composited over what is there. Returns the new
// destination colour.
[[nodiscard]] std::array<float, 3> Composite(
    const TransparentDrawV1& draw,
    const CompositeSample& sample) noexcept;

// Fades a soft particle as it approaches the depth buffer, so it does not
// show a hard line where it intersects the world.
[[nodiscard]] float SoftFade(
    const TransparentDrawV1& draw,
    float sceneDepth,
    float fragmentDepth) noexcept;

// Dissolve coverage in [0,1]. A falloff of zero is a hard cut, which is what
// a threshold alone gives and why the width is carried separately.
// The alpha a transparent fragment actually contributes: its own, narrowed by
// the soft fade and the dissolve. Applied to alpha rather than to colour,
// because both are reductions in how much of the effect is present -- scaling
// colour instead makes an additive effect dim toward its own hue rather than
// disappear, so a fading fire goes dark red instead of going away.
[[nodiscard]] float TransparentAlpha(
    const TransparentDrawV1& draw,
    float sourceAlpha,
    float sceneDepth,
    float fragmentDepth,
    float noise) noexcept;

[[nodiscard]] float DissolveCoverage(
    const TransparentDrawV1& draw,
    float noise) noexcept;

// Whether a decal reaches a surface at all, and how strongly.
[[nodiscard]] TransparencyError ProjectDecal(
    const DecalProjection& projection,
    const std::array<float, 3>& surfacePosition,
    const std::array<float, 3>& surfaceNormal,
    std::uint8_t surfaceStencil,
    const TransparentDrawV1& draw,
    float& coverage) noexcept;

// Refraction reads the scene colour from *before* this draw. Sampling the
// live target instead feeds a refracting surface its own output, and the
// result diverges over a few frames into a smear.
[[nodiscard]] bool RefractionReadsPriorTarget(
    const TransparentDrawV1& draw) noexcept;

// Marks pixels a transparent effect dominates, so a temporal resolve does not
// treat them as static geometry and smear them across frames.
[[nodiscard]] float ReactiveMask(
    const TransparentDrawV1& draw,
    const CompositeSample& sample) noexcept;

[[nodiscard]] CompatibilityLedger ClassifyCoverage(
    std::span<const TransparentDrawV1> draws) noexcept;

[[nodiscard]] const char* ToString(TransparencyError error) noexcept;
[[nodiscard]] const char* ToString(EffectDomain domain) noexcept;

}
