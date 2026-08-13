#include "renderer_host/RendererBootstrap.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("P02_bootstrap_reaches_probing_then_validated_off", "[unit][phase02]")
{
    const vf::renderer::BuildGateReport build{};
    const vf::renderer::HookManifestReport hooks{};
    const auto result = vf::renderer::RunRendererBootstrap(build, hooks);

    CHECK(result.Validated());
    CHECK(result.code == vf::renderer::RendererBootstrapCode::ValidatedOff);
    CHECK(result.mode == vf::renderer::RendererMode::Off);
    CHECK(result.state == vf::renderer::RendererState::Disabled);
    CHECK(result.reachedProbing);
    CHECK_FALSE(result.fault.has_value());
    CHECK(vf::renderer::FormatRendererBootstrap(result) ==
          "renderer-probe: validated mode=Off state=Disabled");
}

TEST_CASE("P02_bootstrap_build_rejection_never_leaves_disabled", "[unit][phase02]")
{
    const vf::renderer::BuildGateReport build{
        static_cast<std::uint32_t>(vf::renderer::BuildMismatch::ExecutableSha256)};
    const vf::renderer::HookManifestReport hooks{};
    const auto result = vf::renderer::RunRendererBootstrap(build, hooks);

    CHECK_FALSE(result.Validated());
    CHECK(result.code == vf::renderer::RendererBootstrapCode::BuildRejected);
    CHECK(result.mode == vf::renderer::RendererMode::Disabled);
    CHECK(result.state == vf::renderer::RendererState::Disabled);
    CHECK_FALSE(result.reachedProbing);
    REQUIRE(result.fault);
    CHECK(result.fault->code == vf::renderer::RendererFaultCode::BuildRejected);
}

TEST_CASE("P02_bootstrap_hook_rejection_returns_to_disabled", "[unit][phase02]")
{
    const vf::renderer::BuildGateReport build{};
    const vf::renderer::HookManifestReport hooks{
        {vf::renderer::HookSiteError::ByteMismatch, 3, 2},
        2,
    };
    const auto result = vf::renderer::RunRendererBootstrap(build, hooks);

    CHECK_FALSE(result.Validated());
    CHECK(result.code == vf::renderer::RendererBootstrapCode::HookManifestRejected);
    CHECK(result.mode == vf::renderer::RendererMode::Disabled);
    CHECK(result.state == vf::renderer::RendererState::Disabled);
    CHECK(result.reachedProbing);
    REQUIRE(result.fault);
    CHECK(result.fault->code ==
          vf::renderer::RendererFaultCode::HookPreparationFailed);
}
