#pragma once

#include "renderer_core/EngineDrawStream.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace vf::engine_mesh_extractor {

// Geometry read back out of the engine's pooled vertex buffers. The pools are
// 128 MB each and persistent, so what is copied is the window a mesh actually
// touches, once, and then kept.
struct ExtractedMesh
{
    std::uint64_t identity{};
    std::uint32_t vertexStride{};
    std::uint64_t inputLayout{};
    // The pool-relative index of the first vertex copied. Indices below are
    // rebased against it, so the mesh is self-contained and can be replayed
    // without the pool it came from.
    std::uint32_t firstVertex{};
    std::uint32_t vertexCount{};
    std::vector<std::byte> vertices;
    std::vector<std::uint32_t> indices;
};

struct ExtractionStats
{
    std::uint32_t extracted{};
    // A request that could not be read: a released buffer, a copy that
    // failed, or a range that would read outside the pool. Counted rather
    // than retried forever, because a mesh that cannot be read once will not
    // read differently next frame.
    std::uint32_t failed{};
    std::uint64_t vertexBytes{};
    std::uint64_t indexBytes{};
    std::uint32_t cachedMeshes{};
    std::uint64_t cachedBytes{};
};

void Configure(ID3D11Device* device, ID3D11DeviceContext* context) noexcept;

// Reads the planned meshes. Runs at Present, never inside a draw hook: each
// read is a staging copy followed by a map, which synchronises the GPU, and
// doing that mid-frame would stall the engine's own submission.
[[nodiscard]] ExtractionStats Extract(
    std::span<const renderer::drawstream::MeshExtractionRequest> requests)
    noexcept;

// The identities already held, for the next frame's plan.
[[nodiscard]] std::vector<std::uint64_t> CachedIdentities();
[[nodiscard]] const ExtractedMesh* Find(std::uint64_t identity) noexcept;
[[nodiscard]] std::size_t CachedCount() noexcept;

}
