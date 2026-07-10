#include "renderer_core/ResourceRegistry.h"

#include <catch2/catch_test_macros.hpp>

using namespace vf::renderer::resource;

TEST_CASE("phase7 resource generations reject duplicate and stale events", "[phase7][resource]")
{
    ResourceRegistry registry;
    ResourceHandle first{};
    REQUIRE(registry.Create(
        0x1000, ResourceKind::VertexBuffer, ResourceUsage::Immutable,
        256, 4, first) == ResourceEventError::None);
    CHECK(first.address == 0x1000);
    CHECK(first.generation == 1);

    ResourceHandle duplicate{};
    CHECK(registry.Create(
        0x1000, ResourceKind::VertexBuffer, ResourceUsage::Immutable,
        256, 4, duplicate) == ResourceEventError::DuplicateCreate);
    CHECK(registry.Update(first, 128, 5) ==
          ResourceEventError::ImmutableUpdate);
    REQUIRE(registry.Destroy(first, 9) == ResourceEventError::None);
    CHECK(registry.Lookup(first)->state == ResourceState::Retiring);
    CHECK(registry.Retire(8) == 0);
    CHECK(registry.Retire(9) == 1);
    CHECK_FALSE(registry.Lookup(first).has_value());

    ResourceHandle reused{};
    REQUIRE(registry.Create(
        0x1000, ResourceKind::VertexBuffer, ResourceUsage::Dynamic,
        512, 10, reused) == ResourceEventError::None);
    CHECK(reused.generation == 2);
    CHECK(registry.Update(first, 64, 11) == ResourceEventError::StaleHandle);
    CHECK(registry.Destroy(first, 12) == ResourceEventError::StaleHandle);
    CHECK(registry.Update(reused, 384, 12) == ResourceEventError::None);
    CHECK(registry.Lookup(reused)->byteSize == 384);
}

TEST_CASE("P12_resource_generations_are_exhausted_rather_than_wrapped",
    "[phase12][resource]")
{
    // A wrapped generation would let a stale handle alias a live resource,
    // so the handle space fails closed at its limit instead.
    ResourceRegistry registry{2};
    ResourceHandle first{};
    REQUIRE(registry.Create(0x2000, ResourceKind::TriShape,
        ResourceUsage::Immutable, 64, 1, first) ==
        ResourceEventError::None);
    CHECK(first.generation == 1);
    REQUIRE(registry.Destroy(first, 1) == ResourceEventError::None);
    CHECK(registry.Retire(1) == 1);

    ResourceHandle second{};
    REQUIRE(registry.Create(0x2000, ResourceKind::TriShape,
        ResourceUsage::Immutable, 64, 2, second) ==
        ResourceEventError::None);
    CHECK(second.generation == 2);
    REQUIRE(registry.Destroy(second, 2) == ResourceEventError::None);
    CHECK(registry.Retire(2) == 1);

    ResourceHandle exhausted{};
    CHECK(registry.Create(0x2000, ResourceKind::TriShape,
        ResourceUsage::Immutable, 64, 3, exhausted) ==
        ResourceEventError::GenerationExhausted);
    CHECK(exhausted.generation == 0);
    // An unrelated address keeps its own generation space.
    ResourceHandle other{};
    CHECK(registry.Create(0x3000, ResourceKind::TriShape,
        ResourceUsage::Immutable, 64, 3, other) ==
        ResourceEventError::None);
    CHECK(other.generation == 1);
}

TEST_CASE("phase7 resource retirement honors last in-flight use", "[phase7][resource]")
{
    ResourceRegistry registry;
    ResourceHandle handle{};
    REQUIRE(registry.Create(
        0x2000, ResourceKind::TriShape, ResourceUsage::Immutable,
        96, 20, handle) == ResourceEventError::None);
    REQUIRE(registry.Touch(handle, 27) == ResourceEventError::None);
    REQUIRE(registry.Destroy(handle, 22) == ResourceEventError::None);
    REQUIRE(registry.Lookup(handle));
    CHECK(registry.Lookup(handle)->retireValue == 27);
    CHECK(registry.Retire(26) == 0);
    CHECK(registry.RetiringCount() == 1);
    CHECK(registry.Retire(27) == 1);
    CHECK(registry.RetiringCount() == 0);
}

TEST_CASE("phase7 resource registry diagnoses illegal event ordering", "[phase7][resource]")
{
    ResourceRegistry registry;
    const ResourceHandle missing{0x3000, 1, ResourceKind::VertexBuffer};
    CHECK(registry.Update(missing, 64, 1) == ResourceEventError::MissingResource);
    CHECK(registry.Destroy(missing, 1) == ResourceEventError::MissingResource);

    ResourceHandle invalid{};
    CHECK(registry.Create(
        0, ResourceKind::VertexBuffer, ResourceUsage::Immutable,
        64, 1, invalid) == ResourceEventError::NullAddress);
    CHECK(registry.Create(
        0x3000, ResourceKind::VertexBuffer, ResourceUsage::Immutable,
        0, 1, invalid) == ResourceEventError::EmptyResource);

    ResourceHandle live{};
    REQUIRE(registry.Create(
        0x3000, ResourceKind::VertexBuffer, ResourceUsage::Dynamic,
        64, 3, live) == ResourceEventError::None);
    CHECK(registry.Update(live, 64, 2) == ResourceEventError::TimelineRegression);
    REQUIRE(registry.Destroy(live, 3) == ResourceEventError::None);
    CHECK(registry.Update(live, 64, 4) == ResourceEventError::ResourceRetiring);
    CHECK(registry.Destroy(live, 4) == ResourceEventError::ResourceRetiring);
}
