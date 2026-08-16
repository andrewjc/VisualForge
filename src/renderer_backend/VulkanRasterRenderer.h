#pragma once

#include "renderer_api/BackendAbi.h"

#include <memory>

namespace vf::renderer::backend {

class VulkanRasterRenderer
{
public:
    VulkanRasterRenderer();
    ~VulkanRasterRenderer();

    VulkanRasterRenderer(const VulkanRasterRenderer&) = delete;
    VulkanRasterRenderer& operator=(const VulkanRasterRenderer&) = delete;

    [[nodiscard]] abi::Result Create(
        const abi::HostCallbacksV1& callbacks,
        const abi::RasterCreateRequestV1& request,
        abi::RasterStatusV1& status) noexcept;
    [[nodiscard]] abi::Result Render(
        const abi::RasterFrameRequestV1& request,
        abi::RasterStatusV1& status) noexcept;
    [[nodiscard]] abi::Result Destroy(
        abi::RasterStatusV1& status) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
