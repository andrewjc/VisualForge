#include "renderer_api/StableId.h"

namespace vf::renderer {

StableId MakeStableId(
    const StableIdDomain domain,
    const std::string_view canonicalKey,
    const std::uint32_t generation) noexcept
{
    constexpr std::uint64_t offsetBasis = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;

    auto hash = offsetBasis;
    const auto mix = [&hash](const std::uint8_t byte) noexcept {
        hash ^= byte;
        hash *= prime;
    };

    mix(static_cast<std::uint8_t>(domain));
    mix(0xFFu);
    for (unsigned index = 0; index < 4; ++index) {
        mix(static_cast<std::uint8_t>(
            (generation >> (index * 8)) & 0xFFu));
    }
    mix(0xFEu);
    for (const char character : canonicalKey) {
        mix(static_cast<std::uint8_t>(
            static_cast<unsigned char>(character)));
    }
    return StableId{hash == 0 ? 1 : hash};
}

}
