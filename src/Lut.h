#pragma once

#include <d3d11.h>

#include <string>
#include <vector>

namespace vf::lut {

// Parses an Adobe/Resolve .cube 3D LUT. On success fills `rgba` with size^3 RGBA float
// entries (alpha = 1, red varying fastest — the Texture3D memory order) and sets `size`.
// Returns false with a reason in `err` for malformed files or 1D LUTs.
bool ParseCube(const wchar_t* path, std::vector<float>& rgba, int& size, std::string& err);

// Parses `fullPath` and uploads it as the active LUT (post::SetLut). Returns false with a
// reason in `err`.
bool LoadInto(ID3D11Device* device, const wchar_t* fullPath, std::string& err);

}
