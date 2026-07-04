#include "renderer_host/BackendHost.h"
#include "renderer_host/WindowsBackendModule.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <iomanip>
#include <iostream>
#include <string_view>

namespace {

using namespace vf::renderer;

void Log(void*, const std::uint32_t level, const char* message)
{
    std::cout << "backend[" << level << "]: "
              << (message == nullptr ? "" : message) << '\n';
}

// Every non-software adapter, most memory first. The caller tries them in
// order because only Vulkan can say which of them it actually has: two DXGI
// entries can describe the same card, one of them mirrored by a headset or
// remote-desktop runtime and carrying a LUID no Vulkan device reports.
std::vector<abi::AdapterLuid> HardwareAdapterLuids()
{
    std::vector<abi::AdapterLuid> adapters;
    IDXGIFactory1* factory{};
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
            reinterpret_cast<void**>(&factory)))) {
        return adapters;
    }
    std::vector<std::pair<SIZE_T, abi::AdapterLuid>> ranked;
    IDXGIAdapter1* candidate{};
    for (UINT index = 0;
         factory->EnumAdapters1(index, &candidate) != DXGI_ERROR_NOT_FOUND;
         ++index) {
        DXGI_ADAPTER_DESC1 described{};
        if (SUCCEEDED(candidate->GetDesc1(&described)) &&
            (described.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            abi::AdapterLuid luid{};
            luid.lowPart = described.AdapterLuid.LowPart;
            luid.highPart = described.AdapterLuid.HighPart;
            ranked.emplace_back(described.DedicatedVideoMemory, luid);
        }
        candidate->Release();
    }
    factory->Release();
    std::stable_sort(ranked.begin(), ranked.end(),
        [](const auto& left, const auto& right) {
            return left.first > right.first;
        });
    for (const auto& entry : ranked) adapters.push_back(entry.second);
    return adapters;
}

bool QueryDefaultAdapterLuid(abi::AdapterLuid& luid)
{
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    D3D_FEATURE_LEVEL selected{};
    const auto result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &device,
        &selected,
        &context);
    if (FAILED(result)) {
        return false;
    }

    IDXGIDevice* dxgiDevice{};
    IDXGIAdapter* adapter{};
    DXGI_ADAPTER_DESC description{};
    const auto queried = SUCCEEDED(device->QueryInterface(
        __uuidof(IDXGIDevice),
        reinterpret_cast<void**>(&dxgiDevice)));
    const auto adapted = queried &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter));
    const auto described = adapted &&
        SUCCEEDED(adapter->GetDesc(&description));
    if (described) {
        luid.lowPart = description.AdapterLuid.LowPart;
        luid.highPart = description.AdapterLuid.HighPart;
    }

    if (adapter != nullptr) {
        adapter->Release();
    }
    if (dxgiDevice != nullptr) {
        dxgiDevice->Release();
    }
    context->Release();
    device->Release();
    return described;
}

const char* CapabilityState(
    const abi::CapabilityReportV1& report,
    const abi::Capability capability)
{
    return (report.supportedCapabilities & capability) != 0 ? "on" : "off";
}

}

int main(const int argc, const char* const* argv)
{
    if ((argc != 3 && argc != 4) ||
        std::string_view{argv[1]} != "--backend" ||
        (argc == 4 && std::string_view{argv[3]} != "--validation")) {
        std::cerr
            << "usage: vf_backend_probe --backend <dll> [--validation]\n";
        return 2;
    }

    // Every hardware adapter, because a DXGI adapter list is not a list of
    // Vulkan devices. Measured on a machine with a headset runtime installed:
    // two adapters report the same name and the same twenty-three gigabytes,
    // one of them mirrored by the runtime and carrying a LUID no Vulkan
    // device has, and it enumerates first -- so the default adapter is the
    // wrong one and nothing visible here distinguishes them. The probe is
    // cheap, so the backend is asked about each in turn rather than guessed
    // at.
    auto candidates = HardwareAdapterLuids();
    if (candidates.empty()) {
        abi::AdapterLuid luid{};
        if (!QueryDefaultAdapterLuid(luid)) {
            std::cerr << "backend-probe: D3D adapter query failed\n";
            return 3;
        }
        candidates.push_back(luid);
    }

    WindowsBackendModule module{argv[2]};
    BackendHost host;
    abi::HostCallbacksV1 callbacks{};
    callbacks.structSize = sizeof(callbacks);
    callbacks.log = Log;
    const auto loaded = host.Load(module, callbacks);
    if (!loaded) {
        std::cerr << "backend-probe: load failed host="
                  << static_cast<unsigned>(loaded.error)
                  << " backend="
                  << static_cast<unsigned>(loaded.backendResult)
                  << " win32=" << module.LastErrorCode() << '\n';
        return 4;
    }

    abi::ProbeRequestV1 request{};
    request.structSize = sizeof(request);
    request.enableValidation = argc == 4 ? 1u : 0u;
    request.requiredCapabilities = abi::kRequiredCapabilities;
    abi::CapabilityReportV1 report{};
    report.structSize = sizeof(report);
    BackendHostResult probed{};
    for (const auto& candidate : candidates) {
        request.adapterLuid = candidate;
        report = abi::CapabilityReportV1{};
        report.structSize = sizeof(report);
        probed = host.Probe(request, report);
        if (probed) break;
    }
    if (!probed) {
        std::cerr << "backend-probe: probe failed host="
                  << static_cast<unsigned>(probed.error)
                  << " backend="
                  << static_cast<unsigned>(probed.backendResult)
                  << " missing=0x" << std::hex
                  << report.missingRequiredCapabilities << std::dec << '\n';
        return 5;
    }

    const auto shutdown = host.RequestShutdown();
    const auto unloadDeferred =
        shutdown.error == BackendHostError::ShutdownDeferred;

    std::cout << "backend-probe device=\"" << report.deviceName
              << "\" driver-name=\"" << report.driverName
              << "\" vendor=0x" << std::hex << report.vendorId
              << " device=0x" << report.deviceId
              << " api=0x" << report.apiVersion
              << " driver-version=0x" << report.driverVersion
              << " capabilities=0x" << report.supportedCapabilities
              << " missing=0x" << report.missingRequiredCapabilities
              << std::dec
              << " required="
              << (report.missingRequiredCapabilities == 0 ? "pass" : "fail")
              << " queue=" << report.queueFamilyIndex
              << " bc=" << CapabilityState(
                    report, abi::Capability::BcTextureFormats)
              << " d3d11-import=" << CapabilityState(
                    report, abi::Capability::D3d11TextureInterop)
              << " d3d12-fence=" << CapabilityState(
                    report, abi::Capability::D3d12FenceInterop)
              << " per-stage-sampled=" << report.maxPerStageSampledImages
              << " set-sampled=" << report.maxDescriptorSetSampledImages
              << " push-constants=" << report.maxPushConstantsSize
              << " ray-recursion=" << report.maxRayRecursionDepth
              << " shader-group=" << report.shaderGroupHandleSize
              << " as-scratch-align="
              << report.accelerationStructureScratchAlignment
              << " validation-errors=" << report.validationErrorCount
              << " unload=" << (unloadDeferred ? "deferred" : "unexpected")
              << '\n';

    return report.validationErrorCount == 0 && unloadDeferred ? 0 : 6;
}
