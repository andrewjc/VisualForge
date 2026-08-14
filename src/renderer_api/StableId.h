#pragma once

#include <cstdint>
#include <string_view>

namespace vf::renderer {

enum class StableIdDomain : std::uint8_t
{
    Capture,
    Frame,
    View,
    Swapchain,
    Writer,
    Resource,
    Correlation
};

struct StableId
{
    std::uint64_t value{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value != 0;
    }

    friend bool operator==(const StableId&, const StableId&) = default;
};

[[nodiscard]] StableId MakeStableId(
    StableIdDomain domain,
    std::string_view canonicalKey,
    std::uint32_t generation = 0) noexcept;

}
