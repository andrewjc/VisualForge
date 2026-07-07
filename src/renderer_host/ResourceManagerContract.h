#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace vf::renderer {

constexpr std::size_t kResourceManagerVtableSlots = 52;
constexpr std::uint32_t kResourceManagerSingletonRva = 0x03438128u;
constexpr std::uint32_t kResourceManagerVtableRva = 0x029139A8u;

enum class ResourceManagerContractError : std::uint8_t
{
    None,
    NullImageBase,
    VtableMismatch,
    SlotCountMismatch,
    SlotMismatch
};

struct ResourceManagerContractReport
{
    ResourceManagerContractError error{ResourceManagerContractError::None};
    std::uint32_t failedSlot{};
    std::size_t validatedSlots{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == ResourceManagerContractError::None;
    }
};

[[nodiscard]] std::span<const std::uint32_t, kResourceManagerVtableSlots>
TargetResourceManagerVtable_1_11_221() noexcept;
[[nodiscard]] ResourceManagerContractReport ValidateResourceManagerVtable(
    std::uintptr_t imageBase,
    std::uintptr_t observedVtable,
    std::span<const std::uintptr_t> slots) noexcept;
[[nodiscard]] const char* ToString(ResourceManagerContractError error) noexcept;

}
