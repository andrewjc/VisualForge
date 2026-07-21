#pragma once

#include "renderer_trace/TraceCodec.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace vf::renderer::trace {

enum class FrameCaptureError : std::uint8_t
{
    None,
    InvalidState,
    IdMismatch,
    TraceFailure
};

class FrameCapture
{
public:
    explicit FrameCapture(
        std::size_t maximumBytes = kDefaultMaximumTraceBytes);

    [[nodiscard]] FrameCaptureError Start(const CaptureBegin& begin);
    [[nodiscard]] FrameCaptureError BeginFrame(
        const FrameBegin& begin,
        std::uint64_t correlationId);
    [[nodiscard]] FrameCaptureError SetView(
        const ViewMetadata& view,
        std::uint64_t correlationId);
    [[nodiscard]] FrameCaptureError RecordWriter(
        const WriterEvent& event,
        std::uint64_t cpuCorrelationId);
    [[nodiscard]] FrameCaptureError RecordResize(
        const ResizeEvent& event,
        std::uint64_t correlationId);
    [[nodiscard]] FrameCaptureError EndFrame(
        const FrameEnd& end,
        std::uint64_t correlationId);
    [[nodiscard]] FrameCaptureError Finish(std::uint64_t cpuEnd);

    [[nodiscard]] std::span<const std::byte> Bytes() const noexcept;
    [[nodiscard]] TraceError LastTraceError() const noexcept;

private:
    TraceWriter writer_;
    TraceError lastTraceError_{TraceError::None};
    std::uint64_t captureId_{};
    std::uint64_t currentFrameId_{};
    std::uint64_t frameCount_{};
    std::uint32_t writerCount_{};
    std::uint8_t state_{};
};

}
