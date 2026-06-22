#include "FlexGuard.h"
#include "EngineSettings.h"
#include "IniWriter.h"
#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <string>
#include <vector>

namespace vf::flexguard {

namespace {

// The CUDA runtime Flex was built against. Its maximum supported compute capability is far
// below anything sold since ~2016, so its presence is the tell that Flex cannot run.
constexpr const wchar_t* kFlexCudaRuntime = L"cudart64_75.dll";

bool FileExistsInGameDir(const wchar_t* name)
{
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH))
        return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash)
        return false;
    *(slash + 1) = 0;
    wcscat_s(path, name);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

bool FlexLikelyUsable()
{
    // Flex is only shipped with the CUDA 7.5 runtime. If that is what is present — which it
    // always is for a stock install — the solver cannot initialise on modern hardware.
    // Treating this as "unusable" is the safe default; it costs a visual effect that is
    // already non-functional, and avoids a guaranteed crash.
    return !FileExistsInGameDir(kFlexCudaRuntime);
}

bool EnforceSafety(bool persistToIni)
{
    settings::Entry* flex = settings::Find("bNVFlexEnable:NVFlex");
    if (!flex) {
        log::Write("flexguard: bNVFlexEnable not resolved — nothing to enforce");
        return false;
    }

    if (!settings::GetBool(*flex)) {
        log::Write("flexguard: weapon debris already off — no action needed");
        return false;
    }

    if (FlexLikelyUsable()) {
        log::Write("flexguard: weapon debris is on and Flex looks usable — leaving it alone");
        return false;
    }

    settings::SetBool(*flex, false);
    log::Write("flexguard: weapon debris was ENABLED but NVIDIA Flex cannot run here "
               "(%ls present => CUDA 7.5 solver, unsupported by modern GPUs).",
               kFlexCudaRuntime);
    log::Write("flexguard: forced bNVFlexEnable=0 to prevent the null-pointer write in "
               "flexCreateTriangleMesh that kills the process when new geometry loads.");

    if (persistToIni) {
        std::vector<settings::Entry*> one{flex};
        std::string err;
        const int n = ini::WriteChanged(one, err);
        if (n >= 0)
            log::Write("flexguard: persisted bNVFlexEnable=0 to the game INI");
        else
            log::Write("flexguard: could not persist to INI (%s) — it is off for this "
                       "session regardless", err.c_str());
    }
    return true;
}

}
