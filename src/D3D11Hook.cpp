#include "D3D11Hook.h"
#include "CaptureRequests.h"
#include "Config.h"
#include "EngineSettings.h"
#include "EngineCameraCapture.h"
#include "EngineDrawCapture.h"
#include "EngineMeshExtractor.h"

#include "renderer_core/EngineDrawStream.h"

#include <cstdio>
#include <vector>
#include "EngineMeshCapture.h"
#include "EngineTextureCapture.h"
#include "Log.h"
#include "DepthCapture.h"
#include "FlexGuard.h"
#include "Lut.h"
#include "Overlay.h"
#include "PostProcess.h"
#include "RendererBackendProbe.h"
#include "RendererObservation.h"
#include "WeaponDebris.h"

#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace vf::d3d {

namespace {

using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn s_origPresent = nullptr;
ResizeBuffersFn s_origResizeBuffers = nullptr;

IDXGISwapChain* s_swapchain = nullptr;
ID3D11Device* s_device = nullptr;
ID3D11DeviceContext* s_context = nullptr;
ID3D11RenderTargetView* s_backbufferRTV = nullptr;
HWND s_window = nullptr;
WNDPROC s_origWndProc = nullptr;
bool s_imguiReady = false;
bool s_initFailed = false;
UINT s_backbufferWidth = 0;
UINT s_backbufferHeight = 0;

template <typename T>
void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

bool CreateBackbufferRTV()
{
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(s_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer))))
        return false;
    HRESULT hr = s_device->CreateRenderTargetView(backbuffer, nullptr, &s_backbufferRTV);
    backbuffer->Release();
    return SUCCEEDED(hr);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    // The toggle key may be an ordinary key (WM_KEYDOWN) or a system key such as
    // F10/F-keys with Alt (WM_SYSKEYDOWN). Handle both; ignore auto-repeat (bit 30).
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
        int(wparam) == config::Get().toggleKey && !(lparam & (1 << 30))) {
        overlay::g_visible = !overlay::g_visible;
        ImGui::GetIO().MouseDrawCursor = overlay::g_visible;
        return 0;
    }

    if (s_imguiReady && overlay::g_visible) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
        // Swallow input while the overlay is open so clicks don't reach game menus.
        switch (msg) {
            case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
            case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            case WM_SYSKEYDOWN: case WM_SYSKEYUP:
                return 0;
            default:
                break;
        }
    }
    return CallWindowProcW(s_origWndProc, hwnd, msg, wparam, lparam);
}

bool InitializeForSwapchain(IDXGISwapChain* swap)
{
    static_cast<void>(engine_mesh_capture::Retry());
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&s_device)))) {
        log::Write("hook: GetDevice failed");
        return false;
    }
    s_device->GetImmediateContext(&s_context);

    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(swap->GetDesc(&desc))) {
        log::Write("hook: GetDesc failed");
        return false;
    }
    s_window = desc.OutputWindow;
    s_swapchain = swap;

    if (!CreateBackbufferRTV()) {
        log::Write("hook: backbuffer RTV creation failed");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // don't scatter imgui.ini into the game folder
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    if (!ImGui_ImplWin32_Init(s_window) || !ImGui_ImplDX11_Init(s_device, s_context)) {
        log::Write("hook: ImGui backend init failed");
        return false;
    }

    s_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(s_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));

    config::Load();
    config::ApplyToPost();
    static_cast<void>(renderer_backend_probe::ProbeOnce(s_device));
    static_cast<void>(engine_draw_capture::Install());
    engine_mesh_extractor::Configure(s_device, s_context);
    static_cast<void>(renderer_backend_probe::InitializePatternBridge(
        s_device,
        s_context,
        desc.BufferDesc.Width,
        desc.BufferDesc.Height,
        static_cast<unsigned>(desc.BufferDesc.Format)));

    if (config::Get().lutFile[0]) {
        wchar_t wname[128];
        MultiByteToWideChar(CP_UTF8, 0, config::Get().lutFile, -1, wname, 128);
        wchar_t full[MAX_PATH];
        swprintf_s(full, L"%sLUTs\\%s", config::PluginDir(), wname);
        std::string err;
        if (!lut::LoadInto(s_device, full, err))
            log::Write("lut: load '%s' failed: %s", config::Get().lutFile, err.c_str());
    }

    settings::ResolveAll();

    if (config::Get().weaponDebrisCrashFix)
        debris::Install(s_device);

    // Runs after the INIs are loaded but before gameplay, which is the window in which
    // turning weapon debris off still prevents the engine from ever calling into Flex.
    if (config::Get().blockWeaponDebris)
        flexguard::EnforceSafety(true);

    s_backbufferWidth = desc.BufferDesc.Width;
    s_backbufferHeight = desc.BufferDesc.Height;

    depth::SetTargetSize(desc.BufferDesc.Width, desc.BufferDesc.Height);
    depth::Install(s_device, s_context);
    depth::SetCaptureMode(config::Get().depthCaptureMode);
    depth::SetSelfTestEnabled(config::Get().depthSelfTest);

    // Reported here rather than at plugin load: the game parses its INIs after F4SE loads
    // plugins, so reading earlier would report built-in defaults instead of real settings.
    {
        static const char* kReport[] = {
            "fDefaultWorldFOV:Display",
            "fDefault1stPersonFOV:Display",
            "fDefaultFOV:Display",
            "bUseAutoDynamicResolution:Display",
            "fNearDistance:Display",
        };
        for (const char* name : kReport) {
            settings::Entry* e = settings::Find(name);
            if (!e) {
                log::Write("live: %s = <unresolved>", name);
            } else if (e->type == settings::Type::Float) {
                log::Write("live: %s = %.3f", name, settings::GetFloat(*e));
            } else if (e->type == settings::Type::Bool) {
                log::Write("live: %s = %d", name, settings::GetBool(*e) ? 1 : 0);
            } else if (e->type == settings::Type::Int) {
                log::Write("live: %s = %d", name, settings::GetInt(*e));
            }
        }
    }

    s_imguiReady = true;
    renderer_observation::OnSwapchainReady(
        desc.BufferDesc.Width,
        desc.BufferDesc.Height,
        static_cast<std::uint32_t>(desc.BufferDesc.Format),
        desc.SampleDesc.Count);
    log::Write("hook: initialized (hwnd=%p, %ux%u, format=%d)", s_window,
               desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format);
    return true;
}

HRESULT WINAPI HookedPresent(IDXGISwapChain* swap, UINT sync, UINT flags)
{
    if (!s_imguiReady && !s_initFailed) {
        if (!InitializeForSwapchain(swap)) {
            s_initFailed = true; // stay out of the way; the game keeps rendering
            log::Write("hook: initialization failed — overlay disabled");
        }
    }

    if (s_imguiReady && swap == s_swapchain) {
        capture_requests::Poll();
        if (engine_draw_capture::Enabled()) {
            // Logged after a settle rather than every frame: the first
            // frames are a loading screen, and a menu draw count says
            // nothing about what a world costs.
            static unsigned frames = 0;
            const auto summary = engine_draw_capture::EndFrame();
            if (frames == 1800) {
                // Described once, after the world has settled: what a
                // full-scene capture would have to stage each frame.
                engine_draw_capture::BufferReport reports[
                    engine_draw_capture::kMaximumReportedBuffers]{};
                const auto vertexCount =
                    engine_draw_capture::DescribeVertexBuffers(
                        reports, std::size(reports));
                for (std::size_t i = 0; i < vertexCount; ++i) {
                    log::Write("draw-capture: vertex-buffer[%zu] bytes=%u "
                        "stride=%u usage=%u cpu=%u bind=%u draws=%llu",
                        i, reports[i].byteWidth, reports[i].stride,
                        reports[i].usage, reports[i].cpuAccessFlags,
                        reports[i].bindFlags,
                        static_cast<unsigned long long>(reports[i].draws));
                }
                const auto constantCount =
                    engine_draw_capture::DescribeConstantBuffers(
                        reports, std::size(reports));
                engine_draw_capture::ConstantSample samples[
                    engine_draw_capture::kMaximumConstantSamples]{};
                const auto sampleCount =
                    engine_draw_capture::CollectConstantSamples(
                        samples, std::size(samples));
                for (std::size_t i = 0; i < sampleCount; ++i) {
                    char text[512];
                    int written = std::snprintf(text, sizeof(text),
                        "draw-capture: cb-sample[%zu] bytes=%u:",
                        i, samples[i].byteWidth);
                    const auto floats = samples[i].bytes / sizeof(float);
                    for (std::size_t f = 0; f < floats &&
                         written > 0 && written < static_cast<int>(
                             sizeof(text)) - 16; ++f) {
                        written += std::snprintf(text + written,
                            sizeof(text) - written, " %.3f",
                            samples[i].values[f]);
                    }
                    log::Write("%s", text);
                }
                for (std::size_t i = 0; i < constantCount; ++i) {
                    log::Write("draw-capture: vs-constant[%zu] bytes=%u "
                        "usage=%u cpu=%u bind=%u maps=%llu",
                        i, reports[i].byteWidth, reports[i].usage,
                        reports[i].cpuAccessFlags, reports[i].bindFlags,
                        static_cast<unsigned long long>(reports[i].maps));
                }
            }
            // The draw arena has one consumer: the mirror. A diagnostic that
            // also drained it left every other frame with nothing to build
            // from, which showed up as the mirror falling back to its own
            // geometry on alternate frames.
            if (++frames % 600 == 0) {
                log::Write("draw-capture: frame=%u draws=%llu instanced=%llu "
                    "indices=%llu buffers=%u overflow=%llu largest=%u",
                    frames,
                    static_cast<unsigned long long>(summary.drawCalls),
                    static_cast<unsigned long long>(summary.instancedDrawCalls),
                    static_cast<unsigned long long>(summary.indices),
                    summary.distinctVertexBuffers,
                    static_cast<unsigned long long>(
                        summary.overflowedVertexBuffers),
                    summary.largestIndexCount);
            }
        }
        engine_camera_capture::OnPresent(
            s_backbufferWidth, s_backbufferHeight);
        renderer_observation::OnPresentBegin(sync, flags);
        // Only pay for depth capture while something consumes it.
        depth::SetWanted(post::g_ssao.enabled || post::g_debugView != 0);
        post::SetDepthSrv(depth::Acquire(s_device, s_context));
        post::Apply(swap, s_device, s_context);
        post::SetDepthSrv(nullptr);

        const auto& values = config::Get();
        if (values.sharpenEnabled || values.gradeEnabled ||
            values.lutEnabled || values.ssaoEnabled ||
            values.giEnabled || values.paniniEnabled ||
            post::g_debugView != 0) {
            renderer_observation::RecordWriter(
                renderer::trace::WriterClassification::Post,
                "visualforge/post");
        }

        // The mirror presents a Vulkan-rendered scene driven by the live
        // engine camera; the pattern only proves the transport. When the
        // mirror is requested but has no world camera yet, the frame stays
        // vanilla rather than falling back to a pattern that would look
        // like success.
        if (renderer_backend_probe::MirrorRequested()) {
            if (renderer_backend_probe::CompositeMirror(
                    swap, reinterpret_cast<unsigned long long>(
                        GetModuleHandleW(nullptr)))) {
                renderer_observation::RecordWriter(
                    renderer::trace::WriterClassification::Bridge,
                    "visualforge/vulkan-mirror");
            }
        } else if (renderer_backend_probe::CompositePattern(swap)) {
            renderer_observation::RecordWriter(
                renderer::trace::WriterClassification::Bridge,
                "visualforge/vulkan-bridge-pattern");
        }

        if (overlay::g_visible) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            overlay::Draw(s_device);
            ImGui::Render();
            s_context->OMSetRenderTargets(1, &s_backbufferRTV, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            renderer_observation::RecordWriter(
                renderer::trace::WriterClassification::Overlay,
                "visualforge/imgui");
        }
    }

    const auto result = s_origPresent(swap, sync, flags);
    if (swap == s_swapchain) {
        renderer_observation::OnPresentEnd(
            static_cast<std::int32_t>(result), sync, flags);
    }
    return result;
}

HRESULT WINAPI HookedResizeBuffers(IDXGISwapChain* swap, UINT count, UINT width, UINT height,
                                   DXGI_FORMAT format, UINT swapFlags)
{
    if (s_imguiReady && swap == s_swapchain) {
        renderer_backend_probe::BeforeResize();
        SafeRelease(s_backbufferRTV);
        post::OnResize();
        depth::OnResize();
        ImGui_ImplDX11_InvalidateDeviceObjects();
    }

    HRESULT hr = s_origResizeBuffers(swap, count, width, height, format, swapFlags);

    if (s_imguiReady && swap == s_swapchain && SUCCEEDED(hr)) {
        ImGui_ImplDX11_CreateDeviceObjects();
        if (!CreateBackbufferRTV())
            log::Write("hook: RTV re-creation after resize failed");
        DXGI_SWAP_CHAIN_DESC updated{};
        if (SUCCEEDED(swap->GetDesc(&updated))) {
            depth::SetTargetSize(
                updated.BufferDesc.Width, updated.BufferDesc.Height);
            renderer_observation::OnResize(
                updated.BufferDesc.Width,
                updated.BufferDesc.Height,
                static_cast<std::uint32_t>(updated.BufferDesc.Format),
                swapFlags);
            renderer_backend_probe::AfterResize(
                s_device,
                s_context,
                updated.BufferDesc.Width,
                updated.BufferDesc.Height,
                static_cast<unsigned>(updated.BufferDesc.Format));
            log::Write(
                "hook: swapchain resized to %ux%u",
                updated.BufferDesc.Width,
                updated.BufferDesc.Height);
        } else {
            depth::SetTargetSize(width, height);
            renderer_observation::OnResize(
                width,
                height,
                static_cast<std::uint32_t>(format),
                swapFlags);
            renderer_backend_probe::AfterResize(
                s_device,
                s_context,
                width,
                height,
                static_cast<unsigned>(format));
            log::Write("hook: swapchain resized to %ux%u", width, height);
        }
    }
    return hr;
}

} // namespace

bool Install()
{
    // Build a throwaway device + swapchain purely to read the vtable; the vtable is
    // shared per-class, so hooks installed on it catch the game's real swapchain.
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VisualForgeDummy";
    if (!RegisterClassExW(&wc)) {
        log::Write("hook: RegisterClassEx failed (%lu)", GetLastError());
        return false;
    }
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 2, 2,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        log::Write("hook: dummy window creation failed (%lu)", GetLastError());
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = 2;
    desc.BufferDesc.Height = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swap = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &desc, &swap, &device, &level, &context);
    if (FAILED(hr)) {
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        log::Write("hook: dummy device creation failed (%08X)", hr);
        return false;
    }

    void** vtbl = *reinterpret_cast<void***>(swap);
    void* presentAddr = vtbl[8];
    void* resizeAddr = vtbl[13];
    void** deviceVtable = *reinterpret_cast<void***>(device);
    void** contextVtable = *reinterpret_cast<void***>(context);
    static_cast<void>(engine_texture_capture::PrepareHooks(
        deviceVtable[5], deviceVtable[7], contextVtable[8],
        contextVtable[10]));
    // ID3D11DeviceContext slots: DrawIndexed 12, IASetVertexBuffers 18,
    // IASetIndexBuffer 19, DrawIndexedInstanced 20, IASetInputLayout 17.
    // ID3D11Device slot 11 is CreateInputLayout, which is the only moment the
    // element descriptions are visible: D3D11 offers no way to read them back
    // off an ID3D11InputLayout afterwards, and without them a draw knows only
    // its stride -- which cannot tell four halves from three floats.
    // Slot 16 is PSSetConstantBuffers, which carries the per-frame lighting
    // the sky publishes. The numbering is the same table the slots above come
    // from, so it is confirmed by every hook that already works.
    // ID3D11Device slot 15 is CreatePixelShader. Reading the buffers alone was
    // not enough to say what is in them -- the engine reuses one wide block
    // across techniques, so an offset means different things from draw to
    // draw. The compiled bytecode passed to this call names every constant
    // buffer and field, and this is the only moment it is readable.
    static_cast<void>(engine_draw_capture::PrepareHooks(
        contextVtable[12], contextVtable[20], contextVtable[18],
        contextVtable[19], contextVtable[7], contextVtable[14],
        contextVtable[15], contextVtable[17], deviceVtable[11],
        contextVtable[16], deviceVtable[15], contextVtable[48]));

    context->Release();
    device->Release();
    swap->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    // Installed here rather than at the first Present. CreateInputLayout only
    // reports the layouts created after it is hooked, and Fallout 4 builds its
    // vertex formats during renderer initialisation -- long before any frame
    // is presented. Hooking at first Present therefore misses every layout the
    // world actually draws with, and every mesh is then declined as
    // undecodable. The addresses come from the dummy device above, and every
    // D3D11 device on the machine shares that vtable.
    static_cast<void>(engine_draw_capture::Install());

    if (MH_CreateHook(presentAddr, reinterpret_cast<void*>(&HookedPresent),
                      reinterpret_cast<void**>(&s_origPresent)) != MH_OK ||
        MH_CreateHook(resizeAddr, reinterpret_cast<void*>(&HookedResizeBuffers),
                      reinterpret_cast<void**>(&s_origResizeBuffers)) != MH_OK) {
        log::Write("hook: MH_CreateHook failed");
        return false;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        log::Write("hook: MH_EnableHook failed");
        return false;
    }

    log::Write("hook: Present/ResizeBuffers hooked (Present=%p, ResizeBuffers=%p)",
               presentAddr, resizeAddr);
    return true;
}

}
