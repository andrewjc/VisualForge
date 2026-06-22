#include "IniWriter.h"
#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace vf::ini {

namespace {

struct SettingRef {
    settings::Entry* entry;
    std::string key;     // "fTAAPostSharpen"
    std::string section; // "Display"
};

std::string FormatValue(const settings::Entry& e)
{
    char buf[64];
    switch (e.type) {
        case settings::Type::Bool:
            return settings::GetBool(e) ? "1" : "0";
        case settings::Type::Int:
            snprintf(buf, sizeof(buf), "%d", settings::GetInt(e));
            return buf;
        case settings::Type::UInt:
            snprintf(buf, sizeof(buf), "%u", settings::GetUInt(e));
            return buf;
        case settings::Type::Float:
            snprintf(buf, sizeof(buf), "%.4f", settings::GetFloat(e));
            return buf;
        default:
            return settings::GetString(e);
    }
}

bool IEquals(const std::string& a, const std::string& b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(unsigned(x)) == std::tolower(unsigned(y));
           });
}

std::string Trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

// Returns section name for "[Name]" lines, empty otherwise.
std::string SectionOf(const std::string& line)
{
    std::string t = Trim(line);
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']')
        return t.substr(1, t.size() - 2);
    return "";
}

// Returns the key of a "key=value" line, empty otherwise.
std::string KeyOf(const std::string& line)
{
    size_t eq = line.find('=');
    if (eq == std::string::npos)
        return "";
    return Trim(line.substr(0, eq));
}

bool ReadLines(const wchar_t* path, std::vector<std::string>& lines, bool& exists)
{
    lines.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) {
        exists = false;
        return true; // a missing file is not an error; we may create it
    }
    exists = true;
    std::string content;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        content.append(buf, n);
    fclose(f);

    size_t start = 0;
    while (start <= content.size()) {
        size_t nl = content.find('\n', start);
        if (nl == std::string::npos) {
            if (start < content.size())
                lines.push_back(content.substr(start));
            break;
        }
        std::string line = content.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
        start = nl + 1;
    }
    return true;
}

bool WriteLines(const wchar_t* path, const std::vector<std::string>& lines)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f)
        return false;
    for (const std::string& line : lines) {
        fwrite(line.data(), 1, line.size(), f);
        fwrite("\r\n", 1, 2, f);
    }
    fclose(f);
    return true;
}

// True when [section]key exists in the file (used to route Prefs-owned settings).
bool HasKey(const std::vector<std::string>& lines, const std::string& section, const std::string& key)
{
    std::string current;
    for (const std::string& line : lines) {
        std::string sec = SectionOf(line);
        if (!sec.empty()) {
            current = sec;
            continue;
        }
        if (IEquals(current, section) && IEquals(KeyOf(line), key))
            return true;
    }
    return false;
}

// Sets [section]key=value in `lines`: updates in place, appends to the section,
// or creates the section at the end of the file.
void MergeKey(std::vector<std::string>& lines, const std::string& section,
              const std::string& key, const std::string& value)
{
    std::string current;
    size_t sectionEnd = std::string::npos; // index one past the last non-empty line of the section
    bool inSection = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string sec = SectionOf(lines[i]);
        if (!sec.empty()) {
            if (inSection && sectionEnd == std::string::npos)
                sectionEnd = i;
            inSection = IEquals(sec, section);
            current = sec;
            continue;
        }
        if (inSection && IEquals(KeyOf(lines[i]), key)) {
            lines[i] = key + "=" + value;
            return;
        }
    }

    // Key not found. Find where the section ends (or create it).
    current.clear();
    size_t insertAt = std::string::npos;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string sec = SectionOf(lines[i]);
        if (!sec.empty()) {
            if (IEquals(current, section)) {
                insertAt = i;
                break;
            }
            current = sec;
        }
    }
    if (IEquals(current, section) && insertAt == std::string::npos)
        insertAt = lines.size();

    if (insertAt != std::string::npos) {
        // Skip back over trailing blank lines so the key lands inside the section body.
        while (insertAt > 0 && Trim(lines[insertAt - 1]).empty())
            --insertAt;
        lines.insert(lines.begin() + insertAt, key + "=" + value);
    } else {
        if (!lines.empty() && !Trim(lines.back()).empty())
            lines.push_back("");
        lines.push_back("[" + section + "]");
        lines.push_back(key + "=" + value);
    }
}

} // namespace

int WriteChanged(const std::vector<settings::Entry*>& changed, std::string& err)
{
    if (changed.empty())
        return 0;

    PWSTR docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        err = "could not locate Documents folder";
        return -1;
    }
    wchar_t prefsPath[MAX_PATH], customPath[MAX_PATH];
    swprintf_s(prefsPath, L"%s\\My Games\\Fallout4\\Fallout4Prefs.ini", docs);
    swprintf_s(customPath, L"%s\\My Games\\Fallout4\\Fallout4Custom.ini", docs);
    CoTaskMemFree(docs);

    std::vector<SettingRef> refs;
    for (settings::Entry* e : changed) {
        const char* colon = strchr(e->fullName, ':');
        if (!colon)
            continue;
        SettingRef r;
        r.entry = e;
        r.key.assign(e->fullName, colon - e->fullName);
        r.section.assign(colon + 1);
        refs.push_back(std::move(r));
    }

    std::vector<std::string> prefs, custom;
    bool prefsExists = false, customExists = false;
    ReadLines(prefsPath, prefs, prefsExists);
    ReadLines(customPath, custom, customExists);

    bool prefsDirty = false, customDirty = false;
    int written = 0;
    for (const SettingRef& r : refs) {
        std::string value = FormatValue(*r.entry);
        if (prefsExists && HasKey(prefs, r.section, r.key)) {
            MergeKey(prefs, r.section, r.key, value);
            prefsDirty = true;
        } else {
            MergeKey(custom, r.section, r.key, value);
            customDirty = true;
        }
        ++written;
        log::Write("ini: %s:%s = %s", r.key.c_str(), r.section.c_str(), value.c_str());
    }

    if (prefsDirty && !WriteLines(prefsPath, prefs)) {
        err = "failed to write Fallout4Prefs.ini";
        return -1;
    }
    if (customDirty && !WriteLines(customPath, custom)) {
        err = "failed to write Fallout4Custom.ini";
        return -1;
    }
    return written;
}

}
