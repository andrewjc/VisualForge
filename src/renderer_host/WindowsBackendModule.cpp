#include "renderer_host/WindowsBackendModule.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vf::renderer {

WindowsBackendModule::WindowsBackendModule(std::filesystem::path path)
    : path_(std::move(path))
{}

WindowsBackendModule::~WindowsBackendModule() = default;

BackendModuleOpenResult WindowsBackendModule::Open() noexcept
{
    if (module_ != nullptr) {
        return BackendModuleOpenResult::Success;
    }

    std::error_code fileError;
    const auto absolutePath = std::filesystem::absolute(path_, fileError);
    if (fileError ||
        !std::filesystem::is_regular_file(absolutePath, fileError)) {
        lastError_ = fileError
            ? static_cast<unsigned long>(fileError.value())
            : ERROR_FILE_NOT_FOUND;
        return BackendModuleOpenResult::ModuleMissing;
    }

    const auto vulkan = LoadLibraryExW(
        L"vulkan-1.dll",
        nullptr,
        LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (vulkan == nullptr) {
        lastError_ = GetLastError();
        return BackendModuleOpenResult::LoaderUnavailable;
    }
    FreeLibrary(vulkan);

    const auto loaded = LoadLibraryExW(
        absolutePath.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
            LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (loaded == nullptr) {
        lastError_ = GetLastError();
        return BackendModuleOpenResult::OpenFailed;
    }
    module_ = loaded;
    lastError_ = ERROR_SUCCESS;
    return BackendModuleOpenResult::Success;
}

abi::QueryInterfaceFunction WindowsBackendModule::ResolveQuery() noexcept
{
    if (module_ == nullptr) {
        return nullptr;
    }
    const auto address = GetProcAddress(
        static_cast<HMODULE>(module_), abi::kBackendQueryExport);
    if (address == nullptr) {
        lastError_ = GetLastError();
        return nullptr;
    }
    return reinterpret_cast<abi::QueryInterfaceFunction>(address);
}

void WindowsBackendModule::MarkUnloadDeferred() noexcept
{
    unloadDeferred_ = module_ != nullptr;
}

bool WindowsBackendModule::Loaded() const noexcept
{
    return module_ != nullptr;
}

bool WindowsBackendModule::UnloadDeferred() const noexcept
{
    return unloadDeferred_;
}

unsigned long WindowsBackendModule::LastErrorCode() const noexcept
{
    return lastError_;
}

}
