#pragma once

#include <cstdint>
#include <vector>

namespace vf::settings {

enum class Type { Bool, Int, UInt, Float, String, Unknown };

// Mirrors the engine's Setting::Data union (Setting layout: vptr @0x00, data @0x08, name @0x10).
union Data {
    uint32_t u32;
    int32_t s32;
    float f32;
    uint8_t u8; // bool
    char* s;
};

struct Entry {
    const char* fullName = nullptr; // "fTAAPostSharpen:Display" — points at the catalog literal
    Type type = Type::Unknown;
    std::vector<Data*> slots;       // every Setting object found for this name (normally one)
    Data original{};                // value captured at resolve time
    bool changed = false;
};

// One-time pass over the game image: locates every catalog setting's Setting object
// by finding its name string and the static object whose name pointer references it,
// validated by a vtable-in-.rdata check. Returns the number of settings resolved.
int ResolveAll();
bool Resolved();

Entry* Find(const char* fullName);
std::vector<Entry*>& AllResolved(); // sorted by name

Type TypeFromName(const char* fullName);

float GetFloat(const Entry& e);
void SetFloat(Entry& e, float v);
int GetInt(const Entry& e);
void SetInt(Entry& e, int v);
unsigned GetUInt(const Entry& e);
void SetUInt(Entry& e, unsigned v);
bool GetBool(const Entry& e);
void SetBool(Entry& e, bool v);
const char* GetString(const Entry& e);

// Writes a string value into the Setting's existing buffer without reallocating: only
// values no longer than the current buffer are allowed (empty always is), so the engine's
// allocator still owns the pointer and freeing stays correct. Returns false if the value
// is too long or the buffer can't be made writable. Used e.g. to blank intro-movie names.
bool SetStringInPlace(Entry& e, const char* value);

void Revert(Entry& e);
void RevertAll();
std::vector<Entry*> ChangedEntries();

}
