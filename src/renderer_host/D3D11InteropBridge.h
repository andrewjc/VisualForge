#pragma once

#include "renderer_api/BackendAbi.h"
#include "renderer_host/BackendHost.h"
#include "renderer_host/BridgeState.h"

#include <cstdint>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace vf::renderer {

enum class D3D11BridgeError : std::uint8_t
{
    None,
    InvalidArgument,
    UnsupportedDescription,
    UnsupportedDevice,
    AdapterQueryFailed,
    TextureCreationFailed,
    TextureHandleFailed,
    FenceCreationFailed,
    FenceHandleFailed,
    BackendUnavailable,
    BackendCreateFailed,
    NotReady,
    SlotBusy,
    OutputMismatch,
    D3dSignalFailed,
    BackendSubmitFailed,
    D3dWaitFailed,
    DrainTimeout,
    BackendDestroyFailed,
    DeviceRemoved
};

struct D3D11BridgeResult
{
    D3D11BridgeError error{D3D11BridgeError::None};
    long hresult{};
    BackendHostResult backend{};
    BridgeTicket ticket{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == D3D11BridgeError::None;
    }
};

class D3D11InteropBridge
{
public:
    D3D11InteropBridge();
    ~D3D11InteropBridge();

    D3D11InteropBridge(const D3D11InteropBridge&) = delete;
    D3D11InteropBridge& operator=(const D3D11InteropBridge&) = delete;

    [[nodiscard]] D3D11BridgeResult Create(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        BackendHost& backend,
        std::uint32_t width,
        std::uint32_t height,
        bool validation,
        std::uint64_t epoch = 1) noexcept;
    [[nodiscard]] D3D11BridgeResult SubmitPattern(
        BackendHost& backend,
        ID3D11Texture2D* output,
        std::uint64_t frameIndex) noexcept;
    // Presents pixels the renderer produced instead of the built-in test
    // pattern. `pixels` is tightly packed R8G8B8A8 at the bridge extent and
    // must remain valid for the call.
    [[nodiscard]] D3D11BridgeResult SubmitImage(
        BackendHost& backend,
        ID3D11Texture2D* output,
        std::uint64_t frameIndex,
        const void* pixels,
        std::uint64_t pixelBytes) noexcept;
    [[nodiscard]] D3D11BridgeResult DrainAndDestroy(
        BackendHost& backend,
        std::uint32_t timeoutMilliseconds) noexcept;

    [[nodiscard]] bool Ready() const noexcept;
    [[nodiscard]] std::uint32_t Width() const noexcept;
    [[nodiscard]] std::uint32_t Height() const noexcept;
    [[nodiscard]] std::uint64_t Epoch() const noexcept;
    [[nodiscard]] std::uint64_t CompletedFenceValue() const noexcept;
    [[nodiscard]] std::uint32_t ValidationErrorCount() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
