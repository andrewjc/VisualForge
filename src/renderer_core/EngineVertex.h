#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace vf::renderer::mesh {

enum class VertexSemantic : std::uint8_t
{
    Position,
    TexCoord0,
    TexCoord1,
    Normal,
    Tangent,
    Color,
    SkinWeights,
    SkinIndices,
    Landscape0,
    Landscape1,
    Eye
};

enum class VertexStorage : std::uint8_t
{
    Half4,
    Half2,
    EngineBasisByte4,
    UNormByte4,
    UByte4,
    Float1,
    // Full-precision positions and coordinates. Both these and the half forms
    // occur in one game, so a decoder that assumes either is wrong for half
    // the meshes -- and reading halves as floats collapses almost every
    // vertex onto the origin, which draws as a fan of spikes rather than as
    // anything recognisably misplaced.
    Float2,
    Float3,
    Float4
};

enum class VertexLayoutError : std::uint8_t
{
    None,
    InvalidStride,
    MissingPosition,
    MissingAttributeOffset,
    AttributeOverlap,
    AttributeOutOfBounds,
    UnsupportedFlags
};

enum class VertexDecodeError : std::uint8_t
{
    None,
    InvalidLayout,
    EmptyVertices,
    SizeOverflow,
    TruncatedStream,
    VertexOutOfRange,
    NonFiniteValue
};

struct VertexAttribute
{
    VertexSemantic semantic{VertexSemantic::Position};
    VertexStorage storage{VertexStorage::Half4};
    std::uint32_t offset{};
    std::uint32_t byteSize{};
};

struct EngineVertexLayout
{
    std::uint64_t raw{};
    std::uint32_t stride{};
    std::uint32_t dynamicStride{};
    std::uint16_t flags{};
    std::vector<VertexAttribute> attributes;

    [[nodiscard]] const VertexAttribute* Find(
        VertexSemantic semantic) const noexcept;
};

struct DecodedEngineVertex
{
    std::array<float, 4> position{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 2> texCoord0{};
    std::array<float, 4> texCoord1{};
    std::array<float, 4> normal{0.0f, 0.0f, 1.0f, 0.0f};
    std::array<float, 4> tangent{1.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> skinWeights{};
    std::array<std::uint8_t, 4> boneIndices{};
    std::array<float, 4> landscape0{};
    std::array<float, 4> landscape1{};
    float eye{};
};

// The DXGI format numbers this build recognises in an input layout. Written
// as named constants rather than pulled from d3d11.h so the layout rules stay
// testable without a device, and so a reader is not left guessing which width
// a raw number means.
inline constexpr std::uint32_t kFormatR32G32B32A32Float = 2;
inline constexpr std::uint32_t kFormatR32G32B32Float = 6;
inline constexpr std::uint32_t kFormatR16G16B16A16Float = 10;
inline constexpr std::uint32_t kFormatR32G32Float = 16;
inline constexpr std::uint32_t kFormatR8G8B8A8Unorm = 28;
inline constexpr std::uint32_t kFormatR8G8B8A8Uint = 30;
inline constexpr std::uint32_t kFormatR16G16Float = 34;
inline constexpr std::uint32_t kFormatR32Float = 41;

// One element of an input layout, as the engine declared it. This is the
// engine's own description of its vertex stream, which is why it is preferred
// to anything inferred from a stride: a stride wide enough for three floats
// is equally wide enough for four halves plus a pair, and the two decode to
// completely different geometry.
struct InputElementDesc
{
    std::string_view semanticName;
    std::uint32_t semanticIndex{};
    std::uint32_t format{};
    std::uint32_t inputSlot{};
    std::uint32_t alignedByteOffset{};
};

// Builds a layout for one input slot from the engine's declared elements.
// Elements bound to other slots belong to other streams and are left out
// rather than folded in, because their offsets are into a different buffer.
[[nodiscard]] VertexLayoutError BuildLayoutFromInputElements(
    std::span<const InputElementDesc> elements,
    std::uint32_t stride,
    std::uint32_t inputSlot,
    EngineVertexLayout& layout) noexcept;

[[nodiscard]] VertexLayoutError ParseEngineVertexLayout(
    std::uint64_t raw,
    EngineVertexLayout& layout) noexcept;
[[nodiscard]] VertexDecodeError DecodeEngineVertex(
    const EngineVertexLayout& layout,
    std::span<const std::byte> bytes,
    std::size_t vertexIndex,
    DecodedEngineVertex& vertex) noexcept;
[[nodiscard]] VertexDecodeError PackEngineVertices(
    const EngineVertexLayout& layout,
    std::span<const DecodedEngineVertex> vertices,
    std::vector<std::byte>& bytes) noexcept;

[[nodiscard]] float HalfToFloat(std::uint16_t value) noexcept;
[[nodiscard]] std::uint16_t FloatToHalf(float value) noexcept;
[[nodiscard]] const char* ToString(VertexLayoutError error) noexcept;
[[nodiscard]] const char* ToString(VertexDecodeError error) noexcept;

}
