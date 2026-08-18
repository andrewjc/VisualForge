#include "renderer_core/EngineArchive.h"

#include "renderer_core/EngineInflate.h"

#include <array>

namespace vf::renderer::archive {
namespace {

constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kRecordSize = 36;
constexpr std::uint32_t kSupportedVersion = 8;
// Version 1 shares this record layout and occurs in older archives; both are
// read the same way. Versions above 8 have not been seen and are refused
// rather than assumed compatible.
constexpr std::uint32_t kOldestVersion = 1;

[[nodiscard]] std::uint16_t ReadU16(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(
        static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8));
}

[[nodiscard]] std::uint32_t ReadU32(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept
{
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(bytes[offset + index]) <<
            (index * 8);
    }
    return value;
}

[[nodiscard]] std::uint64_t ReadU64(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) <<
            (index * 8);
    }
    return value;
}

[[nodiscard]] bool MatchesTag(
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    const char (&tag)[5]) noexcept
{
    for (std::size_t index = 0; index < 4; ++index) {
        if (static_cast<char>(bytes[offset + index]) != tag[index]) {
            return false;
        }
    }
    return true;
}

}  // namespace

ArchiveError ReadArchive(
    const std::span<const std::byte> bytes,
    Archive& archive) noexcept
{
    archive = {};
    if (bytes.size() < kHeaderSize) return ArchiveError::TruncatedHeader;
    if (!MatchesTag(bytes, 0, "BTDX")) return ArchiveError::BadMagic;
    const auto version = ReadU32(bytes, 4);
    if (version < kOldestVersion || version > kSupportedVersion) {
        return ArchiveError::UnsupportedVersion;
    }
    if (!MatchesTag(bytes, 8, "GNRL")) return ArchiveError::UnsupportedType;
    const auto fileCount = ReadU32(bytes, 12);
    const auto nameTableOffset = ReadU64(bytes, 16);

    const auto recordBytes =
        static_cast<std::uint64_t>(fileCount) * kRecordSize;
    if (recordBytes > bytes.size() - kHeaderSize) {
        return ArchiveError::TruncatedRecords;
    }
    if (nameTableOffset > bytes.size()) {
        return ArchiveError::TruncatedNameTable;
    }

    try {
        archive.version = version;
        archive.entries.resize(fileCount);
        for (std::uint32_t index = 0; index < fileCount; ++index) {
            const auto base = kHeaderSize +
                static_cast<std::size_t>(index) * kRecordSize;
            auto& entry = archive.entries[index];
            entry.offset = ReadU64(bytes, base + 16);
            entry.packedSize = ReadU32(bytes, base + 24);
            entry.unpackedSize = ReadU32(bytes, base + 28);
        }

        // The names live in their own stream and are matched to the records
        // by order alone. Walking one without the other yields plausible
        // names attached to the wrong data, which is why the count is
        // required to match rather than taken as far as it goes.
        auto cursor = static_cast<std::size_t>(nameTableOffset);
        for (std::uint32_t index = 0; index < fileCount; ++index) {
            if (cursor + 2 > bytes.size()) {
                return ArchiveError::TruncatedNameTable;
            }
            const auto length = ReadU16(bytes, cursor);
            cursor += 2;
            if (cursor + length > bytes.size()) {
                return ArchiveError::TruncatedNameTable;
            }
            auto& name = archive.entries[index].name;
            name.resize(length);
            for (std::size_t character = 0; character < length; ++character) {
                name[character] =
                    static_cast<char>(bytes[cursor + character]);
            }
            cursor += length;
        }
    } catch (...) {
        archive = {};
        return ArchiveError::TruncatedRecords;
    }
    return ArchiveError::None;
}

ArchiveError ExtractEntry(
    const std::span<const std::byte> bytes,
    const ArchiveEntry& entry,
    std::vector<std::byte>& output) noexcept
{
    output.clear();
    const auto stored = entry.packedSize == 0;
    const auto storedSize = stored ? entry.unpackedSize : entry.packedSize;
    if (entry.offset > bytes.size() ||
        storedSize > bytes.size() - entry.offset) {
        return ArchiveError::InvalidEntry;
    }
    const auto payload = bytes.subspan(
        static_cast<std::size_t>(entry.offset), storedSize);
    if (stored) {
        try {
            output.assign(payload.begin(), payload.end());
        } catch (...) {
            return ArchiveError::DecompressionFailed;
        }
        return ArchiveError::None;
    }
    if (compress::InflateZlib(payload, output) !=
        compress::InflateError::None) {
        output.clear();
        return ArchiveError::DecompressionFailed;
    }
    // The record and the stream have to agree about the size. When they do
    // not, one of them describes a different file and neither can be trusted.
    if (output.size() != entry.unpackedSize) {
        output.clear();
        return ArchiveError::SizeMismatch;
    }
    return ArchiveError::None;
}

const char* ToString(const ArchiveError error) noexcept
{
    switch (error) {
    case ArchiveError::None: return "none";
    case ArchiveError::TruncatedHeader: return "truncated header";
    case ArchiveError::BadMagic: return "bad magic";
    case ArchiveError::UnsupportedVersion: return "unsupported version";
    case ArchiveError::UnsupportedType: return "unsupported type";
    case ArchiveError::TruncatedRecords: return "truncated records";
    case ArchiveError::TruncatedNameTable: return "truncated name table";
    case ArchiveError::InvalidEntry: return "invalid entry";
    case ArchiveError::DecompressionFailed: return "decompression failed";
    case ArchiveError::SizeMismatch: return "size mismatch";
    }
    return "unknown";
}

}  // namespace vf::renderer::archive
