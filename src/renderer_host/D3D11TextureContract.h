#pragma once

#include "renderer_core/EngineTexture.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>

namespace vf::renderer::texture {

[[nodiscard]] TexturePacketError TranslateD3D11Sampler(
    const D3D11_SAMPLER_DESC& source,
    TextureSamplerDesc& destination) noexcept;

}
