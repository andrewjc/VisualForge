#include "renderer_trace/Crc32.h"

namespace vf::renderer::trace {

std::uint32_t Crc32(const std::span<const std::byte> bytes) noexcept
{
    auto crc = 0xFFFFFFFFu;
    for (const auto byte : bytes) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (unsigned bit = 0; bit < 8; ++bit) {
            const auto mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

}
