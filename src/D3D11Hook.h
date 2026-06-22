#pragma once

namespace vf::d3d {

// Hooks IDXGISwapChain::Present / ResizeBuffers via a dummy device's vtable.
// Everything else (ImGui, setting resolution, CAS) initializes lazily on the
// game's first Present.
bool Install();

}
