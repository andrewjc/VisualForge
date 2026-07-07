#include "renderer_host/BackendHost.h"
#include "renderer_host/D3D11InteropBridge.h"
#include "renderer_host/WindowsBackendModule.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {

using Microsoft::WRL::ComPtr;
using namespace vf::renderer;

void Log(void*, const std::uint32_t level, const char* message)
{
    std::cout << "backend[" << level << "]: "
              << (message == nullptr ? "" : message) << '\n';
}

bool QueryAdapterLuid(
    ID3D11Device* device,
    abi::AdapterLuid& luid)
{
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
        FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&description))) {
        return false;
    }
    luid.lowPart = description.AdapterLuid.LowPart;
    luid.highPart = description.AdapterLuid.HighPart;
    return true;
}

bool VerifyPattern(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* output,
    ID3D11Texture2D* staging,
    const std::uint64_t frameIndex,
    const std::uint32_t width,
    const std::uint32_t height)
{
    context->CopyResource(staging, output);
    context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(
            staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    const auto pixel = [&mapped](const std::uint32_t x,
                                 const std::uint32_t y) {
        const auto* row = static_cast<const std::uint8_t*>(mapped.pData) +
            static_cast<std::size_t>(y) * mapped.RowPitch;
        std::array<std::uint8_t, 4> value{};
        for (std::size_t channel = 0; channel < value.size(); ++channel) {
            value[channel] = row[x * 4 + channel];
        }
        return value;
    };
    const auto encoded = pixel(0, 0);
    const auto topLeft = pixel(width / 8, height / 8);
    const auto topRight = pixel(width * 7 / 8, height / 8);
    const auto bottomLeft = pixel(width / 8, height * 7 / 8);
    const auto bottomRight = pixel(width * 7 / 8, height * 7 / 8);
    context->Unmap(staging, 0);

    return encoded == std::array<std::uint8_t, 4>{
               static_cast<std::uint8_t>(frameIndex),
               static_cast<std::uint8_t>(frameIndex >> 8),
               static_cast<std::uint8_t>(frameIndex >> 16),
               255} &&
        topLeft == std::array<std::uint8_t, 4>{255, 0, 0, 255} &&
        topRight == std::array<std::uint8_t, 4>{0, 255, 0, 255} &&
        bottomLeft == std::array<std::uint8_t, 4>{0, 0, 255, 255} &&
        bottomRight == std::array<std::uint8_t, 4>{255, 255, 0, 255};
}

bool CreateTargets(
    ID3D11Device* device,
    const std::uint32_t width,
    const std::uint32_t height,
    ComPtr<ID3D11Texture2D>& output,
    ComPtr<ID3D11Texture2D>& staging)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(
            &description, nullptr, &output))) {
        return false;
    }
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(device->CreateTexture2D(
        &description, nullptr, &staging));
}

}

int main(const int argc, const char* const* argv)
{
    if ((argc != 3 && argc != 5) ||
        std::string_view{argv[1]} != "--backend" ||
        (argc == 5 && std::string_view{argv[3]} != "--exchanges")) {
        std::cerr << "usage: vf_bridge_probe --backend <dll> [--exchanges <count>]\n";
        return 2;
    }
    const auto exchanges = argc == 5
        ? static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 10))
        : 64u;
    if (exchanges == 0 || exchanges > 10000) {
        std::cerr << "bridge-probe: exchanges must be 1..10000\n";
        return 2;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel{};
    // Created on a named adapter rather than on the default one. The bridge
    // shares images between D3D and Vulkan, so the device has to be on an
    // adapter Vulkan actually has -- and a DXGI adapter list is not a list of
    // Vulkan devices. Measured on a machine with a headset runtime installed:
    // two adapters report the same name and the same twenty-three gigabytes,
    // one of them mirrored by the runtime and carrying a LUID no Vulkan device
    // has, and it enumerates first. Sharing with that one fails at import,
    // which reads as a bridge fault rather than as the wrong adapter.
    // The backend is loaded before the device, because which adapter the
    // device belongs on is a question only the backend can answer, and it is
    // process-lifetime: loading it once and asking repeatedly is the only way
    // to ask more than once.
    WindowsBackendModule module{argv[2]};
    BackendHost host;
    abi::HostCallbacksV1 callbacks{};
    callbacks.structSize = sizeof(callbacks);
    callbacks.log = Log;
    {
        const auto preloaded = host.Load(module, callbacks);
        if (!preloaded) {
            std::cerr << "bridge-probe: backend load failed host="
                      << static_cast<unsigned>(preloaded.error) << '\n';
            return 4;
        }
    }

    auto deviceCreated = E_FAIL;
    {
        ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                reinterpret_cast<void**>(factory.GetAddressOf())))) {
            ComPtr<IDXGIAdapter1> candidate;
            for (UINT index = 0;
                 factory->EnumAdapters1(index,
                     candidate.ReleaseAndGetAddressOf())
                     != DXGI_ERROR_NOT_FOUND;
                 ++index) {
                DXGI_ADAPTER_DESC1 described{};
                if (FAILED(candidate->GetDesc1(&described)) ||
                    (described.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                    continue;
                }
                // The adapter is only the right one if the backend can see it
                // too. Two entries describe this card and only one of them is
                // a Vulkan device, so the backend is asked rather than
                // guessed at.
                abi::ProbeRequestV1 candidateRequest{};
                candidateRequest.structSize = sizeof(candidateRequest);
                candidateRequest.adapterLuid.lowPart =
                    described.AdapterLuid.LowPart;
                candidateRequest.adapterLuid.highPart =
                    described.AdapterLuid.HighPart;
                candidateRequest.requiredCapabilities =
                    abi::kRequiredCapabilities;
                abi::CapabilityReportV1 candidateReport{};
                candidateReport.structSize = sizeof(candidateReport);
                if (!host.Probe(candidateRequest, candidateReport)) continue;
                // D3D_DRIVER_TYPE_UNKNOWN is required when an adapter is
                // named; passing HARDWARE with a non-null adapter fails.
                deviceCreated = D3D11CreateDevice(candidate.Get(),
                    D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                    D3D11_SDK_VERSION, device.ReleaseAndGetAddressOf(),
                    &featureLevel, context.ReleaseAndGetAddressOf());
                if (SUCCEEDED(deviceCreated)) break;
            }
        }
    }
    if (FAILED(deviceCreated)) {
        std::cerr << "bridge-probe: no adapter is both a D3D device and a "
                     "Vulkan device; hr=0x" << std::hex << deviceCreated
                  << '\n';
        return 3;
    }

    abi::AdapterLuid luid{};
    if (!QueryAdapterLuid(device.Get(), luid)) {
        std::cerr << "bridge-probe: adapter query failed\n";
        return 3;
    }
    // Already loaded above: the adapter search needed it, and the backend is
    // process-lifetime so a second load is refused rather than ignored.
    abi::ProbeRequestV1 probeRequest{};
    probeRequest.structSize = sizeof(probeRequest);
    probeRequest.enableValidation = 1;
    probeRequest.adapterLuid = luid;
    probeRequest.requiredCapabilities = abi::kRequiredCapabilities;
    abi::CapabilityReportV1 capability{};
    capability.structSize = sizeof(capability);
    const auto probed = host.Probe(probeRequest, capability);
    if (!probed || capability.validationErrorCount != 0) {
        std::cerr << "bridge-probe: capability probe failed backend="
                  << static_cast<unsigned>(probed.backendResult) << '\n';
        return 5;
    }

    constexpr std::uint32_t kWidth = 64;
    constexpr std::uint32_t kHeight = 64;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11Texture2D> staging;
    if (!CreateTargets(
            device.Get(), kWidth, kHeight, output, staging)) {
        std::cerr << "bridge-probe: target creation failed\n";
        return 6;
    }

    D3D11InteropBridge bridge;
    const auto created = bridge.Create(
        device.Get(), context.Get(), host, kWidth, kHeight, true, 1);
    if (!created) {
        std::cerr << "bridge-probe: create failed error="
                  << static_cast<unsigned>(created.error)
                  << " backend="
                  << static_cast<unsigned>(created.backend.backendResult)
                  << " hr=0x" << std::hex
                  << static_cast<unsigned long>(created.hresult) << '\n';
        return 7;
    }

    for (std::uint32_t frame = 0; frame < exchanges; ++frame) {
        const auto submitted = bridge.SubmitPattern(
            host, output.Get(), frame);
        if (!submitted) {
            std::cerr << "bridge-probe: submit failed frame=" << frame
                      << " error=" << static_cast<unsigned>(submitted.error)
                      << " backend="
                      << static_cast<unsigned>(submitted.backend.backendResult)
                      << '\n';
            return 8;
        }
        if (!VerifyPattern(
                context.Get(),
                output.Get(),
                staging.Get(),
                frame,
                kWidth,
                kHeight)) {
            std::cerr << "bridge-probe: pattern mismatch frame=" << frame
                      << '\n';
            return 9;
        }
    }
    const auto destroyed = bridge.DrainAndDestroy(host, 10000);
    if (!destroyed) {
        std::cerr << "bridge-probe: drain failed error="
                  << static_cast<unsigned>(destroyed.error) << '\n';
        return 10;
    }
    auto validationErrors = bridge.ValidationErrorCount();
    if (validationErrors != 0) {
        std::cerr << "bridge-probe: validation errors="
                  << validationErrors << '\n';
        return 11;
    }

    constexpr std::uint32_t kResizeCycles = 16;
    for (std::uint32_t cycle = 0; cycle < kResizeCycles; ++cycle) {
        const auto resizedWidth = 64u + (cycle % 4u) * 16u;
        const auto resizedHeight = 48u + (cycle % 3u) * 16u;
        output.Reset();
        staging.Reset();
        if (!CreateTargets(
                device.Get(),
                resizedWidth,
                resizedHeight,
                output,
                staging)) {
            std::cerr << "bridge-probe: resized target creation failed cycle="
                      << cycle << '\n';
            return 12;
        }
        const auto epoch = static_cast<std::uint64_t>(cycle) + 2;
        const auto recreated = bridge.Create(
            device.Get(),
            context.Get(),
            host,
            resizedWidth,
            resizedHeight,
            true,
            epoch);
        if (!recreated) {
            std::cerr << "bridge-probe: resize recreation failed cycle="
                      << cycle << " error="
                      << static_cast<unsigned>(recreated.error) << '\n';
            return 12;
        }
        const auto resizedFrame =
            static_cast<std::uint64_t>(exchanges) + cycle + 1;
        const auto resizedSubmit = bridge.SubmitPattern(
            host, output.Get(), resizedFrame);
        if (!resizedSubmit ||
            !VerifyPattern(
                context.Get(),
                output.Get(),
                staging.Get(),
                resizedFrame,
                resizedWidth,
                resizedHeight)) {
            std::cerr << "bridge-probe: resized exchange failed cycle="
                      << cycle << '\n';
            return 13;
        }
        const auto resizedDestroyed = bridge.DrainAndDestroy(host, 10000);
        if (!resizedDestroyed) {
            std::cerr << "bridge-probe: resized drain failed cycle="
                      << cycle << '\n';
            return 14;
        }
        validationErrors = bridge.ValidationErrorCount();
        if (validationErrors != 0) {
            std::cerr << "bridge-probe: resized validation errors="
                      << validationErrors << " cycle=" << cycle << '\n';
            return 15;
        }
    }

    std::cout << "bridge-probe exchanges=" << exchanges
              << " extent=" << kWidth << 'x' << kHeight
              << " format=R8G8B8A8_UNORM ring=3"
              << " quadrants=pass frame-index=pass"
              << " resize-cycles=" << kResizeCycles
              << " final-epoch=" << (kResizeCycles + 1)
              << " validation-errors=" << validationErrors
              << " handles=closed\n";
    static_cast<void>(host.RequestShutdown());
    return 0;
}
