#pragma once

#include "renderer_core/EngineTexture.h"

#include <cstddef>
#include <cstdint>

struct ID3D11Texture2D;
struct D3D11_TEXTURE2D_DESC;
struct D3D11_SUBRESOURCE_DATA;

namespace vf::engine_texture_residency {

// The engine's textures, kept for the whole run and addressable by the same
// identity a draw records.
//
// This is the texture-side counterpart to EngineMeshExtractor: the mirror
// needs many textures at once, resident across frames, resolved per draw. The
// existing EngineTextureCapture cannot serve that -- it publishes exactly one
// texture to a file and then latches itself off, which is the right shape for
// a one-shot diagnostic and the wrong one for a renderer.
//
// Pixels are taken from the engine's own upload at creation rather than read
// back off the GPU. Fallout 4 creates its material textures immutable with
// their contents supplied up front, so the bytes are already in hand at that
// moment; a readback would instead mean a staging copy and a map on the render
// thread, which synchronises the GPU inside the engine's own submission. The
// mesh extractor moved its reads to Present for exactly that reason, and a
// texture that arrives with its data needs no such trade.
struct ResidencyStats
{
    std::uint32_t resident{};
    std::uint64_t residentBytes{};
    // Creations that were not material textures: too small, cube maps, arrays,
    // multisampled, or a format outside the diffuse family. Not a failure --
    // most textures a frame creates are render targets and scratch surfaces.
    std::uint32_t rejected{};
    // Textures that would have been kept but for the budget. Reported rather
    // than hidden, because a full cache and an empty one both resolve to
    // "texture not found" at the draw that wanted it.
    std::uint32_t budgetDropped{};
    // Creations whose pixels could not be read: no initial data, or a pitch
    // that disagrees with the format's own footprint.
    std::uint32_t unreadable{};
};

// Records a texture's pixels as the engine creates it.
//
// Called from EngineTextureCapture's CreateTexture2D hook rather than from one
// of ours: MinHook allows a single hook per address and that module prepares
// its own first. The same arrangement carries pixel-shader bindings.
void NoteCreatedTexture(
    const D3D11_TEXTURE2D_DESC* description,
    const D3D11_SUBRESOURCE_DATA* initialData,
    ID3D11Texture2D* texture) noexcept;

// The texture behind an identity a draw recorded, or null when it was never
// resident. Null is a normal answer and must leave the material shading from
// its base colour rather than sampling something else.
[[nodiscard]] const renderer::texture::CapturedTexture* Find(
    std::uint64_t identity) noexcept;

[[nodiscard]] ResidencyStats Counters() noexcept;

}
