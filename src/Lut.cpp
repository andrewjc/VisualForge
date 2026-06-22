#include "Lut.h"
#include "PostProcess.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vf::lut {

namespace {

// Reads the whole file as text. Returns false if it can't be opened.
bool ReadAll(const wchar_t* path, std::string& out)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f)
        return false;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    fclose(f);
    return true;
}

const char* SkipSpaces(const char* p)
{
    while (*p == ' ' || *p == '\t')
        ++p;
    return p;
}

} // namespace

bool ParseCube(const wchar_t* path, std::vector<float>& rgba, int& size, std::string& err)
{
    std::string text;
    if (!ReadAll(path, text)) {
        err = "cannot open file";
        return false;
    }

    int lutSize = 0;
    std::vector<float> entries; // r,g,b triplets in file order
    entries.reserve(1 << 16);

    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() : nl + 1;

        const char* p = SkipSpaces(line.c_str());
        if (*p == '\0' || *p == '#' || *p == '\r')
            continue;

        if (_strnicmp(p, "LUT_1D_SIZE", 11) == 0) {
            err = "1D LUTs are not supported (use a 3D .cube)";
            return false;
        }
        if (_strnicmp(p, "LUT_3D_SIZE", 11) == 0) {
            lutSize = atoi(SkipSpaces(p + 11));
            continue;
        }
        if (_strnicmp(p, "TITLE", 5) == 0 || _strnicmp(p, "DOMAIN_", 7) == 0)
            continue;

        // Otherwise expect three floats.
        char* end = nullptr;
        float r = strtof(p, &end);
        if (end == p)
            continue; // not a data line
        float g = strtof(end, &end);
        float b = strtof(end, &end);
        entries.push_back(r);
        entries.push_back(g);
        entries.push_back(b);
    }

    if (lutSize < 2 || lutSize > 128) {
        err = "missing or unreasonable LUT_3D_SIZE";
        return false;
    }
    const size_t expected = size_t(lutSize) * lutSize * lutSize * 3;
    if (entries.size() != expected) {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected %zu values for size %d, got %zu",
                 expected, lutSize, entries.size());
        err = msg;
        return false;
    }

    // Expand r,g,b triplets to RGBA (alpha = 1). File order already has red fastest, which is
    // the x-fastest order a Texture3D expects, so no reordering is needed.
    rgba.resize(size_t(lutSize) * lutSize * lutSize * 4);
    for (size_t i = 0, e = entries.size() / 3; i < e; ++i) {
        rgba[i * 4 + 0] = entries[i * 3 + 0];
        rgba[i * 4 + 1] = entries[i * 3 + 1];
        rgba[i * 4 + 2] = entries[i * 3 + 2];
        rgba[i * 4 + 3] = 1.0f;
    }
    size = lutSize;
    return true;
}

bool LoadInto(ID3D11Device* device, const wchar_t* fullPath, std::string& err)
{
    std::vector<float> rgba;
    int size = 0;
    if (!ParseCube(fullPath, rgba, size, err))
        return false;
    if (!post::SetLut(device, rgba.data(), size)) {
        err = "GPU upload failed";
        return false;
    }
    return true;
}

}
