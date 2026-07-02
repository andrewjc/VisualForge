#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace vf::renderer::trace {

[[nodiscard]] std::uint32_t Crc32(std::span<const std::byte> bytes) noexcept;

}
