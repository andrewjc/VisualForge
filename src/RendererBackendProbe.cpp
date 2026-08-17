#include "RendererBackendProbe.h"

#include "Config.h"
#include "EngineCameraCapture.h"
#include "EngineDrawCapture.h"
#include "renderer_core/EngineEnvironmentSource.h"
#include "EngineTextureResidency.h"
#include "EngineWorldSuppression.h"
#include "EngineMeshExtractor.h"
#include "Log.h"
#include "renderer_api/BackendAbi.h"
#include "renderer_api/RasterPacket.h"
#include "renderer_core/CameraStateScan.h"
#include "renderer_core/EngineDrawStream.h"
#include "renderer_core/EngineScene.h"
#include "renderer_core/EngineView.h"
#include "renderer_core/RasterGolden.h"
#include "renderer_host/BackendHost.h"
#include "renderer_host/D3D11InteropBridge.h"
#include "renderer_host/RendererHealth.h"
#include "renderer_host/WindowsBackendModule.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <cmath>
#include <limits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

namespace vf::renderer_backend_probe {

namespace {

using namespace renderer;

bool s_attempted{};
bool s_ready{};
std::unique_ptr<WindowsBackendModule> s_module;
BackendHost s_host;
std::unique_ptr<D3D11InteropBridge> s_bridge;
std::uint64_t s_bridgeEpoch{1};
std::uint64_t s_bridgeFrame{};
bool s_bridgeFaultLogged{};
bool s_bridgeFirstFrameLogged{};
bool s_resizePending{};
// Kept from the probe so the raster session selects the same adapter the
// game is already presenting on. Creating it without one fails.
abi::AdapterLuid s_adapterLuid{};

bool EnvironmentFlag(const wchar_t* name) noexcept
{
    wchar_t value[16]{};
    const auto length = GetEnvironmentVariableW(
        name, value, static_cast<DWORD>(std::size(value)));
    if (length == 0 || length >= std::size(value)) {
        return false;
    }
    return _wcsicmp(value, L"1") == 0 ||
        _wcsicmp(value, L"true") == 0 ||
        _wcsicmp(value, L"on") == 0;
}

void BackendLog(
    void*,
    const std::uint32_t level,
    const char* message)
{
    log::Write(
        "renderer-backend[%u]: %s",
        level,
        message == nullptr ? "" : message);
}

bool QueryAdapterLuid(
    ID3D11Device* device,
    abi::AdapterLuid& luid) noexcept
{
    if (device == nullptr) {
        return false;
    }

    IDXGIDevice* dxgiDevice{};
    IDXGIAdapter* adapter{};
    DXGI_ADAPTER_DESC description{};
    const auto queried = SUCCEEDED(device->QueryInterface(
        __uuidof(IDXGIDevice),
        reinterpret_cast<void**>(&dxgiDevice)));
    const auto adapted = queried &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter));
    const auto described = adapted &&
        SUCCEEDED(adapter->GetDesc(&description));
    if (described) {
        luid.lowPart = description.AdapterLuid.LowPart;
        luid.highPart = description.AdapterLuid.HighPart;
    }
    if (adapter != nullptr) {
        adapter->Release();
    }
    if (dxgiDevice != nullptr) {
        dxgiDevice->Release();
    }
    return described;
}

bool HasCapability(
    const abi::CapabilityReportV1& report,
    const abi::Capability capability) noexcept
{
    return (report.supportedCapabilities & capability) != 0;
}

}

bool ProbeOnce(ID3D11Device* device) noexcept
{
    if (!EnvironmentFlag(L"VISUALFORGE_BACKEND_PROBE") &&
        !PatternRequested() && !MirrorRequested()) {
        return false;
    }
    if (s_attempted) {
        return s_ready;
    }
    s_attempted = true;

    abi::AdapterLuid adapterLuid{};
    if (!QueryAdapterLuid(device, adapterLuid)) {
        log::Write("renderer-backend: D3D11 adapter LUID query failed; staying on vanilla");
        return false;
    }

    s_adapterLuid = adapterLuid;
    auto backendPath = std::filesystem::path{config::PluginDir()};
    backendPath /= L"VisualForgeRenderer.dll";
    s_module = std::make_unique<WindowsBackendModule>(backendPath);

    abi::HostCallbacksV1 callbacks{};
    callbacks.structSize = sizeof(callbacks);
    callbacks.log = BackendLog;
    const auto loaded = s_host.Load(*s_module, callbacks);
    if (!loaded) {
        log::Write(
            "renderer-backend: load failed host=%u backend=%u contract=%u win32=%lu; staying on vanilla",
            static_cast<unsigned>(loaded.error),
            static_cast<unsigned>(loaded.backendResult),
            static_cast<unsigned>(loaded.contractError),
            s_module->LastErrorCode());
        return false;
    }

    abi::ProbeRequestV1 request{};
    request.structSize = sizeof(request);
    request.enableValidation =
        EnvironmentFlag(L"VISUALFORGE_VULKAN_VALIDATION") ? 1u : 0u;
    request.adapterLuid = adapterLuid;
    request.requiredCapabilities = abi::kRequiredCapabilities;
    abi::CapabilityReportV1 report{};
    report.structSize = sizeof(report);
    const auto probed = s_host.Probe(request, report);
    if (!probed) {
        log::Write(
            "renderer-backend: probe failed host=%u backend=%u missing=0x%llX validation-errors=%u; staying on vanilla",
            static_cast<unsigned>(probed.error),
            static_cast<unsigned>(probed.backendResult),
            static_cast<unsigned long long>(report.missingRequiredCapabilities),
            report.validationErrorCount);
        // Name the adapter when the backend could not find it, because that
        // failure is almost never about the renderer. A virtual display driver
        // -- a headset compositor, a remote-desktop monitor -- enumerates an
        // adapter that mirrors the real card, reporting the same name and the
        // same memory but carrying a LUID no Vulkan device has. When the
        // engine's swap chain lands on that one there is no Vulkan device to
        // share images with, and the only honest thing to do is stay on
        // vanilla. Measured on this machine: two entries for one RTX 4090,
        // and the mirrored one enumerating first.
        if (probed.backendResult == abi::Result::AdapterLuidNotFound) {
            log::Write(
                "renderer-backend: no Vulkan device carries the engine's "
                "adapter luid=%d:%u. A virtual display adapter is usually "
                "the cause; the renderer cannot share images with a device "
                "Vulkan does not have.",
                adapterLuid.highPart, adapterLuid.lowPart);
        }
        return false;
    }

    s_ready = report.validationErrorCount == 0;
    log::Write(
        "renderer-backend: ready device=\"%s\" driver=\"%s\" vendor=0x%04X device-id=0x%04X api=0x%08X queue=%u required=%s missing=0x%llX bc=%s d3d11-import=%s d3d12-fence=%s per-stage-sampled=%u set-sampled=%u push-constants=%u ray-recursion=%u shader-group=%u as-scratch-align=%u validation-errors=%u unload-policy=process-lifetime",
        report.deviceName,
        report.driverName,
        report.vendorId,
        report.deviceId,
        report.apiVersion,
        report.queueFamilyIndex,
        report.missingRequiredCapabilities == 0 ? "pass" : "fail",
        static_cast<unsigned long long>(report.missingRequiredCapabilities),
        HasCapability(report, abi::Capability::BcTextureFormats) ? "on" : "off",
        HasCapability(report, abi::Capability::D3d11TextureInterop) ? "on" : "off",
        HasCapability(report, abi::Capability::D3d12FenceInterop) ? "on" : "off",
        report.maxPerStageSampledImages,
        report.maxDescriptorSetSampledImages,
        report.maxPushConstantsSize,
        report.maxRayRecursionDepth,
        report.shaderGroupHandleSize,
        report.accelerationStructureScratchAlignment,
        report.validationErrorCount);
    return s_ready;
}

bool PatternRequested() noexcept
{
    return EnvironmentFlag(L"VISUALFORGE_BRIDGE_PATTERN");
}

bool InitializePatternBridge(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const unsigned width,
    const unsigned height,
    const unsigned format) noexcept
{
    // The mirror needs the same shared images and fence timeline the
    // pattern uses, so either request brings the bridge up.
    if (!PatternRequested() && !MirrorRequested()) {
        return false;
    }
    if (!ProbeOnce(device) || context == nullptr || width == 0 ||
        height == 0) {
        return false;
    }
    constexpr unsigned kR8G8B8A8Unorm = 28;
    if (format != kR8G8B8A8Unorm) {
        log::Write(
            "renderer-bridge: unsupported swapchain format=%u; staying on vanilla",
            format);
        return false;
    }
    if (s_bridge != nullptr && s_bridge->Ready()) {
        return true;
    }

    s_bridge = std::make_unique<D3D11InteropBridge>();
    const auto created = s_bridge->Create(
        device,
        context,
        s_host,
        width,
        height,
        EnvironmentFlag(L"VISUALFORGE_VULKAN_VALIDATION"),
        s_bridgeEpoch);
    if (!created) {
        log::Write(
            "renderer-bridge: create failed error=%u backend=%u hr=0x%08lX; staying on vanilla",
            static_cast<unsigned>(created.error),
            static_cast<unsigned>(created.backend.backendResult),
            static_cast<unsigned long>(created.hresult));
        s_bridge.reset();
        return false;
    }

    renderer::StartupHealth health{
        1,
        renderer::RendererMode::Mirror,
        true,
        false,
    };
    log::Write("%s", renderer::FormatStartupHealth(health).c_str());
    log::Write(
        "renderer-bridge: ready extent=%ux%u format=R8G8B8A8_UNORM ring=3 epoch=%llu sync=d3d11-fence-timeline validation-errors=%u",
        width,
        height,
        static_cast<unsigned long long>(s_bridgeEpoch),
        s_bridge->ValidationErrorCount());
    s_bridgeFaultLogged = false;
    s_bridgeFirstFrameLogged = false;
    s_resizePending = false;
    return true;
}

bool CompositePattern(IDXGISwapChain* swapchain) noexcept
{
    if (s_bridge == nullptr || !s_bridge->Ready() ||
        swapchain == nullptr) {
        return false;
    }
    ID3D11Texture2D* backbuffer{};
    if (FAILED(swapchain->GetBuffer(
            0,
            __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&backbuffer)))) {
        return false;
    }
    const auto submitted = s_bridge->SubmitPattern(
        s_host, backbuffer, s_bridgeFrame++);
    backbuffer->Release();
    if (!submitted) {
        if (submitted.error != D3D11BridgeError::SlotBusy &&
            !s_bridgeFaultLogged) {
            log::Write(
                "renderer-bridge: submit failed error=%u backend=%u hr=0x%08lX; frame remains vanilla",
                static_cast<unsigned>(submitted.error),
                static_cast<unsigned>(submitted.backend.backendResult),
                static_cast<unsigned long>(submitted.hresult));
            s_bridgeFaultLogged = true;
        }
        return false;
    }
    if (!s_bridgeFirstFrameLogged) {
        log::Write(
            "renderer-bridge: first-frame displayed release=%llu ready=%llu image=%u validation-errors=%u suppression=off",
            static_cast<unsigned long long>(submitted.ticket.releaseValue),
            static_cast<unsigned long long>(submitted.ticket.readyValue),
            submitted.ticket.imageIndex,
            s_bridge->ValidationErrorCount());
        s_bridgeFirstFrameLogged = true;
    }
    return true;
}

bool MirrorRequested() noexcept
{
    return EnvironmentFlag(L"VISUALFORGE_MIRROR");
}

namespace {

// Geometry the mirror draws, in camera-relative world units. The engine's
// view matrices are camera-relative and carry a zero translation, so these
// positions rotate with the player's view and never slide with position.
// That is the observable proof that the Vulkan frame is driven by the live
// engine camera rather than replayed from a file.
// Below eye level, so the tiles sit on the ground the player is standing on.

std::vector<std::byte> s_mirrorPacket;
std::vector<std::byte> s_mirrorFrame;
std::vector<raster::Rgba8> s_mirrorPixels;
std::vector<std::byte> s_mirrorScene;
// The block the volumetric lighting shaders declare the sun in. Named here
// rather than at the use site so a caller cannot invent its own spelling.
constexpr const char* kMirrorLightBufferName = "cbVolume";
// The sun's identity in the mirror's light packet. Any non-zero value would
// do -- the packet refuses zero and rejects duplicates -- but it is fixed so
// the same light keeps the same identity from frame to frame.
constexpr std::uint64_t kMirrorSunLightId = 0x53554E0000000001ull;
// The frame's light packet, empty whenever the sun could not be resolved.
// Empty is a real answer: the backend then has no environment and leaves the
// albedo alone, which is correct behaviour for a session whose volumetric
// pass is switched off. Lighting the scene from a stale sample would be worse
// than not lighting it.
std::vector<std::byte> s_mirrorLight;
// The frame's texture library and the map from a scene object to the engine
// texture its draws sampled. Objects outlive a single draw, so the identity is
// keyed by the mesh identity the object was built from.
std::vector<std::byte> s_mirrorTextureLibrary;
// The library entries in index order, and the reverse mapping. Persistent so
// an index handed to a material stays valid across frames and the bytes only
// have to be rebuilt when the set actually gains a texture.
// Bumped whenever the encoded library changes, so the backend can tell one
// frame.s library from the next without decoding or hashing it.
std::uint64_t s_mirrorLibraryGeneration = 0;
// Times a re-encode was abandoned because an entry evicted mid-frame. Non-zero
// is expected and harmless; a number that climbs every frame would mean the
// prune is never catching up.
std::uint64_t s_mirrorLibraryDeferredPrunes = 0;
std::vector<std::uint64_t> s_mirrorLibraryIds;
// Whether each entry.s texture was resident last frame. A change either way
// rebuilds the library bytes, while the index the entry occupies never moves.
std::vector<bool> s_mirrorLibraryResident;
std::unordered_map<std::uint64_t, std::uint32_t> s_mirrorLibraryIndexOf;
// A once-per-session measurement of what the texture library is worth: the
// same frame rendered with it withheld, so the difference between the two
// images is attributable to per-material textures and nothing else. Held
// rather than discarded so the comparison runs on identical geometry, lights
// and camera -- a later frame would differ for reasons of its own.
// Where a mirrored frame's time actually goes. Split by stage because the
// total alone cannot distinguish a slow renderer from a slow assembler, and
// the mirror does a great deal of CPU work before the backend sees anything.
//
// Plain integers rather than atomics: every one of these is written only from
// the mirror path, which runs on the presenting thread.
std::uint64_t s_stageGeometryUs = 0;
// Inside the geometry stage. Every rebuild the goal named is now cached, and
// what is left has to be attributed rather than guessed at.
// Whether the packet cache is actually holding. The steady state drifts
// upward within a run while the mesh set reports unchanged, and a miss count
// is the difference between "the machine slowed down" and "the key stopped
// matching".
std::uint64_t s_encodeHits = 0;
std::uint64_t s_encodeMisses = 0;
std::uint64_t s_stageCollectUs = 0;
std::uint64_t s_stageTranslateUs = 0;
std::uint64_t s_stageExtractUs = 0;
std::uint64_t s_reportedExtractUs = 0;
std::uint64_t s_stageMeshBuildUs = 0;
std::uint64_t s_stageAssembleUs = 0;
std::uint64_t s_reportedCollectUs = 0;
std::uint64_t s_reportedTranslateUs = 0;
std::uint64_t s_reportedMeshBuildUs = 0;
std::uint64_t s_reportedAssembleUs = 0;
std::uint64_t s_stageLibraryUs = 0;
std::uint64_t s_stageEncodeUs = 0;
std::uint64_t s_stageLightingUs = 0;
std::uint64_t s_stageRenderUs = 0;
std::uint64_t s_stagePresentUs = 0;
// What the last report already accounted for, so each line covers only the
// frames since the previous one.
// Frames per stage report. Thirty rather than a hundred and twenty because a
// window that spans a cell streaming in reports the streaming, not the steady
// state: the mirror settles for only a few seconds inside a capture, and a
// long window never lands wholly inside it.
constexpr std::uint64_t kStageReportInterval = 30;
std::uint64_t s_reportedGeometryUs = 0;
std::uint64_t s_reportedLibraryUs = 0;
std::uint64_t s_reportedEncodeUs = 0;
std::uint64_t s_reportedLightingUs = 0;
std::uint64_t s_reportedRenderUs = 0;
std::uint64_t s_reportedPresentUs = 0;

class StageTimer
{
public:
    explicit StageTimer(std::uint64_t& sink) noexcept
        : sink_(sink), started_(std::chrono::steady_clock::now())
    {
    }
    StageTimer(const StageTimer&) = delete;
    StageTimer& operator=(const StageTimer&) = delete;
    ~StageTimer()
    {
        sink_ += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started_).count());
    }

private:
    std::uint64_t& sink_;
    std::chrono::steady_clock::time_point started_;
};

std::uint64_t s_mirrorMicrosecondsTotal = 0;
std::uint64_t s_mirrorMicrosecondsWorst = 0;
std::uint64_t s_mirrorTimedFrames = 0;

std::vector<raster::Rgba8> s_mirrorLibraryProbePixels;
bool s_mirrorLibraryProbeDone{};
std::uint32_t s_mirrorLibraryProbeAttempts{};
// The first frame holding *a* library is not a frame worth measuring: the
// load screen carries four materials and two textures, and the world that
// follows carries hundreds. Measuring the first one answers a question nobody
// asked. The probe waits for a library with real coverage and then keeps
// measuring until it observes an actual difference, so the run cannot report
// "no effect" from a frame where the effect had nothing to act on.
constexpr std::uint32_t kMirrorLibraryProbeMinimumTextures = 8;
constexpr std::uint32_t kMirrorLibraryProbeMaximumAttempts = 8;
std::unordered_map<std::uint64_t, std::uint64_t> s_mirrorObjectTextures;
std::uint32_t s_mirrorLibraryTextures = 0;
std::uint32_t s_mirrorTexturedMaterials = 0;
std::uint32_t s_mirrorProbeMaterialCount = 0;
environment::EnvironmentSourceError s_mirrorLightError =
    environment::EnvironmentSourceError::NoCandidate;
// Logged on change rather than once. The first attempt happens during the
// loading screen, before the volumetric pass has run, so a one-shot log
// reports "no-candidate" forever and hides the frame where the sun arrives.
auto s_mirrorLightLogged = environment::EnvironmentSourceError::Ambiguous;
bool s_mirrorLightEverLogged = false;
std::uint32_t s_mirrorLightLayouts = 0;
std::uint32_t s_mirrorLightSamples = 0;
std::uint32_t s_mirrorLightCandidates = 0;
std::uint32_t s_mirrorLightWidth = 0;
std::uint32_t s_mirrorLightCandidatesLogged = 0xFFFFFFFFu;
lighting::LightPacketError s_mirrorLightPacketError =
    lighting::LightPacketError::None;
bool s_mirrorLiveLogged = false;
bool s_mirrorRasterCreated{};
// The last decline reason written to the log, so a reason that starts later
// still gets named. One flag shared by every fault site meant whichever fired
// first -- always a startup gate -- silenced the rest of the run.
char s_mirrorFaultReason[64]{};
bool s_mirrorCreateFaultLogged{};
bool s_mirrorRenderFaultLogged{};
bool s_mirrorSubmitFaultLogged{};
bool s_mirrorFirstFrameLogged{};
std::uint64_t s_mirrorFrameIndex{};

// One reason's count, by enumerator rather than by a literal index, so
// inserting an error in the middle of the enumeration cannot silently
// re-label every column of the log line that reads them.
[[nodiscard]] std::uint32_t RejectionCount(
    const drawstream::TranslationResult& result,
    const drawstream::DrawStreamError reason) noexcept
{
    const auto index = static_cast<std::size_t>(reason);
    if (index >= result.rejectedByReason.size()) return 0;
    return result.rejectedByReason[index];
}

// The live scene, assembled from what the draw hooks recorded and the
// extractor has read back so far. Returns false when the frame cannot be
// built yet -- an empty cache early in a load, or a frame with no usable
// draws -- and the caller declines the frame rather than presenting
// something that did not come from the world.
// Joins the two halves of the lighting measurement into a packet the backend
// can consume: the layouts the engine's pixel shaders declare, and the bytes
// the engine wrote into a buffer matching one of them.
//
// Neither half means anything alone. The declaration says `g_vLightDir` sits
// at byte 608 of a 736-byte block but not what is there; the sample says what
// is at 608 but not what it means. The candidates are every sampled buffer of
// the declared width, because the engine binds thousands and width is the only
// thing that connects a sample to a layout -- the arithmetic checks in
// SelectDirectionalLight are what pick the real one out of them.
void BuildMirrorLighting()
{
    s_mirrorLight.clear();

    static std::array<engine_draw_capture::ShaderBufferLayout,
        engine_draw_capture::kShaderBufferLayoutCapacity> layouts{};
    const auto layoutCount = engine_draw_capture::CopyShaderBufferLayouts(
        layouts.data(), layouts.size());

    shader::ReflectedShader reflection{};
    for (std::size_t index = 0; index < layoutCount; ++index) {
        const auto& layout = layouts[index];
        shader::ReflectedBuffer buffer{};
        buffer.name = layout.name;
        buffer.size = layout.byteWidth;
        for (std::uint32_t field = 0; field < layout.fieldCount; ++field) {
            const auto& member = layout.fields[field];
            buffer.variables.push_back({member.name, member.offset, member.size});
        }
        reflection.buffers.push_back(std::move(buffer));
    }

    const auto* const declared = [&]() -> const shader::ReflectedBuffer* {
        for (const auto& buffer : reflection.buffers) {
            if (buffer.name == kMirrorLightBufferName) return &buffer;
        }
        return nullptr;
    }();
    if (declared == nullptr) {
        s_mirrorLightError = environment::EnvironmentSourceError::BufferNotFound;
        return;
    }

    static std::array<engine_draw_capture::LightingSample,
        engine_draw_capture::kLightingSampleSlots> samples{};
    const auto sampleCount = engine_draw_capture::CopyLightingSamples(
        samples.data(), samples.size());

    std::vector<std::span<const std::byte>> candidates;
    for (std::size_t index = 0; index < sampleCount; ++index) {
        const auto& sample = samples[index];
        if (sample.byteWidth != declared->size) continue;
        candidates.push_back(std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(sample.values),
            sample.byteWidth});
    }

    s_mirrorLightLayouts = static_cast<std::uint32_t>(layoutCount);
    s_mirrorLightSamples = static_cast<std::uint32_t>(sampleCount);
    s_mirrorLightCandidates = static_cast<std::uint32_t>(candidates.size());
    s_mirrorLightWidth = declared->size;

    lighting::EnvironmentRecordV1 record{};
    std::size_t chosen = 0;
    s_mirrorLightError = environment::SelectDirectionalLight(
        reflection, kMirrorLightBufferName, candidates, record, chosen);
    if (s_mirrorLightError != environment::EnvironmentSourceError::None) {
        return;
    }

    lighting::LightPacket packet{};
    packet.environment = record;
    // As a light, not only as environment metadata. The shading reads the
    // ambient term and the light list; the environment's own sunDirection and
    // sunColor are carried for provenance but nothing evaluates them, so a
    // sun delivered only there shades nothing and looks exactly like no sun.
    lighting::LightRecordV1 sun{};
    const auto sunBuilt = environment::MakeDirectionalLight(
        record, kMirrorSunLightId, sun);
    if (sunBuilt != environment::EnvironmentSourceError::None) {
        s_mirrorLightError = sunBuilt;
        return;
    }
    packet.lights.push_back(sun);
    // Reported as itself. An earlier version folded this into the read's own
    // "no candidate", which said the sun was never found when in fact it was
    // found and then failed to encode -- a diagnosis that sent the search back
    // to the capture layer where there was nothing wrong.
    s_mirrorLightPacketError = lighting::EncodeLightPacket(packet, s_mirrorLight);
    if (s_mirrorLightPacketError != lighting::LightPacketError::None) {
        s_mirrorLight.clear();
    }
}

bool BuildLiveSceneGeometry(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint64_t frameId,
    const std::uint64_t viewId,
    // The view the backend will be handed, so the diagnostics below can
    // project the scene with exactly the matrix the device will use rather
    // than with a reconstruction of it that could differ silently.
    const view::ViewRecordV1& record,
    // The position triples the camera record holds. Which one is the camera
    // cannot be told from the bytes, only from the geometry, which is here.
    const std::vector<std::array<float, 3>>& originCandidates,
    std::vector<std::byte>& packetBytes,
    std::vector<std::byte>& sceneBytes,
    drawstream::AssemblyResult& assembly,
    drawstream::TranslationResult& translated)
{
    translated = drawstream::TranslationResult{};

    static std::vector<drawstream::DrawRecordV1> recorded(
        engine_draw_capture::kDrawArenaCapacity);
    std::uint64_t dropped = 0;
    const auto collectStarted = std::chrono::steady_clock::now();
    const auto count = engine_draw_capture::CollectDraws(
        recorded.data(), recorded.size(), dropped);
    s_stageCollectUs += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - collectStarted).count());
    if (count == 0) return false;

    drawstream::DrawStreamFrame frame{};
    frame.frameIndex = frameId;
    frame.droppedDraws = dropped;
    frame.draws.assign(recorded.begin(), recorded.begin() + count);

    // How much of the *mirrored* frame carries a base-colour texture, as
    // opposed to how much of the engine's whole draw stream does. The two are
    // very different populations: the mirror keeps only world geometry, and a
    // resolution rate measured over every draw the engine makes says nothing
    // about whether the objects actually drawn here have one.
    {
        std::uint64_t withTexture = 0;
        // Keyed by mesh identity, which is what an object is built from: many
        // draws collapse into one object, and they agree on the texture
        // whenever they are the same mesh drawn more than once. The first
        // draw to name a texture wins, so a later instance that happened to
        // be recorded without one cannot erase it.
        // Not cleared. A mesh's base-colour texture is a property of the mesh,
        // not of the frame it was seen in, and rebuilding the map every frame
        // made an index appear and disappear as individual draws happened to
        // resolve or not. That flicker changed the encoded packet's contents
        // every frame and defeated the encode cache -- 66 ms of re-encoding a
        // packet whose geometry had not moved -- as well as making a surface
        // lose its texture for a frame at a time.
        //
        // Bounded by the number of distinct mesh identities the session sees,
        // and each entry is two integers.
        // How many recorded draws ran with the main scene depth bound. The
        // water reflection pass draws the whole world again through a
        // mirrored camera into its own target, so a large off-screen share
        // is the measurement behind the scene appearing twice.
        std::uint64_t onSceneDepth = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const auto& draw = recorded[index];
            if (draw.sceneDepthBound) ++onSceneDepth;
            // Two thirds of recorded draws are depth-only or off-screen and
            // are rejected before they can become an object. Hashing their
            // identity and probing two persistent maps for each of them was
            // most of the remaining frame: four thousand of six thousand
            // draws paying for bookkeeping that is then discarded.
            if (!draw.hasPixelShader || !draw.sceneDepthBound) continue;
            if (draw.baseColorTexture == 0) continue;
            ++withTexture;
            s_mirrorObjectTextures.emplace(
                drawstream::MeshIdentity(draw), draw.baseColorTexture);
        }
        static std::uint64_t s_lastWithTexture = 0xFFFF'FFFF'FFFF'FFFFull;
        if (withTexture != s_lastWithTexture) {
            log::Write("renderer-drawtex: draws=%llu with-texture=%llu "
                "scene-depth=%llu offscreen=%llu",
                static_cast<unsigned long long>(count),
                static_cast<unsigned long long>(withTexture),
                static_cast<unsigned long long>(onSceneDepth),
                static_cast<unsigned long long>(count - onSceneDepth));
            s_lastWithTexture = withTexture;
        }
    }

    scene::ScenePacket scenePacket{};
    const auto translateStarted = std::chrono::steady_clock::now();
    const auto translateResult =
        drawstream::TranslateDrawStream(frame, {}, scenePacket, translated);
    s_stageTranslateUs += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - translateStarted).count());
    if (translateResult != drawstream::DrawStreamError::None) {
        return false;
    }

    // Fallout 4 places objects in absolute world coordinates and publishes a
    // view matrix that carries no translation. The scene packet's contract is
    // the other one: instance transforms are already relative to the camera,
    // which is why every consumer -- the device, the reference rasteriser, the
    // acceleration structures -- reads them without asking. Reconciling the
    // two here, once, is what keeps that contract true; narrowing further
    // downstream would mean every consumer had to know which of the two kinds
    // of capture it had been handed.
    //
    // Measured before this existed: every instance sat about a hundred and
    // twenty thousand units from the rendered origin with nothing below eye
    // level, and the cell drew as a band on the horizon.
    if ((record.flags & view::ViewCameraRelative) != 0 &&
        !scenePacket.instances.empty()) {
        std::array<double, 3> origin{
            record.cameraRelativeOrigin[0],
            record.cameraRelativeOrigin[1],
            record.cameraRelativeOrigin[2]};
        // When the scan could not name the position itself, the frame's own
        // geometry names it. The engine keeps the camera position in the same
        // record as the matrices, as three floats among many, and the one that
        // is the position is the one the cell is built around. One cell of
        // radius, and at least sixty-four objects inside it: a populated
        // exterior cell carries hundreds, while the few identity-placed quads
        // that sit at the world origin cannot reach that and so cannot win.
        if (!originCandidates.empty()) {
            std::vector<std::array<float, 3>> translations;
            translations.reserve(scenePacket.instances.size());
            for (const auto& instance : scenePacket.instances) {
                translations.push_back({instance.model[3], instance.model[7],
                    instance.model[11]});
            }
            renderer::camera::OriginSelection selection{};
            const auto selected = renderer::camera::SelectCameraOrigin(
                originCandidates, translations, 4096.0f, 64, selection);
            if (selected == renderer::camera::OriginSelectionError::None) {
                origin = selection.origin;
            }
            if (frameId % 120 == 0) {
                log::Write("renderer-origin-select: status=%u at=["
                    "%.1f,%.1f,%.1f] nearest=%.2f neighbours=%u index=%u candidates=%zu",
                    static_cast<unsigned>(selected), selection.origin[0],
                    selection.origin[1], selection.origin[2],
                    static_cast<double>(selection.nearestDistance),
                    selection.neighbours, selection.candidateIndex,
                    originCandidates.size());
            }
        }
        const std::span<const double, 3> narrowBy{origin};
        for (auto& instance : scenePacket.instances) {
            instance = scene::NarrowInstance(instance, narrowBy);
        }
    }

    // Read back a budgeted slice of whatever is still missing. The cell
    // fills in over frames rather than stalling one frame to read it all.
    const auto cached = engine_mesh_extractor::CachedIdentities();
    // The contract's default budget is deliberately small, because a readback
    // synchronises the pipeline and an unbounded one turns a renderer into a
    // stutter. The mirror is not the shipping path and it needs a cell to be
    // whole before it means anything: measured live, the default filled 576 of
    // roughly 1300 objects in twelve seconds, so a screenshot taken at any
    // normal moment shows a half-empty world and looks like missing geometry
    // rather than like a budget. Chosen against the index-byte limit, which is
    // what actually bound the rate -- the mesh count never reached its own cap.
    drawstream::ExtractionBudget budget{};
    budget.maximumMeshesPerFrame = 64;
    budget.maximumIndexBytesPerFrame = 8ull << 20;
    const auto extractStarted = std::chrono::steady_clock::now();
    const auto plan =
        drawstream::PlanMeshExtraction(frame, {}, cached, budget);
    static_cast<void>(engine_mesh_extractor::Extract(plan.requests));
    s_stageExtractUs += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - extractStarted).count());

    const auto meshBuildStarted = std::chrono::steady_clock::now();
    std::vector<drawstream::AssembledMesh> meshes;
    meshes.reserve(scenePacket.objects.size());
    // Counted separately because the two have different causes and different
    // fixes: geometry not read back yet is a budget filling in over frames,
    // and a mesh with no recorded layout is a hook that was installed too late
    // to see the engine declare its vertex formats. A single "unavailable"
    // could not tell them apart, and the whole cell was declined for a whole
    // run while both looked identical from outside.
    std::uint32_t notExtracted = 0;
    std::uint32_t noLayout = 0;
    // The rasterizer state each mesh was drawn under, keyed by the same
    // identity the scene objects carry. Built from the same records the scene
    // packet was translated from, so a mesh and its winding cannot disagree.
    // A sorted vector, not a hash map. There are about six thousand recorded
    // draws a frame and a map allocates a node for each one, every frame, for
    // a table that is thrown away at the end of the call.
    // Persistent, and only ever gains entries. A mesh's winding is a property
    // of the mesh, so rebuilding this from six thousand draws every frame --
    // and sorting it -- recomputed an answer that had not changed since the
    // first time the mesh was drawn.
    //
    // First writer wins, matching the previous behaviour: the earlier build
    // refused to overwrite an identity it already held.
    static std::unordered_map<std::uint64_t, std::pair<bool, std::uint32_t>>
        winding;
    for (const auto& record : frame.draws) {
        // Same reason as the texture map above: a draw that cannot become an
        // object has no winding worth recording, and probing the map for it
        // costs a hash and a miss on a table that grows all session.
        if (!record.hasPixelShader || !record.sceneDepthBound) continue;
        winding.try_emplace(drawstream::MeshIdentity(record),
            record.frontCounterClockwise, record.cullMode);
    }
    for (const auto& object : scenePacket.objects) {
        const auto* const held =
            engine_mesh_extractor::Find(object.objectId);
        if (held == nullptr) {
            ++notExtracted;
            continue;
        }
        drawstream::AssembledMesh mesh{};
        mesh.identity = held->identity;
        mesh.vertexStride = held->vertexStride;
        // Declined rather than guessed at. A mesh whose layout the capture
        // never saw cannot be decoded, and decoding it as three floats is
        // what turned a cell into a fan of spikes.
        // Memoised per layout and stride. `FindInputLayout` scans a fixed
        // table of five hundred atomic slots and then *rebuilds* the vertex
        // layout from its element descriptors, and doing that for all nine
        // hundred meshes every frame re-derived a few dozen distinct layouts
        // that never change -- the largest single item left in the frame.
        static std::map<std::pair<std::uint64_t, std::uint32_t>,
            std::optional<renderer::mesh::EngineVertexLayout>> layoutCache;
        const auto key = std::pair{held->inputLayout, held->vertexStride};
        auto cached = layoutCache.find(key);
        if (cached == layoutCache.end()) {
            renderer::mesh::EngineVertexLayout built{};
            const auto ok = engine_draw_capture::FindInputLayout(
                held->inputLayout, held->vertexStride, built);
            // The negative is cached too. A layout the capture never saw is
            // not going to appear because it was asked for again, and the
            // scan for it is the expensive case -- it walks every slot.
            cached = layoutCache.emplace(key,
                ok ? std::optional{built} : std::nullopt).first;
        }
        if (!cached->second.has_value()) {
            ++noLayout;
            continue;
        }
        mesh.layout = *cached->second;
        mesh.vertices = held->vertices;
        mesh.indices = held->indices;
        // Absent only if the mesh outlived the record that drew it, in which
        // case the defaults above stand and the runtime.s own defaults apply.
        if (const auto found = winding.find(object.objectId);
            found != winding.end()) {
            mesh.frontCounterClockwise = found->second.first;
            mesh.cullMode = found->second.second;
        }
        meshes.push_back(mesh);
    }

    // How much of the scene actually differs from the previous frame.
    //
    // The assembly rebuilds every vertex and index each frame at a cost of
    // 454 ms, on the assumption that it has to. This measures whether that is
    // true: a mesh is unchanged when it keeps its identity *and* still points
    // at the same bytes, because an identity alone would call a re-extracted
    // mesh unchanged and a cache keyed on it would render stale geometry.
    // Sampled on reporting frames only. Building the comparison map costs a
    // hash node per mesh and frees the previous one, and doing that on every
    // frame put the monitor itself into the steady-state cost it exists to
    // watch. Comparing against the previous sample rather than the previous
    // frame answers the same question: whether the set is holding still.
    if (frameId % 120 == 0) {
        struct Fingerprint
        {
            const void* vertices{};
            std::size_t vertexBytes{};
            std::size_t indexCount{};
        };
        static std::unordered_map<std::uint64_t, Fingerprint> previous;
        std::unordered_map<std::uint64_t, Fingerprint> current;
        current.reserve(meshes.size());
        std::uint32_t unchanged = 0;
        std::uint32_t changed = 0;
        std::uint32_t added = 0;
        for (const auto& mesh : meshes) {
            const Fingerprint print{mesh.vertices.data(),
                mesh.vertices.size_bytes(), mesh.indices.size()};
            current.emplace(mesh.identity, print);
            const auto found = previous.find(mesh.identity);
            if (found == previous.end()) {
                ++added;
            } else if (found->second.vertices == print.vertices &&
                found->second.vertexBytes == print.vertexBytes &&
                found->second.indexCount == print.indexCount) {
                ++unchanged;
            } else {
                ++changed;
            }
        }
        const auto removed = previous.size() > current.size()
            ? static_cast<std::uint32_t>(previous.size() - current.size())
            : 0u;
        log::Write("renderer-mesh-churn: meshes=%zu unchanged=%u "
            "changed=%u added=%u removed=%u",
            meshes.size(), unchanged, changed, added, removed);
        previous = std::move(current);
    }

    if (frameId % 120 == 0) {
        const auto layouts = engine_draw_capture::LayoutCounters();
        log::Write("renderer-meshes: objects=%zu usable=%zu not-extracted=%u "
            "no-layout=%u layouts-recorded=%u overflow=%u hits=%llu "
            "misses=%llu unbuildable=%llu",
            scenePacket.objects.size(), meshes.size(), notExtracted, noLayout,
            layouts.recorded, layouts.overflow,
            static_cast<unsigned long long>(layouts.hits),
            static_cast<unsigned long long>(layouts.misses),
            static_cast<unsigned long long>(layouts.unbuildable));
    }

    // The scene must name the frame and view the camera packet declares, or
    // the backend refuses it for a frame mismatch.
    scenePacket.header.frameId = frameId;
    scenePacket.header.viewId = viewId;
    scenePacket.header.captureSequence = frameId;

    // The assembled geometry survives the frame. A settled scene reproduces
    // every mesh byte for byte -- measured at 940 of 940 unchanged -- and
    // decoding them again cost 454 ms per frame for an identical result.
    static raster::DecodedPacket s_assembled;
    static drawstream::GeometryArena s_arena;
    auto& rasterPacket = s_assembled;

    // Taken before the call, because a rejected assembly clears the objects it
    // was given and the count is the first thing worth knowing about it.
    const auto offered = scenePacket.objects.size();
    const auto assembleStarted = std::chrono::steady_clock::now();
    const auto assembled = drawstream::AssembleSceneGeometry(scenePacket,
        meshes, rasterPacket, assembly, &s_arena);
    s_stageAssembleUs += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - assembleStarted).count());
    if (assembled != drawstream::DrawStreamError::None) {
        // The arena describes geometry this assembly did not finish, so it
        // must not be carried into the next frame.
        s_arena.slots.clear();
        rasterPacket = {};
    }
    if (assembled != drawstream::DrawStreamError::None) {
        // The only path out of this function that said nothing at all. Both
        // encoders below log their rejection, so an absent line was read as
        // "the encoders were never reached" without anything confirming it.
        // Re-logged whenever the reason or the scale changes rather than once:
        // the mirror starts on a loading screen whose four draws assemble
        // perfectly well, and the failure that matters begins a minute later
        // when the cell arrives. A once-only line describes the loading screen
        // for the rest of the run.
        static auto lastError = drawstream::DrawStreamError::None;
        static std::size_t lastOffered = 0;
        if (assembled != lastError || offered != lastOffered) {
            log::Write("renderer-mirror: assembly rejected error=%s "
                "offered=%zu usable=%zu missing=%u unreadable=%u",
                drawstream::ToString(assembled), offered, meshes.size(),
                assembly.missingMeshes, assembly.unreadableMeshes);
            lastError = assembled;
            lastOffered = offered;
        }
        return false;
    }

    // The frame's texture library, and each material's index into it.
    //
    // AssembleSceneGeometry emits exactly one material per surviving object,
    // in object order, which is what lets the two be matched up here without
    // threading a resolver through the assembler itself.
    s_mirrorTexturedMaterials = 0;
    s_mirrorProbeMaterialCount =
        static_cast<std::uint32_t>(rasterPacket.materials.size());
    {
        StageTimer timer{s_stageLibraryUs};
        // The library survives the frame, and its entries keep their indices.
        //
        // It used to be copied and re-encoded from scratch every frame: at 114
        // textures that is 123 MB of memcpy per frame for a set that does not
        // change while the player stands still, and it measured 64 ms of a 177
        // ms frame. Now an index is handed out once and the bytes are rebuilt
        // only when a texture is actually added.
        auto dirty = s_mirrorTextureLibrary.empty();
        // Evicted entries are pruned first, and only they are dropped.
        //
        // Clearing the whole mapping on any single eviction cost a full
        // re-encode of a hundred and thirty megabytes, and then another on
        // each of the frames that re-added the entries one at a time: sixteen
        // evictions measured 186 ms of a frame. One eviction should cost one
        // re-encode of the survivors.
        //
        // Before the material loop, never after it. Compaction shifts every
        // later index down, so a material that had already been handed an
        // index would end up naming a different texture -- which is worse than
        // the white it would otherwise have shown.
        {
            // An index, once handed to a material, is never moved again.
            //
            // Compacting the list on eviction shifted every later index, and
            // those indices are part of what the encoded raster packet says,
            // so a single evicted texture invalidated the packet cache and
            // forced a full 82 MB re-encode. Streaming a cell evicts textures
            // continuously, so that turned a cache that should always hit into
            // one that mostly missed.
            //
            // An evicted entry keeps its place and is filled with a flat
            // fallback until it is resident again. The material still names
            // the same index, and shades from base colour meanwhile, which is
            // what it did before the texture ever arrived.
            s_mirrorLibraryResident.resize(s_mirrorLibraryIds.size(), false);
            for (std::size_t slot = 0; slot < s_mirrorLibraryIds.size();
                ++slot) {
                const auto resident = engine_texture_residency::Find(
                    s_mirrorLibraryIds[slot]) != nullptr;
                if (resident != s_mirrorLibraryResident[slot]) {
                    s_mirrorLibraryResident[slot] = resident;
                    dirty = true;
                }
            }
        }
        const auto materialCount = std::min(
            rasterPacket.materials.size(), scenePacket.objects.size());
        for (std::size_t index = 0; index < materialCount; ++index) {
            const auto found =
                s_mirrorObjectTextures.find(scenePacket.objects[index].objectId);
            if (found == s_mirrorObjectTextures.end() || found->second == 0) {
                continue;
            }
            auto existing = s_mirrorLibraryIndexOf.find(found->second);
            if (existing == s_mirrorLibraryIndexOf.end()) {
                if (s_mirrorLibraryIds.size() >=
                    scene::kSceneMaterialTextureCapacity) {
                    // Full. The rest keep the sentinel and shade from base
                    // colour, which is a partially textured frame rather than
                    // an index pointing past the bound array.
                    continue;
                }
                // Resident is not guaranteed: the engine streams textures in
                // and the budget is finite. A material whose texture is not
                // held keeps the sentinel and shades from base colour, which
                // is a partially textured frame rather than a wrong one.
                if (engine_texture_residency::Find(found->second) == nullptr) {
                    continue;
                }
                existing = s_mirrorLibraryIndexOf.emplace(found->second,
                    static_cast<std::uint32_t>(
                        s_mirrorLibraryIds.size())).first;
                s_mirrorLibraryIds.push_back(found->second);
                s_mirrorLibraryResident.push_back(true);
                dirty = true;
            }
            rasterPacket.materials[index].textureIndex = existing->second;
            ++s_mirrorTexturedMaterials;
        }
        if (dirty && !s_mirrorLibraryIds.empty()) {
            // Pointers, not copies. Copying every CapturedTexture into a
            // contiguous vector duplicated the whole library -- a hundred and
            // thirty megabytes -- before the encoder had copied anything, and
            // it happened every time a single texture was added.
            std::vector<const texture::CapturedTexture*> library;
            library.reserve(s_mirrorLibraryIds.size());
            // A slot whose texture is no longer resident is filled rather
            // than dropped, so every later index keeps its meaning.
            static const auto s_evictedFill =
                texture::MakeFallbackTexture(texture::FallbackTextureRole::White);
            for (const auto id : s_mirrorLibraryIds) {
                const auto* const resident = engine_texture_residency::Find(id);
                library.push_back(resident != nullptr ? resident : &s_evictedFill);
            }
            // An entry was evicted between the prune above and here. The
            // re-encode is abandoned rather than compacted: materials have
            // already been handed indices against the current mapping, and
            // shifting it now would point them at the wrong textures. The
            // previous bytes stay, any index past them resolves to the
            // sentinel in the backend, and next frame's prune fixes it before
            // a single material is assigned.
            {
                // What the library is actually made of.
                //
                // Ninety-three percent of textured draws resolve their base
                // colour from the register-0 convention rather than from the
                // shader's own declaration, so if that convention is wrong for
                // a family the mirror samples whatever else sits at register 0.
                // BC5 is a two-channel format and in this engine is a normal
                // map almost without exception: a BC5 population here is the
                // measurement that turns "the colours look green" into a
                // count.
                std::map<std::uint32_t, std::uint32_t> formats;
                for (const auto& entry : library) {
                    ++formats[static_cast<std::uint32_t>(entry->viewFormat)];
                }
                std::string message{"renderer-texlib-formats:"};
                for (const auto& [format, count] : formats) {
                    message += " f";
                    message += std::to_string(format);
                    message += "=";
                    message += std::to_string(count);
                }
                log::Write("%s", message.c_str());
            }
            if (texture::EncodeTextureLibrary(
                library, s_mirrorTextureLibrary) !=
                    texture::TexturePacketError::None) {
                // The only way out now that eviction is filled rather than
                // dropped. Starting the library over is correct and costs the
                // frames it takes to refill.
                s_mirrorLibraryIds.clear();
                s_mirrorLibraryIndexOf.clear();
                s_mirrorLibraryResident.clear();
                s_mirrorTextureLibrary.clear();
            }
            // Bumped on both paths: an emptied library is as much a change as
            // a re-encoded one, and the backend has to release what it holds.
            ++s_mirrorLibraryGeneration;
        }
        s_mirrorLibraryTextures =
            static_cast<std::uint32_t>(s_mirrorLibraryIds.size());
    }

    {
        const auto residency = engine_texture_residency::Counters();
        static std::uint32_t lastTextured = 0xFFFF'FFFFu;
        if (s_mirrorTexturedMaterials != lastTextured) {
            log::Write("renderer-texlib: materials=%zu textured=%u "
                "library=%u bytes=%zu resident=%u resident-bytes=%llu "
                "rejected=%u budget-dropped=%u unreadable=%u deferred=%llu",
                rasterPacket.materials.size(), s_mirrorTexturedMaterials,
                s_mirrorLibraryTextures, s_mirrorTextureLibrary.size(),
                residency.resident,
                static_cast<unsigned long long>(residency.residentBytes),
                residency.rejected, residency.budgetDropped,
                residency.unreadable,
                static_cast<unsigned long long>(s_mirrorLibraryDeferredPrunes));
            lastTextured = s_mirrorTexturedMaterials;
        }
    }

    rasterPacket.header.width = width;
    rasterPacket.header.height = height;
    rasterPacket.header.frameIndex = frameId;
    rasterPacket.header.viewportWidth = static_cast<float>(width);
    rasterPacket.header.viewportHeight = static_cast<float>(height);
    rasterPacket.header.viewportMaxDepth = 1.0f;
    rasterPacket.header.scissorWidth = width;
    rasterPacket.header.scissorHeight = height;

    StageTimer encodeTimer{s_stageEncodeUs};
    // The encoded packet is 72 MB and, once the geometry is stable, differs
    // between frames only in its header. The packet carries no checksum and
    // the header sits at offset zero, so the header can be rewritten in place
    // instead of re-encoding the whole thing at 41 ms a frame.
    //
    // Keyed on the material texture indices as well as the geometry, because
    // those are assigned above and change as the library fills in.
    std::uint64_t contentSignature = 0xCBF2'9CE4'8422'2325ull;
    const auto fold = [&contentSignature](const std::uint64_t value) {
        contentSignature =
            (contentSignature ^ value) * 0x0000'0100'0000'01B3ull;
    };
    fold(rasterPacket.vertices.size());
    fold(rasterPacket.indices.size());
    // Every field of the packet that is not vertex or index data. The arrays
    // themselves are covered by their sizes plus the arena's own rule: a mesh
    // only keeps its slot while it still names the same bytes.
    fold(rasterPacket.draws.size());
    fold(rasterPacket.materials.size());
    // Order-independent, because the order genuinely is not part of what the
    // frame looks like here and it is not stable.
    //
    // Objects are grouped in draw-submission order, which the engine varies
    // between frames, so an order-sensitive key never matched twice and the
    // packet was re-encoded every frame -- 50 ms for 67 MB that had not
    // changed. Every draw carries its own vertex range, index range and
    // material id, and this class is opaque and depth-tested, so a permutation
    // of the same draws is the same picture.
    std::uint64_t drawSet = 0;
    for (const auto& draw : rasterPacket.draws) {
        std::uint64_t one = 0xCBF2'9CE4'8422'2325ull;
        for (const auto field : {static_cast<std::uint64_t>(draw.firstIndex),
                 static_cast<std::uint64_t>(draw.indexCount),
                 static_cast<std::uint64_t>(
                     static_cast<std::uint32_t>(draw.vertexOffset)),
                 draw.materialId,
                 static_cast<std::uint64_t>(draw.frontFace)}) {
            one = (one ^ field) * 0x0000'0100'0000'01B3ull;
        }
        drawSet ^= one;
    }
    fold(drawSet);
    std::uint64_t materialSet = 0;
    for (const auto& material : rasterPacket.materials) {
        std::uint64_t one = 0xCBF2'9CE4'8422'2325ull;
        one = (one ^ material.resourceId) * 0x0000'0100'0000'01B3ull;
        one = (one ^ material.textureIndex) * 0x0000'0100'0000'01B3ull;
        materialSet ^= one;
    }
    fold(materialSet);
    static std::uint64_t s_encodedSignature = 0;
    static bool s_encodedValid = false;
    const auto reuseEncoded = s_encodedValid &&
        contentSignature == s_encodedSignature &&
        packetBytes.size() >= sizeof(raster::PacketHeaderV1);
    if (reuseEncoded) {
        raster::PacketHeaderV1 header{};
        std::memcpy(&header, packetBytes.data(), sizeof(header));
        // Only the fields this function set above. Everything else -- the
        // section offsets and counts -- describes geometry that has not moved.
        header.frameIndex = rasterPacket.header.frameIndex;
        header.width = rasterPacket.header.width;
        header.height = rasterPacket.header.height;
        header.viewportWidth = rasterPacket.header.viewportWidth;
        header.viewportHeight = rasterPacket.header.viewportHeight;
        header.viewportMaxDepth = rasterPacket.header.viewportMaxDepth;
        header.scissorWidth = rasterPacket.header.scissorWidth;
        header.scissorHeight = rasterPacket.header.scissorHeight;
        std::memcpy(packetBytes.data(), &header, sizeof(header));
    }
    if (reuseEncoded) {
        ++s_encodeHits;
    } else {
        ++s_encodeMisses;
    }
    const auto encoded = reuseEncoded
        ? raster::PacketResult{raster::PacketError::None, 0}
        : raster::EncodePacket(rasterPacket, packetBytes);
    if (encoded) {
        s_encodedSignature = contentSignature;
        s_encodedValid = true;
    } else {
        s_encodedValid = false;
    }
    if (!encoded) {
        log::Write("renderer-mirror: live geometry rejected error=%s",
            raster::ToString(encoded.error));
        return false;
    }
    const auto sceneEncoded =
        scene::EncodeScenePacket(scenePacket, sceneBytes);
    if (sceneEncoded != scene::ScenePacketError::None) {
        log::Write("renderer-mirror: live scene rejected error=%s",
            scene::ToString(sceneEncoded));
        return false;
    }
    // The encoded sizes, which nothing reported. The backend bounds the raster
    // packet and rejects an oversized one with a contract error that names no
    // size, so "the packet is too big" and "the packet is malformed" arrived as
    // the same message. Logged on the same cadence as the counts so the two can
    // be read together.
    if (frameId % 120 == 0) {
        log::Write("renderer-mirror: encoded raster-bytes=%zu scene-bytes=%zu "
            "vertices=%zu indices=%zu draws=%zu",
            packetBytes.size(), sceneBytes.size(),
            rasterPacket.vertices.size(), rasterPacket.indices.size(),
            rasterPacket.draws.size());

        // Where the geometry actually is, in the two spaces that can each put
        // a whole cell into a thin band on screen for entirely different
        // reasons. Object-space extents say what the decoder read out of the
        // pooled buffers; instance translations say where the engine placed
        // those objects. A cell whose objects are the right size but are all
        // placed near the origin is a transform that never arrived, and one
        // whose objects are already flat is a decode that read the wrong
        // field -- and the two are indistinguishable from the picture.
        auto low = std::numeric_limits<float>::infinity();
        std::array<float, 3> vertexLow{low, low, low};
        std::array<float, 3> vertexHigh{-low, -low, -low};
        for (const auto& vertex : rasterPacket.vertices) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                vertexLow[axis] =
                    std::min(vertexLow[axis], vertex.position[axis]);
                vertexHigh[axis] =
                    std::max(vertexHigh[axis], vertex.position[axis]);
            }
        }
        std::array<float, 3> originLow{low, low, low};
        std::array<float, 3> originHigh{-low, -low, -low};
        // How far the closest object sits from the origin, which is the one
        // measurement that separates the two readings of this data. A
        // camera-relative transform puts the ground the player is standing on
        // within a few dozen units of zero, because the origin is the eye. An
        // absolute one leaves the nearest object wherever the cell happens to
        // be, tens of thousands of units out. The player is never standing in
        // empty space, so a large nearest distance cannot mean anything else.
        auto nearest = std::numeric_limits<float>::infinity();
        std::array<float, 3> nearestAt{};
        for (const auto& instance : scenePacket.instances) {
            // Row-major, so the translation is the fourth column of each of
            // the first three rows.
            std::array<float, 3> placed{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                placed[axis] = instance.model[axis * 4 + 3];
                originLow[axis] = std::min(originLow[axis], placed[axis]);
                originHigh[axis] = std::max(originHigh[axis], placed[axis]);
            }
            const auto distance = std::sqrt(
                placed[0] * placed[0] + placed[1] * placed[1] +
                placed[2] * placed[2]);
            if (distance < nearest) {
                nearest = distance;
                nearestAt = placed;
            }
        }
        // What the engine's own pixel shaders say their constant buffers
        // contain. This is the answer to the question the block below could
        // not settle by reading numbers: the compiler recorded the names and
        // offsets, so they are read rather than inferred.
        {
            static std::array<engine_draw_capture::ShaderBufferLayout,
                engine_draw_capture::kShaderBufferLayoutCapacity> layouts{};
            const auto layoutCount = engine_draw_capture::CopyShaderBufferLayouts(
                layouts.data(), layouts.size());
            static std::array<engine_draw_capture::ShaderResourceBinding,
                engine_draw_capture::kShaderResourceCapacity> resources{};
            const auto resourceCount = engine_draw_capture::CopyShaderResourceBindings(
                resources.data(), resources.size());
            const auto reflection = engine_draw_capture::ShaderReflectionCounters();
            log::Write("renderer-reflect: shaders=%llu reflected=%llu failed=%llu "
                "layouts=%u layout-overflow=%u field-overflow=%u resources=%u "
                "resource-overflow=%u",
                static_cast<unsigned long long>(reflection.shaders),
                static_cast<unsigned long long>(reflection.reflected),
                static_cast<unsigned long long>(reflection.failed),
                reflection.layouts, reflection.layoutOverflow,
                reflection.fieldOverflow, reflection.resources,
                reflection.resourceOverflow);
            for (std::size_t index = 0; index < layoutCount; ++index) {
                const auto& layout = layouts[index];
                log::Write("renderer-reflect-buffer: name=%s bytes=%u fields=%u "
                    "shaders=%llu", layout.name, layout.byteWidth,
                    layout.fieldCount,
                    static_cast<unsigned long long>(layout.shaders));
                for (std::uint32_t field = 0; field < layout.fieldCount; ++field) {
                    const auto& entry = layout.fields[field];
                    log::Write("renderer-reflect-field: %s.%s offset=%u size=%u",
                        layout.name, entry.name, entry.offset, entry.size);
                }
            }
            // Printed under its own prefix so a live run can be checked for
            // plausibility before this is trusted for anything: real names,
            // small bind points, texture/sampler pairs sharing a slot. The
            // constant-buffer offsets above were wrong once and only caught
            // by reading real captured bytes -- this is that same check for
            // the bound-resource table, which has not yet had one.
            for (std::size_t index = 0; index < resourceCount; ++index) {
                const auto& resource = resources[index];
                const char* kind = resource.kind ==
                        static_cast<std::uint8_t>(shader::ResourceKind::Texture)
                    ? "texture"
                    : resource.kind ==
                        static_cast<std::uint8_t>(shader::ResourceKind::Sampler)
                    ? "sampler"
                    : "other";
                log::Write("renderer-reflect-resource: name=%s kind=%s slot=%u "
                    "count=%u shaders=%llu", resource.name, kind,
                    resource.bindPoint, resource.bindCount,
                    static_cast<unsigned long long>(resource.shaders));
            }
        }

        // The two measurements joined: a buffer the engine wrote, matched to
        // the layout its shaders declare, printed field by field under the
        // compiler's own names. Neither half is enough alone -- the reflection
        // says where a field is but not what is in it, and the sample says
        // what is in a word but not what the word means.
        {
            static std::array<engine_draw_capture::ShaderBufferLayout,
                engine_draw_capture::kShaderBufferLayoutCapacity> named{};
            const auto namedCount = engine_draw_capture::CopyShaderBufferLayouts(
                named.data(), named.size());
            std::array<engine_draw_capture::LightingSample,
                engine_draw_capture::kLightingSampleSlots> joinSamples{};
            const auto joinCount = engine_draw_capture::CopyLightingSamples(
                joinSamples.data(), joinSamples.size());
            for (std::size_t index = 0; index < joinCount; ++index) {
                const auto& sample = joinSamples[index];
                for (std::size_t entry = 0; entry < namedCount; ++entry) {
                    const auto& layout = named[entry];
                    if (layout.byteWidth != sample.byteWidth) continue;
                    for (std::uint32_t field = 0; field < layout.fieldCount;
                         ++field) {
                        const auto& member = layout.fields[field];
                        // Vectors and scalars only. A matrix is sixteen words
                        // and belongs on its own line, not folded into this.
                        const auto words = std::min<std::uint32_t>(
                            member.size / 4u, 4u);
                        if (words == 0 || member.offset % 4u != 0) continue;
                        const auto base = member.offset / 4u;
                        if (base + words > sample.byteWidth / 4u) continue;
                        char line[240]{};
                        int written = std::snprintf(line, sizeof(line),
                            "renderer-named: %s.%s@%u =", layout.name,
                            member.name, member.offset);
                        auto moved = false;
                        for (std::uint32_t word = 0; word < words; ++word) {
                            const auto slot = base + word;
                            if (sample.highest[slot] != sample.lowest[slot]) {
                                moved = true;
                            }
                            written += std::snprintf(line + written,
                                sizeof(line) - static_cast<std::size_t>(written),
                                " %.4f", static_cast<double>(
                                    sample.values[slot]));
                        }
                        // Whether the field held still across the frame's
                        // writes. A per-frame quantity that moves is a sign
                        // the buffer is shared, not that the sun is.
                        log::Write("%s%s", line, moved ? " (varies)" : "");
                    }
                }
            }
        }

        // The engine's pixel-shader constants, printed rather than decoded.
        // Which words in this block are the sun's direction and colour is a
        // measurement: an offset guessed from what the picture looks like
        // would light the scene by numbers nobody checked, and a direction
        // read from the wrong four bytes is indistinguishable from a sun in
        // the wrong place.
        std::array<engine_draw_capture::LightingSample,
            engine_draw_capture::kLightingSampleSlots> samples{};
        const auto sampleCount = engine_draw_capture::CopyLightingSamples(
            samples.data(), samples.size());
        const engine_draw_capture::LightingSample lighting =
            sampleCount != 0 ? samples[0]
                             : engine_draw_capture::LightingSample{};
        const auto constants = engine_draw_capture::ConstantCounters();
        for (std::size_t slot = 0; slot < sampleCount; ++slot) {
            const auto& sample = samples[slot];
            const auto words = std::min<std::uint32_t>(
                sample.byteWidth / 4u, 256u);
            // Only the words that never moved. A per-draw block holds the
            // scene's lighting and the object's own data side by side, and
            // the ones that held still across every draw of the frame are
            // the scene's.
            for (std::uint32_t base = 0; base < words; base += 8) {
                const auto take = std::min<std::uint32_t>(8, words - base);
                auto anyFixed = false;
                char line[240]{};
                int written = std::snprintf(line, sizeof(line),
                    "renderer-psfixed[%llx/%u@%03u]:",
                    static_cast<unsigned long long>(sample.handle),
                    sample.byteWidth, base);
                for (std::uint32_t index = 0; index < take; ++index) {
                    const auto word = base + index;
                    const auto span = sample.highest[word] -
                        sample.lowest[word];
                    if (span == 0.0f) {
                        anyFixed = true;
                        written += std::snprintf(line + written,
                            sizeof(line) - static_cast<std::size_t>(written),
                            " %.4f", static_cast<double>(
                                sample.values[word]));
                    } else {
                        written += std::snprintf(line + written,
                            sizeof(line) - static_cast<std::size_t>(written),
                            " .");
                    }
                }
                if (anyFixed) log::Write("%s", line);
            }
        }
        log::Write("renderer-lighting: valid=%s bytes=%u binds=%llu "
            "described=%llu sampled=%llu",
            lighting.valid ? "yes" : "no", lighting.byteWidth,
            static_cast<unsigned long long>(constants.psBinds),
            static_cast<unsigned long long>(constants.psDescribed),
            static_cast<unsigned long long>(constants.psSampled));
        // Whether a draw's base-colour texture is being resolved at all, and
        // where it stops if not. `notices` at zero means nothing is forwarding
        // pixel-shader bindings; `shaders-with-base` at zero means the
        // reflection never matched a material texture; `draws-missing` against
        // a healthy `draws-with` means the slot rule is finding a register the
        // engine leaves empty. One number could not separate those.
        log::Write("renderer-basecolor: notices=%llu shaders=%u "
            "shaders-with-base=%u draws-with=%llu draws-missing=%llu "
            "no-shader=%llu shader-unknown=%llu shader-no-base=%llu "
            "by-convention=%llu ps-shader-binds=%llu",
            static_cast<unsigned long long>(constants.psResourceNotices),
            constants.shadersDescribed, constants.shadersWithBaseColor,
            static_cast<unsigned long long>(constants.drawsWithBaseColor),
            static_cast<unsigned long long>(constants.drawsMissingBaseColor),
            static_cast<unsigned long long>(constants.drawsNoShader),
            static_cast<unsigned long long>(constants.drawsShaderUnknown),
            static_cast<unsigned long long>(constants.drawsShaderNoBase),
            static_cast<unsigned long long>(constants.drawsConventionBaseColor),
            static_cast<unsigned long long>(constants.psShaderBinds));
        // The 36% `no-shader` population, split by cause. Reported separately
        // because the two halves call for opposite responses and one number
        // could not tell them apart: `explicit-null` is the engine binding no
        // pixel shader for a depth-only pass, which correctly has no albedo,
        // while `never-set` is a thread whose state this module never observed
        // and is the only half that is a defect. The scene-depth split then
        // says whether the nulls are the depth prepass or the shadow cascades.
        {
            // Whether the winding a draw carries came from the engine or from
            // a default. `states=0` or `known=0` means the capture observed
            // nothing and every draw is using D3D11's default, in which case
            // the winding is still an assumption wearing a different name.
            const auto raster = engine_draw_capture::RasterizerState();
            log::Write("renderer-raster-state: states=%u overflow=%llu "
                "binds=%llu binds-unmatched=%llu draws-known=%llu "
                "draws-unknown=%llu front-ccw=%llu front-cw=%llu",
                raster.statesDescribed,
                static_cast<unsigned long long>(raster.stateOverflow),
                static_cast<unsigned long long>(raster.binds),
                static_cast<unsigned long long>(raster.bindsUnmatched),
                static_cast<unsigned long long>(raster.drawsKnownCull),
                static_cast<unsigned long long>(raster.drawsUnknownCull),
                static_cast<unsigned long long>(raster.drawsFrontCcw),
                static_cast<unsigned long long>(raster.drawsFrontCw));
        }
        {
            // Where the failing draws did bind textures. A register that
            // carries most of this population is the one the slot rule should
            // be reading, and `none` is the share that had nothing to find at
            // any register -- a different defect with a different fix.
            std::array<std::uint64_t, 16> slots{};
            std::uint64_t noResources = 0;
            const auto written = engine_draw_capture::MissingBaseColorSlots(
                slots.data(), slots.size(), noResources);
            std::string message{"renderer-basecolor-slots: none="};
            message += std::to_string(noResources);
            for (std::size_t slot = 0; slot < written; ++slot) {
                if (slots[slot] == 0) continue;
                message += " t";
                message += std::to_string(slot);
                message += "=";
                message += std::to_string(slots[slot]);
            }
            log::Write("%s", message.c_str());
        }
        log::Write("renderer-basecolor-null: no-shader=%llu explicit-null=%llu "
            "never-set=%llu null-scene-depth=%llu null-other-target=%llu",
            static_cast<unsigned long long>(constants.drawsNoShader),
            static_cast<unsigned long long>(constants.drawsShaderExplicitNull),
            static_cast<unsigned long long>(constants.drawsShaderNeverSet),
            static_cast<unsigned long long>(constants.drawsNullSceneDepth),
            static_cast<unsigned long long>(constants.drawsNullOtherTarget));
        // Every pixel-shader constant buffer the engine bound, by size and by
        // how often it was rewritten. A per-frame block is written once a
        // frame; a per-draw block is written thousands of times. That ratio is
        // what tells them apart, and it is why the sizes are printed rather
        // than one of them being picked.
        {
            std::array<engine_draw_capture::BufferReport, 32> reports{};
            const auto count = engine_draw_capture::CopyPsConstantReports(
                reports.data(), reports.size());
            for (std::size_t index = 0; index < count; ++index) {
                log::Write("renderer-psbuf[%zu]: bytes=%u maps=%llu "
                    "usage=%u cpu=%u",
                    index, reports[index].byteWidth,
                    static_cast<unsigned long long>(reports[index].maps),
                    reports[index].usage, reports[index].cpuAccessFlags);
            }
        }
        log::Write("renderer-nearest: distance=%.1f at=[%.1f,%.1f,%.1f]",
            static_cast<double>(nearest),
            static_cast<double>(nearestAt[0]),
            static_cast<double>(nearestAt[1]),
            static_cast<double>(nearestAt[2]));

        // Where those instances land on screen, computed here on the host with
        // the same view-projection the backend is handed. This separates the
        // two halves of the pipeline: if the normalised device box is already
        // flat, the transform chain collapsed the scene before any shader ran,
        // and if it spans the screen properly then the geometry is arriving
        // correctly and the fault is downstream. Counted by side of the near
        // plane too, because instances behind the camera project to the same
        // place as instances in front of it once w changes sign.
        auto ndcLow = std::numeric_limits<float>::infinity();
        std::array<float, 3> projectedLow{ndcLow, ndcLow, ndcLow};
        std::array<float, 3> projectedHigh{-ndcLow, -ndcLow, -ndcLow};
        std::uint32_t inFront = 0;
        std::uint32_t behind = 0;
        std::uint32_t onScreen = 0;
        for (const auto& instance : scenePacket.instances) {
            std::array<float, 4> clip{};
            for (std::size_t row = 0; row < 4; ++row) {
                clip[row] =
                    record.viewProjection.elements[row * 4 + 0] *
                        instance.model[3] +
                    record.viewProjection.elements[row * 4 + 1] *
                        instance.model[7] +
                    record.viewProjection.elements[row * 4 + 2] *
                        instance.model[11] +
                    record.viewProjection.elements[row * 4 + 3];
            }
            if (!(clip[3] > 0.0f)) {
                ++behind;
                continue;
            }
            ++inFront;
            std::array<float, 3> ndc{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                ndc[axis] = clip[axis] / clip[3];
            }
            // Only instances that actually land in the frustum are measured.
            // A box taken over every instance is not a diagnostic: geometry
            // beside or behind the camera divides by a vanishing w and reports
            // coordinates in the hundred thousands entirely correctly, so the
            // extremes describe the widest off-screen object rather than
            // anything about the picture.
            if (ndc[0] < -1.0f || ndc[0] > 1.0f || ndc[1] < -1.0f ||
                ndc[1] > 1.0f || ndc[2] < 0.0f || ndc[2] > 1.0f) {
                continue;
            }
            ++onScreen;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                projectedLow[axis] = std::min(projectedLow[axis], ndc[axis]);
                projectedHigh[axis] = std::max(projectedHigh[axis], ndc[axis]);
            }
        }
        // The same instances projected through the transpose. Exactly one of
        // the two readings can be right, and printing both means the answer is
        // read off rather than argued about: a matrix meant for row vectors
        // and multiplied as though it were meant for column vectors produces
        // finite, plausible-looking nonsense rather than an obvious failure.
        std::array<float, 3> flippedLow{ndcLow, ndcLow, ndcLow};
        std::array<float, 3> flippedHigh{-ndcLow, -ndcLow, -ndcLow};
        std::uint32_t flippedFront = 0;
        for (const auto& instance : scenePacket.instances) {
            std::array<float, 4> clip{};
            for (std::size_t column = 0; column < 4; ++column) {
                clip[column] =
                    record.viewProjection.elements[0 * 4 + column] *
                        instance.model[3] +
                    record.viewProjection.elements[1 * 4 + column] *
                        instance.model[7] +
                    record.viewProjection.elements[2 * 4 + column] *
                        instance.model[11] +
                    record.viewProjection.elements[3 * 4 + column];
            }
            if (!(clip[3] > 0.0f)) continue;
            ++flippedFront;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const auto ndc = clip[axis] / clip[3];
                flippedLow[axis] = std::min(flippedLow[axis], ndc);
                flippedHigh[axis] = std::max(flippedHigh[axis], ndc);
            }
        }
        // Four actual matrices. Every aggregate so far has been consistent
        // with two different readings of the same bytes, and a handful of
        // concrete rows settles which one is true in a way that a bounding box
        // over three thousand instances cannot.
        for (std::size_t sample = 0;
             sample < std::min<std::size_t>(4, scenePacket.instances.size());
             ++sample) {
            const auto& model = scenePacket.instances[sample].model;
            log::Write("renderer-instance[%zu]: "
                "[%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] "
                "[%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]",
                sample,
                static_cast<double>(model[0]), static_cast<double>(model[1]),
                static_cast<double>(model[2]), static_cast<double>(model[3]),
                static_cast<double>(model[4]), static_cast<double>(model[5]),
                static_cast<double>(model[6]), static_cast<double>(model[7]),
                static_cast<double>(model[8]), static_cast<double>(model[9]),
                static_cast<double>(model[10]), static_cast<double>(model[11]),
                static_cast<double>(model[12]), static_cast<double>(model[13]),
                static_cast<double>(model[14]), static_cast<double>(model[15]));
        }
        // The same instances through the projection alone. If the captured
        // transform is already object-to-view rather than object-to-world --
        // which a translation of eighty units along the third axis for a
        // cluster of parts eighty units from the eye would mean, since that
        // axis is then forward and not up -- then applying the view matrix on
        // top of it rotates the scene a second time, and the projection alone
        // is the correct chain. Counted the same way so the two are directly
        // comparable.
        std::uint32_t projectionOnScreen = 0;
        for (const auto& instance : scenePacket.instances) {
            std::array<float, 4> clip{};
            for (std::size_t row = 0; row < 4; ++row) {
                clip[row] =
                    record.projection.elements[row * 4 + 0] * instance.model[3] +
                    record.projection.elements[row * 4 + 1] * instance.model[7] +
                    record.projection.elements[row * 4 + 2] * instance.model[11] +
                    record.projection.elements[row * 4 + 3];
            }
            if (!(clip[3] > 0.0f)) continue;
            const auto x = clip[0] / clip[3];
            const auto y = clip[1] / clip[3];
            const auto z = clip[2] / clip[3];
            if (x < -1.0f || x > 1.0f || y < -1.0f || y > 1.0f ||
                z < 0.0f || z > 1.0f) {
                continue;
            }
            ++projectionOnScreen;
        }
        log::Write("renderer-projection-only: on-screen=%u",
            projectionOnScreen);

        // Whether narrowing would fix the picture, tested before spending any
        // more effort on finding the exact origin to narrow by. The component
        // medians of the instance translations are near the camera for the
        // simple reason that the loaded cell is centred on the player, so they
        // stand in for the real position well enough to answer one question:
        // does subtracting a camera position turn eight on-screen instances
        // into a scene, or is something else also wrong?
        //
        // Diagnostic only. This number is never narrowed by and never reaches
        // the packet -- an approximate origin would misplace the whole cell by
        // however far the approximation was out, which is exactly the fault
        // being diagnosed. It only says whether the diagnosis is right.
        if (!scenePacket.instances.empty()) {
            std::array<std::vector<float>, 3> axes{};
            for (auto& axis : axes) axis.reserve(scenePacket.instances.size());
            for (const auto& instance : scenePacket.instances) {
                axes[0].push_back(instance.model[3]);
                axes[1].push_back(instance.model[7]);
                axes[2].push_back(instance.model[11]);
            }
            std::array<double, 3> guess{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                std::sort(axes[axis].begin(), axes[axis].end());
                guess[axis] = static_cast<double>(
                    axes[axis][axes[axis].size() / 2]);
            }
            std::uint32_t guessedOnScreen = 0;
            for (const auto& instance : scenePacket.instances) {
                const auto narrowed = scene::NarrowInstance(instance,
                    std::span<const double, 3>{guess});
                std::array<float, 4> clip{};
                for (std::size_t row = 0; row < 4; ++row) {
                    clip[row] =
                        record.viewProjection.elements[row * 4 + 0] *
                            narrowed.model[3] +
                        record.viewProjection.elements[row * 4 + 1] *
                            narrowed.model[7] +
                        record.viewProjection.elements[row * 4 + 2] *
                            narrowed.model[11] +
                        record.viewProjection.elements[row * 4 + 3];
                }
                if (!(clip[3] > 0.0f)) continue;
                const auto x = clip[0] / clip[3];
                const auto y = clip[1] / clip[3];
                const auto z = clip[2] / clip[3];
                if (x < -1.0f || x > 1.0f || y < -1.0f || y > 1.0f ||
                    z < 0.0f || z > 1.0f) {
                    continue;
                }
                ++guessedOnScreen;
            }
            log::Write("renderer-origin-probe: median=[%.1f,%.1f,%.1f] "
                "on-screen=%u of %zu",
                guess[0], guess[1], guess[2], guessedOnScreen,
                scenePacket.instances.size());
        }

        // What space these transforms are actually in. Static world geometry
        // is overwhelmingly axis-aligned -- a house or a rock sits on the grid
        // or turned by a right angle -- so an object-to-world basis has one
        // near-unit component per row. An object-to-view basis has been turned
        // by the camera's arbitrary yaw as well, so almost none of its rows
        // look like that. Counting both tells whether the packet holds one
        // space or a mixture of two, which is the difference between a wrong
        // matrix and a wrong choice of matrix per draw.
        std::uint32_t axisAligned = 0;
        std::uint32_t flatThird = 0;
        for (const auto& instance : scenePacket.instances) {
            auto aligned = true;
            for (std::size_t row = 0; row < 3; ++row) {
                auto dominant = false;
                for (std::size_t column = 0; column < 3; ++column) {
                    if (std::abs(instance.model[row * 4 + column]) > 0.99f) {
                        dominant = true;
                    }
                }
                if (!dominant) aligned = false;
            }
            if (aligned) ++axisAligned;
            if (instance.model[11] == 0.0f) ++flatThird;
        }
        log::Write("renderer-space: axis-aligned=%u zero-third=%u total=%zu",
            axisAligned, flatThird, scenePacket.instances.size());

        // Percentiles, not extremes. Every bounds line so far has described
        // whichever single instance was furthest out, which in a scene that
        // mixes near geometry with distant terrain says nothing about where
        // the scene is. The median is what the picture is actually made of,
        // and the vertical percentiles are what say whether there is any
        // geometry below the eye at all -- there has to be, because the
        // player is standing on it.
        try {
            std::vector<float> distances;
            std::vector<float> vertical;
            distances.reserve(scenePacket.instances.size());
            vertical.reserve(scenePacket.instances.size());
            for (const auto& instance : scenePacket.instances) {
                const auto x = instance.model[3];
                const auto y = instance.model[7];
                const auto z = instance.model[11];
                distances.push_back(std::sqrt(x * x + y * y + z * z));
                vertical.push_back(z);
            }
            if (!distances.empty()) {
                std::sort(distances.begin(), distances.end());
                std::sort(vertical.begin(), vertical.end());
                const auto at = [](const std::vector<float>& values,
                                   const double fraction) {
                    const auto index = static_cast<std::size_t>(
                        fraction * static_cast<double>(values.size() - 1));
                    return static_cast<double>(values[index]);
                };
                log::Write("renderer-distribution: distance p10=%.1f p50=%.1f "
                    "p90=%.1f vertical p10=%.1f p50=%.1f p90=%.1f "
                    "below-eye=%zu",
                    at(distances, 0.10), at(distances, 0.50),
                    at(distances, 0.90), at(vertical, 0.10),
                    at(vertical, 0.50), at(vertical, 0.90),
                    static_cast<std::size_t>(std::count_if(vertical.begin(),
                        vertical.end(),
                        [](const float value) { return value < 0.0f; })));
            }
        } catch (...) {
            log::Write("renderer-distribution: unavailable");
        }
        log::Write("renderer-transposed: ndc x=[%.3f,%.3f] y=[%.3f,%.3f] "
            "z=[%.3f,%.3f] in-front=%u",
            static_cast<double>(flippedLow[0]),
            static_cast<double>(flippedHigh[0]),
            static_cast<double>(flippedLow[1]),
            static_cast<double>(flippedHigh[1]),
            static_cast<double>(flippedLow[2]),
            static_cast<double>(flippedHigh[2]),
            flippedFront);
        log::Write("renderer-viewproj: "
            "[%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f] "
            "[%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f]",
            static_cast<double>(record.viewProjection.elements[0]),
            static_cast<double>(record.viewProjection.elements[1]),
            static_cast<double>(record.viewProjection.elements[2]),
            static_cast<double>(record.viewProjection.elements[3]),
            static_cast<double>(record.viewProjection.elements[4]),
            static_cast<double>(record.viewProjection.elements[5]),
            static_cast<double>(record.viewProjection.elements[6]),
            static_cast<double>(record.viewProjection.elements[7]),
            static_cast<double>(record.viewProjection.elements[8]),
            static_cast<double>(record.viewProjection.elements[9]),
            static_cast<double>(record.viewProjection.elements[10]),
            static_cast<double>(record.viewProjection.elements[11]),
            static_cast<double>(record.viewProjection.elements[12]),
            static_cast<double>(record.viewProjection.elements[13]),
            static_cast<double>(record.viewProjection.elements[14]),
            static_cast<double>(record.viewProjection.elements[15]));
        log::Write("renderer-projected: ndc x=[%.3f,%.3f] y=[%.3f,%.3f] "
            "z=[%.3f,%.3f] on-screen=%u in-front=%u behind=%u",
            static_cast<double>(projectedLow[0]),
            static_cast<double>(projectedHigh[0]),
            static_cast<double>(projectedLow[1]),
            static_cast<double>(projectedHigh[1]),
            static_cast<double>(projectedLow[2]),
            static_cast<double>(projectedHigh[2]),
            onScreen, inFront, behind);
        log::Write("renderer-bounds: object x=[%.1f,%.1f] y=[%.1f,%.1f] "
            "z=[%.1f,%.1f] placed x=[%.1f,%.1f] y=[%.1f,%.1f] z=[%.1f,%.1f] "
            "instances=%zu",
            static_cast<double>(vertexLow[0]),
            static_cast<double>(vertexHigh[0]),
            static_cast<double>(vertexLow[1]),
            static_cast<double>(vertexHigh[1]),
            static_cast<double>(vertexLow[2]),
            static_cast<double>(vertexHigh[2]),
            static_cast<double>(originLow[0]),
            static_cast<double>(originHigh[0]),
            static_cast<double>(originLow[1]),
            static_cast<double>(originHigh[1]),
            static_cast<double>(originLow[2]),
            static_cast<double>(originHigh[2]),
            scenePacket.instances.size());
    }
    return true;
}


}

namespace {

// Binary PPM of the pixels the Vulkan renderer produced, written straight
// from the mirror's readback buffer.
void DumpMirrorFrame(
    const std::uint32_t slot,
    const std::uint32_t width,
    const std::uint32_t height) noexcept
{
    wchar_t base[MAX_PATH]{};
    const auto length = GetEnvironmentVariableW(
        L"VISUALFORGE_MIRROR_DUMP", base, static_cast<DWORD>(std::size(base)));
    if (length == 0 || length >= std::size(base)) return;
    try {
        std::wstring path{base};
        path.append(L".slot");
        path.append(std::to_wstring(slot));
        path.append(L".ppm");
        std::string header = "P6\n" + std::to_string(width) + " " +
            std::to_string(height) + "\n255\n";
        std::vector<std::uint8_t> body;
        body.reserve(header.size() +
            static_cast<std::size_t>(width) * height * 3);
        body.insert(body.end(), header.begin(), header.end());
        for (const auto& pixel : s_mirrorPixels) {
            body.push_back(pixel.r);
            body.push_back(pixel.g);
            body.push_back(pixel.b);
        }
        const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        static_cast<void>(WriteFile(handle, body.data(),
            static_cast<DWORD>(body.size()), &written, nullptr));
        CloseHandle(handle);
        log::Write("renderer-mirror: frame written slot=%u bytes=%lu",
            slot, written);
    } catch (...) {
    }
}

}

bool CompositeMirrorImpl(
    IDXGISwapChain* swapchain,
    const unsigned long long imageBase) noexcept
{
    // Each distinct reason names itself. A mirror that silently declines every
    // frame is indistinguishable from one that was never asked to run.
    //
    // This used to be one flag for all of them, which meant the first gate to
    // fire silenced every later gate for the rest of the run. The mirror
    // always declines its opening frames -- there is no world camera yet --
    // so the flag was spent on the startup reason before the run began, and a
    // failure that started a minute later, once the cell had loaded, produced
    // no line at all. The mirror looked like it was still presenting.
    const auto decline = [](const char* reason) {
        if (std::strncmp(s_mirrorFaultReason, reason,
                sizeof(s_mirrorFaultReason)) != 0) {
            log::Write(
                "renderer-mirror: not presenting reason=%s; frame remains "
                "vanilla", reason);
            std::strncpy(s_mirrorFaultReason, reason,
                sizeof(s_mirrorFaultReason) - 1);
            s_mirrorFaultReason[sizeof(s_mirrorFaultReason) - 1] = '\0';
        }
        return false;
    };
    if (s_bridge == nullptr || !s_bridge->Ready()) {
        return decline("bridge-not-ready");
    }
    if (swapchain == nullptr) return decline("no-swapchain");
    if (!s_host.RasterAvailable()) return decline("raster-unavailable");
    const auto width = s_bridge->Width();
    const auto height = s_bridge->Height();
    if (width == 0 || height == 0) return decline("zero-extent");

    const auto cameras = engine_camera_capture::ReadLiveCameras(
        static_cast<std::uintptr_t>(imageBase));
    renderer::camera::CameraScanResult world{};
    if (!engine_camera_capture::SelectWorldCamera(cameras, world)) {
        // Expected before the engine publishes a camera, so this one is not
        // latched: it must clear once the world loads.
        static bool reported = false;
        if (!reported) {
            log::Write(
                "renderer-mirror: waiting for a world camera cameras=%llu",
                static_cast<unsigned long long>(cameras.size()));
            reported = true;
        }
        return false;
    }

    if (!s_mirrorRasterCreated) {
        abi::RasterCreateRequestV1 create{};
        create.structSize = sizeof(create);
        // The raster session must land on the adapter the game already
        // presents from; without the LUID the device creation fails.
        create.adapterLuid = s_adapterLuid;
        create.flags = EnvironmentFlag(L"VISUALFORGE_VULKAN_VALIDATION")
            ? abi::RasterCreateValidation : 0u;
        abi::RasterStatusV1 status{};
        status.structSize = sizeof(status);
        if (!s_host.CreateRaster(create, status)) {
            if (!s_mirrorCreateFaultLogged) {
                log::Write(
                    "renderer-mirror: raster create failed diagnostic=%s; "
                    "frame remains vanilla", status.diagnostic);
                s_mirrorCreateFaultLogged = true;
            }
            return false;
        }
        s_mirrorRasterCreated = true;
    }

    ++s_mirrorFrameIndex;
    try {
        s_mirrorPixels.resize(
            static_cast<std::size_t>(width) * height);
    } catch (...) {
        return decline("allocation");
    }

    renderer::camera::CameraSeries series{};
    try {
        series.cameras.push_back(world);
    } catch (...) {
        return decline("allocation");
    }
    series.outputWidth = width;
    series.outputHeight = height;
    series.frameId = s_mirrorFrameIndex;
    series.engineFrameId = 0xE100'0000'0000'0000ull | s_mirrorFrameIndex;
    series.captureSequence = s_mirrorFrameIndex;
    series.threadId = GetCurrentThreadId();
    view::FramePacket framePacket{};
    const auto built =
        renderer::camera::BuildFrameSeries(series, framePacket);
    if (built != renderer::camera::CameraError::None) {
        return decline(renderer::camera::ToString(built));
    }
    const auto encoded = view::EncodeFramePacket(framePacket, s_mirrorFrame);
    if (encoded != view::FramePacketError::None) {
        return decline(view::ToString(encoded));
    }

    // The camera's translation, beside the geometry it is looking at. The
    // mirror was written when it drew its own tiles in camera-relative units,
    // where a view matrix carrying no translation is exactly right. Live
    // instance transforms are absolute world coordinates instead, and if the
    // view still carries no translation the scene is drawn from the world
    // origin -- which puts a cell tens of thousands of units away on the
    // horizon, looking indistinguishable from a correct render of something
    // genuinely distant. Measured rather than assumed, because the comment
    // asserting the translation is zero predates the live path entirely.
    if (s_mirrorFrameIndex % 120 == 0) {
        // Whether the camera's world position was recovered, and from where.
        // Without this line a narrowing that never ran and a narrowing that
        // ran with a zero origin produce the same unchanged numbers.
        log::Write("renderer-origin: found=%s at=[%.1f,%.1f,%.1f] "
            "residual=%.4f offset=%u candidates=%u",
            world.originFound ? "yes" : "no",
            world.cameraOrigin[0], world.cameraOrigin[1], world.cameraOrigin[2],
            static_cast<double>(world.originResidual), world.originOffset,
            world.candidateCount);
        log::Write("renderer-camera: slot=%u view-translation=[%.1f,%.1f,%.1f]"
            " last-row=[%.1f,%.1f,%.1f,%.1f] storage=%u",
            world.sourceSlot,
            static_cast<double>(world.view.elements[3]),
            static_cast<double>(world.view.elements[7]),
            static_cast<double>(world.view.elements[11]),
            static_cast<double>(world.view.elements[12]),
            static_cast<double>(world.view.elements[13]),
            static_cast<double>(world.view.elements[14]),
            static_cast<double>(world.view.elements[15]),
            static_cast<unsigned>(world.storage));
    }

    // The engine's own scene, or nothing. There used to be a fallback here
    // that drew the mirror's test quads when the live scene could not be
    // built, and it was a mistake: the captured frames then showed geometry
    // from an earlier phase's fixture looking exactly like a rendered cell.
    // Declining leaves the vanilla frame on screen, which is honest about
    // having nothing to show.
    drawstream::AssemblyResult assembly{};
    drawstream::TranslationResult translated{};
    auto liveScene = false;
    try {
        StageTimer geometryTimer{s_stageGeometryUs};
        liveScene = BuildLiveSceneGeometry(width, height,
            framePacket.header.frameId, framePacket.views.front().viewId,
            framePacket.views.front(), world.originCandidates,
            s_mirrorPacket, s_mirrorScene, assembly, translated);
    } catch (...) {
        liveScene = false;
    }

    // The breakdown, whether or not the frame was usable. A single rejected
    // total says the scene is thin; only the per-reason counts say which rule
    // made it thin, and a rule that rejects everything is indistinguishable
    // from one that rejects nothing without them. Logged on a cadence rather
    // than once, because the answer changes as the cell loads.
    if (s_mirrorFrameIndex % 120 == 0) {
        log::Write("renderer-draws: objects=%u instances=%u rejected=%llu "
            "reused=%llu dropped=%llu no-transform=%u non-affine=%u "
            "mirrored=%u singular=%u non-finite=%u unknown-vb=%u "
            "unknown-ib=%u empty=%u not-tri=%u range=%u zero-inst=%u "
            "depth-only=%u offscreen=%u attributed=%llu slots=%zu",
            translated.objects, translated.instances,
            static_cast<unsigned long long>(translated.rejectedDraws),
            static_cast<unsigned long long>(translated.reusedMeshes),
            static_cast<unsigned long long>(translated.droppedDraws),
            RejectionCount(translated, drawstream::DrawStreamError::NoTransform),
            RejectionCount(translated,
                drawstream::DrawStreamError::NonAffineTransform),
            RejectionCount(translated,
                drawstream::DrawStreamError::MirroredTransform),
            RejectionCount(translated,
                drawstream::DrawStreamError::SingularTransform),
            RejectionCount(translated,
                drawstream::DrawStreamError::NonFiniteTransform),
            RejectionCount(translated,
                drawstream::DrawStreamError::UnknownVertexBuffer),
            RejectionCount(translated,
                drawstream::DrawStreamError::UnknownIndexBuffer),
            RejectionCount(translated,
                drawstream::DrawStreamError::EmptyGeometry),
            RejectionCount(translated,
                drawstream::DrawStreamError::NotATriangleList),
            RejectionCount(translated,
                drawstream::DrawStreamError::IndexCountOutOfRange),
            RejectionCount(translated,
                drawstream::DrawStreamError::ZeroInstances));
    }

    if (!liveScene) {
        s_mirrorScene.clear();
        return decline("live-scene-unavailable");
    }
    if (liveScene != s_mirrorLiveLogged) {
        log::Write("renderer-mirror: source=live-scene objects=%u missing=%u "
            "vertices=%llu indices=%llu normals=%llu no-normals=%llu",
            assembly.drawnObjects, assembly.missingMeshes,
            static_cast<unsigned long long>(assembly.vertices),
            static_cast<unsigned long long>(assembly.indices),
            static_cast<unsigned long long>(assembly.verticesWithNormals),
            static_cast<unsigned long long>(assembly.verticesWithoutNormals));
        s_mirrorLiveLogged = liveScene;
    }

    // The frame's lighting, resolved from the engine's own declarations. This
    // is the whole point of the reflection work: without it the mirror sends
    // no light packet, the backend has no environment, and every surface
    // renders as flat albedo -- which is exactly what the mirrored scene
    // looked like.
    {
        StageTimer lightingTimer{s_stageLightingUs};
        BuildMirrorLighting();
    }
    if (!s_mirrorLightEverLogged ||
        s_mirrorLightError != s_mirrorLightLogged ||
        s_mirrorLightCandidates != s_mirrorLightCandidatesLogged) {
        log::Write("renderer-mirror-light: source=%s status=%s bytes=%llu "
            "packet=%s layouts=%u samples=%u candidates=%u width=%u",
            kMirrorLightBufferName,
            environment::ToString(s_mirrorLightError),
            static_cast<unsigned long long>(s_mirrorLight.size()),
            lighting::ToString(s_mirrorLightPacketError),
            s_mirrorLightLayouts, s_mirrorLightSamples,
            s_mirrorLightCandidates, s_mirrorLightWidth);
        s_mirrorLightLogged = s_mirrorLightError;
        s_mirrorLightCandidatesLogged = s_mirrorLightCandidates;
        s_mirrorLightEverLogged = true;
    }

    abi::RasterFrameRequestV1 request{};
    request.structSize = sizeof(request);
    if (!s_mirrorLight.empty()) {
        request.lightData = reinterpret_cast<std::uint64_t>(
            s_mirrorLight.data());
        request.lightSize = s_mirrorLight.size();
    }
    if (!s_mirrorTextureLibrary.empty()) {
        request.textureLibraryData = reinterpret_cast<std::uint64_t>(
            s_mirrorTextureLibrary.data());
        request.textureLibrarySize = s_mirrorTextureLibrary.size();
        request.textureLibraryGeneration = s_mirrorLibraryGeneration;
    }
    request.packetData = reinterpret_cast<std::uint64_t>(
        s_mirrorPacket.data());
    request.packetSize = s_mirrorPacket.size();
    request.outputData = reinterpret_cast<std::uint64_t>(
        s_mirrorPixels.data());
    request.outputRowPitch = width * sizeof(raster::Rgba8);
    request.outputCapacity =
        static_cast<std::uint64_t>(request.outputRowPitch) * height;
    request.frameData = reinterpret_cast<std::uint64_t>(
        s_mirrorFrame.data());
    request.frameSize = s_mirrorFrame.size();
    if (!s_mirrorScene.empty()) {
        request.sceneData = reinterpret_cast<std::uint64_t>(
            s_mirrorScene.data());
        request.sceneSize = s_mirrorScene.size();
    }
    // The withheld arm runs first, so the image that reaches the swapchain is
    // always the one with the library applied. It costs one extra submission,
    // once per session, on the first frame that has both a scene and a
    // library to compare.
    auto probeArmed = false;
    if (!s_mirrorLibraryProbeDone && !s_mirrorScene.empty() &&
        !s_mirrorTextureLibrary.empty() &&
        s_mirrorLibraryTextures >= kMirrorLibraryProbeMinimumTextures) {
        try {
            s_mirrorLibraryProbePixels.assign(s_mirrorPixels.size(),
                raster::Rgba8{});
            probeArmed = true;
        } catch (...) {
            probeArmed = false;
        }
        if (probeArmed) {
            auto withheld = request;
            withheld.textureLibraryData = 0;
            withheld.textureLibrarySize = 0;
            withheld.outputData = reinterpret_cast<std::uint64_t>(
                s_mirrorLibraryProbePixels.data());
            abi::RasterStatusV1 withheldStatus{};
            withheldStatus.structSize = sizeof(withheldStatus);
            probeArmed = static_cast<bool>(
                s_host.RenderRasterFrame(withheld, withheldStatus));
            if (!probeArmed) {
                log::Write("renderer-mirror-texture-probe: withheld render "
                    "failed diagnostic=%s", withheldStatus.diagnostic);
                s_mirrorLibraryProbeDone = true;
            }
        }
    }

    abi::RasterStatusV1 status{};
    status.structSize = sizeof(status);
    const auto renderStarted = std::chrono::steady_clock::now();
    const auto renderOk = s_host.RenderRasterFrame(request, status);
    s_stageRenderUs += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - renderStarted).count());
    if (!renderOk) {
        if (!s_mirrorRenderFaultLogged) {
            log::Write(
                "renderer-mirror: render failed diagnostic=%s; frame remains "
                "vanilla", status.diagnostic);
            s_mirrorRenderFaultLogged = true;
        }
        return false;
    }

    if (probeArmed) {
        std::uint64_t differing = 0;
        std::uint64_t maximumChannel = 0;
        const auto count = std::min(
            s_mirrorPixels.size(), s_mirrorLibraryProbePixels.size());
        for (std::size_t index = 0; index < count; ++index) {
            const auto& applied = s_mirrorPixels[index];
            const auto& withheld = s_mirrorLibraryProbePixels[index];
            const auto channel = [](const std::uint8_t left,
                                    const std::uint8_t right) {
                return static_cast<std::uint64_t>(
                    left > right ? left - right : right - left);
            };
            const auto worst = std::max({channel(applied.r, withheld.r),
                channel(applied.g, withheld.g),
                channel(applied.b, withheld.b)});
            if (worst != 0) ++differing;
            maximumChannel = std::max(maximumChannel, worst);
        }
        ++s_mirrorLibraryProbeAttempts;
        log::Write("renderer-mirror-texture-probe: attempt=%u pixels=%llu "
            "differing=%llu max-channel=%llu library=%u textured=%u "
            "materials=%u",
            s_mirrorLibraryProbeAttempts,
            static_cast<unsigned long long>(count),
            static_cast<unsigned long long>(differing),
            static_cast<unsigned long long>(maximumChannel),
            s_mirrorLibraryTextures, s_mirrorTexturedMaterials,
            s_mirrorProbeMaterialCount);
        // Latched on the first frame that actually shows a difference. A zero
        // is not a result here -- it means the textured materials this frame
        // happened to contribute no visible pixels -- so it is retried rather
        // than reported as the answer, up to a cap that keeps the extra
        // submission off the steady-state frame cost.
        if (differing != 0 ||
            s_mirrorLibraryProbeAttempts >= kMirrorLibraryProbeMaximumAttempts) {
            s_mirrorLibraryProbeDone = true;
            s_mirrorLibraryProbePixels.clear();
            s_mirrorLibraryProbePixels.shrink_to_fit();
        }
    }

    ID3D11Texture2D* backbuffer{};
    if (FAILED(swapchain->GetBuffer(
            0, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&backbuffer)))) {
        return decline("backbuffer");
    }
    StageTimer presentTimer{s_stagePresentUs};
    const auto submitted = s_bridge->SubmitImage(
        s_host, backbuffer, s_bridgeFrame++, s_mirrorPixels.data(),
        static_cast<std::uint64_t>(s_mirrorPixels.size()) *
            sizeof(raster::Rgba8));
    backbuffer->Release();
    if (!submitted) {
        if (submitted.error != D3D11BridgeError::SlotBusy &&
            !s_mirrorSubmitFaultLogged) {
            log::Write(
                "renderer-mirror: submit failed error=%u; frame remains "
                "vanilla", static_cast<unsigned>(submitted.error));
            s_mirrorSubmitFaultLogged = true;
        }
        return false;
    }
    // The selected camera is reported whenever it changes, not only on the
    // first frame. The first frame lands before the engine has published its
    // world view, so a first-frame-only report would permanently describe a
    // loading camera the mirror stopped using seconds later.
    static std::uint32_t reportedSlot = 0xFFFF'FFFFu;
    if (!s_mirrorFirstFrameLogged || world.sourceSlot != reportedSlot) {
        view::ClipPlanes planes{};
        static_cast<void>(view::ExtractClipPlanes(world.projection,
            view::ProjectionMode::Perspective,
            view::Handedness::LeftHanded, planes));
        log::Write(
            "renderer-mirror: %s extent=%ux%u camera-slot=%u near=%.3f "
            "far=%.1f cameras=%llu validation-errors=%u suppression=off",
            s_mirrorFirstFrameLogged ? "camera changed"
                                     : "first-frame displayed",
            width, height, world.sourceSlot,
            static_cast<double>(planes.nearPlane),
            static_cast<double>(planes.farPlane),
            static_cast<unsigned long long>(cameras.size()),
            s_bridge->ValidationErrorCount());
        s_mirrorFirstFrameLogged = true;
        reportedSlot = world.sourceSlot;
        // Writing the frame the renderer actually produced is evidence that
        // does not depend on the game window owning the foreground, which a
        // window screenshot does. One file per camera slot, so the world
        // camera's frame is identifiable rather than whichever frame the
        // screenshot happened to catch.
        DumpMirrorFrame(world.sourceSlot, width, height);
    } else if (liveScene && s_mirrorFrameIndex % 120 == 0) {
        // Rewritten periodically once the scene is live, because a cell fills
        // in over hundreds of frames and the first frame after the camera
        // appears holds almost none of it. The last write is the one that
        // shows what the renderer reached.
        //
        // Every hundred and twenty frames rather than six hundred: the mirror
        // reads a whole frame back per frame and runs at about four a second,
        // so six hundred is two and a half minutes -- longer than a capture
        // window. A run that ended carrying sixteen hundred objects therefore
        // left an artifact holding sixteen, and nothing in the file said so.
        // The counts are logged beside the write so an artifact states what it
        // contains instead of being read as the settled scene.
        // `unreadable` is the one that names a mesh whose bytes did not decode
        // as positions. It is counted by the assembly and was reported
        // nowhere, so a frame that dropped nearly every object it had looked
        // exactly like a frame that only ever had a few.
        log::Write("renderer-mirror: dump objects=%u missing=%u unreadable=%u"
            " vertices=%llu indices=%llu frame=%llu",
            assembly.drawnObjects, assembly.missingMeshes,
            assembly.unreadableMeshes,
            static_cast<unsigned long long>(assembly.vertices),
            static_cast<unsigned long long>(assembly.indices),
            static_cast<unsigned long long>(s_mirrorFrameIndex));
        DumpMirrorFrame(world.sourceSlot, width, height);
    }
    return true;
}

// The frame boundary for phase 25, and the only place the draw hooks' state is
// written.
//
// Present is exactly the right moment: the draws of the frame that follows all
// arrive after it, so publishing here decides that frame and cannot half-decide
// the one in progress. `worldReproduced` is this frame's actual outcome --
// whether an image reached the swapchain -- which is the honest predictor for
// the next one and degrades safely, because the first frame the mirror fails
// is the frame after which suppression stops.
bool CompositeMirror(
    IDXGISwapChain* swapchain,
    const unsigned long long imageBase) noexcept
{
    const auto started = std::chrono::steady_clock::now();
    const auto presented = CompositeMirrorImpl(swapchain, imageBase);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();

    world_suppression::Publish(world_suppression::Enabled(), presented);

    if (world_suppression::Enabled()) {
        s_mirrorMicrosecondsTotal += static_cast<std::uint64_t>(elapsed);
        ++s_mirrorTimedFrames;
        if (static_cast<std::uint64_t>(elapsed) > s_mirrorMicrosecondsWorst) {
            s_mirrorMicrosecondsWorst = static_cast<std::uint64_t>(elapsed);
        }
        // Reported as a running mean and a worst case rather than per frame: a
        // line per frame is unreadable at sixty of them a second, and a mean
        // alone hides the stall that the library rebuild puts in one frame.
        if (s_mirrorTimedFrames % kStageReportInterval == 0) {
            const auto counters = world_suppression::Snapshot();
            log::Write("renderer-suppression: frames=%llu mirror-mean-us=%llu "
                "mirror-worst-us=%llu suppressed=%llu forwarded=%llu "
                "presented=%s",
                static_cast<unsigned long long>(s_mirrorTimedFrames),
                static_cast<unsigned long long>(
                    s_mirrorMicrosecondsTotal / s_mirrorTimedFrames),
                static_cast<unsigned long long>(s_mirrorMicrosecondsWorst),
                static_cast<unsigned long long>(counters.suppressed),
                static_cast<unsigned long long>(counters.forwarded),
                presented ? "yes" : "no");
            // The same frame, broken down. Means rather than totals so the
            // numbers add up against mirror-mean-us above and a stage can be
            // read as a share of the frame directly. `geometry` contains
            // `library` and `encode`, which are reported inside it because
            // each has its own fix.
            // Windowed, not cumulative. A mean taken since load is dominated
            // by the cell-loading phase forever after, which made a steady
            // state of half a millisecond read as thirty.
            const auto window = [](std::uint64_t& total,
                                   std::uint64_t& reported) {
                const auto delta = total - reported;
                reported = total;
                return static_cast<unsigned long long>(delta / kStageReportInterval);
            };
            log::Write("renderer-suppression-stages: geometry-us=%llu "
                "library-us=%llu encode-us=%llu lighting-us=%llu "
                "render-us=%llu present-us=%llu",
                window(s_stageGeometryUs, s_reportedGeometryUs),
                window(s_stageLibraryUs, s_reportedLibraryUs),
                window(s_stageEncodeUs, s_reportedEncodeUs),
                window(s_stageLightingUs, s_reportedLightingUs),
                window(s_stageRenderUs, s_reportedRenderUs),
                window(s_stagePresentUs, s_reportedPresentUs));
            log::Write("renderer-suppression-geometry: translate-us=%llu "
                "extract-us=%llu assemble-us=%llu collect-us=%llu",
                window(s_stageTranslateUs, s_reportedTranslateUs),
                window(s_stageExtractUs, s_reportedExtractUs),
                window(s_stageAssembleUs, s_reportedAssembleUs),
                window(s_stageCollectUs, s_reportedCollectUs));
            log::Write("renderer-encode-cache: hits=%llu misses=%llu",
                static_cast<unsigned long long>(s_encodeHits),
                static_cast<unsigned long long>(s_encodeMisses));
        }
    }
    return presented;
}

void BeforeResize() noexcept
{
    if (s_bridge == nullptr || !s_bridge->Ready()) {
        return;
    }
    const auto destroyed = s_bridge->DrainAndDestroy(s_host, 10000);
    if (!destroyed) {
        log::Write(
            "renderer-bridge: resize drain failed error=%u; bridge disabled",
            static_cast<unsigned>(destroyed.error));
        s_bridge.reset();
        s_resizePending = false;
        return;
    }
    log::Write(
        "renderer-bridge: resize drained epoch=%llu validation-errors=%u",
        static_cast<unsigned long long>(s_bridgeEpoch),
        s_bridge->ValidationErrorCount());
    ++s_bridgeEpoch;
    s_resizePending = true;
}

void AfterResize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const unsigned width,
    const unsigned height,
    const unsigned format) noexcept
{
    if (!s_resizePending) {
        return;
    }
    s_bridge.reset();
    static_cast<void>(InitializePatternBridge(
        device, context, width, height, format));
}

}
