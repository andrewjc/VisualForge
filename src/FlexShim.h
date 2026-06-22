#pragma once

// Interception layer for NVIDIA Flex.
//
// The engine reaches Flex through a plain C API imported from flexRelease_x64.dll and
// flexExtRelease_x64.dll. We redirect those imports in Fallout 4's import table to our own
// implementations, so the game never enters the (unusable) CUDA 7.5 solver.
//
// Two purposes:
//   1. Safety — the game can have weapon debris enabled without dying, because the calls
//      that fault are never made.
//   2. Discovery — it records which functions the engine actually calls, in what order and
//      how often. That is the prerequisite for replacing Flex with our own solver: the
//      public API is documented, but which subset Fallout 4 relies on, and with what
//      lifetime, has to be observed rather than assumed.
namespace vf::flexshim {

// Redirects the Flex imports. Safe to call before or after the DLLs load, since it patches
// the import table rather than the exports.
bool Install();

// Number of Flex calls intercepted this session (0 means the engine never used Flex).
int InterceptedCalls();
bool Installed();

// Live solver tuning. Because the physics is ours, these can be adjusted while the game is
// running and take effect on the next step — no restart, no INI editing, no rebuild.
struct Tunables {
    float gravityScale = 1.0f;      // multiplies the engine's own gravity
    float dragScale = 1.0f;         // multiplies the engine's damping
    float restitutionScale = 1.0f;  // bounciness
    float frictionScale = 1.0f;     // grip
    float spawnSpin = 12.0f;        // radians/sec given to a new chunk
    float spawnVelocityScale = 1.0f;// how hard impacts throw debris
    bool rolling = true;            // round pieces roll instead of only sliding
    float impactTorque = 1.0f;      // how much off-centre hits set pieces spinning
    float windScale = 0.0f;         // 0 = ignore wind entirely
};
extern Tunables g_tune;

// Live counters for the overlay.
int ActivePieces();
float LargestPieceRadius();

}
