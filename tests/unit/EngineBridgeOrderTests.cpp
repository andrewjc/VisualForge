#include "renderer_core/EngineBridgeOrder.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

using namespace vf::renderer;

namespace {

bridge::LayerEntry Layer(
    const bridge::LayerId id,
    const bridge::DrawOwnership ownership = bridge::DrawOwnership::Owned,
    const bool present = true)
{
    bridge::LayerEntry entry{};
    entry.id = id;
    entry.ownership = ownership;
    entry.present = present;
    return entry;
}

}

TEST_CASE("P24_every_draw_after_handoff_is_classified", "[phase24][bridge]")
{
    // A draw nobody owns lands somewhere in the composition by accident, and
    // the accident is reproducible only on the machine that has that
    // middleware installed. Refusing to arm is the only way that failure
    // reaches a developer rather than a user.
    const std::array<bridge::LayerEntry, 4> known{
        Layer(bridge::LayerId::VulkanWorld),
        Layer(bridge::LayerId::Scaleform),
        Layer(bridge::LayerId::RetainedMiddleware,
            bridge::DrawOwnership::Retained),
        Layer(bridge::LayerId::ExternalOverlay,
            bridge::DrawOwnership::Excluded)};
    const auto ledger = bridge::ClassifyLayers(known);
    CHECK(ledger.MayArm());
    CHECK(ledger.owned.size() == 2);
    CHECK(ledger.retained.size() == 1);
    CHECK(ledger.excluded.size() == 1);

    const std::array<bridge::LayerEntry, 2> unknown{
        Layer(bridge::LayerId::VulkanWorld),
        Layer(bridge::LayerId::ImGui, bridge::DrawOwnership::Unclassified)};
    const auto blocked = bridge::ClassifyLayers(unknown);
    CHECK_FALSE(blocked.MayArm());
    CHECK(blocked.unclassified == 1);

    // A retained path must name an owner. One without is indistinguishable
    // from something forgotten, and forgotten paths are found by users.
    bridge::RetainedContract contract{};
    contract.layer = bridge::LayerId::RetainedMiddleware;
    contract.ownerId = 0x2400'0000'0000'0001ull;
    CHECK(bridge::ValidateRetained(contract) == bridge::BridgeError::None);

    bridge::RetainedContract anonymous{};
    anonymous.layer = bridge::LayerId::RetainedMiddleware;
    CHECK(bridge::ValidateRetained(anonymous) ==
        bridge::BridgeError::UnclassifiedDraw);
}

TEST_CASE("P24_composition_order_puts_the_world_underneath",
    "[phase24][bridge]")
{
    // Scaleform expects to draw over a finished world, and an external
    // overlay must sit above everything or it cannot be read.
    const std::array<bridge::LayerEntry, 4> good{
        Layer(bridge::LayerId::VulkanWorld),
        Layer(bridge::LayerId::Scaleform),
        Layer(bridge::LayerId::ImGui),
        Layer(bridge::LayerId::ExternalOverlay)};
    CHECK(bridge::ValidateComposition(good) == bridge::BridgeError::None);

    const std::array<bridge::LayerEntry, 2> inverted{
        Layer(bridge::LayerId::Scaleform),
        Layer(bridge::LayerId::VulkanWorld)};
    CHECK(bridge::ValidateComposition(inverted) ==
        bridge::BridgeError::OrderViolation);

    const std::array<bridge::LayerEntry, 2> overlayBuried{
        Layer(bridge::LayerId::ExternalOverlay),
        Layer(bridge::LayerId::ImGui)};
    CHECK(bridge::ValidateComposition(overlayBuried) ==
        bridge::BridgeError::OrderViolation);

    // Selecting the post-UI target would put the world on top of the menu.
    // It is an easy mistake to make by binding whichever target is current.
    CHECK(bridge::SelectHandoffTarget() == bridge::TargetSelection::PreUi);
}

TEST_CASE("P24_video_replaces_the_world_rather_than_layering_with_it",
    "[phase24][bridge]")
{
    // Compositing the world underneath a playing video wastes the frame;
    // compositing over it hides the video. Neither is a blend.
    const std::array<bridge::LayerEntry, 2> playing{
        Layer(bridge::LayerId::VulkanWorld),
        Layer(bridge::LayerId::Bink)};
    CHECK_FALSE(bridge::WorldIsVisible(playing));

    const std::array<bridge::LayerEntry, 2> notPlaying{
        Layer(bridge::LayerId::VulkanWorld),
        Layer(bridge::LayerId::Bink, bridge::DrawOwnership::Owned, false)};
    CHECK(bridge::WorldIsVisible(notPlaying));

    const std::array<bridge::LayerEntry, 2> menuOnly{
        Layer(bridge::LayerId::VulkanWorld),
        Layer(bridge::LayerId::Scaleform)};
    CHECK(bridge::WorldIsVisible(menuOnly));
}

TEST_CASE("P24_a_missing_ticket_yields_a_whole_vanilla_frame",
    "[phase24][bridge]")
{
    // Half a world over a menu is worse than no world at all, and it is the
    // state that gets reported as a crash. There is no partial composition.
    const std::array<bridge::LayerEntry, 2> layers{
        Layer(bridge::LayerId::VulkanWorld),
        Layer(bridge::LayerId::Scaleform)};

    bridge::BridgeTicket acquired{};
    acquired.frameIndex = 9;
    acquired.acquired = true;
    CHECK(bridge::ShouldCompositeWorld(acquired, true, layers));

    bridge::BridgeTicket refused{};
    refused.frameIndex = 9;
    CHECK_FALSE(bridge::ShouldCompositeWorld(refused, true, layers));

    // A frame Vulkan never produced is not composited either, however
    // available the ticket is: compositing a stale image is worse than
    // compositing none, because it looks current.
    CHECK_FALSE(bridge::ShouldCompositeWorld(acquired, false, layers));

    // And with video playing, the world is not composited even when
    // everything else is in order.
    const std::array<bridge::LayerEntry, 2> video{
        Layer(bridge::LayerId::VulkanWorld),
        Layer(bridge::LayerId::Bink)};
    CHECK_FALSE(bridge::ShouldCompositeWorld(acquired, true, video));
}

TEST_CASE("P24_premultiplied_output_and_colour_space_are_not_assumed",
    "[phase24][bridge]")
{
    // Vulkan output is premultiplied. Compositing it as straight alpha
    // darkens every edge of every UI element drawn over it, which reads as a
    // UI problem rather than as a handoff problem and gets investigated in
    // the wrong place.
    const std::array<float, 4> premultiplied{0.5f, 0.25f, 0.0f, 0.5f};
    const auto passed = bridge::PrepareForComposition(premultiplied, true);
    CHECK(passed[0] == Catch::Approx(0.5f));
    CHECK(passed[3] == Catch::Approx(0.5f));

    // A straight-alpha source is converted rather than assumed compatible.
    const std::array<float, 4> straight{1.0f, 0.5f, 0.0f, 0.5f};
    const auto converted = bridge::PrepareForComposition(straight, false);
    CHECK(converted[0] == Catch::Approx(0.5f));
    CHECK(converted[1] == Catch::Approx(0.25f));
    CHECK(converted[3] == Catch::Approx(0.5f));

    // Fully opaque is unchanged either way, which is what keeps the common
    // case exact.
    const std::array<float, 4> opaque{0.3f, 0.6f, 0.9f, 1.0f};
    const auto still = bridge::PrepareForComposition(opaque, false);
    CHECK(still[0] == Catch::Approx(0.3f));
    CHECK(still[2] == Catch::Approx(0.9f));

    // A colour-space mismatch silently accepted shifts every colour in the
    // frame by an amount that looks like a grading change.
    CHECK(bridge::ValidateColorSpace(bridge::ColorSpace::Srgb,
        bridge::ColorSpace::Srgb) == bridge::BridgeError::None);
    CHECK(bridge::ValidateColorSpace(bridge::ColorSpace::LinearScRgb,
        bridge::ColorSpace::Srgb) ==
        bridge::BridgeError::ColorSpaceMismatch);
}

TEST_CASE("P24_letterboxing_preserves_the_field_of_view", "[phase24][bridge]")
{
    // Stretching to fit changes the field of view without anything saying
    // so, and the change is small enough to be mistaken for a camera setting.
    bridge::Viewport viewport{};
    REQUIRE(bridge::ComputeLetterbox(1280, 720, 1280, 720, viewport) ==
        bridge::BridgeError::None);
    CHECK(viewport.x == 0);
    CHECK(viewport.y == 0);
    CHECK(viewport.width == 1280);
    CHECK(viewport.height == 720);

    // A wider target pillarboxes: the height fills and the width centres.
    REQUIRE(bridge::ComputeLetterbox(1280, 720, 2560, 720, viewport) ==
        bridge::BridgeError::None);
    CHECK(viewport.height == 720);
    CHECK(viewport.width == 1280);
    CHECK(viewport.x == 640);
    CHECK(viewport.y == 0);

    // A taller target letterboxes.
    REQUIRE(bridge::ComputeLetterbox(1280, 720, 1280, 1440, viewport) ==
        bridge::BridgeError::None);
    CHECK(viewport.width == 1280);
    CHECK(viewport.height == 720);
    CHECK(viewport.y == 360);

    // The aspect the world was rendered at survives the fit, which is the
    // property the whole rule exists for.
    REQUIRE(bridge::ComputeLetterbox(1600, 900, 1280, 1024, viewport) ==
        bridge::BridgeError::None);
    const auto sourceAspect = 1600.0 / 900.0;
    const auto fittedAspect =
        static_cast<double>(viewport.width) / viewport.height;
    CHECK(fittedAspect == Catch::Approx(sourceAspect).margin(0.01));
    CHECK(viewport.width <= 1280);
    CHECK(viewport.height <= 1024);

    CHECK(bridge::ComputeLetterbox(0, 720, 1280, 720, viewport) ==
        bridge::BridgeError::InvalidExtent);
    CHECK(bridge::ComputeLetterbox(1280, 720, 1280, 0, viewport) ==
        bridge::BridgeError::InvalidExtent);
}
