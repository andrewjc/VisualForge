#pragma once

#include <d3d11.h>

namespace vf::debris {

// Installs the weapon-debris crash fix by hooking ID3D11Device::CreateShaderResourceView.
// On modern (10-series+) GPUs the NVIDIA Flex weapon-debris system asks the device for a
// shader-resource view that the driver rejects; the game doesn't null-check the result and
// dereferences it, crashing. We intercept the failure and hand back a valid 1x1 fallback
// view so the game never sees null. Harmless when debris is off (only failed creations are
// touched). Call once, after the D3D11 device exists.
bool Install(ID3D11Device* device);

void Shutdown();

// Number of failed SRV creations intercepted this session (0 in normal play).
int InterceptedFailures();
bool Installed();

}
