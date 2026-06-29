#include "renderer_host/BuildGate.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

template <class T>
void WriteLittle(std::vector<std::byte>& bytes, std::size_t offset, T value)
{
    REQUIRE(offset + sizeof(T) <= bytes.size());
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (static_cast<std::uint64_t>(value) >> (index * 8)) & 0xFFu);
    }
}

std::vector<std::byte> MakePeFixture()
{
    std::vector<std::byte> bytes(0x200);
    WriteLittle<std::uint16_t>(bytes, 0x00, 0x5A4D);
    WriteLittle<std::uint32_t>(bytes, 0x3C, 0x80);
    WriteLittle<std::uint32_t>(bytes, 0x80, 0x00004550);
    WriteLittle<std::uint16_t>(bytes, 0x84, 0x8664);
    WriteLittle<std::uint32_t>(bytes, 0x88, 0x69E2A744);
    WriteLittle<std::uint16_t>(bytes, 0x94, 0x00F0);
    WriteLittle<std::uint16_t>(bytes, 0x98, 0x020B);
    WriteLittle<std::uint32_t>(bytes, 0xD0, 0x04244000);
    return bytes;
}

}

TEST_CASE("P02_target_build_manifest_matches_mapped_install", "[unit][phase02]")
{
    const auto& target = vf::renderer::TargetBuild_1_11_221();

    CHECK(target.runtimeVersion == 0x010B0DD0u);
    CHECK(target.machine == 0x8664u);
    CHECK(target.timeDateStamp == 0x69E2A744u);
    CHECK(target.sizeOfImage == 0x04244000u);
    CHECK(target.executableFileSize == 55'293'864u);
    CHECK(vf::renderer::FormatSha256(target.executableSha256) ==
          "428F9996CC4248E26C0F62F9FDD3EAF0E5EB305834B67EE5996538E593218B61");
    CHECK(target.addressLibraryFileSize == 10'425'304u);
    CHECK(vf::renderer::FormatSha256(target.addressLibrarySha256) ==
          "2FDCC2A0926659C37255D2EAF335775240EC7FEFDF6CB3B35B063FACA25A448F");
}

TEST_CASE("P02_sha256_text_is_strict_and_round_trips", "[unit][phase02]")
{
    constexpr auto lower =
        "428f9996cc4248e26c0f62f9fdd3eaf0e5eb305834b67ee5996538e593218b61";
    const auto parsed = vf::renderer::ParseSha256(lower);

    REQUIRE(parsed);
    CHECK(vf::renderer::FormatSha256(parsed.value) ==
          "428F9996CC4248E26C0F62F9FDD3EAF0E5EB305834B67EE5996538E593218B61");
    CHECK(vf::renderer::ParseSha256("00").error ==
          vf::renderer::Sha256ParseError::WrongLength);

    std::string invalid(64, '0');
    invalid[17] = 'Z';
    CHECK(vf::renderer::ParseSha256(invalid).error ==
          vf::renderer::Sha256ParseError::InvalidCharacter);
}

TEST_CASE("P02_build_gate_accepts_only_the_exact_fingerprint", "[unit][phase02]")
{
    const auto expected = vf::renderer::TargetBuild_1_11_221();
    CHECK(vf::renderer::ValidateBuild(expected, expected).Accepted());
    CHECK(vf::renderer::FormatBuildGateReport(
              vf::renderer::ValidateBuild(expected, expected)) == "build-gate: accepted");

    auto observed = expected;
    observed.runtimeVersion ^= 1u;
    auto report = vf::renderer::ValidateBuild(expected, observed);
    CHECK(report.Has(vf::renderer::BuildMismatch::RuntimeVersion));
    CHECK(report.mismatchMask ==
          static_cast<std::uint32_t>(vf::renderer::BuildMismatch::RuntimeVersion));

    observed = expected;
    observed.machine ^= 1u;
    report = vf::renderer::ValidateBuild(expected, observed);
    CHECK(report.Has(vf::renderer::BuildMismatch::Machine));

    observed = expected;
    observed.timeDateStamp ^= 1u;
    report = vf::renderer::ValidateBuild(expected, observed);
    CHECK(report.Has(vf::renderer::BuildMismatch::TimeDateStamp));

    observed = expected;
    observed.sizeOfImage ^= 1u;
    report = vf::renderer::ValidateBuild(expected, observed);
    CHECK(report.Has(vf::renderer::BuildMismatch::SizeOfImage));

    observed = expected;
    ++observed.executableFileSize;
    report = vf::renderer::ValidateBuild(expected, observed);
    CHECK(report.Has(vf::renderer::BuildMismatch::ExecutableFileSize));

    observed = expected;
    observed.executableSha256.bytes[0] ^= 1u;
    report = vf::renderer::ValidateBuild(expected, observed);
    CHECK(report.Has(vf::renderer::BuildMismatch::ExecutableSha256));

    observed = expected;
    ++observed.addressLibraryFileSize;
    report = vf::renderer::ValidateBuild(expected, observed);
    CHECK(report.Has(vf::renderer::BuildMismatch::AddressLibraryFileSize));

    observed = expected;
    observed.addressLibrarySha256.bytes[31] ^= 1u;
    report = vf::renderer::ValidateBuild(expected, observed);
    CHECK(report.Has(vf::renderer::BuildMismatch::AddressLibrarySha256));
    CHECK(vf::renderer::FormatBuildGateReport(report).find("address-library-sha256") !=
          std::string::npos);
}

TEST_CASE("P02_pe_probe_reads_checked_pe32_plus_fields", "[unit][phase02]")
{
    auto bytes = MakePeFixture();
    const auto result = vf::renderer::ProbePeHeader(bytes);

    REQUIRE(result);
    CHECK(result.summary.machine == 0x8664u);
    CHECK(result.summary.timeDateStamp == 0x69E2A744u);
    CHECK(result.summary.sizeOfImage == 0x04244000u);

    bytes[0] = std::byte{0};
    CHECK(vf::renderer::ProbePeHeader(bytes).error == vf::renderer::PeProbeError::BadDosMagic);
}

TEST_CASE("P02_pe_probe_rejects_truncation_and_corruption", "[unit][phase02]")
{
    auto bytes = MakePeFixture();

    CHECK(vf::renderer::ProbePeHeader(std::span{bytes}.first(0x3F)).error ==
          vf::renderer::PeProbeError::TooSmall);

    auto invalidOffset = bytes;
    WriteLittle<std::uint32_t>(invalidOffset, 0x3C, 0x7FFFFFF0u);
    CHECK(vf::renderer::ProbePeHeader(invalidOffset).error ==
          vf::renderer::PeProbeError::BadNtOffset);

    auto badSignature = bytes;
    badSignature[0x80] = std::byte{0};
    CHECK(vf::renderer::ProbePeHeader(badSignature).error ==
          vf::renderer::PeProbeError::BadNtSignature);

    auto badMagic = bytes;
    WriteLittle<std::uint16_t>(badMagic, 0x98, 0x010B);
    CHECK(vf::renderer::ProbePeHeader(badMagic).error ==
          vf::renderer::PeProbeError::NotPe32Plus);

    CHECK(vf::renderer::ProbePeHeader(std::span{bytes}.first(0x90)).error ==
          vf::renderer::PeProbeError::TruncatedFileHeader);
    CHECK(vf::renderer::ProbePeHeader(std::span{bytes}.first(0xA0)).error ==
          vf::renderer::PeProbeError::TruncatedOptionalHeader);
}
