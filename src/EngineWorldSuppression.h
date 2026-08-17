#pragma once

#include <cstdint>

// The plugin-side state behind phase 25's draw-time suppression.
//
// The mirror publishes, once per frame at Present, whether the frame that
// follows may have its world draws dropped. The draw hooks read it thousands
// of times a frame and must not do more than an atomic load.
namespace vf::world_suppression {

// Off unless the process was started with VISUALFORGE_SUPPRESS_WORLD=1.
//
// Opt-in because suppression trades a complete picture for an honest
// measurement: the mirror reproduces a subset of the world, so a suppressed
// frame is visibly incomplete by exactly the amount the mirror has yet to
// cover. That is the correct configuration for measuring this renderer and the
// wrong one for looking at the game.
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
