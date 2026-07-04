#include "renderer_host/BackendHost.h"

namespace vf::renderer {

BackendHostResult BackendHost::Load(
    IBackendModule& module,
    const abi::HostCallbacksV1& callbacks) noexcept
{
    if (state_ != BackendHostState::Empty) {
        return BackendHostResult{BackendHostError::AlreadyLoaded};
    }

    const auto callbackContract = ValidateHostCallbacks(callbacks);
    if (callbackContract != BackendContractError::None) {
        state_ = BackendHostState::Faulted;
        return BackendHostResult{
            BackendHostError::InvalidCallbacks,
            abi::Result::InvalidArgument,
            callbackContract,
        };
    }

    const auto opened = module.Open();
    if (opened != BackendModuleOpenResult::Success) {
        state_ = BackendHostState::Faulted;
        switch (opened) {
        case BackendModuleOpenResult::ModuleMissing:
            return BackendHostResult{BackendHostError::ModuleMissing};
        case BackendModuleOpenResult::LoaderUnavailable:
            return BackendHostResult{BackendHostError::LoaderUnavailable};
        case BackendModuleOpenResult::OpenFailed:
            return BackendHostResult{BackendHostError::ModuleOpenFailed};
        case BackendModuleOpenResult::Success:
            break;
        }
    }

    module_ = &module;
    const auto query = module.ResolveQuery();
    if (query == nullptr) {
        module.MarkUnloadDeferred();
        state_ = BackendHostState::Faulted;
        return BackendHostResult{BackendHostError::MissingExport};
    }

    abi::BackendApiV1 candidate{};
    candidate.structSize = sizeof(candidate);
    const auto queryResult = query(
        abi::kBackendAbiMajor,
        abi::kBackendAbiMinor,
        &callbacks,
        &candidate);
    if (queryResult != abi::Result::Success) {
        module.MarkUnloadDeferred();
        state_ = BackendHostState::Faulted;
        return BackendHostResult{
            BackendHostError::QueryFailed,
            queryResult,
            BackendContractError::None,
        };
    }

    const auto apiContract = ValidateBackendApi(candidate);
    if (apiContract != BackendContractError::None) {
        module.MarkUnloadDeferred();
        state_ = BackendHostState::Faulted;
        return BackendHostResult{
            BackendHostError::InvalidApi,
            abi::Result::InvalidArgument,
            apiContract,
        };
    }

    api_ = candidate;
    state_ = BackendHostState::Ready;
    return BackendHostResult{};
}

BackendHostResult BackendHost::Probe(
    const abi::ProbeRequestV1& request,
    abi::CapabilityReportV1& report) noexcept
{
    if (state_ != BackendHostState::Ready) {
        return BackendHostResult{BackendHostError::NotReady};
    }
    if (request.structSize < abi::kProbeRequestV1RequiredSize ||
        report.structSize < abi::kCapabilityReportV1RequiredSize) {
        return BackendHostResult{
            BackendHostError::ProbeFailed,
            abi::Result::InvalidArgument,
            BackendContractError::None,
        };
    }

    const auto result = api_.probe(api_.context, &request, &report);
    if (result != abi::Result::Success) {
        return BackendHostResult{
            BackendHostError::ProbeFailed,
            result,
            BackendContractError::None,
        };
    }
    return BackendHostResult{};
}

bool BackendHost::BridgeAvailable() const noexcept
{
    return state_ == BackendHostState::Ready &&
        api_.abiMinor >= 1 &&
        api_.structSize >= abi::kBackendApiV1BridgeRequiredSize &&
        api_.createBridge != nullptr &&
        api_.submitBridgePattern != nullptr &&
        api_.destroyBridge != nullptr;
}

BackendHostResult BackendHost::CreateBridge(
    const abi::BridgeCreateRequestV1& request,
    abi::BridgeStatusV1& status) noexcept
{
    if (!BridgeAvailable()) {
        return BackendHostResult{BackendHostError::BridgeApiUnavailable};
    }
    if (bridgeCreated_) {
        return BackendHostResult{
            BackendHostError::BridgeAlreadyCreated,
            abi::Result::BridgeAlreadyCreated,
        };
    }
    if (request.structSize < abi::kBridgeCreateRequestV1RequiredSize ||
        status.structSize < abi::kBridgeStatusV1RequiredSize) {
        return BackendHostResult{
            BackendHostError::BridgeCreateFailed,
            abi::Result::InvalidArgument,
        };
    }
    const auto result = api_.createBridge(api_.context, &request, &status);
    if (result != abi::Result::Success) {
        return BackendHostResult{
            BackendHostError::BridgeCreateFailed,
            result,
        };
    }
    bridgeCreated_ = true;
    return BackendHostResult{};
}

BackendHostResult BackendHost::SubmitBridgePattern(
    const abi::BridgePatternRequestV1& request,
    abi::BridgeStatusV1& status) noexcept
{
    if (!BridgeAvailable()) {
        return BackendHostResult{BackendHostError::BridgeApiUnavailable};
    }
    if (!bridgeCreated_) {
        return BackendHostResult{
            BackendHostError::BridgeNotCreated,
            abi::Result::BridgeNotCreated,
        };
    }
    if (request.structSize < abi::kBridgePatternRequestV1RequiredSize ||
        status.structSize < abi::kBridgeStatusV1RequiredSize) {
        return BackendHostResult{
            BackendHostError::BridgeSubmitFailed,
            abi::Result::InvalidArgument,
        };
    }
    const auto result = api_.submitBridgePattern(
        api_.context, &request, &status);
    if (result != abi::Result::Success) {
        return BackendHostResult{
            BackendHostError::BridgeSubmitFailed,
            result,
        };
    }
    return BackendHostResult{};
}

BackendHostResult BackendHost::DestroyBridge(
    abi::BridgeStatusV1& status) noexcept
{
    if (!BridgeAvailable()) {
        return BackendHostResult{BackendHostError::BridgeApiUnavailable};
    }
    if (!bridgeCreated_) {
        return BackendHostResult{
            BackendHostError::BridgeNotCreated,
            abi::Result::BridgeNotCreated,
        };
    }
    if (status.structSize < abi::kBridgeStatusV1RequiredSize) {
        return BackendHostResult{
            BackendHostError::BridgeDestroyFailed,
            abi::Result::InvalidArgument,
        };
    }
    const auto result = api_.destroyBridge(api_.context, &status);
    if (result != abi::Result::Success) {
        return BackendHostResult{
            BackendHostError::BridgeDestroyFailed,
            result,
        };
    }
    bridgeCreated_ = false;
    return BackendHostResult{};
}

bool BackendHost::RasterAvailable() const noexcept
{
    return state_ == BackendHostState::Ready &&
        api_.abiMinor >= abi::kBackendAbiPhase6Minor &&
        api_.structSize >= abi::kBackendApiV1RasterRequiredSize &&
        api_.createRaster != nullptr &&
        api_.renderRasterFrame != nullptr &&
        api_.destroyRaster != nullptr;
}

BackendHostResult BackendHost::CreateRaster(
    const abi::RasterCreateRequestV1& request,
    abi::RasterStatusV1& status) noexcept
{
    if (!RasterAvailable()) {
        return BackendHostResult{BackendHostError::RasterApiUnavailable};
    }
    if (rasterCreated_) {
        return BackendHostResult{
            BackendHostError::RasterAlreadyCreated,
            abi::Result::RasterAlreadyCreated,
        };
    }
    if (request.structSize < abi::kRasterCreateRequestV1RequiredSize ||
        status.structSize < abi::kRasterStatusV1RequiredSize) {
        return BackendHostResult{
            BackendHostError::RasterCreateFailed,
            abi::Result::InvalidArgument,
        };
    }
    const auto result = api_.createRaster(api_.context, &request, &status);
    if (result != abi::Result::Success) {
        return BackendHostResult{
            BackendHostError::RasterCreateFailed, result};
    }
    rasterCreated_ = true;
    return {};
}

BackendHostResult BackendHost::RenderRasterFrame(
    const abi::RasterFrameRequestV1& request,
    abi::RasterStatusV1& status) noexcept
{
    if (!RasterAvailable()) {
        return BackendHostResult{BackendHostError::RasterApiUnavailable};
    }
    if (!rasterCreated_) {
        return BackendHostResult{
            BackendHostError::RasterNotCreated,
            abi::Result::RasterNotCreated,
        };
    }
    if (request.structSize < abi::kRasterFrameRequestV1RequiredSize ||
        status.structSize < abi::kRasterStatusV1RequiredSize) {
        return BackendHostResult{
            BackendHostError::RasterRenderFailed,
            abi::Result::InvalidArgument,
        };
    }
    const auto result = api_.renderRasterFrame(
        api_.context, &request, &status);
    if (result != abi::Result::Success) {
        return BackendHostResult{
            BackendHostError::RasterRenderFailed, result};
    }
    return {};
}

BackendHostResult BackendHost::DestroyRaster(
    abi::RasterStatusV1& status) noexcept
{
    if (!RasterAvailable()) {
        return BackendHostResult{BackendHostError::RasterApiUnavailable};
    }
    if (!rasterCreated_) {
        return BackendHostResult{
            BackendHostError::RasterNotCreated,
            abi::Result::RasterNotCreated,
        };
    }
    if (status.structSize < abi::kRasterStatusV1RequiredSize) {
        return BackendHostResult{
            BackendHostError::RasterDestroyFailed,
            abi::Result::InvalidArgument,
        };
    }
    const auto result = api_.destroyRaster(api_.context, &status);
    if (result != abi::Result::Success) {
        return BackendHostResult{
            BackendHostError::RasterDestroyFailed, result};
    }
    rasterCreated_ = false;
    return {};
}

BackendHostResult BackendHost::RequestShutdown() noexcept
{
    if (state_ == BackendHostState::ShutdownDeferred) {
        return BackendHostResult{BackendHostError::ShutdownDeferred};
    }
    if (state_ != BackendHostState::Ready ||
        module_ == nullptr) {
        return BackendHostResult{BackendHostError::NotReady};
    }

    if (bridgeCreated_ && api_.destroyBridge != nullptr) {
        abi::BridgeStatusV1 status{};
        status.structSize = sizeof(status);
        static_cast<void>(api_.destroyBridge(api_.context, &status));
        bridgeCreated_ = false;
    }
    if (rasterCreated_ && api_.destroyRaster != nullptr) {
        abi::RasterStatusV1 status{};
        status.structSize = sizeof(status);
        static_cast<void>(api_.destroyRaster(api_.context, &status));
        rasterCreated_ = false;
    }
    api_.shutdown(api_.context);
    module_->MarkUnloadDeferred();
    state_ = BackendHostState::ShutdownDeferred;
    return BackendHostResult{BackendHostError::ShutdownDeferred};
}

BackendHostState BackendHost::State() const noexcept
{
    return state_;
}

}
