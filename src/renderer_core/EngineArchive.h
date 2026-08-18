#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vf::renderer::archive {

// Bethesda's BA2, general form. `Fallout4 - Materials.ba2` is one of these:
// a fixed header, one 36-byte record per file, and a name table at the end
// holding the paths in the same order.
//
// Only GNRL is read. The other form, DX10, stores textures as a chain of
// per-mip chunks with a DDS header to be reconstructed, and nothing here
// needs one; refusing it is better than reading its records as if they had
// this shape, which they do not.
enum class ArchiveError : std::uint8_t
{
    None,
    TruncatedHeader,
    // Not `BTDX`.
    BadMagic,
    UnsupportedVersion,
    // A form other than GNRL, most likely DX10.
    UnsupportedType,
    TruncatedRecords,
    TruncatedNameTable,
    // A record whose data runs past the end of the archive.
    InvalidEntry,
    // The entry is compressed and its stream did not decode.
    DecompressionFailed,
    // The entry decompressed to a size other than the one it declared, which
    // means the record and the data disagree about what is stored.
    SizeMismatch,
};

struct ArchiveEntry
{
    // The full path as the archive spells it, backslash separated and lower
    // case, e.g. `materials\setdressing\barrel01.bgsm`.
    std::string name;
    std::uint64_t offset{};
    // Zero means the entry is stored uncompressed and `unpackedSize` is its
    // whole size. Otherwise it is the length of a zlib stream.
    std::uint32_t packedSize{};
    std::uint32_t unpackedSize{};
};

struct Archive
{
    std::uint32_t version{};
    std::vector<ArchiveEntry> entries;
};

// Reads the header, the records and the name table. The file data itself is
// not touched, so this is cheap on an archive of thousands of entries.
[[nodiscard]] ArchiveError ReadArchive(
    std::span<const std::byte> bytes,
    Archive& archive) noexcept;

// One entry's contents, decompressed if it is stored that way.
[[nodiscard]] ArchiveError ExtractEntry(
    std::span<const std::byte> bytes,
    const ArchiveEntry& entry,
    std::vector<std::byte>& output) noexcept;

[[nodiscard]] const char* ToString(ArchiveError error) noexcept;

}  // namespace vf::renderer::archive
