#include "Overlay.h"
#include "Config.h"
#include "DepthCapture.h"
#include "EngineSettings.h"
#include "IniWriter.h"
#include "Log.h"
#include "Lut.h"
#include "PostProcess.h"
#include "SettingsCatalog.h"

#include <imgui.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>
#include <string>
#include <vector>

namespace vf::overlay {

bool g_visible = false;

namespace {

using settings::Entry;
using settings::Type;

struct QuickItem {
    const char* label;
    const char* fullName;
    float min;
    float max;
    const char* tooltip;
};

struct QuickGroup {
    const char* title;
    const QuickItem* items;
    int count;
};

// Curated knobs. Ranges are sane tuning bounds, not engine limits.
const QuickItem kShadowItems[] = {
    { "Shadow distance", "fShadowDistance:Display", 1000, 40000, "Directional shadow draw distance. Applied on area reload." },
    { "Dir shadow distance", "fDirShadowDistance:Display", 1000, 40000, nullptr },
    { "Shadow cascades", "iDirShadowSplits:Display", 1, 3, "Number of cascade splits for the sun shadow map." },
    { "Cascade blend", "fBlendSplitDirShadow:Display", 0, 128, "Blend width between cascade splits." },
    { "Shadow bias", "fShadowBiasScale:Display", 0, 2, "Lower = tighter contact shadows, risk of acne." },
    { "Focus shadow distance", "fMaxFocusShadowMapDistance:Display", 100, 3000, "High-res spotlight shadows range." },
    { "Max focus shadows", "iMaxFocusShadows:Display", 0, 8, nullptr },
    { "NPC light shadows", "bAllowShadowcasterNPCLights:Display", 0, 1, "Shadows from NPC-carried lights." },
    { "Actor self shadowing", "bActorSelfShadowing:Display", 0, 1, nullptr },
    { "Sun update threshold", "fSunUpdateThreshold:Display", 0.01f, 2, "Lower = smoother sun shadow movement." },
    { "Sun shadow update time", "fSunShadowUpdateTime:Display", 0.05f, 2, "Duration of each sun shadow transition." },
};

const QuickItem kTAAItems[] = {
    { "TAA post sharpen", "fTAAPostSharpen:Display", 0, 1, "Sharpening applied after temporal AA." },
    { "TAA sharpen", "fTAASharpen:Display", 0, 1, nullptr },
    { "TAA high freq weight", "fTAAHighFreq:Display", 0, 1, "Higher preserves more fine detail." },
    { "TAA low freq weight", "fTAALowFreq:Display", 0, 1, nullptr },
    { "TAA post overlay", "fTAAPostOverlay:Display", 0, 1, nullptr },
    { "Mip LOD bias", "fMipBias:Display", -2, 1, "Negative sharpens textures; TAA hides the shimmer. Applies to newly streamed textures." },
};

const QuickItem kGodrayItems[] = {
    { "Volumetric lighting", "bVolumetricLightingEnable:Display", 0, 1, "NVIDIA godrays master toggle." },
    { "Volumetric quality", "iVolumetricLightingQuality:Display", 0, 3, "0=low .. 3=ultra." },
    { "Force casters", "bVolumetricLightingForceCasters:Display", 0, 1, "More geometry participates in volumetric shadowing." },
};

const QuickItem kAOItems[] = {
    { "SAO enable", "bSAOEnable:Display", 0, 1, nullptr },
    { "SAO radius", "fSAORadius:Display", 10, 300, nullptr },
    { "SAO intensity", "fSAOIntensity:Display", 0, 20, nullptr },
    { "SAO bias", "fSAOBias:Display", 0, 2, nullptr },
    { "HBAO+ enable", "bEnable:NVHBAO", 0, 1, "NVIDIA HBAO+ (replaces SAO when on)." },
    { "HBAO+ radius", "fRadius:NVHBAO", 0.1f, 4, nullptr },
    { "HBAO+ power", "fPowerExponent:NVHBAO", 1, 8, nullptr },
    { "HBAO+ detail", "fDetailAO:NVHBAO", 0, 2, nullptr },
    { "HBAO+ coarse", "fCoarseAO:NVHBAO", 0, 2, nullptr },
};

const QuickItem kGrassLODItems[] = {
    { "Grass fade distance", "fGrassStartFadeDistance:Grass", 1000, 20000, nullptr },
    { "Min grass size", "iMinGrassSize:Grass", 5, 60, "Lower = denser grass. Applied on area reload." },
    { "Object LOD fade", "fLODFadeOutMultObjects:LOD", 1, 40, nullptr },
    { "Item LOD fade", "fLODFadeOutMultItems:LOD", 1, 40, nullptr },
    { "Actor LOD fade", "fLODFadeOutMultActors:LOD", 1, 40, nullptr },
    { "Tree load distance", "fTreeLoadDistance:TerrainManager", 20000, 200000, nullptr },
    { "Terrain block max", "fBlockMaximumDistance:TerrainManager", 50000, 500000, nullptr },
    { "Terrain split mult", "fSplitDistanceMult:TerrainManager", 0.5f, 3, "Higher = more detailed distant terrain mesh." },
};

const QuickItem kImageItems[] = {
    { "Depth of field", "bDoDepthOfField:Imagespace", 0, 1, nullptr },
    { "Bokeh DOF", "bScreenSpaceBokeh:ImageSpace", 0, 1, "Higher-quality DOF path." },
    { "Motion blur", "bMBEnable:Imagespace", 0, 1, nullptr },
    { "Lens flare", "bLensFlare:ImageSpace", 0, 1, nullptr },
    { "Screen-space reflections", "bScreenSpaceReflections:LightingShader", 0, 1, nullptr },
    { "SSR intensity", "fSSLRIntensity:SSLR", 0, 4, nullptr },
    { "Subsurface scattering", "bScreenSpaceSubsurfaceScattering:LightingShader", 0, 1, nullptr },
    { "Wetness materials", "bEnableWetnessMaterials:Display", 0, 1, nullptr },
    { "Rain occlusion", "bEnableRainOcclusion:Display", 0, 1, nullptr },
};

const QuickGroup kGroups[] = {
    { "Shadows", kShadowItems, int(sizeof(kShadowItems) / sizeof(QuickItem)) },
    { "TAA & Sharpness", kTAAItems, int(sizeof(kTAAItems) / sizeof(QuickItem)) },
    { "Godrays", kGodrayItems, int(sizeof(kGodrayItems) / sizeof(QuickItem)) },
    { "Ambient Occlusion", kAOItems, int(sizeof(kAOItems) / sizeof(QuickItem)) },
    { "Grass / LOD / Terrain", kGrassLODItems, int(sizeof(kGrassLODItems) / sizeof(QuickItem)) },
    { "Image Space & Reflections", kImageItems, int(sizeof(kImageItems) / sizeof(QuickItem)) },
};

char s_filter[128] = {};
std::vector<int> s_filtered;   // indices into the catalog
bool s_filterDirty = true;
std::string s_status;

bool ContainsI(const char* haystack, const char* needle)
{
    if (!*needle)
        return true;
    size_t nlen = strlen(needle);
    for (const char* p = haystack; *p; ++p) {
        if (_strnicmp(p, needle, nlen) == 0)
            return true;
    }
    return false;
}

void RebuildFilter()
{
    s_filtered.clear();
    for (int i = 0; i < kSettingCatalogCount; ++i)
        if (ContainsI(kSettingCatalog[i], s_filter))
            s_filtered.push_back(i);
    s_filterDirty = false;
}

// Draws the editor control for a resolved entry. Returns true when modified this frame.
bool DrawEntryControl(Entry& e, const char* id, float minV = 0, float maxV = 0)
{
    ImGui::PushID(id);
    bool modified = false;
    switch (e.type) {
        case Type::Bool: {
            bool v = settings::GetBool(e);
            if (ImGui::Checkbox("##v", &v)) {
                settings::SetBool(e, v);
                modified = true;
            }
            break;
        }
        case Type::Float: {
            float v = settings::GetFloat(e);
            bool hasRange = maxV > minV;
            if (hasRange ? ImGui::SliderFloat("##v", &v, minV, maxV, "%.4f")
                         : ImGui::DragFloat("##v", &v, 1.0f, 0, 0, "%.4f")) {
                settings::SetFloat(e, v);
                modified = true;
            }
            break;
        }
        case Type::Int: {
            int v = settings::GetInt(e);
            bool hasRange = maxV > minV;
            if (hasRange ? ImGui::SliderInt("##v", &v, int(minV), int(maxV))
                         : ImGui::DragInt("##v", &v)) {
                settings::SetInt(e, v);
                modified = true;
            }
            break;
        }
        case Type::UInt: {
            int v = int(settings::GetUInt(e));
            bool hasRange = maxV > minV;
            if (hasRange ? ImGui::SliderInt("##v", &v, int(minV), int(maxV))
                         : ImGui::DragInt("##v", &v, 1.0f, 0, INT_MAX)) {
                settings::SetUInt(e, unsigned(v < 0 ? 0 : v));
                modified = true;
            }
            break;
        }
        default:
            ImGui::TextDisabled("%s", settings::GetString(e));
            break;
    }
    if (e.changed) {
        ImGui::SameLine();
        if (ImGui::SmallButton("revert"))
            settings::Revert(e);
    }
    ImGui::PopID();
    return modified;
}

void DrawQuickTab()
{
    for (const QuickGroup& group : kGroups) {
        if (!ImGui::CollapsingHeader(group.title, ImGuiTreeNodeFlags_DefaultOpen))
            continue;
        ImGui::PushID(group.title);
        if (ImGui::BeginTable("t", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.42f);
            ImGui::TableSetupColumn("ctrl", ImGuiTableColumnFlags_WidthStretch, 0.58f);
            for (int i = 0; i < group.count; ++i) {
                const QuickItem& item = group.items[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(item.label);
                if (item.tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                    ImGui::SetTooltip("%s\n(%s)", item.tooltip, item.fullName);
                else if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                    ImGui::SetTooltip("%s", item.fullName);
                ImGui::TableNextColumn();
                Entry* e = settings::Find(item.fullName);
                if (e) {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    DrawEntryControl(*e, item.fullName, item.min, item.max);
                } else {
                    ImGui::TextDisabled("not found");
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
}

void DrawBrowserTab()
{
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##filter", "filter — e.g. shadow, :Water, fTAA", s_filter, sizeof(s_filter)))
        s_filterDirty = true;
    if (s_filterDirty)
        RebuildFilter();

    ImGui::TextDisabled("%zu / %d settings", s_filtered.size(), kSettingCatalogCount);
    ImGui::BeginChild("list", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGuiListClipper clipper;
    clipper.Begin(int(s_filtered.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const char* name = kSettingCatalog[s_filtered[row]];
            Entry* e = settings::Find(name);
            ImGui::PushID(row);
            if (ImGui::BeginTable("r", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("n", ImGuiTableColumnFlags_WidthStretch, 0.55f);
                ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthStretch, 0.45f);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (e && e->changed)
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", name);
                else if (e)
                    ImGui::TextUnformatted(name);
                else
                    ImGui::TextDisabled("%s", name);
                ImGui::TableNextColumn();
                if (e) {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    DrawEntryControl(*e, name);
                } else {
                    ImGui::TextDisabled("unresolved");
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

std::vector<std::string> s_lutFiles;
bool s_lutScanned = false;

void ScanLuts()
{
    s_lutFiles.clear();
    wchar_t pattern[MAX_PATH];
    swprintf_s(pattern, L"%sLUTs\\*.cube", config::PluginDir());
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            char name[128];
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), nullptr, nullptr);
            s_lutFiles.emplace_back(name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    s_lutScanned = true;
}

void DrawLutSection(ID3D11Device* device)
{
    if (!ImGui::CollapsingHeader("Cinematic LUT"))
        return;
    auto& cfg = config::Get();
    if (!s_lutScanned)
        ScanLuts();

    ImGui::TextDisabled("Drop .cube files in Data\\F4SE\\Plugins\\LUTs\\");
    if (ImGui::Button("Rescan"))
        ScanLuts();
    ImGui::SameLine();
    ImGui::Text("%d found", int(s_lutFiles.size()));

    const char* current = cfg.lutFile[0] ? cfg.lutFile : "(none)";
    if (ImGui::BeginCombo("LUT file", current)) {
        for (const std::string& name : s_lutFiles) {
            bool sel = strcmp(name.c_str(), cfg.lutFile) == 0;
            if (ImGui::Selectable(name.c_str(), sel)) {
                wchar_t wname[128], full[MAX_PATH];
                MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wname, 128);
                swprintf_s(full, L"%sLUTs\\%s", config::PluginDir(), wname);
                std::string err;
                if (lut::LoadInto(device, full, err)) {
                    strncpy_s(cfg.lutFile, name.c_str(), _TRUNCATE);
                    cfg.lutEnabled = true;
                    config::ApplyToPost();
                    config::Save();
                    s_status = "LUT loaded: " + name;
                } else {
                    s_status = "LUT error: " + err;
                }
            }
        }
        ImGui::EndCombo();
    }

    bool dirty = false;
    ImGui::BeginDisabled(post::LutSize() == 0);
    dirty |= ImGui::Checkbox("Enable LUT", &cfg.lutEnabled);
    ImGui::SetNextItemWidth(240);
    dirty |= ImGui::SliderFloat("LUT intensity", &cfg.lutIntensity, 0.0f, 1.0f, "%.2f");
    ImGui::EndDisabled();
    if (post::LutSize() > 0)
        ImGui::TextDisabled("Active LUT: %d^3", post::LutSize());

    if (ImGui::Button("Clear LUT")) {
        post::ClearLut();
        cfg.lutFile[0] = '\0';
        cfg.lutEnabled = false;
        dirty = true;
        s_status = "LUT cleared";
    }
    if (dirty) {
        config::ApplyToPost();
        config::Save();
    }
}

void DrawSsaoSection()
{
    if (!ImGui::CollapsingHeader("Ambient Occlusion / GI (screen-space)"))
        return;
    auto& cfg = config::Get();

    if (!depth::HaveDepth()) {
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
                           "Waiting for scene depth — load a save or enter the world.");
    }
    ImGui::TextWrapped(
        "Approximates contact shadows and one bounce of local light from the depth buffer. "
        "Screen-space only: it cannot know about anything off screen. This is not ray tracing.");
    ImGui::Spacing();

    bool dirty = false;
    dirty |= ImGui::Checkbox("Enable ambient occlusion", &cfg.ssaoEnabled);
    ImGui::BeginDisabled(!cfg.ssaoEnabled);
    ImGui::SetNextItemWidth(240);
    dirty |= ImGui::SliderFloat("Strength", &cfg.ssaoIntensity, 0.0f, 3.0f, "%.2f");
    ImGui::SetNextItemWidth(240);
    dirty |= ImGui::SliderFloat("Radius (world units)", &cfg.ssaoRadius, 5.0f, 300.0f, "%.0f");
    ImGui::SetNextItemWidth(240);
    dirty |= ImGui::SliderFloat("Bias", &cfg.ssaoBias, 0.0f, 0.3f, "%.3f");

    ImGui::Spacing();
    dirty |= ImGui::Checkbox("Add bounce light (GI)", &cfg.giEnabled);
    ImGui::BeginDisabled(!cfg.giEnabled);
    ImGui::SetNextItemWidth(240);
    dirty |= ImGui::SliderFloat("Bounce strength", &cfg.giIntensity, 0.0f, 2.0f, "%.2f");
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (ImGui::TreeNode("Projection (affects AO scale)")) {
        ImGui::TextDisabled("Defaults were derived from this build's depth values.");
        ImGui::SetNextItemWidth(200);
        dirty |= ImGui::DragFloat("Near plane", &cfg.ssaoNear, 0.5f, 1.0f, 200.0f, "%.1f");
        ImGui::SetNextItemWidth(200);
        dirty |= ImGui::DragFloat("Far plane", &cfg.ssaoFar, 50.0f, 500.0f, 200000.0f, "%.0f");
        ImGui::SetNextItemWidth(200);
        dirty |= ImGui::SliderFloat("FOV (horizontal)", &cfg.ssaoFov, 40.0f, 120.0f, "%.0f");
        ImGui::TreePop();
    }

    int dbg = post::g_debugView;
    if (ImGui::RadioButton("Normal view", dbg == 0)) post::g_debugView = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Show AO only", dbg == 2)) post::g_debugView = 2;

    if (dirty) {
        config::ApplyToPost();
        config::Save();
    }
}

void DrawPaniniSection()
{
    if (!ImGui::CollapsingHeader("Panini projection (ultrawide edge correction)"))
        return;
    auto& cfg = config::Get();

    ImGui::TextWrapped(
        "Rectilinear projection stretches the edges of a wide view. Panini trades that "
        "stretch for slightly curved horizontal lines, keeping verticals straight.");
    ImGui::Spacing();

    bool dirty = false;
    dirty |= ImGui::Checkbox("Enable Panini", &cfg.paniniEnabled);
    ImGui::BeginDisabled(!cfg.paniniEnabled);
    ImGui::SetNextItemWidth(240);
    dirty |= ImGui::SliderFloat("Strength", &cfg.paniniStrength, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("0 = unchanged (pure rectilinear), 1 = full Panini.");
    ImGui::SetNextItemWidth(240);
    dirty |= ImGui::SliderFloat("Zoom", &cfg.paniniZoom, 0.6f, 1.2f, "%.2f");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("Below 1 pulls the image in, needing less FOV headroom.");
    ImGui::SetNextItemWidth(240);
    dirty |= ImGui::SliderFloat("FOV you want to see", &cfg.paniniDisplayFov, 60.0f, 120.0f, "%.0f");
    ImGui::EndDisabled();

    // The engine has to render wider than the displayed FOV, or the screen edge has no
    // real pixels to pull from and smears.
    const float required = post::RequiredSourceFovDegrees(
        cfg.paniniDisplayFov, cfg.paniniStrength, cfg.paniniZoom);

    ImGui::Spacing();
    ImGui::SeparatorText("Engine FOV compensation (required)");
    ImGui::Text("Render FOV needed: %.1f deg   (displaying %.0f)", required, cfg.paniniDisplayFov);

    Entry* worldFov = settings::Find("fDefaultWorldFOV:Display");
    if (worldFov)
        ImGui::Text("Engine is currently rendering at: %.1f deg", settings::GetFloat(*worldFov));

    if (required > 150.0f)
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f),
                           "That FOV is impractically wide — lower Strength or Zoom.");

    if (ImGui::Button("Apply FOV to game (next launch)")) {
        std::string err;
        if (config::WriteEngineFov(required, required, err)) {
            cfg.ssaoFov = required;   // keep our projection math in sync with the engine
            config::ApplyToPost();
            config::Save();
            s_status = "Wrote fDefaultWorldFOV/fDefault1stPersonFOV = " +
                       std::to_string(int(required + 0.5f)) +
                       " to [Display] in Fallout4Custom.ini — restart to take effect";
        } else {
            s_status = "FOV write failed: " + err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove FOV override")) {
        std::string err;
        if (config::WriteEngineFov(0.0f, 0.0f, err))
            s_status = "FOV override removed — the game returns to its own FOV next launch";
        else
            s_status = "FOV write failed: " + err;
    }
    ImGui::TextDisabled("Writes to [Display]; values under [Interface] are ignored by the engine.");
    ImGui::TextWrapped(
        "Note: the warp is applied to the finished frame, so the HUD warps with it. "
        "Until the FOV above is applied and the game restarted, the screen edges will smear.");

    if (dirty) {
        config::ApplyToPost();
        config::Save();
    }
}

void DrawPostFxTab(ID3D11Device* device)
{
    auto& cfg = config::Get();
    bool dirty = false;

    if (ImGui::CollapsingHeader("Color Grading", ImGuiTreeNodeFlags_DefaultOpen)) {
        dirty |= ImGui::Checkbox("Enable color grading", &cfg.gradeEnabled);
        ImGui::BeginDisabled(!cfg.gradeEnabled);
        dirty |= ImGui::SliderFloat("Exposure (stops)", &cfg.exposure, -2.0f, 2.0f, "%.2f");
        dirty |= ImGui::SliderFloat("Contrast", &cfg.contrast, 0.5f, 1.8f, "%.2f");
        dirty |= ImGui::SliderFloat("Saturation", &cfg.saturation, 0.0f, 2.0f, "%.2f");
        dirty |= ImGui::SliderFloat("Vibrance", &cfg.vibrance, 0.0f, 1.0f, "%.2f");
        dirty |= ImGui::SliderFloat("Temperature", &cfg.temperature, -1.0f, 1.0f, "%.2f");
        dirty |= ImGui::SliderFloat("Tint", &cfg.tint, -1.0f, 1.0f, "%.2f");
        dirty |= ImGui::Checkbox("Filmic tonemap (ACES approx.)", &cfg.filmic);
        if (ImGui::Button("Reset grading")) {
            cfg.exposure = 0.0f; cfg.contrast = 1.0f; cfg.saturation = 1.0f; cfg.vibrance = 0.0f;
            cfg.temperature = 0.0f; cfg.tint = 0.0f; cfg.filmic = false;
            dirty = true;
        }
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Sharpening", ImGuiTreeNodeFlags_DefaultOpen)) {
        dirty |= ImGui::Checkbox("Enable Contrast Adaptive Sharpening", &cfg.sharpenEnabled);
        ImGui::BeginDisabled(!cfg.sharpenEnabled);
        ImGui::SetNextItemWidth(240);
        dirty |= ImGui::SliderFloat("Sharpness", &cfg.sharpness, 0.0f, 1.0f, "%.2f");
        ImGui::EndDisabled();
        ImGui::TextDisabled("If used, consider lowering fTAAPostSharpen to avoid double sharpening.");
    }

    if (dirty) {
        config::ApplyToPost();
        config::Save();
    }

    DrawLutSection(device);
    DrawSsaoSection();
    DrawPaniniSection();

    ImGui::Spacing();
    ImGui::TextWrapped("Grading, LUT, then sharpening are applied to the final frame in one "
                       "pass, before the overlay. Changes apply instantly.");
}

void DrawSystemsTab()
{
    ImGui::SeparatorText("Scene depth capture (foundation for screen-space GI)");
    if (!depth::Installed()) {
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "Depth tracking: not installed");
    } else if (depth::HaveDepth()) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Depth tracking: CAPTURED");
        ImGui::SameLine();
        ImGui::TextDisabled("(format %d)", int(depth::CapturedFormat()));
    } else {
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
                           "Depth tracking: installed, waiting for a scene");
        ImGui::TextDisabled("Load a save or enter the world — the menu draws no scene depth.");
    }

    bool showDepth = post::g_debugView == 1;
    if (ImGui::Checkbox("Show depth buffer (debug view)", &showDepth))
        post::g_debugView = showDepth ? 1 : 0;
    ImGui::BeginDisabled(!showDepth);
    ImGui::SetNextItemWidth(240);
    ImGui::SliderFloat("Depth contrast", &post::g_debugGamma, 1.0f, 512.0f, "%.0f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Magenta screen = no depth captured yet. Debug view is session-only.");
}

void DrawInfoTab(ID3D11Device* device)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("%.1f fps  (%.2f ms)", io.Framerate, 1000.0f / (io.Framerate > 0 ? io.Framerate : 1));
    ImGui::Text("Display: %.0f x %.0f", io.DisplaySize.x, io.DisplaySize.y);

    IDXGIDevice* dxgiDevice = nullptr;
    if (device && SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice)))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(adapter->GetDesc(&desc)))
                ImGui::Text("GPU: %ls", desc.Description);
            adapter->Release();
        }
        dxgiDevice->Release();
    }

    ImGui::Text("Engine settings bound: %zu", settings::AllResolved().size());
    ImGui::Separator();

    auto changed = settings::ChangedEntries();
    ImGui::Text("Changed this session: %zu", changed.size());
    ImGui::BeginDisabled(changed.empty());
    if (ImGui::Button("Write changes to game INIs")) {
        std::string err;
        int n = ini::WriteChanged(changed, err);
        s_status = n >= 0 ? std::to_string(n) + " setting(s) written to Documents INIs"
                          : "Error: " + err;
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert all")) {
        settings::RevertAll();
        s_status = "All session changes reverted";
    }
    ImGui::EndDisabled();
    if (!s_status.empty())
        ImGui::TextWrapped("%s", s_status.c_str());

    ImGui::Separator();
    bool& skip = config::Get().skipIntroMovies;
    if (ImGui::Checkbox("Skip intro movies on next launch", &skip)) {
        config::Save();
        s_status = skip ? "Startup logo & new-game intro will be skipped next launch"
                        : "Intro movies restored for next launch";
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("Blanks sIntroMovie and sIntroSequence at startup.\n"
                          "The animated main-menu background is kept.");

    ImGui::Separator();
    ImGui::TextDisabled("VisualForge 1.0 — toggle overlay with F10");
    ImGui::TextDisabled("Log: Documents\\My Games\\Fallout4\\F4SE\\VisualForge.log");
    ImGui::TextWrapped(
        "Values marked 'Applied on area reload' need a save/load or interior/exterior "
        "transition to show. 'Write changes' persists edits to Fallout4Prefs.ini / "
        "Fallout4Custom.ini so they survive restarts.");
}

// ---------------------------------------------------------------------------------------
// Basic mode: a few high-level "wrap-up" controls that each drive many engine settings.
// ---------------------------------------------------------------------------------------

struct Knob {
    const char* name;
    float value;
};

// Applies a bundle of engine settings through the live binder, then persists the ones that
// resolved to the game INIs so the choice survives a restart. Unresolved names are skipped.
void ApplyKnobs(const Knob* knobs, int count)
{
    std::vector<settings::Entry*> changed;
    for (int i = 0; i < count; ++i) {
        Entry* e = settings::Find(knobs[i].name);
        if (!e)
            continue;
        switch (e->type) {
            case settings::Type::Bool: settings::SetBool(*e, knobs[i].value != 0.0f); break;
            case settings::Type::Int: settings::SetInt(*e, int(knobs[i].value)); break;
            case settings::Type::UInt: settings::SetUInt(*e, unsigned(knobs[i].value < 0 ? 0 : knobs[i].value)); break;
            case settings::Type::Float: settings::SetFloat(*e, knobs[i].value); break;
            default: continue;
        }
        changed.push_back(e);
    }
    std::string err;
    int n = ini::WriteChanged(changed, err);
    s_status = n >= 0
        ? std::to_string(n) + " settings applied and saved (some show fully after a save/load)"
        : "Applied live, but INI save failed: " + err;
}

// Quality tiers. Values mirror the launcher presets, extended at the top end.
const Knob kPerformance[] = {
    {"fShadowDistance:Display", 6000}, {"fDirShadowDistance:Display", 6000},
    {"iShadowMapResolution:Display", 2048}, {"iDirShadowSplits:Display", 1},
    {"bVolumetricLightingEnable:Display", 0}, {"iVolumetricLightingQuality:Display", 0},
    {"fGrassStartFadeDistance:Grass", 3000}, {"uMaxDecals:Decals", 100},
    {"fLODFadeOutMultObjects:LOD", 5}, {"fTreeLoadDistance:TerrainManager", 25000},
    {"fBlockMaximumDistance:TerrainManager", 100000}, {"iMaxFocusShadows:Display", 2},
};
const Knob kBalanced[] = {
    {"fShadowDistance:Display", 14000}, {"fDirShadowDistance:Display", 14000},
    {"iShadowMapResolution:Display", 4096}, {"iDirShadowSplits:Display", 2},
    {"bVolumetricLightingEnable:Display", 1}, {"iVolumetricLightingQuality:Display", 2},
    {"fGrassStartFadeDistance:Grass", 5000}, {"uMaxDecals:Decals", 250},
    {"fLODFadeOutMultObjects:LOD", 9}, {"fTreeLoadDistance:TerrainManager", 50000},
    {"fBlockMaximumDistance:TerrainManager", 180000}, {"iMaxFocusShadows:Display", 4},
};
const Knob kQuality[] = {
    {"fShadowDistance:Display", 20000}, {"fDirShadowDistance:Display", 20000},
    {"iShadowMapResolution:Display", 4096}, {"iDirShadowSplits:Display", 3},
    {"bVolumetricLightingEnable:Display", 1}, {"iVolumetricLightingQuality:Display", 3},
    {"fGrassStartFadeDistance:Grass", 7000}, {"uMaxDecals:Decals", 1000},
    {"fLODFadeOutMultObjects:LOD", 15}, {"fTreeLoadDistance:TerrainManager", 75000},
    {"fBlockMaximumDistance:TerrainManager", 250000}, {"iMaxFocusShadows:Display", 6},
};
// "Ultra+" adds effects the launcher never enables, but deliberately does NOT push the
// streaming and shadow-memory settings past what Bethesda ships. Raising terrain/tree draw
// distance, shadow-map resolution and grass density beyond Ultra destabilised cell streaming
// (the game exited when walking into new cells), so those stay at Ultra values.
const Knob kUltra[] = {
    {"fShadowDistance:Display", 20000}, {"fDirShadowDistance:Display", 20000},
    {"iShadowMapResolution:Display", 4096}, {"iDirShadowSplits:Display", 3},
    {"bVolumetricLightingEnable:Display", 1}, {"iVolumetricLightingQuality:Display", 3},
    {"bVolumetricLightingForceCasters:Display", 1}, {"bActorSelfShadowing:Display", 1},
    {"bAllowShadowcasterNPCLights:Display", 1},
    {"fGrassStartFadeDistance:Grass", 7000},
    {"uMaxDecals:Decals", 1000}, {"fLODFadeOutMultObjects:LOD", 30},
    {"fTreeLoadDistance:TerrainManager", 75000}, {"fBlockMaximumDistance:TerrainManager", 250000},
    {"iMaxFocusShadows:Display", 4}, {"fMaxFocusShadowMapDistance:Display", 900},
    {"fSunUpdateThreshold:Display", 0.05f}, {"fSunShadowUpdateTime:Display", 0.25f},
};

struct QualityTier {
    const char* label;
    const Knob* knobs;
    int count;
    const char* hint;
};
const QualityTier kTiers[] = {
    {"Performance", kPerformance, int(sizeof(kPerformance) / sizeof(Knob)), "Highest FPS"},
    {"Balanced", kBalanced, int(sizeof(kBalanced) / sizeof(Knob)), "Good FPS + looks"},
    {"Quality", kQuality, int(sizeof(kQuality) / sizeof(Knob)), "Ultra preset"},
    {"Ultra+", kUltra, int(sizeof(kUltra) / sizeof(Knob)),
     "Ultra plus extra lighting effects (streaming stays at Ultra for stability)"},
};

// Visual "looks" combine a color grade with one of the bundled LUTs.
void ApplyLook(ID3D11Device* device, int look)
{
    auto& cfg = config::Get();
    // reset grade to neutral first
    cfg.exposure = 0; cfg.contrast = 1; cfg.saturation = 1; cfg.vibrance = 0;
    cfg.temperature = 0; cfg.tint = 0; cfg.filmic = false;
    cfg.gradeEnabled = false;
    cfg.lutEnabled = false;
    cfg.lutFile[0] = '\0';
    post::ClearLut();

    const char* lutName = nullptr;
    switch (look) {
        case 1: lutName = "Warm_Cinematic.cube"; cfg.gradeEnabled = true; cfg.contrast = 1.05f; cfg.vibrance = 0.15f; break;
        case 2: lutName = "Cool_Wasteland.cube"; cfg.gradeEnabled = true; cfg.saturation = 0.9f; break;
        case 3: lutName = "Filmic_Contrast.cube"; cfg.gradeEnabled = true; cfg.filmic = true; break;
        default: break; // 0 = Neutral, no LUT
    }
    if (lutName) {
        wchar_t wname[128], full[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, lutName, -1, wname, 128);
        swprintf_s(full, L"%sLUTs\\%s", config::PluginDir(), wname);
        std::string err;
        if (lut::LoadInto(device, full, err)) {
            strncpy_s(cfg.lutFile, lutName, _TRUNCATE);
            cfg.lutEnabled = true;
            cfg.lutIntensity = 1.0f;
            s_status = std::string("Look applied: ") + lutName;
        } else {
            s_status = "Look grade applied (LUT missing: " + err + ")";
        }
    } else {
        s_status = "Look reset to neutral";
    }
    config::ApplyToPost();
    config::Save();
}

void DrawBasicView(ID3D11Device* device)
{
    auto& cfg = config::Get();

    ImGui::TextDisabled("One-click setups. Switch to Advanced for every individual knob.");
    ImGui::Spacing();

    // Quality preset.
    ImGui::SeparatorText("Visual quality");
    for (int i = 0; i < int(sizeof(kTiers) / sizeof(QualityTier)); ++i) {
        if (i)
            ImGui::SameLine();
        if (ImGui::Button(kTiers[i].label, ImVec2(112, 0)))
            ApplyKnobs(kTiers[i].knobs, kTiers[i].count);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s", kTiers[i].hint);
    }
    ImGui::TextDisabled("Sets shadows, godrays, grass, LOD, decals and draw distance together.");

    // Look / color grade.
    ImGui::SeparatorText("Look");
    const char* looks[] = {"Neutral", "Warm Cinematic", "Bleak Wasteland", "Filmic"};
    for (int i = 0; i < 4; ++i) {
        if (i)
            ImGui::SameLine();
        bool active = (i == 0 && !cfg.lutEnabled) ||
                      (i == 1 && strstr(cfg.lutFile, "Warm")) ||
                      (i == 2 && strstr(cfg.lutFile, "Cool")) ||
                      (i == 3 && strstr(cfg.lutFile, "Filmic"));
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.70f, 1.0f));
        if (ImGui::Button(looks[i], ImVec2(112, 0)))
            ApplyLook(device, i);
        if (active)
            ImGui::PopStyleColor();
    }

    // Simple image sliders that drive the grading pass.
    ImGui::SeparatorText("Image");
    bool gdirty = false;
    ImGui::SetNextItemWidth(240);
    gdirty |= ImGui::SliderFloat("Brightness", &cfg.exposure, -1.0f, 1.0f, "%.2f");
    ImGui::SetNextItemWidth(240);
    gdirty |= ImGui::SliderFloat("Vividness", &cfg.saturation, 0.5f, 1.6f, "%.2f");
    if (gdirty) {
        cfg.gradeEnabled = cfg.gradeEnabled || cfg.exposure != 0.0f || cfg.saturation != 1.0f;
        config::ApplyToPost();
        config::Save();
    }
    ImGui::SetNextItemWidth(240);
    if (ImGui::SliderFloat("Sharpness", &cfg.sharpness, 0.0f, 1.0f, "%.2f")) {
        cfg.sharpenEnabled = cfg.sharpness > 0.001f;
        config::ApplyToPost();
        config::Save();
    }

    // A couple of high-value one-click toggles.
    ImGui::SeparatorText("Extras");
    if (ImGui::Checkbox("Skip intro movies", &cfg.skipIntroMovies))
        config::Save();
    // Weapon debris is not offered here, and this plugin no longer touches NVIDIA Flex at
    // all: FlexRevive replaces the solver rather than guarding against its crash, and two
    // plugins redirecting the same Flex import tables would fight over them.
    if (Entry* g = settings::Find("bVolumetricLightingEnable:Display")) {
        bool on = settings::GetBool(*g);
        if (ImGui::Checkbox("Godrays (volumetric lighting)", &on)) {
            settings::SetBool(*g, on);
            std::vector<settings::Entry*> one{g};
            std::string err;
            ini::WriteChanged(one, err);
        }
    }

    if (!s_status.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", s_status.c_str());
    }
}

} // namespace

void Draw(ID3D11Device* device)
{
    if (!g_visible)
        return;

    ImGui::SetNextWindowSize(ImVec2(560, 640), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("VisualForge", &g_visible)) {
        ImGui::End();
        return;
    }

    // Basic / Advanced switch.
    bool& basic = config::Get().basicMode;
    if (basic)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.70f, 1.0f));
    if (ImGui::Button("Basic")) {
        if (!basic) { basic = true; config::Save(); }
    }
    if (basic)
        ImGui::PopStyleColor();
    ImGui::SameLine();
    if (!basic)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.70f, 1.0f));
    if (ImGui::Button("Advanced")) {
        if (basic) { basic = false; config::Save(); }
    }
    if (!basic)
        ImGui::PopStyleColor();
    ImGui::Separator();

    if (basic) {
        DrawBasicView(device);
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Quick Tweaks")) {
            DrawQuickTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Browser")) {
            DrawBrowserTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Color & Sharpen")) {
            DrawPostFxTab(device);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Systems")) {
            DrawSystemsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Info")) {
            DrawInfoTab(device);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

}
