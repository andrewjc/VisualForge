#pragma once

#include <d3d11.h>

namespace vf::overlay {

extern bool g_visible;

// Builds the overlay windows. Must be called between ImGui::NewFrame and ImGui::Render.
void Draw(ID3D11Device* device);

}
