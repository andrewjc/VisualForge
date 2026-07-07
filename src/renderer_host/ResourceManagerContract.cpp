#include "renderer_host/ResourceManagerContract.h"

#include <array>

namespace vf::renderer {

namespace {

constexpr std::array<std::uint32_t, kResourceManagerVtableSlots> kTargets{
    0x0226F540u, 0x0226E210u, 0x0226E1A0u, 0x0226E180u,
    0x0226E2E0u, 0x0226E300u, 0x0226E3D0u, 0x0226E3F0u,
    0x0226E4A0u, 0x0226E410u, 0x0226E570u, 0x0226E590u,
    0x0226E5B0u, 0x0226E680u, 0x0226E690u, 0x0226E6B0u,
    0x0226E6C0u, 0x0226E6D0u, 0x0226E6E0u, 0x0226E8A0u,
    0x0226E880u, 0x0226E7B0u, 0x0226E930u, 0x0226E950u,
    0x0226E970u, 0x0226E990u, 0x0226E9B0u, 0x0226EA90u,
    0x0226EB20u, 0x0226EB40u, 0x0226EBD0u, 0x0226EBF0u,
    0x0226EC70u, 0x0226EC90u, 0x0226ECB0u, 0x0226ECE0u,
    0x0226ED00u, 0x0226EE00u, 0x0226EE60u, 0x0226EF80u,
    0x0226EFB0u, 0x0226EFD0u, 0x0226EFF0u, 0x0226F220u,
    0x0226F0B0u, 0x0226F380u, 0x0226F3A0u, 0x0226F320u,
    0x0226F3C0u, 0x0226F3D0u, 0x0226F3E0u, 0x0226F4B0u,
};

}

std::span<const std::uint32_t, kResourceManagerVtableSlots>
TargetResourceManagerVtable_1_11_221() noexcept
{
    return kTargets;
}

ResourceManagerContractReport ValidateResourceManagerVtable(
    const std::uintptr_t imageBase,
    const std::uintptr_t observedVtable,
    const std::span<const std::uintptr_t> slots) noexcept
{
    if (imageBase == 0) {
        return {ResourceManagerContractError::NullImageBase, 0, 0};
    }
    if (observedVtable != imageBase + kResourceManagerVtableRva) {
        return {ResourceManagerContractError::VtableMismatch, 0, 0};
    }
    if (slots.size() != kTargets.size()) {
        return {ResourceManagerContractError::SlotCountMismatch, 0, 0};
    }
    for (std::size_t index = 0; index < kTargets.size(); ++index) {
        if (slots[index] != imageBase + kTargets[index]) {
            return {
                ResourceManagerContractError::SlotMismatch,
                static_cast<std::uint32_t>(index),
                index,
            };
        }
    }
    return {ResourceManagerContractError::None, 0, kTargets.size()};
}

const char* ToString(const ResourceManagerContractError error) noexcept
{
    switch (error) {
    case ResourceManagerContractError::None: return "none";
    case ResourceManagerContractError::NullImageBase: return "null-image-base";
    case ResourceManagerContractError::VtableMismatch: return "vtable-mismatch";
    case ResourceManagerContractError::SlotCountMismatch: return "slot-count-mismatch";
    case ResourceManagerContractError::SlotMismatch: return "slot-mismatch";
    }
    return "unknown";
}

}
