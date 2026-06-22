#include "PatternScan.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>

namespace vf::scan {

static uintptr_t s_base = 0;
static size_t s_size = 0;
static std::vector<Section> s_sections;

bool Init()
{
    if (s_base)
        return true;

    HMODULE module = GetModuleHandleW(nullptr);
    if (!module)
        return false;

    auto base = reinterpret_cast<const uint8_t*>(module);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    s_base = reinterpret_cast<uintptr_t>(base);
    s_size = nt->OptionalHeader.SizeOfImage;

    auto section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        Section out;
        out.begin = base + section->VirtualAddress;
        out.size = section->Misc.VirtualSize;
        out.executable = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        out.writable = (section->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        memcpy(out.name, section->Name, 8);
        if (out.size)
            s_sections.push_back(out);
    }
    return !s_sections.empty();
}

uintptr_t ImageBase()
{
    return s_base;
}

size_t ImageSize()
{
    return s_size;
}

const std::vector<Section>& Sections()
{
    return s_sections;
}

bool PointsIntoImage(uintptr_t p)
{
    return p >= s_base && p < s_base + s_size;
}

bool PointsIntoReadOnlyData(uintptr_t p)
{
    for (const Section& s : s_sections) {
        auto begin = reinterpret_cast<uintptr_t>(s.begin);
        if (p >= begin && p < begin + s.size)
            return !s.executable && !s.writable;
    }
    return false;
}

}
