#include "renderer_host/WindowsBuildProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <filesystem>

TEST_CASE("P02_windows_sha256_matches_standard_vectors", "[unit][phase02]")
{
    constexpr std::array abc{
        std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    const auto abcHash = vf::renderer::HashSha256(abc);
    REQUIRE(abcHash);
    CHECK(vf::renderer::FormatSha256(abcHash.value) ==
          "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");

    const auto emptyHash = vf::renderer::HashSha256({});
    REQUIRE(emptyHash);
    CHECK(vf::renderer::FormatSha256(emptyHash.value) ==
          "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855");
}

TEST_CASE("P02_windows_file_probe_reports_missing_file", "[unit][phase02]")
{
    const auto result = vf::renderer::ProbeFileIdentity(
        std::filesystem::path{"Z:/visualforge/definitely-not-present.bin"});
    CHECK_FALSE(result);
    CHECK(result.error == vf::renderer::FileProbeError::NotFound);
}

TEST_CASE("P02_installed_build_probe_matches_recorded_install", "[unit][phase02]")
{
#ifdef VF_GAME_ROOT
    const std::filesystem::path root{VF_GAME_ROOT};
    const auto result = vf::renderer::ProbeInstalledBuild(
        0x010B0DD0u,
        root / "Fallout4.exe",
        root / "Data/F4SE/Plugins/version-1-11-221-0.bin");
    REQUIRE(result);
    const auto report = vf::renderer::ValidateBuild(
        vf::renderer::TargetBuild_1_11_221(),
        result.fingerprint);
    CHECK(report.Accepted());
    CHECK(vf::renderer::FormatInstalledBuildProbe(result) == "build-probe: complete");
#else
    SKIP("VF_GAME_ROOT was not configured");
#endif
}
