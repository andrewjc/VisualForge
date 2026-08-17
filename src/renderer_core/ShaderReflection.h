#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vf::renderer::shader {

// Compiled shader bytecode describes its own constant buffers. Every DXBC
// container carries a reflection chunk naming each buffer and each variable in
// it, with the offset and size of every field.
//
// This exists because the alternative did not work. Sampling the engine's
// constant buffers and reading numbers out of fixed offsets identified a
// filter kernel and a pair of clip planes, and then stalled: the engine reuses
// one wide pixel-shader constant buffer across shader techniques, so an offset
// means different things depending on which technique last bound it. Measured
// live, every meaningful word of the widest buffer varied across a single
// frame's draws, including words that read as the camera origin in isolation.
// A quantity that is constant for a frame cannot vary across that frame, so
// there is no single layout to find by looking.
//
// Reflection answers the question the engine itself answered at compile time.

enum class ReflectionError : std::uint8_t
{
    None,
    TruncatedContainer,
    BadMagic,
    UnsupportedVersion,
    MissingReflectionChunk,
    TruncatedChunk,
    InvalidOffset,
};

// One field inside a constant buffer: what the shader calls it, where it
// starts and how many bytes it occupies.
struct ReflectedVariable
{
    std::string name;
    std::uint32_t offset{};
    std::uint32_t size{};
};

struct ReflectedBuffer
{
    std::string name;
    std::uint32_t size{};
    std::vector<ReflectedVariable> variables;
};

struct ReflectedShader
{
    std::vector<ReflectedBuffer> buffers;
};

// Reads the reflection chunk out of a DXBC container.
//
// Refuses rather than guesses at every step. A parser that reads past a
// truncated chunk produces field names assembled from unrelated bytes, and a
// name is exactly the thing that would then be trusted.
[[nodiscard]] ReflectionError ReflectShader(
    std::span<const std::byte> bytecode,
    ReflectedShader& reflection) noexcept;

[[nodiscard]] const char* ToString(ReflectionError error) noexcept;

}
