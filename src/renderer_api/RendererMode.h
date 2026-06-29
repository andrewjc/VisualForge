#pragma once

#include <cstdint>

namespace vf::renderer {

enum class RendererMode : std::uint8_t
{
    Disabled,
    Off,
    Observe,
    Mirror,
    Takeover,
    Native
};

}
