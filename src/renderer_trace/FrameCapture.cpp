#include "renderer_trace/FrameCapture.h"

namespace vf::renderer::trace {

namespace {

constexpr std::uint8_t kIdle = 0;
constexpr std::uint8_t kCapturing = 1;
constexpr std::uint8_t kInFrame = 2;
constexpr std::uint8_t kComplete = 3;

FrameCaptureError ConvertTraceResult(
    const TraceError error,
    TraceError& destination) noexcept
{
    destination = error;
    return error == TraceError::None
        ? FrameCaptureError::None
        : FrameCaptureError::TraceFailure;
}

}

FrameCapture::FrameCapture(const std::size_t maximumBytes)
    : writer_(maximumBytes)
{}

FrameCaptureError FrameCapture::Start(const CaptureBegin& begin)
{
    if (state_ != kIdle || begin.captureId == 0) {
        return FrameCaptureError::InvalidState;
    }
    const auto result = writer_.Begin(begin);
    if (result != TraceError::None) {
        return ConvertTraceResult(result, lastTraceError_);
    }
    captureId_ = begin.captureId;
    state_ = kCapturing;
    return FrameCaptureError::None;
}

FrameCaptureError FrameCapture::BeginFrame(
    const FrameBegin& begin,
    const std::uint64_t correlationId)
{
    if (state_ != kCapturing || begin.frameId == 0) {
        return FrameCaptureError::InvalidState;
    }
    const auto result = writer_.Write(begin, correlationId);
    if (result != TraceError::None) {
        return ConvertTraceResult(result, lastTraceError_);
    }
    currentFrameId_ = begin.frameId;
    writerCount_ = 0;
    state_ = kInFrame;
    return FrameCaptureError::None;
}

FrameCaptureError FrameCapture::SetView(
    const ViewMetadata& view,
    const std::uint64_t correlationId)
{
    if (state_ != kInFrame) {
        return FrameCaptureError::InvalidState;
    }
    if (view.frameId != currentFrameId_ || view.viewId == 0 ||
        view.swapchainId == 0 || view.width == 0 || view.height == 0) {
        return FrameCaptureError::IdMismatch;
    }
    return ConvertTraceResult(
        writer_.Write(view, correlationId), lastTraceError_);
}

FrameCaptureError FrameCapture::RecordWriter(
    const WriterEvent& event,
    const std::uint64_t cpuCorrelationId)
{
    if (state_ != kInFrame) {
        return FrameCaptureError::InvalidState;
    }
    if (event.frameId != currentFrameId_ || event.writerId == 0 ||
        event.targetId == 0 || event.gpuCorrelationId == 0 ||
        cpuCorrelationId == 0 || event.ordinal != writerCount_) {
        return FrameCaptureError::IdMismatch;
    }
    const auto result = writer_.Write(event, cpuCorrelationId);
    if (result != TraceError::None) {
        return ConvertTraceResult(result, lastTraceError_);
    }
    ++writerCount_;
    return FrameCaptureError::None;
}

FrameCaptureError FrameCapture::RecordResize(
    const ResizeEvent& event,
    const std::uint64_t correlationId)
{
    if (state_ != kCapturing && state_ != kInFrame) {
        return FrameCaptureError::InvalidState;
    }
    if (event.swapchainId == 0 ||
        (state_ == kInFrame && event.frameId != currentFrameId_)) {
        return FrameCaptureError::IdMismatch;
    }
    return ConvertTraceResult(
        writer_.Write(event, correlationId), lastTraceError_);
}

FrameCaptureError FrameCapture::EndFrame(
    const FrameEnd& end,
    const std::uint64_t correlationId)
{
    if (state_ != kInFrame) {
        return FrameCaptureError::InvalidState;
    }
    if (end.frameId != currentFrameId_ || end.writerCount != writerCount_) {
        return FrameCaptureError::IdMismatch;
    }
    const auto result = writer_.Write(end, correlationId);
    if (result != TraceError::None) {
        return ConvertTraceResult(result, lastTraceError_);
    }
    ++frameCount_;
    currentFrameId_ = 0;
    writerCount_ = 0;
    state_ = kCapturing;
    return FrameCaptureError::None;
}

FrameCaptureError FrameCapture::Finish(const std::uint64_t cpuEnd)
{
    if (state_ != kCapturing) {
        return FrameCaptureError::InvalidState;
    }
    const auto result = writer_.Finish({captureId_, frameCount_, cpuEnd});
    if (result != TraceError::None) {
        return ConvertTraceResult(result, lastTraceError_);
    }
    state_ = kComplete;
    return FrameCaptureError::None;
}

std::span<const std::byte> FrameCapture::Bytes() const noexcept
{
    return writer_.Bytes();
}

TraceError FrameCapture::LastTraceError() const noexcept
{
    return lastTraceError_;
}

}
