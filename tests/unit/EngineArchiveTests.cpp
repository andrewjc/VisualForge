#include "renderer_core/EngineArchive.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace vf::renderer;

namespace {

void AppendU32(std::vector<std::byte>& bytes, const std::uint32_t value)
{
    for (std::size_t shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
}

void AppendU64(std::vector<std::byte>& bytes, const std::uint64_t value)
{
    for (std::size_t shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
}

void AppendChars(std::vector<std::byte>& bytes, const std::string& text)
{
    for (const auto character : text) {
        bytes.push_back(static_cast<std::byte>(character));
    }
}

// The zlib stream from the inflate tests, so the archive path is exercised
// against a stream produced by an independent compressor rather than one this
// codebase also wrote.
const std::array<std::uint8_t, 59> kZlibStream{
    0x78, 0x9c, 0x2b, 0xc9, 0x48, 0x55, 0x28, 0x2c, 0xcd, 0x4c, 0xce, 0x56,
    0x48, 0x2a, 0xca, 0x2f, 0xcf, 0x53, 0x48, 0xcb, 0xaf, 0x50, 0xc8, 0x2a,
    0xcd, 0x2d, 0x28, 0x56, 0xc8, 0x2f, 0x4b, 0x2d, 0x52, 0x28, 0xc9, 0x48,
    0x55, 0xc8, 0x49, 0xac, 0xaa, 0x54, 0x48, 0xc9, 0x4f, 0xd7, 0x03, 0xf3,
    0x88, 0x56, 0xec, 0x48, 0x00, 0x00, 0x00, 0x78, 0x3b, 0x28, 0xaf};

constexpr std::uint32_t kZlibUnpackedSize = 122;

// A GNRL archive holding two entries: one stored, one compressed. Built here
// rather than shipped as a binary so the layout under test is spelled out --
// a record is 36 bytes and the field order is the thing most easily got
// wrong.
std::vector<std::byte> BuildArchive(const std::string& stored)
{
    constexpr std::uint32_t kFileCount = 2;
    constexpr std::size_t kHeaderSize = 24;
    constexpr std::size_t kRecordSize = 36;
    const std::size_t dataOffset = kHeaderSize + kRecordSize * kFileCount;
    const auto storedOffset = dataOffset;
    const auto compressedOffset = storedOffset + stored.size();
    const auto nameTableOffset = compressedOffset + kZlibStream.size();

    std::vector<std::byte> bytes;
    AppendChars(bytes, "BTDX");
    AppendU32(bytes, 8);
    AppendChars(bytes, "GNRL");
    AppendU32(bytes, kFileCount);
    AppendU64(bytes, nameTableOffset);

    // Record one: stored, so its packed size is zero.
    AppendU32(bytes, 0x11111111);
    AppendChars(bytes, "bgsm");
    AppendU32(bytes, 0x22222222);
    AppendU32(bytes, 0);
    AppendU64(bytes, storedOffset);
    AppendU32(bytes, 0);
    AppendU32(bytes, static_cast<std::uint32_t>(stored.size()));
    AppendU32(bytes, 0xBAADF00D);

    // Record two: a zlib stream, so its packed size is the stream length.
    AppendU32(bytes, 0x33333333);
    AppendChars(bytes, "bgem");
    AppendU32(bytes, 0x44444444);
    AppendU32(bytes, 0);
    AppendU64(bytes, compressedOffset);
    AppendU32(bytes, static_cast<std::uint32_t>(kZlibStream.size()));
    AppendU32(bytes, kZlibUnpackedSize);
    AppendU32(bytes, 0xBAADF00D);

    AppendChars(bytes, stored);
    for (const auto value : kZlibStream) {
        bytes.push_back(static_cast<std::byte>(value));
    }

    // The name table: a sixteen-bit length and that many characters, with no
    // terminator, in record order.
    const std::array<std::string, 2> names{
        "materials\\setdressing\\barrel01.bgsm",
        "materials\\effects\\glow01.bgem"};
    for (const auto& name : names) {
        const auto length = static_cast<std::uint16_t>(name.size());
        bytes.push_back(static_cast<std::byte>(length & 0xFFu));
        bytes.push_back(static_cast<std::byte>((length >> 8) & 0xFFu));
        AppendChars(bytes, name);
    }
    return bytes;
}

}  // namespace

TEST_CASE("P16_a_general_archive_lists_its_entries", "[phase16][archive]")
{
    const std::string stored = "not compressed at all";
    const auto bytes = BuildArchive(stored);
    archive::Archive read;
    REQUIRE(archive::ReadArchive(bytes, read) == archive::ArchiveError::None);
    REQUIRE(read.entries.size() == 2);
    // The name table is a separate stream from the records and is matched to
    // them by order alone, so a reader that walks one and not the other
    // produces plausible names against the wrong data.
    CHECK(read.entries[0].name == "materials\\setdressing\\barrel01.bgsm");
    CHECK(read.entries[1].name == "materials\\effects\\glow01.bgem");
    CHECK(read.entries[0].packedSize == 0);
    CHECK(read.entries[0].unpackedSize == stored.size());
    CHECK(read.entries[1].packedSize == kZlibStream.size());
    CHECK(read.entries[1].unpackedSize == kZlibUnpackedSize);
}

TEST_CASE("P16_a_general_archive_extracts_both_storage_forms",
    "[phase16][archive]")
{
    const std::string stored = "not compressed at all";
    const auto bytes = BuildArchive(stored);
    archive::Archive read;
    REQUIRE(archive::ReadArchive(bytes, read) == archive::ArchiveError::None);

    std::vector<std::byte> output;
    REQUIRE(archive::ExtractEntry(bytes, read.entries[0], output) ==
        archive::ArchiveError::None);
    CHECK(std::string{reinterpret_cast<const char*>(output.data()),
        output.size()} == stored);

    REQUIRE(archive::ExtractEntry(bytes, read.entries[1], output) ==
        archive::ArchiveError::None);
    CHECK(output.size() == kZlibUnpackedSize);
    CHECK(std::string{reinterpret_cast<const char*>(output.data()), 19} ==
        "the quick brown fox");
}

TEST_CASE("P16_a_general_archive_refuses_what_it_cannot_read",
    "[phase16][archive]")
{
    const std::string stored = "not compressed at all";
    const auto bytes = BuildArchive(stored);

    archive::Archive read;
    auto wrongMagic = bytes;
    wrongMagic[0] = static_cast<std::byte>('X');
    CHECK(archive::ReadArchive(wrongMagic, read) ==
        archive::ArchiveError::BadMagic);

    // DX10 stores per-mip chunks and a DDS header to rebuild. Its records do
    // not have this shape, so reading them as if they did would produce
    // offsets into the middle of texture data.
    auto textureArchive = bytes;
    textureArchive[8] = static_cast<std::byte>('D');
    textureArchive[9] = static_cast<std::byte>('X');
    textureArchive[10] = static_cast<std::byte>('1');
    textureArchive[11] = static_cast<std::byte>('0');
    CHECK(archive::ReadArchive(textureArchive, read) ==
        archive::ArchiveError::UnsupportedType);

    const std::span<const std::byte> truncated{bytes.data(), 20};
    CHECK(archive::ReadArchive(truncated, read) ==
        archive::ArchiveError::TruncatedHeader);

    // An entry whose declared size disagrees with what its stream decodes to
    // is a record and a payload describing different files.
    REQUIRE(archive::ReadArchive(bytes, read) == archive::ArchiveError::None);
    auto lying = read.entries[1];
    lying.unpackedSize = kZlibUnpackedSize + 1;
    std::vector<std::byte> output;
    CHECK(archive::ExtractEntry(bytes, lying, output) ==
        archive::ArchiveError::SizeMismatch);

    auto pastTheEnd = read.entries[0];
    pastTheEnd.offset = bytes.size() - 2;
    pastTheEnd.unpackedSize = 64;
    CHECK(archive::ExtractEntry(bytes, pastTheEnd, output) ==
        archive::ArchiveError::InvalidEntry);
}
