#include "renderer_host/RendererState.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

TEST_CASE("P02_renderer_state_follows_the_safe_takeover_path", "[unit][phase02]")
{
    vf::renderer::RendererStateMachine machine;
    CHECK(machine.Current() == vf::renderer::RendererState::Disabled);

    constexpr std::array path{
        vf::renderer::RendererState::Probing,
        vf::renderer::RendererState::Observing,
        vf::renderer::RendererState::Mirroring,
        vf::renderer::RendererState::Armed,
        vf::renderer::RendererState::TakingOver,
        vf::renderer::RendererState::Draining,
        vf::renderer::RendererState::Disabled,
    };

    auto expected = vf::renderer::RendererState::Disabled;
    for (const auto desired : path) {
        CAPTURE(vf::renderer::ToString(expected), vf::renderer::ToString(desired));
        CHECK(machine.TryTransition(expected, desired) ==
              vf::renderer::TransitionResult::Applied);
        CHECK(machine.Current() == desired);
        expected = desired;
    }
}

TEST_CASE("P02_renderer_state_rejects_skips_and_stale_callers", "[unit][phase02]")
{
    vf::renderer::RendererStateMachine machine;

    CHECK(machine.TryTransition(
              vf::renderer::RendererState::Disabled,
              vf::renderer::RendererState::TakingOver) ==
          vf::renderer::TransitionResult::IllegalTransition);
    CHECK(machine.Current() == vf::renderer::RendererState::Disabled);

    REQUIRE(machine.TryTransition(
                vf::renderer::RendererState::Disabled,
                vf::renderer::RendererState::Probing) ==
            vf::renderer::TransitionResult::Applied);
    CHECK(machine.TryTransition(
              vf::renderer::RendererState::Disabled,
              vf::renderer::RendererState::Observing) ==
          vf::renderer::TransitionResult::StaleExpectedState);
    CHECK(machine.Current() == vf::renderer::RendererState::Probing);
}

TEST_CASE("P02_renderer_fault_state_is_terminal", "[unit][phase02]")
{
    vf::renderer::RendererStateMachine machine;
    REQUIRE(machine.TryTransition(
                vf::renderer::RendererState::Disabled,
                vf::renderer::RendererState::Probing) ==
            vf::renderer::TransitionResult::Applied);
    CHECK(machine.TryTransition(
              vf::renderer::RendererState::Probing,
              vf::renderer::RendererState::Faulted) ==
          vf::renderer::TransitionResult::Applied);
    CHECK_FALSE(vf::renderer::IsLegalTransition(
        vf::renderer::RendererState::Faulted,
        vf::renderer::RendererState::Disabled));
}

TEST_CASE("P02_fault_controller_keeps_first_fault_and_clear_policy", "[unit][phase02]")
{
    vf::renderer::FaultController faults;
    CHECK_FALSE(faults.Current().has_value());
    CHECK(faults.HotReentryAllowed());

    const vf::renderer::FaultEvent first{
        vf::renderer::RendererFaultCode::BackendUnavailable,
        vf::renderer::FaultSeverity::Recoverable,
        42,
    };
    CHECK(faults.Report(first));
    REQUIRE(faults.Current().has_value());
    CHECK(*faults.Current() == first);
    CHECK_FALSE(faults.Report({
        vf::renderer::RendererFaultCode::InternalInvariant,
        vf::renderer::FaultSeverity::ProcessTerminal,
        43,
    }));
    CHECK(*faults.Current() == first);
    CHECK(faults.ClearRecoverable());
    CHECK_FALSE(faults.Current().has_value());

    CHECK(faults.Report({
        vf::renderer::RendererFaultCode::InternalInvariant,
        vf::renderer::FaultSeverity::ProcessTerminal,
        99,
    }));
    CHECK_FALSE(faults.HotReentryAllowed());
    CHECK_FALSE(faults.ClearRecoverable());
    CHECK(faults.Current()->frame == 99);
}
