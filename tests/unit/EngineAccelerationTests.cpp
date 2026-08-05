#include "renderer_core/EngineAcceleration.h"
#include "renderer_core/EngineLighting.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace vf::renderer;

namespace {

accel::GeometryDescV1 BaseGeometry(
    const std::uint64_t id = 0x1800'0000'0000'0001ull,
    const accel::GeometryOpacity opacity = accel::GeometryOpacity::Opaque)
{
    accel::GeometryDescV1 geometry{};
    geometry.geometryId = id;
    geometry.vertexCount = 300;
    geometry.indexCount = 900;
    geometry.firstIndex = 0;
    geometry.vertexOffset = 0;
    geometry.opacity = opacity;
    return geometry;
}

accel::BlasDescV1 BaseBlas(
    const std::uint64_t id = 0x1800'0000'0000'0010ull,
    const bool dynamic = false)
{
    accel::BlasDescV1 blas{};
    blas.blasId = id;
    blas.generation = 1;
    blas.dynamic = dynamic;
    blas.geometries.push_back(BaseGeometry());
    blas.boundsMinimum = {-1.0f, -2.0f, -0.5f};
    blas.boundsMaximum = {1.0f, 2.0f, 0.5f};
    return blas;
}

// The GPU records use raw float arrays so their layout matches the shader
// block exactly, which a std::array would not guarantee.
void SetVec4(float (&destination)[4], const float x, const float y,
    const float z, const float w) noexcept
{
    destination[0] = x;
    destination[1] = y;
    destination[2] = z;
    destination[3] = w;
}

accel::InstanceDescV1 BaseInstance(
    const std::uint64_t id = 0x1800'0000'0000'0100ull,
    const std::uint64_t blasId = 0x1800'0000'0000'0010ull)
{
    accel::InstanceDescV1 instance{};
    instance.instanceId = id;
    instance.blasId = blasId;
    // Row-major 3x4 affine, identity with no translation.
    instance.transform = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f};
    instance.mask = accel::kInstanceMaskOpaque;
    instance.customIndex = 7;
    return instance;
}

}

TEST_CASE("P18_build_sizes_honour_the_device_scratch_alignment",
    "[phase18][accel]")
{
    // The live capability report gives accelerationStructureScratchAlignment
    // 128 on this device. Scratch that is not aligned to it is a validation
    // error at build time, not a slow path, so the query rounds up rather
    // than reporting the raw size.
    accel::DeviceLimits limits{};
    limits.scratchAlignment = 128;
    limits.structureAlignment = 256;

    const auto blas = BaseBlas();
    accel::BuildSizes sizes{};
    REQUIRE(accel::QueryBuildSizes(blas, limits, sizes) ==
        accel::AccelError::None);
    CHECK(sizes.structureBytes % limits.structureAlignment == 0);
    CHECK(sizes.buildScratchBytes % limits.scratchAlignment == 0);
    CHECK(sizes.structureBytes > 0);
    CHECK(sizes.buildScratchBytes > 0);
    // A static structure is never updated in place, so it needs no update
    // scratch at all; reserving it would be pure waste.
    CHECK(sizes.updateScratchBytes == 0);

    auto dynamic = BaseBlas(0x1800'0000'0000'0011ull, true);
    accel::BuildSizes dynamicSizes{};
    REQUIRE(accel::QueryBuildSizes(dynamic, limits, dynamicSizes) ==
        accel::AccelError::None);
    CHECK(dynamicSizes.updateScratchBytes > 0);
    CHECK(dynamicSizes.updateScratchBytes % limits.scratchAlignment == 0);

    // A zero alignment would divide by zero when rounding.
    accel::DeviceLimits degenerate{};
    degenerate.scratchAlignment = 0;
    degenerate.structureAlignment = 256;
    accel::BuildSizes rejected{};
    CHECK(accel::QueryBuildSizes(blas, degenerate, rejected) ==
        accel::AccelError::InvalidAlignment);

    // An alignment that is not a power of two cannot be rounded to by
    // masking, and every device reports a power of two.
    accel::DeviceLimits odd{};
    odd.scratchAlignment = 96;
    odd.structureAlignment = 256;
    CHECK(accel::QueryBuildSizes(blas, odd, rejected) ==
        accel::AccelError::InvalidAlignment);

    auto empty = blas;
    empty.geometries.clear();
    CHECK(accel::QueryBuildSizes(empty, limits, rejected) ==
        accel::AccelError::EmptyGeometry);
}

TEST_CASE("P18_update_is_chosen_only_when_topology_is_unchanged",
    "[phase18][accel]")
{
    // Refitting a structure whose triangle count changed produces a
    // structure that no longer matches its geometry, and the corruption is
    // silent: rays simply miss. Topology change must force a rebuild.
    const auto previous = BaseBlas(0x1800'0000'0000'0020ull, true);

    auto samePose = previous;
    samePose.generation = 2;
    CHECK(accel::DecideBuild(previous, samePose) == accel::BuildMode::Update);

    auto movedVertices = previous;
    movedVertices.generation = 2;
    movedVertices.boundsMaximum = {2.0f, 3.0f, 1.0f};
    CHECK(accel::DecideBuild(previous, movedVertices) ==
        accel::BuildMode::Update);

    auto differentTriangleCount = previous;
    differentTriangleCount.generation = 2;
    differentTriangleCount.geometries[0].indexCount = 903;
    CHECK(accel::DecideBuild(previous, differentTriangleCount) ==
        accel::BuildMode::Rebuild);

    auto differentGeometryCount = previous;
    differentGeometryCount.generation = 2;
    differentGeometryCount.geometries.push_back(
        BaseGeometry(0x1800'0000'0000'0002ull));
    CHECK(accel::DecideBuild(previous, differentGeometryCount) ==
        accel::BuildMode::Rebuild);

    // Opacity decides which hit groups run, so changing it changes the
    // structure's flags and cannot be refitted either.
    auto differentOpacity = previous;
    differentOpacity.generation = 2;
    differentOpacity.geometries[0].opacity =
        accel::GeometryOpacity::AlphaTested;
    CHECK(accel::DecideBuild(previous, differentOpacity) ==
        accel::BuildMode::Rebuild);

    // A static structure is never refitted, whatever changed.
    const auto staticPrevious = BaseBlas(0x1800'0000'0000'0021ull, false);
    auto staticNext = staticPrevious;
    staticNext.generation = 2;
    CHECK(accel::DecideBuild(staticPrevious, staticNext) ==
        accel::BuildMode::Rebuild);

    // A structure that has never been built has nothing to refit.
    CHECK(accel::DecideBuild({}, samePose) == accel::BuildMode::Rebuild);
}

TEST_CASE("P18_only_static_structures_are_compacted",
    "[phase18][accel]")
{
    // Compaction moves a structure into a smaller allocation. A structure
    // that is refitted every frame would have to be rebuilt to be compacted
    // again, so compacting it costs more than it saves.
    const auto staticBlas = BaseBlas(0x1800'0000'0000'0030ull, false);
    const auto dynamicBlas = BaseBlas(0x1800'0000'0000'0031ull, true);
    CHECK(accel::ShouldCompact(staticBlas));
    CHECK_FALSE(accel::ShouldCompact(dynamicBlas));
}

TEST_CASE("P18_instance_transforms_are_validated_not_repaired",
    "[phase18][accel]")
{
    const auto instance = BaseInstance();
    CHECK(accel::ValidateInstance(instance) == accel::AccelError::None);

    // A mirrored instance is legal and keeps its winding reversal; the
    // determinant sign is information, not an error.
    auto mirrored = instance;
    mirrored.transform[0] = -1.0f;
    CHECK(accel::ValidateInstance(mirrored) == accel::AccelError::None);
    CHECK(accel::InstanceDeterminant(mirrored) < 0.0f);
    CHECK(accel::InstanceDeterminant(instance) > 0.0f);

    // A singular transform collapses the instance to zero volume; rays would
    // intersect nothing and the structure would be built for nothing.
    auto singular = instance;
    singular.transform[0] = 0.0f;
    singular.transform[1] = 0.0f;
    singular.transform[2] = 0.0f;
    CHECK(accel::ValidateInstance(singular) ==
        accel::AccelError::SingularTransform);

    auto nonFinite = instance;
    nonFinite.transform[7] = std::numeric_limits<float>::infinity();
    CHECK(accel::ValidateInstance(nonFinite) ==
        accel::AccelError::NonFiniteSource);

    // A zero mask makes the instance invisible to every ray, which is almost
    // always a capture bug rather than an intent.
    auto invisible = instance;
    invisible.mask = 0;
    CHECK(accel::ValidateInstance(invisible) ==
        accel::AccelError::InvalidMask);

    // The custom index is 24 bits in the Vulkan instance record; a larger
    // one would silently truncate and point at the wrong material.
    auto overflowing = instance;
    overflowing.customIndex = 1u << 24;
    CHECK(accel::ValidateInstance(overflowing) ==
        accel::AccelError::CustomIndexOutOfRange);

    auto orphan = instance;
    orphan.blasId = 0;
    CHECK(accel::ValidateInstance(orphan) ==
        accel::AccelError::InvalidIdentity);
}

TEST_CASE("P18_transformed_bounds_enclose_every_rotated_corner",
    "[phase18][accel]")
{
    // A rotated box's axis-aligned bounds are not its bounds rotated: all
    // eight corners have to be transformed, or the TLAS bound clips geometry
    // that is really inside it.
    const auto blas = BaseBlas();
    auto rotated = BaseInstance();
    // 45 degrees about Z.
    const auto c = 0.70710678f;
    rotated.transform = {
        c, -c, 0.0f, 10.0f,
        c, c, 0.0f, -4.0f,
        0.0f, 0.0f, 1.0f, 2.5f};

    accel::Bounds worldBounds{};
    REQUIRE(accel::TransformBounds(blas, rotated, worldBounds) ==
        accel::AccelError::None);
    // Rotating a 2x4 box by 45 degrees widens it in X beyond its local half
    // extent of 1.
    CHECK(worldBounds.minimum[0] < 10.0f - 1.0f);
    CHECK(worldBounds.maximum[0] > 10.0f + 1.0f);
    CHECK(worldBounds.minimum[2] == Catch::Approx(2.0f));
    CHECK(worldBounds.maximum[2] == Catch::Approx(3.0f));
    for (std::size_t axis = 0; axis < 3; ++axis) {
        CHECK(worldBounds.minimum[axis] <= worldBounds.maximum[axis]);
    }

    // Every one of the eight corners must land inside the reported bounds.
    for (int corner = 0; corner < 8; ++corner) {
        const std::array<float, 3> local{
            (corner & 1) ? blas.boundsMaximum[0] : blas.boundsMinimum[0],
            (corner & 2) ? blas.boundsMaximum[1] : blas.boundsMinimum[1],
            (corner & 4) ? blas.boundsMaximum[2] : blas.boundsMinimum[2]};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto value =
                rotated.transform[axis * 4 + 0] * local[0] +
                rotated.transform[axis * 4 + 1] * local[1] +
                rotated.transform[axis * 4 + 2] * local[2] +
                rotated.transform[axis * 4 + 3];
            CHECK(value >= worldBounds.minimum[axis] - 1.0e-4f);
            CHECK(value <= worldBounds.maximum[axis] + 1.0e-4f);
        }
    }

    auto inverted = blas;
    inverted.boundsMinimum = {1.0f, 0.0f, 0.0f};
    inverted.boundsMaximum = {-1.0f, 0.0f, 0.0f};
    accel::Bounds rejected{};
    CHECK(accel::TransformBounds(inverted, rotated, rejected) ==
        accel::AccelError::InvalidBounds);
}

TEST_CASE("P18_geometry_opacity_drives_the_any_hit_decision",
    "[phase18][accel]")
{
    // An opaque geometry marked non-opaque runs an any-hit shader for every
    // intersection it will never reject, which is pure cost. A cutout marked
    // opaque loses its holes and casts a solid shadow.
    CHECK(accel::RequiresAnyHit(BaseGeometry(
        0x1800'0000'0000'0040ull, accel::GeometryOpacity::Opaque)) == false);
    CHECK(accel::RequiresAnyHit(BaseGeometry(
        0x1800'0000'0000'0041ull, accel::GeometryOpacity::AlphaTested)));
    // Blended geometry is classified but casts no ray-traced shadow in this
    // phase, so it must not silently become an opaque occluder.
    CHECK(accel::RequiresAnyHit(BaseGeometry(
        0x1800'0000'0000'0042ull, accel::GeometryOpacity::Blended)));
    CHECK_FALSE(accel::ParticipatesInShadows(BaseGeometry(
        0x1800'0000'0000'0043ull, accel::GeometryOpacity::Blended)));
    CHECK(accel::ParticipatesInShadows(BaseGeometry(
        0x1800'0000'0000'0044ull, accel::GeometryOpacity::AlphaTested)));
}

TEST_CASE("P18_alpha_candidates_resolve_through_the_phase15_coverage_rule",
    "[phase18][accel]")
{
    // The plan requires reusing the coverage function rather than writing a
    // second alpha test. Two rules would drift, and a shadow silhouette that
    // disagreed with the surface silhouette is exactly the artefact this
    // phase exists to avoid.
    visibility::AlphaStateV1 cutout{};
    cutout.classification = visibility::AlphaClass::Tested;
    cutout.source = visibility::AlphaSource::BaseColorTexture;
    cutout.reference = 0.5f;
    cutout.constantAlpha = 1.0f;
    cutout.fade = 1.0f;

    CHECK(accel::ConfirmAlphaCandidate(cutout, 0.75f, 4, 9));
    CHECK_FALSE(accel::ConfirmAlphaCandidate(cutout, 0.25f, 4, 9));
    // The boundary sample survives, matching the engine's >= convention.
    CHECK(accel::ConfirmAlphaCandidate(cutout, 0.5f, 4, 9));

    visibility::AlphaStateV1 opaque{};
    opaque.classification = visibility::AlphaClass::Opaque;
    // An opaque surface never consults alpha, even a zero one.
    CHECK(accel::ConfirmAlphaCandidate(opaque, 0.0f, 0, 0));
}

TEST_CASE("P18_ray_origins_are_offset_along_the_geometric_normal",
    "[phase18][accel]")
{
    // Self-intersection makes a surface shadow itself, producing acne. The
    // offset uses the *geometric* normal, because a shading normal can point
    // into the surface and would push the origin below it.
    const std::array<float, 3> position{100.0f, -50.0f, 25.0f};
    const std::array<float, 3> geometric{0.0f, 0.0f, 1.0f};
    const auto origin = accel::OffsetRayOrigin(position, geometric, 1.0f);
    CHECK(origin[2] > position[2]);
    CHECK(origin[0] == Catch::Approx(position[0]));
    CHECK(origin[1] == Catch::Approx(position[1]));

    // The offset scales with distance from the origin, because float spacing
    // does: a fixed epsilon that works near the camera is invisible far away.
    const std::array<float, 3> distant{100000.0f, 0.0f, 0.0f};
    const auto nearOffset = accel::OffsetRayOrigin(
        {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 1.0f)[0] - 1.0f;
    const auto farOffset =
        accel::OffsetRayOrigin(distant, {1.0f, 0.0f, 0.0f}, 1.0f)[0] -
        100000.0f;
    CHECK(farOffset > nearOffset);

    // A degenerate normal cannot offset in any direction, so the origin is
    // returned unchanged rather than moved somewhere arbitrary.
    const auto unchanged =
        accel::OffsetRayOrigin(position, {0.0f, 0.0f, 0.0f}, 1.0f);
    CHECK(unchanged[2] == Catch::Approx(position[2]));
}

TEST_CASE("P18_tlas_generations_and_retirement_never_reference_a_stale_build",
    "[phase18][accel]")
{
    // A camera cut or resize retires the TLAS. Tracing against a retired
    // structure is a use-after-free that validation may not catch, so the
    // schedule refuses it explicitly.
    accel::AccelSchedule schedule{};
    const auto first = schedule.BeginBuild(1);
    CHECK(first.generation == 1);
    CHECK(schedule.Traceable(first));

    schedule.Retire(first);
    CHECK_FALSE(schedule.Traceable(first));

    const auto second = schedule.BeginBuild(2);
    CHECK(second.generation == 2);
    CHECK(schedule.Traceable(second));
    // The retired handle stays refused even after a newer build exists.
    CHECK_FALSE(schedule.Traceable(first));

    // A structure still in flight cannot be destroyed; its scratch is live.
    CHECK_FALSE(schedule.CanDestroy(second, 0));
    schedule.SignalCompleted(second, 5);
    CHECK(schedule.CanDestroy(second, 5));
    CHECK_FALSE(schedule.CanDestroy(second, 4));
}

TEST_CASE("P18_shadow_term_becomes_available_and_masks_nothing",
    "[phase18][accel]")
{
    // Phase 17 declared the term unavailable so parity could mask it. This
    // phase supplies it, so the mask must come off; leaving it on would hide
    // a broken shadow from every comparison that follows.
    CHECK(accel::ShadowTermAvailable());
    CHECK_FALSE(lighting::ShadowTermAvailable());
    CHECK(accel::ShadowMaskRequired() == false);
}

TEST_CASE("P18_acceleration_packet_round_trips_and_rejects_corruption",
    "[phase18][accel]")
{
    accel::AccelPacket packet;
    packet.header.frameId = 0x1800'0000'0000'0007ull;
    packet.header.viewId = 0x1800'0000'0000'0009ull;
    for (std::uint32_t index = 0; index < 3; ++index) {
        auto instance = BaseInstance(
            0x1800'0000'0000'0100ull + index,
            0x1800'0000'0000'0010ull + index);
        instance.customIndex = index;
        packet.instances.push_back(instance);
    }
    REQUIRE(accel::ValidateAccelPacket(packet) ==
        accel::AccelPacketError::None);

    std::vector<std::byte> bytes;
    REQUIRE(accel::EncodeAccelPacket(packet, bytes) ==
        accel::AccelPacketError::None);
    accel::AccelPacket decoded;
    REQUIRE(accel::DecodeAccelPacket(bytes, decoded) ==
        accel::AccelPacketError::None);
    CHECK(decoded.instances.size() == packet.instances.size());

    std::vector<std::byte> reEncoded;
    REQUIRE(accel::EncodeAccelPacket(decoded, reEncoded) ==
        accel::AccelPacketError::None);
    CHECK(reEncoded == bytes);

    auto corrupted = bytes;
    corrupted[sizeof(accel::AccelPacketHeaderV1) + 4] ^= std::byte{0x11};
    accel::AccelPacket rejected;
    CHECK(accel::DecodeAccelPacket(corrupted, rejected) ==
        accel::AccelPacketError::ChecksumMismatch);

    auto duplicated = packet;
    duplicated.instances[1].instanceId = duplicated.instances[0].instanceId;
    CHECK(accel::ValidateAccelPacket(duplicated) ==
        accel::AccelPacketError::DuplicateInstance);

    CHECK(sizeof(accel::InstanceDescV1) == 80);
    CHECK(sizeof(accel::AccelPacketHeaderV1) == 48);
}

TEST_CASE("P18_shadow_rays_occlude_opaque_and_respect_cutout_holes",
    "[phase18][accel]")
{
    // A quad standing between the surface and the light, in camera-relative
    // space. Two triangles, because a single one would leave half the ray
    // budget untested.
    const auto quad = [](const accel::GeometryOpacity opacity,
                         const std::array<float, 3>& alphaAtVertex) {
        std::vector<accel::ShadowTriangle> triangles;
        accel::ShadowTriangle first{};
        first.a = {-5.0f, 10.0f, -5.0f};
        first.b = {5.0f, 10.0f, -5.0f};
        first.c = {5.0f, 10.0f, 5.0f};
        first.opacity = opacity;
        first.alpha.classification = opacity ==
            accel::GeometryOpacity::AlphaTested
            ? visibility::AlphaClass::Tested : visibility::AlphaClass::Opaque;
        first.alpha.source = visibility::AlphaSource::BaseColorTexture;
        first.alpha.reference = 0.5f;
        first.alpha.constantAlpha = 1.0f;
        first.alpha.fade = 1.0f;
        first.alphaAtVertex = alphaAtVertex;
        auto second = first;
        second.a = {-5.0f, 10.0f, -5.0f};
        second.b = {5.0f, 10.0f, 5.0f};
        second.c = {-5.0f, 10.0f, 5.0f};
        triangles.push_back(first);
        triangles.push_back(second);
        return triangles;
    };

    accel::ShadowRay ray{};
    ray.origin = {0.0f, 0.0f, 0.0f};
    ray.direction = {0.0f, 1.0f, 0.0f};
    ray.minimumDistance = 0.001f;
    ray.maximumDistance = 100.0f;

    const auto opaque = quad(accel::GeometryOpacity::Opaque,
        {1.0f, 1.0f, 1.0f});
    const auto blockedByOpaque = accel::TraceShadowRay(opaque, ray, 0, 0);
    CHECK(blockedByOpaque.occluded);
    CHECK(blockedByOpaque.distance == Catch::Approx(10.0f));
    CHECK(blockedByOpaque.alphaCandidates == 0);

    // A ray that stops short of the occluder is not blocked by it.
    auto shortRay = ray;
    shortRay.maximumDistance = 5.0f;
    CHECK_FALSE(accel::TraceShadowRay(opaque, shortRay, 0, 0).occluded);

    // Pointing away from the occluder reaches nothing.
    auto away = ray;
    away.direction = {0.0f, -1.0f, 0.0f};
    CHECK_FALSE(accel::TraceShadowRay(opaque, away, 0, 0).occluded);

    // A cutout whose alpha clears the reference still blocks.
    const auto solidCutout = quad(accel::GeometryOpacity::AlphaTested,
        {1.0f, 1.0f, 1.0f});
    const auto blockedByCutout =
        accel::TraceShadowRay(solidCutout, ray, 0, 0);
    CHECK(blockedByCutout.occluded);
    CHECK(blockedByCutout.alphaCandidates == 1);

    // A cutout whose alpha fails the reference lets the light through. This
    // is the whole point of confirming candidates rather than treating every
    // intersection as an occlusion.
    const auto holedCutout = quad(accel::GeometryOpacity::AlphaTested,
        {0.0f, 0.0f, 0.0f});
    const auto throughHole = accel::TraceShadowRay(holedCutout, ray, 0, 0);
    CHECK_FALSE(throughHole.occluded);
    CHECK(throughHole.alphaCandidates == 2);

    // Blended geometry is not an occluder, so glass does not cast a solid
    // shadow.
    const auto glass = quad(accel::GeometryOpacity::Blended,
        {1.0f, 1.0f, 1.0f});
    const auto throughGlass = accel::TraceShadowRay(glass, ray, 0, 0);
    CHECK_FALSE(throughGlass.occluded);
    CHECK(throughGlass.testedTriangles == 0);

    // A degenerate direction cannot be traced and reports no occlusion
    // rather than an arbitrary one.
    auto degenerate = ray;
    degenerate.direction = {0.0f, 0.0f, 0.0f};
    CHECK_FALSE(accel::TraceShadowRay(opaque, degenerate, 0, 0).occluded);
}

TEST_CASE("P18_a_light_casts_exactly_one_shadow_ray_rule",
    "[phase18][accel]")
{
    // One rule for the ray a light casts. The GPU query and this oracle
    // read the same light record, so if they disagreed about the origin
    // offset, the direction, or how far the ray reaches, the shadow the
    // mirror draws would not be the shadow the comparison predicts.
    const std::array<float, 3> position{2.0f, 3.0f, 6.0f};
    const std::array<float, 3> normal{0.0f, 0.0f, 1.0f};

    lighting::GpuLightRecordV1 directional{};
    SetVec4(directional.color, 1.0f, 1.0f, 1.0f,
        static_cast<float>(lighting::LightType::Directional));
    // The record stores the direction the light travels.
    SetVec4(directional.direction, 0.0f, 0.0f, -1.0f, 0.0f);

    const auto sunRay =
        accel::ShadowRayForLight(directional, position, normal);
    // The origin is the offset origin, never the surface point itself.
    const auto offset = accel::OffsetRayOrigin(position, normal, 1.0f);
    CHECK(sunRay.origin == offset);
    // Toward the light is the negation of the direction it travels.
    CHECK(sunRay.direction[2] == Catch::Approx(1.0f));
    CHECK(sunRay.minimumDistance == Catch::Approx(0.0f));
    // A directional light has no position, so the ray must reach past any
    // occluder in the scene rather than stop at an arbitrary distance.
    CHECK(sunRay.maximumDistance == accel::kDirectionalShadowDistance);

    lighting::GpuLightRecordV1 point{};
    SetVec4(point.color, 1.0f, 1.0f, 1.0f,
        static_cast<float>(lighting::LightType::Point));
    SetVec4(point.position, 2.0f, 3.0f, 12.0f, 0.0f);
    SetVec4(point.attenuation, 1.0f, 0.0f, 0.0f, 100.0f);

    const auto pointRay = accel::ShadowRayForLight(point, position, normal);
    CHECK(pointRay.direction[2] == Catch::Approx(1.0f));
    // The ray stops at the light. Anything beyond it is behind the light and
    // cannot shadow the surface, so tracing further would invent occluders.
    const auto expectedDistance = 12.0f - offset[2];
    CHECK(pointRay.maximumDistance == Catch::Approx(expectedDistance));

    // A light sitting exactly on the offset origin leaves no direction to
    // normalize. The ray must come back zero length rather than carrying a
    // division by zero into the trace.
    lighting::GpuLightRecordV1 coincident{point};
    SetVec4(coincident.position, offset[0], offset[1], offset[2], 0.0f);
    const auto degenerate =
        accel::ShadowRayForLight(coincident, position, normal);
    CHECK(degenerate.maximumDistance == Catch::Approx(0.0f));
    CHECK(std::isfinite(degenerate.direction[0]));
    CHECK(std::isfinite(degenerate.direction[1]));
    CHECK(std::isfinite(degenerate.direction[2]));
}

TEST_CASE("P18_shadowing_darkens_direct_light_and_never_ambient",
    "[phase18][lighting]")
{
    // Ambient is not cast from anywhere, so there is no ray that could be
    // blocked. Shadowing it would black out the interior of every shadow
    // instead of leaving the ambient floor the engine actually shows.
    lighting::GpuEnvironmentV1 environment{};
    environment.flagsAndCount[0] = lighting::EnvironmentPresent;
    environment.flagsAndCount[1] = 2;
    SetVec4(environment.ambientAndFogNear, 0.0f, 0.0f, 0.0f, 1.0e9f);
    SetVec4(environment.sunDirectionAndFogFar, 0.0f, 0.0f, 0.0f, 2.0e9f);
    SetVec4(environment.fogColorAndPower, 0.0f, 0.0f, 0.0f, 1.0f);
    SetVec4(environment.moonColorAndFogMaximum, 0.0f, 0.0f, 0.0f, 0.0f);

    std::array<lighting::GpuLightRecordV1, 2> lights{};
    SetVec4(lights[0].color, 0.25f, 0.25f, 0.25f,
        static_cast<float>(lighting::LightType::Ambient));
    SetVec4(lights[1].color, 1.0f, 1.0f, 1.0f,
        static_cast<float>(lighting::LightType::Directional));
    SetVec4(lights[1].direction, 0.0f, 0.0f, -1.0f, 0.0f);

    const std::array<float, 3> albedo{1.0f, 1.0f, 1.0f};
    const std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    const std::array<float, 3> normal{0.0f, 0.0f, 1.0f};

    const auto unshadowed = lighting::ShadeSurfaceGpu(
        environment, lights, albedo, position, normal);

    // Fully occluding both entries must remove the directional contribution
    // and leave the ambient one untouched.
    const std::array<float, 2> occluded{0.0f, 0.0f};
    const auto shadowed = lighting::ShadeSurfaceGpu(
        environment, lights, albedo, position, normal, occluded);
    CHECK(shadowed[0] == Catch::Approx(0.25f));
    CHECK(unshadowed[0] == Catch::Approx(1.25f));

    // A partial term scales only the direct half.
    const std::array<float, 2> half{0.0f, 0.5f};
    const auto partial = lighting::ShadeSurfaceGpu(
        environment, lights, albedo, position, normal, half);
    CHECK(partial[0] == Catch::Approx(0.75f));

    // A shorter span than the light list leaves the remaining lights lit,
    // which is what a device without ray query supplies.
    const auto absent = lighting::ShadeSurfaceGpu(
        environment, lights, albedo, position, normal, {});
    CHECK(absent[0] == Catch::Approx(unshadowed[0]));
}
