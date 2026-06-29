#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer {

struct HookSpec
{
    std::uint32_t id{};
    std::uintptr_t target{};
    std::uintptr_t replacement{};
};

using HookHandle = std::uint64_t;

enum class HookBackendResult : std::uint8_t
{
    Ok,
    Failed
};

class IHookBackend
{
public:
    virtual ~IHookBackend() = default;
    virtual HookBackendResult Create(const HookSpec& spec, HookHandle& handle) noexcept = 0;
    virtual HookBackendResult Enable(HookHandle handle) noexcept = 0;
    virtual void Disable(HookHandle handle) noexcept = 0;
    virtual void Remove(HookHandle handle) noexcept = 0;
};

enum class HookInstallError : std::uint8_t
{
    None,
    Empty,
    AlreadyInstalled,
    InvalidSpec,
    DuplicateId,
    CreateFailed,
    EnableFailed
};

struct HookInstallResult
{
    HookInstallError error{HookInstallError::None};
    std::uint32_t failedHookId{};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == HookInstallError::None;
    }
};

class HookSet
{
public:
    HookSet(IHookBackend& backend, std::atomic<std::uint64_t>& generation) noexcept;
    ~HookSet();

    HookSet(const HookSet&) = delete;
    HookSet& operator=(const HookSet&) = delete;

    [[nodiscard]] HookInstallResult Install(std::span<const HookSpec> specs);
    void Reset() noexcept;
    [[nodiscard]] bool Installed() const noexcept;

private:
    IHookBackend& backend_;
    std::atomic<std::uint64_t>& generation_;
    std::vector<HookHandle> handles_;
    bool installed_{};
};

}
