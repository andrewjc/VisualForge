#pragma once

#include "renderer_api/RasterPacket.h"

#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::texture { struct CapturedTexture; }
namespace vf::renderer::material { struct MaterialReplayBundle; }
namespace vf::renderer::view { struct ViewRecordV1; }

namespace vf::renderer::raster {

struct Rgba8
{
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
    std::uint8_t a{};

    friend bool operator==(const Rgba8&, const Rgba8&) = default;
};

struct RasterImage
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<Rgba8> pixels;

    [[nodiscard]] const Rgba8& At(
        std::uint32_t x,
        std::uint32_t y) const;
};

enum class ReferenceRasterError : std::uint8_t
{
    None,
    InvalidPacket,
    UnsupportedState
};

struct RasterComparison
{
    std::uint64_t comparedPixels{};
    std::uint64_t differingPixels{};
    std::uint32_t maximumChannelError{};
    double meanAbsoluteError{};

    [[nodiscard]] bool Within(
        std::uint32_t maximumError,
        double maximumMeanError,
        std::uint64_t maximumDifferingPixels) const noexcept;
};

[[nodiscard]] ReferenceRasterError RenderReference(
    const DecodedPacket& packet,
    RasterImage& image) noexcept;
[[nodiscard]] ReferenceRasterError RenderReferenceTextured(
    const DecodedPacket& packet,
    const texture::CapturedTexture& texture,
    RasterImage& image) noexcept;
[[nodiscard]] ReferenceRasterError RenderReferenceMaterial(
    const DecodedPacket& packet,
    const material::MaterialReplayBundle& material,
    RasterImage& image) noexcept;
// Per-draw texturing: each draw's material selects its own texture from
// `library` by RasterMaterialV1::textureIndex, or shades flat from baseColor
// at raster::kNoMaterialTexture. This is the reference the backend's
// descriptor-indexed array is checked against -- RenderReferenceTextured
// applies one texture to the whole frame and cannot tell "every material
// happens to share a texture" from "the renderer cannot address more than
// one", which is the exact bug this exists to catch.
[[nodiscard]] ReferenceRasterError RenderReferenceTextureLibrary(
    const DecodedPacket& packet,
    std::span<const texture::CapturedTexture> library,
    RasterImage& image) noexcept;
[[nodiscard]] ReferenceRasterError ProjectPacketForView(
    const DecodedPacket& packet,
    const view::ViewRecordV1& view,
    DecodedPacket& projected) noexcept;
[[nodiscard]] RasterComparison CompareRaster(
    std::span<const Rgba8> expected,
    std::span<const Rgba8> actual) noexcept;
[[nodiscard]] std::uint8_t ToneMapToSrgb8(float linear) noexcept;

}
