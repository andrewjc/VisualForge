#include "Config.h"
#include "PostProcess.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ShlObj.h>

#include <cstdio>
#include <cstdlib>

namespace vf::config {

static Values s_values;
static wchar_t s_path[MAX_PATH] = {};
static wchar_t s_dir[MAX_PATH] = {};

Values& Get()
{
    return s_values;
}

static void ResolvePath()
{
    if (s_path[0])
        return;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ResolvePath), &self);
    GetModuleFileNameW(self, s_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(s_dir, L'\\');
    if (slash)
        *(slash + 1) = 0;
    swprintf_s(s_path, L"%sVisualForge.ini", s_dir);
}

const wchar_t* PluginDir()
{
    ResolvePath();
    return s_dir;
}

static float ReadFloat(const wchar_t* section, const wchar_t* key, float fallback)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(section, key, L"", buf, 64, s_path);
    return buf[0] ? float(_wtof(buf)) : fallback;
}

static void WriteFloat(const wchar_t* section, const wchar_t* key, float value)
{
    wchar_t buf[64];
    swprintf_s(buf, L"%.4f", value);
    WritePrivateProfileStringW(section, key, buf, s_path);
}

static void WriteBool(const wchar_t* section, const wchar_t* key, bool value)
{
    WritePrivateProfileStringW(section, key, value ? L"1" : L"0", s_path);
}

void Load()
{
    ResolvePath();
    s_values.toggleKey = int(GetPrivateProfileIntW(L"Overlay", L"iToggleKey", s_values.toggleKey, s_path));
    s_values.basicMode = GetPrivateProfileIntW(L"Overlay", L"bBasicMode", s_values.basicMode ? 1 : 0, s_path) != 0;
    s_values.skipIntroMovies = GetPrivateProfileIntW(L"Startup", L"bSkipIntroMovies", s_values.skipIntroMovies ? 1 : 0, s_path) != 0;

    s_values.sharpenEnabled = GetPrivateProfileIntW(L"Sharpen", L"bEnabled", s_values.sharpenEnabled ? 1 : 0, s_path) != 0;
    s_values.sharpness = ReadFloat(L"Sharpen", L"fSharpness", s_values.sharpness);

    s_values.gradeEnabled = GetPrivateProfileIntW(L"Grade", L"bEnabled", s_values.gradeEnabled ? 1 : 0, s_path) != 0;
    s_values.exposure = ReadFloat(L"Grade", L"fExposure", s_values.exposure);
    s_values.contrast = ReadFloat(L"Grade", L"fContrast", s_values.contrast);
    s_values.saturation = ReadFloat(L"Grade", L"fSaturation", s_values.saturation);
    s_values.vibrance = ReadFloat(L"Grade", L"fVibrance", s_values.vibrance);
    s_values.temperature = ReadFloat(L"Grade", L"fTemperature", s_values.temperature);
    s_values.tint = ReadFloat(L"Grade", L"fTint", s_values.tint);
    s_values.filmic = GetPrivateProfileIntW(L"Grade", L"bFilmic", s_values.filmic ? 1 : 0, s_path) != 0;

    s_values.lutEnabled = GetPrivateProfileIntW(L"Grade", L"bLut", s_values.lutEnabled ? 1 : 0, s_path) != 0;
    s_values.lutIntensity = ReadFloat(L"Grade", L"fLutIntensity", s_values.lutIntensity);
    {
        wchar_t w[128];
        GetPrivateProfileStringW(L"Grade", L"sLutFile", L"", w, 128, s_path);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s_values.lutFile, sizeof(s_values.lutFile), nullptr, nullptr);
    }

    s_values.ssaoEnabled = GetPrivateProfileIntW(L"SSAO", L"bEnabled", s_values.ssaoEnabled ? 1 : 0, s_path) != 0;
    s_values.ssaoIntensity = ReadFloat(L"SSAO", L"fIntensity", s_values.ssaoIntensity);
    s_values.ssaoRadius = ReadFloat(L"SSAO", L"fRadius", s_values.ssaoRadius);
    s_values.ssaoBias = ReadFloat(L"SSAO", L"fBias", s_values.ssaoBias);
    s_values.ssaoNear = ReadFloat(L"SSAO", L"fNearZ", s_values.ssaoNear);
    s_values.ssaoFar = ReadFloat(L"SSAO", L"fFarZ", s_values.ssaoFar);
    s_values.ssaoFov = ReadFloat(L"SSAO", L"fFovDegrees", s_values.ssaoFov);
    s_values.giEnabled = GetPrivateProfileIntW(L"SSAO", L"bGI", s_values.giEnabled ? 1 : 0, s_path) != 0;
    s_values.giIntensity = ReadFloat(L"SSAO", L"fGIIntensity", s_values.giIntensity);

    // Debug view is normally a session-only overlay toggle, but it can be forced from the
    // INI (0 = normal, 1 = scene depth, 2 = ambient occlusion) for troubleshooting.
    post::g_debugView = int(GetPrivateProfileIntW(L"Debug", L"iView", 0, s_path));
    s_values.depthCaptureMode = int(GetPrivateProfileIntW(L"Debug", L"iDepthCaptureMode",
                                                          s_values.depthCaptureMode, s_path));
    s_values.depthSelfTest = GetPrivateProfileIntW(L"Debug", L"bDepthSelfTest", 0, s_path) != 0;

    s_values.paniniEnabled = GetPrivateProfileIntW(L"Panini", L"bEnablePanini", s_values.paniniEnabled ? 1 : 0, s_path) != 0;
    s_values.paniniStrength = ReadFloat(L"Panini", L"fStrength", s_values.paniniStrength);
    s_values.paniniZoom = ReadFloat(L"Panini", L"fZoom", s_values.paniniZoom);
    s_values.paniniDisplayFov = ReadFloat(L"Panini", L"fDisplayFov", s_values.paniniDisplayFov);
    if (s_values.paniniStrength < 0.0f) s_values.paniniStrength = 0.0f;
    if (s_values.paniniStrength > 1.0f) s_values.paniniStrength = 1.0f;

    if (s_values.sharpness < 0.0f) s_values.sharpness = 0.0f;
    if (s_values.sharpness > 1.0f) s_values.sharpness = 1.0f;
    if (s_values.lutIntensity < 0.0f) s_values.lutIntensity = 0.0f;
    if (s_values.lutIntensity > 1.0f) s_values.lutIntensity = 1.0f;
}

void Save()
{
    ResolvePath();
    wchar_t buf[64];
    swprintf_s(buf, L"%d", s_values.toggleKey);
    WritePrivateProfileStringW(L"Overlay", L"iToggleKey", buf, s_path);
    WriteBool(L"Overlay", L"bBasicMode", s_values.basicMode);
    WriteBool(L"Startup", L"bSkipIntroMovies", s_values.skipIntroMovies);

    WriteBool(L"Sharpen", L"bEnabled", s_values.sharpenEnabled);
    WriteFloat(L"Sharpen", L"fSharpness", s_values.sharpness);

    WriteBool(L"Grade", L"bEnabled", s_values.gradeEnabled);
    WriteFloat(L"Grade", L"fExposure", s_values.exposure);
    WriteFloat(L"Grade", L"fContrast", s_values.contrast);
    WriteFloat(L"Grade", L"fSaturation", s_values.saturation);
    WriteFloat(L"Grade", L"fVibrance", s_values.vibrance);
    WriteFloat(L"Grade", L"fTemperature", s_values.temperature);
    WriteFloat(L"Grade", L"fTint", s_values.tint);
    WriteBool(L"Grade", L"bFilmic", s_values.filmic);

    WriteBool(L"Grade", L"bLut", s_values.lutEnabled);
    WriteFloat(L"Grade", L"fLutIntensity", s_values.lutIntensity);
    {
        wchar_t w[128];
        MultiByteToWideChar(CP_UTF8, 0, s_values.lutFile, -1, w, 128);
        WritePrivateProfileStringW(L"Grade", L"sLutFile", w, s_path);
    }

    WriteBool(L"SSAO", L"bEnabled", s_values.ssaoEnabled);
    WriteFloat(L"SSAO", L"fIntensity", s_values.ssaoIntensity);
    WriteFloat(L"SSAO", L"fRadius", s_values.ssaoRadius);
    WriteFloat(L"SSAO", L"fBias", s_values.ssaoBias);
    WriteFloat(L"SSAO", L"fNearZ", s_values.ssaoNear);
    WriteFloat(L"SSAO", L"fFarZ", s_values.ssaoFar);
    WriteFloat(L"SSAO", L"fFovDegrees", s_values.ssaoFov);
    WriteBool(L"SSAO", L"bGI", s_values.giEnabled);
    WriteFloat(L"SSAO", L"fGIIntensity", s_values.giIntensity);

    WriteBool(L"Panini", L"bEnablePanini", s_values.paniniEnabled);
    WriteFloat(L"Panini", L"fStrength", s_values.paniniStrength);
    WriteFloat(L"Panini", L"fZoom", s_values.paniniZoom);
    WriteFloat(L"Panini", L"fDisplayFov", s_values.paniniDisplayFov);
}

bool WriteEngineFov(float worldFov, float firstPersonFov, std::string& err)
{
    PWSTR docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        err = "could not locate Documents folder";
        return false;
    }
    wchar_t custom[MAX_PATH];
    swprintf_s(custom, L"%s\\My Games\\Fallout4\\Fallout4Custom.ini", docs);
    CoTaskMemFree(docs);

    // WritePrivateProfileString creates the [Display] section if it is missing and replaces
    // the key in place if it already exists, so repeated writes stay clean.
    auto put = [&](const wchar_t* key, float value) {
        if (value <= 0.0f) {
            WritePrivateProfileStringW(L"Display", key, nullptr, custom); // delete override
            return;
        }
        wchar_t buf[32];
        swprintf_s(buf, L"%.4f", value);
        WritePrivateProfileStringW(L"Display", key, buf, custom);
    };

    put(L"fDefaultWorldFOV", worldFov);
    put(L"fDefault1stPersonFOV", firstPersonFov);

    if (!WritePrivateProfileStringW(nullptr, nullptr, nullptr, custom)) { // flush
        err = "could not write Fallout4Custom.ini";
        return false;
    }
    return true;
}

void ApplyToPost()
{
    post::g_sharpenEnabled = s_values.sharpenEnabled;
    post::g_sharpness = s_values.sharpness;

    post::g_grade.enabled = s_values.gradeEnabled;
    post::g_grade.exposure = s_values.exposure;
    post::g_grade.contrast = s_values.contrast;
    post::g_grade.saturation = s_values.saturation;
    post::g_grade.vibrance = s_values.vibrance;
    post::g_grade.temperature = s_values.temperature;
    post::g_grade.tint = s_values.tint;
    post::g_grade.filmic = s_values.filmic;

    post::g_lutEnabled = s_values.lutEnabled;
    post::g_lutIntensity = s_values.lutIntensity;

    post::g_ssao.enabled = s_values.ssaoEnabled;
    post::g_ssao.intensity = s_values.ssaoIntensity;
    post::g_ssao.radius = s_values.ssaoRadius;
    post::g_ssao.bias = s_values.ssaoBias;
    post::g_ssao.nearZ = s_values.ssaoNear;
    post::g_ssao.farZ = s_values.ssaoFar;
    post::g_ssao.fovDegrees = s_values.ssaoFov;
    post::g_ssao.gi = s_values.giEnabled;
    post::g_ssao.giIntensity = s_values.giIntensity;


    post::g_panini.enabled = s_values.paniniEnabled;
    post::g_panini.strength = s_values.paniniStrength;
    post::g_panini.zoom = s_values.paniniZoom;

    // The warp reads view angles from the same projection the SSAO uses, so keep the FOV
    // it works in equal to the FOV the engine is actually rendering.
    post::g_ssao.fovDegrees = s_values.ssaoFov;
}

}
