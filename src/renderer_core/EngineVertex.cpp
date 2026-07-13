#include "renderer_core/EngineVertex.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace vf::renderer::mesh {

namespace {

constexpr std::uint64_t kPosition = std::uint64_t{1} << 44;
constexpr std::uint64_t kTexCoord0 = std::uint64_t{1} << 45;
constexpr std::uint64_t kTexCoord1 = std::uint64_t{1} << 46;
constexpr std::uint64_t kNormal = std::uint64_t{1} << 47;
constexpr std::uint64_t kTangent = std::uint64_t{1} << 48;
constexpr std::uint64_t kColor = std::uint64_t{1} << 49;
constexpr std::uint64_t kSkin = std::uint64_t{1} << 50;
constexpr std::uint64_t kLandscape = std::uint64_t{1} << 51;
constexpr std::uint64_t kEye = std::uint64_t{1} << 52;
constexpr std::uint64_t kFullPrecision = std::uint64_t{1} << 54;
constexpr std::uint64_t kKnownFlagMask =
    kPosition | kTexCoord0 | kTexCoord1 | kNormal | kTangent |
    kColor | kSkin | kLandscape | kEye | kFullPrecision;
constexpr std::uint64_t kDescriptorFlagMask = std::uint64_t{0xFFFF} << 40;

std::uint32_t NibbleBytes(const std::uint64_t raw, const unsigned bit) noexcept
{
    return static_cast<std::uint32_t>((raw >> bit) & 0xFu) * 4u;
}

void AddAttribute(
    EngineVertexLayout& layout,
    const VertexSemantic semantic,
    const VertexStorage storage,
    const std::uint32_t offset,
    const std::uint32_t byteSize)
{
    layout.attributes.push_back({semantic, storage, offset, byteSize});
}

bool Overlaps(const VertexAttribute& first, const VertexAttribute& second) noexcept
{
    const auto firstEnd = first.offset + first.byteSize;
    const auto secondEnd = second.offset + second.byteSize;
    return first.offset < secondEnd && second.offset < firstEnd;
}

std::uint16_t ReadU16(const std::byte* bytes) noexcept
{
    std::uint16_t value{};
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

void WriteU16(std::byte* bytes, const std::uint16_t value) noexcept
{
    std::memcpy(bytes, &value, sizeof(value));
}

float DecodeBasisByte(const std::byte value) noexcept
{
    return static_cast<float>(std::to_integer<std::uint8_t>(value)) *
        (2.0f / 255.0f) - 1.0f;
}

std::byte EncodeBasisByte(const float value) noexcept
{
    const auto scaled = (std::clamp(value, -1.0f, 1.0f) + 1.0f) *
        (255.0f * 0.5f);
    const auto encoded = static_cast<unsigned>(
        std::floor(scaled + 0.0001f));
    return static_cast<std::byte>(std::min(encoded, 255u));
}

float DecodeUNormByte(const std::byte value) noexcept
{
    return static_cast<float>(std::to_integer<std::uint8_t>(value)) /
        255.0f;
}

std::byte EncodeUNormByte(const float value) noexcept
{
    const auto scaled = std::clamp(value, 0.0f, 1.0f) * 255.0f;
    const auto encoded = static_cast<unsigned>(
        std::floor(scaled + 0.0001f));
    return static_cast<std::byte>(std::min(encoded, 255u));
}

template <std::size_t Extent>
bool AllFinite(const std::array<float, Extent>& values) noexcept
{
    return std::all_of(values.begin(), values.end(), [](const float value) {
        return std::isfinite(value);
    });
}

void DecodeHalfArray(
    const std::byte* source,
    float* destination,
    const std::size_t count) noexcept
{
    for (std::size_t index = 0; index < count; ++index) {
        destination[index] = HalfToFloat(ReadU16(source + index * 2));
    }
}

// A real-valued attribute, read at whatever width the layout says it was
// written at. The same semantic occurs in both widths in one game, so the
// width is data rather than a constant of the format.
void DecodeReal(
    const std::byte* source,
    const VertexStorage storage,
    float* destination,
    const std::size_t count) noexcept
{
    switch (storage) {
    case VertexStorage::Float1:
    case VertexStorage::Float2:
    case VertexStorage::Float3:
    case VertexStorage::Float4: {
        const auto available = storage == VertexStorage::Float1 ? 1u
            : storage == VertexStorage::Float2 ? 2u
            : storage == VertexStorage::Float3 ? 3u : 4u;
        const auto copied = std::min<std::size_t>(count, available);
        for (std::size_t index = 0; index < copied; ++index) {
            float value = 0.0f;
            std::memcpy(&value, source + index * sizeof(float), sizeof(value));
            destination[index] = value;
        }
        // A three-float position has no w. Leaving it at one keeps the
        // homogeneous form the rest of the pipeline expects.
        if (count == 4 && available == 3) destination[3] = 1.0f;
        break;
    }
    case VertexStorage::Half2:
        DecodeHalfArray(source, destination, std::min<std::size_t>(count, 2));
        break;
    default:
        DecodeHalfArray(source, destination, count);
        break;
    }
}

void EncodeHalfArray(
    const float* source,
    std::byte* destination,
    const std::size_t count) noexcept
{
    for (std::size_t index = 0; index < count; ++index) {
        WriteU16(destination + index * 2, FloatToHalf(source[index]));
    }
}

}

const VertexAttribute* EngineVertexLayout::Find(
    const VertexSemantic semantic) const noexcept
{
    const auto found = std::find_if(
        attributes.begin(), attributes.end(), [semantic](const auto& value) {
            return value.semantic == semantic;
        });
    return found == attributes.end() ? nullptr : &*found;
}

VertexLayoutError BuildLayoutFromInputElements(
    const std::span<const InputElementDesc> elements,
    const std::uint32_t stride,
    const std::uint32_t inputSlot,
    EngineVertexLayout& layout) noexcept
{
    try {
        layout = {};
        layout.stride = stride;
        if (stride < 8) return VertexLayoutError::InvalidStride;

        for (const auto& element : elements) {
            // Another slot is another buffer. Its offsets are into that
            // buffer, so folding them in here would read this stream at
            // addresses that mean nothing in it.
            if (element.inputSlot != inputSlot) continue;

            VertexSemantic semantic{};
            if (element.semanticName == "POSITION" ||
                element.semanticName == "SV_Position") {
                if (element.semanticIndex != 0) continue;
                semantic = VertexSemantic::Position;
            } else if (element.semanticName == "TEXCOORD") {
                if (element.semanticIndex == 0) {
                    semantic = VertexSemantic::TexCoord0;
                } else if (element.semanticIndex == 1) {
                    semantic = VertexSemantic::TexCoord1;
                } else {
                    continue;
                }
            } else if (element.semanticName == "NORMAL") {
                semantic = VertexSemantic::Normal;
            } else if (element.semanticName == "TANGENT" ||
                element.semanticName == "BINORMAL") {
                semantic = VertexSemantic::Tangent;
            } else if (element.semanticName == "COLOR") {
                semantic = VertexSemantic::Color;
            } else if (element.semanticName == "BLENDWEIGHT") {
                semantic = VertexSemantic::SkinWeights;
            } else if (element.semanticName == "BLENDINDICES") {
                semantic = VertexSemantic::SkinIndices;
            } else {
                // Not a semantic this build decodes. Skipped rather than
                // guessed at: an attribute read as the wrong thing is worse
                // than one that is absent, because it looks like data.
                continue;
            }

            VertexStorage storage{};
            std::uint32_t byteSize = 0;
            switch (element.format) {
            case kFormatR32G32B32A32Float:
                storage = VertexStorage::Float4;
                byteSize = 16;
                break;
            case kFormatR32G32B32Float:
                storage = VertexStorage::Float3;
                byteSize = 12;
                break;
            case kFormatR32G32Float:
                storage = VertexStorage::Float2;
                byteSize = 8;
                break;
            case kFormatR32Float:
                storage = VertexStorage::Float1;
                byteSize = 4;
                break;
            case kFormatR16G16B16A16Float:
                storage = VertexStorage::Half4;
                byteSize = 8;
                break;
            case kFormatR16G16Float:
                storage = VertexStorage::Half2;
                byteSize = 4;
                break;
            case kFormatR8G8B8A8Unorm:
                storage = semantic == VertexSemantic::Normal ||
                        semantic == VertexSemantic::Tangent
                    ? VertexStorage::EngineBasisByte4
                    : VertexStorage::UNormByte4;
                byteSize = 4;
                break;
            case kFormatR8G8B8A8Uint:
                storage = VertexStorage::UByte4;
                byteSize = 4;
                break;
            default:
                continue;
            }

            // An attribute that runs past the stride reads into the next
            // vertex, which decodes as data belonging to a different point.
            if (element.alignedByteOffset > stride ||
                byteSize > stride - element.alignedByteOffset) {
                return VertexLayoutError::AttributeOutOfBounds;
            }
            AddAttribute(layout, semantic, storage,
                element.alignedByteOffset, byteSize);
        }

        // A stream with no position is not geometry, whatever else it
        // declares.
        if (layout.Find(VertexSemantic::Position) == nullptr) {
            return VertexLayoutError::MissingPosition;
        }
        return VertexLayoutError::None;
    } catch (const std::bad_alloc&) {
        layout = {};
        return VertexLayoutError::InvalidStride;
    }
}

VertexLayoutError ParseEngineVertexLayout(
    const std::uint64_t raw,
    EngineVertexLayout& layout) noexcept
{
    try {
        layout = {};
        layout.raw = raw;
        layout.stride = NibbleBytes(raw, 0);
        layout.dynamicStride = NibbleBytes(raw, 4);
        layout.flags = static_cast<std::uint16_t>((raw >> 40) & 0xFFFFu);
        if (layout.stride < 8) {
            return VertexLayoutError::InvalidStride;
        }
        if ((raw & kPosition) == 0) {
            return VertexLayoutError::MissingPosition;
        }
        if ((raw & kDescriptorFlagMask & ~kKnownFlagMask) != 0) {
            return VertexLayoutError::UnsupportedFlags;
        }

        AddAttribute(
            layout, VertexSemantic::Position, VertexStorage::Half4, 0, 8);
        const auto addFlagged = [&](
            const std::uint64_t flag,
            const unsigned offsetBit,
            const VertexSemantic semantic,
            const VertexStorage storage,
            const std::uint32_t byteSize) -> VertexLayoutError {
            if ((raw & flag) == 0) {
                return VertexLayoutError::None;
            }
            const auto offset = NibbleBytes(raw, offsetBit);
            if (offset == 0) {
                return VertexLayoutError::MissingAttributeOffset;
            }
            AddAttribute(layout, semantic, storage, offset, byteSize);
            return VertexLayoutError::None;
        };

        VertexLayoutError result{};
        result = addFlagged(kTexCoord0, 8, VertexSemantic::TexCoord0,
                            VertexStorage::Half2, 4);
        if (result != VertexLayoutError::None) return result;
        result = addFlagged(kTexCoord1, 12, VertexSemantic::TexCoord1,
                            VertexStorage::Half4, 8);
        if (result != VertexLayoutError::None) return result;
        result = addFlagged(kNormal, 16, VertexSemantic::Normal,
                            VertexStorage::EngineBasisByte4, 4);
        if (result != VertexLayoutError::None) return result;
        result = addFlagged(kTangent, 20, VertexSemantic::Tangent,
                            VertexStorage::EngineBasisByte4, 4);
        if (result != VertexLayoutError::None) return result;
        result = addFlagged(kColor, 24, VertexSemantic::Color,
                            VertexStorage::UNormByte4, 4);
        if (result != VertexLayoutError::None) return result;
        if ((raw & kSkin) != 0) {
            const auto offset = NibbleBytes(raw, 28);
            if (offset == 0) return VertexLayoutError::MissingAttributeOffset;
            AddAttribute(layout, VertexSemantic::SkinWeights,
                         VertexStorage::Half4, offset, 8);
            AddAttribute(layout, VertexSemantic::SkinIndices,
                         VertexStorage::UByte4, offset + 8, 4);
        }
        if ((raw & kLandscape) != 0) {
            const auto offset = NibbleBytes(raw, 32);
            if (offset == 0) return VertexLayoutError::MissingAttributeOffset;
            AddAttribute(layout, VertexSemantic::Landscape0,
                         VertexStorage::UNormByte4, offset, 4);
            AddAttribute(layout, VertexSemantic::Landscape1,
                         VertexStorage::UNormByte4, offset + 4, 4);
        }
        result = addFlagged(kEye, 36, VertexSemantic::Eye,
                            VertexStorage::Float1, 4);
        if (result != VertexLayoutError::None) return result;

        for (std::size_t index = 0; index < layout.attributes.size(); ++index) {
            const auto& attribute = layout.attributes[index];
            if (attribute.offset > layout.stride ||
                attribute.byteSize > layout.stride - attribute.offset) {
                return VertexLayoutError::AttributeOutOfBounds;
            }
            for (std::size_t prior = 0; prior < index; ++prior) {
                if (Overlaps(attribute, layout.attributes[prior])) {
                    return VertexLayoutError::AttributeOverlap;
                }
            }
        }
        return VertexLayoutError::None;
    } catch (...) {
        layout = {};
        return VertexLayoutError::InvalidStride;
    }
}

float HalfToFloat(const std::uint16_t value) noexcept
{
    const auto sign = static_cast<std::uint32_t>(value & 0x8000u) << 16;
    auto exponent = static_cast<std::uint32_t>((value >> 10) & 0x1Fu);
    auto mantissa = static_cast<std::uint32_t>(value & 0x03FFu);
    std::uint32_t bits{};
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 113;
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x03FFu;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1Fu) {
        bits = sign | 0x7F800000u | (mantissa << 13);
        if (mantissa != 0) bits |= 0x00400000u;
    } else {
        bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    return std::bit_cast<float>(bits);
}

std::uint16_t FloatToHalf(const float value) noexcept
{
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000u);
    const auto absolute = bits & 0x7FFFFFFFu;
    if (absolute >= 0x7F800000u) {
        if (absolute == 0x7F800000u) return sign | 0x7C00u;
        return static_cast<std::uint16_t>(sign | 0x7E00u);
    }
    if (absolute > 0x477FEFFFu) {
        return static_cast<std::uint16_t>(sign | 0x7C00u);
    }
    if (absolute < 0x33000001u) {
        return sign;
    }

    const auto exponent = static_cast<int>((absolute >> 23) & 0xFFu) - 127;
    auto mantissa = absolute & 0x007FFFFFu;
    if (exponent < -14) {
        mantissa |= 0x00800000u;
        const auto shift = static_cast<unsigned>(-exponent - 1);
        const auto halfMantissa = mantissa >> shift;
        const auto remainder = mantissa & ((std::uint32_t{1} << shift) - 1u);
        const auto halfway = std::uint32_t{1} << (shift - 1u);
        auto rounded = halfMantissa;
        if (remainder > halfway ||
            (remainder == halfway && (halfMantissa & 1u) != 0)) {
            ++rounded;
        }
        return static_cast<std::uint16_t>(sign | rounded);
    }

    auto halfExponent = static_cast<std::uint32_t>(exponent + 15);
    auto halfMantissa = mantissa >> 13;
    const auto remainder = mantissa & 0x1FFFu;
    if (remainder > 0x1000u ||
        (remainder == 0x1000u && (halfMantissa & 1u) != 0)) {
        ++halfMantissa;
        if (halfMantissa == 0x400u) {
            halfMantissa = 0;
            ++halfExponent;
        }
    }
    if (halfExponent >= 31) return static_cast<std::uint16_t>(sign | 0x7C00u);
    return static_cast<std::uint16_t>(
        sign | (halfExponent << 10) | halfMantissa);
}

VertexDecodeError DecodeEngineVertex(
    const EngineVertexLayout& layout,
    const std::span<const std::byte> bytes,
    const std::size_t vertexIndex,
    DecodedEngineVertex& vertex) noexcept
{
    vertex = {};
    if (layout.stride < 8 || layout.attributes.empty()) {
        return VertexDecodeError::InvalidLayout;
    }
    if (vertexIndex > std::numeric_limits<std::size_t>::max() / layout.stride) {
        return VertexDecodeError::VertexOutOfRange;
    }
    const auto offset = vertexIndex * layout.stride;
    if (offset >= bytes.size()) {
        return bytes.empty() && vertexIndex != 0
            ? VertexDecodeError::VertexOutOfRange
            : VertexDecodeError::TruncatedStream;
    }
    if (layout.stride > bytes.size() - offset) {
        return VertexDecodeError::TruncatedStream;
    }
    const auto* source = bytes.data() + offset;
    for (const auto& attribute : layout.attributes) {
        const auto* input = source + attribute.offset;
        switch (attribute.semantic) {
        // These three are the ones that occur in both widths. Switching on the
        // storage rather than the semantic is what keeps a full-precision
        // mesh from being read as halves and a half mesh from being read as
        // floats -- the second of which collapses almost every vertex onto
        // the origin and draws as a fan of spikes.
        case VertexSemantic::Position:
            DecodeReal(input, attribute.storage, vertex.position.data(), 4);
            break;
        case VertexSemantic::TexCoord0:
            DecodeReal(input, attribute.storage, vertex.texCoord0.data(), 2);
            break;
        case VertexSemantic::TexCoord1:
            DecodeReal(input, attribute.storage, vertex.texCoord1.data(), 4);
            break;
        case VertexSemantic::Normal:
        case VertexSemantic::Tangent: {
            auto& target = attribute.semantic == VertexSemantic::Normal
                ? vertex.normal : vertex.tangent;
            for (std::size_t component = 0; component < 4; ++component) {
                target[component] = DecodeBasisByte(input[component]);
            }
            break;
        }
        case VertexSemantic::Color:
        case VertexSemantic::Landscape0:
        case VertexSemantic::Landscape1: {
            auto* target = vertex.color.data();
            if (attribute.semantic == VertexSemantic::Landscape0) {
                target = vertex.landscape0.data();
            } else if (attribute.semantic == VertexSemantic::Landscape1) {
                target = vertex.landscape1.data();
            }
            for (std::size_t component = 0; component < 4; ++component) {
                target[component] = DecodeUNormByte(input[component]);
            }
            break;
        }
        case VertexSemantic::SkinWeights:
            DecodeHalfArray(input, vertex.skinWeights.data(), 4);
            break;
        case VertexSemantic::SkinIndices:
            for (std::size_t component = 0; component < 4; ++component) {
                vertex.boneIndices[component] =
                    std::to_integer<std::uint8_t>(input[component]);
            }
            break;
        case VertexSemantic::Eye:
            std::memcpy(&vertex.eye, input, sizeof(vertex.eye));
            break;
        }
    }
    return VertexDecodeError::None;
}

VertexDecodeError PackEngineVertices(
    const EngineVertexLayout& layout,
    const std::span<const DecodedEngineVertex> vertices,
    std::vector<std::byte>& bytes) noexcept
{
    bytes.clear();
    if (layout.stride < 8 || layout.attributes.empty()) {
        return VertexDecodeError::InvalidLayout;
    }
    if (vertices.empty()) {
        return VertexDecodeError::EmptyVertices;
    }
    if (vertices.size() >
        std::numeric_limits<std::size_t>::max() / layout.stride) {
        return VertexDecodeError::SizeOverflow;
    }
    try {
        bytes.assign(vertices.size() * layout.stride, std::byte{});
        for (std::size_t index = 0; index < vertices.size(); ++index) {
            const auto& vertex = vertices[index];
            if (!AllFinite(vertex.position) || !AllFinite(vertex.texCoord0) ||
                !AllFinite(vertex.texCoord1) || !AllFinite(vertex.normal) ||
                !AllFinite(vertex.tangent) || !AllFinite(vertex.color) ||
                !AllFinite(vertex.skinWeights) ||
                !AllFinite(vertex.landscape0) ||
                !AllFinite(vertex.landscape1) || !std::isfinite(vertex.eye)) {
                bytes.clear();
                return VertexDecodeError::NonFiniteValue;
            }
            auto* destination = bytes.data() + index * layout.stride;
            for (const auto& attribute : layout.attributes) {
                auto* output = destination + attribute.offset;
                switch (attribute.semantic) {
                case VertexSemantic::Position:
                    EncodeHalfArray(vertex.position.data(), output, 4);
                    break;
                case VertexSemantic::TexCoord0:
                    EncodeHalfArray(vertex.texCoord0.data(), output, 2);
                    break;
                case VertexSemantic::TexCoord1:
                    EncodeHalfArray(vertex.texCoord1.data(), output, 4);
                    break;
                case VertexSemantic::Normal:
                case VertexSemantic::Tangent: {
                    const auto& source = attribute.semantic == VertexSemantic::Normal
                        ? vertex.normal : vertex.tangent;
                    for (std::size_t component = 0; component < 4; ++component) {
                        output[component] = EncodeBasisByte(source[component]);
                    }
                    break;
                }
                case VertexSemantic::Color:
                case VertexSemantic::Landscape0:
                case VertexSemantic::Landscape1: {
                    const auto* source = vertex.color.data();
                    if (attribute.semantic == VertexSemantic::Landscape0) {
                        source = vertex.landscape0.data();
                    } else if (attribute.semantic == VertexSemantic::Landscape1) {
                        source = vertex.landscape1.data();
                    }
                    for (std::size_t component = 0; component < 4; ++component) {
                        output[component] = EncodeUNormByte(source[component]);
                    }
                    break;
                }
                case VertexSemantic::SkinWeights:
                    EncodeHalfArray(vertex.skinWeights.data(), output, 4);
                    break;
                case VertexSemantic::SkinIndices:
                    for (std::size_t component = 0; component < 4; ++component) {
                        output[component] =
                            static_cast<std::byte>(vertex.boneIndices[component]);
                    }
                    break;
                case VertexSemantic::Eye:
                    std::memcpy(output, &vertex.eye, sizeof(vertex.eye));
                    break;
                }
            }
        }
        return VertexDecodeError::None;
    } catch (...) {
        bytes.clear();
        return VertexDecodeError::SizeOverflow;
    }
}

const char* ToString(const VertexLayoutError error) noexcept
{
    switch (error) {
    case VertexLayoutError::None: return "none";
    case VertexLayoutError::InvalidStride: return "invalid-stride";
    case VertexLayoutError::MissingPosition: return "missing-position";
    case VertexLayoutError::MissingAttributeOffset: return "missing-attribute-offset";
    case VertexLayoutError::AttributeOverlap: return "attribute-overlap";
    case VertexLayoutError::AttributeOutOfBounds: return "attribute-out-of-bounds";
    case VertexLayoutError::UnsupportedFlags: return "unsupported-flags";
    }
    return "unknown";
}

const char* ToString(const VertexDecodeError error) noexcept
{
    switch (error) {
    case VertexDecodeError::None: return "none";
    case VertexDecodeError::InvalidLayout: return "invalid-layout";
    case VertexDecodeError::EmptyVertices: return "empty-vertices";
    case VertexDecodeError::SizeOverflow: return "size-overflow";
    case VertexDecodeError::TruncatedStream: return "truncated-stream";
    case VertexDecodeError::VertexOutOfRange: return "vertex-out-of-range";
    case VertexDecodeError::NonFiniteValue: return "non-finite-value";
    }
    return "unknown";
}

}
