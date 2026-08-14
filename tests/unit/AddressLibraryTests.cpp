#include "renderer_host/AddressLibrary.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {

using namespace vf::renderer;

// The database is a flat, ID-sorted table: a u64 entry count followed by that
// many {u64 id, u64 rva} pairs. Building one here keeps the negative cases
// exact instead of depending on a corrupted copy of the real file.
std::vector<std::byte> BuildDatabase(
    const std::vector<AddressLibraryEntry>& entries)
{
    std::vector<std::byte> bytes(8 + entries.size() * 16);
    const auto count = static_cast<std::uint64_t>(entries.size());
    std::memcpy(bytes.data(), &count, sizeof(count));
    for (std::size_t index = 0; index < entries.size(); ++index) {
        std::memcpy(bytes.data() + 8 + index * 16,
            &entries[index].id, sizeof(std::uint64_t));
        std::memcpy(bytes.data() + 8 + index * 16 + 8,
            &entries[index].offset, sizeof(std::uint64_t));
    }
    return bytes;
}

constexpr std::uint64_t kImageSize = 0x0500'0000;

const std::vector<AddressLibraryEntry>& SampleEntries()
{
    static const std::vector<AddressLibraryEntry> entries{
        {15, 0x029B3ED8},
        {37, 0x02F027B8},
        {60, 0x02666600},
        {69, 0x00A12340},
        {2704621, 0x03D70920},
    };
    return entries;
}

}

TEST_CASE("P16_address_library_parses_the_sorted_id_table",
    "[phase16][addresslibrary]")
{
    const auto bytes = BuildDatabase(SampleEntries());
    AddressLibrary library;
    REQUIRE(library.Load(bytes, kImageSize) == AddressLibraryError::None);
    CHECK(library.Size() == SampleEntries().size());

    std::uint64_t offset = 0;
    REQUIRE(library.Resolve(15, offset) == AddressLibraryError::None);
    CHECK(offset == 0x029B3ED8);
    REQUIRE(library.Resolve(2704621, offset) == AddressLibraryError::None);
    CHECK(offset == 0x03D70920);
    // The table is searched, not scanned linearly from either end.
    REQUIRE(library.Resolve(60, offset) == AddressLibraryError::None);
    CHECK(offset == 0x02666600);

    CHECK(library.Resolve(16, offset) == AddressLibraryError::UnknownId);
    CHECK(library.Resolve(0, offset) == AddressLibraryError::UnknownId);
    CHECK(library.Resolve(2704622, offset) == AddressLibraryError::UnknownId);

    // The reverse direction is what lets a hand-recorded RVA be confirmed
    // against the database instead of being trusted on its own.
    std::uint64_t id = 0;
    REQUIRE(library.Identify(0x03D70920, id) == AddressLibraryError::None);
    CHECK(id == 2704621);
    CHECK(library.Identify(0x03D70921, id) == AddressLibraryError::UnknownId);
}

TEST_CASE("P16_address_library_rejects_malformed_databases",
    "[phase16][addresslibrary]")
{
    AddressLibrary library;
    std::array<std::byte, 4> tiny{};
    CHECK(library.Load(tiny, kImageSize) ==
        AddressLibraryError::TruncatedHeader);

    CHECK(library.Load(BuildDatabase({}), kImageSize) ==
        AddressLibraryError::EmptyDatabase);

    auto truncated = BuildDatabase(SampleEntries());
    truncated.resize(truncated.size() - 1);
    CHECK(library.Load(truncated, kImageSize) ==
        AddressLibraryError::SizeMismatch);

    auto oversized = BuildDatabase(SampleEntries());
    oversized.push_back(std::byte{0});
    CHECK(library.Load(oversized, kImageSize) ==
        AddressLibraryError::SizeMismatch);

    // A table that is not strictly ascending cannot be binary searched, and a
    // duplicate id would make a lookup ambiguous.
    auto unsorted = SampleEntries();
    std::swap(unsorted[1], unsorted[2]);
    CHECK(library.Load(BuildDatabase(unsorted), kImageSize) ==
        AddressLibraryError::UnsortedIds);

    auto duplicated = SampleEntries();
    duplicated[2].id = duplicated[1].id;
    CHECK(library.Load(BuildDatabase(duplicated), kImageSize) ==
        AddressLibraryError::UnsortedIds);

    // An offset outside the mapped image would resolve to a wild pointer.
    auto beyondImage = SampleEntries();
    beyondImage[3].offset = kImageSize;
    CHECK(library.Load(BuildDatabase(beyondImage), kImageSize) ==
        AddressLibraryError::OffsetOutOfImage);

    // A failed load must leave nothing resolvable behind.
    std::uint64_t offset = 0;
    CHECK(library.Size() == 0);
    CHECK(library.Resolve(15, offset) == AddressLibraryError::NotLoaded);
}

TEST_CASE("P16_address_library_resolves_against_the_loaded_image",
    "[phase16][addresslibrary]")
{
    const auto bytes = BuildDatabase(SampleEntries());
    AddressLibrary library;
    REQUIRE(library.Load(bytes, kImageSize) == AddressLibraryError::None);

    constexpr std::uintptr_t imageBase = 0x7FF6'0000'0000ull;
    std::uintptr_t address = 0;
    REQUIRE(library.ResolveAddress(imageBase, 2704621, address) ==
        AddressLibraryError::None);
    CHECK(address == imageBase + 0x03D70920);
    CHECK(library.ResolveAddress(0, 2704621, address) ==
        AddressLibraryError::NullImageBase);
}

#ifdef VF_GAME_ROOT
TEST_CASE("P16_address_library_reads_the_installed_database",
    "[phase16][addresslibrary][contract]")
{
    const std::filesystem::path root{VF_GAME_ROOT};
    const auto path = root / "Data" / "F4SE" / "Plugins" /
        "version-1-11-221-0.bin";
    if (!std::filesystem::exists(path)) {
        WARN("address library not installed; skipping installed-database case");
        return;
    }
    AddressLibrary library;
    // The real image is larger than the file on disk, so the bound has to be
    // the PE SizeOfImage rather than the file size.
    constexpr std::uint64_t installedImageSize = 0x0500'0000;
    REQUIRE(library.LoadFromFile(path, installedImageSize) ==
        AddressLibraryError::None);
    CHECK(library.Size() == 651'581);

    // The graphics-state RVA this project already relies on must be a real
    // entry, which corroborates both the parse and the recorded constant.
    std::uint64_t id = 0;
    REQUIRE(library.Identify(0x03D70920, id) == AddressLibraryError::None);
    CHECK(id == 2'704'621);
    std::uint64_t offset = 0;
    REQUIRE(library.Resolve(2'704'621, offset) == AddressLibraryError::None);
    CHECK(offset == 0x03D70920);
}
#endif
