#include "DepthCapture.h"
#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <MinHook.h>

#include <cstdint>
#include <mutex>

namespace vf::depth {

namespace {

using OMSetRenderTargetsFn = void(WINAPI*)(ID3D11DeviceContext*, UINT,
                                           ID3D11RenderTargetView* const*,
                                           ID3D11DepthStencilView*);
using OMSetRTAndUAVFn = void(WINAPI*)(ID3D11DeviceContext*, UINT,
                                      ID3D11RenderTargetView* const*,
                                      ID3D11DepthStencilView*, UINT, UINT,
                                      ID3D11UnorderedAccessView* const*, const UINT*);
using ClearDSVFn = void(WINAPI*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT,
                                 UINT8);

// ID3D11DeviceContext vtable slots.
constexpr size_t kOMSetRenderTargets = 33;
constexpr size_t kOMSetRTAndUAV = 34;
constexpr size_t kClearDepthStencilView = 53;

OMSetRenderTargetsFn s_origOMSetRT = nullptr;
OMSetRTAndUAVFn s_origOMSetRTUAV = nullptr;
ClearDSVFn s_origClearDSV = nullptr;

ID3D11Device* s_device = nullptr;
// The engine clears the scene depth at the start of each frame; at that instant the buffer
// still holds the previous frame's completed depth, which is what we snapshot. One frame of
// latency is irrelevant for screen-space effects.
int s_clearsThisFrame = 0;
int s_clearsLastFrame = 0;
bool s_haveContent = false;
int s_mode = 0;              // default: snapshot before the clear (verified on 1.11.221)
bool s_loggedContextType = false;

std::mutex s_mutex;
ID3D11Texture2D* s_sceneDepth = nullptr;   // engine's texture (we hold a reference)
ID3D11DepthStencilView* s_lastDSV = nullptr; // cheap repeat-call filter, not dereferenced
UINT s_targetW = 0, s_targetH = 0;

ID3D11Texture2D* s_copy = nullptr;
ID3D11ShaderResourceView* s_copySRV = nullptr;
DXGI_FORMAT s_copyFormat = DXGI_FORMAT_UNKNOWN;
UINT s_copyW = 0, s_copyH = 0;

bool s_installed = false;
bool s_loggedFound = false;
bool s_loggedFormatIssue = false;

// One-shot content self-test: reads the captured depth back on the CPU and logs statistics,
// so the capture can be verified from the log without any UI interaction.
int s_framesCaptured = 0;
int s_selfTestsRun = 0;
bool s_everPassed = false;
bool s_selfTestEnabled = false;        // diagnostic only — allocates and stalls the GPU
bool s_wanted = false;                 // no consumer => do no work at all
ID3D11Texture2D* s_staging = nullptr;  // reused, never reallocated per test
constexpr int kSelfTestInterval = 600;
constexpr int kSelfTestCount = 6;      // enough to get past loading, then it stops

template <typename T>
void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Defined below; used by the clear hook. Caller must hold s_mutex.
bool EnsureCopy(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& src);

// Depth textures are created typeless when the engine also samples them. Returns the SRV
// format for a given texture format, or UNKNOWN when it cannot be sampled.
DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT texFormat)
{
    switch (texFormat) {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
            return DXGI_FORMAT_R16_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

// A typed depth format cannot back an SRV, so our copy must use the typeless equivalent.
DXGI_FORMAT TypelessEquivalent(DXGI_FORMAT texFormat)
{
    switch (texFormat) {
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
        default: return texFormat; // already typeless
    }
}

// The scene depth's COM identity. Only IUnknown is guaranteed identical across
// an object's interfaces, so a resource recovered from a view cannot be
// compared to a texture pointer directly -- the two need not share an address.
// This project has already paid for that lesson once, in the texture path.
//
// Not an owning reference: s_sceneDepth holds the object alive, and an object's
// IUnknown address is stable for its lifetime.
IUnknown* s_sceneDepthIdentity = nullptr;
// Bumped whenever the identity changes. A thread cannot reset another thread's
// cache, so the caches carry the generation they were computed under and
// recompute when it moves. Without it, a resize that hands the allocator's
// same view address back would keep a stale classification alive per thread.
std::uint64_t s_sceneDepthGeneration = 0;

// Cached per thread and keyed on the view, so a repeated bind of the same
// target costs a pointer compare rather than a QueryInterface. Draws outnumber
// binds by a wide margin and this sits in front of every one of them.
thread_local ID3D11DepthStencilView* t_classifiedDSV = nullptr;
thread_local std::uint64_t t_classifiedGeneration = 0;
thread_local bool t_sceneDepthBound = false;

IUnknown* CanonicalIdentity(IUnknown* value)
{
    if (!value) return nullptr;
    IUnknown* identity = nullptr;
    if (FAILED(value->QueryInterface(__uuidof(IUnknown),
            reinterpret_cast<void**>(&identity)))) {
        return nullptr;
    }
    // Released immediately: the caller keeps the object alive by other means
    // and only the address is wanted.
    identity->Release();
    return identity;
}

// Classifies the bind for the calling thread. Separate from NoteDepthTarget
// because that one filters repeated binds through a shared pointer and would
// skip exactly the calls this needs to see.
void ClassifyBoundDepth(ID3D11DepthStencilView* dsv)
{
    IUnknown* expected = nullptr;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        expected = s_sceneDepthIdentity;
        generation = s_sceneDepthGeneration;
    }
    if (dsv == t_classifiedDSV && generation == t_classifiedGeneration) return;
    t_classifiedDSV = dsv;
    t_classifiedGeneration = generation;
    t_sceneDepthBound = false;
    if (!dsv || !expected) return;

    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    if (!res) return;
    t_sceneDepthBound = CanonicalIdentity(res) == expected;
    res->Release();
}

// Called from the render thread on every depth-target bind. Must stay cheap.
void NoteDepthTarget(ID3D11DepthStencilView* dsv)
{
    if (!dsv || dsv == s_lastDSV)
        return;
    s_lastDSV = dsv;

    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    if (!res)
        return;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
        res->Release();
        return;
    }
    res->Release();

    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);

    // Only the full-resolution, single-sampled depth target is the main scene depth;
    // shadow maps and half-res buffers are a different size.
    const bool fullRes = s_targetW && desc.Width == s_targetW && desc.Height == s_targetH;
    if (!fullRes || desc.SampleDesc.Count != 1) {
        tex->Release();
        return;
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_sceneDepth != tex) {
        SafeRelease(s_sceneDepth);
        s_sceneDepth = tex; // keep the reference from QueryInterface
        s_sceneDepthIdentity = CanonicalIdentity(tex);
        ++s_sceneDepthGeneration;
        if (!s_loggedFound) {
            s_loggedFound = true;
            log::Write("depth: scene depth acquired (%ux%u, format=%d)", desc.Width, desc.Height,
                       int(desc.Format));
        }
    } else {
        tex->Release();
    }
}

void WINAPI HookedOMSetRenderTargets(ID3D11DeviceContext* self, UINT numViews,
                                     ID3D11RenderTargetView* const* rtvs,
                                     ID3D11DepthStencilView* dsv)
{
    NoteDepthTarget(dsv);
    ClassifyBoundDepth(dsv);
    s_origOMSetRT(self, numViews, rtvs, dsv);
}

void WINAPI HookedOMSetRTAndUAV(ID3D11DeviceContext* self, UINT numRTVs,
                                ID3D11RenderTargetView* const* rtvs,
                                ID3D11DepthStencilView* dsv, UINT uavStart, UINT numUAVs,
                                ID3D11UnorderedAccessView* const* uavs, const UINT* counts)
{
    NoteDepthTarget(dsv);
    ClassifyBoundDepth(dsv);
    s_origOMSetRTUAV(self, numRTVs, rtvs, dsv, uavStart, numUAVs, uavs, counts);
}

// Snapshot the depth immediately before the engine wipes it — that is the only moment it
// reliably holds a fully rendered scene. Only the first full-res depth clear of each frame
// is taken, which is the main scene depth rather than a later shadow/UI pass.
void WINAPI HookedClearDSV(ID3D11DeviceContext* self, ID3D11DepthStencilView* dsv, UINT flags,
                           FLOAT depthValue, UINT8 stencil)
{
    if (!s_loggedContextType) {
        s_loggedContextType = true;
        D3D11_DEVICE_CONTEXT_TYPE t = self->GetType();
        log::Write("depth: depth clears arrive on a %s context",
                   t == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "IMMEDIATE" : "DEFERRED");
    }

    // Snapshot on every qualifying clear, so the copy ends up holding the content of the
    // last clear before Present — by then the world has been rendered into it.
    if (s_wanted && s_mode == 0 && dsv && (flags & D3D11_CLEAR_DEPTH) && s_device) {
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        if (res) {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),
                                              reinterpret_cast<void**>(&tex)))) {
                D3D11_TEXTURE2D_DESC desc;
                tex->GetDesc(&desc);
                if (s_targetW && desc.Width == s_targetW && desc.Height == s_targetH &&
                    desc.SampleDesc.Count == 1) {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    if (EnsureCopy(s_device, desc)) {
                        self->CopyResource(s_copy, tex);
                        ++s_clearsThisFrame;
                        s_haveContent = true;
                    }
                }
                tex->Release();
            }
            res->Release();
        }
    }
    s_origClearDSV(self, dsv, flags, depthValue, stencil);
}

bool EnsureCopy(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& src)
{
    if (s_copy && s_copyW == src.Width && s_copyH == src.Height && s_copyFormat == src.Format)
        return true;

    SafeRelease(s_copySRV);
    SafeRelease(s_copy);

    D3D11_TEXTURE2D_DESC cd = src;
    cd.Format = TypelessEquivalent(src.Format);
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cd.CPUAccessFlags = 0;
    cd.MiscFlags = 0;

    DXGI_FORMAT srvFormat = DepthSrvFormat(cd.Format);
    if (srvFormat == DXGI_FORMAT_UNKNOWN) {
        if (!s_loggedFormatIssue) {
            s_loggedFormatIssue = true;
            log::Write("depth: unsupported depth format %d — capture disabled", int(src.Format));
        }
        return false;
    }

    if (FAILED(device->CreateTexture2D(&cd, nullptr, &s_copy))) {
        if (!s_loggedFormatIssue) {
            s_loggedFormatIssue = true;
            log::Write("depth: copy texture creation failed (format=%d)", int(cd.Format));
        }
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = srvFormat;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(s_copy, &sd, &s_copySRV))) {
        SafeRelease(s_copy);
        log::Write("depth: SRV creation failed (srvFormat=%d)", int(srvFormat));
        return false;
    }

    s_copyW = src.Width;
    s_copyH = src.Height;
    s_copyFormat = src.Format;
    log::Write("depth: capture buffer ready (%ux%u, srvFormat=%d)", s_copyW, s_copyH, int(srvFormat));
    return true;
}

// Reads the captured depth back once and logs a 5x5 sample grid plus min/max. Real scene
// depth varies across the frame; a constant value would mean we grabbed the wrong target.
void RunSelfTest(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
    ++s_selfTestsRun;

    D3D11_TEXTURE2D_DESC desc;
    s_copy->GetDesc(&desc);

    const bool is24_8 = desc.Format == DXGI_FORMAT_R24G8_TYPELESS;
    const bool is32f = desc.Format == DXGI_FORMAT_R32_TYPELESS;
    if (!is24_8 && !is32f) {
        log::Write("depth: self-test skipped (format %d not handled by the readback)",
                   int(desc.Format));
        return;
    }

    // One staging texture is created once and reused; allocating a full-resolution one per
    // test churned tens of megabytes repeatedly during play.
    if (!s_staging) {
        D3D11_TEXTURE2D_DESC sd = desc;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&sd, nullptr, &s_staging)) || !s_staging) {
            log::Write("depth: self-test staging texture creation failed — disabling self-test");
            s_selfTestEnabled = false;
            return;
        }
    }

    ctx->CopyResource(s_staging, s_copy);

    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(s_staging, 0, D3D11_MAP_READ, 0, &m))) {
        log::Write("depth: self-test map failed");
        return;
    }

    float mn = 1e9f, mx = -1e9f;
    char grid[256];
    int used = 0;
    for (int gy = 0; gy < 5; ++gy) {
        for (int gx = 0; gx < 5; ++gx) {
            UINT x = UINT((desc.Width - 1) * gx / 4);
            UINT y = UINT((desc.Height - 1) * gy / 4);
            auto row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
            float d;
            if (is24_8) {
                uint32_t v = *reinterpret_cast<const uint32_t*>(row + size_t(x) * 4);
                d = float(v & 0x00FFFFFFu) / float(0x00FFFFFFu);
            } else {
                d = *reinterpret_cast<const float*>(row + size_t(x) * 4);
            }
            if (d < mn) mn = d;
            if (d > mx) mx = d;
            if (gy == 2 && used < int(sizeof(grid)) - 16)
                used += snprintf(grid + used, sizeof(grid) - used, "%.5f ", d);
        }
    }
    ctx->Unmap(s_staging, 0);

    log::Write("depth: SELF-TEST #%d min=%.6f max=%.6f spread=%.6f (clears/frame=%d)",
               s_selfTestsRun, mn, mx, mx - mn, s_clearsLastFrame);
    log::Write("depth: SELF-TEST #%d middle row: %s", s_selfTestsRun, grid);
    if (mx - mn > 1e-6f) {
        if (!s_everPassed) {
            s_everPassed = true;
            log::Write("depth: *** FIRST PASS *** real scene depth captured (mode %d)", s_mode);
        }
        log::Write("depth: SELF-TEST #%d PASS — depth varies across the frame", s_selfTestsRun);
    } else {
        log::Write("depth: SELF-TEST #%d FAIL — constant depth (menu/loading screens are "
                   "expected to look like this)", s_selfTestsRun);
    }
}

} // namespace

bool Install(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
    if (s_installed)
        return true;
    if (!device || !ctx)
        return false;

    void** vtbl = *reinterpret_cast<void***>(ctx);

    if (MH_CreateHook(vtbl[kOMSetRenderTargets],
                      reinterpret_cast<void*>(&HookedOMSetRenderTargets),
                      reinterpret_cast<void**>(&s_origOMSetRT)) != MH_OK) {
        log::Write("depth: hook OMSetRenderTargets failed");
        return false;
    }
    if (MH_CreateHook(vtbl[kOMSetRTAndUAV],
                      reinterpret_cast<void*>(&HookedOMSetRTAndUAV),
                      reinterpret_cast<void**>(&s_origOMSetRTUAV)) != MH_OK) {
        log::Write("depth: hook OMSetRenderTargetsAndUnorderedAccessViews failed");
        return false;
    }
    if (MH_CreateHook(vtbl[kClearDepthStencilView],
                      reinterpret_cast<void*>(&HookedClearDSV),
                      reinterpret_cast<void**>(&s_origClearDSV)) != MH_OK) {
        log::Write("depth: hook ClearDepthStencilView failed");
        return false;
    }
    if (MH_EnableHook(vtbl[kOMSetRenderTargets]) != MH_OK ||
        MH_EnableHook(vtbl[kOMSetRTAndUAV]) != MH_OK ||
        MH_EnableHook(vtbl[kClearDepthStencilView]) != MH_OK) {
        log::Write("depth: enabling depth-target hooks failed");
        return false;
    }

    s_device = device;
    s_installed = true;
    log::Write("depth: depth-target tracking installed");
    return true;
}

void SetTargetSize(UINT width, UINT height)
{
    s_targetW = width;
    s_targetH = height;
}

void SetCaptureMode(int mode)
{
    s_mode = (mode == 0) ? 0 : 1;
    log::Write("depth: capture mode %d (%s)", s_mode,
               s_mode == 0 ? "snapshot before clear" : "copy at Present");
}

int CaptureMode()
{
    return s_mode;
}

void SetWanted(bool wanted)
{
    if (s_wanted == wanted)
        return;
    s_wanted = wanted;
    if (!wanted) {
        // Release the copy so an idle plugin holds no render-target-sized resources.
        std::lock_guard<std::mutex> lock(s_mutex);
        SafeRelease(s_copySRV);
        SafeRelease(s_copy);
        SafeRelease(s_staging);
        s_copyW = s_copyH = 0;
        s_copyFormat = DXGI_FORMAT_UNKNOWN;
        s_haveContent = false;
    }
    log::Write("depth: capture %s", wanted ? "enabled (a consumer needs it)" : "idle (no consumer)");
}

void SetSelfTestEnabled(bool enabled)
{
    s_selfTestEnabled = enabled;
}

ID3D11ShaderResourceView* Acquire(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
    if (!s_installed || !device || !ctx || !s_wanted)
        return nullptr;

    std::lock_guard<std::mutex> lock(s_mutex);

    s_clearsLastFrame = s_clearsThisFrame;
    s_clearsThisFrame = 0;

    // Mode 1: copy here, on the immediate context. At Present the scene depth still holds
    // this frame's rendered content — the clear for the next frame has not happened yet.
    if (s_mode == 1 && s_sceneDepth) {
        D3D11_TEXTURE2D_DESC desc;
        s_sceneDepth->GetDesc(&desc);
        if (EnsureCopy(device, desc)) {
            ctx->CopyResource(s_copy, s_sceneDepth);
            s_haveContent = true;
        }
    }

    if (!s_haveContent || !s_copySRV)
        return nullptr;

    ++s_framesCaptured;
    if (s_selfTestEnabled && s_selfTestsRun < kSelfTestCount &&
        s_framesCaptured >= kSelfTestInterval * (s_selfTestsRun + 1))
        RunSelfTest(device, ctx);

    return s_copySRV;
}

void OnResize()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    SafeRelease(s_copySRV);
    SafeRelease(s_copy);
    SafeRelease(s_staging);
    SafeRelease(s_sceneDepth);
    // Cleared with the texture it names. A stale identity would classify the
    // next frame's world draws against an object that no longer exists, and on
    // a resize the allocator is free to hand the same address back.
    s_sceneDepthIdentity = nullptr;
    ++s_sceneDepthGeneration;
    s_lastDSV = nullptr;
    s_copyW = s_copyH = 0;
    s_copyFormat = DXGI_FORMAT_UNKNOWN;
    s_haveContent = false;
    s_clearsThisFrame = 0;
}

void Shutdown()
{
    OnResize();
}

bool SceneDepthBound()
{
    return t_sceneDepthBound;
}

bool Installed()
{
    return s_installed;
}

bool HaveDepth()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_haveContent;
}

DXGI_FORMAT CapturedFormat()
{
    return s_copyFormat;
}

}
