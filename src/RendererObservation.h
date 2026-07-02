#pragma once

#include "renderer_api/TraceProtocol.h"

#include <cstdint>
#include <string_view>

namespace vf::renderer_observation {

[[nodiscard]] bool Configure(bool buildValidated) noexcept;
[[nodiscard]] bool Enabled() noexcept;
// Starts a fresh trace at a new path. A trace already being published is
// left alone so a live request cannot truncate it.
[[nodiscard]] bool ArmTrace(const wchar_t* path) noexcept;

void OnSwapchainReady(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t format,
    std::uint32_t sampleCount) noexcept;
void OnPresentBegin(
    std::uint32_t syncInterval,
    std::uint32_t presentFlags) noexcept;
void RecordWriter(
    vf::renderer::trace::WriterClassification classification,
    std::string_view canonicalWriterKey) noexcept;
void OnPresentEnd(
    std::int32_t presentResult,
    std::uint32_t syncInterval,
    std::uint32_t presentFlags) noexcept;
void OnResize(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t format,
    std::uint32_t flags) noexcept;

}
