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

// A texture or sampler the shader declares, with the register it binds to.
// D3D_SIT_TEXTURE and D3D_SIT_SAMPLER are 2 and 3 in the D3D_SHADER_INPUT_TYPE
// enum this is read from; other input types (constant buffers, UAVs) are
// skipped, since nothing here consumes them.
enum class ResourceKind : std::uint8_t
{
    Texture,
    Sampler,
    Other,
};

struct ReflectedResource
{
    std::string name;
    ResourceKind kind{ResourceKind::Other};
    std::uint32_t bindPoint{};
    std::uint32_t bindCount{};
};

struct ReflectedShader
{
    std::vector<ReflectedBuffer> buffers;
    std::vector<ReflectedResource> resources;
};

// Reads the reflection chunk out of a DXBC container.
//
// Refuses rather than guesses at every step. A parser that reads past a
// truncated chunk produces field names assembled from unrelated bytes, and a
// name is exactly the thing that would then be trusted.
[[nodiscard]] ReflectionError ReflectShader(
    std::span<const std::byte> bytecode,
    ReflectedShader& reflection) noexcept;

// Finds the register a shader's base-colour texture binds to, from the
// shader's own declarations. False when this shader declares no material
// texture at all, which is the common case: the post, godray and volumetric
// passes bind real textures that are not anybody's albedo, and answering
// "slot 0" for those would attach a depth buffer to a material.
//
// The rule is measured, not assumed. Fallout 4 1.11.221's material shaders
// declare their textures as an array `tex[N]` bound at register N, with index
// 0 the base colour; some techniques declare a lone scalar `tex` instead.
// Both forms appear in the live reflection dump, `tex[0]` across 150 distinct
// pixel shaders and `tex` across 144 -- by far the two largest texture
// populations in the game.
//
// The register comes from the reflected bind point, never from the digit in
// the name: the two are separate facts and a shader may bind `tex[0]`
// anywhere.
[[nodiscard]] bool FindBaseColorTextureSlot(
    const ReflectedShader& reflection,
    std::uint32_t& slot) noexcept;

[[nodiscard]] const char* ToString(ReflectionError error) noexcept;

}
