#include "EngineTextureResidency.h"

#include "Log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace vf::engine_texture_residency {

using namespace vf::renderer::texture;

namespace {

// One texture's worth. Fallout 4's diffuse maps are block-compressed, so a
// 2048x2048 BC3 with mips is about 5.6 MB; this admits those and refuses the
// pathological.
constexpr std::size_t kMaximumTextureBytes = 32ull * 1024ull * 1024ull;

// The whole cache. Chosen so a dense cell's material set fits while the plugin
// stays a guest in the engine's address space -- Fallout 4 is a 64-bit process
// but shares it with the game's own streaming budget, and a renderer mirror
// that pushes the engine into swapping has changed the thing it measures.
constexpr std::uint64_t kResidencyBudgetBytes = 512ull * 1024ull * 1024ull;

std::mutex s_mutex;
std::unordered_map<std::uint64_t, CapturedTexture> s_resident;
std::uint64_t s_residentBytes = 0;
std::atomic<std::uint32_t> s_rejected{};
std::atomic<std::uint32_t> s_budgetDropped{};
std::atomic<std::uint32_t> s_unreadable{};
std::atomic<bool> s_budgetLogged{};

// Typeless formats carry no interpretation of their own; the engine creates
// them and supplies the meaning through the view. These are the mappings the
// one-shot capture already established for the same textures.
[[nodiscard]] TextureFormat DefaultTypedView(const TextureFormat format) noexcept
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

// Block-compressed colour only. A material's base colour is always one of
// these in this engine; admitting the rest would fill the budget with render
// targets, depth surfaces and scratch buffers that no material samples.
[[nodiscard]] bool IsDiffuseCaptureFamily(const TextureFamily family) noexcept
{
    return family == TextureFamily::BC1 ||
        family == TextureFamily::BC2 ||
        family == TextureFamily::BC3;
}

// Through IUnknown, exactly as the draw path does. COM guarantees only that
// IUnknown is identical across a given object's interfaces, so a texture
// reached as ID3D11Texture2D here and as ID3D11Resource at bind time must be
// reduced to the same canonical pointer or the two tables never agree.
[[nodiscard]] std::uint64_t CanonicalIdentity(ID3D11Texture2D* texture) noexcept
{
    if (texture == nullptr) return 0;
    IUnknown* canonical = nullptr;
    if (FAILED(texture->QueryInterface(
            __uuidof(IUnknown), reinterpret_cast<void**>(&canonical))) ||
        canonical == nullptr) {
        return 0;
    }
    const auto identity =
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(canonical));
    canonical->Release();
    return identity;
}

}

void NoteCreatedTexture(
    const D3D11_TEXTURE2D_DESC* const description,
    const D3D11_SUBRESOURCE_DATA* const initialData,
    ID3D11Texture2D* const texture) noexcept
{
    if (description == nullptr || texture == nullptr) return;
    // No initial data means the engine will fill this later from the GPU
    // side -- a render target or a streamed surface. Those are not refused as
    // an error; they are simply not material uploads.
    if (initialData == nullptr) {
        s_rejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (description->Width < 64 || description->Height < 64 ||
        description->MipLevels == 0 || description->ArraySize != 1 ||
        description->SampleDesc.Count != 1 ||
        (description->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0) {
        s_rejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const auto resourceFormat = static_cast<TextureFormat>(
        static_cast<std::uint32_t>(description->Format));
    const auto viewFormat = DefaultTypedView(resourceFormat);
    TextureFormatInfo formatInfo{};
    if (ResolveTextureFormat(resourceFormat, viewFormat, formatInfo) !=
            TexturePacketError::None ||
        !IsDiffuseCaptureFamily(formatInfo.family)) {
        s_rejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const auto identity = CanonicalIdentity(texture);
    if (identity == 0) {
        s_unreadable.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    try {
        CapturedTexture captured{};
        captured.resourceId = identity;
        captured.generation = 1;
        captured.width = description->Width;
        captured.height = description->Height;
        captured.mipLevels = description->MipLevels;
        captured.resourceFormat = resourceFormat;
        captured.viewFormat = viewFormat;
        captured.residentBaseMip = 0;
        captured.residentMipCount = description->MipLevels;

        std::size_t totalBytes = 0;
        for (std::uint32_t mip = 0; mip < description->MipLevels; ++mip) {
            const auto width = std::max(1u, description->Width >> mip);
            const auto height = std::max(1u, description->Height >> mip);
            TextureFootprint footprint{};
            if (ComputeTextureFootprint(viewFormat, width, height, footprint) !=
                    TexturePacketError::None ||
                initialData[mip].pSysMem == nullptr ||
                initialData[mip].SysMemPitch < footprint.rowBytes ||
                footprint.byteSize > kMaximumTextureBytes - totalBytes) {
                s_unreadable.fetch_add(1, std::memory_order_relaxed);
                return;
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
            // Row by row, because the engine's upload pitch is its own and is
            // routinely wider than the format's packed rows.
            for (std::uint32_t row = 0; row < footprint.rowCount; ++row) {
                const auto* const source =
                    static_cast<const std::byte*>(initialData[mip].pSysMem) +
                    static_cast<std::size_t>(row) * initialData[mip].SysMemPitch;
                auto* const destination = subresource.bytes.data() +
                    static_cast<std::size_t>(row) * footprint.rowBytes;
                std::memcpy(destination, source, footprint.rowBytes);
            }
            totalBytes += subresource.bytes.size();
            captured.subresources.push_back(std::move(subresource));
        }

        const std::lock_guard<std::mutex> guard{s_mutex};
        if (s_resident.find(identity) != s_resident.end()) return;
        if (s_residentBytes + totalBytes > kResidencyBudgetBytes) {
            s_budgetDropped.fetch_add(1, std::memory_order_relaxed);
            if (!s_budgetLogged.exchange(true, std::memory_order_relaxed)) {
                vf::log::Write(
                    "renderer-texture-residency: budget reached resident=%llu "
                    "bytes=%llu",
                    static_cast<unsigned long long>(s_resident.size()),
                    static_cast<unsigned long long>(s_residentBytes));
            }
            return;
        }
        s_residentBytes += totalBytes;
        s_resident.emplace(identity, std::move(captured));
    } catch (...) {
        s_unreadable.fetch_add(1, std::memory_order_relaxed);
    }
}

const CapturedTexture* Find(const std::uint64_t identity) noexcept
{
    if (identity == 0) return nullptr;
    const std::lock_guard<std::mutex> guard{s_mutex};
    const auto found = s_resident.find(identity);
    // The map is never erased from, so the pointer stays valid after the lock
    // is dropped: unordered_map does not invalidate references to existing
    // elements on insert.
    return found == s_resident.end() ? nullptr : &found->second;
}

ResidencyStats Counters() noexcept
{
    ResidencyStats stats{};
    stats.rejected = s_rejected.load(std::memory_order_relaxed);
    stats.budgetDropped = s_budgetDropped.load(std::memory_order_relaxed);
    stats.unreadable = s_unreadable.load(std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> guard{s_mutex};
        stats.resident = static_cast<std::uint32_t>(s_resident.size());
        stats.residentBytes = s_residentBytes;
    }
    return stats;
}

}
