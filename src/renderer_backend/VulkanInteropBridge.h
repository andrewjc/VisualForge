#pragma once

#include "renderer_api/BackendAbi.h"

#include <memory>

namespace vf::renderer::backend {

class VulkanInteropBridge
{
public:
    VulkanInteropBridge();
    ~VulkanInteropBridge();

    VulkanInteropBridge(const VulkanInteropBridge&) = delete;
    VulkanInteropBridge& operator=(const VulkanInteropBridge&) = delete;

    [[nodiscard]] abi::Result Create(
        const abi::HostCallbacksV1& callbacks,
        const abi::BridgeCreateRequestV1& request,
        abi::BridgeStatusV1& status) noexcept;
    [[nodiscard]] abi::Result SubmitPattern(
        const abi::BridgePatternRequestV1& request,
        abi::BridgeStatusV1& status) noexcept;
    [[nodiscard]] abi::Result Destroy(
        abi::BridgeStatusV1& status) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
