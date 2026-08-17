#include "EngineWorldSuppression.h"

#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <cstring>

namespace vf::world_suppression {

namespace {

std::atomic<bool> s_enabled{false};
std::atomic<bool> s_initialized{false};
// Relaxed throughout: these are read by the draw hooks on the engine's own
// threads and published once per frame from Present. A draw that reads the
// previous frame's value for a few instructions either forwards a world draw
// that could have been dropped or drops one a frame late, and neither changes
// the picture -- the frame boundary is not a synchronisation point for the
// engine's render threads, and making it one would cost more than it saves.
std::atomic<bool> s_permitGrants{false};
std::atomic<bool> s_worldReproduced{false};
std::atomic<std::uint64_t> s_suppressed{0};
std::atomic<std::uint64_t> s_forwarded{0};

// On unless explicitly turned off. The engine drawing a world that is then
// overwritten is pure waste, and leaving it on by default made every run
// measure two renderers. `VISUALFORGE_SUPPRESS_WORLD=0` restores the vanilla
// world for a run that wants to compare against it.
bool ReadEnvironmentFlag() noexcept
{
    char value[16]{};
    const auto length = ::GetEnvironmentVariableA(
        "VISUALFORGE_SUPPRESS_WORLD", value, sizeof(value));
    // Unset, or too long to be one of the words below, is the default.
    if (length == 0 || length >= sizeof(value)) return true;
    return !(_stricmp(value, "0") == 0 ||
        _stricmp(value, "false") == 0 ||
        _stricmp(value, "off") == 0);
}

}

void Initialize() noexcept
{
    auto expected = false;
    if (!s_initialized.compare_exchange_strong(expected, true,
            std::memory_order_relaxed)) {
        return;
    }
    const auto enabled = ReadEnvironmentFlag();
    s_enabled.store(enabled, std::memory_order_relaxed);
    // Stated in full rather than as a flag value, because a reader finding
    // this line in a performance log needs to know the picture was expected to
    // be incomplete without going to look it up.
    log::Write(enabled
        ? "renderer-suppression: on; world draws are dropped once the mirror "
          "is presenting, so the engine does not draw a world this renderer "
          "overwrites. The world is incomplete by whatever the mirror does not "
          "yet reproduce"
        : "renderer-suppression: off by VISUALFORGE_SUPPRESS_WORLD; vanilla "
          "draws the world and the mirror renders alongside it");
}

bool Enabled() noexcept
{
    return s_enabled.load(std::memory_order_relaxed);
}

void Publish(const bool permitGrants, const bool worldReproduced) noexcept
{
    s_permitGrants.store(permitGrants, std::memory_order_relaxed);
    s_worldReproduced.store(worldReproduced, std::memory_order_relaxed);
}

bool PermitGrants() noexcept
{
    return s_permitGrants.load(std::memory_order_relaxed);
}

bool WorldReproduced() noexcept
{
    return s_worldReproduced.load(std::memory_order_relaxed);
}

void NoteSuppressed() noexcept
{
    s_suppressed.fetch_add(1, std::memory_order_relaxed);
}

void NoteForwarded() noexcept
{
    s_forwarded.fetch_add(1, std::memory_order_relaxed);
}

Counters Snapshot() noexcept
{
    Counters counters{};
    counters.suppressed = s_suppressed.load(std::memory_order_relaxed);
    counters.forwarded = s_forwarded.load(std::memory_order_relaxed);
    return counters;
}

}
