#pragma once

#include "renderer_host/BuildGate.h"
#include "renderer_host/HookManifest.h"
#include "renderer_host/WindowsBuildProbe.h"

#include <cstdint>
#include <string>

namespace vf::renderer {

enum class WindowsStartupProbeError : std::uint8_t
{
    None,
    ModulePathUnavailable,
    BuildProbeFailed,
    BuildRejected,
    ModuleUnavailable,
    HookManifestRejected
};

struct WindowsStartupProbeResult
{
    WindowsStartupProbeError error{WindowsStartupProbeError::ModulePathUnavailable};
    InstalledBuildProbeResult installedBuild{};
    BuildGateReport buildGate{};
    HookManifestReport hookManifest{
        HookSiteValidation{HookSiteError::EmptyManifest, 0, 0},
        0,
    };
    std::uintptr_t moduleBase{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == WindowsStartupProbeError::None;
    }
};

[[nodiscard]] WindowsStartupProbeResult RunWindowsStartupProbe(
    std::uint32_t runtimeVersion) noexcept;
[[nodiscard]] std::string FormatWindowsStartupProbe(
    const WindowsStartupProbeResult& result);

}
