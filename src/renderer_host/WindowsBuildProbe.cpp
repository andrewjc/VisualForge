#include "renderer_host/WindowsBuildProbe.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <vector>

namespace vf::renderer {

namespace {

constexpr std::size_t kReadBufferSize = 64 * 1024;
constexpr std::size_t kPePrefixSize = 4096;

class Sha256Session
{
public:
    Sha256Session() = default;

    ~Sha256Session()
    {
        if (hash_ != nullptr) {
            BCryptDestroyHash(hash_);
        }
        if (algorithm_ != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    Sha256Session(const Sha256Session&) = delete;
    Sha256Session& operator=(const Sha256Session&) = delete;

    Sha256Error Initialize() noexcept
    {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                &algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            return Sha256Error::ProviderUnavailable;
        }

        ULONG objectLength{};
        ULONG bytesWritten{};
        if (!BCRYPT_SUCCESS(BCryptGetProperty(
                algorithm_,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectLength),
                sizeof(objectLength),
                &bytesWritten,
                0)) ||
            bytesWritten != sizeof(objectLength)) {
            return Sha256Error::HashCreationFailed;
        }

        try {
            object_.resize(objectLength);
        } catch (...) {
            return Sha256Error::HashCreationFailed;
        }

        if (!BCRYPT_SUCCESS(BCryptCreateHash(
                algorithm_,
                &hash_,
                object_.data(),
                objectLength,
                nullptr,
                0,
                0))) {
            return Sha256Error::HashCreationFailed;
        }
        return Sha256Error::None;
    }

    Sha256Error Update(const std::span<const std::byte> bytes) noexcept
    {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto remaining = bytes.size() - offset;
            const auto chunkSize = std::min(
                remaining,
                static_cast<std::size_t>(std::numeric_limits<ULONG>::max()));
            auto* data = reinterpret_cast<PUCHAR>(
                const_cast<std::byte*>(bytes.data() + offset));
            if (!BCRYPT_SUCCESS(BCryptHashData(
                    hash_, data, static_cast<ULONG>(chunkSize), 0))) {
                return Sha256Error::HashingFailed;
            }
            offset += chunkSize;
        }
        return Sha256Error::None;
    }

    Sha256Result Finish() noexcept
    {
        Sha256 digest;
        if (!BCRYPT_SUCCESS(BCryptFinishHash(
                hash_,
                digest.bytes.data(),
                static_cast<ULONG>(digest.bytes.size()),
                0))) {
            return Sha256Result{{}, Sha256Error::HashingFailed};
        }
        return Sha256Result{digest, Sha256Error::None};
    }

private:
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};
    std::vector<UCHAR> object_;
};

const char* ToString(const FileProbeError error) noexcept
{
    switch (error) {
    case FileProbeError::None:
        return "none";
    case FileProbeError::NotFound:
        return "not-found";
    case FileProbeError::OpenFailed:
        return "open-failed";
    case FileProbeError::ReadFailed:
        return "read-failed";
    case FileProbeError::HashFailed:
        return "hash-failed";
    }
    return "unknown";
}

}

Sha256Result HashSha256(const std::span<const std::byte> bytes) noexcept
{
    Sha256Session session;
    const auto initialization = session.Initialize();
    if (initialization != Sha256Error::None) {
        return Sha256Result{{}, initialization};
    }
    const auto update = session.Update(bytes);
    if (update != Sha256Error::None) {
        return Sha256Result{{}, update};
    }
    return session.Finish();
}

FileIdentityResult ProbeFileIdentity(const std::filesystem::path& path) noexcept
{
    std::error_code existsError;
    if (!std::filesystem::exists(path, existsError)) {
        return FileIdentityResult{{}, FileProbeError::NotFound};
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return FileIdentityResult{{}, FileProbeError::OpenFailed};
    }

    Sha256Session session;
    if (session.Initialize() != Sha256Error::None) {
        return FileIdentityResult{{}, FileProbeError::HashFailed};
    }

    std::array<std::byte, kReadBufferSize> buffer{};
    std::uint64_t size = 0;
    while (stream) {
        stream.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        if (count <= 0) {
            continue;
        }
        const auto byteCount = static_cast<std::size_t>(count);
        if (size > std::numeric_limits<std::uint64_t>::max() - byteCount) {
            return FileIdentityResult{{}, FileProbeError::ReadFailed};
        }
        if (session.Update(std::span{buffer}.first(byteCount)) != Sha256Error::None) {
            return FileIdentityResult{{}, FileProbeError::HashFailed};
        }
        size += byteCount;
    }
    if (stream.bad() || (!stream.eof() && stream.fail())) {
        return FileIdentityResult{{}, FileProbeError::ReadFailed};
    }

    const auto hash = session.Finish();
    if (!hash) {
        return FileIdentityResult{{}, FileProbeError::HashFailed};
    }
    return FileIdentityResult{
        FileIdentity{size, hash.value},
        FileProbeError::None,
    };
}

InstalledBuildProbeResult ProbeInstalledBuild(
    const std::uint32_t runtimeVersion,
    const std::filesystem::path& executable,
    const std::filesystem::path& addressLibrary) noexcept
{
    const auto executableIdentity = ProbeFileIdentity(executable);
    if (!executableIdentity) {
        return InstalledBuildProbeResult{
            {},
            InstalledBuildProbeError::ExecutableIdentity,
            executableIdentity.error,
            PeProbeError::None,
        };
    }

    std::ifstream executableStream(executable, std::ios::binary);
    if (!executableStream.is_open()) {
        return InstalledBuildProbeResult{
            {},
            InstalledBuildProbeError::ExecutablePe,
            FileProbeError::OpenFailed,
            PeProbeError::None,
        };
    }
    std::array<std::byte, kPePrefixSize> prefix{};
    executableStream.read(
        reinterpret_cast<char*>(prefix.data()),
        static_cast<std::streamsize>(prefix.size()));
    const auto prefixCount = executableStream.gcount();
    if (prefixCount <= 0 || executableStream.bad()) {
        return InstalledBuildProbeResult{
            {},
            InstalledBuildProbeError::ExecutablePe,
            FileProbeError::ReadFailed,
            PeProbeError::None,
        };
    }
    const auto pe = ProbePeHeader(
        std::span{prefix}.first(static_cast<std::size_t>(prefixCount)));
    if (!pe) {
        return InstalledBuildProbeResult{
            {},
            InstalledBuildProbeError::ExecutablePe,
            FileProbeError::None,
            pe.error,
        };
    }

    const auto addressLibraryIdentity = ProbeFileIdentity(addressLibrary);
    if (!addressLibraryIdentity) {
        return InstalledBuildProbeResult{
            {},
            InstalledBuildProbeError::AddressLibraryIdentity,
            addressLibraryIdentity.error,
            PeProbeError::None,
        };
    }

    const BuildFingerprint fingerprint{
        runtimeVersion,
        pe.summary.machine,
        pe.summary.timeDateStamp,
        pe.summary.sizeOfImage,
        executableIdentity.identity.size,
        executableIdentity.identity.sha256,
        addressLibraryIdentity.identity.size,
        addressLibraryIdentity.identity.sha256,
    };
    return InstalledBuildProbeResult{
        fingerprint,
        InstalledBuildProbeError::None,
        FileProbeError::None,
        PeProbeError::None,
    };
}

std::string FormatInstalledBuildProbe(const InstalledBuildProbeResult& result)
{
    if (result) {
        return "build-probe: complete";
    }

    std::string text = "build-probe: failed ";
    switch (result.error) {
    case InstalledBuildProbeError::None:
        text += "unknown";
        break;
    case InstalledBuildProbeError::ExecutableIdentity:
        text += "executable-identity/";
        text += ToString(result.fileError);
        break;
    case InstalledBuildProbeError::ExecutablePe:
        text += "executable-pe";
        if (result.fileError != FileProbeError::None) {
            text.push_back('/');
            text += ToString(result.fileError);
        }
        break;
    case InstalledBuildProbeError::AddressLibraryIdentity:
        text += "address-library-identity/";
        text += ToString(result.fileError);
        break;
    }
    return text;
}

}
