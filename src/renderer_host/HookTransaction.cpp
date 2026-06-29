#include "renderer_host/HookTransaction.h"

namespace vf::renderer {

HookSet::HookSet(IHookBackend& backend, std::atomic<std::uint64_t>& generation) noexcept
    : backend_(backend), generation_(generation)
{}

HookSet::~HookSet()
{
    Reset();
}

HookInstallResult HookSet::Install(const std::span<const HookSpec> specs)
{
    if (installed_) {
        return HookInstallResult{HookInstallError::AlreadyInstalled, 0};
    }
    if (specs.empty()) {
        return HookInstallResult{HookInstallError::Empty, 0};
    }

    for (std::size_t index = 0; index < specs.size(); ++index) {
        const auto& spec = specs[index];
        if (spec.id == 0 || spec.target == 0 || spec.replacement == 0) {
            return HookInstallResult{HookInstallError::InvalidSpec, spec.id};
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (specs[prior].id == spec.id) {
                return HookInstallResult{HookInstallError::DuplicateId, spec.id};
            }
        }
    }

    handles_.reserve(specs.size());
    for (const auto& spec : specs) {
        HookHandle handle{};
        if (backend_.Create(spec, handle) != HookBackendResult::Ok) {
            for (auto created = handles_.rbegin(); created != handles_.rend(); ++created) {
                backend_.Remove(*created);
            }
            handles_.clear();
            return HookInstallResult{HookInstallError::CreateFailed, spec.id};
        }
        handles_.push_back(handle);
    }

    std::size_t enabledCount = 0;
    for (std::size_t index = 0; index < handles_.size(); ++index) {
        if (backend_.Enable(handles_[index]) != HookBackendResult::Ok) {
            while (enabledCount > 0) {
                --enabledCount;
                backend_.Disable(handles_[enabledCount]);
            }
            for (auto created = handles_.rbegin(); created != handles_.rend(); ++created) {
                backend_.Remove(*created);
            }
            handles_.clear();
            return HookInstallResult{HookInstallError::EnableFailed, specs[index].id};
        }
        ++enabledCount;
    }

    installed_ = true;
    generation_.fetch_add(1, std::memory_order_release);
    return HookInstallResult{};
}

void HookSet::Reset() noexcept
{
    if (!installed_) {
        return;
    }

    for (auto handle = handles_.rbegin(); handle != handles_.rend(); ++handle) {
        backend_.Disable(*handle);
    }
    for (auto handle = handles_.rbegin(); handle != handles_.rend(); ++handle) {
        backend_.Remove(*handle);
    }
    handles_.clear();
    installed_ = false;
    generation_.fetch_add(1, std::memory_order_release);
}

bool HookSet::Installed() const noexcept
{
    return installed_;
}

}
