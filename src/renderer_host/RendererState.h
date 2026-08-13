#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace vf::renderer {

enum class RendererState : std::uint8_t
{
    Disabled,
    Probing,
    Observing,
    Mirroring,
    Armed,
    TakingOver,
    Draining,
    Faulted
};

enum class TransitionResult : std::uint8_t
{
    Applied,
    StaleExpectedState,
    IllegalTransition
};

[[nodiscard]] bool IsLegalTransition(RendererState from, RendererState to) noexcept;
[[nodiscard]] const char* ToString(RendererState state) noexcept;

class RendererStateMachine
{
public:
    [[nodiscard]] RendererState Current() const noexcept;
    [[nodiscard]] TransitionResult TryTransition(
        RendererState expected,
        RendererState desired) noexcept;

private:
    std::atomic<RendererState> state_{RendererState::Disabled};
};

enum class RendererFaultCode : std::uint8_t
{
    None,
    BuildRejected,
    HookPreparationFailed,
    HookPublicationFailed,
    BackendUnavailable,
    InternalInvariant
};

enum class FaultSeverity : std::uint8_t
{
    Recoverable,
    ProcessTerminal
};

struct FaultEvent
{
    RendererFaultCode code{RendererFaultCode::None};
    FaultSeverity severity{FaultSeverity::Recoverable};
    std::uint64_t frame{};

    friend bool operator==(const FaultEvent&, const FaultEvent&) = default;
};

class FaultController
{
public:
    [[nodiscard]] bool Report(const FaultEvent& event) noexcept;
    [[nodiscard]] std::optional<FaultEvent> Current() const noexcept;
    [[nodiscard]] bool HotReentryAllowed() const noexcept;
    [[nodiscard]] bool ClearRecoverable() noexcept;

private:
    std::atomic<std::uint64_t> packed_{0};
};

}
