#include "renderer_host/D3D11TextureContract.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace vf::renderer::texture;

TEST_CASE("phase8 D3D11 sampler states normalize without semantic loss", "[phase8][texture]")
{
    D3D11_SAMPLER_DESC source{};
    source.Filter = D3D11_FILTER_ANISOTROPIC;
    source.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    source.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    source.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    source.MaxAnisotropy = 32;
    source.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    source.MaxLOD = std::numeric_limits<float>::max();
    TextureSamplerDesc translated{};
    REQUIRE(TranslateD3D11Sampler(source, translated) ==
        TexturePacketError::None);
    CHECK(translated.anisotropyEnable == 1);
    CHECK(translated.maxAnisotropy == 16.0f);
    CHECK(translated.addressU == TextureAddressMode::Wrap);
    CHECK(translated.addressV == TextureAddressMode::Clamp);
    CHECK(translated.addressW == TextureAddressMode::MirrorOnce);
    CHECK(translated.maxLod == 1000.0f);

    source.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    source.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    REQUIRE(TranslateD3D11Sampler(source, translated) ==
        TexturePacketError::None);
    CHECK(translated.comparisonEnable == 1);
    CHECK(translated.compareOp == TextureCompareOp::LessOrEqual);
    CHECK(translated.minFilter == TextureFilter::Linear);
    CHECK(translated.mipFilter == TextureFilter::Nearest);
}
