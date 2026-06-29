#include "renderer_host/HookTransaction.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

class FakeHookBackend final : public vf::renderer::IHookBackend
{
public:
    vf::renderer::HookBackendResult Create(
        const vf::renderer::HookSpec& spec,
        vf::renderer::HookHandle& handle) noexcept override
    {
        events.push_back("create:" + std::to_string(spec.id));
        const auto call = createCalls++;
        if (call == failCreateCall) {
            return vf::renderer::HookBackendResult::Failed;
        }
        handle = 1000u + spec.id;
        return vf::renderer::HookBackendResult::Ok;
    }

    vf::renderer::HookBackendResult Enable(vf::renderer::HookHandle handle) noexcept override
    {
        events.push_back("enable:" + std::to_string(handle));
        const auto call = enableCalls++;
        return call == failEnableCall
            ? vf::renderer::HookBackendResult::Failed
            : vf::renderer::HookBackendResult::Ok;
    }

    void Disable(vf::renderer::HookHandle handle) noexcept override
    {
        events.push_back("disable:" + std::to_string(handle));
    }

    void Remove(vf::renderer::HookHandle handle) noexcept override
    {
        events.push_back("remove:" + std::to_string(handle));
    }

    std::vector<std::string> events;
    std::size_t createCalls{};
    std::size_t enableCalls{};
    std::size_t failCreateCall{static_cast<std::size_t>(-1)};
    std::size_t failEnableCall{static_cast<std::size_t>(-1)};
};

constexpr vf::renderer::HookSpec kHooks[] = {
    {1, 0x1000, 0x2000},
    {2, 0x3000, 0x4000},
};

}

TEST_CASE("P02_hook_set_publishes_only_after_all_hooks_enable", "[unit][phase02]")
{
    FakeHookBackend backend;
    std::atomic<std::uint64_t> generation{0};
    vf::renderer::HookSet hooks(backend, generation);

    const auto result = hooks.Install(kHooks);
    REQUIRE(result);
    CHECK(hooks.Installed());
    CHECK(generation.load() == 1);
    CHECK(backend.events == std::vector<std::string>{
        "create:1", "create:2", "enable:1001", "enable:1002"});

    hooks.Reset();
    CHECK_FALSE(hooks.Installed());
    CHECK(generation.load() == 2);
    CHECK(backend.events == std::vector<std::string>{
        "create:1", "create:2", "enable:1001", "enable:1002",
        "disable:1002", "disable:1001", "remove:1002", "remove:1001"});
}

TEST_CASE("P02_hook_set_rolls_back_partial_create", "[unit][phase02]")
{
    FakeHookBackend backend;
    backend.failCreateCall = 1;
    std::atomic<std::uint64_t> generation{0};
    vf::renderer::HookSet hooks(backend, generation);

    const auto result = hooks.Install(kHooks);
    CHECK_FALSE(result);
    CHECK(result.error == vf::renderer::HookInstallError::CreateFailed);
    CHECK(result.failedHookId == 2);
    CHECK_FALSE(hooks.Installed());
    CHECK(generation.load() == 0);
    CHECK(backend.events == std::vector<std::string>{
        "create:1", "create:2", "remove:1001"});
}

TEST_CASE("P02_hook_set_rolls_back_partial_enable", "[unit][phase02]")
{
    FakeHookBackend backend;
    backend.failEnableCall = 1;
    std::atomic<std::uint64_t> generation{7};
    vf::renderer::HookSet hooks(backend, generation);

    const auto result = hooks.Install(kHooks);
    CHECK_FALSE(result);
    CHECK(result.error == vf::renderer::HookInstallError::EnableFailed);
    CHECK(result.failedHookId == 2);
    CHECK_FALSE(hooks.Installed());
    CHECK(generation.load() == 7);
    CHECK(backend.events == std::vector<std::string>{
        "create:1", "create:2", "enable:1001", "enable:1002",
        "disable:1001", "remove:1002", "remove:1001"});
}

TEST_CASE("P02_hook_set_validates_complete_manifest_before_backend_calls", "[unit][phase02]")
{
    FakeHookBackend backend;
    std::atomic<std::uint64_t> generation{0};
    vf::renderer::HookSet hooks(backend, generation);

    CHECK(hooks.Install({}).error == vf::renderer::HookInstallError::Empty);

    constexpr vf::renderer::HookSpec invalid[] = {{3, 0, 0x2000}};
    CHECK(hooks.Install(invalid).error == vf::renderer::HookInstallError::InvalidSpec);

    constexpr vf::renderer::HookSpec duplicate[] = {
        {4, 0x1000, 0x2000},
        {4, 0x3000, 0x4000},
    };
    CHECK(hooks.Install(duplicate).error == vf::renderer::HookInstallError::DuplicateId);
    CHECK(backend.events.empty());

    REQUIRE(hooks.Install(kHooks));
    CHECK(hooks.Install(kHooks).error == vf::renderer::HookInstallError::AlreadyInstalled);
}
