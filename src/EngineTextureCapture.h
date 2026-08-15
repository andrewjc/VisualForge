#pragma once

namespace vf::engine_texture_capture {

// Called while MinHook is initialized but before MH_EnableHook(MH_ALL_HOOKS).
// Returns true when capture is not requested or all requested hooks were
// prepared transactionally.
[[nodiscard]] bool PrepareHooks(
    void* createTexture2D,
    void* createShaderResourceView,
    void* psSetShaderResources,
    void* psSetSamplers) noexcept;
[[nodiscard]] bool Enabled() noexcept;
// Re-arms the installed one-shot capture at a new path and drops candidates
// observed before the arm, so a live request captures the content the
// harness asked for.
[[nodiscard]] bool Arm(const wchar_t* path) noexcept;

}
