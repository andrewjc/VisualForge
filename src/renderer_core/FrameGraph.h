#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::raster {

enum class GraphResource : std::uint8_t
{
    VertexUpload,
    IndexUpload,
    MaterialUpload,
    HdrColor,
    Depth,
    ToneMapped,
    Readback
};

enum class GraphPass : std::uint8_t
{
    Upload,
    OpaqueRaster,
    ToneMap,
    Readback
};

enum class GraphAccess : std::uint8_t
{
    Read,
    Write
};

struct GraphUse
{
    GraphResource resource{};
    GraphAccess access{};
};

struct GraphPassDeclaration
{
    GraphPass pass{};
    std::vector<GraphUse> uses;
};

enum class GraphError : std::uint8_t
{
    None,
    Empty,
    DuplicatePass,
    ReadBeforeWrite,
    MissingReadback
};

struct FrameGraph
{
    std::vector<GraphPassDeclaration> passes;
};

[[nodiscard]] FrameGraph BuildPhase6FrameGraph();
[[nodiscard]] GraphError ValidateFrameGraph(const FrameGraph& graph) noexcept;
[[nodiscard]] std::uint32_t CountTransitions(
    const FrameGraph& graph) noexcept;
[[nodiscard]] const char* ToString(GraphError error) noexcept;

}
