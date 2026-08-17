#include "renderer_core/EngineTexture.h"
#include "renderer_api/RasterPacket.h"
#include "renderer_core/RasterGolden.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using namespace vf::renderer::texture;

TextureSubresource SolidRgba(
    const std::uint32_t mip,
    const std::uint32_t layer,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::array<std::uint8_t, 4> color)
{
    TextureSubresource subresource{};
    subresource.mipLevel = mip;
    subresource.arrayLayer = layer;
    subresource.width = width;
    subresource.height = height;
    subresource.rowPitch = width * 4;
    subresource.slicePitch = subresource.rowPitch * height;
    subresource.bytes.resize(subresource.slicePitch);
    for (std::size_t offset = 0; offset < subresource.bytes.size(); offset += 4) {
        std::memcpy(subresource.bytes.data() + offset, color.data(), 4);
    }
    return subresource;
}

}

TEST_CASE("phase8 DXGI resource and view mappings are exact", "[phase8][texture]")
{
    struct Fixture {
        TextureFormat resource;
        TextureFormat view;
        TextureFamily family;
        std::uint32_t bytes;
        bool srgb;
    };
    const Fixture fixtures[]{
        {TextureFormat::R8G8B8A8Typeless, TextureFormat::R8G8B8A8UnormSrgb,
            TextureFamily::RGBA8, 4, true},
        {TextureFormat::BC1Typeless, TextureFormat::BC1Unorm,
            TextureFamily::BC1, 8, false},
        {TextureFormat::BC2Typeless, TextureFormat::BC2UnormSrgb,
            TextureFamily::BC2, 16, true},
        {TextureFormat::BC3Unorm, TextureFormat::BC3Unorm,
            TextureFamily::BC3, 16, false},
        {TextureFormat::BC4Typeless, TextureFormat::BC4Snorm,
            TextureFamily::BC4, 8, false},
        {TextureFormat::BC5Typeless, TextureFormat::BC5Unorm,
            TextureFamily::BC5, 16, false},
        {TextureFormat::BC6HTypeless, TextureFormat::BC6HUf16,
            TextureFamily::BC6H, 16, false},
        {TextureFormat::BC7Typeless, TextureFormat::BC7UnormSrgb,
            TextureFamily::BC7, 16, true},
    };
    for (const auto& fixture : fixtures) {
        TextureFormatInfo info{};
        INFO(static_cast<std::uint32_t>(fixture.view));
        REQUIRE(ResolveTextureFormat(
            fixture.resource, fixture.view, info) == TexturePacketError::None);
        CHECK(info.family == fixture.family);
        CHECK(info.bytesPerBlock == fixture.bytes);
        CHECK(info.srgb == fixture.srgb);
    }
    TextureFormatInfo ignored{};
    CHECK(ResolveTextureFormat(TextureFormat::BC5Typeless,
        TextureFormat::BC7Unorm, ignored) ==
        TexturePacketError::IllegalViewFormat);
    CHECK(ResolveTextureFormat(TextureFormat::BC1Unorm,
        TextureFormat::BC1UnormSrgb, ignored) ==
        TexturePacketError::IllegalViewFormat);
}

TEST_CASE("phase8 block footprints handle odd mip extents", "[phase8][texture]")
{
    TextureFootprint footprint{};
    REQUIRE(ComputeTextureFootprint(
        TextureFormat::BC1Unorm, 7, 5, footprint) == TexturePacketError::None);
    CHECK(footprint.rowBytes == 16);
    CHECK(footprint.rowCount == 2);
    CHECK(footprint.byteSize == 32);
    REQUIRE(ComputeTextureFootprint(
        TextureFormat::BC7Unorm, 1, 1, footprint) == TexturePacketError::None);
    CHECK(footprint.rowBytes == 16);
    CHECK(footprint.rowCount == 1);
    CHECK(footprint.byteSize == 16);
    REQUIRE(ComputeTextureFootprint(
        TextureFormat::R8G8Unorm, 7, 5, footprint) == TexturePacketError::None);
    CHECK(footprint.rowBytes == 14);
    CHECK(footprint.rowCount == 5);
    CHECK(footprint.byteSize == 70);
    CHECK(ComputeTextureFootprint(
        TextureFormat::BC1Unorm, 0, 1, footprint) ==
        TexturePacketError::InvalidExtent);
}

TEST_CASE("phase8 padded source rows repack to tight block payloads", "[phase8][texture]")
{
    std::vector<std::byte> padded(48, std::byte{0x7F});
    for (std::size_t index = 0; index < 16; ++index) {
        padded[index] = static_cast<std::byte>(index);
        padded[24 + index] = static_cast<std::byte>(32 + index);
    }
    std::vector<std::byte> packed;
    REQUIRE(RepackTextureRows(TextureFormat::BC1Unorm,
        7, 5, 24, padded, packed) == TexturePacketError::None);
    REQUIRE(packed.size() == 32);
    CHECK(packed[0] == std::byte{0});
    CHECK(packed[15] == std::byte{15});
    CHECK(packed[16] == std::byte{32});
    CHECK(packed[31] == std::byte{47});
    CHECK(RepackTextureRows(TextureFormat::BC1Unorm,
        7, 5, 8, padded, packed) == TexturePacketError::InvalidPitch);
}

TEST_CASE("phase8 sampler normalization produces stable cache keys", "[phase8][texture]")
{
    TextureSamplerDesc input{};
    input.addressU = TextureAddressMode::MirrorOnce;
    input.addressV = TextureAddressMode::Clamp;
    input.mipLodBias = -0.0f;
    input.maxAnisotropy = 64.0f;
    input.anisotropyEnable = 1;
    input.minFilter = TextureFilter::Nearest;
    input.magFilter = TextureFilter::Nearest;
    input.mipFilter = TextureFilter::Nearest;
    TextureSamplerDesc normalized{};
    REQUIRE(NormalizeSampler(input, normalized) == TexturePacketError::None);
    CHECK(normalized.minFilter == TextureFilter::Linear);
    CHECK(normalized.magFilter == TextureFilter::Linear);
    CHECK(normalized.mipFilter == TextureFilter::Linear);
    CHECK(normalized.maxAnisotropy == 16.0f);
    CHECK(std::signbit(normalized.mipLodBias) == false);
    TextureSamplerDesc again{};
    REQUIRE(NormalizeSampler(normalized, again) == TexturePacketError::None);
    CHECK(again == normalized);
    input.maxLod = std::numeric_limits<float>::quiet_NaN();
    CHECK(NormalizeSampler(input, normalized) ==
        TexturePacketError::InvalidSampler);
}

TEST_CASE("phase8 texture packet round trips cube mip payloads", "[phase8][texture]")
{
    CapturedTexture source{};
    source.resourceId = 0x8000000000000042ull;
    source.generation = 7;
    source.dimension = TextureDimension::Cube;
    source.width = 2;
    source.height = 2;
    source.arrayLayers = 6;
    source.mipLevels = 2;
    source.resourceFormat = TextureFormat::R8G8B8A8Typeless;
    source.viewFormat = TextureFormat::R8G8B8A8UnormSrgb;
    source.residentBaseMip = 0;
    source.residentMipCount = 2;
    for (std::uint32_t layer = 0; layer < 6; ++layer) {
        source.subresources.push_back(SolidRgba(
            0, layer, 2, 2, {static_cast<std::uint8_t>(layer), 2, 3, 255}));
        source.subresources.push_back(SolidRgba(
            1, layer, 1, 1, {static_cast<std::uint8_t>(layer), 4, 5, 255}));
    }
    std::vector<std::byte> bytes;
    REQUIRE(EncodeCapturedTexture(source, bytes) == TexturePacketError::None);
    CapturedTexture decoded;
    REQUIRE(DecodeCapturedTexture(bytes, decoded) == TexturePacketError::None);
    CHECK(decoded.resourceId == source.resourceId);
    CHECK(decoded.generation == 7);
    CHECK(decoded.dimension == TextureDimension::Cube);
    CHECK(decoded.subresources.size() == 12);
    CHECK(decoded.subresources[7].bytes == source.subresources[7].bytes);

    bytes.back() ^= std::byte{0x40};
    CHECK(DecodeCapturedTexture(bytes, decoded) ==
        TexturePacketError::ChecksumMismatch);
}

TEST_CASE("phase8 malformed row pitch and mip coverage fail closed", "[phase8][texture]")
{
    CapturedTexture texture{};
    texture.resourceId = 9;
    texture.generation = 1;
    texture.width = 7;
    texture.height = 5;
    texture.mipLevels = 1;
    texture.resourceFormat = TextureFormat::BC1Unorm;
    texture.viewFormat = TextureFormat::BC1Unorm;
    TextureSubresource bad{};
    bad.width = 7;
    bad.height = 5;
    bad.rowPitch = 8;
    bad.slicePitch = 16;
    bad.bytes.resize(16);
    texture.subresources.push_back(bad);
    std::vector<std::byte> bytes;
    CHECK(EncodeCapturedTexture(texture, bytes) ==
        TexturePacketError::InvalidPitch);
    texture.subresources[0].rowPitch = 16;
    texture.subresources[0].slicePitch = 32;
    texture.subresources[0].bytes.resize(32);
    texture.residentMipCount = 2;
    CHECK(EncodeCapturedTexture(texture, bytes) ==
        TexturePacketError::InvalidMipRange);
}

TEST_CASE("phase8 fallback and BC samples preserve semantic channels", "[phase8][texture]")
{
    SampledColor color{};
    const struct FallbackFixture {
        FallbackTextureRole role;
        std::array<float, 4> expected;
    } fallbacks[]{
        {FallbackTextureRole::White, {1.0f, 1.0f, 1.0f, 1.0f}},
        {FallbackTextureRole::Black, {0.0f, 0.0f, 0.0f, 1.0f}},
        {FallbackTextureRole::FlatNormal,
            {128.0f / 255.0f, 128.0f / 255.0f, 1.0f, 1.0f}},
        {FallbackTextureRole::NeutralMask, {1.0f, 1.0f, 1.0f, 1.0f}},
    };
    std::array<std::uint64_t, 4> resourceIds{};
    for (std::size_t index = 0; index < std::size(fallbacks); ++index) {
        const auto fallback = MakeFallbackTexture(fallbacks[index].role);
        resourceIds[index] = fallback.resourceId;
        REQUIRE(SampleTexture2D(fallback, 0.25f, 0.75f, 0.0f, color) ==
            TexturePacketError::None);
        CHECK(std::abs(color.r - fallbacks[index].expected[0]) < 0.0001f);
        CHECK(std::abs(color.g - fallbacks[index].expected[1]) < 0.0001f);
        CHECK(std::abs(color.b - fallbacks[index].expected[2]) < 0.0001f);
        CHECK(std::abs(color.a - fallbacks[index].expected[3]) < 0.0001f);
    }
    CHECK(resourceIds[0] != resourceIds[1]);
    CHECK(resourceIds[0] != resourceIds[2]);
    CHECK(resourceIds[0] != resourceIds[3]);

    const auto normal = MakeFallbackTexture(FallbackTextureRole::FlatNormal);
    REQUIRE(SampleTexture2D(normal, 0.25f, 0.75f, 0.0f, color) ==
        TexturePacketError::None);
    CHECK(std::abs(color.r - 128.0f / 255.0f) < 0.0001f);
    CHECK(std::abs(color.g - 128.0f / 255.0f) < 0.0001f);
    CHECK(color.b == 1.0f);
    CHECK(color.a == 1.0f);

    CapturedTexture bc{};
    bc.resourceId = 10;
    bc.generation = 1;
    bc.width = 4;
    bc.height = 4;
    bc.resourceFormat = TextureFormat::BC1Unorm;
    bc.viewFormat = TextureFormat::BC1Unorm;
    TextureSubresource block{};
    block.width = 4;
    block.height = 4;
    block.rowPitch = 8;
    block.slicePitch = 8;
    block.bytes.resize(8);
    const std::uint16_t red565 = 0xF800u;
    std::memcpy(block.bytes.data(), &red565, sizeof(red565));
    std::memcpy(block.bytes.data() + 2, &red565, sizeof(red565));
    bc.subresources.push_back(block);
    REQUIRE(SampleTexture2D(bc, 0.5f, 0.5f, 0.0f, color) ==
        TexturePacketError::None);
    CHECK(color.r == 1.0f);
    CHECK(color.g == 0.0f);
    CHECK(color.b == 0.0f);
    CHECK(color.a == 1.0f);

    bc.resourceId = 11;
    bc.resourceFormat = TextureFormat::BC4Unorm;
    bc.viewFormat = TextureFormat::BC4Unorm;
    block.rowPitch = 8;
    block.slicePitch = 8;
    block.bytes.assign(8, std::byte{0});
    block.bytes[0] = std::byte{64};
    block.bytes[1] = std::byte{64};
    bc.subresources = {block};
    REQUIRE(SampleTexture2D(bc, 0.5f, 0.5f, 0.0f, color) ==
        TexturePacketError::None);
    CHECK(std::abs(color.r - 64.0f / 255.0f) < 0.0001f);
    CHECK(color.g == 0.0f);
    CHECK(color.b == 0.0f);

    bc.resourceId = 12;
    bc.resourceFormat = TextureFormat::BC5Unorm;
    bc.viewFormat = TextureFormat::BC5Unorm;
    block.rowPitch = 16;
    block.slicePitch = 16;
    block.bytes.assign(16, std::byte{0});
    block.bytes[0] = std::byte{128};
    block.bytes[1] = std::byte{128};
    block.bytes[8] = std::byte{32};
    block.bytes[9] = std::byte{32};
    bc.subresources = {block};
    REQUIRE(SampleTexture2D(bc, 0.5f, 0.5f, 0.0f, color) ==
        TexturePacketError::None);
    CHECK(std::abs(color.r - 128.0f / 255.0f) < 0.0001f);
    CHECK(std::abs(color.g - 32.0f / 255.0f) < 0.0001f);
    CHECK(color.b == 0.0f);
}

TEST_CASE("phase8 cube sampler selects Vulkan face order", "[phase8][texture]")
{
    CapturedTexture cube{};
    cube.resourceId = 13;
    cube.generation = 1;
    cube.dimension = TextureDimension::Cube;
    cube.width = 1;
    cube.height = 1;
    cube.arrayLayers = 6;
    cube.resourceFormat = TextureFormat::R8G8B8A8Unorm;
    cube.viewFormat = TextureFormat::R8G8B8A8Unorm;
    for (std::uint32_t face = 0; face < 6; ++face) {
        cube.subresources.push_back(SolidRgba(
            0, face, 1, 1,
            {static_cast<std::uint8_t>(16 + face * 32),
             static_cast<std::uint8_t>(240 - face * 32), 64, 255}));
    }
    const std::array<std::array<float, 3>, 6> directions{{
        {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f},
    }};
    for (std::uint32_t face = 0; face < directions.size(); ++face) {
        SampledColor color{};
        REQUIRE(SampleTextureCube(cube,
            directions[face][0], directions[face][1], directions[face][2],
            0.0f, color) == TexturePacketError::None);
        CHECK(std::abs(color.r - static_cast<float>(16 + face * 32) / 255.0f) <
            0.0001f);
        CHECK(std::abs(color.g - static_cast<float>(240 - face * 32) / 255.0f) <
            0.0001f);
    }
    SampledColor ignored{};
    CHECK(SampleTextureCube(cube, 0.0f, 0.0f, 0.0f, 0.0f, ignored) ==
        TexturePacketError::UnsupportedSampling);
}

TEST_CASE("phase8 raster packet preserves captured UV coordinates", "[phase8][texture]")
{
    using namespace vf::renderer::raster;
    const auto originalBytes = BuildSyntheticPacket();
    DecodedPacket packet;
    REQUIRE(DecodePacket(originalBytes, packet));
    REQUIRE(packet.vertices.size() == 3);
    packet.vertices[0].texCoord[0] = 0.125f;
    packet.vertices[0].texCoord[1] = 0.875f;
    packet.vertices[1].texCoord[0] = 1.25f;
    packet.vertices[2].texCoord[1] = -0.5f;
    std::vector<std::byte> encoded;
    REQUIRE(EncodePacket(packet, encoded));
    DecodedPacket decoded;
    REQUIRE(DecodePacket(encoded, decoded));
    // At least the version that introduced texCoord, not exactly it: an
    // exact match restates today's constant and then goes stale the next
    // time any field anywhere in the packet earns a version bump, for a
    // reason that has nothing to do with UV preservation. >= is the actual
    // invariant this test verifies -- the encoder never stamps a version
    // older than the format it just wrote.
    CHECK(decoded.header.versionMinor >=
        kPacketVertexNormalVersionMinor);
    CHECK(decoded.vertices[0].texCoord[0] == 0.125f);
    CHECK(decoded.vertices[0].texCoord[1] == 0.875f);
    CHECK(decoded.vertices[1].texCoord[0] == 1.25f);
    CHECK(decoded.vertices[2].texCoord[1] == -0.5f);
}

TEST_CASE("phase8 raster packet rejects non-finite vertex channels", "[phase8][texture]")
{
    using namespace vf::renderer::raster;
    auto bytes = BuildSyntheticPacket();
    REQUIRE(bytes.size() >= sizeof(PacketHeaderV1));
    auto& header = *reinterpret_cast<PacketHeaderV1*>(bytes.data());
    auto* vertices = reinterpret_cast<RasterVertexV3*>(
        bytes.data() + header.verticesOffset);
    vertices[0].texCoord[0] = std::numeric_limits<float>::quiet_NaN();
    DecodedPacket decoded;
    CHECK(DecodePacket(bytes, decoded).error == PacketError::InvalidVertex);

    bytes = BuildSyntheticPacket();
    auto& secondHeader = *reinterpret_cast<PacketHeaderV1*>(bytes.data());
    vertices = reinterpret_cast<RasterVertexV3*>(
        bytes.data() + secondHeader.verticesOffset);
    vertices[1].color[2] = std::numeric_limits<float>::infinity();
    CHECK(DecodePacket(bytes, decoded).error == PacketError::InvalidVertex);
}

TEST_CASE("phase8 textured CPU oracle samples interpolated mesh UVs", "[phase8][texture]")
{
    using namespace vf::renderer::raster;
    DecodedPacket packet;
    REQUIRE(DecodePacket(BuildSyntheticPacket(), packet));
    packet.vertices[0].texCoord[0] = 0.0f;
    packet.vertices[0].texCoord[1] = 1.0f;
    packet.vertices[1].texCoord[0] = 1.0f;
    packet.vertices[1].texCoord[1] = 1.0f;
    packet.vertices[2].texCoord[0] = 0.5f;
    packet.vertices[2].texCoord[1] = 0.0f;
    auto texture = MakeFallbackTexture(FallbackTextureRole::White);
    texture.subresources[0].bytes = {
        std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255}};
    RasterImage image;
    REQUIRE(RenderReferenceTextured(packet, texture, image) ==
        ReferenceRasterError::None);
    const auto center = image.At(48, 32);
    CHECK(center.g > center.r);
    CHECK(center.g > center.b);
}

TEST_CASE("PM_the_reference_shades_each_draw_from_its_own_material_texture")
{
    using namespace vf::renderer::raster;
    // Two triangles, side by side, each its own material, each material
    // pointing at a different entry in the same two-texture library. If the
    // renderer applied one texture to the whole frame -- the ceiling this
    // exists to raise -- both halves would come out identical.
    DecodedPacket packet{};
    packet.header.width = 64;
    packet.header.height = 32;
    packet.header.frameIndex = 1;
    packet.header.viewportWidth = 64.0f;
    packet.header.viewportHeight = 32.0f;
    packet.header.viewportMaxDepth = 1.0f;
    packet.header.scissorWidth = 64;
    packet.header.scissorHeight = 32;

    // Pixel coordinates, converted to NDC -- position is clip space, not
    // screen space, and passing raw pixel numbers through unconverted put
    // every vertex outside the viewport entirely on the first attempt here.
    const auto makeVertex = [](float pixelX, float pixelY, float u, float v) {
        RasterVertexV3 vertex{};
        vertex.position[0] = (pixelX / 64.0f) * 2.0f - 1.0f;
        vertex.position[1] = (pixelY / 32.0f) * 2.0f - 1.0f;
        vertex.position[2] = 0.5f;
        vertex.color[0] = vertex.color[1] = vertex.color[2] = 1.0f;
        vertex.texCoord[0] = u;
        vertex.texCoord[1] = v;
        return vertex;
    };
    // Vertex order matches the known-good phase6 fixture: the wide pair at
    // small pixel Y (NDC-negative, screen top), the apex at large pixel Y
    // (NDC-positive, screen bottom). The mirror image of this order rasterizes
    // as clockwise and every triangle here is silently dropped by the default
    // CounterClockwise front face -- which is exactly what the first attempt
    // at this fixture did, and both centroid samples read pure background.
    //
    // Left triangle's centroid is pixel (16,12); right triangle's is (48,12).
    packet.vertices = {
        makeVertex(4.0f, 4.0f, 0.0f, 1.0f),
        makeVertex(28.0f, 4.0f, 1.0f, 1.0f),
        makeVertex(16.0f, 28.0f, 0.5f, 0.0f),
        makeVertex(36.0f, 4.0f, 0.0f, 1.0f),
        makeVertex(60.0f, 4.0f, 1.0f, 1.0f),
        makeVertex(48.0f, 28.0f, 0.5f, 0.0f),
    };
    packet.indices = {0, 1, 2, 3, 4, 5};

    RasterMaterialV1 leftMaterial{};
    leftMaterial.resourceId = 0x7000'0000'0000'0001ull;
    leftMaterial.textureIndex = 0;
    RasterMaterialV1 rightMaterial{};
    rightMaterial.resourceId = 0x7000'0000'0000'0002ull;
    rightMaterial.textureIndex = 1;
    packet.materials = {leftMaterial, rightMaterial};

    RasterDrawV1 leftDraw{};
    leftDraw.materialId = leftMaterial.resourceId;
    leftDraw.firstIndex = 0;
    leftDraw.indexCount = 3;
    RasterDrawV1 rightDraw{};
    rightDraw.materialId = rightMaterial.resourceId;
    rightDraw.firstIndex = 3;
    rightDraw.indexCount = 3;
    packet.draws = {leftDraw, rightDraw};

    auto redTexture = MakeFallbackTexture(FallbackTextureRole::White);
    redTexture.subresources[0].bytes = {
        std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
    auto blueTexture = MakeFallbackTexture(FallbackTextureRole::White);
    blueTexture.subresources[0].bytes = {
        std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}};
    const std::array<CapturedTexture, 2> library{redTexture, blueTexture};

    RasterImage image;
    REQUIRE(RenderReferenceTextureLibrary(packet, library, image) ==
        ReferenceRasterError::None);

    const auto left = image.At(16, 12);
    const auto right = image.At(48, 12);
    CHECK(left.r > left.b);
    CHECK(right.b > right.r);
}

TEST_CASE("PM_a_material_with_no_texture_shades_flat_from_base_colour")
{
    using namespace vf::renderer::raster;
    DecodedPacket packet;
    REQUIRE(DecodePacket(BuildSyntheticPacket(), packet));
    packet.vertices[0].texCoord[0] = 0.0f;
    packet.vertices[0].texCoord[1] = 1.0f;
    packet.vertices[1].texCoord[0] = 1.0f;
    packet.vertices[1].texCoord[1] = 1.0f;
    packet.vertices[2].texCoord[0] = 0.5f;
    packet.vertices[2].texCoord[1] = 0.0f;
    REQUIRE(packet.materials[0].textureIndex == kNoMaterialTexture);

    auto greenTexture = MakeFallbackTexture(FallbackTextureRole::White);
    greenTexture.subresources[0].bytes = {
        std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255}};
    const std::array<CapturedTexture, 1> library{greenTexture};

    RasterImage image;
    REQUIRE(RenderReferenceTextureLibrary(packet, library, image) ==
        ReferenceRasterError::None);
    // The library has a texture, but this material does not reference it.
    // Sampling it anyway is exactly the bug a shared-fallback frame produces.
    const auto center = image.At(48, 32);
    CHECK_FALSE(center.g > center.r);
}

TEST_CASE("PM_a_texture_index_past_the_library_is_refused_not_guessed")
{
    using namespace vf::renderer::raster;
    DecodedPacket packet;
    REQUIRE(DecodePacket(BuildSyntheticPacket(), packet));
    packet.materials[0].textureIndex = 5;
    const std::array<CapturedTexture, 1> library{
        MakeFallbackTexture(FallbackTextureRole::White)};

    RasterImage image;
    CHECK(RenderReferenceTextureLibrary(packet, library, image) ==
        ReferenceRasterError::UnsupportedState);
}

TEST_CASE("a texture library carries many textures under one contract")
{
    // A scene draws from many materials, and the renderer accepted exactly one
    // texture per frame -- so every surface sampled the same white fallback and
    // the mirrored cell came out flat. Engine textures differ in size and in
    // format, so they cannot share one array image; they travel as a list and
    // are bound through a descriptor array, which is why the count and the
    // per-entry offsets are part of the contract rather than implied.
    const auto makeTexture = [](const std::uint64_t id,
                                const std::uint32_t extent,
                                const std::uint8_t fill) {
        CapturedTexture value{};
        value.resourceId = id;
        value.generation = 1;
        value.dimension = TextureDimension::Texture2D;
        value.width = extent;
        value.height = extent;
        value.depth = 1;
        value.arrayLayers = 1;
        value.mipLevels = 1;
        value.resourceFormat = TextureFormat::R8G8B8A8Unorm;
        value.viewFormat = TextureFormat::R8G8B8A8Unorm;
        value.residentBaseMip = 0;
        value.residentMipCount = 1;
        value.subresources.push_back(SolidRgba(0, 0, extent, extent,
            {fill, fill, fill, 255}));
        return value;
    };

    SECTION("every texture survives the round trip, whatever its size")
    {
        // Different extents on purpose. A library that quietly required one
        // size would work on a fixture built from one texture and fail on the
        // first real cell.
        const std::vector<CapturedTexture> library{
            makeTexture(0x1001, 4, 0x11),
            makeTexture(0x1002, 8, 0x22),
            makeTexture(0x1003, 2, 0x33),
        };
        std::vector<std::byte> bytes;
        REQUIRE(EncodeTextureLibrary(library, bytes) ==
            TexturePacketError::None);

        std::vector<CapturedTexture> decoded;
        REQUIRE(DecodeTextureLibrary(bytes, decoded) ==
            TexturePacketError::None);
        REQUIRE(decoded.size() == library.size());
        for (std::size_t index = 0; index < library.size(); ++index) {
            INFO(index);
            CHECK(decoded[index].resourceId == library[index].resourceId);
            CHECK(decoded[index].width == library[index].width);
            CHECK(decoded[index].height == library[index].height);
            CHECK(decoded[index].viewFormat == library[index].viewFormat);
            REQUIRE(decoded[index].subresources.size() ==
                library[index].subresources.size());
            CHECK(decoded[index].subresources[0].bytes ==
                library[index].subresources[0].bytes);
        }
    }

    SECTION("order is preserved, because a material indexes into it")
    {
        // The index a material carries is a position in this list. If the
        // list came back reordered, every surface would sample some other
        // surface's texture and the frame would look like the materials had
        // been shuffled rather than like the packet had.
        std::vector<CapturedTexture> library{
            makeTexture(0xA001, 2, 0x01),
            makeTexture(0xA002, 2, 0x02),
            makeTexture(0xA003, 2, 0x03),
            makeTexture(0xA004, 2, 0x04),
        };
        std::vector<std::byte> bytes;
        REQUIRE(EncodeTextureLibrary(library, bytes) ==
            TexturePacketError::None);
        std::vector<CapturedTexture> decoded;
        REQUIRE(DecodeTextureLibrary(bytes, decoded) ==
            TexturePacketError::None);
        REQUIRE(decoded.size() == 4);
        for (std::size_t index = 0; index < decoded.size(); ++index) {
            INFO(index);
            CHECK(decoded[index].resourceId == 0xA001 + index);
        }
    }

    SECTION("an empty library is a library, not an error")
    {
        // A frame whose draws reference no texture is ordinary. It must encode
        // and decode to nothing rather than being refused, or the caller would
        // have to special-case the first frame of every load.
        std::vector<std::byte> bytes;
        REQUIRE(EncodeTextureLibrary({}, bytes) ==
            TexturePacketError::None);
        std::vector<CapturedTexture> decoded;
        decoded.emplace_back();
        REQUIRE(DecodeTextureLibrary(bytes, decoded) ==
            TexturePacketError::None);
        CHECK(decoded.empty());
    }

    SECTION("a truncated library is refused rather than half read")
    {
        const std::vector<CapturedTexture> library{
            makeTexture(0x2001, 4, 0x55),
            makeTexture(0x2002, 4, 0x66),
        };
        std::vector<std::byte> bytes;
        REQUIRE(EncodeTextureLibrary(library, bytes) ==
            TexturePacketError::None);
        // Half a library decodes to half a scene's materials, and the missing
        // half would sample whatever the descriptor array happened to hold.
        bytes.resize(bytes.size() / 2);
        std::vector<CapturedTexture> decoded;
        CHECK(DecodeTextureLibrary(bytes, decoded) !=
            TexturePacketError::None);
    }

    SECTION("a library with trailing bytes is refused")
    {
        // Not a truncation -- every entry still fits, so the per-entry bounds
        // check passes. Only the declared total size can notice that the
        // payload is not the payload that was written, and without it a
        // caller could append anything to a valid library and have it read.
        const std::vector<CapturedTexture> library{makeTexture(0x4001, 4, 0x99)};
        std::vector<std::byte> bytes;
        REQUIRE(EncodeTextureLibrary(library, bytes) ==
            TexturePacketError::None);
        bytes.push_back(std::byte{0});
        std::vector<CapturedTexture> decoded;
        CHECK(DecodeTextureLibrary(bytes, decoded) !=
            TexturePacketError::None);
    }

    SECTION("a swapped entry table is caught")
    {
        // The one corruption every other check accepts. Each entry carries its
        // own checksum, so pixel damage is caught when that entry decodes, and
        // a shifted offset lands on a wrong magic. Exchanging two entries
        // leaves both of them decodable, in bounds, and correctly framed --
        // only the order is wrong, and order is what a material index means.
        // Nothing inside an entry can notice; only a checksum over the table.
        const std::vector<CapturedTexture> library{
            makeTexture(0x5001, 4, 0xAA), makeTexture(0x5002, 8, 0xBB)};
        std::vector<std::byte> bytes;
        REQUIRE(EncodeTextureLibrary(library, bytes) ==
            TexturePacketError::None);
        // The table sits immediately after the twenty-four byte header, two
        // eight-byte entries.
        std::array<std::byte, 8> first{};
        std::memcpy(first.data(), bytes.data() + 24, 8);
        std::memcpy(bytes.data() + 24, bytes.data() + 32, 8);
        std::memcpy(bytes.data() + 32, first.data(), 8);
        std::vector<CapturedTexture> decoded;
        CHECK(DecodeTextureLibrary(bytes, decoded) !=
            TexturePacketError::None);
    }

    SECTION("a corrupted entry is caught rather than sampled")
    {
        const std::vector<CapturedTexture> library{
            makeTexture(0x3001, 4, 0x77)};
        std::vector<std::byte> bytes;
        REQUIRE(EncodeTextureLibrary(library, bytes) ==
            TexturePacketError::None);
        // The last pixel byte, which no header field describes. Only a
        // checksum over the payload can notice it.
        bytes.back() = static_cast<std::byte>(
            static_cast<unsigned char>(bytes.back()) ^ 0xFFu);
        std::vector<CapturedTexture> decoded;
        CHECK(DecodeTextureLibrary(bytes, decoded) !=
            TexturePacketError::None);
    }
}
