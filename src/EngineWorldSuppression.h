#pragma once

#include <cstdint>

// The plugin-side state behind phase 25's draw-time suppression.
//
// The mirror publishes, once per frame at Present, whether the frame that
// follows may have its world draws dropped. The draw hooks read it thousands
// of times a frame and must not do more than an atomic load.
namespace vf::world_suppression {

// On by default; VISUALFORGE_SUPPRESS_WORLD=0 turns it off.
//
// Default-on because the engine drawing a world the mirror then overwrites is
// wasted work in every configuration. The cost is that a suppressed frame is
// incomplete by exactly the amount the mirror has yet to reproduce, so the
// opt-out exists for a run that needs the vanilla world to compare against.
[[nodiscard]] bool Enabled() noexcept;

// Reads the environment once and logs the resulting mode. Safe to call more
// than once; only the first call reads.
void Initialize() noexcept;

// Published at Present for the frame that follows. `worldReproduced` is
// whether the mirror actually put an image up: without it, suppression removes
// the world and puts nothing in its place.
void Publish(bool permitGrants, bool worldReproduced) noexcept;

[[nodiscard]] bool PermitGrants() noexcept;
[[nodiscard]] bool WorldReproduced() noexcept;

void NoteSuppressed() noexcept;
void NoteForwarded() noexcept;

struct Counters
{
    std::uint64_t suppressed{};
    std::uint64_t forwarded{};
};

[[nodiscard]] Counters Snapshot() noexcept;

}
