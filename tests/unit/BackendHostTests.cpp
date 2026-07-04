#include "renderer_host/BackendHost.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

using namespace vf::renderer;
using namespace vf::renderer::abi;

struct QueryBehavior
{
    Result queryResult{Result::Success};
    Result probeResult{Result::Success};
    std::uint32_t apiSize{sizeof(BackendApiV1)};
    std::uint32_t apiMajor{kBackendAbiMajor};
    std::uint32_t apiMinor{kBackendAbiMinor};
    bool nullProbe{};
    bool nullShutdown{};
    std::uint32_t probeCalls{};
    std::uint32_t shutdownCalls{};
    Result createBridgeResult{Result::Success};
    Result submitBridgeResult{Result::Success};
    Result destroyBridgeResult{Result::Success};
    std::uint32_t createBridgeCalls{};
    std::uint32_t submitBridgeCalls{};
    std::uint32_t destroyBridgeCalls{};
    Result createRasterResult{Result::Success};
    Result renderRasterResult{Result::Success};
    Result destroyRasterResult{Result::Success};
    std::uint32_t createRasterCalls{};
    std::uint32_t renderRasterCalls{};
    std::uint32_t destroyRasterCalls{};
};

QueryBehavior s_behavior;

void Log(void*, std::uint32_t, const char*)
{}

Result FakeProbe(
    void*,
    const ProbeRequestV1* request,
    CapabilityReportV1* report)
{
    ++s_behavior.probeCalls;
    if (request == nullptr || report == nullptr) {
        return Result::InvalidArgument;
    }
    report->result = s_behavior.probeResult;
    report->vendorId = 0x10DE;
    report->deviceId = 0x2684;
    strcpy_s(
        report->deviceName,
        sizeof(report->deviceName),
        "Fake Vulkan Adapter");
    return s_behavior.probeResult;
}

void FakeShutdown(void*)
{
    ++s_behavior.shutdownCalls;
}

Result FakeCreateBridge(
    void*,
    const BridgeCreateRequestV1*,
    BridgeStatusV1* status)
{
    ++s_behavior.createBridgeCalls;
    status->result = s_behavior.createBridgeResult;
    return s_behavior.createBridgeResult;
}

Result FakeSubmitBridge(
    void*,
    const BridgePatternRequestV1*,
    BridgeStatusV1* status)
{
    ++s_behavior.submitBridgeCalls;
    status->result = s_behavior.submitBridgeResult;
    return s_behavior.submitBridgeResult;
}

Result FakeDestroyBridge(void*, BridgeStatusV1* status)
{
    ++s_behavior.destroyBridgeCalls;
    status->result = s_behavior.destroyBridgeResult;
    return s_behavior.destroyBridgeResult;
}

Result FakeCreateRaster(
    void*, const RasterCreateRequestV1*, RasterStatusV1* status)
{
    ++s_behavior.createRasterCalls;
    status->result = s_behavior.createRasterResult;
    return s_behavior.createRasterResult;
}

Result FakeRenderRaster(
    void*, const RasterFrameRequestV1*, RasterStatusV1* status)
{
    ++s_behavior.renderRasterCalls;
    status->result = s_behavior.renderRasterResult;
    return s_behavior.renderRasterResult;
}

Result FakeDestroyRaster(void*, RasterStatusV1* status)
{
    ++s_behavior.destroyRasterCalls;
    status->result = s_behavior.destroyRasterResult;
    return s_behavior.destroyRasterResult;
}

Result FakeQuery(
    const std::uint32_t,
    const std::uint32_t,
    const HostCallbacksV1*,
    BackendApiV1* api)
{
    if (s_behavior.queryResult != Result::Success) {
        return s_behavior.queryResult;
    }
    api->structSize = s_behavior.apiSize;
    api->abiMajor = s_behavior.apiMajor;
    api->abiMinor = s_behavior.apiMinor;
    api->context = &s_behavior;
    api->probe = s_behavior.nullProbe ? nullptr : FakeProbe;
    api->shutdown = s_behavior.nullShutdown ? nullptr : FakeShutdown;
    api->createBridge = FakeCreateBridge;
    api->submitBridgePattern = FakeSubmitBridge;
    api->destroyBridge = FakeDestroyBridge;
    api->createRaster = FakeCreateRaster;
    api->renderRasterFrame = FakeRenderRaster;
    api->destroyRaster = FakeDestroyRaster;
    return Result::Success;
}

class FakeModule final : public IBackendModule
{
public:
    BackendModuleOpenResult Open() noexcept override
    {
        ++openCalls;
        return openResult;
    }

    QueryInterfaceFunction ResolveQuery() noexcept override
    {
        ++resolveCalls;
        return exportPresent ? FakeQuery : nullptr;
    }

    void MarkUnloadDeferred() noexcept override
    {
        ++deferredCalls;
    }

    BackendModuleOpenResult openResult{BackendModuleOpenResult::Success};
    bool exportPresent{true};
    std::uint32_t openCalls{};
    std::uint32_t resolveCalls{};
    std::uint32_t deferredCalls{};
};

HostCallbacksV1 ValidCallbacks()
{
    HostCallbacksV1 callbacks{};
    callbacks.structSize = sizeof(callbacks);
    callbacks.log = Log;
    return callbacks;
}

}

TEST_CASE("P04_backend_host_reports_missing_loader_module_and_export", "[unit][phase04]")
{
    s_behavior = {};

    SECTION("module missing")
    {
        FakeModule module;
        module.openResult = BackendModuleOpenResult::ModuleMissing;
        BackendHost host;
        const auto result = host.Load(module, ValidCallbacks());
        CHECK(result.error == BackendHostError::ModuleMissing);
        CHECK(host.State() == BackendHostState::Faulted);
        CHECK(module.resolveCalls == 0);
    }

    SECTION("Vulkan loader unavailable")
    {
        FakeModule module;
        module.openResult = BackendModuleOpenResult::LoaderUnavailable;
        BackendHost host;
        const auto result = host.Load(module, ValidCallbacks());
        CHECK(result.error == BackendHostError::LoaderUnavailable);
        CHECK(host.State() == BackendHostState::Faulted);
    }

    SECTION("query export missing")
    {
        FakeModule module;
        module.exportPresent = false;
        BackendHost host;
        const auto result = host.Load(module, ValidCallbacks());
        CHECK(result.error == BackendHostError::MissingExport);
        CHECK(host.State() == BackendHostState::Faulted);
        CHECK(module.deferredCalls == 1);
    }
}

TEST_CASE("P04_backend_host_rejects_query_and_abi_failures", "[unit][phase04]")
{
    s_behavior = {};
    FakeModule module;
    BackendHost host;

    SECTION("query failure")
    {
        s_behavior.queryResult = Result::InternalFailure;
        const auto result = host.Load(module, ValidCallbacks());
        CHECK(result.error == BackendHostError::QueryFailed);
        CHECK(result.backendResult == Result::InternalFailure);
    }

    SECTION("major mismatch")
    {
        ++s_behavior.apiMajor;
        const auto result = host.Load(module, ValidCallbacks());
        CHECK(result.error == BackendHostError::InvalidApi);
        CHECK(result.contractError == BackendContractError::AbiMajorMismatch);
    }

    SECTION("optional minor accepted")
    {
        s_behavior.apiMinor = 42;
        const auto result = host.Load(module, ValidCallbacks());
        CHECK(result);
        CHECK(host.State() == BackendHostState::Ready);
    }

    SECTION("null callback rejected before open")
    {
        auto callbacks = ValidCallbacks();
        callbacks.log = nullptr;
        const auto result = host.Load(module, callbacks);
        CHECK(result.error == BackendHostError::InvalidCallbacks);
        CHECK(module.openCalls == 0);
    }
}

TEST_CASE("P04_backend_host_probes_once_loaded_and_defers_unload", "[unit][phase04]")
{
    s_behavior = {};
    FakeModule module;
    BackendHost host;
    REQUIRE(host.Load(module, ValidCallbacks()));
    CHECK(host.Load(module, ValidCallbacks()).error ==
          BackendHostError::AlreadyLoaded);
    CHECK(module.openCalls == 1);

    ProbeRequestV1 request{};
    request.structSize = sizeof(request);
    request.adapterLuid = {1, 2};
    CapabilityReportV1 report{};
    report.structSize = sizeof(report);
    auto result = host.Probe(request, report);
    REQUIRE(result);
    CHECK(s_behavior.probeCalls == 1);
    CHECK(report.vendorId == 0x10DE);
    CHECK(report.deviceId == 0x2684);

    result = host.RequestShutdown();
    CHECK(result.error == BackendHostError::ShutdownDeferred);
    CHECK(host.State() == BackendHostState::ShutdownDeferred);
    CHECK(s_behavior.shutdownCalls == 1);
    CHECK(module.deferredCalls == 1);
    CHECK(host.RequestShutdown().error ==
          BackendHostError::ShutdownDeferred);
    CHECK(s_behavior.shutdownCalls == 1);
}

TEST_CASE("P04_backend_host_preserves_typed_device_creation_failure", "[unit][phase04]")
{
    s_behavior = {};
    s_behavior.probeResult = Result::DeviceCreationFailed;
    FakeModule module;
    BackendHost host;
    REQUIRE(host.Load(module, ValidCallbacks()));

    ProbeRequestV1 request{};
    request.structSize = sizeof(request);
    CapabilityReportV1 report{};
    report.structSize = sizeof(report);
    const auto result = host.Probe(request, report);
    CHECK(result.error == BackendHostError::ProbeFailed);
    CHECK(result.backendResult == Result::DeviceCreationFailed);
    CHECK(report.result == Result::DeviceCreationFailed);
    CHECK(host.State() == BackendHostState::Ready);
}

TEST_CASE("P05_backend_host_negotiates_and_forwards_optional_bridge_api", "[unit][phase05]")
{
    s_behavior = {};
    FakeModule module;
    BackendHost host;
    REQUIRE(host.Load(module, ValidCallbacks()));
    REQUIRE(host.BridgeAvailable());

    BridgeCreateRequestV1 create{};
    create.structSize = sizeof(create);
    create.epoch = 1;
    BridgeStatusV1 status{};
    status.structSize = sizeof(status);
    REQUIRE(host.CreateBridge(create, status));
    CHECK(s_behavior.createBridgeCalls == 1);
    CHECK(host.CreateBridge(create, status).error ==
          BackendHostError::BridgeAlreadyCreated);

    BridgePatternRequestV1 pattern{};
    pattern.structSize = sizeof(pattern);
    pattern.epoch = 1;
    REQUIRE(host.SubmitBridgePattern(pattern, status));
    CHECK(s_behavior.submitBridgeCalls == 1);
    REQUIRE(host.DestroyBridge(status));
    CHECK(s_behavior.destroyBridgeCalls == 1);
    CHECK(host.SubmitBridgePattern(pattern, status).error ==
          BackendHostError::BridgeNotCreated);
}

TEST_CASE("P05_backend_host_preserves_bridge_failures_and_optional_absence", "[unit][phase05]")
{
    s_behavior = {};
    FakeModule module;
    BackendHost host;

    SECTION("old optional size stays valid but bridge is unavailable")
    {
        s_behavior.apiSize = static_cast<std::uint32_t>(
            offsetof(BackendApiV1, createBridge));
        REQUIRE(host.Load(module, ValidCallbacks()));
        CHECK_FALSE(host.BridgeAvailable());
        BridgeCreateRequestV1 create{};
        create.structSize = sizeof(create);
        BridgeStatusV1 status{};
        status.structSize = sizeof(status);
        CHECK(host.CreateBridge(create, status).error ==
              BackendHostError::BridgeApiUnavailable);
    }

    SECTION("typed create failure remains visible")
    {
        s_behavior.createBridgeResult = Result::BridgeCreateFailed;
        REQUIRE(host.Load(module, ValidCallbacks()));
        BridgeCreateRequestV1 create{};
        create.structSize = sizeof(create);
        BridgeStatusV1 status{};
        status.structSize = sizeof(status);
        const auto result = host.CreateBridge(create, status);
        CHECK(result.error == BackendHostError::BridgeCreateFailed);
        CHECK(result.backendResult == Result::BridgeCreateFailed);
        CHECK(status.result == Result::BridgeCreateFailed);
    }
}

TEST_CASE("P06_backend_host_negotiates_and_forwards_raster_api", "[unit][phase6]")
{
    s_behavior = {};
    FakeModule module;
    BackendHost host;
    REQUIRE(host.Load(module, ValidCallbacks()));
    REQUIRE(host.RasterAvailable());

    RasterCreateRequestV1 create{};
    create.structSize = sizeof(create);
    RasterStatusV1 status{};
    status.structSize = sizeof(status);
    REQUIRE(host.CreateRaster(create, status));
    CHECK(s_behavior.createRasterCalls == 1);
    CHECK(host.CreateRaster(create, status).error ==
          BackendHostError::RasterAlreadyCreated);

    RasterFrameRequestV1 frame{};
    frame.structSize = sizeof(frame);
    REQUIRE(host.RenderRasterFrame(frame, status));
    CHECK(s_behavior.renderRasterCalls == 1);
    REQUIRE(host.DestroyRaster(status));
    CHECK(s_behavior.destroyRasterCalls == 1);
    CHECK(host.RenderRasterFrame(frame, status).error ==
          BackendHostError::RasterNotCreated);
}

TEST_CASE("P06_backend_host_preserves_raster_failures_and_optional_absence", "[unit][phase6]")
{
    s_behavior = {};
    FakeModule module;
    BackendHost host;

    SECTION("phase5 backend remains valid without raster functions")
    {
        s_behavior.apiMinor = kBackendAbiPhase5Minor;
        s_behavior.apiSize = static_cast<std::uint32_t>(
            offsetof(BackendApiV1, createRaster));
        REQUIRE(host.Load(module, ValidCallbacks()));
        CHECK_FALSE(host.RasterAvailable());
    }

    SECTION("typed render failure remains visible")
    {
        s_behavior.renderRasterResult = Result::RasterInvalidPacket;
        REQUIRE(host.Load(module, ValidCallbacks()));
        RasterCreateRequestV1 create{};
        create.structSize = sizeof(create);
        RasterStatusV1 status{};
        status.structSize = sizeof(status);
        REQUIRE(host.CreateRaster(create, status));
        RasterFrameRequestV1 frame{};
        frame.structSize = sizeof(frame);
        const auto result = host.RenderRasterFrame(frame, status);
        CHECK(result.error == BackendHostError::RasterRenderFailed);
        CHECK(result.backendResult == Result::RasterInvalidPacket);
        CHECK(status.result == Result::RasterInvalidPacket);
    }
}
