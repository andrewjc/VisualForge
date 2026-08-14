#pragma once

#include "renderer_host/BackendHost.h"

#include <filesystem>

namespace vf::renderer {

class WindowsBackendModule final : public IBackendModule
{
public:
    explicit WindowsBackendModule(std::filesystem::path path);
    ~WindowsBackendModule() override;

    [[nodiscard]] BackendModuleOpenResult Open() noexcept override;
    [[nodiscard]] abi::QueryInterfaceFunction ResolveQuery() noexcept override;
    void MarkUnloadDeferred() noexcept override;

    [[nodiscard]] bool Loaded() const noexcept;
    [[nodiscard]] bool UnloadDeferred() const noexcept;
    [[nodiscard]] unsigned long LastErrorCode() const noexcept;

private:
    std::filesystem::path path_;
    void* module_{};
    bool unloadDeferred_{};
    unsigned long lastError_{};
};

}
