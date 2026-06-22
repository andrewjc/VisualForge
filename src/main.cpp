// VisualForge — engine tuning overlay, live setting binder, and CAS sharpening
// for Fallout 4. Address-independent: all engine access goes through signature
// scanning of the running executable, no version-specific offsets.

#include "CaptureRequests.h"
#include "Config.h"
#include "CrashLog.h"
#include "D3D11Hook.h"
#include "FlexShim.h"
#include "EngineSettings.h"
#include "EngineCameraCapture.h"
#include "EngineMeshCapture.h"
#include "IntroSkip.h"
#include "Log.h"
#include "RendererObservation.h"
#include "renderer_host/RendererBootstrap.h"
#include "renderer_host/RendererHealth.h"
#include "renderer_host/WindowsStartupProbe.h"

#include <cstdint>

// Minimal type shims so F4SE's PluginAPI.h can be used without the full common library.
typedef uint8_t UInt8;
typedef uint16_t UInt16;
typedef uint32_t UInt32;
typedef uint64_t UInt64;
typedef int8_t SInt8;
typedef int16_t SInt16;
typedef int32_t SInt32;
typedef int64_t SInt64;

#include "f4se/PluginAPI.h"
#include "f4se_common/f4se_version.h"

#include <MinHook.h>

extern "C" {

__declspec(dllexport) F4SEPluginVersionData F4SEPlugin_Version = {
    F4SEPluginVersionData::kVersion,      // dataVersion
    1,                                    // pluginVersion
    "VisualForge",                        // name
    "Andrew & Claude",                    // author
    F4SEPluginVersionData::kAddressIndependence_Signatures,
    F4SEPluginVersionData::kStructureIndependence_NoStructs,
    { RUNTIME_VERSION_1_11_221, 0 },      // compatibleVersions
    0,                                    // seVersionRequired
    0, 0,                                 // reserved bitfields
    {}                                    // reserved
};

__declspec(dllexport) bool F4SEPlugin_Load(const F4SEInterface* f4se)
{
    vf::log::Open();
    const auto startupProbe = vf::renderer::RunWindowsStartupProbe(
        f4se ? f4se->runtimeVersion : 0);
    vf::log::Write(
        "%s",
        vf::renderer::FormatInstalledBuildProbe(
            startupProbe.installedBuild).c_str());
    vf::log::Write(
        "%s",
        vf::renderer::FormatBuildGateReport(
            startupProbe.buildGate).c_str());
    vf::log::Write(
        "hook-manifest: %s validated=%llu failed-site=%u reason=%s",
        startupProbe.hookManifest ? "accepted" : "rejected",
        static_cast<unsigned long long>(
            startupProbe.hookManifest.validatedCount),
        startupProbe.hookManifest.failure.siteId,
        vf::renderer::ToString(
            startupProbe.hookManifest.failure.error));
    vf::log::Write(
        "%s",
        vf::renderer::FormatWindowsStartupProbe(startupProbe).c_str());

    const auto rendererBootstrap = vf::renderer::RunRendererBootstrap(
        startupProbe.buildGate,
        startupProbe.hookManifest);
    vf::log::Write(
        "%s",
        vf::renderer::FormatRendererBootstrap(
            rendererBootstrap).c_str());

    const auto observing = vf::renderer_observation::Configure(
        rendererBootstrap.Validated());
    auto health = vf::renderer::MakeStartupHealth();
    health.mode = observing
        ? vf::renderer::RendererMode::Observe
        : rendererBootstrap.mode;
    const auto rendererHealth = vf::renderer::FormatStartupHealth(health);
    vf::log::Write("%s", rendererHealth.c_str());
    vf::log::Write("VisualForge 1.0 loading (runtime %08X, f4se %08X)",
                   f4se ? f4se->runtimeVersion : 0, f4se ? f4se->f4seVersion : 0);

    // Installed first so a fault anywhere after this point produces a readable report.
    vf::crashlog::Install();

    vf::config::Load();

    // Fallout 4 loads its INI settings before F4SE loads plugins, so the setting objects
    // already hold their real values here. Resolving and blanking the intro movies now —
    // before the main menu is built — is what makes the startup logo actually skip.
    vf::settings::ResolveAll();
    if (vf::config::Get().skipIntroMovies) {
        int n = vf::intro::Apply();
        vf::log::Write("intro: %d movie setting(s) blanked at load", n);
    }

    if (MH_Initialize() != MH_OK) {
        vf::log::Write("MinHook initialization failed");
        return true; // don't take the game down; just stay inert
    }

    // Needs MinHook, so it goes after MH_Initialize.
    vf::crashlog::InstallExitInstrumentation();

    static_cast<void>(vf::engine_mesh_capture::Configure(
        rendererBootstrap.Validated(), startupProbe.moduleBase));
    static_cast<void>(vf::engine_camera_capture::Configure(
        rendererBootstrap.Validated(), startupProbe.moduleBase));

    // Live capture control lets the harness arm a capture at a chosen
    // moment instead of firing on whatever the main menu happens to draw.
    static_cast<void>(vf::capture_requests::Configure());

    // Redirect Flex before the engine can reach it. Import-table patching, so it works
    // whether or not the Flex DLLs have loaded yet.
    if (vf::config::Get().interceptFlex)
        vf::flexshim::Install();

    if (!vf::d3d::Install())
        vf::log::Write("D3D11 hook installation failed — plugin inert");

    return true;
}

}
