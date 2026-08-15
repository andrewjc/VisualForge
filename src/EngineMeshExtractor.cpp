#include "EngineMeshExtractor.h"

#include "Log.h"

#include <windows.h>
#include <d3d11.h>

#include <cstring>
#include <limits>
#include <new>
#include <unordered_map>

namespace vf::engine_mesh_extractor {

namespace {

ID3D11Device* s_device = nullptr;
ID3D11DeviceContext* s_context = nullptr;
ID3D11Buffer* s_staging = nullptr;
std::uint32_t s_stagingBytes = 0;

std::unordered_map<std::uint64_t, ExtractedMesh> s_cache;
std::uint64_t s_cachedBytes = 0;

// DXGI_FORMAT_R16_UINT and R32_UINT. Stored raw by the hook so the reader is
// not guessing which width the pooled indices are; a wrong guess reads every
// index at the wrong stride and produces geometry that is not merely
// misplaced but meaningless.
constexpr std::uint32_t kFormatR16Uint = 57;
constexpr std::uint32_t kFormatR32Uint = 42;

[[nodiscard]] bool EnsureStaging(const std::uint32_t bytes) noexcept
{
    if (s_staging != nullptr && s_stagingBytes >= bytes) return true;
    if (s_device == nullptr || bytes == 0) return false;
    if (s_staging != nullptr) {
        s_staging->Release();
        s_staging = nullptr;
        s_stagingBytes = 0;
    }
    D3D11_BUFFER_DESC desc{};
    // Rounded up so a slightly larger mesh does not recreate the buffer.
    desc.ByteWidth = (bytes + 0xFFFFu) & ~0xFFFFu;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(s_device->CreateBuffer(&desc, nullptr, &s_staging))) {
        s_staging = nullptr;
        return false;
    }
    s_stagingBytes = desc.ByteWidth;
    return true;
}

// Copies a byte range out of a pooled buffer and hands back the mapped
// staging memory. The caller must call Unmap.
[[nodiscard]] bool ReadRange(
    ID3D11Buffer* const source,
    const std::uint32_t offset,
    const std::uint32_t bytes,
    D3D11_MAPPED_SUBRESOURCE& mapped) noexcept
{
    if (source == nullptr || bytes == 0) return false;
    if (!EnsureStaging(bytes)) return false;
    D3D11_BUFFER_DESC desc{};
    source->GetDesc(&desc);
    // The range has to be inside the pool. A copy that runs off the end is a
    // device removal, not a smaller mesh.
    if (static_cast<std::uint64_t>(offset) + bytes > desc.ByteWidth) {
        return false;
    }
    D3D11_BOX box{};
    box.left = offset;
    box.right = offset + bytes;
    box.top = 0;
    box.bottom = 1;
    box.front = 0;
    box.back = 1;
    s_context->CopySubresourceRegion(s_staging, 0, 0, 0, 0, source, 0, &box);
    return SUCCEEDED(s_context->Map(s_staging, 0, D3D11_MAP_READ, 0, &mapped));
}

}

void Configure(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context) noexcept
{
    s_device = device;
    s_context = context;
}

ExtractionStats Extract(
    const std::span<const renderer::drawstream::MeshExtractionRequest>
        requests) noexcept
{
    ExtractionStats stats{};
    if (s_device == nullptr || s_context == nullptr) return stats;

    for (const auto& request : requests) {
        if (s_cache.find(request.meshIdentity) != s_cache.end()) continue;

        const auto indexWidth =
            request.indexFormat == kFormatR16Uint ? 2u :
            request.indexFormat == kFormatR32Uint ? 4u : 0u;
        if (indexWidth == 0 || request.vertexStride == 0) {
            ++stats.failed;
            continue;
        }

        auto* const indexBuffer =
            reinterpret_cast<ID3D11Buffer*>(request.indexBuffer);
        auto* const vertexBuffer =
            reinterpret_cast<ID3D11Buffer*>(request.vertexBuffer);

        std::vector<std::uint32_t> indices;
        {
            const auto indexBytes = request.indexCount * indexWidth;
            const auto indexOffset =
                request.indexOffset + request.firstIndex * indexWidth;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (!ReadRange(indexBuffer, indexOffset, indexBytes, mapped)) {
                ++stats.failed;
                continue;
            }
            try {
                indices.resize(request.indexCount);
            } catch (const std::bad_alloc&) {
                s_context->Unmap(s_staging, 0);
                ++stats.failed;
                continue;
            }
            // Widened here rather than kept at source width, so everything
            // downstream reads one type and cannot mistake a 16-bit pool for
            // a 32-bit one.
            if (indexWidth == 2) {
                const auto* const source =
                    static_cast<const std::uint16_t*>(mapped.pData);
                for (std::uint32_t i = 0; i < request.indexCount; ++i) {
                    indices[i] = source[i];
                }
            } else {
                std::memcpy(indices.data(), mapped.pData, indexBytes);
            }
            s_context->Unmap(s_staging, 0);
            stats.indexBytes += indexBytes;
        }

        std::uint32_t firstVertex = 0;
        std::uint32_t vertexCount = 0;
        if (!renderer::drawstream::VertexRangeForIndices(
                indices, request.baseVertex, firstVertex, vertexCount)) {
            ++stats.failed;
            continue;
        }

        ExtractedMesh mesh{};
        mesh.identity = request.meshIdentity;
        mesh.vertexStride = request.vertexStride;
        mesh.inputLayout = request.inputLayout;
        mesh.firstVertex = firstVertex;
        mesh.vertexCount = vertexCount;
        {
            const auto vertexBytes =
                static_cast<std::uint64_t>(vertexCount) * request.vertexStride;
            if (vertexBytes == 0 ||
                vertexBytes > std::numeric_limits<std::uint32_t>::max()) {
                ++stats.failed;
                continue;
            }
            // The mesh's address inside the pool is the bound byte offset plus
            // the vertex range, not the range alone. Fallout 4 draws every
            // mesh out of one of two 128 MB pools, so reading from the pool's
            // base returns whatever geometry happens to live there: real
            // triangles, belonging to a different object somewhere else in
            // the cell. A mesh that happened to sit near offset zero read
            // correctly, which is what made the omission look verified.
            const auto vertexBase =
                static_cast<std::uint64_t>(request.vertexByteOffset) +
                static_cast<std::uint64_t>(firstVertex) * request.vertexStride;
            if (vertexBase + vertexBytes >
                std::numeric_limits<std::uint32_t>::max()) {
                ++stats.failed;
                continue;
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (!ReadRange(vertexBuffer,
                    static_cast<std::uint32_t>(vertexBase),
                    static_cast<std::uint32_t>(vertexBytes), mapped)) {
                ++stats.failed;
                continue;
            }
            try {
                mesh.vertices.resize(static_cast<std::size_t>(vertexBytes));
            } catch (const std::bad_alloc&) {
                s_context->Unmap(s_staging, 0);
                ++stats.failed;
                continue;
            }
            std::memcpy(mesh.vertices.data(), mapped.pData,
                static_cast<std::size_t>(vertexBytes));
            s_context->Unmap(s_staging, 0);
            stats.vertexBytes += vertexBytes;
        }

        // Rebased against the copied window, so the mesh is self-contained
        // and can be replayed without the 128 MB pool it came from.
        mesh.indices = std::move(indices);
        for (auto& index : mesh.indices) {
            index = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(index) + request.baseVertex -
                firstVertex);
        }

        try {
            s_cachedBytes += mesh.vertices.size() +
                mesh.indices.size() * sizeof(std::uint32_t);
            s_cache.emplace(request.meshIdentity, std::move(mesh));
        } catch (const std::bad_alloc&) {
            ++stats.failed;
            continue;
        }
        ++stats.extracted;
    }

    stats.cachedMeshes = static_cast<std::uint32_t>(s_cache.size());
    stats.cachedBytes = s_cachedBytes;
    return stats;
}

std::vector<std::uint64_t> CachedIdentities()
{
    std::vector<std::uint64_t> identities;
    identities.reserve(s_cache.size());
    for (const auto& entry : s_cache) identities.push_back(entry.first);
    return identities;
}

const ExtractedMesh* Find(const std::uint64_t identity) noexcept
{
    const auto entry = s_cache.find(identity);
    return entry != s_cache.end() ? &entry->second : nullptr;
}

std::size_t CachedCount() noexcept
{
    return s_cache.size();
}

}
