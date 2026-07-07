#include "renderer_host/ResourceManagerContract.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace vf::renderer;

TEST_CASE("phase7 resource-manager contract validates all 52 slots", "[phase7][hook]")
{
    constexpr std::uintptr_t base = 0x140000000ull;
    std::array<std::uintptr_t, kResourceManagerVtableSlots> observed{};
    const auto expected = TargetResourceManagerVtable_1_11_221();
    for (std::size_t index = 0; index < observed.size(); ++index) {
        observed[index] = base + expected[index];
    }
    const auto report = ValidateResourceManagerVtable(
        base, base + kResourceManagerVtableRva, observed);
    REQUIRE(report);
    CHECK(report.validatedSlots == kResourceManagerVtableSlots);
}

TEST_CASE("phase7 resource-manager contract reports exact drift", "[phase7][hook]")
{
    constexpr std::uintptr_t base = 0x140000000ull;
    std::array<std::uintptr_t, kResourceManagerVtableSlots> observed{};
    const auto expected = TargetResourceManagerVtable_1_11_221();
    for (std::size_t index = 0; index < observed.size(); ++index) {
        observed[index] = base + expected[index];
    }
    observed[40] += 1;
    auto report = ValidateResourceManagerVtable(
        base, base + kResourceManagerVtableRva, observed);
    CHECK(report.error == ResourceManagerContractError::SlotMismatch);
    CHECK(report.failedSlot == 40);
    CHECK(report.validatedSlots == 40);

    report = ValidateResourceManagerVtable(base, base + 1, observed);
    CHECK(report.error == ResourceManagerContractError::VtableMismatch);
    CHECK(report.validatedSlots == 0);
}

TEST_CASE("phase7 capture slots remain pinned to mapped entry points", "[phase7][hook]")
{
    const auto expected = TargetResourceManagerVtable_1_11_221();
    CHECK(kResourceManagerSingletonRva == 0x03438128u);
    CHECK(kResourceManagerVtableRva == 0x029139A8u);
    CHECK(expected[4] == 0x0226E2E0u);
    CHECK(expected[7] == 0x0226E3F0u);
    CHECK(expected[40] == 0x0226EFB0u);
    CHECK(expected[41] == 0x0226EFD0u);
}
