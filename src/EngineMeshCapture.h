#pragma once

#include <cstdint>

namespace vf::engine_mesh_capture {

[[nodiscard]] bool Configure(
    bool buildValidated,
    std::uintptr_t imageBase) noexcept;
[[nodiscard]] bool Retry() noexcept;
[[nodiscard]] bool Enabled() noexcept;
// Re-arms the installed one-shot capture at a new path and drops any
// snapshots accumulated before the arm, so a live request captures content
// created after it rather than whatever the menu left behind.
[[nodiscard]] bool Arm(const wchar_t* path) noexcept;

}
