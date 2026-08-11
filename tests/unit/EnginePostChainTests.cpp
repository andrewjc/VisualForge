#include "renderer_core/EnginePostChain.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace vf::renderer;

namespace {

post::EffectEntry Entry(
    const post::EffectId id,
    const post::Coverage coverage = post::Coverage::Covered,
    const bool enabled = true)
{
    post::EffectEntry entry{};
    entry.id = id;
    entry.coverage = coverage;
    entry.enabled = enabled;
    return entry;
}

post::TransientImage Image(
    const std::uint64_t id,
    const std::uint32_t first,
    const std::uint32_t last,
    const bool borrowed = false)
{
    post::TransientImage image{};
    image.resourceId = id;
    image.firstPass = first;
    image.lastPass = last;
    image.width = 1280;
    image.height = 720;
    image.borrowed = borrowed;
    return image;
}

}

TEST_CASE("P23_an_unknown_effect_prevents_arming_rather_than_disappearing",
    "[phase23][post]")
{
    // An effect that disappears silently leaves a frame that is wrong in a
    // way nobody sees. Refusing to arm is loud, and loud is what a
    // compatibility gap needs to be.
    const std::array<post::EffectEntry, 4> known{
        Entry(post::EffectId::Exposure),
        Entry(post::EffectId::Bloom),
        Entry(post::EffectId::ToneMap),
        Entry(post::EffectId::MotionBlur, post::Coverage::Retained)};
    const auto ledger = post::ClassifyChain(known);
    CHECK(ledger.MayArm());
    CHECK(ledger.covered.size() == 3);
    CHECK(ledger.retained.size() == 1);
    CHECK(ledger.unsupported.empty());

    const std::array<post::EffectEntry, 2> withUnknown{
        Entry(post::EffectId::Exposure),
        Entry(post::EffectId::Volumetrics, post::Coverage::Unknown)};
    const auto blocked = post::ClassifyChain(withUnknown);
    CHECK_FALSE(blocked.MayArm());
    CHECK(blocked.unknown == 1);

    // An explicitly unsupported effect is a decision, not a gap, so it does
    // not block: the difference between "we know and chose not to" and "we
    // have no idea" is the whole point of keeping them apart.
    const std::array<post::EffectEntry, 2> withUnsupported{
        Entry(post::EffectId::Exposure),
        Entry(post::EffectId::Volumetrics, post::Coverage::Unsupported)};
    const auto allowed = post::ClassifyChain(withUnsupported);
    CHECK(allowed.MayArm());
    REQUIRE(allowed.unsupported.size() == 1);
    CHECK(allowed.unsupported[0] == post::EffectId::Volumetrics);
}

TEST_CASE("P23_the_chain_order_is_checked_not_assumed", "[phase23][post]")
{
    // A grading table authored in display space applied before tone mapping
    // reads an entry that has nothing to do with the colour, and bloom
    // gathered after tone mapping gathers from values already compressed.
    // Both are subtle enough to be mistaken for a shader bug.
    const std::array<post::EffectEntry, 4> good{
        Entry(post::EffectId::Exposure),
        Entry(post::EffectId::Bloom),
        Entry(post::EffectId::ToneMap),
        Entry(post::EffectId::ColorGradingLut)};
    CHECK(post::ValidateOrder(good) == post::PostError::None);

    const std::array<post::EffectEntry, 2> lutFirst{
        Entry(post::EffectId::ColorGradingLut),
        Entry(post::EffectId::ToneMap)};
    CHECK(post::ValidateOrder(lutFirst) == post::PostError::OrderViolation);

    const std::array<post::EffectEntry, 2> bloomLate{
        Entry(post::EffectId::ToneMap),
        Entry(post::EffectId::Bloom)};
    CHECK(post::ValidateOrder(bloomLate) == post::PostError::OrderViolation);

    // A disabled effect still holds its place: enabling it must not reorder
    // the chain, or a settings change silently becomes a rendering change.
    const std::array<post::EffectEntry, 3> withDisabled{
        Entry(post::EffectId::Bloom, post::Coverage::Covered, false),
        Entry(post::EffectId::ToneMap),
        Entry(post::EffectId::ColorGradingLut)};
    CHECK(post::ValidateOrder(withDisabled) == post::PostError::None);
}

TEST_CASE("P23_aliasing_requires_disjoint_lifetimes", "[phase23][post]")
{
    // Two transients sharing memory with overlapping live ranges corrupt
    // silently, and the corruption looks like a shader bug in whichever pass
    // reads second.
    const auto early = Image(1, 0, 3);
    const auto late = Image(2, 4, 8);
    const auto overlapping = Image(3, 2, 6);

    CHECK(post::MayAlias(early, late));
    CHECK(post::MayAlias(late, early));
    CHECK_FALSE(post::MayAlias(early, overlapping));
    CHECK_FALSE(post::MayAlias(overlapping, late));

    // Touching at a single pass is still an overlap: the pass that ends one
    // range and the pass that begins the other are the same pass, and both
    // are live inside it.
    const auto adjacent = Image(4, 3, 5);
    CHECK_FALSE(post::MayAlias(early, adjacent));

    const std::array<post::TransientImage, 3> images{early, late,
        overlapping};
    const std::array<std::array<std::uint64_t, 2>, 1> valid{{{1, 2}}};
    CHECK(post::ValidateAliasing(images, valid) == post::PostError::None);

    const std::array<std::array<std::uint64_t, 2>, 1> invalid{{{1, 3}}};
    CHECK(post::ValidateAliasing(images, invalid) ==
        post::PostError::AliasOverlap);

    // A borrowed image never aliases: it belongs to the engine and the engine
    // decides when it is done with it.
    const auto borrowed = Image(5, 20, 21, true);
    CHECK_FALSE(post::MayAlias(borrowed, early));

    // Writing to a borrowed image changes what vanilla draws next, and the
    // damage appears somewhere unrelated.
    CHECK(post::ValidateBorrowedUsage(borrowed, false) ==
        post::PostError::None);
    CHECK(post::ValidateBorrowedUsage(borrowed, true) ==
        post::PostError::BorrowedWrite);
    CHECK(post::ValidateBorrowedUsage(early, true) == post::PostError::None);
}

TEST_CASE("P23_exposure_adapts_asymmetrically_and_resets_on_a_cut",
    "[phase23][post]")
{
    post::ExposureRules rules{};
    rules.brightenRate = 3.0f;
    rules.darkenRate = 1.0f;

    // Nothing established yet: the first frame takes the target outright
    // rather than fading up from an invented starting point.
    const auto first = post::AdaptExposure({}, 2.0f, 0.016f, false, rules);
    CHECK(first.established);
    CHECK(first.value == Catch::Approx(2.0f));

    // Adaptation is asymmetric because eyes are. One rate for both directions
    // reads as a lag in whichever direction is wrong.
    post::ExposureState settled{};
    settled.value = 1.0f;
    settled.established = true;
    const auto brightening =
        post::AdaptExposure(settled, 4.0f, 0.1f, false, rules);
    const auto darkening =
        post::AdaptExposure(settled, 0.25f, 0.1f, false, rules);
    const auto upStep = brightening.value - settled.value;
    const auto downStep = settled.value - darkening.value;
    CHECK(upStep > 0.0f);
    CHECK(downStep > 0.0f);
    CHECK(upStep > downStep);

    // It converges rather than oscillating or overshooting past the target.
    auto state = settled;
    for (int frame = 0; frame < 400; ++frame) {
        state = post::AdaptExposure(state, 4.0f, 0.016f, false, rules);
        CHECK(state.value <= 4.0f + 1.0e-3f);
    }
    CHECK(state.value == Catch::Approx(4.0f).margin(0.05));

    // Adapting across a cut shows the previous scene's brightness for as long
    // as the adaptation takes, which reads as the new scene being wrong.
    const auto afterCut =
        post::AdaptExposure(state, 0.5f, 0.016f, true, rules);
    CHECK(afterCut.value == Catch::Approx(0.5f));

    // Bounded, so a black frame does not drive exposure to infinity and a
    // white one does not drive it to zero.
    const auto clampedLow =
        post::AdaptExposure(settled, 0.0f, 100.0f, false, rules);
    CHECK(clampedLow.value >= rules.minimum);
    const auto clampedHigh =
        post::AdaptExposure(settled, 1000.0f, 100.0f, false, rules);
    CHECK(clampedHigh.value <= rules.maximum);
}

TEST_CASE("P23_bloom_has_a_knee_and_jitter_restarts_on_resize",
    "[phase23][post]")
{
    post::BloomRules rules{};
    rules.threshold = 1.0f;
    rules.knee = 0.5f;

    // A bare threshold makes bloom pop on as a highlight crosses it, which
    // reads as flicker on any moving specular.
    CHECK(post::BloomWeight(rules, 0.2f) == Catch::Approx(0.0f));
    CHECK(post::BloomWeight(rules, 4.0f) == Catch::Approx(1.0f));
    const auto knee = post::BloomWeight(rules, 1.0f);
    CHECK(knee > 0.0f);
    CHECK(knee < 1.0f);
    CHECK(post::BloomWeight(rules, 1.2f) > knee);

    // With no knee it is the hard threshold, still available and still exact.
    post::BloomRules hard{};
    hard.knee = 0.0f;
    CHECK(post::BloomWeight(hard, 1.01f) == Catch::Approx(1.0f));
    CHECK(post::BloomWeight(hard, 0.99f) == Catch::Approx(0.0f));

    // Jitter is deterministic, stays inside the pixel, and covers it rather
    // than clustering at one corner.
    post::JitterState state{};
    state = post::AdvanceJitter(state, 1280, 720);
    auto meanX = 0.0f;
    auto meanY = 0.0f;
    for (int sample = 0; sample < 64; ++sample) {
        const auto offset = post::JitterOffset(state);
        CHECK(offset[0] >= -0.5f);
        CHECK(offset[0] <= 0.5f);
        CHECK(offset[1] >= -0.5f);
        CHECK(offset[1] <= 0.5f);
        meanX += offset[0];
        meanY += offset[1];
        state = post::AdvanceJitter(state, 1280, 720);
    }
    CHECK(std::abs(meanX / 64.0f) < 0.1f);
    CHECK(std::abs(meanY / 64.0f) < 0.1f);

    // A sequence carried across a resize samples positions that no longer
    // exist, so it restarts.
    post::JitterState carried{};
    carried.index = 17;
    carried.width = 1280;
    carried.height = 720;
    const auto resized = post::AdvanceJitter(carried, 1920, 1080);
    CHECK(resized.index == 0);
    CHECK(resized.width == 1920);
    const auto continued = post::AdvanceJitter(carried, 1280, 720);
    CHECK(continued.index == 18);

    CHECK(post::ResizeInvalidatesHistory(1280, 720, 1920, 1080));
    CHECK_FALSE(post::ResizeInvalidatesHistory(1280, 720, 1280, 720));
    // A history with no extent was never established.
    CHECK(post::ResizeInvalidatesHistory(0, 0, 1280, 720));
}

TEST_CASE("P23_motion_vectors_point_to_where_a_pixel_was", "[phase23][post]")
{
    // The convention, stated once and tested. A sign error smears in the
    // opposite direction, which looks like a different defect entirely and
    // gets diagnosed as one.
    const auto motion = post::MotionToPrevious({100.0f, 50.0f},
        {96.0f, 53.0f});
    CHECK(motion[0] == Catch::Approx(-4.0f));
    CHECK(motion[1] == Catch::Approx(3.0f));

    // A pixel that did not move has no motion, exactly.
    const auto still = post::MotionToPrevious({7.0f, 9.0f}, {7.0f, 9.0f});
    CHECK(still[0] == Catch::Approx(0.0f));
    CHECK(still[1] == Catch::Approx(0.0f));

    // Adding the vector to the current position lands on the previous one,
    // which is the property every consumer relies on.
    const std::array<float, 2> current{31.0f, 12.0f};
    const std::array<float, 2> previous{40.0f, 4.0f};
    const auto vector = post::MotionToPrevious(current, previous);
    CHECK(current[0] + vector[0] == Catch::Approx(previous[0]));
    CHECK(current[1] + vector[1] == Catch::Approx(previous[1]));
}

TEST_CASE("P23_tone_map_precedes_grading_and_disabled_is_exact_identity",
    "[phase23][post]")
{
    const std::array<float, 3> hdr{4.0f, 2.0f, 0.5f};

    // Tone mapping compresses into display range and is monotonic: a
    // non-monotonic curve makes a brighter input darker somewhere, which
    // shows as banding that looks like a precision problem.
    const auto mapped = post::ToneMap(hdr, 1.0f);
    for (const auto channel : mapped) {
        CHECK(channel >= 0.0f);
        CHECK(channel <= 1.0f);
    }
    const auto brighter = post::ToneMap({8.0f, 2.0f, 0.5f}, 1.0f);
    CHECK(brighter[0] > mapped[0]);

    // Exposure scales before the curve, so doubling exposure and halving the
    // input is the same image.
    const auto exposed = post::ToneMap({2.0f, 1.0f, 0.25f}, 2.0f);
    CHECK(exposed[0] == Catch::Approx(mapped[0]).margin(1.0e-5));

    // Black stays black through every output transform: a black level that
    // drifts is visible on every dark scene.
    for (const auto format : {post::OutputFormat::Srgb8,
             post::OutputFormat::Rec2020Pq,
             post::OutputFormat::ScRgbLinear}) {
        const auto black = post::ApplyOutputTransform({0.0f, 0.0f, 0.0f},
            format);
        CHECK(black[0] == Catch::Approx(0.0f).margin(1.0e-6));
    }

    // The linear output transform is exactly the identity, so a display path
    // that needs no conversion performs none.
    const std::array<float, 3> display{0.25f, 0.5f, 0.75f};
    const auto linear = post::ApplyOutputTransform(display,
        post::OutputFormat::ScRgbLinear);
    CHECK(linear[0] == display[0]);
    CHECK(linear[1] == display[1]);
    CHECK(linear[2] == display[2]);

    // The sRGB transform brightens midtones, which is what an encode does,
    // and is not confused with the identity.
    const auto srgb = post::ApplyOutputTransform(display,
        post::OutputFormat::Srgb8);
    CHECK(srgb[0] > display[0]);

    // A disabled effect must be an *exact* identity. Anything else means
    // turning an effect off still changes the image, and every A/B
    // comparison after that measures two differences at once.
    CHECK(post::IsIdentityWhenDisabled(post::EffectId::Bloom, display,
        display));
    CHECK_FALSE(post::IsIdentityWhenDisabled(post::EffectId::Bloom, display,
        {0.25f, 0.5f, 0.7500001f}));
}

TEST_CASE("P23_bloom_thresholds_a_stated_luminance_not_a_channel")
{
    // Bloom is thresholded against luminance, and which luminance is a rule,
    // not a detail. Thresholding a single channel or an unweighted mean makes
    // a saturated blue highlight bloom at a different level from a green one
    // of the same brightness, which reads as a colour cast appearing only on
    // bright edges.
    SECTION("the weights are Rec. 709 and sum to one")
    {
        // A set that does not sum to one rescales every image that passes
        // through the threshold, so the bloom level would depend on the
        // weights rather than on the picture.
        const auto red = post::Luminance({1.0f, 0.0f, 0.0f});
        const auto green = post::Luminance({0.0f, 1.0f, 0.0f});
        const auto blue = post::Luminance({0.0f, 0.0f, 1.0f});
        CHECK(red == Catch::Approx(0.2126f));
        CHECK(green == Catch::Approx(0.7152f));
        CHECK(blue == Catch::Approx(0.0722f));
        CHECK(red + green + blue == Catch::Approx(1.0f));
        CHECK(post::Luminance({1.0f, 1.0f, 1.0f}) == Catch::Approx(1.0f));
    }

    SECTION("green weighs more than red and red more than blue")
    {
        // The ordering is what makes this a luminance rather than a mean, and
        // it is what a reader would check first.
        CHECK(post::Luminance({0.0f, 1.0f, 0.0f}) >
            post::Luminance({1.0f, 0.0f, 0.0f}));
        CHECK(post::Luminance({1.0f, 0.0f, 0.0f}) >
            post::Luminance({0.0f, 0.0f, 1.0f}));
    }

    SECTION("a non-finite channel yields no luminance rather than a NaN")
    {
        // A NaN reaching the threshold compares false against both ends of
        // the knee and the pixel silently takes whichever branch fell
        // through, so it is stopped here instead.
        const auto notANumber = std::numeric_limits<float>::quiet_NaN();
        CHECK(post::Luminance({notANumber, 1.0f, 1.0f}) == 0.0f);
        CHECK(post::Luminance({
            1.0f, std::numeric_limits<float>::infinity(), 1.0f}) == 0.0f);
    }

    SECTION("it is the value bloom actually thresholds")
    {
        // The two exist together or neither means anything: a luminance
        // nothing thresholds is arithmetic, and a threshold with no stated
        // luminance is ambiguous.
        post::BloomRules rules{};
        rules.threshold = 0.5f;
        rules.knee = 0.0f;
        const std::array<float, 3> bright{0.0f, 1.0f, 0.0f};
        const std::array<float, 3> dim{0.0f, 0.0f, 1.0f};
        CHECK(post::BloomWeight(rules, post::Luminance(bright)) == 1.0f);
        CHECK(post::BloomWeight(rules, post::Luminance(dim)) == 0.0f);
    }
}
