#pragma once

#include <d3d11.h>
#include <dxgi.h>

// Final-frame post processing: a single fullscreen pass that applies color grading and then
// AMD FidelityFX Contrast Adaptive Sharpening to the backbuffer just before the overlay.
// Runs only when at least one of the two is active, and is a no-op if its shader fails to
// compile (logged), so it can never black-screen the game.
namespace vf::post {

struct Grade {
    bool enabled = false;
    float exposure = 0.0f;    // stops
    float contrast = 1.0f;    // 1 = neutral
    float saturation = 1.0f;  // 1 = neutral
    float vibrance = 0.0f;    // 0 = neutral (boosts unsaturated pixels)
    float temperature = 0.0f; // -1 warm .. +1 cool
    float tint = 0.0f;        // -1 magenta .. +1 green
    bool filmic = false;      // ACES-approx tonemap curve
};

extern Grade g_grade;
extern bool g_sharpenEnabled;
extern float g_sharpness; // 0..1

// 3D color LUT applied after grading.
extern bool g_lutEnabled;
extern float g_lutIntensity; // 0..1 blend

// Screen-space ambient occlusion / global illumination, reconstructed from captured depth.
// This is a screen-space approximation, not ray tracing: it only knows about what is
// currently visible on screen.
struct Ssao {
    bool enabled = false;
    float intensity = 1.0f;
    float radius = 60.0f; // world units
    float bias = 0.05f;
    float nearZ = 15.0f;  // projection near plane (engine fNearDistance)
    float farZ = 10000.0f;
    float fovDegrees = 70.0f;
    bool gi = false;      // gather one bounce of colour as well
    float giIntensity = 0.6f;
};
extern Ssao g_ssao;

// Panini projection (screen-space remap of the final frame).
struct Panini {
    bool enabled = false;
    float strength = 0.5f; // 0 = rectilinear (no change), 1 = full Panini
    float zoom = 1.0f;     // <1 pulls the view in, trading FOV for less edge sampling
};
extern Panini g_panini;

// Half-FOV tangent the engine must render at so Panini has real image data at the screen
// edge instead of smearing. Returns the required *horizontal* FOV in degrees for a given
// displayed FOV and strength.
float RequiredSourceFovDegrees(float displayedFovDegrees, float strength, float zoom);

// Debug visualization (session-only, not persisted). 0 = off, 1 = scene depth, 2 = AO.
extern int g_debugView;
extern float g_debugGamma; // shaping power for the depth view

// Non-owning depth SRV for this frame; set before Apply, cleared by passing nullptr.
void SetDepthSrv(ID3D11ShaderResourceView* srv);

void Apply(IDXGISwapChain* swap, ID3D11Device* device, ID3D11DeviceContext* ctx);
void OnResize();
void Shutdown();

// Uploads a size^3 RGBA-float 3D LUT (from Lut::ParseCube) as a Texture3D. Replaces any
// previous LUT. Returns false on a D3D failure. Resolution-independent — survives resizes.
bool SetLut(ID3D11Device* device, const float* rgba, int size);
void ClearLut();
int LutSize(); // 0 when none loaded

}
