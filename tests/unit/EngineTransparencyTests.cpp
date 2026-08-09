#include "renderer_core/EngineTransparency.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace vf::renderer;

namespace {

blend::TransparentDrawV1 BaseDraw(
    const std::uint64_t id = 0x2100'0000'0000'0001ull,
    const blend::EffectDomain domain = blend::EffectDomain::GeneralBlended,
    const float depth = 100.0f)
{
    blend::TransparentDrawV1 draw{};
    draw.drawId = id;
    draw.materialId = 0x2100'0000'0000'00F0ull;
    draw.domain = domain;
    draw.blend = blend::BlendMode::StraightAlpha;
    draw.sortDepth = depth;
    draw.depthTest = true;
    draw.depthWrite = false;
    return draw;
}

blend::CompositeSample BaseSample()
{
    blend::CompositeSample sample{};
    sample.source = {1.0f, 0.0f, 0.0f, 0.5f};
    sample.destination = {0.0f, 0.0f, 1.0f};
    sample.sceneDepth = 200.0f;
    sample.fragmentDepth = 100.0f;
    return sample;
}

}

TEST_CASE("P21_straight_and_premultiplied_alpha_are_not_interchangeable",
    "[phase21][blend]")
{
    // Compositing a straight-alpha source as premultiplied darkens every edge
    // twice. The error is invisible in the middle of a sprite and obvious
    // around its rim, which is exactly where it is hardest to attribute to
    // the blend rather than to the art.
    auto sample = BaseSample();
    sample.source = {1.0f, 0.0f, 0.0f, 0.5f};
    sample.destination = {0.0f, 0.0f, 1.0f};

    auto straight = BaseDraw();
    straight.blend = blend::BlendMode::StraightAlpha;
    const auto blended = blend::Composite(straight, sample);
    CHECK(blended[0] == Catch::Approx(0.5f));
    CHECK(blended[2] == Catch::Approx(0.5f));

    // Premultiplied does not scale the source again: it is already scaled.
    auto premultiplied = BaseDraw();
    premultiplied.blend = blend::BlendMode::Premultiplied;
    const auto over = blend::Composite(premultiplied, sample);
    CHECK(over[0] == Catch::Approx(1.0f));
    CHECK(over[2] == Catch::Approx(0.5f));
    CHECK(over[0] > blended[0]);

    // Additive ignores the destination's share entirely, which is what makes
    // fire brighten what is behind it rather than replacing it.
    auto additive = BaseDraw();
    additive.blend = blend::BlendMode::Additive;
    const auto glow = blend::Composite(additive, sample);
    CHECK(glow[0] == Catch::Approx(0.5f));
    CHECK(glow[2] == Catch::Approx(1.0f));

    // Multiply darkens and can never brighten, which is what a scorch mark
    // needs and what an additive decal would get wrong in the opposite
    // direction.
    auto multiply = BaseDraw();
    multiply.blend = blend::BlendMode::Multiply;
    auto grey = sample;
    grey.source = {0.5f, 0.5f, 0.5f, 1.0f};
    grey.destination = {1.0f, 1.0f, 1.0f};
    const auto scorched = blend::Composite(multiply, grey);
    CHECK(scorched[0] == Catch::Approx(0.5f));
    CHECK(scorched[0] <= grey.destination[0]);

    // An opaque draw in a transparent batch replaces rather than blends; it
    // is a classification mistake if it happens, but the composite must be
    // predictable when it does.
    auto opaque = BaseDraw();
    opaque.blend = blend::BlendMode::Opaque;
    const auto replaced = blend::Composite(opaque, sample);
    CHECK(replaced[0] == Catch::Approx(1.0f));
    CHECK(replaced[2] == Catch::Approx(0.0f));
}

TEST_CASE("P21_sorting_is_back_to_front_layered_and_total", "[phase21][blend]")
{
    // Two draws at the same depth must have one deterministic order. Without
    // a total key the frame flickers between them, which reads as a
    // shimmering artefact rather than as a sorting bug and is diagnosed
    // accordingly.
    const auto near = BaseDraw(0x2100'0000'0000'0001ull,
        blend::EffectDomain::GeneralBlended, 10.0f);
    const auto far = BaseDraw(0x2100'0000'0000'0002ull,
        blend::EffectDomain::GeneralBlended, 500.0f);
    CHECK(blend::SortsBefore(blend::MakeSortKey(far),
        blend::MakeSortKey(near)));
    CHECK_FALSE(blend::SortsBefore(blend::MakeSortKey(near),
        blend::MakeSortKey(far)));

    const auto tieA = BaseDraw(0x2100'0000'0000'0003ull,
        blend::EffectDomain::GeneralBlended, 42.0f);
    const auto tieB = BaseDraw(0x2100'0000'0000'0004ull,
        blend::EffectDomain::GeneralBlended, 42.0f);
    CHECK(blend::SortsBefore(blend::MakeSortKey(tieA),
        blend::MakeSortKey(tieB)));
    CHECK_FALSE(blend::SortsBefore(blend::MakeSortKey(tieB),
        blend::MakeSortKey(tieA)));

    // A decal composites before smoke whatever their depths say: they are
    // different kinds of thing, not different distances. Depth alone would
    // put a distant decal behind nearby smoke and it would never appear on
    // the surface it belongs to.
    const auto decal = BaseDraw(0x2100'0000'0000'0005ull,
        blend::EffectDomain::Decal, 5.0f);
    const auto smoke = BaseDraw(0x2100'0000'0000'0006ull,
        blend::EffectDomain::Smoke, 900.0f);
    CHECK(blend::SortsBefore(blend::MakeSortKey(decal),
        blend::MakeSortKey(smoke)));

    // The batch sorts to the same order every time it is sorted.
    const std::array<blend::TransparentDrawV1, 5> batch{
        near, far, tieB, tieA, smoke};
    std::vector<std::uint32_t> first;
    std::vector<std::uint32_t> second;
    REQUIRE(blend::SortDraws(batch, first) ==
        blend::TransparencyError::None);
    REQUIRE(blend::SortDraws(batch, second) ==
        blend::TransparencyError::None);
    CHECK(first == second);
    REQUIRE(first.size() == batch.size());
    for (std::size_t index = 1; index < first.size(); ++index) {
        CHECK_FALSE(blend::SortsBefore(
            blend::MakeSortKey(batch[first[index]]),
            blend::MakeSortKey(batch[first[index - 1]])));
    }
}

TEST_CASE("P21_blended_geometry_tests_depth_and_does_not_write_it",
    "[phase21][blend]")
{
    // Writing depth from a transparent draw occludes the transparent draws
    // behind it, and the whole layer collapses to whichever was drawn first.
    const auto draw = BaseDraw();
    CHECK(draw.depthTest);
    CHECK_FALSE(draw.depthWrite);
    CHECK(blend::ValidateDraw(draw) == blend::TransparencyError::None);

    auto writing = BaseDraw();
    writing.depthWrite = true;
    CHECK(blend::ValidateDraw(writing) ==
        blend::TransparencyError::InvalidDepthRange);

    // An opaque draw may write depth; it is not in the sorted layer.
    auto opaque = BaseDraw();
    opaque.blend = blend::BlendMode::Opaque;
    opaque.depthWrite = true;
    CHECK(blend::ValidateDraw(opaque) == blend::TransparencyError::None);

    auto broken = BaseDraw();
    broken.sortDepth = std::numeric_limits<float>::quiet_NaN();
    CHECK(blend::ValidateDraw(broken) ==
        blend::TransparencyError::NonFiniteSource);

    auto badDissolve = BaseDraw();
    badDissolve.dissolve = 1.5f;
    CHECK(blend::ValidateDraw(badDissolve) ==
        blend::TransparencyError::InvalidBlend);
}

TEST_CASE("P21_soft_particles_fade_instead_of_showing_an_intersection",
    "[phase21][blend]")
{
    // A hard particle shows the line where it cuts through the world. The
    // fade is over view-space distance, so it does not change with the
    // camera's near plane the way a depth-buffer difference would.
    auto soft = BaseDraw(0x2100'0000'0000'0010ull,
        blend::EffectDomain::Smoke, 100.0f);
    soft.softFade = 20.0f;

    // Deep in front of the surface: fully visible.
    CHECK(blend::SoftFade(soft, 200.0f, 100.0f) == Catch::Approx(1.0f));
    // Touching it: gone.
    CHECK(blend::SoftFade(soft, 100.0f, 100.0f) == Catch::Approx(0.0f));
    // Half a fade width away: partly there, and monotonic in between.
    const auto half = blend::SoftFade(soft, 110.0f, 100.0f);
    CHECK(half > 0.0f);
    CHECK(half < 1.0f);
    CHECK(blend::SoftFade(soft, 115.0f, 100.0f) > half);

    // Behind the surface it is occluded, not negatively faded.
    CHECK(blend::SoftFade(soft, 90.0f, 100.0f) == Catch::Approx(0.0f));

    // A hard particle is unaffected, which is what keeps existing effects
    // looking as they did.
    auto hard = soft;
    hard.softFade = 0.0f;
    CHECK(blend::SoftFade(hard, 100.5f, 100.0f) == Catch::Approx(1.0f));
}

TEST_CASE("P21_dissolve_has_a_width_not_just_a_threshold", "[phase21][blend]")
{
    // A threshold alone gives a hard cut with stair-stepped edges. The width
    // is carried separately so an effect can dissolve smoothly without the
    // threshold having to be animated at sub-pixel precision.
    auto draw = BaseDraw();
    draw.dissolve = 0.5f;
    draw.dissolveFalloff = 0.2f;

    CHECK(blend::DissolveCoverage(draw, 0.9f) == Catch::Approx(1.0f));
    CHECK(blend::DissolveCoverage(draw, 0.1f) == Catch::Approx(0.0f));
    const auto edge = blend::DissolveCoverage(draw, 0.5f);
    CHECK(edge > 0.0f);
    CHECK(edge < 1.0f);
    CHECK(blend::DissolveCoverage(draw, 0.55f) > edge);

    // Zero width is the hard cut, still available and still exact.
    auto hard = draw;
    hard.dissolveFalloff = 0.0f;
    CHECK(blend::DissolveCoverage(hard, 0.51f) == Catch::Approx(1.0f));
    CHECK(blend::DissolveCoverage(hard, 0.49f) == Catch::Approx(0.0f));

    // No dissolve authored leaves the effect alone rather than fading it.
    auto none = BaseDraw();
    CHECK(blend::DissolveCoverage(none, 0.0f) == Catch::Approx(1.0f));
}

TEST_CASE("P21_decals_need_a_receiver_and_a_bounded_range", "[phase21][blend]")
{
    // Without the stencil mask a decal lands on the sky and on characters
    // walking past it; without a bounded range it stretches across every
    // surface behind the one it was meant for.
    blend::DecalProjection projection{};
    projection.origin = {0.0f, 0.0f, 10.0f};
    projection.axis = {0.0f, 0.0f, -1.0f};
    projection.range = 8.0f;
    projection.radius = 4.0f;

    auto decal = BaseDraw(0x2100'0000'0000'0020ull,
        blend::EffectDomain::Decal, 10.0f);
    decal.stencilReceiverMask = 0x01;
    decal.stencilReference = 0x01;

    float coverage = 0.0f;
    REQUIRE(blend::ProjectDecal(projection, {0.0f, 0.0f, 4.0f},
        {0.0f, 0.0f, 1.0f}, 0x01, decal, coverage) ==
        blend::TransparencyError::None);
    CHECK(coverage > 0.0f);

    // A surface the engine did not mark as a receiver takes nothing.
    REQUIRE(blend::ProjectDecal(projection, {0.0f, 0.0f, 4.0f},
        {0.0f, 0.0f, 1.0f}, 0x02, decal, coverage) ==
        blend::TransparencyError::None);
    CHECK(coverage == Catch::Approx(0.0f));

    // Beyond the projection range it takes nothing either, however well it
    // lines up.
    REQUIRE(blend::ProjectDecal(projection, {0.0f, 0.0f, -20.0f},
        {0.0f, 0.0f, 1.0f}, 0x01, decal, coverage) ==
        blend::TransparencyError::None);
    CHECK(coverage == Catch::Approx(0.0f));

    // Outside the radius, likewise.
    REQUIRE(blend::ProjectDecal(projection, {90.0f, 0.0f, 4.0f},
        {0.0f, 0.0f, 1.0f}, 0x01, decal, coverage) ==
        blend::TransparencyError::None);
    CHECK(coverage == Catch::Approx(0.0f));

    // A surface facing away from the projection axis is not projected onto:
    // a decal wrapping around to the back of a wall is the artefact this
    // prevents.
    REQUIRE(blend::ProjectDecal(projection, {0.0f, 0.0f, 4.0f},
        {0.0f, 0.0f, -1.0f}, 0x01, decal, coverage) ==
        blend::TransparencyError::None);
    CHECK(coverage == Catch::Approx(0.0f));

    // A degenerate axis has no direction to project along.
    auto degenerate = projection;
    degenerate.axis = {0.0f, 0.0f, 0.0f};
    CHECK(blend::ProjectDecal(degenerate, {0.0f, 0.0f, 4.0f},
        {0.0f, 0.0f, 1.0f}, 0x01, decal, coverage) ==
        blend::TransparencyError::DegenerateAxis);

    auto unbounded = projection;
    unbounded.range = 0.0f;
    CHECK(blend::ProjectDecal(unbounded, {0.0f, 0.0f, 4.0f},
        {0.0f, 0.0f, 1.0f}, 0x01, decal, coverage) ==
        blend::TransparencyError::InvalidProjection);
}

TEST_CASE("P21_refraction_reads_the_prior_target_and_effects_stay_reactive",
    "[phase21][blend]")
{
    // A refracting surface that samples the live target feeds itself its own
    // output, and the result diverges over a few frames into a smear.
    auto refractive = BaseDraw(0x2100'0000'0000'0030ull,
        blend::EffectDomain::Refractive, 50.0f);
    CHECK(blend::RefractionReadsPriorTarget(refractive));
    CHECK_FALSE(blend::RefractionReadsPriorTarget(BaseDraw()));

    // A pixel a transparent effect dominates must be marked, or a temporal
    // resolve treats it as static geometry and smears it across frames.
    auto sample = BaseSample();
    sample.source = {1.0f, 1.0f, 1.0f, 1.0f};
    auto fire = BaseDraw(0x2100'0000'0000'0031ull,
        blend::EffectDomain::Fire, 50.0f);
    fire.blend = blend::BlendMode::Additive;
    CHECK(blend::ReactiveMask(fire, sample) > 0.5f);

    // A nearly transparent draw barely disturbs the pixel and must not be
    // marked, or the mask covers the frame and the resolve stops resolving.
    auto faint = sample;
    faint.source = {1.0f, 1.0f, 1.0f, 0.02f};
    CHECK(blend::ReactiveMask(BaseDraw(), faint) < 0.1f);
}

TEST_CASE("P21_unsupported_effects_go_on_the_ledger_not_into_a_guess",
    "[phase21][blend]")
{
    // A wrong blend is harder to notice than a missing effect and much harder
    // to trace, so an effect this renderer does not model keeps its vanilla
    // path and is recorded by name.
    const std::array<blend::TransparentDrawV1, 4> draws{
        BaseDraw(0x2100'0000'0000'0040ull, blend::EffectDomain::Decal),
        BaseDraw(0x2100'0000'0000'0041ull, blend::EffectDomain::Fire),
        BaseDraw(0x2100'0000'0000'0042ull, blend::EffectDomain::Unsupported),
        BaseDraw(0x2100'0000'0000'0043ull, blend::EffectDomain::Smoke)};

    const auto ledger = blend::ClassifyCoverage(draws);
    CHECK(ledger.covered == 3);
    REQUIRE(ledger.unsupportedDraws.size() == 1);
    CHECK(ledger.unsupportedDraws[0] == 0x2100'0000'0000'0042ull);

    // The ledger names the draw, so a compatibility gap is a list of
    // identities rather than a count nobody can act on.
    const std::array<blend::TransparentDrawV1, 1> allCovered{
        BaseDraw(0x2100'0000'0000'0044ull, blend::EffectDomain::Particle)};
    const auto clean = blend::ClassifyCoverage(allCovered);
    CHECK(clean.covered == 1);
    CHECK(clean.unsupportedDraws.empty());
}

TEST_CASE("P21_the_contributed_alpha_narrows_rather_than_the_colour",
    "[phase21][blend]")
{
    // Soft fade and dissolve both reduce how much of an effect is present,
    // which is a change in coverage. Applying them to colour instead makes an
    // additive effect dim toward its own hue rather than disappear, so a
    // fading fire goes dark red instead of going away.
    blend::TransparentDrawV1 draw{};
    draw.blend = blend::BlendMode::Additive;
    draw.softFade = 4.0f;
    draw.dissolve = 0.5f;
    draw.dissolveFalloff = 0.2f;

    // Fully present: well behind the scene surface and past the dissolve.
    CHECK(blend::TransparentAlpha(draw, 1.0f, 10.0f, 2.0f, 1.0f) ==
        Catch::Approx(1.0f));

    // Touching the surface it passes through contributes nothing, which is
    // what removes the intersection line a hard particle shows.
    CHECK(blend::TransparentAlpha(draw, 1.0f, 2.0f, 2.0f, 1.0f) ==
        Catch::Approx(0.0f));

    // Half a soft-fade distance away contributes half.
    CHECK(blend::TransparentAlpha(draw, 1.0f, 4.0f, 2.0f, 1.0f) ==
        Catch::Approx(0.5f));

    // The dissolve narrows it again, multiplicatively: two independent
    // reasons to be less present do not each get the whole budget.
    CHECK(blend::TransparentAlpha(draw, 1.0f, 10.0f, 2.0f, 0.5f) ==
        Catch::Approx(0.5f));
    CHECK(blend::TransparentAlpha(draw, 1.0f, 4.0f, 2.0f, 0.5f) ==
        Catch::Approx(0.25f));

    // The source's own alpha still bounds it, and is clamped rather than
    // trusted: a value above one would brighten the effect beyond what it
    // authored.
    CHECK(blend::TransparentAlpha(draw, 0.5f, 10.0f, 2.0f, 1.0f) ==
        Catch::Approx(0.5f));
    CHECK(blend::TransparentAlpha(draw, 4.0f, 10.0f, 2.0f, 1.0f) ==
        Catch::Approx(1.0f));
    CHECK(blend::TransparentAlpha(draw, -1.0f, 10.0f, 2.0f, 1.0f) ==
        Catch::Approx(0.0f));

    // An effect that authored neither is unchanged by both.
    blend::TransparentDrawV1 plain{};
    plain.blend = blend::BlendMode::StraightAlpha;
    CHECK(blend::TransparentAlpha(plain, 0.75f, 0.0f, 5.0f, 0.0f) ==
        Catch::Approx(0.75f));
}
