#pragma once

#include "renderer_core/EngineDrawStream.h"
#include "renderer_core/EngineVertex.h"

#include <cstddef>
#include <cstdint>

namespace vf::engine_draw_capture {

// What one frame's draw stream looked like. This is the measurement that has
// to come before a full scene can be mirrored: until the drawn set is known,
// "render the whole scene" has no definition and no way to be checked.
struct FrameSummary
{
    std::uint64_t drawCalls{};
    std::uint64_t instancedDrawCalls{};
    std::uint64_t indices{};
    // Distinct vertex buffers bound across the frame, counted through a small
    // fixed table. A scene is not the number of draws but the number of
    // distinct meshes those draws reach.
    std::uint32_t distinctVertexBuffers{};
    // Buffers seen beyond the table's capacity. Reported rather than dropped
    // silently, because a truncated count that looks plausible is worse than
    // a count that says it is incomplete.
    std::uint64_t overflowedVertexBuffers{};
    std::uint32_t largestIndexCount{};
};

// One buffer the draw stream touched, described once on first sighting. The
// description is what decides whether a full-scene mirror is affordable: six
// pooled buffers of a few megabytes can be staged per frame, and six of a few
// hundred cannot.
struct BufferReport
{
    std::uint64_t handle{};
    std::uint32_t byteWidth{};
    std::uint32_t stride{};
    std::uint32_t usage{};
    std::uint32_t cpuAccessFlags{};
    std::uint32_t bindFlags{};
    // Draws that read this buffer as the slot-0 geometry stream.
    std::uint64_t draws{};
    // Times the buffer was mapped for writing. A constant buffer rewritten
    // once per draw is the per-object one; a buffer mapped once per frame is
    // per-pass state and carries no per-object transform.
    std::uint64_t maps{};
};

inline constexpr std::size_t kMaximumReportedBuffers = 32;

// The pixel-shader constant table, sized from what the engine actually binds.
// Thirty-two was not enough, and neither was 256: every slot filled and the
// per-frame blocks the shaders name never got one. Indexed by a hash of the
// buffer address, so it must stay a power of two.
inline constexpr std::size_t kMaximumPsConstantBuffers = 4096;

// Vertex buffers, then the vertex-shader constant buffers, both in first-seen
// order. Returns how many entries were written.
[[nodiscard]] std::size_t DescribeVertexBuffers(
    BufferReport* destination,
    std::size_t capacity) noexcept;
// Samples of what the engine wrote into the small, frequently mapped vertex
// constant buffers. The per-draw world transform is in one of them, and
// reading the bytes is the only way to say which: a size alone is a guess.
struct ConstantSample
{
    std::uint32_t byteWidth{};
    std::uint32_t bytes{};
    float values[32]{};
};

inline constexpr std::size_t kMaximumConstantSamples = 8;

// The draws the frame recorded, in submission order, capped by the arena.
// Bounded because it is written on the render thread: an unbounded list
// would allocate there, and a frame is about four thousand draws.
inline constexpr std::size_t kDrawArenaCapacity = 8192;

// Copies out the draws recorded since the last call and empties the arena.
[[nodiscard]] std::size_t CollectDraws(
    renderer::drawstream::DrawRecordV1* destination,
    std::size_t capacity,
    std::uint64_t& dropped) noexcept;

[[nodiscard]] std::size_t CollectConstantSamples(
    ConstantSample* destination,
    std::size_t capacity) noexcept;

[[nodiscard]] std::size_t DescribeConstantBuffers(
    BufferReport* destination,
    std::size_t capacity) noexcept;

// Resolves the context entry points from a vtable captured off a dummy
// device, exactly as the texture capture does. Returns false when a slot is
// missing rather than hooking something unknown.
// The layout the engine declared for a bound input layout, built for the
// geometry stream at slot zero. False when the handle was never seen, which a
// caller must treat as "cannot decode this mesh" rather than as a default.
[[nodiscard]] bool FindInputLayout(
    std::uint64_t handle,
    std::uint32_t stride,
    renderer::mesh::EngineVertexLayout& layout) noexcept;

// Layouts the engine created past the fixed table. Reported rather than
// hidden: draws using them decode as unknown, so a non-zero count explains a
// scene that is missing objects.
[[nodiscard]] std::uint32_t LayoutOverflowCount() noexcept;

// Why meshes are or are not decodable, as counts rather than as a guess.
// `recorded` at zero means the hook was installed after the engine built its
// vertex formats and every mesh will be declined; misses against a healthy
// `recorded` mean something else. One number could not tell those apart.
struct LayoutStats
{
    std::uint32_t recorded{};
    std::uint32_t overflow{};
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t unbuildable{};
};

[[nodiscard]] LayoutStats LayoutCounters() noexcept;

[[nodiscard]] bool PrepareHooks(
    void* drawIndexed,
    void* drawIndexedInstanced,
    void* setVertexBuffers,
    void* setIndexBuffer,
    void* setVsConstantBuffers,
    void* map,
    void* unmap,
    void* setInputLayout,
    void* createInputLayout,
    void* setPsConstantBuffers,
    void* createPixelShader,
    void* updateSubresource) noexcept;

// A constant buffer as the shader that uses it declares it.
//
// Sampling the engine's buffers and reading fixed offsets stalled, because the
// engine binds one wide pixel-shader buffer across shader techniques and an
// offset therefore means different things from draw to draw. Compiled shaders
// carry their own reflection, so the names and offsets can be read from the
// bytecode the engine hands to CreatePixelShader instead of inferred from the
// numbers flowing through it.
struct ShaderBufferField
{
    char name[64]{};
    std::uint32_t offset{};
    std::uint32_t size{};
};

constexpr std::size_t kShaderBufferFieldCapacity = 48;

struct ShaderBufferLayout
{
    char name[64]{};
    std::uint32_t byteWidth{};
    std::uint32_t fieldCount{};
    // How many distinct shaders declared this buffer. A block shared by
    // hundreds of techniques is engine-wide state; one used by a single
    // technique is that technique's own.
    std::uint64_t shaders{};
    ShaderBufferField fields[kShaderBufferFieldCapacity]{};
};

constexpr std::size_t kShaderBufferLayoutCapacity = 64;

[[nodiscard]] std::size_t CopyShaderBufferLayouts(
    ShaderBufferLayout* destination,
    std::size_t capacity) noexcept;

// Why the catalogue looks the way it does. `shaders` at zero means the hook
// was installed after the engine compiled its shaders, which is a different
// problem from bytecode that would not parse.
struct ShaderReflectionStats
{
    std::uint64_t shaders{};
    std::uint64_t reflected{};
    std::uint64_t failed{};
    std::uint32_t layouts{};
    std::uint32_t layoutOverflow{};
    std::uint32_t fieldOverflow{};
};

[[nodiscard]] ShaderReflectionStats ShaderReflectionCounters() noexcept;

// Installs the hooks. Off unless VISUALFORGE_DRAW_CAPTURE is set, because
// these run on the render thread at draw frequency and must not be paid for
// by a session that did not ask for them.
// The per-frame lighting the engine wrote into its pixel-shader constants,
// and whether a frame has been seen at all. Empty until a buffer of the right
// shape has been observed: the mirror declares no environment rather than
// inventing one, because a frame with no captured lighting must leave the
// albedo alone rather than be lit by numbers nobody measured.
struct LightingSample
{
    bool valid{};
    // Which buffer this is. Sampling by width instead mixed every buffer of
    // the same size into one slot, and the engine has several 752-byte
    // pixel-shader blocks: every word then straddles unrelated contents and
    // reads as varying, which is what made the earlier offset archaeology
    // stall. A sample is only a measurement of one buffer at a time.
    std::uint64_t handle{};
    // How large the buffer was, so a wrong reading is diagnosable as the
    // wrong buffer rather than as bad lighting.
    std::uint32_t byteWidth{};
    // How often the engine rewrote it. A per-frame block is written once a
    // frame and a per-draw block thousands of times, and that ratio is what
    // separates the sky's lighting from a material's constants.
    std::uint64_t maps{};
    // Wide enough for the lighting shader's per-object block, which carries a
    // list of point lights as well as the directional one and the ambient.
    float values[256]{};
    // The range each word took across every write seen. A per-draw buffer
    // carries both global and per-object data in one block, and a single
    // sample cannot tell them apart -- it is whichever object happened to be
    // drawn last. A word that never moved across thousands of draws is the
    // sun, the ambient, the fog or the camera; a word that moved is that
    // object's transform, its material or its own point lights.
    float lowest[256]{};
    float highest[256]{};
};

// One sample per distinct buffer size in the per-frame range, because which
// size carries the lighting is exactly what is being measured.
// One per distinct pixel-shader constant buffer, not per width. The engine
// binds a few dozen; this holds all of them with room to spare.
constexpr std::size_t kLightingSampleSlots = 64;

[[nodiscard]] std::size_t CopyLightingSamples(
    LightingSample* destination,
    std::size_t capacity) noexcept;

// Counts of what the pixel-shader constant hook has seen, so a frame that
// finds no lighting can be read as "no buffer of that shape was bound"
// rather than as "the decode failed".
struct ConstantStats
{
    // Binds that found no free slot. Non-zero means the table is too small and
    // some buffers are invisible, which is a different failure from a buffer
    // that is described but never written.
    std::uint64_t psDescribeOverflow{};
    std::uint64_t psBinds{};
    std::uint64_t psDescribed{};
    std::uint64_t psSampled{};
};

[[nodiscard]] ConstantStats ConstantCounters() noexcept;

// The pixel-shader constant buffers the engine has bound, so their sizes can
// be read off rather than assumed. Which one carries the per-frame lighting is
// a measurement.
[[nodiscard]] std::size_t CopyPsConstantReports(
    BufferReport* destination,
    std::size_t capacity) noexcept;

[[nodiscard]] bool Install() noexcept;
[[nodiscard]] bool Enabled() noexcept;

// Called from Present. Returns the frame that just ended and starts a new
// one.
[[nodiscard]] FrameSummary EndFrame() noexcept;

}
