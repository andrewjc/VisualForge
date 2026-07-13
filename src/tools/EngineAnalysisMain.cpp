// Offline static analysis of the target executable. Everything here reads a
// file on disk; nothing attaches to, or runs alongside, a live game.
//
// The discoveries this produces are recorded in docs/engine_render.md. Keeping
// the tool in the repository is what makes those recorded addresses
// reproducible instead of being trusted because a one-off script said so.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Section
{
    std::string name;
    std::uint32_t virtualAddress{};
    std::uint32_t virtualSize{};
    std::uint32_t rawPointer{};
    std::uint32_t rawSize{};
    std::uint32_t characteristics{};

    [[nodiscard]] bool Executable() const noexcept
    {
        return (characteristics & 0x20000000u) != 0;
    }
};

class PeImage
{
public:
    [[nodiscard]] bool Load(const std::filesystem::path& path)
    {
        std::error_code code;
        const auto size = std::filesystem::file_size(path, code);
        if (code) return false;
        std::ifstream stream{path, std::ios::binary};
        if (!stream) return false;
        bytes_.resize(static_cast<std::size_t>(size));
        stream.read(reinterpret_cast<char*>(bytes_.data()),
            static_cast<std::streamsize>(bytes_.size()));
        if (static_cast<std::size_t>(stream.gcount()) != bytes_.size()) {
            return false;
        }
        return ParseHeaders();
    }

    [[nodiscard]] std::uint64_t ImageBase() const noexcept { return imageBase_; }
    [[nodiscard]] std::uint32_t SizeOfImage() const noexcept
    {
        return sizeOfImage_;
    }
    [[nodiscard]] const std::vector<Section>& Sections() const noexcept
    {
        return sections_;
    }

    // Only the part of a section that exists on disk can be read; the rest is
    // zero-filled at load time and has no file bytes to inspect.
    [[nodiscard]] std::optional<std::size_t> RvaToFile(
        const std::uint64_t rva) const noexcept
    {
        for (const auto& section : sections_) {
            if (rva < section.virtualAddress) continue;
            const auto delta = rva - section.virtualAddress;
            if (delta >= section.rawSize) continue;
            return static_cast<std::size_t>(section.rawPointer + delta);
        }
        return std::nullopt;
    }

    [[nodiscard]] const Section* FindSection(
        const std::string_view name) const noexcept
    {
        const auto found = std::find_if(sections_.begin(), sections_.end(),
            [name](const Section& section) { return section.name == name; });
        return found == sections_.end() ? nullptr : &*found;
    }

    [[nodiscard]] std::uint64_t ReadUint64(const std::size_t offset) const
    {
        std::uint64_t value{};
        std::memcpy(&value, bytes_.data() + offset, sizeof(value));
        return value;
    }

    [[nodiscard]] std::uint32_t ReadUint32(const std::size_t offset) const
    {
        std::uint32_t value{};
        std::memcpy(&value, bytes_.data() + offset, sizeof(value));
        return value;
    }

    [[nodiscard]] std::int32_t ReadInt32(const std::size_t offset) const
    {
        std::int32_t value{};
        std::memcpy(&value, bytes_.data() + offset, sizeof(value));
        return value;
    }

    // Returns nullopt when the bytes are not a printable, NUL-terminated
    // string, which is how a false anchor gets rejected.
    [[nodiscard]] std::optional<std::string> ReadString(
        const std::uint64_t rva,
        const std::size_t limit = 128) const
    {
        const auto offset = RvaToFile(rva);
        if (!offset) return std::nullopt;
        std::string text;
        for (std::size_t index = 0; index < limit; ++index) {
            const auto position = *offset + index;
            if (position >= bytes_.size()) return std::nullopt;
            const auto value = static_cast<unsigned char>(bytes_[position]);
            if (value == 0) return text;
            if (value < 0x20 || value > 0x7E) return std::nullopt;
            text.push_back(static_cast<char>(value));
        }
        return std::nullopt;
    }

    [[nodiscard]] const std::vector<std::byte>& Bytes() const noexcept
    {
        return bytes_;
    }

private:
    [[nodiscard]] bool ParseHeaders()
    {
        if (bytes_.size() < 0x40) return false;
        if (ReadUint32(0) % 0x10000 != 0x5A4D) return false;   // 'MZ'
        const auto ntOffset = static_cast<std::size_t>(ReadUint32(0x3C));
        if (ntOffset + 0x108 > bytes_.size()) return false;
        if (ReadUint32(ntOffset) != 0x00004550) return false;  // 'PE\0\0'
        const auto sectionCount = ReadUint32(ntOffset + 6) & 0xFFFFu;
        // SizeOfOptionalHeader is the low half of the dword at +20; the high
        // half is Characteristics.
        const auto optionalSize = ReadUint32(ntOffset + 20) & 0xFFFFu;
        if (ReadUint32(ntOffset + 24) % 0x10000 != 0x20B) return false;
        imageBase_ = ReadUint64(ntOffset + 24 + 24);
        sizeOfImage_ = ReadUint32(ntOffset + 24 + 56);
        const auto tableOffset = ntOffset + 24 + optionalSize;
        for (std::uint32_t index = 0; index < sectionCount; ++index) {
            const auto entry = tableOffset + index * 40;
            if (entry + 40 > bytes_.size()) return false;
            Section section;
            const auto* raw = reinterpret_cast<const char*>(
                bytes_.data() + entry);
            section.name.assign(raw, strnlen(raw, 8));
            section.virtualSize = ReadUint32(entry + 8);
            section.virtualAddress = ReadUint32(entry + 12);
            section.rawSize = ReadUint32(entry + 16);
            section.rawPointer = ReadUint32(entry + 20);
            section.characteristics = ReadUint32(entry + 36);
            sections_.push_back(std::move(section));
        }
        return !sections_.empty();
    }

    std::vector<std::byte> bytes_;
    std::vector<Section> sections_;
    std::uint64_t imageBase_{};
    std::uint32_t sizeOfImage_{};
};

// The console command table's SCRIPT_FUNCTION record, mapped in
// docs/engine_render.md section 13.1.
constexpr std::uint32_t kRecordSize = 0x50;
constexpr std::uint32_t kLongNameOffset = 0x00;
constexpr std::uint32_t kShortNameOffset = 0x08;
constexpr std::uint32_t kOpcodeOffset = 0x10;
constexpr std::uint32_t kHelpOffset = 0x18;
constexpr std::uint32_t kParametersOffset = 0x28;
constexpr std::uint32_t kExecuteOffset = 0x30;
constexpr std::uint32_t kCompileOffset = 0x38;

struct CommandRecord
{
    std::uint64_t recordRva{};
    std::string longName;
    std::string shortName;
    std::uint32_t opcode{};
    std::uint64_t executeRva{};
    std::uint64_t compileRva{};
    std::uint64_t parametersRva{};
    std::string help;
};

[[nodiscard]] bool PointsIntoImage(
    const PeImage& image,
    const std::uint64_t pointer,
    std::uint64_t& rva) noexcept
{
    if (pointer < image.ImageBase()) return false;
    const auto candidate = pointer - image.ImageBase();
    if (candidate >= image.SizeOfImage()) return false;
    rva = candidate;
    return true;
}

[[nodiscard]] bool ReadCommandRecord(
    const PeImage& image,
    const std::uint64_t recordRva,
    CommandRecord& record)
{
    const auto offset = image.RvaToFile(recordRva);
    if (!offset) return false;
    std::uint64_t longRva = 0;
    std::uint64_t executeRva = 0;
    if (!PointsIntoImage(image,
            image.ReadUint64(*offset + kLongNameOffset), longRva) ||
        !PointsIntoImage(image,
            image.ReadUint64(*offset + kExecuteOffset), executeRva)) {
        return false;
    }
    const auto longName = image.ReadString(longRva);
    if (!longName || longName->size() < 2) return false;
    // The execute pointer has to land in code, which is what separates a real
    // record from an unrelated run of pointers.
    const auto containing = std::find_if(image.Sections().begin(),
        image.Sections().end(), [executeRva](const Section& candidate) {
            return executeRva >= candidate.virtualAddress &&
                executeRva < candidate.virtualAddress + candidate.virtualSize;
        });
    if (containing == image.Sections().end() || !containing->Executable()) {
        return false;
    }

    record = {};
    record.recordRva = recordRva;
    record.longName = *longName;
    record.opcode = image.ReadUint32(*offset + kOpcodeOffset);
    record.executeRva = executeRva;
    std::uint64_t shortRva = 0;
    if (PointsIntoImage(image,
            image.ReadUint64(*offset + kShortNameOffset), shortRva)) {
        if (const auto text = image.ReadString(shortRva)) {
            record.shortName = *text;
        }
    }
    std::uint64_t helpRva = 0;
    if (PointsIntoImage(image,
            image.ReadUint64(*offset + kHelpOffset), helpRva)) {
        if (const auto text = image.ReadString(helpRva)) record.help = *text;
    }
    // Both are optional: a record may legitimately carry neither, so a
    // failed resolve simply leaves the field zero.
    static_cast<void>(PointsIntoImage(image,
        image.ReadUint64(*offset + kCompileOffset), record.compileRva));
    static_cast<void>(PointsIntoImage(image,
        image.ReadUint64(*offset + kParametersOffset), record.parametersRva));
    return true;
}

// Walks outward from a record known to be inside the table until the records
// stop validating, so the extent is measured rather than assumed.
[[nodiscard]] std::vector<CommandRecord> WalkCommandTable(
    const PeImage& image,
    const std::uint64_t seedRva)
{
    CommandRecord seed;
    if (!ReadCommandRecord(image, seedRva, seed)) return {};
    auto first = seedRva;
    while (first >= kRecordSize) {
        CommandRecord probe;
        if (!ReadCommandRecord(image, first - kRecordSize, probe)) break;
        first -= kRecordSize;
    }
    std::vector<CommandRecord> records;
    for (auto rva = first;; rva += kRecordSize) {
        CommandRecord record;
        if (!ReadCommandRecord(image, rva, record)) break;
        records.push_back(std::move(record));
    }
    return records;
}

struct LeaReference
{
    std::uint64_t instructionRva{};
    std::uint64_t targetRva{};
};

// Finds RIP-relative `lea reg, [rip+disp32]` whose target lands in a range.
// This is how a data structure's users are located without a disassembler:
// the code never stores the absolute address, only a displacement.
[[nodiscard]] std::vector<LeaReference> ScanLeaReferences(
    const PeImage& image,
    const std::uint64_t low,
    const std::uint64_t high)
{
    std::vector<LeaReference> references;
    for (const auto& section : image.Sections()) {
        if (!section.Executable()) continue;
        const auto* data = image.Bytes().data() + section.rawPointer;
        const auto size = static_cast<std::size_t>(std::min(
            section.rawSize, section.virtualSize));
        if (size < 7) continue;
        for (std::size_t index = 0; index + 7 <= size; ++index) {
            const auto rex = static_cast<unsigned char>(data[index]);
            if (rex != 0x48 && rex != 0x4C) continue;
            if (static_cast<unsigned char>(data[index + 1]) != 0x8D) continue;
            const auto modrm = static_cast<unsigned char>(data[index + 2]);
            // mod == 00 and rm == 101 is the RIP-relative form.
            if ((modrm & 0xC7) != 0x05) continue;
            std::int32_t displacement{};
            std::memcpy(&displacement, data + index + 3, sizeof(displacement));
            const auto instruction = section.virtualAddress + index;
            const auto target = static_cast<std::int64_t>(instruction) + 7 +
                displacement;
            if (target < 0) continue;
            const auto unsignedTarget = static_cast<std::uint64_t>(target);
            if (unsignedTarget < low || unsignedTarget >= high) continue;
            references.push_back({instruction, unsignedTarget});
        }
    }
    return references;
}

// Any 8-byte value in the file whose value lands in an address range. Finds
// absolute pointers wherever they live, including `mov reg, imm64` operands
// inside code, which a RIP-relative scan cannot see.
[[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>>
ScanAbsolutePointers(
    const PeImage& image,
    const std::uint64_t low,
    const std::uint64_t high)
{
    std::vector<std::pair<std::uint64_t, std::uint64_t>> found;
    const auto lowValue = image.ImageBase() + low;
    const auto highValue = image.ImageBase() + high;
    for (const auto& section : image.Sections()) {
        const auto size = static_cast<std::size_t>(std::min(
            section.rawSize, section.virtualSize));
        if (size < 8) continue;
        const auto* data = image.Bytes().data() + section.rawPointer;
        for (std::size_t index = 0; index + 8 <= size; ++index) {
            std::uint64_t value{};
            std::memcpy(&value, data + index, sizeof(value));
            if (value < lowValue || value >= highValue) continue;
            found.emplace_back(section.virtualAddress + index,
                value - image.ImageBase());
        }
    }
    return found;
}

// MSVC RTTI reconstruction. This is the version-independent way to identify a
// class in the image: the mangled name is a literal string, and the chain
// name -> TypeDescriptor -> CompleteObjectLocator -> vtable is all structural.
// No address-library id is involved, which matters because id spaces are an
// artifact of one generation run and are not portable between builds.
struct RttiClass
{
    std::string name;
    std::uint64_t typeDescriptorRva{};
    std::uint64_t locatorRva{};
    std::uint64_t vtableRva{};
    std::uint32_t vtableSlots{};
};

// TypeDescriptor: { void* pVFTable; void* spare; char name[]; }
constexpr std::uint32_t kTypeDescriptorNameOffset = 0x10;
// x64 RTTICompleteObjectLocator: signature, offset, cdOffset, then
// image-relative pTypeDescriptor, pClassDescriptor, pSelf.
constexpr std::uint32_t kLocatorTypeDescriptorOffset = 0x0C;
constexpr std::uint32_t kLocatorSelfOffset = 0x14;

[[nodiscard]] bool IsExecutableRva(
    const PeImage& image,
    const std::uint64_t rva) noexcept
{
    const auto found = std::find_if(image.Sections().begin(),
        image.Sections().end(), [rva](const Section& section) {
            return rva >= section.virtualAddress &&
                rva < section.virtualAddress + section.virtualSize;
        });
    return found != image.Sections().end() && found->Executable();
}

[[nodiscard]] std::vector<RttiClass> ReconstructRtti(
    const PeImage& image,
    const std::string_view filter)
{
    // 1. Mangled type names are plain strings; a descriptor starts 0x10 bytes
    //    before its name.
    std::vector<RttiClass> classes;
    std::map<std::uint64_t, std::size_t> byDescriptor;
    for (const auto& section : image.Sections()) {
        if (section.Executable()) continue;
        const auto size = static_cast<std::size_t>(std::min(
            section.rawSize, section.virtualSize));
        const auto* data = image.Bytes().data() + section.rawPointer;
        for (std::size_t index = 0; index + 4 < size; ++index) {
            if (static_cast<char>(data[index]) != '.' ||
                static_cast<char>(data[index + 1]) != '?' ||
                static_cast<char>(data[index + 2]) != 'A') {
                continue;
            }
            const auto nameRva = section.virtualAddress + index;
            const auto name = image.ReadString(nameRva);
            if (!name || name->size() < 5) continue;
            if (!filter.empty() &&
                name->find(filter) == std::string::npos) {
                continue;
            }
            if (nameRva < kTypeDescriptorNameOffset) continue;
            RttiClass entry;
            entry.name = *name;
            entry.typeDescriptorRva = nameRva - kTypeDescriptorNameOffset;
            byDescriptor.emplace(entry.typeDescriptorRva, classes.size());
            classes.push_back(std::move(entry));
        }
    }
    if (classes.empty()) return classes;

    // 2. A complete-object-locator points back at its own address, which is
    //    what distinguishes it from an unrelated dword that happens to match.
    std::map<std::uint64_t, std::size_t> byLocator;
    for (const auto& section : image.Sections()) {
        if (section.Executable()) continue;
        const auto size = static_cast<std::size_t>(std::min(
            section.rawSize, section.virtualSize));
        if (size < kLocatorSelfOffset + 4) continue;
        for (std::size_t index = 0;
             index + kLocatorSelfOffset + 4 <= size; index += 4) {
            const auto base = section.rawPointer + index;
            if (image.ReadUint32(base) != 1) continue;
            const auto descriptor = image.ReadUint32(
                base + kLocatorTypeDescriptorOffset);
            const auto found = byDescriptor.find(descriptor);
            if (found == byDescriptor.end()) continue;
            const auto locatorRva = section.virtualAddress + index;
            if (image.ReadUint32(base + kLocatorSelfOffset) != locatorRva) {
                continue;
            }
            classes[found->second].locatorRva = locatorRva;
            byLocator.emplace(locatorRva, found->second);
        }
    }

    // 3. A vtable is preceded by a pointer to its locator, so the table itself
    //    begins one qword later.
    for (const auto& section : image.Sections()) {
        if (section.Executable()) continue;
        const auto size = static_cast<std::size_t>(std::min(
            section.rawSize, section.virtualSize));
        if (size < 8) continue;
        for (std::size_t index = 0; index + 8 <= size; index += 8) {
            const auto value = image.ReadUint64(section.rawPointer + index);
            if (value < image.ImageBase()) continue;
            const auto candidate = value - image.ImageBase();
            const auto found = byLocator.find(candidate);
            if (found == byLocator.end()) continue;
            auto& entry = classes[found->second];
            entry.vtableRva = section.virtualAddress + index + 8;
            std::uint32_t slots = 0;
            for (std::size_t slot = index + 8; slot + 8 <= size; slot += 8) {
                const auto target = image.ReadUint64(
                    section.rawPointer + slot);
                if (target < image.ImageBase()) break;
                if (!IsExecutableRva(image, target - image.ImageBase())) break;
                ++slots;
            }
            entry.vtableSlots = slots;
        }
    }
    return classes;
}

// x64 exception unwind data gives every function's exact bounds, which is how
// an address is attributed to its containing function without a disassembler.
struct RuntimeFunction
{
    std::uint32_t begin{};
    std::uint32_t end{};
    std::uint32_t unwind{};
};

// UNWIND_INFO flags live in the top 5 bits of the first byte. A chained entry
// is a fragment of a larger function, which is why it has no callers of its
// own: control reaches it by a jump, not a call.
constexpr std::uint8_t kUnwindChainInfo = 0x04;

[[nodiscard]] std::vector<RuntimeFunction> ReadRuntimeFunctions(
    const PeImage& image)
{
    std::vector<RuntimeFunction> functions;
    const auto* section = image.FindSection(".pdata");
    if (section == nullptr) return functions;
    const auto size = static_cast<std::size_t>(std::min(
        section->rawSize, section->virtualSize));
    for (std::size_t offset = 0; offset + 12 <= size; offset += 12) {
        RuntimeFunction entry;
        entry.begin = image.ReadUint32(section->rawPointer + offset);
        entry.end = image.ReadUint32(section->rawPointer + offset + 4);
        entry.unwind = image.ReadUint32(section->rawPointer + offset + 8);
        if (entry.end <= entry.begin) continue;
        functions.push_back(entry);
    }
    std::sort(functions.begin(), functions.end(),
        [](const RuntimeFunction& left, const RuntimeFunction& right) {
            return left.begin < right.begin;
        });
    return functions;
}

[[nodiscard]] std::optional<RuntimeFunction> FindContainingFunction(
    const std::vector<RuntimeFunction>& functions,
    const std::uint64_t rva)
{
    const auto found = std::upper_bound(functions.begin(), functions.end(), rva,
        [](const std::uint64_t value, const RuntimeFunction& entry) {
            return value < entry.begin;
        });
    if (found == functions.begin()) return std::nullopt;
    const auto candidate = std::prev(found);
    if (rva >= candidate->begin && rva < candidate->end) return *candidate;
    return std::nullopt;
}

// Follows UNWIND_INFO chain records until the primary entry is reached, so a
// cold or split fragment is attributed to the function it belongs to.
[[nodiscard]] std::optional<RuntimeFunction> ResolvePrimaryFunction(
    const PeImage& image,
    const std::vector<RuntimeFunction>& functions,
    const std::uint64_t rva)
{
    auto current = FindContainingFunction(functions, rva);
    for (int guard = 0; guard < 16 && current; ++guard) {
        const auto info = image.RvaToFile(current->unwind);
        if (!info) return current;
        const auto header = static_cast<std::uint8_t>(
            image.Bytes()[*info]);
        if ((header >> 3) != 0 && (header >> 3 & kUnwindChainInfo) == 0) {
            // Flags are the upper five bits; no chain flag means primary.
        }
        const auto flags = static_cast<std::uint8_t>(header >> 3);
        if ((flags & kUnwindChainInfo) == 0) return current;
        const auto codeCount = static_cast<std::uint8_t>(
            image.Bytes()[*info + 2]);
        // Unwind codes are padded to an even count before the chained entry.
        const auto chained = *info + 4 +
            ((codeCount + 1) & ~std::size_t{1}) * 2;
        const auto parentBegin = image.ReadUint32(chained);
        const auto parent = FindContainingFunction(functions, parentBegin);
        if (!parent || parent->begin == current->begin) return current;
        current = parent;
    }
    return current;
}

// Direct `call rel32` sites targeting one function.
[[nodiscard]] std::vector<std::uint64_t> ScanCallSites(
    const PeImage& image,
    const std::uint64_t targetRva)
{
    std::vector<std::uint64_t> callers;
    for (const auto& section : image.Sections()) {
        if (!section.Executable()) continue;
        const auto* data = image.Bytes().data() + section.rawPointer;
        const auto size = static_cast<std::size_t>(std::min(
            section.rawSize, section.virtualSize));
        if (size < 5) continue;
        for (std::size_t index = 0; index + 5 <= size; ++index) {
            if (static_cast<unsigned char>(data[index]) != 0xE8) continue;
            std::int32_t displacement{};
            std::memcpy(&displacement, data + index + 1, sizeof(displacement));
            const auto instruction = section.virtualAddress + index;
            const auto target = static_cast<std::int64_t>(instruction) + 5 +
                displacement;
            if (target >= 0 &&
                static_cast<std::uint64_t>(target) == targetRva) {
                callers.push_back(instruction);
            }
        }
    }
    return callers;
}

[[nodiscard]] bool ParseNumber(
    const std::string_view text,
    std::uint64_t& value)
{
    auto view = text;
    int base = 10;
    if (view.size() > 2 && view[0] == '0' && (view[1] == 'x' || view[1] == 'X')) {
        view.remove_prefix(2);
        base = 16;
    }
    const auto* begin = view.data();
    const auto* end = begin + view.size();
    return std::from_chars(begin, end, value, base).ptr == end;
}

// Locates camera matrices in a raw memory dump by the same self-consistency
// test the live scan uses: viewProjection == projection * view. Unlike the
// live scan this reports every near-miss residual, so a window that holds no
// camera can be told apart from one whose tolerance is simply too tight.
struct MatrixTriple
{
    std::size_t viewOffset{};
    std::size_t projectionOffset{};
    std::size_t viewProjectionOffset{};
    double residual{};
    bool columnMajor{};
};

[[nodiscard]] bool PlausibleMatrix(const float* values) noexcept
{
    for (std::size_t index = 0; index < 16; ++index) {
        if (!std::isfinite(values[index])) return false;
        if (std::abs(values[index]) > 1.0e7f) return false;
    }
    // An all-zero block is finite and bounded but is not a matrix.
    return std::any_of(values, values + 16,
        [](const float value) { return value != 0.0f; });
}

[[nodiscard]] double MultiplyResidual(
    const float* left,
    const float* right,
    const float* product,
    const bool columnMajor) noexcept
{
    double worst = 0.0;
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            double sum = 0.0;
            for (std::size_t k = 0; k < 4; ++k) {
                const auto a = columnMajor
                    ? left[k * 4 + row] : left[row * 4 + k];
                const auto b = columnMajor
                    ? right[column * 4 + k] : right[k * 4 + column];
                sum += static_cast<double>(a) * b;
            }
            const auto expected = columnMajor
                ? product[column * 4 + row] : product[row * 4 + column];
            worst = std::max(worst, std::abs(sum - expected));
        }
    }
    return worst;
}

// A view matrix's upper 3x3 is a rotation: unit rows that are mutually
// orthogonal. Filtering on that before the pairwise search is what keeps the
// scan tractable, because an unfiltered triple loop over every plausible
// 64-byte block is cubic and does not finish.
[[nodiscard]] bool OrthonormalRotation(
    const float* values,
    const bool columnMajor) noexcept
{
    const auto at = [values, columnMajor](
        const std::size_t row, const std::size_t column) {
        return columnMajor ? values[column * 4 + row]
                           : values[row * 4 + column];
    };
    for (std::size_t row = 0; row < 3; ++row) {
        double length = 0.0;
        for (std::size_t column = 0; column < 3; ++column) {
            length += static_cast<double>(at(row, column)) * at(row, column);
        }
        if (std::abs(length - 1.0) > 1.0e-3) return false;
    }
    for (std::size_t left = 0; left < 3; ++left) {
        for (std::size_t right = left + 1; right < 3; ++right) {
            double dot = 0.0;
            for (std::size_t column = 0; column < 3; ++column) {
                dot += static_cast<double>(at(left, column)) *
                    at(right, column);
            }
            if (std::abs(dot) > 1.0e-3) return false;
        }
    }
    return true;
}

// A perspective projection has an all-but-empty w row and a zero w scale.
[[nodiscard]] bool ProjectionShaped(
    const float* values,
    const bool columnMajor) noexcept
{
    const auto at = [values, columnMajor](
        const std::size_t row, const std::size_t column) {
        return columnMajor ? values[column * 4 + row]
                           : values[row * 4 + column];
    };
    if (std::abs(at(3, 3)) > 1.0e-4) return false;
    if (std::abs(std::abs(at(3, 2)) - 1.0f) > 1.0e-3) return false;
    if (std::abs(at(3, 0)) > 1.0e-4 || std::abs(at(3, 1)) > 1.0e-4) {
        return false;
    }
    return std::abs(at(0, 0)) > 1.0e-4 && std::abs(at(1, 1)) > 1.0e-4;
}

[[nodiscard]] std::vector<MatrixTriple> ScanDumpForCameras(
    const std::vector<std::byte>& block,
    const double tolerance)
{
    std::vector<MatrixTriple> triples;
    if (block.size() < 64) return triples;
    std::vector<std::size_t> candidates;
    for (std::size_t offset = 0; offset + 64 <= block.size();
         offset += sizeof(float)) {
        const auto* values = reinterpret_cast<const float*>(
            block.data() + offset);
        if (PlausibleMatrix(values)) candidates.push_back(offset);
    }
    for (const auto columnMajor : {false, true}) {
        std::vector<std::size_t> views;
        std::vector<std::size_t> projections;
        for (const auto offset : candidates) {
            const auto* values = reinterpret_cast<const float*>(
                block.data() + offset);
            if (OrthonormalRotation(values, columnMajor)) {
                views.push_back(offset);
            }
            if (ProjectionShaped(values, columnMajor)) {
                projections.push_back(offset);
            }
        }
        for (const auto viewOffset : views) {
            const auto* view = reinterpret_cast<const float*>(
                block.data() + viewOffset);
            for (const auto projectionOffset : projections) {
                if (projectionOffset == viewOffset) continue;
                const auto* projection = reinterpret_cast<const float*>(
                    block.data() + projectionOffset);
                for (const auto productOffset : candidates) {
                    if (productOffset == viewOffset ||
                        productOffset == projectionOffset) {
                        continue;
                    }
                    const auto* product = reinterpret_cast<const float*>(
                        block.data() + productOffset);
                    const auto residual = MultiplyResidual(
                        projection, view, product, columnMajor);
                    if (residual > tolerance) continue;
                    triples.push_back({viewOffset, projectionOffset,
                        productOffset, residual, columnMajor});
                }
            }
        }
    }
    return triples;
}

void PrintUsage()
{
    std::cout
        << "vf_engine_analysis --exe <Fallout4.exe> <mode>\n"
           "  --command-table [--seed <rva>]   walk the console command table\n"
           "  --find-command <name>            one command by long or short name\n"
           "  --xref-lea <lo> [--high <hi>]    RIP-relative lea references\n"
           "  --calls <rva>                    direct call sites of a function\n"
           "  --sections                       PE section table\n";
}

}

int main(const int argc, const char* const* argv)
{
    std::filesystem::path executable;
    std::string mode;
    std::string commandName;
    std::uint64_t low = 0;
    std::uint64_t high = 0;
    std::uint64_t seed = 0x2EF8C30;   // the UpdateGodraySettings record
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--exe" && index + 1 < argc) {
            executable = argv[++index];
        } else if (argument == "--command-table" || argument == "--sections") {
            mode = argument;
        } else if (argument == "--find-command" && index + 1 < argc) {
            mode = "--find-command";
            commandName = argv[++index];
        } else if (argument == "--xref-lea" && index + 1 < argc) {
            mode = "--xref-lea";
            if (!ParseNumber(argv[++index], low)) return 2;
        } else if (argument == "--rtti" && index + 1 < argc) {
            mode = "--rtti";
            commandName = argv[++index];
        } else if (argument == "--scan-dump" && index + 1 < argc) {
            mode = "--scan-dump";
            commandName = argv[++index];
        } else if (argument == "--class-of" && index + 1 < argc) {
            mode = "--class-of";
            if (!ParseNumber(argv[++index], low)) return 2;
        } else if (argument == "--vtable" && index + 1 < argc) {
            mode = "--vtable";
            if (!ParseNumber(argv[++index], low)) return 2;
        } else if (argument == "--callers-of" && index + 1 < argc) {
            mode = "--callers-of";
            if (!ParseNumber(argv[++index], low)) return 2;
        } else if (argument == "--find-pointer" && index + 1 < argc) {
            mode = "--find-pointer";
            if (!ParseNumber(argv[++index], low)) return 2;
        } else if (argument == "--calls" && index + 1 < argc) {
            mode = "--calls";
            if (!ParseNumber(argv[++index], low)) return 2;
        } else if (argument == "--high" && index + 1 < argc) {
            if (!ParseNumber(argv[++index], high)) return 2;
        } else if (argument == "--seed" && index + 1 < argc) {
            if (!ParseNumber(argv[++index], seed)) return 2;
        } else {
            PrintUsage();
            return 2;
        }
    }
    if (mode.empty() || (executable.empty() && mode != "--scan-dump")) {
        PrintUsage();
        return 2;
    }

    // The dump scanner reads a raw memory capture, not the executable.
    if (mode == "--scan-dump") {
        std::ifstream stream{commandName, std::ios::binary | std::ios::ate};
        if (!stream) {
            std::cerr << "engine-analysis: cannot read dump " << commandName
                      << '\n';
            return 9;
        }
        const auto size = static_cast<std::size_t>(stream.tellg());
        stream.seekg(0);
        std::vector<std::byte> block(size);
        stream.read(reinterpret_cast<char*>(block.data()),
            static_cast<std::streamsize>(size));
        std::cout << "dump bytes " << std::dec << size << '\n';
        for (const auto tolerance : {1.0e-3, 1.0e-1, 1.0}) {
            const auto triples = ScanDumpForCameras(block, tolerance);
            std::cout << "tolerance " << tolerance << " triples "
                      << triples.size() << '\n';
            for (std::size_t index = 0;
                 index < triples.size() && index < 12; ++index) {
                const auto& triple = triples[index];
                std::cout << "  view +0x" << std::hex << triple.viewOffset
                          << " projection +0x" << triple.projectionOffset
                          << " viewProjection +0x"
                          << triple.viewProjectionOffset << std::dec
                          << (triple.columnMajor ? " column-major" : " row-major")
                          << " residual " << triple.residual << '\n';
            }
            if (!triples.empty()) break;
        }
        return 0;
    }

    PeImage image;
    if (!image.Load(executable)) {
        std::cerr << "engine-analysis: could not load " << executable << '\n';
        return 3;
    }
    std::cout << std::hex << std::showbase;

    if (mode == "--sections") {
        std::cout << "image-base " << image.ImageBase()
                  << " size-of-image " << image.SizeOfImage() << '\n';
        for (const auto& section : image.Sections()) {
            std::cout << "  " << section.name << " va " << section.virtualAddress
                      << " vsize " << section.virtualSize << " raw "
                      << section.rawPointer << " rawsize " << section.rawSize
                      << (section.Executable() ? " exec" : "") << '\n';
        }
        return 0;
    }

    if (mode == "--command-table" || mode == "--find-command") {
        const auto records = WalkCommandTable(image, seed);
        if (records.empty()) {
            std::cerr << "engine-analysis: no command table at seed " << seed
                      << '\n';
            return 4;
        }
        if (mode == "--command-table") {
            std::cout << "records " << std::dec << records.size() << std::hex
                      << " first " << records.front().recordRva << " last "
                      << records.back().recordRva << " stride " << kRecordSize
                      << '\n';
            std::cout << "opcodes " << std::dec << records.front().opcode
                      << ".." << records.back().opcode << std::hex << '\n';
            for (const auto& record : records) {
                std::cout << "  " << record.recordRva << " opcode " << std::dec
                          << record.opcode << std::hex << " execute "
                          << record.executeRva << "  " << record.longName;
                if (!record.shortName.empty()) {
                    std::cout << " (" << record.shortName << ')';
                }
                std::cout << '\n';
            }
            return 0;
        }
        const auto equalsIgnoringCase = [](const std::string& left,
                                           const std::string& right) {
            return left.size() == right.size() &&
                std::equal(left.begin(), left.end(), right.begin(),
                    [](const char a, const char b) {
                        return std::tolower(static_cast<unsigned char>(a)) ==
                            std::tolower(static_cast<unsigned char>(b));
                    });
        };
        for (const auto& record : records) {
            if (!equalsIgnoringCase(record.longName, commandName) &&
                !equalsIgnoringCase(record.shortName, commandName)) {
                continue;
            }
            std::cout << "long " << record.longName << "\nshort "
                      << record.shortName << "\nopcode " << std::dec
                      << record.opcode << std::hex << "\nrecord "
                      << record.recordRva << "\nexecute " << record.executeRva
                      << "\ncompile " << record.compileRva << "\nparameters "
                      << record.parametersRva << "\nhelp " << record.help
                      << '\n';
            return 0;
        }
        std::cerr << "engine-analysis: no command named " << commandName
                  << '\n';
        return 5;
    }

    if (mode == "--xref-lea") {
        const auto upper = high > low ? high : low + 1;
        const auto references = ScanLeaReferences(image, low, upper);
        std::cout << "lea references " << std::dec << references.size()
                  << std::hex << '\n';
        for (const auto& reference : references) {
            std::cout << "  " << reference.instructionRva << " -> "
                      << reference.targetRva << '\n';
        }
        return 0;
    }

    if (mode == "--rtti") {
        const auto classes = ReconstructRtti(image, commandName);
        std::cout << "classes " << std::dec << classes.size() << std::hex
                  << '\n';
        for (const auto& entry : classes) {
            std::cout << "  " << entry.name << "\n    descriptor "
                      << entry.typeDescriptorRva << " locator "
                      << entry.locatorRva << " vtable " << entry.vtableRva
                      << " slots " << std::dec << entry.vtableSlots
                      << std::hex << '\n';
        }
        return 0;
    }

    if (mode == "--class-of") {
        // Attributes a vtable slot to its owning class, which turns a bare
        // function address into "class X, virtual slot N".
        const auto classes = ReconstructRtti(image, {});
        bool matched = false;
        for (const auto& entry : classes) {
            if (entry.vtableRva == 0 || entry.vtableSlots == 0) continue;
            const auto last = entry.vtableRva + entry.vtableSlots * 8;
            if (low < entry.vtableRva || low >= last) continue;
            std::cout << entry.name << " vtable " << entry.vtableRva
                      << " slot " << std::dec
                      << (low - entry.vtableRva) / 8 << " of "
                      << entry.vtableSlots << std::hex << '\n';
            matched = true;
        }
        if (!matched) std::cout << "no class vtable contains " << low << '\n';
        return 0;
    }

    if (mode == "--vtable") {
        const auto functions = ReadRuntimeFunctions(image);
        const auto offset = image.RvaToFile(low);
        if (!offset) {
            std::cerr << "engine-analysis: vtable " << low << " not in file\n";
            return 8;
        }
        for (std::uint32_t slot = 0;; ++slot) {
            const auto value = image.ReadUint64(*offset + slot * 8);
            if (value < image.ImageBase()) break;
            const auto target = value - image.ImageBase();
            if (!IsExecutableRva(image, target)) break;
            std::cout << "  [" << std::dec << slot << "] " << std::hex
                      << target;
            if (const auto owner = FindContainingFunction(functions, target)) {
                std::cout << " (" << owner->begin << ".." << owner->end << ')';
            }
            std::cout << '\n';
        }
        return 0;
    }

    if (mode == "--callers-of") {
        const auto functions = ReadRuntimeFunctions(image);
        if (functions.empty()) {
            std::cerr << "engine-analysis: no .pdata\n";
            return 6;
        }
        const auto chunk = FindContainingFunction(functions, low);
        const auto owner = ResolvePrimaryFunction(image, functions, low);
        if (!owner || !chunk) {
            std::cerr << "engine-analysis: no function contains " << low
                      << '\n';
            return 7;
        }
        std::cout << "chunk " << chunk->begin << ".." << chunk->end
                  << " contains " << low << '\n';
        std::cout << "primary " << owner->begin << ".." << owner->end << '\n';
        const auto callers = ScanCallSites(image, owner->begin);
        std::cout << "callers " << std::dec << callers.size() << std::hex
                  << '\n';
        for (const auto site : callers) {
            const auto containing = FindContainingFunction(functions, site);
            std::cout << "  call at " << site << " in ";
            if (containing) {
                std::cout << containing->begin << ".." << containing->end;
            } else {
                std::cout << "<unknown>";
            }
            std::cout << '\n';
        }
        return 0;
    }

    if (mode == "--find-pointer") {
        const auto upper = high > low ? high : low + 1;
        const auto found = ScanAbsolutePointers(image, low, upper);
        std::cout << "absolute pointers " << std::dec << found.size()
                  << std::hex << '\n';
        for (const auto& [site, target] : found) {
            std::cout << "  at " << site << " -> " << target << '\n';
        }
        return 0;
    }

    if (mode == "--calls") {
        const auto callers = ScanCallSites(image, low);
        std::cout << "call sites " << std::dec << callers.size() << std::hex
                  << '\n';
        for (const auto caller : callers) std::cout << "  " << caller << '\n';
        return 0;
    }

    PrintUsage();
    return 2;
}
