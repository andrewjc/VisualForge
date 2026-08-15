#include "EngineTextureCapture.h"

#include "Config.h"
#include "Log.h"
#include "renderer_core/EngineTexture.h"
#include "renderer_host/D3D11TextureContract.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vf::engine_texture_capture {

namespace {

using namespace vf::renderer::texture;

constexpr std::size_t kMaximumCaptureBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaximumCandidateCount = 8;

using CreateTexture2DFn = HRESULT (STDMETHODCALLTYPE *)(
    ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
    const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using CreateShaderResourceViewFn = HRESULT (STDMETHODCALLTYPE *)(
    ID3D11Device*, ID3D11Resource*,
    const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
using PSSetShaderResourcesFn = void (STDMETHODCALLTYPE *)(
    ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using PSSetSamplersFn = void (STDMETHODCALLTYPE *)(
    ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);

struct Candidate
{
    void* identity{};
    CapturedTexture texture;
    bool viewReady{};
    std::size_t byteSize{};
};

struct BoundTexture
{
    void* identity{};
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    bool valid{};
};

struct CaptureState
{
    SRWLOCK lock = SRWLOCK_INIT;
    CreateTexture2DFn createTexture2D{};
    CreateShaderResourceViewFn createShaderResourceView{};
    PSSetShaderResourcesFn psSetShaderResources{};
    PSSetSamplersFn psSetSamplers{};
    std::vector<Candidate> candidates;
    std::size_t candidateBytes{};
    std::array<BoundTexture,
        D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> boundTextures{};
    std::array<std::optional<TextureSamplerDesc>,
        D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers{};
    std::wstring path;
    std::atomic<bool> captured{false};
    std::atomic<bool> candidateLogged{false};
    std::atomic<bool> viewLogged{false};
    std::atomic<bool> bindLogged{false};
    std::atomic<bool> samplerLogged{false};
    bool publishing{};
    bool enabled{};
};

CaptureState s_state;

class ExclusiveLock
{
public:
    explicit ExclusiveLock(SRWLOCK& lock) noexcept : lock_(lock)
    {
        AcquireSRWLockExclusive(&lock_);
    }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&lock_); }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;
private:
    SRWLOCK& lock_;
};

bool Requested() noexcept
{
    wchar_t value[16]{};
    const auto length = GetEnvironmentVariableW(
        L"VISUALFORGE_CAPTURE_TEXTURE_ONCE", value,
        static_cast<DWORD>(std::size(value)));
    if (length == 0 || length >= std::size(value)) return false;
    return _wcsicmp(value, L"1") == 0 ||
        _wcsicmp(value, L"true") == 0 ||
        _wcsicmp(value, L"yes") == 0;
}

std::wstring CapturePath()
{
    const auto required = GetEnvironmentVariableW(
        L"VISUALFORGE_CAPTURE_TEXTURE_PATH", nullptr, 0);
    if (required > 1) {
        std::wstring value(required, L'\0');
        const auto written = GetEnvironmentVariableW(
            L"VISUALFORGE_CAPTURE_TEXTURE_PATH", value.data(), required);
        if (written > 0 && written < required) {
            value.resize(written);
            return value;
        }
    }
    return std::wstring{vf::config::PluginDir()} +
        L"VisualForge-texture.vftex";
}

std::string Narrow(const std::wstring& value)
{
    if (value.empty()) return {};
    const auto required = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return "<path-conversion-failed>";
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), result.data(), required,
        nullptr, nullptr);
    return result;
}

bool SafeCopy(const void* source, void* destination,
    const std::size_t size) noexcept
{
    if (source == nullptr || destination == nullptr) return false;
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* Identity(ID3D11Resource* resource) noexcept
{
    if (resource == nullptr) return nullptr;
    IUnknown* identity{};
    if (FAILED(resource->QueryInterface(
            __uuidof(IUnknown), reinterpret_cast<void**>(&identity)))) {
        return nullptr;
    }
    auto* result = identity;
    identity->Release();
    return result;
}

TextureFormat DefaultTypedView(const TextureFormat format) noexcept
{
    switch (format) {
    case TextureFormat::BC1Typeless: return TextureFormat::BC1Unorm;
    case TextureFormat::BC2Typeless: return TextureFormat::BC2Unorm;
    case TextureFormat::BC3Typeless: return TextureFormat::BC3Unorm;
    case TextureFormat::R8G8B8A8Typeless:
        return TextureFormat::R8G8B8A8Unorm;
    default: return format;
    }
}

bool IsDiffuseCaptureFamily(const TextureFamily family) noexcept
{
    return family == TextureFamily::BC1 ||
        family == TextureFamily::BC2 ||
        family == TextureFamily::BC3;
}

std::uint64_t ResourceId(const void* identity) noexcept
{
    auto value = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(identity));
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    return 0x8000'0000'0000'0000ull |
        (value & 0x0FFF'FFFF'FFFF'FFFFull);
}

bool SnapshotCandidate(
    const D3D11_TEXTURE2D_DESC& description,
    const D3D11_SUBRESOURCE_DATA* initialData,
    Candidate& candidate) noexcept
{
    if (initialData == nullptr || description.Width < 64 ||
        description.Height < 64 || description.MipLevels == 0 ||
        description.ArraySize != 1 || description.SampleDesc.Count != 1 ||
        (description.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0) {
        return false;
    }
    const auto resourceFormat = static_cast<TextureFormat>(
        static_cast<std::uint32_t>(description.Format));
    const auto provisionalView = DefaultTypedView(resourceFormat);
    TextureFormatInfo formatInfo{};
    if (ResolveTextureFormat(resourceFormat, provisionalView, formatInfo) !=
            TexturePacketError::None ||
        !IsDiffuseCaptureFamily(formatInfo.family)) {
        return false;
    }
    try {
        CapturedTexture texture{};
        texture.generation = 1;
        texture.width = description.Width;
        texture.height = description.Height;
        texture.mipLevels = description.MipLevels;
        texture.resourceFormat = resourceFormat;
        texture.viewFormat = provisionalView;
        texture.residentMipCount = description.MipLevels;
        std::size_t totalBytes{};
        for (std::uint32_t mip = 0; mip < description.MipLevels; ++mip) {
            const auto width = std::max(1u, description.Width >> mip);
            const auto height = std::max(1u, description.Height >> mip);
            TextureFootprint footprint{};
            if (ComputeTextureFootprint(provisionalView,
                    width, height, footprint) != TexturePacketError::None ||
                footprint.byteSize > kMaximumCaptureBytes - totalBytes ||
                initialData[mip].pSysMem == nullptr ||
                initialData[mip].SysMemPitch < footprint.rowBytes) {
                return false;
            }
            TextureSubresource subresource{};
            subresource.mipLevel = mip;
            subresource.width = width;
            subresource.height = height;
            subresource.rowPitch = footprint.rowBytes;
            subresource.slicePitch =
                static_cast<std::uint32_t>(footprint.byteSize);
            subresource.bytes.resize(
                static_cast<std::size_t>(footprint.byteSize));
            for (std::uint32_t row = 0; row < footprint.rowCount; ++row) {
                const auto* source = static_cast<const std::byte*>(
                    initialData[mip].pSysMem) +
                    static_cast<std::size_t>(row) *
                        initialData[mip].SysMemPitch;
                auto* destination = subresource.bytes.data() +
                    static_cast<std::size_t>(row) * footprint.rowBytes;
                if (!SafeCopy(source, destination, footprint.rowBytes)) {
                    return false;
                }
            }
            totalBytes += subresource.bytes.size();
            texture.subresources.push_back(std::move(subresource));
        }
        candidate.texture = std::move(texture);
        candidate.byteSize = totalBytes;
        return true;
    } catch (...) {
        return false;
    }
}

Candidate* FindCandidate(const void* identity) noexcept
{
    const auto found = std::find_if(
        s_state.candidates.begin(), s_state.candidates.end(),
        [identity](const Candidate& candidate) {
            return candidate.identity == identity;
        });
    return found == s_state.candidates.end() ? nullptr : &*found;
}

TexturePacketError ResolveViewRange(
    const CapturedTexture& texture,
    const D3D11_SHADER_RESOURCE_VIEW_DESC& view,
    std::uint32_t& baseMip,
    std::uint32_t& mipCount) noexcept
{
    if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
        return TexturePacketError::InvalidDimension;
    }
    TextureFormatInfo info{};
    const auto viewFormat = static_cast<TextureFormat>(
        static_cast<std::uint32_t>(view.Format));
    const auto formatResult = ResolveTextureFormat(
        texture.resourceFormat, viewFormat, info);
    if (formatResult != TexturePacketError::None) return formatResult;
    baseMip = view.Texture2D.MostDetailedMip;
    const auto available = baseMip < texture.mipLevels
        ? texture.mipLevels - baseMip : 0u;
    mipCount = view.Texture2D.MipLevels ==
        std::numeric_limits<UINT>::max()
        ? available : view.Texture2D.MipLevels;
    if (mipCount == 0 || mipCount > available) {
        return TexturePacketError::InvalidMipRange;
    }
    return TexturePacketError::None;
}

TexturePacketError ApplyView(
    CapturedTexture& texture,
    const D3D11_SHADER_RESOURCE_VIEW_DESC& view)
{
    std::uint32_t baseMip{};
    std::uint32_t mipCount{};
    const auto rangeResult = ResolveViewRange(
        texture, view, baseMip, mipCount);
    if (rangeResult != TexturePacketError::None) return rangeResult;
    std::vector<TextureSubresource> selected;
    selected.reserve(mipCount);
    for (auto& subresource : texture.subresources) {
        if (subresource.mipLevel >= baseMip &&
            subresource.mipLevel < baseMip + mipCount) {
            selected.push_back(std::move(subresource));
        }
    }
    if (selected.size() != mipCount) {
        return TexturePacketError::InvalidSubresource;
    }
    texture.viewFormat = static_cast<TextureFormat>(
        static_cast<std::uint32_t>(view.Format));
    texture.residentBaseMip = baseMip;
    texture.residentMipCount = mipCount;
    texture.subresources = std::move(selected);
    return TexturePacketError::None;
}

void RemoveCandidate(const void* identity) noexcept
{
    const auto found = std::find_if(
        s_state.candidates.begin(), s_state.candidates.end(),
        [identity](const Candidate& candidate) {
            return candidate.identity == identity;
        });
    if (found == s_state.candidates.end()) return;
    s_state.candidateBytes -= found->byteSize;
    s_state.candidates.erase(found);
}

bool CanAcceptCandidate() noexcept
{
    ExclusiveLock guard{s_state.lock};
    return !s_state.captured.load(std::memory_order_relaxed) &&
        s_state.candidates.size() < kMaximumCandidateCount &&
        s_state.candidateBytes < kMaximumCaptureBytes;
}

bool WriteAll(const HANDLE file,
    const std::span<const std::byte> bytes) noexcept
{
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto count = std::min(bytes.size() - offset,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
        DWORD written{};
        if (!WriteFile(file, bytes.data() + offset,
                static_cast<DWORD>(count), &written, nullptr) ||
            written != count) return false;
        offset += written;
    }
    return true;
}

bool PublishBytes(const std::wstring& path,
    const std::span<const std::byte> bytes, DWORD& error) noexcept
{
    const auto temporary = path + L".tmp";
    const auto file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    const auto wrote = WriteAll(file, bytes);
    const auto flushed = wrote && FlushFileBuffers(file) != FALSE;
    const auto closed = CloseHandle(file) != FALSE;
    if (!wrote || !flushed || !closed) {
        error = GetLastError();
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = GetLastError();
        DeleteFileW(temporary.c_str());
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

void TryPublish() noexcept
{
    if (s_state.captured.load(std::memory_order_acquire)) return;
    Candidate candidate;
    BoundTexture binding;
    std::size_t selectedSlot{};
    try {
        {
            ExclusiveLock guard{s_state.lock};
            if (s_state.publishing) return;
            Candidate* selected{};
            for (std::size_t slot = 0;
                 slot < s_state.samplers.size(); ++slot) {
                if (!s_state.samplers[slot] ||
                    !s_state.boundTextures[slot].valid) continue;
                selected = FindCandidate(
                    s_state.boundTextures[slot].identity);
                if (selected == nullptr || !selected->viewReady) continue;
                selectedSlot = slot;
                binding = s_state.boundTextures[slot];
                candidate = *selected;
                candidate.texture.sampler = *s_state.samplers[slot];
                break;
            }
            if (selected == nullptr) return;
            s_state.publishing = true;
        }
        const auto viewResult = ApplyView(candidate.texture, binding.view);
        if (viewResult != TexturePacketError::None) {
            vf::log::Write(
                "renderer-texture-capture: bound view rejected slot=%llu "
                "reason=%s suppression=off",
                static_cast<unsigned long long>(selectedSlot),
                ToString(viewResult));
            ExclusiveLock guard{s_state.lock};
            s_state.boundTextures[selectedSlot] = {};
            s_state.publishing = false;
            return;
        }
        std::vector<std::byte> bytes;
        const auto encoded = EncodeCapturedTexture(candidate.texture, bytes);
        if (encoded != TexturePacketError::None) {
            vf::log::Write(
                "renderer-texture-capture: encode failed reason=%s suppression=off",
                ToString(encoded));
            ExclusiveLock guard{s_state.lock};
            s_state.publishing = false;
            RemoveCandidate(candidate.identity);
            return;
        }
        DWORD error{};
        if (!PublishBytes(s_state.path, bytes, error)) {
            vf::log::Write(
                "renderer-texture-capture: publication failed win32=%lu suppression=off",
                error);
            ExclusiveLock guard{s_state.lock};
            s_state.publishing = false;
            return;
        }
        s_state.captured.store(true, std::memory_order_release);
        const auto path = Narrow(s_state.path);
        vf::log::Write(
            "renderer-texture-capture: complete path=%s resource=%llu generation=%u "
            "extent=%ux%u mips=%u resident=%u+%u resource-format=%u "
            "view-format=%u subresources=%llu bytes=%llu anisotropy=%.3g "
            "ps-slot=%llu suppression=off",
            path.c_str(),
            static_cast<unsigned long long>(candidate.texture.resourceId),
            candidate.texture.generation, candidate.texture.width,
            candidate.texture.height, candidate.texture.mipLevels,
            candidate.texture.residentBaseMip,
            candidate.texture.residentMipCount,
            static_cast<unsigned>(candidate.texture.resourceFormat),
            static_cast<unsigned>(candidate.texture.viewFormat),
            static_cast<unsigned long long>(candidate.texture.subresources.size()),
            static_cast<unsigned long long>(bytes.size()),
            candidate.texture.sampler.maxAnisotropy,
            static_cast<unsigned long long>(selectedSlot));
        ExclusiveLock guard{s_state.lock};
        s_state.publishing = false;
        s_state.candidates.clear();
        s_state.candidateBytes = 0;
    } catch (...) {
        ExclusiveLock guard{s_state.lock};
        s_state.publishing = false;
        vf::log::Write(
            "renderer-texture-capture: publication failed reason=exception suppression=off");
    }
}

HRESULT STDMETHODCALLTYPE HookedCreateTexture2D(
    ID3D11Device* device,
    const D3D11_TEXTURE2D_DESC* description,
    const D3D11_SUBRESOURCE_DATA* initialData,
    ID3D11Texture2D** texture)
{
    Candidate snapshot;
    const auto wantsSnapshot = !s_state.captured.load(std::memory_order_acquire) &&
        CanAcceptCandidate() &&
        description != nullptr && texture != nullptr &&
        SnapshotCandidate(*description, initialData, snapshot);
    const auto result = s_state.createTexture2D(
        device, description, initialData, texture);
    if (wantsSnapshot && SUCCEEDED(result) && texture != nullptr &&
        *texture != nullptr) {
        snapshot.identity = Identity(*texture);
        snapshot.texture.resourceId = ResourceId(snapshot.identity);
        const auto acceptedWidth = snapshot.texture.width;
        const auto acceptedHeight = snapshot.texture.height;
        const auto acceptedMips = snapshot.texture.mipLevels;
        const auto acceptedFormat = snapshot.texture.resourceFormat;
        bool stored{};
        try {
            ExclusiveLock guard{s_state.lock};
            if (!s_state.captured.load(std::memory_order_relaxed) &&
                snapshot.identity != nullptr &&
                FindCandidate(snapshot.identity) == nullptr &&
                s_state.candidates.size() < kMaximumCandidateCount &&
                snapshot.byteSize <=
                    kMaximumCaptureBytes - s_state.candidateBytes) {
                s_state.candidateBytes += snapshot.byteSize;
                s_state.candidates.push_back(std::move(snapshot));
                stored = true;
            }
        } catch (...) {
        }
        if (stored && !s_state.candidateLogged.exchange(
                true, std::memory_order_relaxed)) {
            vf::log::Write(
                "renderer-texture-capture: candidate accepted extent=%ux%u "
                "mips=%u resource-format=%u suppression=off",
                acceptedWidth, acceptedHeight, acceptedMips,
                static_cast<unsigned>(acceptedFormat));
        }
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookedCreateShaderResourceView(
    ID3D11Device* device,
    ID3D11Resource* resource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* description,
    ID3D11ShaderResourceView** view)
{
    const auto result = s_state.createShaderResourceView(
        device, resource, description, view);
    if (SUCCEEDED(result) && view != nullptr && *view != nullptr &&
        !s_state.captured.load(std::memory_order_acquire)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC actual{};
        (*view)->GetDesc(&actual);
        const auto identity = Identity(resource);
        bool matched{};
        std::uint32_t baseMip{};
        std::uint32_t mipCount{};
        try {
            ExclusiveLock guard{s_state.lock};
            if (auto* candidate = FindCandidate(identity);
                candidate != nullptr &&
                ResolveViewRange(candidate->texture, actual,
                    baseMip, mipCount) == TexturePacketError::None) {
                candidate->viewReady = true;
                matched = true;
            }
        } catch (...) {
        }
        if (matched && !s_state.viewLogged.exchange(
                true, std::memory_order_relaxed)) {
            vf::log::Write(
                "renderer-texture-capture: view matched view-format=%u "
                "resident=%u+%u suppression=off",
                static_cast<unsigned>(actual.Format), baseMip, mipCount);
        }
    }
    return result;
}

void STDMETHODCALLTYPE HookedPSSetShaderResources(
    ID3D11DeviceContext* context,
    const UINT startSlot,
    const UINT viewCount,
    ID3D11ShaderResourceView* const* views)
{
    s_state.psSetShaderResources(context, startSlot, viewCount, views);
    if (s_state.captured.load(std::memory_order_acquire) || views == nullptr) {
        return;
    }
    for (UINT index = 0; index < viewCount; ++index) {
        const auto slot64 = static_cast<std::uint64_t>(startSlot) + index;
        if (slot64 >= s_state.boundTextures.size()) continue;
        const auto slot = static_cast<std::size_t>(slot64);
        BoundTexture bound{};
        if (views[index] != nullptr) {
            ID3D11Resource* resource{};
            views[index]->GetResource(&resource);
            if (resource != nullptr) {
                bound.identity = Identity(resource);
                resource->Release();
                views[index]->GetDesc(&bound.view);
            }
        }
        bool matched{};
        {
            ExclusiveLock guard{s_state.lock};
            if (auto* candidate = FindCandidate(bound.identity);
                candidate != nullptr) {
                std::uint32_t baseMip{};
                std::uint32_t mipCount{};
                if (ResolveViewRange(candidate->texture, bound.view,
                        baseMip, mipCount) == TexturePacketError::None) {
                    candidate->viewReady = true;
                    bound.valid = true;
                    matched = true;
                }
            }
            s_state.boundTextures[slot] = bound;
        }
        if (matched && !s_state.bindLogged.exchange(
                true, std::memory_order_relaxed)) {
            vf::log::Write(
                "renderer-texture-capture: texture bound ps-slot=%llu "
                "suppression=off",
                static_cast<unsigned long long>(slot));
        }
    }
    TryPublish();
}

void STDMETHODCALLTYPE HookedPSSetSamplers(
    ID3D11DeviceContext* context,
    const UINT startSlot,
    const UINT samplerCount,
    ID3D11SamplerState* const* samplers)
{
    s_state.psSetSamplers(context, startSlot, samplerCount, samplers);
    if (s_state.captured.load(std::memory_order_acquire) ||
        samplers == nullptr) return;
    for (UINT index = 0; index < samplerCount; ++index) {
        const auto slot64 = static_cast<std::uint64_t>(startSlot) + index;
        if (slot64 >= s_state.samplers.size()) continue;
        const auto slot = static_cast<std::size_t>(slot64);
        std::optional<TextureSamplerDesc> translated;
        if (samplers[index] != nullptr) {
            D3D11_SAMPLER_DESC source{};
            samplers[index]->GetDesc(&source);
            TextureSamplerDesc candidate{};
            if (TranslateD3D11Sampler(source, candidate) ==
                    TexturePacketError::None &&
                candidate.comparisonEnable == 0) {
                translated = candidate;
            }
        }
        {
            ExclusiveLock guard{s_state.lock};
            s_state.samplers[slot] = translated;
        }
        if (translated && !s_state.samplerLogged.exchange(
                true, std::memory_order_relaxed)) {
            vf::log::Write(
                "renderer-texture-capture: sampler bound ps-slot=%llu "
                "anisotropy=%.3g suppression=off",
                static_cast<unsigned long long>(slot),
                translated->maxAnisotropy);
        }
    }
    TryPublish();
}

}

bool PrepareHooks(
    void* const createTexture2D,
    void* const createShaderResourceView,
    void* const psSetShaderResources,
    void* const psSetSamplers) noexcept
{
    if (!Requested()) return true;
    if (createTexture2D == nullptr || createShaderResourceView == nullptr ||
        psSetShaderResources == nullptr || psSetSamplers == nullptr) {
        vf::log::Write(
            "renderer-texture-capture: hook targets unavailable suppression=off");
        return false;
    }
    s_state.path = CapturePath();
    const struct HookRequest { void* target; void* detour; void** original; } hooks[]{
        {createTexture2D, reinterpret_cast<void*>(&HookedCreateTexture2D),
            reinterpret_cast<void**>(&s_state.createTexture2D)},
        {createShaderResourceView,
            reinterpret_cast<void*>(&HookedCreateShaderResourceView),
            reinterpret_cast<void**>(&s_state.createShaderResourceView)},
        {psSetShaderResources,
            reinterpret_cast<void*>(&HookedPSSetShaderResources),
            reinterpret_cast<void**>(&s_state.psSetShaderResources)},
        {psSetSamplers, reinterpret_cast<void*>(&HookedPSSetSamplers),
            reinterpret_cast<void**>(&s_state.psSetSamplers)},
    };
    std::size_t created{};
    for (; created < std::size(hooks); ++created) {
        if (MH_CreateHook(hooks[created].target, hooks[created].detour,
                hooks[created].original) != MH_OK) break;
    }
    if (created != std::size(hooks)) {
        const auto failedStep = created;
        while (created != 0) {
            --created;
            static_cast<void>(MH_RemoveHook(hooks[created].target));
        }
        vf::log::Write(
            "renderer-texture-capture: hook transaction failed step=%llu suppression=off",
            static_cast<unsigned long long>(failedStep));
        return false;
    }
    s_state.enabled = true;
    const auto path = Narrow(s_state.path);
    vf::log::Write(
        "renderer-texture-capture: armed boundary=d3d11-create-view-bind hooks=4 "
        "path=%s suppression=off", path.c_str());
    return true;
}

bool Enabled() noexcept
{
    ExclusiveLock guard{s_state.lock};
    return s_state.enabled;
}

bool Arm(const wchar_t* const path) noexcept
{
    if (path == nullptr || *path == L'\0') return false;
    try {
        ExclusiveLock guard{s_state.lock};
        if (!s_state.enabled || s_state.publishing) return false;
        s_state.path.assign(path);
        s_state.captured.store(false, std::memory_order_release);
        s_state.candidates.clear();
        s_state.candidateBytes = 0;
        return true;
    } catch (...) {
        return false;
    }
}

}
