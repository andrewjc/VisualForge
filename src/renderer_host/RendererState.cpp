#include "renderer_host/RendererState.h"

namespace vf::renderer {

namespace {

constexpr std::uint64_t kCodeMask = 0xFFu;
constexpr std::uint64_t kSeverityMask = 0xFFu;
constexpr unsigned kSeverityShift = 8;
constexpr unsigned kFrameShift = 16;
constexpr std::uint64_t kFrameMask = (std::uint64_t{1} << 48) - 1;

std::uint64_t PackFault(const FaultEvent& event) noexcept
{
    return (event.frame & kFrameMask) << kFrameShift |
        static_cast<std::uint64_t>(event.severity) << kSeverityShift |
        static_cast<std::uint64_t>(event.code);
}

FaultEvent UnpackFault(const std::uint64_t packed) noexcept
{
    return FaultEvent{
        static_cast<RendererFaultCode>(packed & kCodeMask),
        static_cast<FaultSeverity>((packed >> kSeverityShift) & kSeverityMask),
        packed >> kFrameShift,
    };
}

}

bool IsLegalTransition(const RendererState from, const RendererState to) noexcept
{
    switch (from) {
    case RendererState::Disabled:
        return to == RendererState::Probing;
    case RendererState::Probing:
        return to == RendererState::Observing ||
            to == RendererState::Disabled ||
            to == RendererState::Faulted;
    case RendererState::Observing:
        return to == RendererState::Mirroring ||
            to == RendererState::Draining ||
            to == RendererState::Disabled ||
            to == RendererState::Faulted;
    case RendererState::Mirroring:
        return to == RendererState::Armed ||
            to == RendererState::Draining ||
            to == RendererState::Disabled ||
            to == RendererState::Faulted;
    case RendererState::Armed:
        return to == RendererState::TakingOver ||
            to == RendererState::Draining ||
            to == RendererState::Faulted;
    case RendererState::TakingOver:
        return to == RendererState::Draining ||
            to == RendererState::Faulted;
    case RendererState::Draining:
        return to == RendererState::Disabled ||
            to == RendererState::Faulted;
    case RendererState::Faulted:
        return false;
    }
    return false;
}

const char* ToString(const RendererState state) noexcept
{
    switch (state) {
    case RendererState::Disabled:
        return "Disabled";
    case RendererState::Probing:
        return "Probing";
    case RendererState::Observing:
        return "Observing";
    case RendererState::Mirroring:
        return "Mirroring";
    case RendererState::Armed:
        return "Armed";
    case RendererState::TakingOver:
        return "TakingOver";
    case RendererState::Draining:
        return "Draining";
    case RendererState::Faulted:
        return "Faulted";
    }
    return "Unknown";
}

RendererState RendererStateMachine::Current() const noexcept
{
    return state_.load(std::memory_order_acquire);
}

TransitionResult RendererStateMachine::TryTransition(
    const RendererState expected,
    const RendererState desired) noexcept
{
    auto observed = state_.load(std::memory_order_acquire);
    if (observed != expected) {
        return TransitionResult::StaleExpectedState;
    }
    if (!IsLegalTransition(expected, desired)) {
        return TransitionResult::IllegalTransition;
    }
    if (!state_.compare_exchange_strong(
            observed,
            desired,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return TransitionResult::StaleExpectedState;
    }
    return TransitionResult::Applied;
}

bool FaultController::Report(const FaultEvent& event) noexcept
{
    if (event.code == RendererFaultCode::None || event.frame > kFrameMask) {
        return false;
    }
    auto expected = std::uint64_t{0};
    return packed_.compare_exchange_strong(
        expected,
        PackFault(event),
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

std::optional<FaultEvent> FaultController::Current() const noexcept
{
    const auto packed = packed_.load(std::memory_order_acquire);
    if (packed == 0) {
        return std::nullopt;
    }
    return UnpackFault(packed);
}

bool FaultController::HotReentryAllowed() const noexcept
{
    const auto current = Current();
    return !current || current->severity == FaultSeverity::Recoverable;
}

bool FaultController::ClearRecoverable() noexcept
{
    auto observed = packed_.load(std::memory_order_acquire);
    while (observed != 0) {
        if (UnpackFault(observed).severity != FaultSeverity::Recoverable) {
            return false;
        }
        if (packed_.compare_exchange_weak(
                observed,
                0,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

}
