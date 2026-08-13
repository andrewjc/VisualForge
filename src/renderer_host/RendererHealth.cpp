#include "renderer_host/RendererHealth.h"

namespace vf::renderer {

StartupHealth MakeStartupHealth() noexcept
{
    return StartupHealth{
        1,
        RendererMode::Off,
        false,
        false,
    };
}

const char* ToString(RendererMode mode) noexcept
{
    switch (mode) {
        case RendererMode::Disabled:
            return "Disabled";
        case RendererMode::Off:
            return "Off";
        case RendererMode::Observe:
            return "Observe";
        case RendererMode::Mirror:
            return "Mirror";
        case RendererMode::Takeover:
            return "Takeover";
        case RendererMode::Native:
            return "Native";
    }

    return "Unknown";
}

std::string FormatStartupHealth(const StartupHealth& health)
{
    std::string result{"renderer-health schema="};
    result += std::to_string(health.schemaVersion);
    result += " mode=";
    result += ToString(health.mode);
    result += " backend=";
    result += health.backendLoaded ? "loaded" : "absent";
    result += " suppression=";
    result += health.renderSuppressionEnabled ? "on" : "off";
    return result;
}

}
