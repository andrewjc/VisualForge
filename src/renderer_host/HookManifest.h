#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace vf::renderer {

enum class HookSiteKind : std::uint8_t
{
    MaskedCode,
    RelocatedPointer
};

struct HookSiteDescriptor
{
    std::uint32_t id{};
    std::string_view name;
    HookSiteKind kind{HookSiteKind::MaskedCode};
    std::uint32_t rva{};
    std::span<const std::byte> expected;
    std::span<const std::byte> mask;
    std::uint32_t expectedTargetRva{};
};

enum class HookSiteError : std::uint8_t
{
    None,
    EmptyManifest,
    InvalidDescriptor,
    DuplicateId,
    OutOfBounds,
    ByteMismatch,
    PointerMismatch
};

struct HookSiteValidation
{
    HookSiteError error{HookSiteError::None};
    std::uint32_t siteId{};
    std::uint32_t mismatchOffset{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == HookSiteError::None;
    }
};

struct HookManifestReport
{
    HookSiteValidation failure{};
    std::size_t validatedCount{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return failure.error == HookSiteError::None;
    }
};

[[nodiscard]] HookSiteValidation ValidateHookSite(
    const HookSiteDescriptor& descriptor,
    std::span<const std::byte> image,
    std::uintptr_t imageBase) noexcept;

[[nodiscard]] HookManifestReport ValidateHookManifest(
    std::span<const HookSiteDescriptor> manifest,
    std::span<const std::byte> image,
    std::uintptr_t imageBase) noexcept;

[[nodiscard]] std::span<const HookSiteDescriptor> TargetHookManifest_1_11_221() noexcept;
[[nodiscard]] const char* ToString(HookSiteError error) noexcept;

}
