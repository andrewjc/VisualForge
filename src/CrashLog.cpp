#include "CrashLog.h"
#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <Psapi.h>
#include <ShlObj.h>

#include <MinHook.h>

#include <cstdio>

#pragma comment(lib, "Dbghelp.lib")

namespace vf::crashlog {

namespace {

FILE* s_out = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER s_previous = nullptr;

void Emit(const char* fmt, ...)
{
    if (!s_out)
        return;
    va_list args;
    va_start(args, fmt);
    vfprintf(s_out, fmt, args);
    va_end(args);
    fputc('\n', s_out);
    fflush(s_out);
}

const char* ExceptionName(DWORD code)
{
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
        default: return "UNKNOWN";
    }
}

// Finds which loaded module an address belongs to, which is the single most useful line in
// a crash report: it says whose code faulted.
bool ModuleForAddress(uintptr_t addr, char* nameOut, size_t nameLen, uintptr_t& base)
{
    HMODULE mods[512];
    DWORD needed = 0;
    HANDLE proc = GetCurrentProcess();
    if (!EnumProcessModules(proc, mods, sizeof(mods), &needed))
        return false;

    const size_t count = needed / sizeof(HMODULE);
    for (size_t i = 0; i < count; ++i) {
        MODULEINFO mi{};
        if (!GetModuleInformation(proc, mods[i], &mi, sizeof(mi)))
            continue;
        auto start = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        if (addr >= start && addr < start + mi.SizeOfImage) {
            char path[MAX_PATH];
            if (GetModuleFileNameA(mods[i], path, MAX_PATH)) {
                const char* slash = strrchr(path, '\\');
                strncpy_s(nameOut, nameLen, slash ? slash + 1 : path, _TRUNCATE);
            } else {
                strncpy_s(nameOut, nameLen, "<unknown>", _TRUNCATE);
            }
            base = start;
            return true;
        }
    }
    return false;
}

void WriteStack(CONTEXT* ctx)
{
    HANDLE proc = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx->Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx->Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx->Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    // StackWalk64 can modify the context, so hand it a copy.
    CONTEXT walkCtx = *ctx;

    alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;

    Emit("");
    Emit("CALL STACK");
    Emit("----------");

    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &walkCtx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (frame.AddrPC.Offset == 0)
            break;

        const uintptr_t pc = uintptr_t(frame.AddrPC.Offset);
        char mod[MAX_PATH] = "<unknown>";
        uintptr_t base = 0;
        ModuleForAddress(pc, mod, sizeof(mod), base);

        DWORD64 disp = 0;
        if (SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym)) {
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisp = 0;
            if (SymGetLineFromAddr64(proc, frame.AddrPC.Offset, &lineDisp, &line)) {
                Emit("  [%2d] %-24s +0x%08llX  %s+0x%llX  (%s:%lu)", i, mod,
                     base ? pc - base : pc, sym->Name, disp, line.FileName, line.LineNumber);
            } else {
                Emit("  [%2d] %-24s +0x%08llX  %s+0x%llX", i, mod, base ? pc - base : pc,
                     sym->Name, disp);
            }
        } else {
            Emit("  [%2d] %-24s +0x%08llX", i, mod, base ? pc - base : pc);
        }
    }
}

void WriteModules()
{
    Emit("");
    Emit("LOADED MODULES");
    Emit("--------------");

    HMODULE mods[512];
    DWORD needed = 0;
    HANDLE proc = GetCurrentProcess();
    if (!EnumProcessModules(proc, mods, sizeof(mods), &needed))
        return;

    const size_t count = needed / sizeof(HMODULE);
    for (size_t i = 0; i < count; ++i) {
        MODULEINFO mi{};
        char path[MAX_PATH];
        if (!GetModuleInformation(proc, mods[i], &mi, sizeof(mi)))
            continue;
        if (!GetModuleFileNameA(mods[i], path, MAX_PATH))
            continue;
        const char* slash = strrchr(path, '\\');
        Emit("  0x%016llX - 0x%016llX  %s", uintptr_t(mi.lpBaseOfDll),
             uintptr_t(mi.lpBaseOfDll) + mi.SizeOfImage, slash ? slash + 1 : path);
    }
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info)
{
    // Open the report lazily so a healthy session never creates the file.
    PWSTR docs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\My Games\\Fallout4\\F4SE\\VisualForge-crash.log", docs);
        CoTaskMemFree(docs);
        _wfopen_s(&s_out, path, L"w");
    }

    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const auto addr = uintptr_t(info->ExceptionRecord->ExceptionAddress);

    SYSTEMTIME st;
    GetLocalTime(&st);
    Emit("Fallout 4 crash report — written by VisualForge");
    Emit("%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
         st.wSecond);
    Emit("");
    Emit("EXCEPTION  %s (0x%08lX)  at 0x%016llX", ExceptionName(code), code, addr);

    char mod[MAX_PATH] = "<unknown>";
    uintptr_t base = 0;
    if (ModuleForAddress(addr, mod, sizeof(mod), base))
        Emit("FAULTING MODULE  %s  +0x%08llX", mod, addr - base);
    else
        Emit("FAULTING MODULE  <not in any loaded module>");

    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR op = info->ExceptionRecord->ExceptionInformation[0];
        const ULONG_PTR target = info->ExceptionRecord->ExceptionInformation[1];
        Emit("ACCESS VIOLATION  tried to %s 0x%016llX",
             op == 0 ? "read" : (op == 1 ? "write" : "execute"), uintptr_t(target));
    }

    if (auto* c = info->ContextRecord) {
        Emit("");
        Emit("REGISTERS");
        Emit("---------");
        Emit("  RIP 0x%016llX  RSP 0x%016llX  RBP 0x%016llX", c->Rip, c->Rsp, c->Rbp);
        Emit("  RAX 0x%016llX  RBX 0x%016llX  RCX 0x%016llX", c->Rax, c->Rbx, c->Rcx);
        Emit("  RDX 0x%016llX  RSI 0x%016llX  RDI 0x%016llX", c->Rdx, c->Rsi, c->Rdi);
        Emit("  R8  0x%016llX  R9  0x%016llX  R10 0x%016llX", c->R8, c->R9, c->R10);
        Emit("  R11 0x%016llX  R12 0x%016llX  R13 0x%016llX", c->R11, c->R12, c->R13);
        Emit("  R14 0x%016llX  R15 0x%016llX", c->R14, c->R15);

        WriteStack(c);
    }

    WriteModules();

    Emit("");
    Emit("Note: VisualForge only observes — it does not patch engine allocators, so its");
    Emit("presence in the module list does not imply it caused the fault. Check the");
    Emit("FAULTING MODULE line and the top of the call stack.");

    if (s_out) {
        fclose(s_out);
        s_out = nullptr;
    }

    return s_previous ? s_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

// ---- deliberate-exit instrumentation ---------------------------------------------------

using ExitProcessFn = void(WINAPI*)(UINT);
using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);
using RtlExitUserProcessFn = void(WINAPI*)(UINT);
using NtTerminateProcessFn = LONG(WINAPI*)(HANDLE, LONG);

ExitProcessFn s_origExitProcess = nullptr;
TerminateProcessFn s_origTerminateProcess = nullptr;
RtlExitUserProcessFn s_origRtlExitUserProcess = nullptr;
NtTerminateProcessFn s_origNtTerminateProcess = nullptr;
bool s_exitReported = false;

// Opens the report file if it is not already open.
void OpenReport()
{
    if (s_out)
        return;
    PWSTR docs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\My Games\\Fallout4\\F4SE\\VisualForge-crash.log", docs);
        CoTaskMemFree(docs);
        _wfopen_s(&s_out, path, L"w");
    }
}

// Reports a deliberate termination and, crucially, the call stack that requested it — that
// is what identifies which component decided to quit.
void ReportExit(const char* how, UINT code)
{
    if (s_exitReported)
        return;
    s_exitReported = true;

    OpenReport();

    SYSTEMTIME st;
    GetLocalTime(&st);
    Emit("Fallout 4 shutdown report — written by VisualForge");
    Emit("%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
         st.wSecond);
    Emit("");
    Emit("DELIBERATE PROCESS EXIT via %s (exit code %u / 0x%08X)", how, code, code);
    Emit("");
    Emit("The process was told to quit rather than faulting, which is why no exception or");
    Emit("Windows Error Reporting entry exists. The stack below shows who requested it.");

    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    WriteStack(&ctx);
    WriteModules();

    if (s_out) {
        fclose(s_out);
        s_out = nullptr;
    }
}

void WINAPI HookedExitProcess(UINT code)
{
    ReportExit("ExitProcess", code);
    s_origExitProcess(code);
}

BOOL WINAPI HookedTerminateProcess(HANDLE process, UINT code)
{
    // Only care about this process killing itself.
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
        ReportExit("TerminateProcess", code);
    return s_origTerminateProcess(process, code);
}

// kernel32's ExitProcess is a thin wrapper; code that calls the ntdll routines directly (or
// via the CRT) would slip past the wrappers above, so cover those too.
void WINAPI HookedRtlExitUserProcess(UINT code)
{
    ReportExit("RtlExitUserProcess", code);
    s_origRtlExitUserProcess(code);
}

LONG WINAPI HookedNtTerminateProcess(HANDLE process, LONG status)
{
    if (process == nullptr || process == GetCurrentProcess() ||
        GetProcessId(process) == GetCurrentProcessId())
        ReportExit("NtTerminateProcess", UINT(status));
    return s_origNtTerminateProcess(process, status);
}

// Serious first-chance exceptions are logged (capped) even when something else handles them,
// because a swallowed access violation often precedes a decision to quit.
LONG CALLBACK OnVectoredException(EXCEPTION_POINTERS* info)
{
    static int logged = 0;
    const DWORD code = info->ExceptionRecord->ExceptionCode;

    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_IN_PAGE_ERROR:
            break;
        default:
            return EXCEPTION_CONTINUE_SEARCH; // C++ throws, debugger events, etc.
    }

    if (logged < 8) {
        ++logged;
        const auto addr = uintptr_t(info->ExceptionRecord->ExceptionAddress);
        char mod[MAX_PATH] = "<unknown>";
        uintptr_t base = 0;
        ModuleForAddress(addr, mod, sizeof(mod), base);
        log::Write("crashlog: first-chance %s at %s+0x%llX (may be handled by the game)",
                   ExceptionName(code), mod, base ? addr - base : addr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

bool InstallExitInstrumentation()
{
    AddVectoredExceptionHandler(0 /* call last */, &OnVectoredException);

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) {
        log::Write("crashlog: kernel32 not found — exit instrumentation skipped");
        return false;
    }

    auto exitProc = reinterpret_cast<void*>(GetProcAddress(k32, "ExitProcess"));
    auto termProc = reinterpret_cast<void*>(GetProcAddress(k32, "TerminateProcess"));

    bool ok = true;
    if (exitProc && MH_CreateHook(exitProc, reinterpret_cast<void*>(&HookedExitProcess),
                                  reinterpret_cast<void**>(&s_origExitProcess)) == MH_OK &&
        MH_EnableHook(exitProc) == MH_OK) {
        log::Write("crashlog: ExitProcess instrumented");
    } else {
        log::Write("crashlog: could not instrument ExitProcess");
        ok = false;
    }

    if (termProc && MH_CreateHook(termProc, reinterpret_cast<void*>(&HookedTerminateProcess),
                                  reinterpret_cast<void**>(&s_origTerminateProcess)) == MH_OK &&
        MH_EnableHook(termProc) == MH_OK) {
        log::Write("crashlog: TerminateProcess instrumented");
    } else {
        log::Write("crashlog: could not instrument TerminateProcess");
        ok = false;
    }

    // The ntdll routines are the last common path before the process actually dies.
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        struct { const char* name; void* hook; void** orig; } ntHooks[] = {
            {"RtlExitUserProcess", reinterpret_cast<void*>(&HookedRtlExitUserProcess),
             reinterpret_cast<void**>(&s_origRtlExitUserProcess)},
            {"NtTerminateProcess", reinterpret_cast<void*>(&HookedNtTerminateProcess),
             reinterpret_cast<void**>(&s_origNtTerminateProcess)},
        };
        for (auto& h : ntHooks) {
            auto target = reinterpret_cast<void*>(GetProcAddress(ntdll, h.name));
            if (target && MH_CreateHook(target, h.hook, h.orig) == MH_OK &&
                MH_EnableHook(target) == MH_OK)
                log::Write("crashlog: %s instrumented", h.name);
            else
                log::Write("crashlog: could not instrument %s", h.name);
        }
    }

    return ok;
}

bool Install()
{
    HANDLE proc = GetCurrentProcess();

    // Point the symbol handler at the plugins folder so Fallout4.pdb (shipped with Buffout's
    // archive) resolves engine functions to names instead of bare addresses.
    char searchPath[MAX_PATH * 2] = ".";
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
        char* slash = strrchr(exePath, '\\');
        if (slash) {
            *slash = 0;
            snprintf(searchPath, sizeof(searchPath), "%s;%s\\Data\\F4SE\\Plugins", exePath,
                     exePath);
        }
    }

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES |
                  SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitialize(proc, searchPath, TRUE))
        log::Write("crashlog: SymInitialize failed (%lu) — stacks will be unsymbolised",
                   GetLastError());

    s_previous = SetUnhandledExceptionFilter(&OnUnhandledException);
    log::Write("crashlog: crash reporter installed (symbol path: %s)", searchPath);
    return true;
}

}
