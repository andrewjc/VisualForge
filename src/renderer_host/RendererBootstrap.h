#pragma once

#include "renderer_api/RendererMode.h"
#include "renderer_host/BuildGate.h"
#include "renderer_host/HookManifest.h"
#include "renderer_host/RendererState.h"

#include <cstdint>
#include <optional>
#include <string>

namespace vf::renderer {

enum class RendererBootstrapCode : std::uint8_t
{
    ValidatedOff,
    BuildRejected,
    HookManifestRejected,
    StateTransitionRejected
};

struct RendererBootstrapResult
{
    RendererBootstrapCode code{RendererBootstrapCode::BuildRejected};
    RendererMode mode{RendererMode::Disabled};
    RendererState state{RendererState::Disabled};
    bool reachedProbing{};
    std::optional<FaultEvent> fault;

    [[nodiscard]] bool Validated() const noexcept
    {
        return code == RendererBootstrapCode::ValidatedOff;
    }
};

[[nodiscard]] RendererBootstrapResult RunRendererBootstrap(
    const BuildGateReport& build,
    const HookManifestReport& hooks) noexcept;
[[nodiscard]] std::string FormatRendererBootstrap(
    const RendererBootstrapResult& result);

}
