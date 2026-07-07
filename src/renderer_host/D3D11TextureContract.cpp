#include "renderer_host/D3D11TextureContract.h"

#include <algorithm>

namespace vf::renderer::texture {

TexturePacketError TranslateD3D11Sampler(
    const D3D11_SAMPLER_DESC& source,
    TextureSamplerDesc& destination) noexcept
{
    destination = {};
    const auto filter = static_cast<std::uint32_t>(source.Filter);
    if ((filter & 0x100u) != 0) {
        return TexturePacketError::InvalidSampler;
    }
    const auto comparison = (filter & 0x80u) != 0;
    const auto baseFilter = filter & ~0x80u;
    const auto anisotropic = baseFilter == 0x55u;
    const auto decode = [](const std::uint32_t value,
                           TextureFilter& decoded) {
        if (value > 1) return false;
        decoded = value == 0
            ? TextureFilter::Nearest : TextureFilter::Linear;
        return true;
    };
    if (!anisotropic &&
        (!decode((baseFilter >> 4) & 3u, destination.minFilter) ||
         !decode((baseFilter >> 2) & 3u, destination.magFilter) ||
         !decode(baseFilter & 3u, destination.mipFilter))) {
        return TexturePacketError::InvalidSampler;
    }
    if (anisotropic) {
        destination.minFilter = TextureFilter::Linear;
        destination.magFilter = TextureFilter::Linear;
        destination.mipFilter = TextureFilter::Linear;
        destination.anisotropyEnable = 1;
    }
    const auto translateAddress = [](const D3D11_TEXTURE_ADDRESS_MODE value,
                                     TextureAddressMode& translated) {
        if (value < D3D11_TEXTURE_ADDRESS_WRAP ||
            value > D3D11_TEXTURE_ADDRESS_MIRROR_ONCE) return false;
        translated = static_cast<TextureAddressMode>(
            static_cast<std::uint32_t>(value) - 1u);
        return true;
    };
    if (!translateAddress(source.AddressU, destination.addressU) ||
        !translateAddress(source.AddressV, destination.addressV) ||
        !translateAddress(source.AddressW, destination.addressW) ||
        source.ComparisonFunc < D3D11_COMPARISON_NEVER ||
        source.ComparisonFunc > D3D11_COMPARISON_ALWAYS) {
        return TexturePacketError::InvalidSampler;
    }
    destination.mipLodBias = source.MipLODBias;
    destination.maxAnisotropy = static_cast<float>(
        std::max(1u, source.MaxAnisotropy));
    destination.minLod = source.MinLOD;
    destination.maxLod = source.MaxLOD;
    destination.comparisonEnable = comparison ? 1u : 0u;
    destination.compareOp = static_cast<TextureCompareOp>(
        static_cast<std::uint32_t>(source.ComparisonFunc) - 1u);
    std::copy(std::begin(source.BorderColor),
        std::end(source.BorderColor), std::begin(destination.borderColor));
    TextureSamplerDesc normalized{};
    const auto result = NormalizeSampler(destination, normalized);
    if (result == TexturePacketError::None) destination = normalized;
    return result;
}

}
