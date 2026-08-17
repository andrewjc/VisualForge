#pragma once

#include <sal.h>

namespace vf::log {

// Opens Documents\My Games\Fallout4\F4SE\VisualForge.log for writing.
bool Open();
// Annotated so the compiler checks the format against its arguments.
//
// Without this a mismatch is silent: a diagnostic line here was written with
// twenty conversions and sixteen arguments, and the four with nothing behind
// them printed whatever was on the stack. They read as zeros, which is a
// plausible measurement, and were taken as evidence for two runs. A log that
// cannot be trusted is worse than no log, so the check belongs at the
// declaration rather than in the reviewer's head.
void Write(_Printf_format_string_ const char* fmt, ...);
const wchar_t* Path();

}
