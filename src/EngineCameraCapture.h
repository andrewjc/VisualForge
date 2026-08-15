#pragma once

#include "renderer_core/CameraStateScan.h"

#include <cstdint>
#include <vector>

namespace vf::engine_camera_capture {

// Every camera the engine currently holds: the state record's embedded one
// followed by each entry of the camera-state cache array. Empty before the
// engine publishes any camera.
[[nodiscard]] std::vector<vf::renderer::camera::CameraScanResult>
ReadLiveCameras(std::uintptr_t imageBase);

// The camera with the largest far plane, which is how the world view is told
// apart from the narrow loading and model-viewer cameras. This is a
// measurement, not a hardcoded slot: observed live, the world view runs to
// ~353,000 units against 15,000 for the others. Returns false when no camera
// has been published yet.
[[nodiscard]] bool SelectWorldCamera(
    const std::vector<vf::renderer::camera::CameraScanResult>& cameras,
    vf::renderer::camera::CameraScanResult& selected) noexcept;

[[nodiscard]] bool Configure(
    bool buildValidated,
    std::uintptr_t imageBase) noexcept;
[[nodiscard]] bool Enabled() noexcept;
[[nodiscard]] bool Arm(const wchar_t* path) noexcept;

// Called once per presented frame with the live swapchain extent. Does
// nothing until a capture is armed.
void OnPresent(std::uint32_t width, std::uint32_t height) noexcept;

}
