#include "renderer_api/StableId.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("P03_stable_ids_ignore_string_storage_addresses", "[unit][phase03]")
{
    const std::string first = "mesh/actors/powerarmor";
    const std::string second = first;
    REQUIRE(first.data() != second.data());

    const auto firstId = vf::renderer::MakeStableId(
        vf::renderer::StableIdDomain::Resource, first, 3);
    const auto secondId = vf::renderer::MakeStableId(
        vf::renderer::StableIdDomain::Resource, second, 3);
    CHECK(firstId);
    CHECK(firstId == secondId);
    CHECK(firstId.value == 0xF1B422F79EF2C2E1ull);
}

TEST_CASE("P03_stable_id_domain_and_generation_are_part_of_identity", "[unit][phase03]")
{
    const auto resource = vf::renderer::MakeStableId(
        vf::renderer::StableIdDomain::Resource, "shared-key", 1);
    const auto writer = vf::renderer::MakeStableId(
        vf::renderer::StableIdDomain::Writer, "shared-key", 1);
    const auto nextGeneration = vf::renderer::MakeStableId(
        vf::renderer::StableIdDomain::Resource, "shared-key", 2);

    CHECK(resource != writer);
    CHECK(resource != nextGeneration);
    CHECK(writer != nextGeneration);
}
