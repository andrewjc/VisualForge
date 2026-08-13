#include "renderer_host/RendererHealth.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

TEST_CASE("P01_startup_health_defaults_to_inert_off_mode", "[unit][phase01]")
{
    const auto health = vf::renderer::MakeStartupHealth();

    CHECK(health.schemaVersion == 1);
    CHECK(health.mode == vf::renderer::RendererMode::Off);
    CHECK_FALSE(health.backendLoaded);
    CHECK_FALSE(health.renderSuppressionEnabled);
    CHECK(vf::renderer::ToString(health.mode) == std::string{"Off"});
    CHECK(vf::renderer::FormatStartupHealth(health) ==
          "renderer-health schema=1 mode=Off backend=absent suppression=off");
}

TEST_CASE("P01_renderer_mode_names_are_stable", "[unit][phase01]")
{
    struct ModeName
    {
        vf::renderer::RendererMode mode;
        const char* name;
    };

    constexpr std::array expected{
        ModeName{vf::renderer::RendererMode::Disabled, "Disabled"},
        ModeName{vf::renderer::RendererMode::Off, "Off"},
        ModeName{vf::renderer::RendererMode::Observe, "Observe"},
        ModeName{vf::renderer::RendererMode::Mirror, "Mirror"},
        ModeName{vf::renderer::RendererMode::Takeover, "Takeover"},
        ModeName{vf::renderer::RendererMode::Native, "Native"},
    };

    for (const auto& item : expected) {
        CAPTURE(item.name);
        CHECK(vf::renderer::ToString(item.mode) == std::string{item.name});
    }
}

TEST_CASE("P01_startup_health_format_reports_active_fields", "[unit][phase01]")
{
    const vf::renderer::StartupHealth health{
        7,
        vf::renderer::RendererMode::Mirror,
        true,
        true,
    };

    CHECK(vf::renderer::FormatStartupHealth(health) ==
          "renderer-health schema=7 mode=Mirror backend=loaded suppression=on");
}
