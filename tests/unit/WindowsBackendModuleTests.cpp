#include "renderer_host/WindowsBackendModule.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("P04_windows_backend_module_reports_missing_file_without_loading", "[unit][phase04]")
{
    vf::renderer::WindowsBackendModule module{
        "Z:/visualforge/does-not-exist/VisualForgeRenderer.dll"};
    CHECK(module.Open() ==
          vf::renderer::BackendModuleOpenResult::ModuleMissing);
    CHECK_FALSE(module.Loaded());
    CHECK_FALSE(module.UnloadDeferred());
    CHECK(module.ResolveQuery() == nullptr);
}
