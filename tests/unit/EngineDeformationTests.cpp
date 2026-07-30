#include "renderer_core/EngineDeformation.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using namespace vf::renderer;

std::vector<std::array<float, 3>> BuildBaseVertices()
{
    return {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {1.0f, 1.0f, 0.0f},
    };
}

deform::BoneTransformV1 IdentityBone()
{
    deform::BoneTransformV1 bone{};
    bone.rows[0] = 1.0f;
    bone.rows[5] = 1.0f;
    bone.rows[10] = 1.0f;
    return bone;
}

deform::BoneTransformV1 TranslationBone(
    const float x,
    const float y,
    const float z)
{
    auto bone = IdentityBone();
    bone.rows[3] = x;
    bone.rows[7] = y;
    bone.rows[11] = z;
    return bone;
}

deform::DeformVertexV1 BuildInfluence(
    const std::uint32_t bone,
    const float weight,
    const float flexibility = 0.0f)
{
    deform::DeformVertexV1 vertex{};
    vertex.bones[0] = bone;
    vertex.weights[0] = weight;
    vertex.flexibility = flexibility;
    return vertex;
}

deform::DeformationPacket BuildPacket()
{
    deform::DeformationPacket packet{};
    packet.header.topologyId = 0xD0D0'0000'0000'0001ull;
    packet.header.generation = 1;
    packet.header.captureThreadId = 3;
    packet.header.renderThreadId = 3;
    for (std::uint32_t index = 0; index < 4; ++index) {
        packet.vertices.push_back(BuildInfluence(index < 2 ? 0 : 1, 1.0f));
    }
    packet.bones.push_back(IdentityBone());
    packet.bones.push_back(TranslationBone(0.0f, 2.0f, 0.0f));
    packet.previousBones.push_back(IdentityBone());
    packet.previousBones.push_back(TranslationBone(0.0f, 1.0f, 0.0f));
    return packet;
}

}

TEST_CASE("P13_skin_weights_normalize_explicitly_and_fail_closed",
    "[phase13][deform]")
{
    auto packet = BuildPacket();
    REQUIRE(deform::ValidateDeformationPacket(packet) ==
        deform::DeformError::None);

    // Weights that do not sum to one are normalized explicitly rather than
    // silently scaling the deformed position.
    auto scaled = packet;
    scaled.vertices[0].bones[1] = 1;
    scaled.vertices[0].weights[0] = 0.5f;
    scaled.vertices[0].weights[1] = 0.25f;
    deform::DeformationResult result{};
    REQUIRE(deform::EvaluateDeformation(scaled, BuildBaseVertices(),
        result) == deform::DeformError::None);
    CHECK(result.normalizedVertices == 1);
    // 2/3 of bone 0 and 1/3 of bone 1 (which translates by +2 in Y).
    CHECK(result.current[0][1] == Catch::Approx(2.0f / 3.0f));

    auto zeroed = packet;
    std::fill(std::begin(zeroed.vertices[1].weights),
        std::end(zeroed.vertices[1].weights), 0.0f);
    CHECK(deform::ValidateDeformationPacket(zeroed) ==
        deform::DeformError::InvalidWeights);

    auto negative = packet;
    negative.vertices[1].weights[0] = -0.5f;
    CHECK(deform::ValidateDeformationPacket(negative) ==
        deform::DeformError::InvalidWeights);

    auto outOfRange = packet;
    outOfRange.vertices[1].bones[0] = 9;
    CHECK(deform::ValidateDeformationPacket(outOfRange) ==
        deform::DeformError::InvalidBoneIndex);

    auto singular = packet;
    std::fill(std::begin(singular.bones[0].rows),
        std::end(singular.bones[0].rows), 0.0f);
    CHECK(deform::ValidateDeformationPacket(singular) ==
        deform::DeformError::InvalidMatrix);

    auto mismatched = packet;
    mismatched.previousBones.pop_back();
    CHECK(deform::ValidateDeformationPacket(mismatched) ==
        deform::DeformError::TopologyMismatch);
}

TEST_CASE("P13_skinning_morphs_and_wind_accumulate_in_a_defined_order",
    "[phase13][deform]")
{
    auto packet = BuildPacket();
    // A morph moves vertex 3 along +X before skinning applies.
    deform::MorphTargetV1 target{};
    target.firstDelta = 0;
    target.deltaCount = 1;
    target.weight = 0.5f;
    target.previousWeight = 0.0f;
    packet.morphTargets.push_back(target);
    deform::MorphDeltaV1 delta{};
    delta.vertexIndex = 3;
    delta.delta[0] = 4.0f;
    packet.morphDeltas.push_back(delta);
    packet.header.wind.direction[0] = 1.0f;
    packet.header.wind.amplitude = 0.0f;
    packet.header.wind.frequency = 1.0f;
    REQUIRE(deform::ValidateDeformationPacket(packet) ==
        deform::DeformError::None);

    deform::DeformationResult result{};
    REQUIRE(deform::EvaluateDeformation(packet, BuildBaseVertices(),
        result) == deform::DeformError::None);
    REQUIRE(result.current.size() == 4);
    // Vertex 0 rides the identity bone and is unchanged.
    CHECK(result.current[0][0] == Catch::Approx(0.0f));
    CHECK(result.current[0][1] == Catch::Approx(0.0f));
    // Vertex 2 rides the translating bone.
    CHECK(result.current[2][1] == Catch::Approx(3.0f));
    CHECK(result.previous[2][1] == Catch::Approx(2.0f));
    // Vertex 3 is morphed by half of a four unit delta, then skinned.
    CHECK(result.current[3][0] == Catch::Approx(3.0f));
    CHECK(result.previous[3][0] == Catch::Approx(1.0f));

    // Wind displaces flexible vertices only.
    auto windy = packet;
    windy.header.wind.amplitude = 0.5f;
    windy.header.wind.time = 0.25f;
    windy.header.wind.previousTime = 0.0f;
    windy.vertices[0].flexibility = 1.0f;
    deform::DeformationResult windResult{};
    REQUIRE(deform::EvaluateDeformation(windy, BuildBaseVertices(),
        windResult) == deform::DeformError::None);
    CHECK(windResult.current[0][0] != Catch::Approx(0.0f));
    CHECK(windResult.current[1][0] == Catch::Approx(result.current[1][0]));
    CHECK(windResult.previous[0][0] == Catch::Approx(0.0f));

    auto invalidMorph = packet;
    invalidMorph.morphTargets[0].weight = 1.5f;
    CHECK(deform::ValidateDeformationPacket(invalidMorph) ==
        deform::DeformError::InvalidMorph);
    invalidMorph = packet;
    invalidMorph.morphDeltas[0].vertexIndex = 9;
    CHECK(deform::ValidateDeformationPacket(invalidMorph) ==
        deform::DeformError::InvalidMorph);
    auto invalidWind = packet;
    invalidWind.header.wind.amplitude = -1.0f;
    CHECK(deform::ValidateDeformationPacket(invalidWind) ==
        deform::DeformError::InvalidWind);
}

TEST_CASE("P13_deformed_bounds_cover_current_and_previous_positions",
    "[phase13][deform]")
{
    auto packet = BuildPacket();
    deform::DeformationResult result{};
    REQUIRE(deform::EvaluateDeformation(packet, BuildBaseVertices(),
        result) == deform::DeformError::None);
    // Bind bounds are the unit square; skinning lifts half of it by two.
    CHECK(result.bounds.minimum[1] == Catch::Approx(0.0f));
    CHECK(result.bounds.maximum[1] == Catch::Approx(3.0f));
    // The previous pose only reached two, so the union must still cover it.
    CHECK(result.previousBounds.maximum[1] == Catch::Approx(2.0f));
    CHECK(result.bounds.maximum[1] >= result.previousBounds.maximum[1]);
    CHECK(result.motionMagnitude == Catch::Approx(1.0f));
}

TEST_CASE("P13_deformation_packets_round_trip_and_reject_corruption",
    "[phase13][deform]")
{
    const auto source = BuildPacket();
    std::vector<std::byte> first;
    std::vector<std::byte> second;
    REQUIRE(deform::EncodeDeformationPacket(source, first) ==
        deform::DeformError::None);
    REQUIRE(deform::EncodeDeformationPacket(source, second) ==
        deform::DeformError::None);
    CHECK(first == second);

    deform::DeformationPacket decoded;
    REQUIRE(deform::DecodeDeformationPacket(first, decoded) ==
        deform::DeformError::None);
    CHECK(decoded.header.topologyId == source.header.topologyId);
    CHECK(decoded.vertices.size() == source.vertices.size());
    CHECK(decoded.bones.size() == source.bones.size());

    first.back() ^= std::byte{0x20};
    CHECK(deform::DecodeDeformationPacket(first, decoded) ==
        deform::DeformError::ChecksumMismatch);
}

TEST_CASE("P13_topology_changes_require_a_new_generation",
    "[phase13][deform]")
{
    deform::TopologyRegistry registry;
    const auto packet = BuildPacket();
    REQUIRE(registry.Observe(packet) == deform::DeformError::None);
    // A fixed topology updates in place.
    CHECK(registry.Observe(packet) == deform::DeformError::None);

    auto grown = packet;
    grown.vertices.push_back(BuildInfluence(0, 1.0f));
    CHECK(registry.Observe(grown) == deform::DeformError::GenerationMismatch);

    grown.header.generation = 2;
    CHECK(registry.Observe(grown) == deform::DeformError::None);
    // A generation may not move backwards.
    CHECK(registry.Observe(packet) ==
        deform::DeformError::GenerationMismatch);
}

TEST_CASE("P13_dynamic_ring_wraps_without_overwriting_in_flight_ranges",
    "[phase13][deform]")
{
    deform::DynamicRing ring{256, 64};
    deform::RingAllocation allocation{};
    REQUIRE(ring.Allocate(128, 1, allocation));
    CHECK(allocation.offset == 0);
    CHECK(allocation.size == 128);
    REQUIRE(ring.Allocate(64, 2, allocation));
    CHECK(allocation.offset == 128);

    // The remaining 64 bytes cannot serve a 128 byte request, and wrapping
    // would land on ranges the GPU has not finished with.
    CHECK_FALSE(ring.Allocate(128, 3, allocation));
    CHECK(ring.InFlightBytes() == 192);

    ring.Retire(1);
    REQUIRE(ring.Allocate(128, 3, allocation));
    CHECK(allocation.offset == 0);
    CHECK(ring.InFlightBytes() == 192);

    // A completed timeline value retires every earlier allocation too.
    ring.Retire(2);
    CHECK(ring.InFlightBytes() == 128);
    // An oversized request can never be served by wrapping.
    CHECK_FALSE(ring.Allocate(512, 4, allocation));
    ring.Retire(3);
    CHECK(ring.InFlightBytes() == 0);
    // Alignment is honored on every allocation.
    REQUIRE(ring.Allocate(1, 5, allocation));
    CHECK(allocation.offset % 64 == 0);
}
