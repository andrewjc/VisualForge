#pragma once

#include "renderer_host/BuildGate.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace vf::renderer {

enum class Sha256Error : std::uint8_t
{
    None,
    ProviderUnavailable,
    HashCreationFailed,
    HashingFailed
};

struct Sha256Result
{
    Sha256 value{};
    Sha256Error error{Sha256Error::None};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == Sha256Error::None;
    }
};

enum class FileProbeError : std::uint8_t
{
    None,
    NotFound,
    OpenFailed,
    ReadFailed,
    HashFailed
};

struct FileIdentity
{
    std::uint64_t size{};
    Sha256 sha256{};
};

struct FileIdentityResult
{
    FileIdentity identity{};
    FileProbeError error{FileProbeError::None};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == FileProbeError::None;
    }
};

enum class InstalledBuildProbeError : std::uint8_t
{
    None,
    ExecutableIdentity,
    ExecutablePe,
    AddressLibraryIdentity
};

struct InstalledBuildProbeResult
{
    BuildFingerprint fingerprint{};
    InstalledBuildProbeError error{InstalledBuildProbeError::None};
    FileProbeError fileError{FileProbeError::None};
    PeProbeError peError{PeProbeError::None};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == InstalledBuildProbeError::None;
    }
};

[[nodiscard]] Sha256Result HashSha256(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] FileIdentityResult ProbeFileIdentity(
    const std::filesystem::path& path) noexcept;
[[nodiscard]] InstalledBuildProbeResult ProbeInstalledBuild(
    std::uint32_t runtimeVersion,
    const std::filesystem::path& executable,
    const std::filesystem::path& addressLibrary) noexcept;
[[nodiscard]] std::string FormatInstalledBuildProbe(
    const InstalledBuildProbeResult& result);

}
