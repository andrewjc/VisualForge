#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace vf::renderer_backend_probe {

// Performs the opt-in, process-lifetime Vulkan backend capability handshake.
// Failure is reported but never changes the active D3D11 rendering path.
[[nodiscard]] bool ProbeOnce(ID3D11Device* device) noexcept;
[[nodiscard]] bool PatternRequested() noexcept;
[[nodiscard]] bool InitializePatternBridge(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    unsigned width,
    unsigned height,
    unsigned format) noexcept;
[[nodiscard]] bool CompositePattern(
    IDXGISwapChain* swapchain) noexcept;
[[nodiscard]] bool MirrorRequested() noexcept;
// Renders a scene with the Vulkan backend, driven by the live engine world
// camera, and presents it through the bridge. Returns false when no world
// camera is available yet or the render failed, in which case the frame
// stays vanilla.
[[nodiscard]] bool CompositeMirror(
    IDXGISwapChain* swapchain,
    unsigned long long imageBase) noexcept;
void BeforeResize() noexcept;
void AfterResize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    unsigned width,
    unsigned height,
    unsigned format) noexcept;

}
