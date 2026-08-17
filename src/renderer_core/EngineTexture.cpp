#include "renderer_core/EngineTexture.h"

#include "renderer_trace/Crc32.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace vf::renderer::texture {

namespace {

struct FormatTraits
{
    TextureFamily family{TextureFamily::Unknown};
    std::uint32_t blockWidth{1};
    std::uint32_t blockHeight{1};
    std::uint32_t bytesPerBlock{};
    bool compressed{};
    bool typeless{};
    bool srgb{};
    bool signedData{};
};

FormatTraits Traits(const TextureFormat format) noexcept
{
    using enum TextureFormat;
    switch (format) {
    case R8Unorm: return {TextureFamily::R8, 1, 1, 1};
    case R8Typeless: return {TextureFamily::R8, 1, 1, 1, false, true};
    case R8G8Unorm: return {TextureFamily::RG8, 1, 1, 2};
    case R8G8Typeless: return {TextureFamily::RG8, 1, 1, 2, false, true};
    case R8G8B8A8Unorm: return {TextureFamily::RGBA8, 1, 1, 4};
    case R8G8B8A8UnormSrgb:
        return {TextureFamily::RGBA8, 1, 1, 4, false, false, true};
    case R8G8B8A8Typeless:
        return {TextureFamily::RGBA8, 1, 1, 4, false, true};
    case B8G8R8A8Unorm: return {TextureFamily::BGRA8, 1, 1, 4};
    case B8G8R8A8UnormSrgb:
        return {TextureFamily::BGRA8, 1, 1, 4, false, false, true};
    case B8G8R8A8Typeless:
        return {TextureFamily::BGRA8, 1, 1, 4, false, true};
    case BC1Unorm: return {TextureFamily::BC1, 4, 4, 8, true};
    case BC1UnormSrgb:
        return {TextureFamily::BC1, 4, 4, 8, true, false, true};
    case BC1Typeless:
        return {TextureFamily::BC1, 4, 4, 8, true, true};
    case BC2Unorm: return {TextureFamily::BC2, 4, 4, 16, true};
    case BC2UnormSrgb:
        return {TextureFamily::BC2, 4, 4, 16, true, false, true};
    case BC2Typeless:
        return {TextureFamily::BC2, 4, 4, 16, true, true};
    case BC3Unorm: return {TextureFamily::BC3, 4, 4, 16, true};
    case BC3UnormSrgb:
        return {TextureFamily::BC3, 4, 4, 16, true, false, true};
    case BC3Typeless:
        return {TextureFamily::BC3, 4, 4, 16, true, true};
    case BC4Unorm: return {TextureFamily::BC4, 4, 4, 8, true};
    case BC4Snorm:
        return {TextureFamily::BC4, 4, 4, 8, true, false, false, true};
    case BC4Typeless:
        return {TextureFamily::BC4, 4, 4, 8, true, true};
    case BC5Unorm: return {TextureFamily::BC5, 4, 4, 16, true};
    case BC5Snorm:
        return {TextureFamily::BC5, 4, 4, 16, true, false, false, true};
    case BC5Typeless:
        return {TextureFamily::BC5, 4, 4, 16, true, true};
    case BC6HUf16: return {TextureFamily::BC6H, 4, 4, 16, true};
    case BC6HSf16:
        return {TextureFamily::BC6H, 4, 4, 16, true, false, false, true};
    case BC6HTypeless:
        return {TextureFamily::BC6H, 4, 4, 16, true, true};
    case BC7Unorm: return {TextureFamily::BC7, 4, 4, 16, true};
    case BC7UnormSrgb:
        return {TextureFamily::BC7, 4, 4, 16, true, false, true};
    case BC7Typeless:
        return {TextureFamily::BC7, 4, 4, 16, true, true};
    case Unknown: break;
    }
    return {};
}

bool IsValidDimension(const CapturedTexture& texture) noexcept
{
    switch (texture.dimension) {
    case TextureDimension::Texture2D:
        return texture.depth == 1 && texture.arrayLayers == 1;
    case TextureDimension::Texture2DArray:
        return texture.depth == 1 && texture.arrayLayers != 0;
    case TextureDimension::Cube:
        return texture.depth == 1 && texture.width == texture.height &&
            texture.arrayLayers >= 6 && texture.arrayLayers % 6 == 0;
    }
    return false;
}

bool IsValidFilter(const TextureFilter value) noexcept
{
    return value == TextureFilter::Nearest || value == TextureFilter::Linear;
}

bool IsValidAddress(const TextureAddressMode value) noexcept
{
    return value >= TextureAddressMode::Wrap &&
        value <= TextureAddressMode::MirrorOnce;
}

bool IsValidCompare(const TextureCompareOp value) noexcept
{
    return value >= TextureCompareOp::Never &&
        value <= TextureCompareOp::Always;
}

float CanonicalZero(const float value) noexcept
{
    return value == 0.0f ? 0.0f : value;
}

std::size_t AlignUp(const std::size_t value, const std::size_t alignment) noexcept
{
    if (alignment == 0 || value >
        std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

std::uint32_t MipExtent(const std::uint32_t base, const std::uint32_t mip) noexcept
{
    return std::max(1u, mip >= 32 ? 0u : base >> mip);
}

TexturePacketError ValidateTexture(
    const CapturedTexture& texture,
    TextureSamplerDesc& normalizedSampler) noexcept
{
    if (texture.resourceId == 0 || texture.generation == 0) {
        return TexturePacketError::InvalidResource;
    }
    if (texture.width == 0 || texture.height == 0 || texture.depth == 0 ||
        texture.width > 16384 || texture.height > 16384 ||
        !IsValidDimension(texture)) {
        return IsValidDimension(texture)
            ? TexturePacketError::InvalidExtent
            : TexturePacketError::InvalidDimension;
    }
    TextureFormatInfo formatInfo{};
    auto result = ResolveTextureFormat(
        texture.resourceFormat, texture.viewFormat, formatInfo);
    if (result != TexturePacketError::None) return result;
    if (texture.mipLevels == 0 || texture.mipLevels > 32 ||
        texture.residentMipCount == 0 ||
        texture.residentBaseMip >= texture.mipLevels ||
        texture.residentMipCount >
            texture.mipLevels - texture.residentBaseMip) {
        return TexturePacketError::InvalidMipRange;
    }
    result = NormalizeSampler(texture.sampler, normalizedSampler);
    if (result != TexturePacketError::None) return result;
    const auto expectedCount64 =
        static_cast<std::uint64_t>(texture.arrayLayers) *
        texture.residentMipCount;
    if (expectedCount64 != texture.subresources.size()) {
        return TexturePacketError::InvalidSubresource;
    }
    std::size_t cursor{};
    for (std::uint32_t layer = 0; layer < texture.arrayLayers; ++layer) {
        for (std::uint32_t localMip = 0;
             localMip < texture.residentMipCount; ++localMip, ++cursor) {
            const auto mip = texture.residentBaseMip + localMip;
            const auto& subresource = texture.subresources[cursor];
            const auto width = MipExtent(texture.width, mip);
            const auto height = MipExtent(texture.height, mip);
            if (subresource.mipLevel != mip ||
                subresource.arrayLayer != layer ||
                subresource.width != width || subresource.height != height) {
                return TexturePacketError::InvalidSubresource;
            }
            TextureFootprint footprint{};
            result = ComputeTextureFootprint(
                texture.viewFormat, width, height, footprint);
            if (result != TexturePacketError::None) return result;
            if (footprint.byteSize > std::numeric_limits<std::uint32_t>::max() ||
                subresource.rowPitch != footprint.rowBytes ||
                subresource.slicePitch != footprint.byteSize ||
                subresource.bytes.size() != footprint.byteSize) {
                return TexturePacketError::InvalidPitch;
            }
        }
    }
    return TexturePacketError::None;
}

std::uint16_t ReadU16(const std::byte* source) noexcept
{
    std::uint16_t value{};
    std::memcpy(&value, source, sizeof(value));
    return value;
}

std::uint32_t ReadU32(const std::byte* source) noexcept
{
    std::uint32_t value{};
    std::memcpy(&value, source, sizeof(value));
    return value;
}

float SrgbToLinear(const float value) noexcept
{
    return value <= 0.04045f
        ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

SampledColor Decode565(const std::uint16_t value) noexcept
{
    return {
        static_cast<float>((value >> 11) & 31u) / 31.0f,
        static_cast<float>((value >> 5) & 63u) / 63.0f,
        static_cast<float>(value & 31u) / 31.0f,
        1.0f,
    };
}

SampledColor Lerp(
    const SampledColor& first,
    const SampledColor& second,
    const float amount) noexcept
{
    return {
        first.r + (second.r - first.r) * amount,
        first.g + (second.g - first.g) * amount,
        first.b + (second.b - first.b) * amount,
        first.a + (second.a - first.a) * amount,
    };
}

SampledColor DecodeBcColor(
    const std::byte* block,
    const std::uint32_t localX,
    const std::uint32_t localY,
    const bool allowTransparent) noexcept
{
    const auto firstValue = ReadU16(block);
    const auto secondValue = ReadU16(block + 2);
    std::array<SampledColor, 4> colors{};
    colors[0] = Decode565(firstValue);
    colors[1] = Decode565(secondValue);
    if (!allowTransparent || firstValue > secondValue) {
        colors[2] = Lerp(colors[0], colors[1], 1.0f / 3.0f);
        colors[3] = Lerp(colors[0], colors[1], 2.0f / 3.0f);
    } else {
        colors[2] = Lerp(colors[0], colors[1], 0.5f);
        colors[3] = {0.0f, 0.0f, 0.0f, 0.0f};
    }
    const auto selectors = ReadU32(block + 4);
    const auto texel = localY * 4 + localX;
    return colors[(selectors >> (texel * 2)) & 3u];
}

float DecodeBcAlpha(
    const std::byte* block,
    const std::uint32_t localX,
    const std::uint32_t localY,
    const bool signedData) noexcept
{
    const auto firstByte = std::to_integer<std::uint8_t>(block[0]);
    const auto secondByte = std::to_integer<std::uint8_t>(block[1]);
    std::array<float, 8> values{};
    if (signedData) {
        const auto first = static_cast<float>(
            static_cast<std::int8_t>(firstByte)) / 127.0f;
        const auto second = static_cast<float>(
            static_cast<std::int8_t>(secondByte)) / 127.0f;
        values[0] = std::max(-1.0f, first);
        values[1] = std::max(-1.0f, second);
    } else {
        values[0] = static_cast<float>(firstByte) / 255.0f;
        values[1] = static_cast<float>(secondByte) / 255.0f;
    }
    if (values[0] > values[1]) {
        for (std::size_t index = 1; index <= 6; ++index) {
            values[index + 1] =
                (values[0] * static_cast<float>(7 - index) +
                 values[1] * static_cast<float>(index)) / 7.0f;
        }
    } else {
        for (std::size_t index = 1; index <= 4; ++index) {
            values[index + 1] =
                (values[0] * static_cast<float>(5 - index) +
                 values[1] * static_cast<float>(index)) / 5.0f;
        }
        values[6] = signedData ? -1.0f : 0.0f;
        values[7] = 1.0f;
    }
    std::uint64_t selectors{};
    for (std::size_t index = 0; index < 6; ++index) {
        selectors |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(block[index + 2])) << (index * 8);
    }
    const auto texel = localY * 4 + localX;
    return values[(selectors >> (texel * 3)) & 7u];
}

float AddressCoordinate(
    const float value,
    const TextureAddressMode mode,
    bool& border) noexcept
{
    switch (mode) {
    case TextureAddressMode::Wrap:
        return value - std::floor(value);
    case TextureAddressMode::Mirror: {
        auto period = std::fmod(value, 2.0f);
        if (period < 0.0f) period += 2.0f;
        return period <= 1.0f ? period : 2.0f - period;
    }
    case TextureAddressMode::Clamp:
        return std::clamp(value, 0.0f, 1.0f);
    case TextureAddressMode::Border:
        if (value < 0.0f || value > 1.0f) border = true;
        return std::clamp(value, 0.0f, 1.0f);
    case TextureAddressMode::MirrorOnce:
        return std::clamp(std::abs(value), 0.0f, 1.0f);
    }
    border = true;
    return 0.0f;
}

const TextureSubresource* FindSubresource(
    const CapturedTexture& texture,
    const std::uint32_t mip,
    const std::uint32_t arrayLayer) noexcept
{
    const auto found = std::find_if(
        texture.subresources.begin(), texture.subresources.end(),
        [mip, arrayLayer](const TextureSubresource& value) {
            return value.arrayLayer == arrayLayer && value.mipLevel == mip;
        });
    return found == texture.subresources.end() ? nullptr : &*found;
}

TexturePacketError DecodeTexel(
    const CapturedTexture& texture,
    const TextureSubresource& subresource,
    const std::uint32_t x,
    const std::uint32_t y,
    SampledColor& color) noexcept
{
    const auto traits = Traits(texture.viewFormat);
    const auto blockX = x / traits.blockWidth;
    const auto blockY = y / traits.blockHeight;
    const auto offset = static_cast<std::size_t>(blockY) *
        subresource.rowPitch + static_cast<std::size_t>(blockX) *
        traits.bytesPerBlock;
    if (offset > subresource.bytes.size() ||
        traits.bytesPerBlock > subresource.bytes.size() - offset) {
        return TexturePacketError::InvalidSubresource;
    }
    const auto* source = subresource.bytes.data() + offset;
    const auto localX = x % traits.blockWidth;
    const auto localY = y % traits.blockHeight;
    switch (traits.family) {
    case TextureFamily::R8:
        color = {static_cast<float>(std::to_integer<std::uint8_t>(source[0])) /
            255.0f, 0.0f, 0.0f, 1.0f};
        break;
    case TextureFamily::RG8:
        color = {
            static_cast<float>(std::to_integer<std::uint8_t>(source[0])) / 255.0f,
            static_cast<float>(std::to_integer<std::uint8_t>(source[1])) / 255.0f,
            0.0f, 1.0f};
        break;
    case TextureFamily::RGBA8:
    case TextureFamily::BGRA8: {
        const auto red = traits.family == TextureFamily::RGBA8 ? 0u : 2u;
        const auto blue = traits.family == TextureFamily::RGBA8 ? 2u : 0u;
        color = {
            static_cast<float>(std::to_integer<std::uint8_t>(source[red])) / 255.0f,
            static_cast<float>(std::to_integer<std::uint8_t>(source[1])) / 255.0f,
            static_cast<float>(std::to_integer<std::uint8_t>(source[blue])) / 255.0f,
            static_cast<float>(std::to_integer<std::uint8_t>(source[3])) / 255.0f};
        break;
    }
    case TextureFamily::BC1:
        color = DecodeBcColor(source, localX, localY, true);
        break;
    case TextureFamily::BC2: {
        color = DecodeBcColor(source + 8, localX, localY, false);
        const auto alphaBits = ReadU16(source + localY * 2);
        color.a = static_cast<float>((alphaBits >> (localX * 4)) & 15u) / 15.0f;
        break;
    }
    case TextureFamily::BC3:
        color = DecodeBcColor(source + 8, localX, localY, false);
        color.a = DecodeBcAlpha(source, localX, localY, false);
        break;
    case TextureFamily::BC4:
        color = {DecodeBcAlpha(source, localX, localY, traits.signedData),
            0.0f, 0.0f, 1.0f};
        break;
    case TextureFamily::BC5:
        color = {
            DecodeBcAlpha(source, localX, localY, traits.signedData),
            DecodeBcAlpha(source + 8, localX, localY, traits.signedData),
            0.0f, 1.0f};
        break;
    case TextureFamily::BC6H:
    case TextureFamily::BC7:
    case TextureFamily::Unknown:
        return TexturePacketError::UnsupportedSampling;
    }
    if (traits.srgb) {
        color.r = SrgbToLinear(color.r);
        color.g = SrgbToLinear(color.g);
        color.b = SrgbToLinear(color.b);
    }
    return TexturePacketError::None;
}

TexturePacketError SampleMip(
    const CapturedTexture& texture,
    const std::uint32_t mip,
    const std::uint32_t arrayLayer,
    float u,
    float v,
    const TextureFilter filter,
    SampledColor& color) noexcept
{
    const auto* subresource = FindSubresource(texture, mip, arrayLayer);
    if (subresource == nullptr) return TexturePacketError::InvalidSubresource;
    bool border{};
    u = AddressCoordinate(u, texture.sampler.addressU, border);
    v = AddressCoordinate(v, texture.sampler.addressV, border);
    if (border) {
        color = {texture.sampler.borderColor[0],
            texture.sampler.borderColor[1], texture.sampler.borderColor[2],
            texture.sampler.borderColor[3]};
        return TexturePacketError::None;
    }
    const auto width = subresource->width;
    const auto height = subresource->height;
    if (filter == TextureFilter::Nearest || width == 1 || height == 1) {
        const auto x = std::min(
            static_cast<std::uint32_t>(u * static_cast<float>(width)),
            width - 1);
        const auto y = std::min(
            static_cast<std::uint32_t>(v * static_cast<float>(height)),
            height - 1);
        return DecodeTexel(texture, *subresource, x, y, color);
    }
    const auto x = u * static_cast<float>(width) - 0.5f;
    const auto y = v * static_cast<float>(height) - 0.5f;
    const auto x0 = static_cast<std::int32_t>(std::floor(x));
    const auto y0 = static_cast<std::int32_t>(std::floor(y));
    const auto fetch = [&](const std::int32_t inputX, const std::int32_t inputY,
                           SampledColor& output) {
        bool texelBorder{};
        auto texelU = (static_cast<float>(inputX) + 0.5f) /
            static_cast<float>(width);
        auto texelV = (static_cast<float>(inputY) + 0.5f) /
            static_cast<float>(height);
        texelU = AddressCoordinate(texelU, texture.sampler.addressU, texelBorder);
        texelV = AddressCoordinate(texelV, texture.sampler.addressV, texelBorder);
        if (texelBorder) {
            output = {texture.sampler.borderColor[0],
                texture.sampler.borderColor[1], texture.sampler.borderColor[2],
                texture.sampler.borderColor[3]};
            return TexturePacketError::None;
        }
        const auto sampleX = std::min(
            static_cast<std::uint32_t>(texelU * width), width - 1);
        const auto sampleY = std::min(
            static_cast<std::uint32_t>(texelV * height), height - 1);
        return DecodeTexel(texture, *subresource, sampleX, sampleY, output);
    };
    std::array<SampledColor, 4> samples{};
    auto result = fetch(x0, y0, samples[0]);
    if (result != TexturePacketError::None) return result;
    result = fetch(x0 + 1, y0, samples[1]);
    if (result != TexturePacketError::None) return result;
    result = fetch(x0, y0 + 1, samples[2]);
    if (result != TexturePacketError::None) return result;
    result = fetch(x0 + 1, y0 + 1, samples[3]);
    if (result != TexturePacketError::None) return result;
    const auto horizontal0 = Lerp(samples[0], samples[1], x - std::floor(x));
    const auto horizontal1 = Lerp(samples[2], samples[3], x - std::floor(x));
    color = Lerp(horizontal0, horizontal1, y - std::floor(y));
    return TexturePacketError::None;
}

TexturePacketError SampleTextureLayer(
    const CapturedTexture& texture,
    const float u,
    const float v,
    const std::uint32_t arrayLayer,
    const float lod,
    SampledColor& color) noexcept
{
    color = {};
    if (!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(lod)) {
        return TexturePacketError::UnsupportedSampling;
    }
    TextureSamplerDesc normalized{};
    const auto validated = ValidateTexture(texture, normalized);
    if (validated != TexturePacketError::None) return validated;
    if (arrayLayer >= texture.arrayLayers) {
        return TexturePacketError::InvalidSubresource;
    }
    const auto relativeLod = std::clamp(
        lod + normalized.mipLodBias, normalized.minLod,
        std::min(normalized.maxLod,
            static_cast<float>(texture.residentMipCount - 1)));
    const auto firstLocal = static_cast<std::uint32_t>(
        std::floor(relativeLod));
    const auto secondLocal = std::min(
        firstLocal + 1, texture.residentMipCount - 1);
    const auto filter = relativeLod <= 0.0f
        ? normalized.magFilter : normalized.minFilter;
    auto result = SampleMip(texture,
        texture.residentBaseMip + firstLocal, arrayLayer, u, v,
        filter, color);
    if (result != TexturePacketError::None ||
        normalized.mipFilter == TextureFilter::Nearest ||
        firstLocal == secondLocal) {
        return result;
    }
    SampledColor second{};
    result = SampleMip(texture,
        texture.residentBaseMip + secondLocal, arrayLayer, u, v,
        filter, second);
    if (result == TexturePacketError::None) {
        color = Lerp(color, second, relativeLod - std::floor(relativeLod));
    }
    return result;
}

}

TexturePacketError ResolveTextureFormat(
    const TextureFormat resourceFormat,
    const TextureFormat viewFormat,
    TextureFormatInfo& info) noexcept
{
    info = {};
    const auto resource = Traits(resourceFormat);
    const auto view = Traits(viewFormat);
    if (resource.family == TextureFamily::Unknown ||
        view.family == TextureFamily::Unknown) {
        return TexturePacketError::UnsupportedFormat;
    }
    if (view.typeless || resource.family != view.family ||
        (!resource.typeless && resourceFormat != viewFormat)) {
        return TexturePacketError::IllegalViewFormat;
    }
    info = {view.family, view.blockWidth, view.blockHeight,
        view.bytesPerBlock, view.compressed, view.srgb, view.signedData};
    return TexturePacketError::None;
}

TexturePacketError ComputeTextureFootprint(
    const TextureFormat viewFormat,
    const std::uint32_t width,
    const std::uint32_t height,
    TextureFootprint& footprint) noexcept
{
    footprint = {};
    if (width == 0 || height == 0) return TexturePacketError::InvalidExtent;
    const auto traits = Traits(viewFormat);
    if (traits.family == TextureFamily::Unknown) {
        return TexturePacketError::UnsupportedFormat;
    }
    const auto blocksWide =
        (static_cast<std::uint64_t>(width) + traits.blockWidth - 1) /
        traits.blockWidth;
    const auto blocksHigh =
        (static_cast<std::uint64_t>(height) + traits.blockHeight - 1) /
        traits.blockHeight;
    const auto rowBytes = blocksWide * traits.bytesPerBlock;
    const auto byteSize = rowBytes * blocksHigh;
    if (rowBytes > std::numeric_limits<std::uint32_t>::max() ||
        blocksHigh > std::numeric_limits<std::uint32_t>::max()) {
        return TexturePacketError::InvalidExtent;
    }
    footprint.rowBytes = static_cast<std::uint32_t>(rowBytes);
    footprint.rowCount = static_cast<std::uint32_t>(blocksHigh);
    footprint.byteSize = byteSize;
    return TexturePacketError::None;
}

TexturePacketError NormalizeSampler(
    const TextureSamplerDesc& input,
    TextureSamplerDesc& normalized) noexcept
{
    normalized = {};
    if (!IsValidFilter(input.minFilter) ||
        !IsValidFilter(input.magFilter) ||
        !IsValidFilter(input.mipFilter) ||
        !IsValidAddress(input.addressU) ||
        !IsValidAddress(input.addressV) ||
        !IsValidAddress(input.addressW) ||
        !IsValidCompare(input.compareOp) ||
        !std::isfinite(input.mipLodBias) ||
        !std::isfinite(input.maxAnisotropy) ||
        !std::isfinite(input.minLod) || !std::isfinite(input.maxLod) ||
        input.minLod > input.maxLod ||
        !std::all_of(std::begin(input.borderColor),
            std::end(input.borderColor), [](const float value) {
                return std::isfinite(value);
            })) {
        return TexturePacketError::InvalidSampler;
    }
    normalized = input;
    normalized.anisotropyEnable = input.anisotropyEnable != 0 ? 1u : 0u;
    normalized.comparisonEnable = input.comparisonEnable != 0 ? 1u : 0u;
    normalized.mipLodBias = CanonicalZero(
        std::clamp(input.mipLodBias, -16.0f, 15.996f));
    normalized.minLod = CanonicalZero(std::max(0.0f, input.minLod));
    normalized.maxLod = CanonicalZero(std::max(
        normalized.minLod, std::min(input.maxLod, 1000.0f)));
    if (normalized.anisotropyEnable != 0) {
        normalized.maxAnisotropy = CanonicalZero(
            std::clamp(input.maxAnisotropy, 1.0f, 16.0f));
        normalized.minFilter = TextureFilter::Linear;
        normalized.magFilter = TextureFilter::Linear;
        normalized.mipFilter = TextureFilter::Linear;
    } else {
        normalized.maxAnisotropy = 1.0f;
    }
    if (normalized.comparisonEnable == 0) {
        normalized.compareOp = TextureCompareOp::Always;
    }
    normalized.reserved = 0;
    for (auto& value : normalized.borderColor) {
        value = CanonicalZero(std::clamp(value, 0.0f, 1.0f));
    }
    return TexturePacketError::None;
}

TexturePacketError RepackTextureRows(
    const TextureFormat viewFormat,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t sourceRowPitch,
    const std::span<const std::byte> source,
    std::vector<std::byte>& tightlyPacked) noexcept
{
    tightlyPacked.clear();
    TextureFootprint footprint{};
    const auto result = ComputeTextureFootprint(
        viewFormat, width, height, footprint);
    if (result != TexturePacketError::None) return result;
    const auto requiredSource = static_cast<std::uint64_t>(sourceRowPitch) *
        footprint.rowCount;
    if (sourceRowPitch < footprint.rowBytes ||
        requiredSource > source.size() ||
        footprint.byteSize > std::numeric_limits<std::size_t>::max()) {
        return TexturePacketError::InvalidPitch;
    }
    try {
        tightlyPacked.resize(static_cast<std::size_t>(footprint.byteSize));
        for (std::uint32_t row = 0; row < footprint.rowCount; ++row) {
            std::memcpy(tightlyPacked.data() +
                    static_cast<std::size_t>(row) * footprint.rowBytes,
                source.data() +
                    static_cast<std::size_t>(row) * sourceRowPitch,
                footprint.rowBytes);
        }
        return TexturePacketError::None;
    } catch (...) {
        tightlyPacked.clear();
        return TexturePacketError::AllocationFailure;
    }
}

namespace {

// One library entry: where its encoded texture starts and how long it is.
// Offsets are stored rather than implied by walking the entries, so a
// truncated payload is caught by arithmetic instead of by decoding into it.
struct LibraryEntry
{
    std::uint32_t offset{};
    std::uint32_t size{};
};

struct alignas(8) LibraryHeader
{
    std::uint32_t magic{kTextureLibraryMagic};
    std::uint16_t versionMajor{kTextureLibraryVersionMajor};
    std::uint16_t versionMinor{kTextureLibraryVersionMinor};
    std::uint32_t headerSize{sizeof(LibraryHeader)};
    std::uint32_t count{};
    std::uint32_t totalSize{};
    std::uint32_t payloadCrc32{};
};

}

namespace {

// One implementation for both overloads. `fetch` returns the texture at an
// index, or null when the caller handed in a hole.
template <typename Fetch>
TexturePacketError EncodeLibraryImpl(
    const std::size_t count,
    Fetch fetch,
    std::vector<std::byte>& bytes) noexcept
{
    try {
        bytes.clear();
        // Each texture is encoded by the single-texture encoder, so a library
        // entry and a standalone packet are the same bytes. One format, one
        // set of validation rules, and a library entry can be handed to
        // anything that already reads a texture.
        std::vector<std::vector<std::byte>> encoded;
        encoded.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto* const texture = fetch(index);
            if (texture == nullptr) return TexturePacketError::InvalidResource;
            std::vector<std::byte> one;
            const auto result = EncodeCapturedTexture(*texture, one);
            if (result != TexturePacketError::None) return result;
            encoded.push_back(std::move(one));
        }

        const auto tableBytes = sizeof(LibraryEntry) * encoded.size();
        auto payloadBytes = std::size_t{0};
        for (const auto& one : encoded) payloadBytes += one.size();
        const auto total = sizeof(LibraryHeader) + tableBytes + payloadBytes;
        if (total > std::numeric_limits<std::uint32_t>::max()) {
            return TexturePacketError::SectionOutOfBounds;
        }

        bytes.resize(total);
        auto offset = sizeof(LibraryHeader) + tableBytes;
        std::vector<LibraryEntry> table(encoded.size());
        for (std::size_t index = 0; index < encoded.size(); ++index) {
            table[index].offset = static_cast<std::uint32_t>(offset);
            table[index].size =
                static_cast<std::uint32_t>(encoded[index].size());
            std::memcpy(bytes.data() + offset, encoded[index].data(),
                encoded[index].size());
            offset += encoded[index].size();
        }
        if (!table.empty()) {
            std::memcpy(bytes.data() + sizeof(LibraryHeader), table.data(),
                tableBytes);
        }

        LibraryHeader header{};
        header.count = static_cast<std::uint32_t>(encoded.size());
        header.totalSize = static_cast<std::uint32_t>(total);
        // Over the table and the payload together, so a corrupted entry is
        // caught even where no header field describes the bytes.
        header.payloadCrc32 = vf::renderer::trace::Crc32(
            std::span{bytes.data() + sizeof(LibraryHeader),
                total - sizeof(LibraryHeader)});
        std::memcpy(bytes.data(), &header, sizeof(header));
        return TexturePacketError::None;
    } catch (...) {
        bytes.clear();
        return TexturePacketError::AllocationFailure;
    }
}

}

TexturePacketError EncodeTextureLibrary(
    const std::span<const CapturedTexture> textures,
    std::vector<std::byte>& bytes) noexcept
{
    return EncodeLibraryImpl(textures.size(),
        [textures](const std::size_t index) { return &textures[index]; },
        bytes);
}

TexturePacketError EncodeTextureLibrary(
    const std::span<const CapturedTexture* const> textures,
    std::vector<std::byte>& bytes) noexcept
{
    return EncodeLibraryImpl(textures.size(),
        [textures](const std::size_t index) { return textures[index]; },
        bytes);
}

TexturePacketError DecodeTextureLibrary(
    const std::span<const std::byte> bytes,
    std::vector<CapturedTexture>& textures) noexcept
{
    textures.clear();
    if (bytes.size() < sizeof(LibraryHeader)) {
        return TexturePacketError::TruncatedHeader;
    }
    LibraryHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kTextureLibraryMagic ||
        header.versionMajor != kTextureLibraryVersionMajor ||
        header.headerSize != sizeof(LibraryHeader)) {
        return TexturePacketError::UnsupportedVersion;
    }
    // An early-out, not an independent guard: the checksum below covers the
    // same bytes and catches any length change too. This is here so a wrong
    // length is rejected before hashing a payload that may be megabytes.
    if (header.totalSize != bytes.size()) {
        return TexturePacketError::SectionOutOfBounds;
    }
    // The count is bounded by the payload before it is multiplied, so a
    // declared count of four billion cannot overflow into a small table size
    // and pass the bounds check it was supposed to fail.
    if (header.count > bytes.size() / sizeof(LibraryEntry)) {
        return TexturePacketError::SectionOutOfBounds;
    }
    const auto tableBytes =
        static_cast<std::size_t>(header.count) * sizeof(LibraryEntry);
    if (sizeof(LibraryHeader) + tableBytes > bytes.size()) {
        return TexturePacketError::SectionOutOfBounds;
    }
    const auto computed = vf::renderer::trace::Crc32(
        std::span{bytes.data() + sizeof(LibraryHeader),
            bytes.size() - sizeof(LibraryHeader)});
    if (computed != header.payloadCrc32) {
        return TexturePacketError::ChecksumMismatch;
    }

    try {
        textures.reserve(header.count);
        for (std::uint32_t index = 0; index < header.count; ++index) {
            LibraryEntry entry{};
            std::memcpy(&entry,
                bytes.data() + sizeof(LibraryHeader) +
                    index * sizeof(LibraryEntry),
                sizeof(entry));
            // Both are thirty-two bit and the sum is computed in size_t, which
            // is wider, so the addition cannot wrap.
            const auto end = static_cast<std::size_t>(entry.offset) +
                static_cast<std::size_t>(entry.size);
            if (end > bytes.size() ||
                entry.offset < sizeof(LibraryHeader) + tableBytes) {
                return TexturePacketError::SectionOutOfBounds;
            }
            CapturedTexture decoded{};
            const auto result = DecodeCapturedTexture(
                bytes.subspan(entry.offset, entry.size), decoded);
            if (result != TexturePacketError::None) return result;
            textures.push_back(std::move(decoded));
        }
    } catch (...) {
        textures.clear();
        return TexturePacketError::AllocationFailure;
    }
    return TexturePacketError::None;
}

TexturePacketError EncodeCapturedTexture(
    const CapturedTexture& texture,
    std::vector<std::byte>& bytes) noexcept
{
    bytes.clear();
    TextureSamplerDesc normalizedSampler{};
    const auto validated = ValidateTexture(texture, normalizedSampler);
    if (validated != TexturePacketError::None) return validated;
    try {
        if (texture.subresources.size() >
            std::numeric_limits<std::uint32_t>::max()) {
            return TexturePacketError::AllocationFailure;
        }
        const auto tableBytes = texture.subresources.size() *
            sizeof(TextureSubresourceV1);
        const auto tableOffset = AlignUp(sizeof(TexturePacketHeaderV1), 8);
        const auto payloadOffset = AlignUp(tableOffset + tableBytes, 8);
        if (tableOffset == std::numeric_limits<std::size_t>::max() ||
            payloadOffset == std::numeric_limits<std::size_t>::max() ||
            payloadOffset > std::numeric_limits<std::uint32_t>::max()) {
            return TexturePacketError::AllocationFailure;
        }
        std::size_t payloadSize{};
        for (const auto& subresource : texture.subresources) {
            if (subresource.bytes.size() >
                std::numeric_limits<std::uint32_t>::max() - payloadSize) {
                return TexturePacketError::AllocationFailure;
            }
            payloadSize += subresource.bytes.size();
        }
        if (payloadSize > std::numeric_limits<std::uint32_t>::max() ||
            payloadOffset > std::numeric_limits<std::size_t>::max() - payloadSize ||
            payloadOffset + payloadSize >
                std::numeric_limits<std::uint32_t>::max()) {
            return TexturePacketError::AllocationFailure;
        }
        bytes.resize(payloadOffset + payloadSize);
        auto* records = reinterpret_cast<TextureSubresourceV1*>(
            bytes.data() + tableOffset);
        std::size_t payloadCursor{};
        for (std::size_t index = 0;
             index < texture.subresources.size(); ++index) {
            const auto& source = texture.subresources[index];
            records[index] = {
                source.mipLevel, source.arrayLayer, source.width, source.height,
                source.rowPitch, source.slicePitch,
                static_cast<std::uint32_t>(payloadOffset + payloadCursor),
                static_cast<std::uint32_t>(source.bytes.size()),
            };
            std::memcpy(bytes.data() + payloadOffset + payloadCursor,
                source.bytes.data(), source.bytes.size());
            payloadCursor += source.bytes.size();
        }
        TexturePacketHeaderV1 header{};
        header.totalSize = static_cast<std::uint32_t>(bytes.size());
        header.resourceId = texture.resourceId;
        header.generation = texture.generation;
        header.dimension = texture.dimension;
        header.width = texture.width;
        header.height = texture.height;
        header.depth = texture.depth;
        header.arrayLayers = texture.arrayLayers;
        header.mipLevels = texture.mipLevels;
        header.resourceFormat = texture.resourceFormat;
        header.viewFormat = texture.viewFormat;
        header.residentBaseMip = texture.residentBaseMip;
        header.residentMipCount = texture.residentMipCount;
        header.subresourceCount =
            static_cast<std::uint32_t>(texture.subresources.size());
        header.subresourcesOffset = static_cast<std::uint32_t>(tableOffset);
        header.payloadOffset = static_cast<std::uint32_t>(payloadOffset);
        header.payloadSize = static_cast<std::uint32_t>(payloadSize);
        header.payloadCrc32 = vf::renderer::trace::Crc32(
            std::span<const std::byte>{bytes}.subspan(payloadOffset, payloadSize));
        header.sampler = normalizedSampler;
        std::memcpy(bytes.data(), &header, sizeof(header));
        return TexturePacketError::None;
    } catch (...) {
        bytes.clear();
        return TexturePacketError::AllocationFailure;
    }
}

TexturePacketError DecodeCapturedTexture(
    const std::span<const std::byte> bytes,
    CapturedTexture& texture) noexcept
{
    texture = {};
    if (bytes.size() < sizeof(TexturePacketHeaderV1)) {
        return TexturePacketError::TruncatedHeader;
    }
    TexturePacketHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kTexturePacketMagic) return TexturePacketError::BadMagic;
    if (header.versionMajor != kTexturePacketVersionMajor ||
        header.versionMinor > kTexturePacketVersionMinor) {
        return TexturePacketError::UnsupportedVersion;
    }
    if (header.headerSize != sizeof(TexturePacketHeaderV1) ||
        header.totalSize != bytes.size()) {
        return header.totalSize != bytes.size()
            ? TexturePacketError::SizeMismatch
            : TexturePacketError::TruncatedHeader;
    }
    if (header.subresourcesOffset % alignof(TextureSubresourceV1) != 0 ||
        header.payloadOffset % 8 != 0) {
        return TexturePacketError::MisalignedSection;
    }
    const auto tableSize64 = static_cast<std::uint64_t>(
        header.subresourceCount) * sizeof(TextureSubresourceV1);
    if (tableSize64 > bytes.size() ||
        header.subresourcesOffset < header.headerSize ||
        header.subresourcesOffset > bytes.size() ||
        tableSize64 > bytes.size() - header.subresourcesOffset ||
        header.payloadOffset < header.subresourcesOffset + tableSize64 ||
        header.payloadOffset > bytes.size() ||
        header.payloadSize > bytes.size() - header.payloadOffset ||
        header.payloadOffset + header.payloadSize != bytes.size()) {
        return TexturePacketError::SectionOutOfBounds;
    }
    const auto payload = bytes.subspan(header.payloadOffset, header.payloadSize);
    if (vf::renderer::trace::Crc32(payload) != header.payloadCrc32) {
        return TexturePacketError::ChecksumMismatch;
    }
    try {
        CapturedTexture candidate{};
        candidate.resourceId = header.resourceId;
        candidate.generation = header.generation;
        candidate.dimension = header.dimension;
        candidate.width = header.width;
        candidate.height = header.height;
        candidate.depth = header.depth;
        candidate.arrayLayers = header.arrayLayers;
        candidate.mipLevels = header.mipLevels;
        candidate.resourceFormat = header.resourceFormat;
        candidate.viewFormat = header.viewFormat;
        candidate.residentBaseMip = header.residentBaseMip;
        candidate.residentMipCount = header.residentMipCount;
        candidate.sampler = header.sampler;
        candidate.subresources.resize(header.subresourceCount);
        std::size_t expectedPayload = header.payloadOffset;
        for (std::size_t index = 0; index < header.subresourceCount; ++index) {
            TextureSubresourceV1 record{};
            std::memcpy(&record,
                bytes.data() + header.subresourcesOffset +
                    index * sizeof(record), sizeof(record));
            if (record.payloadOffset != expectedPayload ||
                record.payloadOffset < header.payloadOffset ||
                record.payloadOffset > bytes.size() ||
                record.payloadSize > bytes.size() - record.payloadOffset) {
                return TexturePacketError::SectionOutOfBounds;
            }
            auto& destination = candidate.subresources[index];
            destination.mipLevel = record.mipLevel;
            destination.arrayLayer = record.arrayLayer;
            destination.width = record.width;
            destination.height = record.height;
            destination.rowPitch = record.rowPitch;
            destination.slicePitch = record.slicePitch;
            destination.bytes.assign(
                bytes.begin() + record.payloadOffset,
                bytes.begin() + record.payloadOffset + record.payloadSize);
            expectedPayload += record.payloadSize;
        }
        if (expectedPayload != bytes.size()) {
            return TexturePacketError::SectionOutOfBounds;
        }
        TextureSamplerDesc normalized{};
        const auto validated = ValidateTexture(candidate, normalized);
        if (validated != TexturePacketError::None) return validated;
        if (!(normalized == candidate.sampler)) {
            return TexturePacketError::InvalidSampler;
        }
        texture = std::move(candidate);
        return TexturePacketError::None;
    } catch (...) {
        texture = {};
        return TexturePacketError::AllocationFailure;
    }
}

TexturePacketError SampleTexture2D(
    const CapturedTexture& texture,
    const float u,
    const float v,
    const float lod,
    SampledColor& color) noexcept
{
    if (texture.dimension != TextureDimension::Texture2D) {
        return TexturePacketError::UnsupportedSampling;
    }
    return SampleTextureLayer(texture, u, v, 0, lod, color);
}

TexturePacketError SampleTexture2DArray(
    const CapturedTexture& texture,
    const float u,
    const float v,
    const std::uint32_t arrayLayer,
    const float lod,
    SampledColor& color) noexcept
{
    if (texture.dimension != TextureDimension::Texture2DArray) {
        return TexturePacketError::UnsupportedSampling;
    }
    return SampleTextureLayer(texture, u, v, arrayLayer, lod, color);
}

TexturePacketError SampleTextureCube(
    const CapturedTexture& texture,
    const float directionX,
    const float directionY,
    const float directionZ,
    const float lod,
    SampledColor& color) noexcept
{
    color = {};
    if (texture.dimension != TextureDimension::Cube ||
        !std::isfinite(directionX) || !std::isfinite(directionY) ||
        !std::isfinite(directionZ) || !std::isfinite(lod)) {
        return TexturePacketError::UnsupportedSampling;
    }
    const auto absoluteX = std::abs(directionX);
    const auto absoluteY = std::abs(directionY);
    const auto absoluteZ = std::abs(directionZ);
    const auto major = std::max({absoluteX, absoluteY, absoluteZ});
    if (major == 0.0f) {
        return TexturePacketError::UnsupportedSampling;
    }

    std::uint32_t face{};
    float faceU{};
    float faceV{};
    if (absoluteX >= absoluteY && absoluteX >= absoluteZ) {
        if (directionX >= 0.0f) {
            face = 0; // +X
            faceU = -directionZ;
            faceV = -directionY;
        } else {
            face = 1; // -X
            faceU = directionZ;
            faceV = -directionY;
        }
    } else if (absoluteY >= absoluteZ) {
        if (directionY >= 0.0f) {
            face = 2; // +Y
            faceU = directionX;
            faceV = directionZ;
        } else {
            face = 3; // -Y
            faceU = directionX;
            faceV = -directionZ;
        }
    } else if (directionZ >= 0.0f) {
        face = 4; // +Z
        faceU = directionX;
        faceV = -directionY;
    } else {
        face = 5; // -Z
        faceU = -directionX;
        faceV = -directionY;
    }
    const auto u = 0.5f * (faceU / major + 1.0f);
    const auto v = 0.5f * (faceV / major + 1.0f);
    return SampleTextureLayer(texture, u, v, face, lod, color);
}

CapturedTexture MakeFallbackTexture(const FallbackTextureRole role)
{
    CapturedTexture texture{};
    texture.resourceId = 0x8000'0000'0000'0001ull +
        static_cast<std::uint64_t>(role);
    texture.generation = 1;
    texture.width = 1;
    texture.height = 1;
    texture.resourceFormat = TextureFormat::R8G8B8A8Unorm;
    texture.viewFormat = TextureFormat::R8G8B8A8Unorm;
    texture.sampler.minFilter = TextureFilter::Nearest;
    texture.sampler.magFilter = TextureFilter::Nearest;
    texture.sampler.mipFilter = TextureFilter::Nearest;
    texture.sampler.addressU = TextureAddressMode::Clamp;
    texture.sampler.addressV = TextureAddressMode::Clamp;
    texture.sampler.addressW = TextureAddressMode::Clamp;
    texture.sampler.maxLod = 0.0f;
    std::array<std::uint8_t, 4> value{};
    switch (role) {
    case FallbackTextureRole::White: value = {255, 255, 255, 255}; break;
    case FallbackTextureRole::Black: value = {0, 0, 0, 255}; break;
    case FallbackTextureRole::FlatNormal: value = {128, 128, 255, 255}; break;
    case FallbackTextureRole::NeutralMask: value = {255, 255, 255, 255}; break;
    }
    TextureSubresource subresource{};
    subresource.width = 1;
    subresource.height = 1;
    subresource.rowPitch = 4;
    subresource.slicePitch = 4;
    subresource.bytes.resize(4);
    std::memcpy(subresource.bytes.data(), value.data(), value.size());
    texture.subresources.push_back(std::move(subresource));
    return texture;
}

const char* ToString(const TexturePacketError error) noexcept
{
    switch (error) {
    case TexturePacketError::None: return "none";
    case TexturePacketError::TruncatedHeader: return "truncated-header";
    case TexturePacketError::BadMagic: return "bad-magic";
    case TexturePacketError::UnsupportedVersion: return "unsupported-version";
    case TexturePacketError::SizeMismatch: return "size-mismatch";
    case TexturePacketError::InvalidResource: return "invalid-resource";
    case TexturePacketError::UnsupportedFormat: return "unsupported-format";
    case TexturePacketError::IllegalViewFormat: return "illegal-view-format";
    case TexturePacketError::InvalidDimension: return "invalid-dimension";
    case TexturePacketError::InvalidExtent: return "invalid-extent";
    case TexturePacketError::InvalidMipRange: return "invalid-mip-range";
    case TexturePacketError::InvalidSampler: return "invalid-sampler";
    case TexturePacketError::InvalidSubresource: return "invalid-subresource";
    case TexturePacketError::InvalidPitch: return "invalid-pitch";
    case TexturePacketError::MisalignedSection: return "misaligned-section";
    case TexturePacketError::SectionOutOfBounds: return "section-out-of-bounds";
    case TexturePacketError::ChecksumMismatch: return "checksum-mismatch";
    case TexturePacketError::AllocationFailure: return "allocation-failure";
    case TexturePacketError::UnsupportedSampling: return "unsupported-sampling";
    }
    return "unknown";
}

}
