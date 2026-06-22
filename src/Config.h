#pragma once

#include <string>

namespace vf::config {

// VisualForge.ini next to the plugin DLL (Data\F4SE\Plugins).
struct Values {
    int toggleKey = 0x79; // VK_F10
    bool basicMode = true; // overlay opens in Basic (simplified) mode by default
    bool skipIntroMovies = true;
    bool weaponDebrisCrashFix = true; // guard a null shader-resource view from the debris path
    // Force weapon debris off when NVIDIA Flex cannot run (CUDA 7.5 solver on a modern GPU).
    // Without this, bNVFlexEnable=1 crashes the game within seconds of walking.
    bool blockWeaponDebris = true;
    // Redirect the Flex API into the plugin instead of the broken CUDA 7.5 library.
    bool interceptFlex = true;

    // Weapon-debris solver tuning. Because the physics is ours these apply live.
    float debrisGravityScale = 1.0f;
    float debrisDragScale = 1.0f;
    float debrisRestitutionScale = 1.0f;
    float debrisFrictionScale = 1.0f;
    float debrisSpawnSpin = 12.0f;
    float debrisImpactTorque = 1.0f;
    bool debrisRolling = true;

    // Post processing (final-frame pass).
    bool sharpenEnabled = false;
    float sharpness = 0.4f; // 0..1

    bool gradeEnabled = false;
    float exposure = 0.0f;    // stops
    float contrast = 1.0f;
    float saturation = 1.0f;
    float vibrance = 0.0f;
    float temperature = 0.0f; // -1 warm .. +1 cool
    float tint = 0.0f;        // -1 magenta .. +1 green
    bool filmic = false;

    bool lutEnabled = false;
    float lutIntensity = 1.0f;
    char lutFile[128] = {}; // filename within the LUTs folder, "" = none

    // Screen-space ambient occlusion / GI (depth-based approximation).
    bool ssaoEnabled = false;
    float ssaoIntensity = 1.0f;
    float ssaoRadius = 60.0f;
    float ssaoBias = 0.05f;
    float ssaoNear = 15.0f; // matches the engine's fNearDistance
    float ssaoFar = 10000.0f;
    float ssaoFov = 70.0f;
    bool giEnabled = false;
    float giIntensity = 0.6f;

    // Panini projection. `paniniDisplayFov` is the FOV you want to *see*; the engine is
    // asked to render wider than that so the warp has real pixels at the screen edge.
    bool paniniEnabled = false;
    float paniniStrength = 0.5f;
    float paniniZoom = 1.0f;
    float paniniDisplayFov = 80.0f;

    // 0 = snapshot before the depth clear (correct for this engine), 1 = copy at Present.
    // Mode 1 does not work on Fallout 4: it clears depth again for the UI pass, so by
    // Present the buffer is wiped. Kept only as a diagnostic alternative.
    int depthCaptureMode = 0;

    // Diagnostic CPU readback of the depth buffer. Allocates a full-resolution staging
    // texture and stalls the GPU, so it stays off during normal play.
    bool depthSelfTest = false;
};

Values& Get();
void Load();
void Save();

// Pushes the post-processing values into the render module's live state.
void ApplyToPost();

// Directory containing the plugin DLL (with trailing backslash). Stable after first call.
const wchar_t* PluginDir();

// Writes the FOV Panini needs into [Display] of Fallout4Custom.ini — the section the engine
// actually reads (values under [Interface] are silently ignored). Pass 0 to remove the
// override and restore the game's own FOV. Returns false with a reason in `err`.
bool WriteEngineFov(float worldFov, float firstPersonFov, std::string& err);

}
