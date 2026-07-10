#include "renderer_api/BackendAbi.h"
#include "renderer_backend/VulkanInteropBridge.h"
#include "renderer_backend/VulkanProbe.h"
#include "renderer_backend/VulkanRasterRenderer.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace {

using namespace vf::renderer;

struct BackendContext
{
    abi::HostCallbacksV1 callbacks{};
    std::unique_ptr<vf::renderer::backend::VulkanInteropBridge> bridge;
    std::unique_ptr<vf::renderer::backend::VulkanRasterRenderer> raster;
};

BackendContext s_context;

abi::Result Probe(
    void* context,
    const abi::ProbeRequestV1* request,
    abi::CapabilityReportV1* report)
{
    if (context == nullptr || request == nullptr || report == nullptr ||
        request->structSize < abi::kProbeRequestV1RequiredSize ||
        report->structSize < abi::kCapabilityReportV1RequiredSize) {
        return abi::Result::InvalidArgument;
    }
    return vf::renderer::backend::ProbeVulkan(
        static_cast<BackendContext*>(context)->callbacks,
        *request,
        *report);
}

void Shutdown(void* context)
{
    auto* backend = static_cast<BackendContext*>(context);
    if (backend == nullptr) {
        return;
    }
    if (backend->bridge != nullptr) {
        abi::BridgeStatusV1 status{};
        status.structSize = sizeof(status);
        static_cast<void>(backend->bridge->Destroy(status));
        backend->bridge.reset();
    }
    if (backend->raster != nullptr) {
        abi::RasterStatusV1 status{};
        status.structSize = sizeof(status);
        static_cast<void>(backend->raster->Destroy(status));
        backend->raster.reset();
    }
}

abi::Result CreateBridge(
    void* context,
    const abi::BridgeCreateRequestV1* request,
    abi::BridgeStatusV1* status)
{
    auto* backend = static_cast<BackendContext*>(context);
    if (backend == nullptr || request == nullptr || status == nullptr ||
        request->structSize < abi::kBridgeCreateRequestV1RequiredSize ||
        status->structSize < abi::kBridgeStatusV1RequiredSize) {
        return abi::Result::InvalidArgument;
    }
    if (backend->bridge != nullptr) {
        status->result = abi::Result::BridgeAlreadyCreated;
        return abi::Result::BridgeAlreadyCreated;
    }
    try {
        auto bridge = std::make_unique<
            vf::renderer::backend::VulkanInteropBridge>();
        const auto result = bridge->Create(
            backend->callbacks, *request, *status);
        if (result == abi::Result::Success) {
            backend->bridge = std::move(bridge);
        }
        return result;
    } catch (...) {
        status->result = abi::Result::InternalFailure;
        return abi::Result::InternalFailure;
    }
}

abi::Result SubmitBridgePattern(
    void* context,
    const abi::BridgePatternRequestV1* request,
    abi::BridgeStatusV1* status)
{
    auto* backend = static_cast<BackendContext*>(context);
    if (backend == nullptr || request == nullptr || status == nullptr) {
        return abi::Result::InvalidArgument;
    }
    if (backend->bridge == nullptr) {
        status->result = abi::Result::BridgeNotCreated;
        return abi::Result::BridgeNotCreated;
    }
    return backend->bridge->SubmitPattern(*request, *status);
}

abi::Result DestroyBridge(void* context, abi::BridgeStatusV1* status)
{
    auto* backend = static_cast<BackendContext*>(context);
    if (backend == nullptr || status == nullptr) {
        return abi::Result::InvalidArgument;
    }
    if (backend->bridge == nullptr) {
        status->result = abi::Result::BridgeNotCreated;
        return abi::Result::BridgeNotCreated;
    }
    const auto result = backend->bridge->Destroy(*status);
    if (result == abi::Result::Success) {
        backend->bridge.reset();
    }
    return result;
}

abi::Result CreateRaster(
    void* context,
    const abi::RasterCreateRequestV1* request,
    abi::RasterStatusV1* status)
{
    auto* backend = static_cast<BackendContext*>(context);
    if (backend == nullptr || request == nullptr || status == nullptr ||
        request->structSize < abi::kRasterCreateRequestV1RequiredSize ||
        status->structSize < abi::kRasterStatusV1RequiredSize) {
        return abi::Result::InvalidArgument;
    }
    if (backend->raster != nullptr) {
        status->result = abi::Result::RasterAlreadyCreated;
        return abi::Result::RasterAlreadyCreated;
    }
    try {
        auto raster = std::make_unique<
            vf::renderer::backend::VulkanRasterRenderer>();
        const auto result = raster->Create(
            backend->callbacks, *request, *status);
        if (result == abi::Result::Success) {
            backend->raster = std::move(raster);
        }
        return result;
    } catch (...) {
        status->result = abi::Result::InternalFailure;
        return abi::Result::InternalFailure;
    }
}

abi::Result RenderRasterFrame(
    void* context,
    const abi::RasterFrameRequestV1* request,
    abi::RasterStatusV1* status)
{
    auto* backend = static_cast<BackendContext*>(context);
    if (backend == nullptr || request == nullptr || status == nullptr) {
        return abi::Result::InvalidArgument;
    }
    if (backend->raster == nullptr) {
        status->result = abi::Result::RasterNotCreated;
        return abi::Result::RasterNotCreated;
    }
    return backend->raster->Render(*request, *status);
}

abi::Result DestroyRaster(void* context, abi::RasterStatusV1* status)
{
    auto* backend = static_cast<BackendContext*>(context);
    if (backend == nullptr || status == nullptr) {
        return abi::Result::InvalidArgument;
    }
    if (backend->raster == nullptr) {
        status->result = abi::Result::RasterNotCreated;
        return abi::Result::RasterNotCreated;
    }
    const auto result = backend->raster->Destroy(*status);
    if (result == abi::Result::Success) {
        backend->raster.reset();
    }
    return result;
}

}

extern "C" __declspec(dllexport)
vf::renderer::abi::Result VFRenderer_QueryInterface(
    const std::uint32_t hostAbiMajor,
    const std::uint32_t,
    const vf::renderer::abi::HostCallbacksV1* callbacks,
    vf::renderer::abi::BackendApiV1* api)
{
    using namespace vf::renderer;
    if (hostAbiMajor != abi::kBackendAbiMajor) {
        return abi::Result::AbiMajorMismatch;
    }
    if (callbacks == nullptr || api == nullptr ||
        callbacks->structSize < abi::kHostCallbacksV1RequiredSize ||
        callbacks->log == nullptr ||
        api->structSize < abi::kBackendApiV1RequiredSize) {
        return abi::Result::InvalidArgument;
    }

    s_context.callbacks = {};
    std::memcpy(
        &s_context.callbacks,
        callbacks,
        std::min<std::size_t>(
            callbacks->structSize, sizeof(s_context.callbacks)));
    const auto callerSize = api->structSize;
    abi::BackendApiV1 provided{};
    provided.structSize = sizeof(provided);
    provided.abiMajor = abi::kBackendAbiMajor;
    provided.abiMinor = abi::kBackendAbiMinor;
    provided.context = &s_context;
    provided.probe = Probe;
    provided.shutdown = Shutdown;
    provided.createBridge = CreateBridge;
    provided.submitBridgePattern = SubmitBridgePattern;
    provided.destroyBridge = DestroyBridge;
    provided.createRaster = CreateRaster;
    provided.renderRasterFrame = RenderRasterFrame;
    provided.destroyRaster = DestroyRaster;
    std::memcpy(
        api,
        &provided,
        std::min<std::size_t>(callerSize, sizeof(provided)));
    return abi::Result::Success;
}
