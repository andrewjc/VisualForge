#include "renderer_core/SceneDatabase.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace {

using namespace vf::renderer;

constexpr std::uint64_t kBudget = 64 * 1024;

scene::GeometryDesc BuildGeometry(
    const std::uint64_t address,
    const std::uint64_t contentHash,
    const resource::ResourceUsage usage = resource::ResourceUsage::Immutable,
    const std::uint64_t byteSize = 1024)
{
    scene::GeometryDesc geometry{};
    geometry.address = address;
    geometry.contentHash = contentHash;
    geometry.byteSize = byteSize;
    geometry.usage = usage;
    return geometry;
}

scene::InstanceDesc BuildInstance(
    const std::uint64_t sourceId,
    const std::uint32_t groupId,
    const float x = 0.0f)
{
    scene::InstanceDesc instance{};
    instance.sourceId = sourceId;
    instance.materialId = 0x9000ull + sourceId;
    instance.groupId = groupId;
    instance.passSequence = 1;
    instance.model[0] = 1.0f;
    instance.model[5] = 1.0f;
    instance.model[10] = 1.0f;
    instance.model[15] = 1.0f;
    instance.model[3] = x;
    instance.model[11] = 4.0f;
    instance.parameters = {1.0f, 1.0f, 1.0f, 1.0f};
    return instance;
}

std::size_t CountDeltas(
    const std::vector<scene::SceneDelta>& deltas,
    const scene::DeltaKind kind)
{
    return static_cast<std::size_t>(std::count_if(
        deltas.begin(), deltas.end(),
        [kind](const scene::SceneDelta& delta) {
            return delta.kind == kind;
        }));
}

}

TEST_CASE("P12_immutable_geometry_deduplicates_while_mutable_stays_independent",
    "[phase12][scene]")
{
    scene::SceneDatabase database{kBudget};
    REQUIRE(database.AttachGroup(7, 1) == scene::DatabaseError::None);

    scene::InstanceHandle first{};
    scene::InstanceHandle second{};
    REQUIRE(database.AddInstance(BuildGeometry(0x1000, 0xABCD),
        BuildInstance(1, 7, -1.0f), 1, first) ==
        scene::DatabaseError::None);
    // A different engine address with identical immutable content shares the
    // resident geometry instead of uploading a second copy.
    REQUIRE(database.AddInstance(BuildGeometry(0x2000, 0xABCD),
        BuildInstance(2, 7, 1.0f), 1, second) ==
        scene::DatabaseError::None);

    auto stats = database.Stats();
    CHECK(stats.aliveInstances == 2);
    CHECK(stats.residentGeometries == 1);
    CHECK(stats.sharedInstances == 1);
    CHECK(stats.residentBytes == 1024);
    CHECK(stats.pendingUploads == 1);

    // Independently mutable resources must never be folded together even
    // when their current contents hash identically.
    scene::InstanceHandle dynamicFirst{};
    scene::InstanceHandle dynamicSecond{};
    REQUIRE(database.AddInstance(
        BuildGeometry(0x3000, 0x5555, resource::ResourceUsage::Dynamic),
        BuildInstance(3, 7), 1, dynamicFirst) ==
        scene::DatabaseError::None);
    REQUIRE(database.AddInstance(
        BuildGeometry(0x4000, 0x5555, resource::ResourceUsage::Dynamic),
        BuildInstance(4, 7), 1, dynamicSecond) ==
        scene::DatabaseError::None);
    stats = database.Stats();
    CHECK(stats.residentGeometries == 3);
    CHECK(stats.residentBytes == 3072);
    CHECK(stats.sharedInstances == 1);

    // Identity is stable and distinct per instance.
    const auto firstRecord = database.Lookup(first);
    const auto secondRecord = database.Lookup(second);
    REQUIRE(firstRecord.has_value());
    REQUIRE(secondRecord.has_value());
    CHECK(firstRecord->objectId != 0);
    CHECK(firstRecord->objectId != secondRecord->objectId);
    CHECK(firstRecord->contentHash == 0xABCD);
    CHECK(firstRecord->descriptorIndex != secondRecord->descriptorIndex);
    CHECK_FALSE(firstRecord->resident);
}

TEST_CASE("P12_budget_and_capacity_fail_closed_without_partial_state",
    "[phase12][scene]")
{
    scene::SceneDatabase database{2048};
    REQUIRE(database.AttachGroup(1, 1) == scene::DatabaseError::None);
    scene::InstanceHandle handle{};
    REQUIRE(database.AddInstance(BuildGeometry(0x10, 0x01),
        BuildInstance(1, 1), 1, handle) == scene::DatabaseError::None);
    REQUIRE(database.AddInstance(BuildGeometry(0x20, 0x02),
        BuildInstance(2, 1), 1, handle) == scene::DatabaseError::None);

    scene::InstanceHandle rejected{};
    CHECK(database.AddInstance(BuildGeometry(0x30, 0x03),
        BuildInstance(3, 1), 1, rejected) ==
        scene::DatabaseError::BudgetExceeded);
    CHECK_FALSE(rejected.Valid());
    const auto stats = database.Stats();
    CHECK(stats.aliveInstances == 2);
    CHECK(stats.residentGeometries == 2);
    CHECK(stats.residentBytes == 2048);

    // A shared immutable copy still fits because it adds no bytes.
    scene::InstanceHandle shared{};
    CHECK(database.AddInstance(BuildGeometry(0x40, 0x01),
        BuildInstance(4, 1), 1, shared) == scene::DatabaseError::None);
    CHECK(database.Stats().residentBytes == 2048);

    CHECK(database.AddInstance(BuildGeometry(0x50, 0x05),
        BuildInstance(5, 9), 1, rejected) ==
        scene::DatabaseError::UnknownGroup);
    CHECK(database.AddInstance(BuildGeometry(0, 0x06),
        BuildInstance(6, 1), 1, rejected) ==
        scene::DatabaseError::InvalidIdentity);
    auto singular = BuildInstance(7, 1);
    singular.model[0] = 0.0f;
    CHECK(database.AddInstance(BuildGeometry(0x60, 0x07), singular, 1,
        rejected) == scene::DatabaseError::InvalidTransform);
    CHECK(database.Stats().aliveInstances == 3);
}

TEST_CASE("P12_instance_updates_rotate_previous_transform_and_keep_identity",
    "[phase12][scene]")
{
    scene::SceneDatabase database{kBudget};
    REQUIRE(database.AttachGroup(3, 1) == scene::DatabaseError::None);
    scene::InstanceHandle handle{};
    REQUIRE(database.AddInstance(BuildGeometry(0x100, 0x11),
        BuildInstance(1, 3, 2.0f), 1, handle) ==
        scene::DatabaseError::None);

    const auto initial = database.Lookup(handle);
    REQUIRE(initial.has_value());
    // First observation has no motion: previous equals current.
    CHECK(initial->model[3] == Catch::Approx(2.0f));
    CHECK(initial->previousModel[3] == Catch::Approx(2.0f));

    auto moved = BuildInstance(1, 3, 5.0f);
    REQUIRE(database.UpdateInstance(handle, moved.model, moved.parameters, 2) ==
        scene::DatabaseError::None);
    const auto updated = database.Lookup(handle);
    REQUIRE(updated.has_value());
    CHECK(updated->objectId == initial->objectId);
    CHECK(updated->model[3] == Catch::Approx(5.0f));
    CHECK(updated->previousModel[3] == Catch::Approx(2.0f));

    CHECK(database.UpdateInstance(handle, moved.model, moved.parameters, 1) ==
        scene::DatabaseError::TimelineRegression);
    scene::InstanceHandle stale{handle.slot, handle.generation + 1};
    CHECK(database.UpdateInstance(stale, moved.model, moved.parameters, 3) ==
        scene::DatabaseError::StaleHandle);
}

TEST_CASE("P12_cell_detach_retires_instances_and_releases_geometry_on_timeline",
    "[phase12][scene]")
{
    scene::SceneDatabase database{kBudget};
    REQUIRE(database.AttachGroup(1, 1) == scene::DatabaseError::None);
    REQUIRE(database.AttachGroup(2, 1) == scene::DatabaseError::None);
    CHECK(database.AttachGroup(1, 1) == scene::DatabaseError::DuplicateGroup);

    scene::InstanceHandle keptFirst{};
    scene::InstanceHandle keptSecond{};
    scene::InstanceHandle removed{};
    REQUIRE(database.AddInstance(BuildGeometry(0x100, 0x11),
        BuildInstance(1, 1), 1, keptFirst) == scene::DatabaseError::None);
    REQUIRE(database.AddInstance(BuildGeometry(0x200, 0x11),
        BuildInstance(2, 1), 1, keptSecond) == scene::DatabaseError::None);
    REQUIRE(database.AddInstance(BuildGeometry(0x300, 0x33),
        BuildInstance(3, 2), 1, removed) == scene::DatabaseError::None);
    REQUIRE(database.CompleteUpload(0x100, 1, 1) ==
        scene::DatabaseError::None);
    REQUIRE(database.CompleteUpload(0x300, 1, 1) ==
        scene::DatabaseError::None);
    CHECK(database.Stats().pendingUploads == 0);

    REQUIRE(database.DetachGroup(2, 10) == scene::DatabaseError::None);
    CHECK(database.DetachGroup(2, 10) == scene::DatabaseError::UnknownGroup);
    auto stats = database.Stats();
    CHECK(stats.attachedGroups == 1);
    CHECK(stats.aliveInstances == 2);
    CHECK(stats.retiringInstances == 1);
    // Retirement is timeline gated: nothing is released early.
    CHECK(database.Retire(9) == 0);
    CHECK(database.Stats().residentGeometries == 2);
    CHECK(database.Lookup(removed).has_value());

    CHECK(database.Retire(10) == 1);
    stats = database.Stats();
    CHECK(stats.retiringInstances == 0);
    CHECK(stats.residentGeometries == 1);
    CHECK(stats.residentBytes == 1024);
    CHECK_FALSE(database.Lookup(removed).has_value());
    // The surviving cell keeps both shared instances and their identity.
    CHECK(database.Lookup(keptFirst).has_value());
    CHECK(database.Lookup(keptSecond).has_value());
    CHECK(database.Lookup(keptFirst)->objectId !=
        database.Lookup(keptSecond)->objectId);
}

TEST_CASE("P12_late_completion_and_in_flight_unload_cannot_resurrect_geometry",
    "[phase12][scene]")
{
    scene::SceneDatabase database{kBudget};
    REQUIRE(database.AttachGroup(1, 1) == scene::DatabaseError::None);
    scene::InstanceHandle handle{};
    REQUIRE(database.AddInstance(BuildGeometry(0x400, 0x44),
        BuildInstance(1, 1), 1, handle) == scene::DatabaseError::None);
    CHECK(database.Stats().pendingUploads == 1);

    // Unloading while the upload is still in flight cancels it.
    REQUIRE(database.DetachGroup(1, 5) == scene::DatabaseError::None);
    CHECK(database.Stats().pendingUploads == 0);
    CHECK(database.CompleteUpload(0x400, 1, 6) ==
        scene::DatabaseError::UploadNotPending);

    // Re-adding at an address whose geometry is still retiring fails closed
    // instead of aliasing an in-flight resource.
    REQUIRE(database.AttachGroup(2, 5) == scene::DatabaseError::None);
    scene::InstanceHandle conflicting{};
    CHECK(database.AddInstance(BuildGeometry(0x400, 0x44),
        BuildInstance(9, 2), 5, conflicting) ==
        scene::DatabaseError::GeometryRetiring);
    CHECK(database.Retire(5) == 1);
    CHECK(database.Stats().residentGeometries == 0);
    CHECK(database.Stats().residentBytes == 0);

    // The address is reused for different content in a new generation.
    scene::InstanceHandle reused{};
    REQUIRE(database.AddInstance(BuildGeometry(0x400, 0x99),
        BuildInstance(2, 2), 6, reused) == scene::DatabaseError::None);
    // A completion for the retired generation must not resurrect it.
    CHECK(database.CompleteUpload(0x400, 1, 7) ==
        scene::DatabaseError::StaleHandle);
    CHECK(database.Stats().pendingUploads == 1);
    REQUIRE(database.CompleteUpload(0x400, 2, 7) ==
        scene::DatabaseError::None);
    CHECK(database.Stats().pendingUploads == 0);
    CHECK(database.CompleteUpload(0x400, 2, 8) ==
        scene::DatabaseError::UploadNotPending);
    CHECK(database.CancelUpload(0x400, 2) ==
        scene::DatabaseError::UploadNotPending);
}

TEST_CASE("P12_descriptor_slots_are_quarantined_until_the_timeline_passes",
    "[phase12][scene]")
{
    scene::SceneDatabase database{kBudget, 4};
    REQUIRE(database.AttachGroup(1, 1) == scene::DatabaseError::None);
    std::array<scene::InstanceHandle, 4> handles{};
    for (std::uint32_t index = 0; index < handles.size(); ++index) {
        REQUIRE(database.AddInstance(
            BuildGeometry(0x1000 + index, 0x77),
            BuildInstance(index + 1, 1), 1, handles[index]) ==
            scene::DatabaseError::None);
    }
    scene::InstanceHandle overflow{};
    CHECK(database.AddInstance(BuildGeometry(0x2000, 0x77),
        BuildInstance(9, 1), 1, overflow) ==
        scene::DatabaseError::CapacityExceeded);

    const auto released = database.Lookup(handles[0])->descriptorIndex;
    REQUIRE(database.RemoveInstance(handles[0], 20) ==
        scene::DatabaseError::None);
    CHECK(database.RemoveInstance(handles[0], 20) ==
        scene::DatabaseError::StaleHandle);
    // The freed slot stays quarantined until the GPU passes the retire value.
    CHECK(database.AddInstance(BuildGeometry(0x3000, 0x77),
        BuildInstance(10, 1), 21, overflow) ==
        scene::DatabaseError::CapacityExceeded);
    CHECK(database.Retire(20) == 1);
    REQUIRE(database.AddInstance(BuildGeometry(0x3000, 0x77),
        BuildInstance(10, 1), 21, overflow) ==
        scene::DatabaseError::None);
    CHECK(database.Lookup(overflow)->descriptorIndex == released);
    // Slot reuse mints a new identity so motion history cannot be inherited.
    CHECK(database.Lookup(overflow)->objectId != 0);
    CHECK_FALSE(database.Lookup(handles[0]).has_value());
}

TEST_CASE("P12_generation_rollover_retires_a_slot_instead_of_aliasing",
    "[phase12][scene]")
{
    // The handle space is deliberately tiny so exhaustion is reachable.
    scene::SceneDatabase database{kBudget, 1, 3};
    REQUIRE(database.AttachGroup(1, 1) == scene::DatabaseError::None);
    std::uint64_t previousObjectId{};
    for (std::uint32_t cycle = 0; cycle < 3; ++cycle) {
        scene::InstanceHandle handle{};
        REQUIRE(database.AddInstance(BuildGeometry(0x10, 0x01),
            BuildInstance(1, 1), cycle + 1, handle) ==
            scene::DatabaseError::None);
        const auto record = database.Lookup(handle);
        REQUIRE(record.has_value());
        CHECK(record->objectId != previousObjectId);
        previousObjectId = record->objectId;
        REQUIRE(database.RemoveInstance(handle, cycle + 1) ==
            scene::DatabaseError::None);
        CHECK(database.Retire(cycle + 1) == 1);
    }
    scene::InstanceHandle exhausted{};
    CHECK(database.AddInstance(BuildGeometry(0x10, 0x01),
        BuildInstance(1, 1), 9, exhausted) ==
        scene::DatabaseError::GenerationExhausted);
    CHECK_FALSE(exhausted.Valid());
}

TEST_CASE("P12_registry_deltas_record_every_lifecycle_transition",
    "[phase12][scene]")
{
    scene::SceneDatabase database{kBudget};
    REQUIRE(database.AttachGroup(1, 1) == scene::DatabaseError::None);
    scene::InstanceHandle first{};
    scene::InstanceHandle shared{};
    REQUIRE(database.AddInstance(BuildGeometry(0x100, 0x11),
        BuildInstance(1, 1), 1, first) == scene::DatabaseError::None);
    REQUIRE(database.AddInstance(BuildGeometry(0x200, 0x11),
        BuildInstance(2, 1), 1, shared) == scene::DatabaseError::None);
    REQUIRE(database.CompleteUpload(0x100, 1, 2) ==
        scene::DatabaseError::None);
    REQUIRE(database.DetachGroup(1, 3) == scene::DatabaseError::None);
    CHECK(database.Retire(3) == 2);

    const auto deltas = database.DrainDeltas();
    CHECK(CountDeltas(deltas, scene::DeltaKind::GroupAttached) == 1);
    CHECK(CountDeltas(deltas, scene::DeltaKind::InstanceAdded) == 2);
    CHECK(CountDeltas(deltas, scene::DeltaKind::GeometryShared) == 1);
    CHECK(CountDeltas(deltas, scene::DeltaKind::GeometryResident) == 1);
    CHECK(CountDeltas(deltas, scene::DeltaKind::GroupDetached) == 1);
    CHECK(CountDeltas(deltas, scene::DeltaKind::InstanceRetired) == 2);
    CHECK(CountDeltas(deltas, scene::DeltaKind::GeometryReleased) == 1);
    CHECK(database.DrainDeltas().empty());

    const auto added = std::find_if(deltas.begin(), deltas.end(),
        [](const scene::SceneDelta& delta) {
            return delta.kind == scene::DeltaKind::InstanceAdded;
        });
    REQUIRE(added != deltas.end());
    CHECK(added->groupId == 1);
    CHECK(added->contentHash == 0x11);
    CHECK(added->objectId != 0);
}

TEST_CASE("P12_scene_deltas_map_onto_pointer_free_trace_records",
    "[phase12][scene]")
{
    scene::SceneDatabase database{kBudget};
    REQUIRE(database.AttachGroup(5, 1) == scene::DatabaseError::None);
    scene::InstanceHandle handle{};
    REQUIRE(database.AddInstance(BuildGeometry(0x900, 0x1234),
        BuildInstance(1, 5), 11, handle) == scene::DatabaseError::None);
    const auto deltas = database.DrainDeltas();
    REQUIRE(deltas.size() == 2);

    const auto attached = scene::ToTraceRecord(deltas[0], 7);
    CHECK(attached.frameId == 7);
    CHECK(attached.groupId == 5);
    CHECK(attached.kind == static_cast<std::uint32_t>(
        vf::renderer::trace::RegistryDeltaKind::GroupAttached));

    const auto added = scene::ToTraceRecord(deltas[1], 7);
    CHECK(added.frameId == 7);
    CHECK(added.groupId == 5);
    CHECK(added.kind == static_cast<std::uint32_t>(
        vf::renderer::trace::RegistryDeltaKind::InstanceAdded));
    CHECK(added.objectId == database.Lookup(handle)->objectId);
    CHECK(added.contentHash == 0x1234);
    CHECK(added.byteSize == 1024);
    CHECK(added.generation == 1);
    CHECK(added.timelineValue == 11);
    CHECK(added.reserved == 0);
}

TEST_CASE("P12_repeated_load_unload_cycles_reach_a_stable_plateau",
    "[phase12][scene]")
{
    scene::SceneDatabase database{kBudget};
    std::uint64_t timeline = 1;
    std::uint64_t plateauBytes{};
    std::uint32_t plateauDescriptors{};
    for (std::uint32_t cycle = 0; cycle < 8; ++cycle) {
        REQUIRE(database.AttachGroup(100 + cycle, timeline) ==
            scene::DatabaseError::None);
        for (std::uint32_t index = 0; index < 4; ++index) {
            scene::InstanceHandle handle{};
            // Address reuse across cycles is deliberate.
            REQUIRE(database.AddInstance(
                BuildGeometry(0x8000 + index, 0x2000 + index),
                BuildInstance(index + 1, 100 + cycle), timeline, handle) ==
                scene::DatabaseError::None);
            REQUIRE(database.CompleteUpload(0x8000 + index, cycle + 1,
                timeline) == scene::DatabaseError::None);
        }
        const auto peak = database.Stats();
        REQUIRE(database.DetachGroup(100 + cycle, timeline) ==
            scene::DatabaseError::None);
        CHECK(database.Retire(timeline) == 4);
        const auto settled = database.Stats();
        CHECK(settled.aliveInstances == 0);
        CHECK(settled.retiringInstances == 0);
        CHECK(settled.residentGeometries == 0);
        CHECK(settled.residentBytes == 0);
        CHECK(settled.attachedGroups == 0);
        if (cycle == 1) {
            plateauBytes = peak.residentBytes;
            plateauDescriptors = peak.peakDescriptorIndex;
        } else if (cycle > 1) {
            CHECK(peak.residentBytes == plateauBytes);
            CHECK(peak.peakDescriptorIndex == plateauDescriptors);
        }
        ++timeline;
    }
    static_cast<void>(database.DrainDeltas());
}
