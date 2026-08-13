#include "renderer_host/RendererBootstrap.h"

#include "renderer_host/RendererHealth.h"

namespace vf::renderer {

namespace {

RendererBootstrapResult StateFailure(
    RendererStateMachine& machine,
    FaultController& faults,
    const bool reachedProbing) noexcept
{
    static_cast<void>(faults.Report({
        RendererFaultCode::InternalInvariant,
        FaultSeverity::ProcessTerminal,
        0,
    }));
    return RendererBootstrapResult{
        RendererBootstrapCode::StateTransitionRejected,
        RendererMode::Disabled,
        machine.Current(),
        reachedProbing,
        faults.Current(),
    };
}

}

RendererBootstrapResult RunRendererBootstrap(
    const BuildGateReport& build,
    const HookManifestReport& hooks) noexcept
{
    RendererStateMachine machine;
    FaultController faults;

    if (!build.Accepted()) {
        static_cast<void>(faults.Report({
            RendererFaultCode::BuildRejected,
            FaultSeverity::Recoverable,
            0,
        }));
        return RendererBootstrapResult{
            RendererBootstrapCode::BuildRejected,
            RendererMode::Disabled,
            machine.Current(),
            false,
            faults.Current(),
        };
    }

    if (machine.TryTransition(RendererState::Disabled, RendererState::Probing) !=
        TransitionResult::Applied) {
        return StateFailure(machine, faults, false);
    }

    if (!hooks) {
        static_cast<void>(faults.Report({
            RendererFaultCode::HookPreparationFailed,
            FaultSeverity::Recoverable,
            0,
        }));
        if (machine.TryTransition(RendererState::Probing, RendererState::Disabled) !=
            TransitionResult::Applied) {
            return StateFailure(machine, faults, true);
        }
        return RendererBootstrapResult{
            RendererBootstrapCode::HookManifestRejected,
            RendererMode::Disabled,
            machine.Current(),
            true,
            faults.Current(),
        };
    }

    if (machine.TryTransition(RendererState::Probing, RendererState::Disabled) !=
        TransitionResult::Applied) {
        return StateFailure(machine, faults, true);
    }
    return RendererBootstrapResult{
        RendererBootstrapCode::ValidatedOff,
        RendererMode::Off,
        machine.Current(),
        true,
        std::nullopt,
    };
}

std::string FormatRendererBootstrap(const RendererBootstrapResult& result)
{
    std::string text = "renderer-probe: ";
    switch (result.code) {
    case RendererBootstrapCode::ValidatedOff:
        text += "validated";
        break;
    case RendererBootstrapCode::BuildRejected:
        text += "rejected reason=build";
        break;
    case RendererBootstrapCode::HookManifestRejected:
        text += "rejected reason=hook-manifest";
        break;
    case RendererBootstrapCode::StateTransitionRejected:
        text += "rejected reason=state-transition";
        break;
    }
    text += " mode=";
    text += ToString(result.mode);
    text += " state=";
    text += ToString(result.state);
    return text;
}

}
