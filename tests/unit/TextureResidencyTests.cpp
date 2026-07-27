#include "renderer_core/TextureResidency.h"

#include <catch2/catch_test_macros.hpp>

using namespace vf::renderer::texture;

TEST_CASE("phase8 mip publication waits for a contiguous completed tail", "[phase8][texture]")
{
    TextureResidency residency{4, 5};
    REQUIRE(residency.ScheduleUpload(4, 4, 10) == ResidencyError::None);
    REQUIRE(residency.ScheduleUpload(4, 2, 12) == ResidencyError::None);
    REQUIRE(residency.ScheduleUpload(4, 3, 11) == ResidencyError::None);
    REQUIRE(residency.ScheduleUpload(4, 1, 14) == ResidencyError::None);

    REQUIRE(residency.Advance(4, 9) == ResidencyError::None);
    CHECK_FALSE(residency.Published().has_value());
    REQUIRE(residency.Advance(4, 10) == ResidencyError::None);
    REQUIRE(residency.Published().has_value());
    CHECK(residency.Published()->baseMip == 4);
    CHECK(residency.Published()->levelCount == 1);
    const auto firstRevision = residency.Published()->revision;

    REQUIRE(residency.Advance(4, 12) == ResidencyError::None);
    CHECK(residency.Published()->baseMip == 2);
    CHECK(residency.Published()->levelCount == 3);
    CHECK(residency.Published()->revision > firstRevision);
    REQUIRE(residency.Advance(4, 14) == ResidencyError::None);
    CHECK(residency.Published()->baseMip == 1);
    CHECK(residency.Published()->levelCount == 4);
}

TEST_CASE("phase8 residency rejects stale generations and unsafe eviction", "[phase8][texture]")
{
    TextureResidency residency{8, 3};
    CHECK(residency.ScheduleUpload(7, 2, 1) ==
        ResidencyError::InvalidGeneration);
    CHECK(residency.ScheduleUpload(8, 3, 1) == ResidencyError::InvalidMip);
    REQUIRE(residency.ScheduleUpload(8, 2, 1) == ResidencyError::None);
    CHECK(residency.ScheduleUpload(8, 2, 2) ==
        ResidencyError::DuplicateUpload);
    REQUIRE(residency.Advance(8, 1) == ResidencyError::None);
    CHECK(residency.Evict(8, 1) == ResidencyError::NotResident);
    REQUIRE(residency.Evict(8, 2) == ResidencyError::None);
    CHECK_FALSE(residency.Published().has_value());
}

TEST_CASE("phase8 retired descriptor indices remain quarantined", "[phase8][texture]")
{
    DescriptorQuarantine descriptors{3}; // fallback 0, usable 1 and 2
    const auto first = descriptors.Acquire();
    const auto second = descriptors.Acquire();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first != 0);
    CHECK(*second != 0);
    CHECK_FALSE(descriptors.Acquire().has_value());
    REQUIRE(descriptors.Retire(*first, 20));
    CHECK(descriptors.IsQuarantined(*first));
    CHECK_FALSE(descriptors.Acquire().has_value());
    descriptors.Advance(19);
    CHECK_FALSE(descriptors.Acquire().has_value());
    descriptors.Advance(20);
    CHECK(descriptors.Acquire() == first);
    CHECK_FALSE(descriptors.Retire(0, 30));
}
