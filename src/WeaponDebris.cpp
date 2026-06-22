#include "WeaponDebris.h"
#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <MinHook.h>

#include <atomic>

namespace vf::debris {

namespace {

using CreateSRVFn = HRESULT(WINAPI*)(ID3D11Device*, ID3D11Resource*,
                                     const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                     ID3D11ShaderResourceView**);

CreateSRVFn s_origCreateSRV = nullptr;
ID3D11ShaderResourceView* s_fallbackSRV = nullptr;
std::atomic<int> s_failures{0};
bool s_installed = false;
bool s_loggedFirst = false;

// ID3D11Device vtable index of CreateShaderResourceView (after IUnknown 0-2, then
// CreateBuffer, CreateTexture1D/2D/3D at 3-6).
constexpr size_t kCreateSRVIndex = 7;

HRESULT WINAPI HookedCreateSRV(ID3D11Device* self, ID3D11Resource* resource,
                               const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
                               ID3D11ShaderResourceView** outView)
{
    HRESULT hr = s_origCreateSRV(self, resource, desc, outView);
    if (SUCCEEDED(hr))
        return hr; // the overwhelmingly common path — no added cost beyond one branch

    // Creation failed. Left alone, the debris system dereferences the null view and crashes.
    // Hand back a valid fallback so the game continues; the debris just samples 1x1 black.
    if (!s_loggedFirst) {
        s_loggedFirst = true;
        DXGI_FORMAT fmt = desc ? desc->Format : DXGI_FORMAT_UNKNOWN;
        D3D11_SRV_DIMENSION dim = desc ? desc->ViewDimension : D3D11_SRV_DIMENSION_UNKNOWN;
        log::Write("debris: intercepted failed CreateShaderResourceView (hr=%08X, format=%d, dim=%d) — "
                   "substituting fallback view",
                   hr, int(fmt), int(dim));
    }

    if (outView) {
        if (s_fallbackSRV) {
            s_fallbackSRV->AddRef(); // caller will Release its reference
            *outView = s_fallbackSRV;
        } else {
            *outView = nullptr;
            return hr; // no fallback available; preserve original failure
        }
    }
    s_failures.fetch_add(1, std::memory_order_relaxed);
    return S_OK;
}

bool CreateFallback(ID3D11Device* device)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 1;
    td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    const uint32_t black = 0;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = &black;
    init.SysMemPitch = sizeof(black);

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device->CreateTexture2D(&td, &init, &tex)) || !tex)
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = td.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    HRESULT hr = s_origCreateSRV(device, tex, &sd, &s_fallbackSRV);
    tex->Release();
    return SUCCEEDED(hr) && s_fallbackSRV;
}

} // namespace

bool Install(ID3D11Device* device)
{
    if (s_installed)
        return true;
    if (!device)
        return false;

    void** vtbl = *reinterpret_cast<void***>(device);
    void* target = vtbl[kCreateSRVIndex];

    // Resolve the original first so the fallback texture can be built through it.
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedCreateSRV),
                      reinterpret_cast<void**>(&s_origCreateSRV)) != MH_OK) {
        log::Write("debris: MH_CreateHook(CreateShaderResourceView) failed");
        return false;
    }

    if (!CreateFallback(device))
        log::Write("debris: fallback view creation failed — failures will preserve original error");

    if (MH_EnableHook(target) != MH_OK) {
        log::Write("debris: MH_EnableHook failed");
        return false;
    }

    s_installed = true;
    log::Write("debris: weapon-debris crash fix active (CreateShaderResourceView guarded)");
    return true;
}

void Shutdown()
{
    if (s_fallbackSRV) {
        s_fallbackSRV->Release();
        s_fallbackSRV = nullptr;
    }
}

int InterceptedFailures()
{
    return s_failures.load(std::memory_order_relaxed);
}

bool Installed()
{
    return s_installed;
}

}
