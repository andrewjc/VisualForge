#pragma once

#include "renderer_core/CameraStateScan.h"

#include <cstddef>
#include <cstdint>

namespace vf::renderer {

// Mapped from the target build in engine_render.md section 7.7.
// `BSGraphics::State` is 0x3C0 bytes and holds the current
// `CameraStateData` (0x250) at 0x160. The camera matrices are then located
// inside that window by consistency rather than by a fixed field offset.
constexpr std::uint32_t kGraphicsStateRva = 0x03D70920u;
constexpr std::uint32_t kGraphicsStateSize = 0x3C0u;
constexpr std::uint32_t kCameraStateDataOffset = 0x160u;
constexpr std::uint32_t kCameraStateDataSize = 0x250u;

enum class GraphicsStateError : std::uint8_t
{
    None,
    NullImageBase,
    WindowOutOfBounds,
};

struct GraphicsStateWindow
{
    std::uintptr_t address{};
    std::size_t size{};
};

[[nodiscard]] GraphicsStateError ResolveCameraStateWindow(
    std::uintptr_t imageBase,
    GraphicsStateWindow& window) noexcept;
// The whole state record, so the camera-state cache array at 0x140 is
// searched as well as the current record at 0x160. A capture that only
// reads the current record cannot tell a first-person camera from the main
// world view.
[[nodiscard]] GraphicsStateError ResolveGraphicsStateWindow(
    std::uintptr_t imageBase,
    GraphicsStateWindow& window) noexcept;
[[nodiscard]] const char* ToString(GraphicsStateError error) noexcept;

}
