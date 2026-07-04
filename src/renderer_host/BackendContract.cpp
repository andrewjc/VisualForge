#include "renderer_host/BackendContract.h"

namespace vf::renderer {

BackendContractError ValidateHostCallbacks(
    const abi::HostCallbacksV1& callbacks) noexcept
{
    if (callbacks.structSize < abi::kHostCallbacksV1RequiredSize) {
        return BackendContractError::HostCallbacksTooSmall;
    }
    if (callbacks.log == nullptr) {
        return BackendContractError::NullLogCallback;
    }
    return BackendContractError::None;
}

BackendContractError ValidateBackendApi(const abi::BackendApiV1& api) noexcept
{
    if (api.structSize < abi::kBackendApiV1RequiredSize) {
        return BackendContractError::ApiTooSmall;
    }
    if (api.abiMajor != abi::kBackendAbiMajor) {
        return BackendContractError::AbiMajorMismatch;
    }
    if (api.probe == nullptr) {
        return BackendContractError::NullProbeFunction;
    }
    if (api.shutdown == nullptr) {
        return BackendContractError::NullShutdownFunction;
    }
    return BackendContractError::None;
}

AdapterSelection SelectAdapter(
    const abi::AdapterLuid requiredLuid,
    const std::uint64_t requiredCapabilities,
    const std::span<const AdapterSnapshot> adapters) noexcept
{
    for (std::size_t adapterIndex = 0;
         adapterIndex < adapters.size();
         ++adapterIndex) {
        const auto& adapter = adapters[adapterIndex];
        if (adapter.luidValid == 0 || adapter.luid != requiredLuid) {
            continue;
        }

        const auto missing =
            requiredCapabilities & ~adapter.capabilities;
        if (missing != 0) {
            return AdapterSelection{
                AdapterSelectionError::MissingRequiredCapabilities,
                adapterIndex,
                0xFFFFFFFFu,
                missing,
            };
        }

        constexpr auto requiredQueueFlags =
            static_cast<std::uint32_t>(QueueGraphics | QueueCompute);
        for (std::size_t queueIndex = 0;
             queueIndex < adapter.queues.size();
             ++queueIndex) {
            const auto& queue = adapter.queues[queueIndex];
            if (queue.queueCount != 0 &&
                (queue.flags & requiredQueueFlags) ==
                    requiredQueueFlags) {
                return AdapterSelection{
                    AdapterSelectionError::None,
                    adapterIndex,
                    static_cast<std::uint32_t>(queueIndex),
                    0,
                };
            }
        }
        return AdapterSelection{
            AdapterSelectionError::QueueFamilyNotFound,
            adapterIndex,
            0xFFFFFFFFu,
            0,
        };
    }
    return AdapterSelection{
        AdapterSelectionError::AdapterLuidNotFound,
        0,
        0xFFFFFFFFu,
        0,
    };
}

}
