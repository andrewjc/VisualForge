#pragma once

#include "renderer_api/BackendAbi.h"
#include "renderer_host/BackendContract.h"

#include <cstdint>

namespace vf::renderer {

enum class BackendModuleOpenResult : std::uint8_t
{
    Success,
    ModuleMissing,
    LoaderUnavailable,
    OpenFailed
};

class IBackendModule
{
public:
    virtual ~IBackendModule() = default;
    [[nodiscard]] virtual BackendModuleOpenResult Open() noexcept = 0;
    [[nodiscard]] virtual abi::QueryInterfaceFunction ResolveQuery() noexcept = 0;
    virtual void MarkUnloadDeferred() noexcept = 0;
};

enum class BackendHostState : std::uint8_t
{
    Empty,
    Ready,
    Faulted,
    ShutdownDeferred
};

enum class BackendHostError : std::uint8_t
{
    None,
    InvalidCallbacks,
    ModuleMissing,
    LoaderUnavailable,
    ModuleOpenFailed,
    MissingExport,
    QueryFailed,
    InvalidApi,
    AlreadyLoaded,
    NotReady,
    ProbeFailed,
    BridgeApiUnavailable,
    BridgeAlreadyCreated,
    BridgeNotCreated,
    BridgeCreateFailed,
    BridgeSubmitFailed,
    BridgeDestroyFailed,
    RasterApiUnavailable,
    RasterAlreadyCreated,
    RasterNotCreated,
    RasterCreateFailed,
    RasterRenderFailed,
    RasterDestroyFailed,
    ShutdownDeferred
};

struct BackendHostResult
{
    BackendHostError error{BackendHostError::None};
    abi::Result backendResult{abi::Result::Success};
    BackendContractError contractError{BackendContractError::None};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == BackendHostError::None;
    }
};

class BackendHost
{
public:
    [[nodiscard]] BackendHostResult Load(
        IBackendModule& module,
        const abi::HostCallbacksV1& callbacks) noexcept;
    [[nodiscard]] BackendHostResult Probe(
        const abi::ProbeRequestV1& request,
        abi::CapabilityReportV1& report) noexcept;
    [[nodiscard]] bool BridgeAvailable() const noexcept;
    [[nodiscard]] BackendHostResult CreateBridge(
        const abi::BridgeCreateRequestV1& request,
        abi::BridgeStatusV1& status) noexcept;
    [[nodiscard]] BackendHostResult SubmitBridgePattern(
        const abi::BridgePatternRequestV1& request,
        abi::BridgeStatusV1& status) noexcept;
    [[nodiscard]] BackendHostResult DestroyBridge(
        abi::BridgeStatusV1& status) noexcept;
    [[nodiscard]] bool RasterAvailable() const noexcept;
    [[nodiscard]] BackendHostResult CreateRaster(
        const abi::RasterCreateRequestV1& request,
        abi::RasterStatusV1& status) noexcept;
    [[nodiscard]] BackendHostResult RenderRasterFrame(
        const abi::RasterFrameRequestV1& request,
        abi::RasterStatusV1& status) noexcept;
    [[nodiscard]] BackendHostResult DestroyRaster(
        abi::RasterStatusV1& status) noexcept;
    [[nodiscard]] BackendHostResult RequestShutdown() noexcept;
    [[nodiscard]] BackendHostState State() const noexcept;

private:
    IBackendModule* module_{};
    abi::BackendApiV1 api_{};
    BackendHostState state_{BackendHostState::Empty};
    bool bridgeCreated_{};
    bool rasterCreated_{};
};

}
