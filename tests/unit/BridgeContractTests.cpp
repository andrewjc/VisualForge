#include "renderer_host/BridgeContract.h"

#include <catch2/catch_test_macros.hpp>

using namespace vf::renderer;

TEST_CASE("P05_bridge_format_contract_is_explicit", "[unit][phase05]")
{
    const auto rgba = FindBridgeFormat(BridgeFormat::R8G8B8A8Unorm);
    REQUIRE(rgba.has_value());
    CHECK(rgba->bytesPerPixel == 4);
    CHECK(rgba->ntHandleShareable);
    CHECK((rgba->allowedUsage & BridgeTransferDestination) != 0);
    CHECK((rgba->allowedUsage & BridgeSampled) != 0);

    CHECK_FALSE(FindBridgeFormat(BridgeFormat::Unknown).has_value());
    CHECK_FALSE(FindBridgeFormat(
        BridgeFormat::R16G16B16A16Float).has_value());
}

TEST_CASE("P05_bridge_description_rejects_unsupported_shape_and_usage", "[unit][phase05]")
{
    const BridgeImageDescription valid{
        3440,
        1440,
        BridgeFormat::R8G8B8A8Unorm,
        BridgeTransferDestination | BridgeSampled,
        1,
    };
    CHECK(ValidateBridgeDescription(valid) == BridgeDescriptionError::None);

    auto changed = valid;
    changed.width = 0;
    CHECK(ValidateBridgeDescription(changed) ==
          BridgeDescriptionError::EmptyExtent);
    changed = valid;
    changed.height = 0;
    CHECK(ValidateBridgeDescription(changed) ==
          BridgeDescriptionError::EmptyExtent);
    changed = valid;
    changed.width = 16385;
    CHECK(ValidateBridgeDescription(changed) ==
          BridgeDescriptionError::ExtentTooLarge);
    changed = valid;
    changed.sampleCount = 4;
    CHECK(ValidateBridgeDescription(changed) ==
          BridgeDescriptionError::InvalidSampleCount);
    changed = valid;
    changed.format = BridgeFormat::Unknown;
    CHECK(ValidateBridgeDescription(changed) ==
          BridgeDescriptionError::UnsupportedFormat);
    changed = valid;
    changed.usage |= 1u << 31;
    CHECK(ValidateBridgeDescription(changed) ==
          BridgeDescriptionError::UnsupportedUsage);
}
