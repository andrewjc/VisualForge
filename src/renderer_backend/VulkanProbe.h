#pragma once

#include "renderer_api/BackendAbi.h"

namespace vf::renderer::backend {

[[nodiscard]] abi::Result ProbeVulkan(
    const abi::HostCallbacksV1& callbacks,
    const abi::ProbeRequestV1& request,
    abi::CapabilityReportV1& report) noexcept;

}
