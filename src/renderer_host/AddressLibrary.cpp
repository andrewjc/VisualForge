#include "renderer_host/AddressLibrary.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

namespace vf::renderer {

namespace {

constexpr std::size_t kHeaderSize = sizeof(std::uint64_t);
constexpr std::size_t kEntrySize = 2 * sizeof(std::uint64_t);

std::uint64_t ReadUint64(const std::byte* source) noexcept
{
    std::uint64_t value{};
    std::memcpy(&value, source, sizeof(value));
    return value;
}

}

AddressLibraryError AddressLibrary::Load(
    const std::span<const std::byte> bytes,
    const std::uint64_t imageSize) noexcept
{
    entries_.clear();
    if (bytes.size() < kHeaderSize) {
        return AddressLibraryError::TruncatedHeader;
    }
    const auto count = ReadUint64(bytes.data());
    if (count == 0) return AddressLibraryError::EmptyDatabase;
    if (count > (std::numeric_limits<std::size_t>::max() - kHeaderSize) /
            kEntrySize) {
        return AddressLibraryError::SizeMismatch;
    }
    const auto expected = kHeaderSize +
        static_cast<std::size_t>(count) * kEntrySize;
    if (bytes.size() != expected) return AddressLibraryError::SizeMismatch;

    try {
        std::vector<AddressLibraryEntry> candidate;
        candidate.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t index = 0; index < count; ++index) {
            const auto* record = bytes.data() + kHeaderSize +
                static_cast<std::size_t>(index) * kEntrySize;
            AddressLibraryEntry entry{};
            entry.id = ReadUint64(record);
            entry.offset = ReadUint64(record + sizeof(std::uint64_t));
            // Strictly ascending: equal ids would make a lookup ambiguous and
            // any other order would break the binary search.
            if (index != 0 && entry.id <= candidate.back().id) {
                return AddressLibraryError::UnsortedIds;
            }
            if (imageSize != 0 && entry.offset >= imageSize) {
                return AddressLibraryError::OffsetOutOfImage;
            }
            candidate.push_back(entry);
        }
        entries_ = std::move(candidate);
        return AddressLibraryError::None;
    } catch (...) {
        entries_.clear();
        return AddressLibraryError::AllocationFailure;
    }
}

AddressLibraryError AddressLibrary::LoadFromFile(
    const std::filesystem::path& path,
    const std::uint64_t imageSize) noexcept
{
    entries_.clear();
    try {
        std::error_code code;
        const auto size = std::filesystem::file_size(path, code);
        if (code || size > std::numeric_limits<std::size_t>::max()) {
            return AddressLibraryError::FileUnavailable;
        }
        std::ifstream stream{path, std::ios::binary};
        if (!stream) return AddressLibraryError::FileUnavailable;
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        if (!bytes.empty()) {
            stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            if (stream.gcount() !=
                static_cast<std::streamsize>(bytes.size())) {
                return AddressLibraryError::FileUnavailable;
            }
        }
        return Load(bytes, imageSize);
    } catch (...) {
        entries_.clear();
        return AddressLibraryError::AllocationFailure;
    }
}

AddressLibraryError AddressLibrary::Resolve(
    const std::uint64_t id,
    std::uint64_t& offset) const noexcept
{
    offset = 0;
    if (entries_.empty()) return AddressLibraryError::NotLoaded;
    const auto found = std::lower_bound(entries_.begin(), entries_.end(), id,
        [](const AddressLibraryEntry& entry, const std::uint64_t value) {
            return entry.id < value;
        });
    if (found == entries_.end() || found->id != id) {
        return AddressLibraryError::UnknownId;
    }
    offset = found->offset;
    return AddressLibraryError::None;
}

AddressLibraryError AddressLibrary::Identify(
    const std::uint64_t offset,
    std::uint64_t& id) const noexcept
{
    id = 0;
    if (entries_.empty()) return AddressLibraryError::NotLoaded;
    // Offsets are not sorted, so this is a scan. It exists for one-off
    // corroboration of a recorded constant, not for a hot path.
    const auto found = std::find_if(entries_.begin(), entries_.end(),
        [offset](const AddressLibraryEntry& entry) {
            return entry.offset == offset;
        });
    if (found == entries_.end()) return AddressLibraryError::UnknownId;
    id = found->id;
    return AddressLibraryError::None;
}

AddressLibraryError AddressLibrary::ResolveAddress(
    const std::uintptr_t imageBase,
    const std::uint64_t id,
    std::uintptr_t& address) const noexcept
{
    address = 0;
    if (imageBase == 0) return AddressLibraryError::NullImageBase;
    std::uint64_t offset = 0;
    const auto result = Resolve(id, offset);
    if (result != AddressLibraryError::None) return result;
    address = imageBase + static_cast<std::uintptr_t>(offset);
    return AddressLibraryError::None;
}

std::size_t AddressLibrary::Size() const noexcept
{
    return entries_.size();
}

bool AddressLibrary::Loaded() const noexcept
{
    return !entries_.empty();
}

const char* ToString(const AddressLibraryError error) noexcept
{
    switch (error) {
    case AddressLibraryError::None: return "none";
    case AddressLibraryError::NotLoaded: return "not loaded";
    case AddressLibraryError::FileUnavailable: return "file unavailable";
    case AddressLibraryError::TruncatedHeader: return "truncated header";
    case AddressLibraryError::EmptyDatabase: return "empty database";
    case AddressLibraryError::SizeMismatch: return "size mismatch";
    case AddressLibraryError::UnsortedIds: return "unsorted ids";
    case AddressLibraryError::OffsetOutOfImage: return "offset out of image";
    case AddressLibraryError::UnknownId: return "unknown id";
    case AddressLibraryError::NullImageBase: return "null image base";
    case AddressLibraryError::AllocationFailure: return "allocation failure";
    }
    return "unknown";
}

}
