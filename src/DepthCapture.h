#pragma once

#include <d3d11.h>

// Captures the engine's main scene depth buffer so post-process passes can read it.
//
// The engine never hands us depth directly, so we watch every depth-stencil view it binds
// (ID3D11DeviceContext::OMSetRenderTargets) and remember the one matching the backbuffer
// size — that is the main scene depth. Once per frame we copy it into our own
// shader-readable texture, because a resource cannot be bound for depth-write and sampled
// at the same time.
namespace vf::depth {

bool Install(ID3D11Device* device, ID3D11DeviceContext* ctx);
void Shutdown();

// Tells the tracker which resolution counts as "full screen" (the backbuffer size).
void SetTargetSize(UINT width, UINT height);

// Capture strategy:
//   0 = snapshot inside the ClearDepthStencilView hook (on whatever context clears it)
//   1 = copy at Present from the tracked depth texture (always the immediate context)
// Both are kept because the engine renders multi-threaded and the correct one has to be
// determined empirically rather than assumed.
void SetCaptureMode(int mode);
int CaptureMode();

// Depth is only copied when something actually consumes it. With every consumer off the
// hooks fall through immediately, so the plugin costs nothing and touches no resources.
void SetWanted(bool wanted);

// The CPU readback self-test is a diagnostic: it allocates a full-resolution staging
// texture and stalls the GPU, so it is off unless explicitly enabled.
void SetSelfTestEnabled(bool enabled);

// Refreshes the copy from the most recent full-res depth target and returns an SRV for it,
// or nullptr when no depth has been seen yet. Call once per frame before the post pass.
ID3D11ShaderResourceView* Acquire(ID3D11Device* device, ID3D11DeviceContext* ctx);

// Drops size-dependent resources (call from the ResizeBuffers hook).
void OnResize();

// Whether the depth-stencil view currently bound on the calling thread is the
// main scene depth. This is the world-target classification the phase 25 draw
// path needs: the engine draws the world with the scene depth bound and the
// interface without it, so it separates the draws Vulkan reproduces from the
// draws it does not.
//
// Thread-local because the engine renders on several threads through deferred
// contexts, and the binding that matters to a draw is the one its own thread
// made. Answers false until the scene depth has been identified, which keeps
// an unclassified frame on the vanilla path.
bool SceneDepthBound();

bool Installed();
bool HaveDepth();
// Format of the captured depth texture, for diagnostics (DXGI_FORMAT_UNKNOWN if none).
DXGI_FORMAT CapturedFormat();

}
