#include "renderer_host/HookManifest.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr std::array kExpected{
    std::byte{0x48}, std::byte{0x80}, std::byte{0xC4}};
constexpr std::array kMask{
    std::byte{0xFF}, std::byte{0xF0}, std::byte{0xFF}};

vf::renderer::HookSiteDescriptor MaskedSite()
{
    return {
        11,
        "masked",
        vf::renderer::HookSiteKind::MaskedCode,
        16,
        kExpected,
        kMask,
        0,
    };
}

vf::renderer::HookSiteDescriptor PointerSite()
{
    return {
        12,
        "vtable",
        vf::renderer::HookSiteKind::RelocatedPointer,
        40,
        {},
        {},
        60,
    };
}

std::vector<std::byte> MakeImage(const std::uintptr_t imageBase)
{
    std::vector<std::byte> image(96);
    image[16] = std::byte{0x48};
    image[17] = std::byte{0x8F};
    image[18] = std::byte{0xC4};
    const auto pointer = imageBase + 60;
    std::memcpy(image.data() + 40, &pointer, sizeof(pointer));
    return image;
}

}

TEST_CASE("P02_hook_manifest_validates_masked_code_and_relocated_pointer", "[unit][phase02]")
{
    constexpr std::uintptr_t imageBase = 0x140000000;
    auto image = MakeImage(imageBase);
    const std::array sites{MaskedSite(), PointerSite()};

    const auto report = vf::renderer::ValidateHookManifest(sites, image, imageBase);
    REQUIRE(report);
    CHECK(report.validatedCount == sites.size());
}

TEST_CASE("P02_hook_manifest_reports_exact_failed_predicate", "[unit][phase02]")
{
    constexpr std::uintptr_t imageBase = 0x140000000;
    auto image = MakeImage(imageBase);
    image[17] = std::byte{0x7F};

    auto result = vf::renderer::ValidateHookSite(MaskedSite(), image, imageBase);
    CHECK_FALSE(result);
    CHECK(result.error == vf::renderer::HookSiteError::ByteMismatch);
    CHECK(result.siteId == 11);
    CHECK(result.mismatchOffset == 1);

    image = MakeImage(imageBase);
    const std::uintptr_t wrongPointer = imageBase + 61;
    std::memcpy(image.data() + 40, &wrongPointer, sizeof(wrongPointer));
    result = vf::renderer::ValidateHookSite(PointerSite(), image, imageBase);
    CHECK(result.error == vf::renderer::HookSiteError::PointerMismatch);
    CHECK(result.siteId == 12);
}

TEST_CASE("P02_hook_manifest_rejects_invalid_bounds_and_duplicate_ids", "[unit][phase02]")
{
    constexpr std::uintptr_t imageBase = 0x140000000;
    const auto image = MakeImage(imageBase);

    auto invalid = MaskedSite();
    invalid.mask = {};
    CHECK(vf::renderer::ValidateHookSite(invalid, image, imageBase).error ==
          vf::renderer::HookSiteError::InvalidDescriptor);

    invalid = MaskedSite();
    invalid.rva = 95;
    CHECK(vf::renderer::ValidateHookSite(invalid, image, imageBase).error ==
          vf::renderer::HookSiteError::OutOfBounds);

    const std::array duplicate{MaskedSite(), MaskedSite()};
    CHECK(vf::renderer::ValidateHookManifest(duplicate, image, imageBase)
              .failure.error == vf::renderer::HookSiteError::DuplicateId);
}

TEST_CASE("P02_target_hook_manifest_records_current_build_anchors", "[unit][phase02]")
{
    const auto manifest = vf::renderer::TargetHookManifest_1_11_221();
    REQUIRE(manifest.size() == 6);
    CHECK(manifest[0].rva == 0x018174F0u);
    CHECK(manifest[1].rva == 0x01817E30u);
    CHECK(manifest[2].rva == 0x01818080u);
    CHECK(manifest[3].rva == 0x021F16D0u);
    CHECK(manifest[4].rva == 0x0267DD60u);
    CHECK(manifest[4].expectedTargetRva == 0x016D2710u);
    CHECK(manifest[5].rva == 0x01818760u);
}
