#include "renderer_host/GraphicsStateContract.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <vector>

using namespace vf::renderer;

TEST_CASE("P10L_graphics_state_contract_pins_the_mapped_layout",
    "[live][hook]")
{
    // These come from the mapped build in engine_render.md section 7.7. If a
    // future build moves them, the capture must fail closed rather than read
    // an arbitrary address.
    CHECK(kGraphicsStateRva == 0x03D70920u);
    CHECK(kGraphicsStateSize == 0x3C0u);
    CHECK(kCameraStateDataOffset == 0x160u);
    CHECK(kCameraStateDataSize == 0x250u);
    CHECK(kCameraStateDataOffset + kCameraStateDataSize <= kGraphicsStateSize);
}

TEST_CASE("P10L_graphics_state_window_is_bounded_and_checked",
    "[live][hook]")
{
    constexpr std::uintptr_t base = 0x140000000ull;
    GraphicsStateWindow window{};
    REQUIRE(ResolveCameraStateWindow(base, window) ==
        GraphicsStateError::None);
    CHECK(window.address == base + kGraphicsStateRva + kCameraStateDataOffset);
    CHECK(window.size == kCameraStateDataSize);

    CHECK(ResolveCameraStateWindow(0, window) ==
        GraphicsStateError::NullImageBase);
}

TEST_CASE("P10L_graphics_state_rejects_blocks_that_hold_no_camera",
    "[live][hook]")
{
    // A zeroed block is the common failure when the singleton is not yet
    // populated, and it must never be reported as a camera.
    const std::vector<std::byte> zeroed(kCameraStateDataSize, std::byte{0});
    CHECK_FALSE(camera::ScanCameraState(zeroed).found);
}
