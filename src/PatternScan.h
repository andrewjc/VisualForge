#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace vf::scan {

struct Section {
    const uint8_t* begin = nullptr;
    size_t size = 0;
    bool executable = false;
    bool writable = false;
    char name[9] = {};
};

// Walks the game executable's PE headers once. Safe to call repeatedly.
bool Init();

uintptr_t ImageBase();
size_t ImageSize();
const std::vector<Section>& Sections();

bool PointsIntoImage(uintptr_t p);

// True when p lands in a mapped, non-executable, non-writable section (.rdata):
// where MSVC places vtables. Used as the sanity check for candidate Setting objects.
bool PointsIntoReadOnlyData(uintptr_t p);

}
