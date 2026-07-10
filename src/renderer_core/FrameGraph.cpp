#include "renderer_core/FrameGraph.h"

#include <array>

namespace vf::renderer::raster {

FrameGraph BuildPhase6FrameGraph()
{
    return FrameGraph{{
        {GraphPass::Upload, {
            {GraphResource::VertexUpload, GraphAccess::Write},
            {GraphResource::IndexUpload, GraphAccess::Write},
            {GraphResource::MaterialUpload, GraphAccess::Write},
        }},
        {GraphPass::OpaqueRaster, {
            {GraphResource::VertexUpload, GraphAccess::Read},
            {GraphResource::IndexUpload, GraphAccess::Read},
            {GraphResource::MaterialUpload, GraphAccess::Read},
            {GraphResource::HdrColor, GraphAccess::Write},
            {GraphResource::Depth, GraphAccess::Write},
        }},
        {GraphPass::ToneMap, {
            {GraphResource::HdrColor, GraphAccess::Read},
            {GraphResource::ToneMapped, GraphAccess::Write},
        }},
        {GraphPass::Readback, {
            {GraphResource::ToneMapped, GraphAccess::Read},
            {GraphResource::Readback, GraphAccess::Write},
        }},
    }};
}

GraphError ValidateFrameGraph(const FrameGraph& graph) noexcept
{
    if (graph.passes.empty()) {
        return GraphError::Empty;
    }
    constexpr auto passCount = static_cast<std::size_t>(GraphPass::Readback) + 1;
    constexpr auto resourceCount =
        static_cast<std::size_t>(GraphResource::Readback) + 1;
    std::array<bool, passCount> seenPass{};
    std::array<bool, resourceCount> written{};
    for (const auto& declaration : graph.passes) {
        const auto passIndex = static_cast<std::size_t>(declaration.pass);
        if (passIndex >= seenPass.size() || seenPass[passIndex]) {
            return GraphError::DuplicatePass;
        }
        seenPass[passIndex] = true;
        for (const auto& use : declaration.uses) {
            const auto resourceIndex = static_cast<std::size_t>(use.resource);
            if (resourceIndex >= written.size()) {
                return GraphError::ReadBeforeWrite;
            }
            if (use.access == GraphAccess::Read && !written[resourceIndex]) {
                return GraphError::ReadBeforeWrite;
            }
            if (use.access == GraphAccess::Write) {
                written[resourceIndex] = true;
            }
        }
    }
    if (!written[static_cast<std::size_t>(GraphResource::Readback)]) {
        return GraphError::MissingReadback;
    }
    return GraphError::None;
}

std::uint32_t CountTransitions(const FrameGraph& graph) noexcept
{
    constexpr auto resourceCount =
        static_cast<std::size_t>(GraphResource::Readback) + 1;
    std::array<bool, resourceCount> used{};
    std::array<GraphAccess, resourceCount> previous{};
    std::uint32_t transitions{};
    for (const auto& declaration : graph.passes) {
        for (const auto& use : declaration.uses) {
            const auto resourceIndex = static_cast<std::size_t>(use.resource);
            if (resourceIndex >= used.size()) {
                continue;
            }
            if (used[resourceIndex] && previous[resourceIndex] != use.access) {
                ++transitions;
            }
            used[resourceIndex] = true;
            previous[resourceIndex] = use.access;
        }
    }
    return transitions;
}

const char* ToString(const GraphError error) noexcept
{
    switch (error) {
    case GraphError::None: return "none";
    case GraphError::Empty: return "empty";
    case GraphError::DuplicatePass: return "duplicate-pass";
    case GraphError::ReadBeforeWrite: return "read-before-write";
    case GraphError::MissingReadback: return "missing-readback";
    }
    return "unknown";
}

}
