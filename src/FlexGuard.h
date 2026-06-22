#pragma once

// NVIDIA Flex (weapon debris) safety.
//
// Flex is a CUDA-accelerated solver shipped against the CUDA 7.5 runtime (cudart64_75.dll,
// 2015). That runtime supports compute capability ~5.x; anything from Pascal onward — and
// certainly Ada — cannot initialise it. Flex's context therefore stays null, and the first
// call that touches it faults:
//
//   flexRelease_x64!flexCreateTriangleMesh+0x3a   <- null-pointer write
//   (reached whenever the game registers new collision geometry, i.e. while walking)
//
// The engine never checks whether Flex initialised, so with bNVFlexEnable=1 the game dies
// within seconds of moving. Nothing in a plugin can make the solver work — this only makes
// sure the engine is never told to use it.
namespace vf::flexguard {

// Returns true if the machine actually has a usable Flex/CUDA stack. Currently a
// conservative check: the shipped CUDA runtime is too old for any modern GPU.
bool FlexLikelyUsable();

// If weapon debris is enabled, turns it off in the live setting (and reports it) before the
// engine can hand geometry to Flex. Call after the INIs have loaded, before gameplay.
// Returns true if it had to intervene.
bool EnforceSafety(bool persistToIni);

}
