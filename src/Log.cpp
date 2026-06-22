#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ShlObj.h>

#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <ctime>
#include <mutex>

namespace vf::log {

static FILE* s_file = nullptr;
static std::mutex s_mutex;
static wchar_t s_path[MAX_PATH] = {};

bool Open()
{
    std::lock_guard lock(s_mutex);
    if (s_file)
        return true;

    PWSTR docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)))
        return false;

    swprintf_s(s_path, L"%s\\My Games\\Fallout4\\F4SE", docs);
    CoTaskMemFree(docs);

    // The F4SE directory normally exists (the loader logs there first), but be safe.
    SHCreateDirectoryExW(nullptr, s_path, nullptr);
    wcscat_s(s_path, L"\\VisualForge.log");

    // Open with deny-write sharing so external tools (and us) can tail the log live.
    s_file = _wfsopen(s_path, L"w", _SH_DENYWR);
    return s_file != nullptr;
}

void Write(const char* fmt, ...)
{
    std::lock_guard lock(s_mutex);
    if (!s_file)
        return;

    time_t now = time(nullptr);
    tm local{};
    localtime_s(&local, &now);
    fprintf(s_file, "[%02d:%02d:%02d] ", local.tm_hour, local.tm_min, local.tm_sec);

    va_list args;
    va_start(args, fmt);
    vfprintf(s_file, fmt, args);
    va_end(args);

    fputc('\n', s_file);
    fflush(s_file);
}

const wchar_t* Path()
{
    return s_path;
}

}
