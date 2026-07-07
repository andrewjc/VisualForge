#include "renderer_host/D3D11InteropBridge.h"

#include "renderer_host/BridgeContract.h"

#include <array>
#include <limits>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace vf::renderer {

namespace {

using Microsoft::WRL::ComPtr;

class UniqueHandle
{
public:
    UniqueHandle() = default;
    ~UniqueHandle()
    {
        Reset();
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept
        : value_(other.Release())
    {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }
    void Reset(HANDLE value = nullptr) noexcept
    {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }
    [[nodiscard]] HANDLE Get() const noexcept
    {
        return value_;
    }
    [[nodiscard]] HANDLE* Put() noexcept
    {
        Reset();
        return &value_;
    }
    [[nodiscard]] HANDLE Release() noexcept
    {
        const auto value = value_;
        value_ = nullptr;
        return value;
    }

private:
    HANDLE value_{};
};

bool QueryAdapterLuid(
    ID3D11Device* device,
    abi::AdapterLuid& luid) noexcept
{
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (device == nullptr ||
        FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
        FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&description))) {
        return false;
    }
    luid.lowPart = description.AdapterLuid.LowPart;
    luid.highPart = description.AdapterLuid.HighPart;
    return true;
}

D3D11BridgeResult FromBackendCreate(const BackendHostResult result) noexcept
{
    return D3D11BridgeResult{
        D3D11BridgeError::BackendCreateFailed, 0, result};
}

}

struct D3D11InteropBridge::Impl
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> fence;
    std::array<ComPtr<ID3D11Texture2D>, abi::kBridgeImageCount> images;
    std::array<UniqueHandle, abi::kBridgeImageCount> imageHandles;
    UniqueHandle fenceHandle;
    BridgeExchangeTracker tracker;
    std::array<std::uint64_t, abi::kBridgeImageCount> lastReady{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t validationErrors{};
    bool ready{};

    void Reset() noexcept
    {
        ready = false;
        fenceHandle.Reset();
        for (auto& handle : imageHandles) {
            handle.Reset();
        }
        for (auto& image : images) {
            image.Reset();
        }
        fence.Reset();
        context4.Reset();
        device5.Reset();
        context.Reset();
        device.Reset();
        lastReady = {};
        width = 0;
        height = 0;
    }
};

D3D11InteropBridge::D3D11InteropBridge()
    : impl_(std::make_unique<Impl>())
{}

D3D11InteropBridge::~D3D11InteropBridge() = default;

D3D11BridgeResult D3D11InteropBridge::Create(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    BackendHost& backend,
    const std::uint32_t width,
    const std::uint32_t height,
    const bool validation,
    const std::uint64_t epoch) noexcept
{
    if (device == nullptr || context == nullptr || impl_->ready) {
        return {D3D11BridgeError::InvalidArgument};
    }
    const BridgeImageDescription bridgeDescription{
        width,
        height,
        BridgeFormat::R8G8B8A8Unorm,
        BridgeTransferDestination | BridgeSampled,
        1,
    };
    if (ValidateBridgeDescription(bridgeDescription) !=
        BridgeDescriptionError::None) {
        return {D3D11BridgeError::UnsupportedDescription};
    }
    if (!backend.BridgeAvailable()) {
        return {D3D11BridgeError::BackendUnavailable};
    }

    impl_->device = device;
    impl_->context = context;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&impl_->device5))) ||
        FAILED(context->QueryInterface(IID_PPV_ARGS(&impl_->context4)))) {
        impl_->Reset();
        return {D3D11BridgeError::UnsupportedDevice};
    }

    abi::AdapterLuid adapterLuid{};
    if (!QueryAdapterLuid(device, adapterLuid)) {
        impl_->Reset();
        return {D3D11BridgeError::AdapterQueryFailed};
    }

    D3D11_TEXTURE2D_DESC textureDescription{};
    textureDescription.Width = width;
    textureDescription.Height = height;
    textureDescription.MipLevels = 1;
    textureDescription.ArraySize = 1;
    textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDescription.SampleDesc.Count = 1;
    textureDescription.Usage = D3D11_USAGE_DEFAULT;
    textureDescription.BindFlags =
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    textureDescription.MiscFlags =
        D3D11_RESOURCE_MISC_SHARED |
        D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    for (std::uint32_t index = 0;
         index < abi::kBridgeImageCount;
         ++index) {
        const auto created = device->CreateTexture2D(
            &textureDescription, nullptr, &impl_->images[index]);
        if (FAILED(created)) {
            impl_->Reset();
            return {D3D11BridgeError::TextureCreationFailed, created};
        }
        ComPtr<IDXGIResource1> resource;
        const auto queried = impl_->images[index].As(&resource);
        if (FAILED(queried)) {
            impl_->Reset();
            return {D3D11BridgeError::TextureHandleFailed, queried};
        }
        const auto shared = resource->CreateSharedHandle(
            nullptr,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr,
            impl_->imageHandles[index].Put());
        if (FAILED(shared)) {
            impl_->Reset();
            return {D3D11BridgeError::TextureHandleFailed, shared};
        }
    }

    const auto fenceCreated = impl_->device5->CreateFence(
        0,
        D3D11_FENCE_FLAG_SHARED,
        IID_PPV_ARGS(&impl_->fence));
    if (FAILED(fenceCreated)) {
        impl_->Reset();
        return {D3D11BridgeError::FenceCreationFailed, fenceCreated};
    }
    const auto fenceShared = impl_->fence->CreateSharedHandle(
        nullptr,
        GENERIC_ALL,
        nullptr,
        impl_->fenceHandle.Put());
    if (FAILED(fenceShared)) {
        impl_->Reset();
        return {D3D11BridgeError::FenceHandleFailed, fenceShared};
    }

    abi::BridgeCreateRequestV1 request{};
    request.structSize = sizeof(request);
    request.flags = validation ? abi::BridgeCreateValidation : 0;
    request.adapterLuid = adapterLuid;
    request.epoch = epoch;
    request.width = width;
    request.height = height;
    request.format = abi::BridgeFormat::R8G8B8A8Unorm;
    request.imageCount = abi::kBridgeImageCount;
    request.fenceHandle = reinterpret_cast<std::uint64_t>(
        impl_->fenceHandle.Get());
    for (std::uint32_t index = 0;
         index < abi::kBridgeImageCount;
         ++index) {
        request.imageHandles[index] = reinterpret_cast<std::uint64_t>(
            impl_->imageHandles[index].Get());
    }
    abi::BridgeStatusV1 status{};
    status.structSize = sizeof(status);
    const auto backendResult = backend.CreateBridge(request, status);
    if (!backendResult) {
        impl_->Reset();
        return FromBackendCreate(backendResult);
    }
    impl_->validationErrors = status.validationErrorCount;
    const auto configured = impl_->tracker.Configure(
        epoch, abi::kBridgeImageCount);
    if (configured != BridgeExchangeError::None) {
        status.structSize = sizeof(status);
        static_cast<void>(backend.DestroyBridge(status));
        impl_->Reset();
        return {D3D11BridgeError::InvalidArgument};
    }
    impl_->width = width;
    impl_->height = height;
    impl_->ready = true;
    return {};
}

D3D11BridgeResult D3D11InteropBridge::SubmitPattern(
    BackendHost& backend,
    ID3D11Texture2D* output,
    const std::uint64_t frameIndex) noexcept
{
    return SubmitImage(backend, output, frameIndex, nullptr, 0);
}

D3D11BridgeResult D3D11InteropBridge::SubmitImage(
    BackendHost& backend,
    ID3D11Texture2D* output,
    const std::uint64_t frameIndex,
    const void* const pixels,
    const std::uint64_t pixelBytes) noexcept
{
    if (!impl_->ready) {
        return {D3D11BridgeError::NotReady};
    }
    if (output == nullptr) {
        return {D3D11BridgeError::InvalidArgument};
    }
    D3D11_TEXTURE2D_DESC outputDescription{};
    output->GetDesc(&outputDescription);
    if (outputDescription.Width != impl_->width ||
        outputDescription.Height != impl_->height ||
        outputDescription.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        outputDescription.SampleDesc.Count != 1) {
        return {D3D11BridgeError::OutputMismatch};
    }

    const auto imageIndex = static_cast<std::uint32_t>(
        frameIndex % abi::kBridgeImageCount);
    const auto completed = impl_->fence->GetCompletedValue();
    if (completed == std::numeric_limits<std::uint64_t>::max()) {
        impl_->tracker.ReportDeviceRemoved();
        return {D3D11BridgeError::DeviceRemoved};
    }
    if (impl_->lastReady[imageIndex] != 0 &&
        completed < impl_->lastReady[imageIndex]) {
        return {D3D11BridgeError::SlotBusy};
    }

    const auto begun = impl_->tracker.Begin(
        impl_->tracker.Epoch(), imageIndex);
    if (!begun) {
        return {D3D11BridgeError::SlotBusy};
    }
    const auto signaled = impl_->context4->Signal(
        impl_->fence.Get(), begun.ticket.releaseValue);
    if (FAILED(signaled)) {
        impl_->tracker.ReportDeviceRemoved();
        return {
            D3D11BridgeError::D3dSignalFailed,
            signaled,
            {},
            begun.ticket,
        };
    }

    abi::BridgePatternRequestV1 request{};
    request.structSize = sizeof(request);
    request.imageIndex = imageIndex;
    request.epoch = begun.ticket.epoch;
    request.releaseValue = begun.ticket.releaseValue;
    request.readyValue = begun.ticket.readyValue;
    request.frameIndex = frameIndex;
    // Left zero this is a pattern submission, which is what SubmitPattern
    // asks for and what every existing caller gets.
    request.pixelData = reinterpret_cast<std::uint64_t>(pixels);
    request.pixelSize = pixels != nullptr ? pixelBytes : 0;
    abi::BridgeStatusV1 status{};
    status.structSize = sizeof(status);
    const auto submitted = backend.SubmitBridgePattern(request, status);
    if (!submitted) {
        impl_->tracker.ReportBackendFailure();
        return {
            D3D11BridgeError::BackendSubmitFailed,
            0,
            submitted,
            begun.ticket,
        };
    }
    impl_->validationErrors = status.validationErrorCount;
    if (impl_->validationErrors != 0) {
        impl_->tracker.ReportBackendFailure();
        return {
            D3D11BridgeError::BackendSubmitFailed,
            0,
            {},
            begun.ticket,
        };
    }
    const auto tracked = impl_->tracker.MarkVulkanQueued(begun.ticket);
    if (tracked != BridgeExchangeError::None) {
        impl_->tracker.ReportBackendFailure();
        return {D3D11BridgeError::BackendSubmitFailed};
    }
    const auto waited = impl_->context4->Wait(
        impl_->fence.Get(), begun.ticket.readyValue);
    if (FAILED(waited)) {
        impl_->tracker.ReportDeviceRemoved();
        return {
            D3D11BridgeError::D3dWaitFailed,
            waited,
            {},
            begun.ticket,
        };
    }
    impl_->context->CopyResource(output, impl_->images[imageIndex].Get());
    static_cast<void>(impl_->tracker.CompleteHostAcquire(begun.ticket));
    impl_->lastReady[imageIndex] = begun.ticket.readyValue;
    return {D3D11BridgeError::None, 0, {}, begun.ticket};
}

D3D11BridgeResult D3D11InteropBridge::DrainAndDestroy(
    BackendHost& backend,
    const std::uint32_t timeoutMilliseconds) noexcept
{
    if (!impl_->ready) {
        return {D3D11BridgeError::NotReady};
    }
    const auto last = impl_->tracker.LastIssuedValue();
    if (last == std::numeric_limits<std::uint64_t>::max()) {
        return {D3D11BridgeError::DeviceRemoved};
    }
    const auto drainValue = last + 1;
    const auto signaled = impl_->context4->Signal(
        impl_->fence.Get(), drainValue);
    if (FAILED(signaled)) {
        return {D3D11BridgeError::D3dSignalFailed, signaled};
    }
    impl_->context->Flush();

    UniqueHandle event{ };
    event.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (event.Get() == nullptr) {
        return {
            D3D11BridgeError::D3dWaitFailed,
            static_cast<long>(HRESULT_FROM_WIN32(GetLastError())),
        };
    }
    const auto eventSet = impl_->fence->SetEventOnCompletion(
        drainValue, event.Get());
    if (FAILED(eventSet)) {
        return {D3D11BridgeError::D3dWaitFailed, eventSet};
    }
    if (WaitForSingleObject(event.Get(), timeoutMilliseconds) !=
        WAIT_OBJECT_0) {
        return {D3D11BridgeError::DrainTimeout};
    }

    abi::BridgeStatusV1 status{};
    status.structSize = sizeof(status);
    const auto destroyed = backend.DestroyBridge(status);
    if (!destroyed) {
        return {
            D3D11BridgeError::BackendDestroyFailed,
            0,
            destroyed,
        };
    }
    impl_->validationErrors = status.validationErrorCount;
    impl_->Reset();
    return {};
}

bool D3D11InteropBridge::Ready() const noexcept
{
    return impl_->ready;
}

std::uint32_t D3D11InteropBridge::Width() const noexcept
{
    return impl_->width;
}

std::uint32_t D3D11InteropBridge::Height() const noexcept
{
    return impl_->height;
}

std::uint64_t D3D11InteropBridge::Epoch() const noexcept
{
    return impl_->tracker.Epoch();
}

std::uint64_t D3D11InteropBridge::CompletedFenceValue() const noexcept
{
    return impl_->fence == nullptr ? 0 : impl_->fence->GetCompletedValue();
}

std::uint32_t D3D11InteropBridge::ValidationErrorCount() const noexcept
{
    return impl_->validationErrors;
}

}
