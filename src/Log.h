#pragma once

namespace vf::log {

// Opens Documents\My Games\Fallout4\F4SE\VisualForge.log for writing.
bool Open();
void Write(const char* fmt, ...);
const wchar_t* Path();

}
