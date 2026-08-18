#pragma once

#include "renderer_core/EngineScene.h"
#include "renderer_core/EngineVisibility.h"
#include "renderer_core/EngineVertex.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace vf::renderer::drawstream {

// One draw the engine issued, as the D3D11 hooks observed it. This is the
// unit a full-scene mirror is built from: the engine pools its geometry into
// a couple of very large buffers and each draw names a range inside one, so a
// frame is a list of ranges and transforms rather than a list of meshes.
struct DrawRecordV1
{
    // The pooled vertex buffer this draw read. Zero means the capture never
    // saw the buffer bound, which is a hole rather than a draw.
    std::uint64_t vertexBuffer{};
    std::uint64_t indexBuffer{};
    std::uint32_t vertexStride{};
    // The byte offset IASetVertexBuffers bound the stream at. Fallout 4 draws
    // out of two 128 MB pools, so this is not decoration: it is half of the
    // address a readback has to use, and the other half is baseVertex. Two
    // meshes at different offsets in one pool otherwise share every field
    // that identifies them.
    std::uint32_t vertexByteOffset{};
    // The input layout bound for this draw. The engine declared the format of
    // its own vertex stream there, and it is the only exact source: a stride
    // wide enough for three floats is equally wide enough for four halves
    // plus a pair, and the two decode to completely different geometry.
    std::uint64_t inputLayout{};
    std::uint32_t indexCount{};
    std::uint32_t startIndex{};
    std::int32_t baseVertex{};
    std::uint32_t instanceCount{1};
    // DXGI_FORMAT_R16_UINT is 57 and R32_UINT is 42; stored as the raw value
    // so the reader is not guessing which width the pooled indices are.
    std::uint32_t indexFormat{};
    std::uint32_t indexOffset{};
    // Whether this draw's vertex-shader constant buffer at slot 0 is large
    // enough to hold a 4x4 at all. User-interface and fullscreen passes bind
    // small buffers there -- a sixteen-byte viewport constant, for instance --
    // and would otherwise inherit whatever world matrix the previous world
    // draw left behind, placing a screen-space quad somewhere in the cell.
    bool hasTransform{true};
    // The engine texture this draw shaded its base colour from, or zero when
    // none was resolved. Zero is a real answer, not a failure: most draws in
    // a frame are not material draws, and the shader bound for a post or
    // godray pass declares textures that are nobody's albedo.
    //
    // Resolved through the *shader*, never from a fixed slot: which
    // pixel-shader resource register carries base colour is a property of the
    // technique bound at the time, exactly as the lighting constant's offset
    // was a property of the shader that declared it.
    std::uint64_t baseColorTexture{};
    // The rasterizer state this draw ran under, read from the engine rather
    // than assumed.
    //
    // Assuming it is how the mirror rendered every model inside out: the
    // stream declared counter-clockwise front faces for everything, while
    // D3D11's own default is `FrontCounterClockwise = FALSE`. Which way round
    // it actually is depends on the engine's state *and* on the handedness of
    // the captured view, so it is the one thing here that must never be a
    // constant.
    //
    // `false` when no rasterizer state was ever bound on the drawing thread,
    // which is D3D11's documented default state rather than a guess.
    bool frontCounterClockwise{};
    // Whether the main scene depth was bound when this draw ran. False means
    // an off-screen pass -- the water reflection pass renders the whole world
    // a second time through a mirrored camera, and those draws are not world
    // geometry however much they look like it.
    bool sceneDepthBound{true};
    // Whether a pixel shader was bound when this draw ran. False means a
    // depth-only pass, which carries no colour and is not world geometry the
    // mirror should reproduce.
    //
    // Defaults true for the same reason `hasTransform` does: a producer that
    // does not set it gets the permissive answer, so forgetting the field
    // costs a duplicate rather than silently emptying the scene.
    bool hasPixelShader{true};
    // D3D11_CULL_MODE: 1 NONE, 2 FRONT, 3 BACK. Zero means no state was seen;
    // the reader treats that as the D3D default of BACK. Carried raw so the
    // core does not depend on a D3D header.
    std::uint32_t cullMode{};
    // Row-major 4x4, exactly as the vertex-shader constant buffer holds it.
    float model[16]{};
};

// D3D11_CULL_MODE, named here so the assembler is not comparing bare numbers.
inline constexpr std::uint32_t kCullModeUnknown = 0;
inline constexpr std::uint32_t kCullModeNone = 1;
inline constexpr std::uint32_t kCullModeFront = 2;
inline constexpr std::uint32_t kCullModeBack = 3;

enum class DrawStreamError : std::uint8_t
{
    None,
    UnknownVertexBuffer,
    UnknownIndexBuffer,
    EmptyGeometry,
    // An index count that is not a multiple of three is not a triangle list.
    // Translating it as one reads past the range and draws garbage.
    NotATriangleList,
    IndexCountOutOfRange,
    NonFiniteTransform,
    SingularTransform,
    ZeroInstances,
    // The draw carries no world transform of its own: not world geometry.
    NoTransform,
    // The bottom row is not (0,0,0,1), so the matrix is not an affine
    // placement and the scene packet will not carry it.
    NonAffineTransform,
    // Negative determinant. The scene packet requires a positive one, so a
    // mirrored placement cannot be represented; refused here, where it can
    // be counted, rather than at encode time where it costs the whole frame.
    MirroredTransform,
    // No pixel shader was bound, so this is a depth-only pass -- the depth
    // prepass or a shadow cascade. Measured live at 22,320 of 42,590 draws.
    //
    // These are not colour draws and must not become scene objects. Shadow
    // cascades often draw a different LOD mesh, which takes its own mesh
    // identity, is textured by nothing, and renders as a white duplicate
    // standing in the same place as the real one. The mirror derives its own
    // depth and its own shadows, so it needs none of them.
    DepthOnlyPass,
    // The main scene depth was not bound, so this draw belongs to an
    // off-screen pass. Measured live at 4,248 of 6,529 recorded draws.
    //
    // The water reflection pass redraws the whole world through a mirrored
    // camera, and the loading screen draws its object through its own. Both
    // look like world geometry and neither is: re-projected through the world
    // camera they arrive as a second, inverted scene, and the loading-screen
    // object arrives inside out because its winding was decided against a
    // projection this frame is not using.
    OffscreenPass,
};

inline constexpr std::size_t kDrawStreamErrorCount =
    static_cast<std::size_t>(DrawStreamError::OffscreenPass) + 1;

// A frame's worth of draws, plus what did not fit. The arena that fills this
// is bounded because it is written on the render thread; a dropped count that
// is reported is a known gap, and one that is not is a scene that is quietly
// missing objects.
struct DrawStreamFrame
{
    std::uint64_t frameIndex{};
    std::uint64_t droppedDraws{};
    std::vector<DrawRecordV1> draws;
};

// Limits a translation refuses to exceed. They are the scene packet's own,
// restated here so a translation can be rejected before it builds anything.
struct TranslationLimits
{
    std::uint32_t maximumObjects{scene::kMaximumOpaqueObjects};
    std::uint32_t maximumInstances{scene::kMaximumSceneInstances};
    // A single draw larger than this is a pooled batch rather than an object,
    // and is refused rather than becoming one enormous scene object.
    std::uint32_t maximumIndicesPerDraw{3'000'000};
};

struct TranslationResult
{
    std::uint32_t objects{};
    std::uint32_t instances{};
    std::uint64_t rejectedDraws{};
    std::uint64_t droppedDraws{};
    // Draws that shared a mesh with an earlier draw. This is the instancing
    // the engine already found; collapsing it away would multiply the scene's
    // object count by the number of copies on screen.
    std::uint64_t reusedMeshes{};
    // One count per reason, indexed by DrawStreamError. A single rejected
    // total says the scene is thin; only the breakdown says which rule made
    // it thin, and a rule that rejects everything is indistinguishable from
    // a rule that rejects nothing without it.
    std::array<std::uint32_t, kDrawStreamErrorCount> rejectedByReason{};
};

// Whether a draw can be translated at all, and why not when it cannot.
[[nodiscard]] DrawStreamError ValidateDraw(
    const DrawRecordV1& draw,
    const TranslationLimits& limits) noexcept;

// Stable identity for the *mesh* a draw reads: the pooled buffer and the
// range inside it. Deliberately excludes the transform, because the same
// range drawn at two transforms is one mesh drawn twice, which is exactly
// what the scene packet's instance table represents.
[[nodiscard]] std::uint64_t MeshIdentity(const DrawRecordV1& draw) noexcept;

// Identity for one drawn instance: the mesh, plus which occurrence of it this
// is. Two instances of one mesh must not collide, or the scene packet refuses
// the frame for a duplicate object.
[[nodiscard]] std::uint64_t InstanceIdentity(
    std::uint64_t meshIdentity,
    std::uint32_t occurrence) noexcept;

// Builds a scene packet from the frame. Objects are the distinct meshes and
// instances are the draws, so a cell full of the same fence post costs one
// object and many instances rather than many objects.
[[nodiscard]] DrawStreamError TranslateDrawStream(
    const DrawStreamFrame& frame,
    const TranslationLimits& limits,
    scene::ScenePacket& packet,
    TranslationResult& result) noexcept;

// One mesh whose geometry has not been read out of the pooled buffers yet.
// Reading it means a staging copy and a map, which synchronises the GPU, so
// the number of these a frame issues is a budget rather than a wish.
struct MeshExtractionRequest
{
    std::uint64_t meshIdentity{};
    std::uint64_t vertexBuffer{};
    std::uint64_t indexBuffer{};
    std::uint32_t vertexStride{};
    std::uint32_t vertexByteOffset{};
    std::uint64_t inputLayout{};
    std::uint32_t firstIndex{};
    std::uint32_t indexCount{};
    std::int32_t baseVertex{};
    std::uint32_t indexFormat{};
    std::uint32_t indexOffset{};
};

// What a frame may spend reading geometry back. A readback stalls the
// pipeline, so an unbounded one turns a renderer into a stutter.
struct ExtractionBudget
{
    std::uint32_t maximumMeshesPerFrame{8};
    std::uint64_t maximumIndexBytesPerFrame{1 << 20};
};

struct ExtractionPlan
{
    std::vector<MeshExtractionRequest> requests;
    // Meshes this frame wanted but the budget deferred. A scene still filling
    // in says so rather than looking finished and being wrong.
    std::uint64_t deferred{};
    // Meshes already cached, so nothing needed reading.
    std::uint64_t satisfied{};
};

// Decides which meshes to read this frame. `cached` is the set already held,
// in any order; a mesh in it is never requested again, because the pooled
// geometry behind a range does not change once it has streamed in.
[[nodiscard]] ExtractionPlan PlanMeshExtraction(
    const DrawStreamFrame& frame,
    const TranslationLimits& limits,
    std::span<const std::uint64_t> cached,
    const ExtractionBudget& budget) noexcept;

// The vertex range a set of indices actually touches, which is what has to be
// copied out of a 128 MB pool. Returns false when the index list is empty or
// the range would exceed what a single mesh may hold.
[[nodiscard]] bool VertexRangeForIndices(
    std::span<const std::uint32_t> indices,
    std::int32_t baseVertex,
    std::uint32_t& firstVertex,
    std::uint32_t& vertexCount) noexcept;

// One mesh that has been read out of the pools, ready to be concatenated.
// Positions are float3 at offset zero, which is what the stride-12 pool
// holds and was confirmed against the running game rather than assumed.
// Assembled geometry held across frames, one slot per mesh.
//
// A mesh keeps its place in the concatenated vertex and index arrays for as
// long as it keeps its identity and its backing bytes, so an unchanged mesh
// costs a lookup instead of a decode. Slots are appended and never moved: a
// mesh that disappears leaves its geometry behind unreferenced, which is
// cheaper than reshuffling every draw range around the hole. The arena is
// rebuilt whole once the unreferenced share outgrows the referenced one.
struct GeometryArena
{
    struct Slot
    {
        std::uint32_t vertexOffset{};
        std::uint32_t vertexCount{};
        std::uint32_t indexOffset{};
        std::uint32_t indexCount{};
        // What the slot was built from. Identity alone would call a
        // re-extracted mesh unchanged and draw the previous geometry under
        // the new mesh.s name.
        const void* vertices{};
        std::size_t vertexBytes{};
        std::size_t sourceIndices{};
        // What this mesh contributed to the normal counters, so a reused slot
        // reports the same totals a freshly decoded one does.
        std::uint64_t verticesWithNormals{};
        std::uint64_t verticesWithoutNormals{};
    };

    std::unordered_map<std::uint64_t, Slot> slots;
};

struct AssembledMesh
{
    std::uint64_t identity{};
    std::uint32_t vertexStride{};
    // How this mesh stores its vertices, as the engine declared it. Without it
    // the only option is to guess, and guessing three floats for a four-half
    // position collapses almost every vertex onto the origin.
    mesh::EngineVertexLayout layout;
    std::span<const std::byte> vertices;
    std::span<const std::uint32_t> indices;
    // The rasterizer state the engine drew this mesh under, carried from the
    // draw record that produced it. Defaults are D3D11's own defaults, so a
    // mesh whose state was never observed is culled the way the runtime would
    // have culled it rather than the way the mirror once assumed.
    bool frontCounterClockwise{};
    std::uint32_t cullMode{kCullModeUnknown};
};

struct AssemblyResult
{
    std::uint32_t drawnObjects{};
    // Objects whose geometry has not been read back yet. They are removed
    // from the scene rather than drawn empty, and counted, because a cell
    // still filling in must look unfinished rather than wrong.
    std::uint32_t missingMeshes{};
    // Meshes whose bytes do not read as positions: a pooled format this
    // build does not understand yet. Dropped and counted for the same reason
    // a missing one is, rather than allowed to reject the whole frame at
    // encode time and cost every other object its render.
    std::uint32_t unreadableMeshes{};
    std::uint64_t vertices{};
    std::uint64_t indices{};
    // Where the shading normals came from. Flat lighting has two causes that
    // no picture distinguishes: the engine's layouts did not declare NORMAL
    // and every vertex took the default, or the normals arrived and something
    // downstream ignored them. These two numbers separate them, which decides
    // which half of the pipeline to look in.
    std::uint64_t verticesWithNormals{};
    std::uint64_t verticesWithoutNormals{};
};

// Concatenates the cached meshes into one self-contained raster packet and
// narrows the scene packet to the objects that packet can actually draw.
// Both are rewritten together: a scene object whose draw index no longer
// matches its geometry would place the wrong mesh, which is worse than not
// drawing it.
[[nodiscard]] DrawStreamError AssembleSceneGeometry(
    scene::ScenePacket& packet,
    std::span<const AssembledMesh> meshes,
    raster::DecodedPacket& rasterPacket,
    AssemblyResult& result,
    // Assembled geometry that survives between frames. Null rebuilds
    // everything, which is what every offline caller wants.
    //
    // An all-or-nothing cache is not enough: a live cell streams continuously,
    // and twenty-one new meshes out of nine hundred forced a full re-decode of
    // 1.1 million vertices. Each mesh keeps its own slot, so a frame pays only
    // for the meshes that actually changed.
    GeometryArena* arena = nullptr) noexcept;

[[nodiscard]] const char* ToString(DrawStreamError error) noexcept;

}
