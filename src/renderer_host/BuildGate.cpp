#include "renderer_host/BuildGate.h"

#include <array>

namespace vf::renderer {

namespace {

constexpr std::size_t kSha256TextLength = 64;
constexpr std::uint16_t kDosMagic = 0x5A4D;
constexpr std::uint32_t kNtSignature = 0x00004550;
constexpr std::uint16_t kPe32PlusMagic = 0x020B;
constexpr std::size_t kDosNtOffsetField = 0x3C;
constexpr std::size_t kFileHeaderSize = 20;
constexpr std::size_t kOptionalSizeOfImageOffset = 0x38;
constexpr std::size_t kRequiredOptionalHeaderSize =
    kOptionalSizeOfImageOffset + sizeof(std::uint32_t);

int HexValue(const char character) noexcept
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

bool HasRange(
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    const std::size_t length) noexcept
{
    return offset <= bytes.size() && length <= bytes.size() - offset;
}

std::uint16_t ReadU16(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::uint32_t ReadU32(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept
{
    return std::to_integer<std::uint32_t>(bytes[offset]) |
        (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8) |
        (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16) |
        (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24);
}

Sha256 KnownDigest(const std::string_view text) noexcept
{
    return ParseSha256(text).value;
}

void AddMismatch(
    BuildGateReport& report,
    const BuildMismatch mismatch,
    const bool differs) noexcept
{
    if (differs) {
        report.mismatchMask |= static_cast<std::uint32_t>(mismatch);
    }
}

}

Sha256ParseResult ParseSha256(const std::string_view text) noexcept
{
    if (text.size() != kSha256TextLength) {
        return Sha256ParseResult{{}, Sha256ParseError::WrongLength};
    }

    Sha256 digest;
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        const auto high = HexValue(text[index * 2]);
        const auto low = HexValue(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return Sha256ParseResult{{}, Sha256ParseError::InvalidCharacter};
        }
        digest.bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
    }

    return Sha256ParseResult{digest, Sha256ParseError::None};
}

std::string FormatSha256(const Sha256& digest)
{
    constexpr std::array hex{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

    std::string result(kSha256TextLength, '0');
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        const auto value = digest.bytes[index];
        result[index * 2] = hex[value >> 4];
        result[index * 2 + 1] = hex[value & 0x0Fu];
    }
    return result;
}

const BuildFingerprint& TargetBuild_1_11_221()
{
    static const BuildFingerprint target{
        0x010B0DD0u,
        0x8664u,
        0x69E2A744u,
        0x04244000u,
        55'293'864u,
        KnownDigest("428F9996CC4248E26C0F62F9FDD3EAF0E5EB305834B67EE5996538E593218B61"),
        10'425'304u,
        KnownDigest("2FDCC2A0926659C37255D2EAF335775240EC7FEFDF6CB3B35B063FACA25A448F"),
    };
    return target;
}

BuildGateReport ValidateBuild(
    const BuildFingerprint& expected,
    const BuildFingerprint& observed) noexcept
{
    BuildGateReport report;
    AddMismatch(report, BuildMismatch::RuntimeVersion,
        expected.runtimeVersion != observed.runtimeVersion);
    AddMismatch(report, BuildMismatch::Machine,
        expected.machine != observed.machine);
    AddMismatch(report, BuildMismatch::TimeDateStamp,
        expected.timeDateStamp != observed.timeDateStamp);
    AddMismatch(report, BuildMismatch::SizeOfImage,
        expected.sizeOfImage != observed.sizeOfImage);
    AddMismatch(report, BuildMismatch::ExecutableFileSize,
        expected.executableFileSize != observed.executableFileSize);
    AddMismatch(report, BuildMismatch::ExecutableSha256,
        expected.executableSha256 != observed.executableSha256);
    AddMismatch(report, BuildMismatch::AddressLibraryFileSize,
        expected.addressLibraryFileSize != observed.addressLibraryFileSize);
    AddMismatch(report, BuildMismatch::AddressLibrarySha256,
        expected.addressLibrarySha256 != observed.addressLibrarySha256);
    return report;
}

std::string FormatBuildGateReport(const BuildGateReport& report)
{
    if (report.Accepted()) {
        return "build-gate: accepted";
    }

    constexpr std::array mismatches{
        std::pair{BuildMismatch::RuntimeVersion, "runtime-version"},
        std::pair{BuildMismatch::Machine, "machine"},
        std::pair{BuildMismatch::TimeDateStamp, "time-date-stamp"},
        std::pair{BuildMismatch::SizeOfImage, "size-of-image"},
        std::pair{BuildMismatch::ExecutableFileSize, "executable-file-size"},
        std::pair{BuildMismatch::ExecutableSha256, "executable-sha256"},
        std::pair{BuildMismatch::AddressLibraryFileSize, "address-library-file-size"},
        std::pair{BuildMismatch::AddressLibrarySha256, "address-library-sha256"},
    };

    std::string result = "build-gate: rejected [";
    bool first = true;
    for (const auto& [mismatch, name] : mismatches) {
        if (!report.Has(mismatch)) {
            continue;
        }
        if (!first) {
            result.push_back(',');
        }
        result.append(name);
        first = false;
    }
    result.push_back(']');
    return result;
}

PeProbeResult ProbePeHeader(const std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() < kDosNtOffsetField + sizeof(std::uint32_t)) {
        return PeProbeResult{{}, PeProbeError::TooSmall};
    }
    if (ReadU16(bytes, 0) != kDosMagic) {
        return PeProbeResult{{}, PeProbeError::BadDosMagic};
    }

    const auto ntOffset = static_cast<std::size_t>(ReadU32(bytes, kDosNtOffsetField));
    if (!HasRange(bytes, ntOffset, sizeof(std::uint32_t))) {
        return PeProbeResult{{}, PeProbeError::BadNtOffset};
    }
    if (ReadU32(bytes, ntOffset) != kNtSignature) {
        return PeProbeResult{{}, PeProbeError::BadNtSignature};
    }

    const auto fileHeaderOffset = ntOffset + sizeof(std::uint32_t);
    if (!HasRange(bytes, fileHeaderOffset, kFileHeaderSize)) {
        return PeProbeResult{{}, PeProbeError::TruncatedFileHeader};
    }

    const auto optionalHeaderSize = static_cast<std::size_t>(
        ReadU16(bytes, fileHeaderOffset + 16));
    const auto optionalHeaderOffset = fileHeaderOffset + kFileHeaderSize;
    if (optionalHeaderSize < kRequiredOptionalHeaderSize ||
        !HasRange(bytes, optionalHeaderOffset, optionalHeaderSize)) {
        return PeProbeResult{{}, PeProbeError::TruncatedOptionalHeader};
    }
    if (ReadU16(bytes, optionalHeaderOffset) != kPe32PlusMagic) {
        return PeProbeResult{{}, PeProbeError::NotPe32Plus};
    }

    const PeHeaderSummary summary{
        ReadU16(bytes, fileHeaderOffset),
        ReadU32(bytes, fileHeaderOffset + 4),
        ReadU32(bytes, optionalHeaderOffset + kOptionalSizeOfImageOffset),
    };
    return PeProbeResult{summary, PeProbeError::None};
}

}
