#include "EngineSettings.h"
#include "Log.h"
#include "PatternScan.h"
#include "SettingsCatalog.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace vf::settings {

static std::vector<Entry> s_entries;                          // one per catalog name, index-aligned
static std::unordered_map<std::string_view, int> s_byName;    // catalog name -> entry index
static std::vector<Entry*> s_resolved;                        // entries with at least one slot, sorted
static bool s_done = false;

Type TypeFromName(const char* fullName)
{
    switch (fullName[0]) {
        case 'b': return Type::Bool;
        case 'i': return Type::Int;
        case 'u': return Type::UInt;
        case 'f': return Type::Float;
        case 's': case 'S': return Type::String;
        default: return Type::Unknown;
    }
}

int ResolveAll()
{
    if (s_done)
        return int(s_resolved.size());

    if (!scan::Init()) {
        log::Write("settings: PE header walk failed");
        s_done = true;
        return 0;
    }

    s_entries.resize(kSettingCatalogCount);
    s_byName.reserve(kSettingCatalogCount);
    for (int i = 0; i < kSettingCatalogCount; ++i) {
        s_entries[i].fullName = kSettingCatalog[i];
        s_entries[i].type = TypeFromName(kSettingCatalog[i]);
        s_byName.emplace(kSettingCatalog[i], i);
    }

    // Pass 1: find the address of every catalog name string in the image.
    // A Setting's name is NUL-terminated, but the linker sometimes packs it directly
    // after non-string data (e.g. float constants) with no leading NUL — so the name
    // is a *suffix* of the run ending at the terminator. We therefore test every
    // name-length suffix ending at each NUL, not just whole NUL-delimited tokens.
    constexpr size_t kMinName = 5, kMaxName = 96;
    auto isNameStart = [](uint8_t c0, uint8_t c1) {
        bool prefix = c0 == 'b' || c0 == 'i' || c0 == 'f' || c0 == 's' || c0 == 'u' || c0 == 'S';
        return prefix && c1 >= 'A' && c1 <= 'Z';
    };

    std::unordered_map<uintptr_t, int> stringAddrToEntry;
    for (const scan::Section& sec : scan::Sections()) {
        if (sec.executable)
            continue;
        const uint8_t* p = sec.begin;
        const uint8_t* end = sec.begin + sec.size;
        while (p < end) {
            if (*p == 0) {
                ++p;
                continue;
            }
            const uint8_t* strEnd = static_cast<const uint8_t*>(memchr(p, 0, size_t(end - p)));
            if (!strEnd)
                strEnd = end;

            // Candidate name starts: longest suffix first, so the recorded address is the
            // true name start (where a Setting object's name pointer would point).
            const uint8_t* earliest = (size_t(strEnd - p) > kMaxName) ? strEnd - kMaxName : p;
            for (const uint8_t* s = earliest; s + kMinName <= strEnd; ++s) {
                if (!isNameStart(s[0], s[1]))
                    continue;
                std::string_view token(reinterpret_cast<const char*>(s), size_t(strEnd - s));
                auto it = s_byName.find(token);
                if (it != s_byName.end()) {
                    stringAddrToEntry.emplace(reinterpret_cast<uintptr_t>(s), it->second);
                    break;
                }
            }
            p = strEnd + 1;
        }
    }

    // Pass 2: find static Setting objects — any 8-aligned pointer to a known name
    // string where the qword 0x10 bytes earlier is a vtable pointer into .rdata.
    int slotCount = 0;
    for (const scan::Section& sec : scan::Sections()) {
        if (sec.executable)
            continue;
        auto secBase = reinterpret_cast<uintptr_t>(sec.begin);
        uintptr_t aligned = (secBase + 7) & ~uintptr_t(7);
        const uintptr_t* p = reinterpret_cast<const uintptr_t*>(aligned);
        const uintptr_t* end = reinterpret_cast<const uintptr_t*>((secBase + sec.size) & ~uintptr_t(7));
        for (; p < end; ++p) {
            auto it = stringAddrToEntry.find(*p);
            if (it == stringAddrToEntry.end())
                continue;
            uintptr_t namePtrAddr = reinterpret_cast<uintptr_t>(p);
            if (namePtrAddr < secBase + 0x10)
                continue;
            uintptr_t settingBase = namePtrAddr - 0x10;
            uintptr_t vtbl = *reinterpret_cast<const uintptr_t*>(settingBase);
            if (!scan::PointsIntoReadOnlyData(vtbl))
                continue;
            Entry& entry = s_entries[it->second];
            entry.slots.push_back(reinterpret_cast<Data*>(settingBase + 0x08));
            ++slotCount;
        }
    }

    // Mark which entries had their name string located (regardless of object resolution),
    // so diagnostics can separate "no string in image" from "string but no static object".
    std::vector<bool> stringLocated(kSettingCatalogCount, false);
    for (const auto& [addr, entryIndex] : stringAddrToEntry)
        stringLocated[entryIndex] = true;

    for (Entry& e : s_entries) {
        if (!e.slots.empty()) {
            e.original = *e.slots[0];
            s_resolved.push_back(&e);
        }
    }
    std::sort(s_resolved.begin(), s_resolved.end(),
              [](const Entry* a, const Entry* b) { return strcmp(a->fullName, b->fullName) < 0; });

    s_done = true;
    log::Write("settings: %zu of %d catalog entries resolved (%d Setting objects found, %zu name strings located)",
               s_resolved.size(), kSettingCatalogCount, slotCount, stringAddrToEntry.size());

    int stringNoObject = 0, noString = 0;
    for (int i = 0; i < kSettingCatalogCount; ++i) {
        if (!s_entries[i].slots.empty())
            continue;
        if (stringLocated[i])
            ++stringNoObject;
        else
            ++noString;
    }
    log::Write("settings: unresolved breakdown — %d have a name string but no object, %d have no string (console tokens)",
               stringNoObject, noString);
    return int(s_resolved.size());
}

bool Resolved()
{
    return s_done;
}

Entry* Find(const char* fullName)
{
    auto it = s_byName.find(fullName);
    if (it == s_byName.end())
        return nullptr;
    Entry& e = s_entries[it->second];
    return e.slots.empty() ? nullptr : &e;
}

std::vector<Entry*>& AllResolved()
{
    return s_resolved;
}

static void MarkChanged(Entry& e)
{
    e.changed = memcmp(&e.original, e.slots[0], sizeof(Data)) != 0;
}

float GetFloat(const Entry& e) { return e.slots[0]->f32; }
int GetInt(const Entry& e) { return e.slots[0]->s32; }
unsigned GetUInt(const Entry& e) { return e.slots[0]->u32; }
bool GetBool(const Entry& e) { return e.slots[0]->u8 != 0; }
const char* GetString(const Entry& e) { return e.slots[0]->s ? e.slots[0]->s : ""; }

bool SetStringInPlace(Entry& e, const char* value)
{
    char* buf = e.slots[0]->s;
    if (!buf)
        return false;
    size_t cur = strlen(buf);
    size_t want = strlen(value);
    if (want > cur)
        return false; // would need reallocation with the engine's allocator; refuse

    DWORD oldProtect = 0;
    if (!VirtualProtect(buf, cur + 1, PAGE_READWRITE, &oldProtect))
        return false;
    memcpy(buf, value, want);
    buf[want] = '\0';
    VirtualProtect(buf, cur + 1, oldProtect, &oldProtect);

    // All slots for one setting share the same underlying buffer pointer, so writing
    // through slot 0's buffer updates every alias. Mark changed for the UI.
    e.changed = true;
    return true;
}

void SetFloat(Entry& e, float v)
{
    for (Data* d : e.slots)
        d->f32 = v;
    MarkChanged(e);
}

void SetInt(Entry& e, int v)
{
    for (Data* d : e.slots)
        d->s32 = v;
    MarkChanged(e);
}

void SetUInt(Entry& e, unsigned v)
{
    for (Data* d : e.slots)
        d->u32 = v;
    MarkChanged(e);
}

void SetBool(Entry& e, bool v)
{
    for (Data* d : e.slots)
        d->u8 = v ? 1 : 0;
    MarkChanged(e);
}

void Revert(Entry& e)
{
    for (Data* d : e.slots)
        *d = e.original;
    e.changed = false;
}

void RevertAll()
{
    for (Entry* e : s_resolved)
        if (e->changed)
            Revert(*e);
}

std::vector<Entry*> ChangedEntries()
{
    std::vector<Entry*> out;
    for (Entry* e : s_resolved)
        if (e->changed)
            out.push_back(e);
    return out;
}

}
