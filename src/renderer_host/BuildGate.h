#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace vf::renderer {

struct Sha256
{
    std::array<std::uint8_t, 32> bytes{};

    friend bool operator==(const Sha256&, const Sha256&) = default;
};

enum class Sha256ParseError : std::uint8_t
{
    None,
    WrongLength,
    InvalidCharacter
};

struct Sha256ParseResult
{
    Sha256 value{};
    Sha256ParseError error{Sha256ParseError::None};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == Sha256ParseError::None;
    }
};

[[nodiscard]] Sha256ParseResult ParseSha256(std::string_view text) noexcept;
[[nodiscard]] std::string FormatSha256(const Sha256& digest);

struct BuildFingerprint
{
    std::uint32_t runtimeVersion{};
    std::uint16_t machine{};
    std::uint32_t timeDateStamp{};
    std::uint32_t sizeOfImage{};
    std::uint64_t executableFileSize{};
    Sha256 executableSha256{};
    std::uint64_t addressLibraryFileSize{};
    Sha256 addressLibrarySha256{};
};

enum class BuildMismatch : std::uint32_t
{
    None = 0,
    RuntimeVersion = 1u << 0,
    Machine = 1u << 1,
    TimeDateStamp = 1u << 2,
    SizeOfImage = 1u << 3,
    ExecutableFileSize = 1u << 4,
    ExecutableSha256 = 1u << 5,
    AddressLibraryFileSize = 1u << 6,
    AddressLibrarySha256 = 1u << 7
};

struct BuildGateReport
{
    std::uint32_t mismatchMask{};

    [[nodiscard]] bool Accepted() const noexcept
    {
        return mismatchMask == 0;
    }

    [[nodiscard]] bool Has(BuildMismatch mismatch) const noexcept
    {
        return (mismatchMask & static_cast<std::uint32_t>(mismatch)) != 0;
    }
};

[[nodiscard]] const BuildFingerprint& TargetBuild_1_11_221();
[[nodiscard]] BuildGateReport ValidateBuild(
    const BuildFingerprint& expected,
    const BuildFingerprint& observed) noexcept;
[[nodiscard]] std::string FormatBuildGateReport(const BuildGateReport& report);

enum class PeProbeError : std::uint8_t
{
    None,
    TooSmall,
    BadDosMagic,
    BadNtOffset,
    BadNtSignature,
    TruncatedFileHeader,
    TruncatedOptionalHeader,
    NotPe32Plus
};

struct PeHeaderSummary
{
    std::uint16_t machine{};
    std::uint32_t timeDateStamp{};
    std::uint32_t sizeOfImage{};
};

struct PeProbeResult
{
    PeHeaderSummary summary{};
    PeProbeError error{PeProbeError::None};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == PeProbeError::None;
    }
};

[[nodiscard]] PeProbeResult ProbePeHeader(std::span<const std::byte> bytes) noexcept;

}
