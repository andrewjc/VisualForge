#pragma once

#include "renderer_api/BackendAbi.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace vf::renderer {

enum class BackendContractError : std::uint8_t
{
    None,
    HostCallbacksTooSmall,
    NullLogCallback,
    ApiTooSmall,
    AbiMajorMismatch,
    NullProbeFunction,
    NullShutdownFunction
};

[[nodiscard]] BackendContractError ValidateHostCallbacks(
    const abi::HostCallbacksV1& callbacks) noexcept;
[[nodiscard]] BackendContractError ValidateBackendApi(
    const abi::BackendApiV1& api) noexcept;

enum QueueFlags : std::uint32_t
{
    QueueGraphics = 1u << 0,
    QueueCompute = 1u << 1,
    QueueTransfer = 1u << 2
};

struct QueueFamilySnapshot
{
    std::uint32_t flags{};
    std::uint32_t queueCount{};
};

struct AdapterSnapshot
{
    abi::AdapterLuid luid{};
    std::uint64_t capabilities{};
    std::span<const QueueFamilySnapshot> queues;
    std::uint32_t vendorId{};
    std::uint32_t deviceId{};
    std::uint32_t luidValid{};
    std::uint32_t reserved{};
};

enum class AdapterSelectionError : std::uint8_t
{
    None,
    AdapterLuidNotFound,
    MissingRequiredCapabilities,
    QueueFamilyNotFound
};

struct AdapterSelection
{
    AdapterSelectionError error{AdapterSelectionError::AdapterLuidNotFound};
    std::size_t adapterIndex{};
    std::uint32_t queueFamilyIndex{0xFFFFFFFFu};
    std::uint64_t missingCapabilities{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == AdapterSelectionError::None;
    }
};

[[nodiscard]] AdapterSelection SelectAdapter(
    abi::AdapterLuid requiredLuid,
    std::uint64_t requiredCapabilities,
    std::span<const AdapterSnapshot> adapters) noexcept;

}
