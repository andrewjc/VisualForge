#include "renderer_core/EngineSuppression.h"

#include <catch2/catch_test_macros.hpp>

using namespace vf::renderer;

namespace {

// A draw that would be suppressed. Every test spoils exactly one condition, so
// a forwarded draw is attributable to that one condition rather than to
// whatever else the fixture happened to leave unset.
suppression::DrawContext Suppressible()
{
    suppression::DrawContext context{};
    context.permitGrants = true;
    context.writesWorldTarget = true;
    context.worldReproduced = true;
    return context;
}

}

TEST_CASE("P25_world_draw_is_suppressed_only_with_every_condition",
    "[phase25][suppression]")
{
    REQUIRE(suppression::ClassifyDraw(Suppressible()) ==
        suppression::DrawDisposition::Suppressed);

    // No permit: the frame was never armed, so nothing may be dropped. This is
    // the condition that keeps every un-armed frame byte-identical to vanilla.
    auto unarmed = Suppressible();
    unarmed.permitGrants = false;
    CHECK(suppression::ClassifyDraw(unarmed) ==
        suppression::DrawDisposition::Vanilla);

    // Not a world draw. The HUD, the Pip-Boy and every Scaleform surface reach
    // this predicate too, and dropping them would remove the interface from a
    // frame the renderer never claimed to draw.
    auto interface = Suppressible();
    interface.writesWorldTarget = false;
    CHECK(suppression::ClassifyDraw(interface) ==
        suppression::DrawDisposition::Vanilla);

    // Nothing is reproducing the world this frame. Suppressing here removes the
    // world and puts nothing in its place, which is the one outcome worse than
    // rendering it twice.
    auto unmirrored = Suppressible();
    unmirrored.worldReproduced = false;
    CHECK(suppression::ClassifyDraw(unmirrored) ==
        suppression::DrawDisposition::Vanilla);
}

TEST_CASE("P25_suppression_conditions_are_independent",
    "[phase25][suppression]")
{
    // Every one of the eight combinations, so no pair of conditions can stand
    // in for each other. A predicate that ignored one input entirely would
    // agree with this on four rows and disagree on four.
    for (int mask = 0; mask < 8; ++mask) {
        suppression::DrawContext context{};
        context.permitGrants = (mask & 1) != 0;
        context.writesWorldTarget = (mask & 2) != 0;
        context.worldReproduced = (mask & 4) != 0;
        const auto expected = (mask == 7)
            ? suppression::DrawDisposition::Suppressed
            : suppression::DrawDisposition::Vanilla;
        CHECK(suppression::ClassifyDraw(context) == expected);
    }
}

TEST_CASE("P25_suppressed_frame_may_not_present_vanilla",
    "[phase25][suppression]")
{
    // The whole-frame property, restated at the boundary this module owns: a
    // frame that dropped world draws cannot be completed by the renderer whose
    // draws were dropped. Half a vanilla world and half a Vulkan one is not a
    // degraded picture, it is one nobody can diagnose.
    CHECK_FALSE(suppression::PresentationIsWhole(true, false));
    CHECK(suppression::PresentationIsWhole(true, true));
    CHECK(suppression::PresentationIsWhole(false, false));
    CHECK_FALSE(suppression::PresentationIsWhole(false, true));
}
