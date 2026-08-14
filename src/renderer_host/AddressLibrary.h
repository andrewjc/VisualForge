#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace vf::renderer {

// The installed Address Library database for a build. The file is a flat,
// ID-sorted table: a u64 entry count followed by that many {u64 id, u64 rva}
// pairs. Resolving engine addresses through stable IDs is what lets this
// project stop hand-recording RVAs per build.
struct AddressLibraryEntry
{
    std::uint64_t id{};
    std::uint64_t offset{};
};

enum class AddressLibraryError : std::uint8_t
{
    None,
    NotLoaded,
    FileUnavailable,
    TruncatedHeader,
    EmptyDatabase,
    SizeMismatch,
    // Strictly ascending ids are what make the table binary searchable and
    // make a lookup unambiguous, so a duplicate reports the same way.
    UnsortedIds,
    OffsetOutOfImage,
    UnknownId,
    NullImageBase,
    AllocationFailure,
};

class AddressLibrary
{
public:
    [[nodiscard]] AddressLibraryError Load(
        std::span<const std::byte> bytes,
        std::uint64_t imageSize) noexcept;
    [[nodiscard]] AddressLibraryError LoadFromFile(
        const std::filesystem::path& path,
        std::uint64_t imageSize) noexcept;
    [[nodiscard]] AddressLibraryError Resolve(
        std::uint64_t id,
        std::uint64_t& offset) const noexcept;
    // Reverse lookup. A hand-recorded RVA that names no id is either wrong or
    // from a different build, so this is how a recorded constant gets
    // corroborated instead of trusted on its own.
    [[nodiscard]] AddressLibraryError Identify(
        std::uint64_t offset,
        std::uint64_t& id) const noexcept;
    [[nodiscard]] AddressLibraryError ResolveAddress(
        std::uintptr_t imageBase,
        std::uint64_t id,
        std::uintptr_t& address) const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] bool Loaded() const noexcept;

private:
    std::vector<AddressLibraryEntry> entries_;
};

[[nodiscard]] const char* ToString(AddressLibraryError error) noexcept;

}
