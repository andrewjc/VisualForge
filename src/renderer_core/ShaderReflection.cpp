#include "ShaderReflection.h"

#include <cstring>

namespace vf::renderer::shader {
namespace {

constexpr std::uint32_t kContainerMagic = 0x43425844u;   // 'DXBC'
constexpr std::uint32_t kReflectionMagic = 0x46454452u;  // 'RDEF'

// 'DXBC', a 16-byte digest, a version pair, the total size, and the chunk
// count, followed by one offset per chunk.
constexpr std::size_t kContainerHeaderSize = 32;
constexpr std::size_t kChunkHeaderSize = 8;

// The reflection header: buffer count and offset, bound-resource count and
// offset, the shader version, flags, and the creator string offset.
constexpr std::size_t kReflectionHeaderSize = 28;
constexpr std::size_t kBufferEntrySize = 24;

// Shader model 5 appends four texture and sampler fields to each variable
// entry. The count is the only thing that moves, so the stride is the only
// thing this parser needs to know about the difference.
constexpr std::size_t kVariableEntrySizeSm4 = 24;
constexpr std::size_t kVariableEntrySizeSm5 = 40;

[[nodiscard]] std::uint32_t ReadU32(
    std::span<const std::byte> bytes, const std::size_t offset) noexcept
{
    std::uint32_t value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

// Reads a NUL-terminated name that must lie wholly inside the chunk.
//
// An unterminated name is a refusal, not a truncation: a name assembled from
// whatever follows the chunk is indistinguishable from a real one at the point
// where it would be read and believed.
[[nodiscard]] bool ReadName(
    std::span<const std::byte> chunk,
    const std::uint32_t offset,
    std::string& name)
{
    // Only `>` is load-bearing; without it the subtraction below wraps and
    // memchr runs off the chunk. The `=` costs nothing and is not separately
    // observable -- an offset exactly at the end yields a zero-length search
    // that finds no terminator and refuses by the other path.
    if (offset >= chunk.size()) {
        return false;
    }
    const auto* const begin = reinterpret_cast<const char*>(chunk.data()) + offset;
    const std::size_t available = chunk.size() - offset;
    const void* const terminator = std::memchr(begin, '\0', available);
    if (terminator == nullptr) {
        return false;
    }
    name.assign(begin, static_cast<const char*>(terminator) - begin);
    return true;
}

// True when reading `count` records of `stride` bytes starting at `offset`
// would leave the chunk. Multiplication happens only after the count is known
// to be small enough that the product cannot wrap.
[[nodiscard]] bool RegionOutOfBounds(
    const std::size_t chunkSize,
    const std::uint32_t offset,
    const std::uint32_t count,
    const std::size_t stride) noexcept
{
    if (offset > chunkSize) {
        return true;
    }
    const std::size_t available = chunkSize - offset;
    if (count > available / stride) {
        return true;
    }
    return false;
}

// D3D_SHADER_INPUT_TYPE. Only the values this reader distinguishes are named;
// everything else (constant buffers, UAVs, structured buffers) is kept as
// ResourceKind::Other rather than dropped, because a resource this reader
// does not care about is still a real declaration and undercounting the
// table is its own kind of wrong answer.
constexpr std::uint32_t kShaderInputTypeTexture = 2;
constexpr std::uint32_t kShaderInputTypeSampler = 3;

// A D3D11_SHADER_INPUT_BIND_DESC record: name offset, type, return type,
// dimension, sample count, bind point, bind count, flags. Unlike constant
// buffer variables this record does not grow between SM4 and SM5.
constexpr std::size_t kResourceEntrySize = 32;

[[nodiscard]] ReflectionError ReadReflectionChunk(
    std::span<const std::byte> chunk, ReflectedShader& reflection)
{
    if (chunk.size() < kReflectionHeaderSize) {
        return ReflectionError::TruncatedChunk;
    }

    const std::uint32_t bufferCount = ReadU32(chunk, 0);
    const std::uint32_t bufferOffset = ReadU32(chunk, 4);
    const std::uint32_t resourceCount = ReadU32(chunk, 8);
    const std::uint32_t resourceOffset = ReadU32(chunk, 12);
    const auto majorVersion = static_cast<std::uint8_t>(chunk[17]);

    if (RegionOutOfBounds(chunk.size(), bufferOffset, bufferCount, kBufferEntrySize)) {
        return ReflectionError::InvalidOffset;
    }
    if (RegionOutOfBounds(
            chunk.size(), resourceOffset, resourceCount, kResourceEntrySize)) {
        return ReflectionError::InvalidOffset;
    }

    reflection.resources.clear();
    reflection.resources.reserve(resourceCount);
    for (std::uint32_t index = 0; index < resourceCount; ++index) {
        const std::size_t entry =
            static_cast<std::size_t>(resourceOffset) + index * kResourceEntrySize;

        ReflectedResource resource{};
        if (!ReadName(chunk, ReadU32(chunk, entry), resource.name)) {
            return ReflectionError::InvalidOffset;
        }
        const auto type = ReadU32(chunk, entry + 4);
        resource.kind = type == kShaderInputTypeTexture ? ResourceKind::Texture
            : type == kShaderInputTypeSampler            ? ResourceKind::Sampler
                                                           : ResourceKind::Other;
        resource.bindPoint = ReadU32(chunk, entry + 20);
        resource.bindCount = ReadU32(chunk, entry + 24);
        reflection.resources.push_back(std::move(resource));
    }

    const std::size_t variableStride =
        majorVersion >= 5 ? kVariableEntrySizeSm5 : kVariableEntrySizeSm4;

    reflection.buffers.clear();
    reflection.buffers.reserve(bufferCount);

    for (std::uint32_t index = 0; index < bufferCount; ++index) {
        const std::size_t entry =
            static_cast<std::size_t>(bufferOffset) + index * kBufferEntrySize;

        ReflectedBuffer buffer{};
        if (!ReadName(chunk, ReadU32(chunk, entry), buffer.name)) {
            return ReflectionError::InvalidOffset;
        }
        const std::uint32_t variableCount = ReadU32(chunk, entry + 4);
        const std::uint32_t variableOffset = ReadU32(chunk, entry + 8);
        buffer.size = ReadU32(chunk, entry + 12);

        if (RegionOutOfBounds(chunk.size(), variableOffset, variableCount, variableStride)) {
            return ReflectionError::InvalidOffset;
        }

        buffer.variables.reserve(variableCount);
        for (std::uint32_t slot = 0; slot < variableCount; ++slot) {
            const std::size_t record =
                static_cast<std::size_t>(variableOffset) + slot * variableStride;

            ReflectedVariable variable{};
            if (!ReadName(chunk, ReadU32(chunk, record), variable.name)) {
                return ReflectionError::InvalidOffset;
            }
            variable.offset = ReadU32(chunk, record + 4);
            variable.size = ReadU32(chunk, record + 8);
            buffer.variables.push_back(std::move(variable));
        }

        reflection.buffers.push_back(std::move(buffer));
    }

    return ReflectionError::None;
}

}

ReflectionError ReflectShader(
    std::span<const std::byte> bytecode, ReflectedShader& reflection) noexcept
try {
    if (bytecode.size() < kContainerHeaderSize) {
        return ReflectionError::TruncatedContainer;
    }
    if (ReadU32(bytecode, 0) != kContainerMagic) {
        return ReflectionError::BadMagic;
    }

    const std::uint32_t totalSize = ReadU32(bytecode, 24);
    if (totalSize != bytecode.size()) {
        return ReflectionError::TruncatedContainer;
    }

    const std::uint32_t chunkCount = ReadU32(bytecode, 28);
    if (RegionOutOfBounds(
            bytecode.size(), kContainerHeaderSize, chunkCount, sizeof(std::uint32_t))) {
        return ReflectionError::TruncatedContainer;
    }

    for (std::uint32_t index = 0; index < chunkCount; ++index) {
        const std::uint32_t chunkOffset =
            ReadU32(bytecode, kContainerHeaderSize + index * sizeof(std::uint32_t));
        if (chunkOffset > bytecode.size() - kChunkHeaderSize) {
            return ReflectionError::InvalidOffset;
        }
        if (ReadU32(bytecode, chunkOffset) != kReflectionMagic) {
            continue;
        }

        const std::uint32_t chunkSize = ReadU32(bytecode, chunkOffset + 4);
        const std::size_t body = static_cast<std::size_t>(chunkOffset) + kChunkHeaderSize;
        if (chunkSize > bytecode.size() - body) {
            return ReflectionError::TruncatedChunk;
        }
        return ReadReflectionChunk(bytecode.subspan(body, chunkSize), reflection);
    }

    return ReflectionError::MissingReflectionChunk;
} catch (...) {
    return ReflectionError::TruncatedChunk;
}

const char* ToString(const ReflectionError error) noexcept
{
    switch (error) {
    case ReflectionError::None: return "none";
    case ReflectionError::TruncatedContainer: return "truncated-container";
    case ReflectionError::BadMagic: return "bad-magic";
    case ReflectionError::UnsupportedVersion: return "unsupported-version";
    case ReflectionError::MissingReflectionChunk: return "missing-reflection-chunk";
    case ReflectionError::TruncatedChunk: return "truncated-chunk";
    case ReflectionError::InvalidOffset: return "invalid-offset";
    }
    return "unknown";
}

}
