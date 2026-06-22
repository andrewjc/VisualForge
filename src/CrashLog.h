#pragma once

// Crash reporting. Installs an unhandled-exception filter that writes a readable report to
// Documents\My Games\Fallout4\F4SE\VisualForge-crash.log: the exception, the faulting
// module, registers, a symbolised call stack and the loaded-module map.
//
// Deliberately read-only with respect to the engine — it observes and reports, and never
// patches allocators or engine internals, so it cannot itself destabilise the game. That is
// why it works on any runtime while Buffout 4 is pinned to the version it was built for.
namespace vf::crashlog {

// Call once, as early as possible.
bool Install();

// Instruments the deliberate-exit path (ExitProcess / TerminateProcess / RtlExitUserProcess)
// and installs a vectored handler for serious first-chance exceptions. A process that
// vanishes with no exception and no Windows Error Reporting entry was almost certainly told
// to quit rather than having faulted, and only this catches that. Requires MinHook to be
// initialised first.
bool InstallExitInstrumentation();

}
