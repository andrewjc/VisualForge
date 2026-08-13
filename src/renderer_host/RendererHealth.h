#pragma once

#include "renderer_api/RendererMode.h"

#include <cstdint>
#include <string>

namespace vf::renderer {

struct StartupHealth
{
    std::uint32_t schemaVersion{};
    RendererMode mode{RendererMode::Disabled};
    bool backendLoaded{};
    bool renderSuppressionEnabled{};
};

[[nodiscard]] StartupHealth MakeStartupHealth() noexcept;
[[nodiscard]] const char* ToString(RendererMode mode) noexcept;
[[nodiscard]] std::string FormatStartupHealth(const StartupHealth& health);

}
