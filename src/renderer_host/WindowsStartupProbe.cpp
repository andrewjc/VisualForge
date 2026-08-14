#include "renderer_host/WindowsStartupProbe.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>

namespace vf::renderer {

namespace {

constexpr std::size_t kMaximumWindowsPath = 32768;

HookManifestReport UnevaluatedManifest() noexcept
{
    return HookManifestReport{
        HookSiteValidation{HookSiteError::EmptyManifest, 0, 0},
        0,
    };
}

WindowsStartupProbeResult PathFailure() noexcept
{
    return WindowsStartupProbeResult{
        WindowsStartupProbeError::ModulePathUnavailable,
        {},
        BuildGateReport{0xFFFFFFFFu},
        UnevaluatedManifest(),
        0,
    };
}

}

WindowsStartupProbeResult RunWindowsStartupProbe(
    const std::uint32_t runtimeVersion) noexcept
{
    try {
        std::array<wchar_t, kMaximumWindowsPath> pathBuffer{};
        const auto pathLength = GetModuleFileNameW(
            nullptr,
            pathBuffer.data(),
            static_cast<DWORD>(pathBuffer.size()));
        if (pathLength == 0 || pathLength >= pathBuffer.size()) {
            return PathFailure();
        }

        const std::filesystem::path executable{
            std::wstring_view{pathBuffer.data(), pathLength}};
        const auto addressLibrary =
            executable.parent_path() /
            "Data/F4SE/Plugins/version-1-11-221-0.bin";
        const auto installed = ProbeInstalledBuild(
            runtimeVersion, executable, addressLibrary);
        if (!installed) {
            return WindowsStartupProbeResult{
                WindowsStartupProbeError::BuildProbeFailed,
                installed,
                BuildGateReport{0xFFFFFFFFu},
                UnevaluatedManifest(),
                0,
            };
        }

        const auto buildGate = ValidateBuild(
            TargetBuild_1_11_221(), installed.fingerprint);
        if (!buildGate.Accepted()) {
            return WindowsStartupProbeResult{
                WindowsStartupProbeError::BuildRejected,
                installed,
                buildGate,
                UnevaluatedManifest(),
                0,
            };
        }

        const auto module = GetModuleHandleW(nullptr);
        if (module == nullptr) {
            return WindowsStartupProbeResult{
                WindowsStartupProbeError::ModuleUnavailable,
                installed,
                buildGate,
                UnevaluatedManifest(),
                0,
            };
        }

        const auto moduleBase = reinterpret_cast<std::uintptr_t>(module);
        const auto image = std::span{
            reinterpret_cast<const std::byte*>(module),
            static_cast<std::size_t>(installed.fingerprint.sizeOfImage)};
        const auto manifest = ValidateHookManifest(
            TargetHookManifest_1_11_221(), image, moduleBase);
        if (!manifest) {
            return WindowsStartupProbeResult{
                WindowsStartupProbeError::HookManifestRejected,
                installed,
                buildGate,
                manifest,
                moduleBase,
            };
        }

        return WindowsStartupProbeResult{
            WindowsStartupProbeError::None,
            installed,
            buildGate,
            manifest,
            moduleBase,
        };
    } catch (...) {
        return PathFailure();
    }
}

std::string FormatWindowsStartupProbe(const WindowsStartupProbeResult& result)
{
    std::string text = "startup-probe: ";
    switch (result.error) {
    case WindowsStartupProbeError::None:
        text += "ready";
        break;
    case WindowsStartupProbeError::ModulePathUnavailable:
        text += "failed reason=module-path";
        break;
    case WindowsStartupProbeError::BuildProbeFailed:
        text += "failed reason=build-probe";
        break;
    case WindowsStartupProbeError::BuildRejected:
        text += "failed reason=build-rejected";
        break;
    case WindowsStartupProbeError::ModuleUnavailable:
        text += "failed reason=module-unavailable";
        break;
    case WindowsStartupProbeError::HookManifestRejected:
        text += "failed reason=hook-manifest";
        break;
    }
    text += " hooks=";
    text += std::to_string(result.hookManifest.validatedCount);
    return text;
}

}
