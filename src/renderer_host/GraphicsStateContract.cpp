#include "renderer_host/GraphicsStateContract.h"

#include <limits>

namespace vf::renderer {

// The camera window lies inside the mapped state record by construction, so
// a drift in these constants is a build-time failure, not a runtime branch.
static_assert(kCameraStateDataOffset + kCameraStateDataSize <=
    kGraphicsStateSize);

GraphicsStateError ResolveCameraStateWindow(
    const std::uintptr_t imageBase,
    GraphicsStateWindow& window) noexcept
{
    window = {};
    if (imageBase == 0) return GraphicsStateError::NullImageBase;
    constexpr std::uintptr_t span =
        static_cast<std::uintptr_t>(kGraphicsStateRva) + kGraphicsStateSize;
    if (imageBase > std::numeric_limits<std::uintptr_t>::max() - span) {
        return GraphicsStateError::WindowOutOfBounds;
    }
    window.address = imageBase + kGraphicsStateRva + kCameraStateDataOffset;
    window.size = kCameraStateDataSize;
    return GraphicsStateError::None;
}

GraphicsStateError ResolveGraphicsStateWindow(
    const std::uintptr_t imageBase,
    GraphicsStateWindow& window) noexcept
{
    window = {};
    if (imageBase == 0) return GraphicsStateError::NullImageBase;
    constexpr std::uintptr_t span =
        static_cast<std::uintptr_t>(kGraphicsStateRva) + kGraphicsStateSize;
    if (imageBase > std::numeric_limits<std::uintptr_t>::max() - span) {
        return GraphicsStateError::WindowOutOfBounds;
    }
    window.address = imageBase + kGraphicsStateRva;
    window.size = kGraphicsStateSize;
    return GraphicsStateError::None;
}

const char* ToString(const GraphicsStateError error) noexcept
{
    switch (error) {
    case GraphicsStateError::None: return "none";
    case GraphicsStateError::NullImageBase: return "null-image-base";
    case GraphicsStateError::WindowOutOfBounds: return "window-out-of-bounds";
    }
    return "unknown";
}

}
