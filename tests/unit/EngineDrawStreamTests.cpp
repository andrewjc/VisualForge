#include "renderer_core/EngineDrawStream.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <vector>

using namespace vf::renderer;

namespace {

void SetModel(
    float (&model)[16],
    const float x,
    const float y,
    const float z)
{
    // Row-major, exactly as the vertex-shader constant buffer holds it.
    for (auto& value : model) value = 0.0f;
    model[0] = 1.0f;
    model[3] = x;
    model[5] = 1.0f;
    model[7] = y;
    model[10] = 1.0f;
    model[11] = z;
    model[15] = 1.0f;
}

drawstream::DrawRecordV1 BaseDraw(
    const std::uint64_t buffer = 0x2000'0000'0000'0001ull,
    const std::uint32_t startIndex = 0,
    const std::uint32_t indexCount = 900)
{
    drawstream::DrawRecordV1 draw{};
    draw.vertexBuffer = buffer;
    draw.indexBuffer = 0x2000'0000'0000'00F0ull;
    draw.vertexStride = 12;
    draw.indexCount = indexCount;
    draw.startIndex = startIndex;
    draw.baseVertex = 0;
    draw.instanceCount = 1;
    SetModel(draw.model, 30.0f, 210.0f, -93.0f);
    return draw;
}

}

TEST_CASE("PLS_draws_the_capture_did_not_see_are_holes_not_objects",
    "[livescene][drawstream]")
{
    drawstream::TranslationLimits limits{};
    const auto draw = BaseDraw();
    CHECK(drawstream::ValidateDraw(draw, limits) ==
        drawstream::DrawStreamError::None);

    // A zero buffer handle means the draw read geometry the capture never saw
    // bound. Translating it would put an object in the scene with no vertices
    // behind it, which is worse than leaving a hole and reporting it.
    auto unseen = draw;
    unseen.vertexBuffer = 0;
    CHECK(drawstream::ValidateDraw(unseen, limits) ==
        drawstream::DrawStreamError::UnknownVertexBuffer);

    auto noIndices = draw;
    noIndices.indexBuffer = 0;
    CHECK(drawstream::ValidateDraw(noIndices, limits) ==
        drawstream::DrawStreamError::UnknownIndexBuffer);

    auto empty = draw;
    empty.indexCount = 0;
    CHECK(drawstream::ValidateDraw(empty, limits) ==
        drawstream::DrawStreamError::EmptyGeometry);

    // Not a multiple of three: a strip or a line list, not a triangle list.
    // Translating it as triangles reads past the range and draws garbage.
    auto strip = draw;
    strip.indexCount = 901;
    CHECK(drawstream::ValidateDraw(strip, limits) ==
        drawstream::DrawStreamError::NotATriangleList);

    // A single draw larger than the cap is a pooled batch, not an object.
    auto batch = draw;
    batch.indexCount = limits.maximumIndicesPerDraw + 3;
    CHECK(drawstream::ValidateDraw(batch, limits) ==
        drawstream::DrawStreamError::IndexCountOutOfRange);

    auto zeroInstances = draw;
    zeroInstances.instanceCount = 0;
    CHECK(drawstream::ValidateDraw(zeroInstances, limits) ==
        drawstream::DrawStreamError::ZeroInstances);

    auto broken = draw;
    broken.model[3] = std::numeric_limits<float>::quiet_NaN();
    CHECK(drawstream::ValidateDraw(broken, limits) ==
        drawstream::DrawStreamError::NonFiniteTransform);

    // A singular transform collapses the object to zero volume, so it would
    // occupy the scene without ever being visible.
    auto singular = draw;
    singular.model[0] = 0.0f;
    singular.model[5] = 0.0f;
    singular.model[10] = 0.0f;
    CHECK(drawstream::ValidateDraw(singular, limits) ==
        drawstream::DrawStreamError::SingularTransform);
}

TEST_CASE("PLS_mesh_identity_is_the_pooled_range_not_the_transform",
    "[livescene][drawstream]")
{
    // The engine pools geometry, so what identifies a mesh is which buffer and
    // which range, never where it was drawn. Folding the transform in would
    // make every copy of a fence post a separate mesh and multiply the scene's
    // object count by however many are on screen.
    const auto first = BaseDraw();
    auto moved = first;
    SetModel(moved.model, -400.0f, 12.0f, 5.0f);
    CHECK(drawstream::MeshIdentity(first) == drawstream::MeshIdentity(moved));

    auto otherRange = first;
    otherRange.startIndex = 900;
    CHECK(drawstream::MeshIdentity(first) !=
        drawstream::MeshIdentity(otherRange));

    auto otherCount = first;
    otherCount.indexCount = 600;
    CHECK(drawstream::MeshIdentity(first) !=
        drawstream::MeshIdentity(otherCount));

    auto otherBuffer = first;
    otherBuffer.vertexBuffer = 0x2000'0000'0000'0002ull;
    CHECK(drawstream::MeshIdentity(first) !=
        drawstream::MeshIdentity(otherBuffer));

    // baseVertex shifts which vertices the same indices read, so it is part
    // of the mesh, not of the placement.
    auto otherBase = first;
    otherBase.baseVertex = 4096;
    CHECK(drawstream::MeshIdentity(first) !=
        drawstream::MeshIdentity(otherBase));

    // The byte offset IASetVertexBuffers bound the stream at. Two meshes at
    // different offsets in the same 128 MB pool can share every other field,
    // so leaving it out collapses them onto one identity: the first one read
    // is then served for both, and the second object is drawn with geometry
    // that belongs to something else somewhere else in the cell.
    auto otherOffset = first;
    otherOffset.vertexByteOffset = 8'192;
    CHECK(drawstream::MeshIdentity(first) !=
        drawstream::MeshIdentity(otherOffset));

    // And it reaches the extraction request, because it is half of the address
    // the readback has to use. Without it the read starts at the pool's base
    // and returns whatever geometry happens to live there.
    drawstream::DrawStreamFrame frame{};
    frame.draws.push_back(otherOffset);
    const auto plan = drawstream::PlanMeshExtraction(frame, {}, {}, {});
    REQUIRE(plan.requests.size() == 1);
    CHECK(plan.requests.front().vertexByteOffset == 8'192);

    // Two instances of one mesh must not collide, or the scene packet refuses
    // the frame for a duplicate object.
    const auto mesh = drawstream::MeshIdentity(first);
    CHECK(drawstream::InstanceIdentity(mesh, 0) !=
        drawstream::InstanceIdentity(mesh, 1));
    CHECK(drawstream::InstanceIdentity(mesh, 0) != 0);
}

TEST_CASE("PLS_translation_turns_repeated_meshes_into_instances",
    "[livescene][drawstream]")
{
    // A cell full of the same fence post must cost one object and many
    // instances. Emitting one object per draw is what makes a real frame's
    // four thousand draws overflow a scene packet that allows sixty-five
    // thousand objects but represents them far more cheaply as instances.
    drawstream::DrawStreamFrame frame{};
    frame.frameIndex = 41;
    for (int copy = 0; copy < 5; ++copy) {
        auto draw = BaseDraw();
        SetModel(draw.model, static_cast<float>(copy) * 100.0f, 0.0f, 0.0f);
        frame.draws.push_back(draw);
    }
    // A second, different mesh drawn once.
    frame.draws.push_back(BaseDraw(0x2000'0000'0000'0001ull, 900, 300));

    drawstream::TranslationLimits limits{};
    scene::ScenePacket packet{};
    drawstream::TranslationResult result{};
    REQUIRE(drawstream::TranslateDrawStream(frame, limits, packet, result) ==
        drawstream::DrawStreamError::None);

    CHECK(result.objects == 2);
    CHECK(result.instances == 6);
    CHECK(result.reusedMeshes == 4);
    CHECK(result.rejectedDraws == 0);
    CHECK(packet.objects.size() == 2);
    CHECK(packet.instances.size() == 6);

    // Every instance points at an object that exists.
    for (const auto& instance : packet.instances) {
        REQUIRE(instance.objectIndex < packet.objects.size());
    }

    // The five copies are grouped under one object and carry the transforms
    // their draws did, in submission order *within* that object.
    //
    // Located by counting rather than by position, because the packet's object
    // order is deliberately not the engine's submission order: the raster
    // packet is cached across frames and paired to the scene positionally, so
    // an order that follows the engine makes a cached frame resolve the wrong
    // material. Asserting a position here would pin the very thing that had to
    // stop depending on the engine.
    std::map<std::uint32_t, std::vector<float>> byObject;
    for (const auto& instance : packet.instances) {
        byObject[instance.objectIndex].push_back(instance.model[3]);
    }
    REQUIRE(byObject.size() == 2);
    const std::vector<float> expectedFive{0.0f, 100.0f, 200.0f, 300.0f,
        400.0f};
    auto sawFive = false;
    auto sawOne = false;
    for (const auto& [object, offsets] : byObject) {
        if (offsets.size() == 5) {
            sawFive = true;
            for (std::size_t copy = 0; copy < offsets.size(); ++copy) {
                CHECK(offsets[copy] == Catch::Approx(expectedFive[copy]));
            }
        } else if (offsets.size() == 1) {
            sawOne = true;
        }
    }
    CHECK(sawFive);
    CHECK(sawOne);

    // The packet the engine's own validator accepts, or the translation has
    // produced something no consumer can use.
    CHECK(scene::ValidateScenePacket(packet) ==
        scene::ScenePacketError::None);
    CHECK(scene::ValidateSceneInstances(packet) ==
        scene::ScenePacketError::None);
}

TEST_CASE("PLS_translation_reports_what_it_refused", "[livescene][drawstream]")
{
    // A frame that silently drops objects looks like a frame that rendered
    // everything. Both the arena's overflow and the validator's rejections
    // have to survive into the result.
    drawstream::DrawStreamFrame frame{};
    frame.frameIndex = 7;
    frame.droppedDraws = 12;
    frame.draws.push_back(BaseDraw());
    auto unseen = BaseDraw();
    unseen.vertexBuffer = 0;
    frame.draws.push_back(unseen);
    auto strip = BaseDraw();
    strip.indexCount = 902;
    frame.draws.push_back(strip);

    drawstream::TranslationLimits limits{};
    scene::ScenePacket packet{};
    drawstream::TranslationResult result{};
    REQUIRE(drawstream::TranslateDrawStream(frame, limits, packet, result) ==
        drawstream::DrawStreamError::None);
    CHECK(result.objects == 1);
    CHECK(result.instances == 1);
    CHECK(result.rejectedDraws == 2);
    CHECK(result.droppedDraws == 12);

    // Beyond the object limit the translation refuses rather than truncating
    // to something that looks like a complete scene.
    drawstream::TranslationLimits tight{};
    tight.maximumObjects = 1;
    drawstream::DrawStreamFrame crowded{};
    crowded.frameIndex = 8;
    crowded.draws.push_back(BaseDraw(0x2000'0000'0000'0001ull, 0, 900));
    crowded.draws.push_back(BaseDraw(0x2000'0000'0000'0001ull, 900, 900));
    scene::ScenePacket refused{};
    drawstream::TranslationResult ignored{};
    CHECK(drawstream::TranslateDrawStream(crowded, tight, refused, ignored) !=
        drawstream::DrawStreamError::None);

    // An empty frame is a valid answer to "what did this frame draw" only if
    // the packet says so rather than being left half built.
    drawstream::DrawStreamFrame nothing{};
    nothing.frameIndex = 9;
    scene::ScenePacket emptyPacket{};
    drawstream::TranslationResult emptyResult{};
    CHECK(drawstream::TranslateDrawStream(nothing, limits, emptyPacket,
        emptyResult) == drawstream::DrawStreamError::EmptyGeometry);
    CHECK(emptyResult.objects == 0);
}

TEST_CASE("PLS_extraction_reads_each_mesh_once_within_a_budget",
    "[livescene][drawstream]")
{
    // Reading geometry out of a pooled buffer means a staging copy and a map,
    // which synchronises the GPU. An unbounded readback turns a renderer into
    // a stutter, so the budget is part of the contract rather than a tuning
    // knob added later.
    drawstream::DrawStreamFrame frame{};
    frame.frameIndex = 12;
    for (std::uint32_t mesh = 0; mesh < 20; ++mesh) {
        frame.draws.push_back(
            BaseDraw(0x2000'0000'0000'0001ull, mesh * 900, 900));
    }
    // The same mesh drawn a second time must not be read twice.
    frame.draws.push_back(BaseDraw(0x2000'0000'0000'0001ull, 0, 900));

    drawstream::TranslationLimits limits{};
    drawstream::ExtractionBudget budget{};
    budget.maximumMeshesPerFrame = 8;

    const auto plan = drawstream::PlanMeshExtraction(frame, limits, {},
        budget);
    CHECK(plan.requests.size() == 8);
    CHECK(plan.deferred == 12);
    CHECK(plan.satisfied == 1);

    // Every request names a distinct mesh.
    for (std::size_t i = 0; i < plan.requests.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            CHECK(plan.requests[i].meshIdentity !=
                plan.requests[j].meshIdentity);
        }
    }

    // A mesh already held is never requested again: the pooled geometry
    // behind a range does not change once it has streamed in.
    std::vector<std::uint64_t> cached;
    for (const auto& request : plan.requests) {
        cached.push_back(request.meshIdentity);
    }
    const auto second = drawstream::PlanMeshExtraction(frame, limits, cached,
        budget);
    CHECK(second.requests.size() == 8);
    CHECK(second.satisfied == 9);
    for (const auto& request : second.requests) {
        CHECK(std::find(cached.begin(), cached.end(), request.meshIdentity) ==
            cached.end());
    }

    // A byte budget stops the frame before the mesh count does when the
    // meshes are large.
    drawstream::ExtractionBudget tight{};
    tight.maximumMeshesPerFrame = 8;
    tight.maximumIndexBytesPerFrame = 900 * sizeof(std::uint32_t) * 3;
    const auto bounded = drawstream::PlanMeshExtraction(frame, limits, {},
        tight);
    CHECK(bounded.requests.size() == 3);
    CHECK(bounded.deferred == 17);

    // A rejected draw is never a request: reading geometry for something the
    // translation refuses would spend a readback on nothing.
    drawstream::DrawStreamFrame broken{};
    broken.frameIndex = 13;
    auto unseen = BaseDraw();
    unseen.vertexBuffer = 0;
    broken.draws.push_back(unseen);
    const auto none = drawstream::PlanMeshExtraction(broken, limits, {},
        budget);
    CHECK(none.requests.empty());
    CHECK(none.deferred == 0);
}

TEST_CASE("PLS_vertex_range_follows_the_indices_not_the_pool",
    "[livescene][drawstream]")
{
    // The pool is 128 MB; what has to be copied is the window the indices
    // actually touch. Copying from zero to the highest index would move
    // megabytes for a fence post.
    const std::array<std::uint32_t, 6> indices{40, 41, 42, 44, 43, 40};
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    REQUIRE(drawstream::VertexRangeForIndices(indices, 0, first, count));
    CHECK(first == 40);
    CHECK(count == 5);

    // baseVertex shifts the window, because the hardware adds it to every
    // index before the fetch.
    REQUIRE(drawstream::VertexRangeForIndices(indices, 1000, first, count));
    CHECK(first == 1040);
    CHECK(count == 5);

    // A negative base is legal and must not wrap the window to the top of the
    // address space.
    const std::array<std::uint32_t, 3> high{2000, 2001, 2002};
    REQUIRE(drawstream::VertexRangeForIndices(high, -1500, first, count));
    CHECK(first == 500);
    CHECK(count == 3);

    // A base that would push an index below zero is refused rather than
    // wrapping into a read of unrelated memory.
    CHECK_FALSE(drawstream::VertexRangeForIndices(high, -2500, first, count));

    // An empty list has no range at all.
    CHECK_FALSE(drawstream::VertexRangeForIndices({}, 0, first, count));
}

TEST_CASE("PLS_assembly_draws_only_the_geometry_it_actually_has",
    "[livescene][drawstream]")
{
    // A cell fills in over many frames because reading geometry is budgeted.
    // An object whose mesh has not arrived must be removed from the scene,
    // not drawn empty and not drawn with someone else's geometry: a draw
    // index that no longer matches its mesh places the wrong model, which is
    // worse than an unfinished cell.
    drawstream::DrawStreamFrame frame{};
    frame.frameIndex = 55;
    frame.draws.push_back(BaseDraw(0x2000'0000'0000'0001ull, 0, 900));
    auto second = BaseDraw(0x2000'0000'0000'0001ull, 900, 300);
    SetModel(second.model, 50.0f, 0.0f, 0.0f);
    frame.draws.push_back(second);
    // A third draw of the first mesh, so instancing survives assembly.
    auto third = BaseDraw(0x2000'0000'0000'0001ull, 0, 900);
    SetModel(third.model, 0.0f, 75.0f, 0.0f);
    frame.draws.push_back(third);

    scene::ScenePacket packet{};
    drawstream::TranslationResult translated{};
    REQUIRE(drawstream::TranslateDrawStream(frame, {}, packet, translated) ==
        drawstream::DrawStreamError::None);
    REQUIRE(packet.objects.size() == 2);
    REQUIRE(packet.instances.size() == 3);

    // Only the first mesh has been read back so far.
    const std::array<float, 9> positions{
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    drawstream::AssembledMesh held{};
    // The mesh drawn more than once -- the one whose instancing this case
    // exists to follow through assembly -- chosen by that property rather
    // than by its position. The packet's object order is deliberately not the
    // engine's submission order, because the raster packet is cached across
    // frames and paired to the scene positionally, so an order that followed
    // the engine made a cached frame resolve the wrong material.
    std::map<std::uint32_t, std::uint32_t> placements;
    for (const auto& instance : packet.instances) {
        ++placements[instance.objectIndex];
    }
    auto repeated = packet.objects.size();
    for (const auto& [object, count] : placements) {
        if (count > 1) repeated = object;
    }
    REQUIRE(repeated < packet.objects.size());
    held.identity = packet.objects[repeated].objectId;
    held.vertexStride = 12;
    // The layout is declared, not implied by the stride. A stride of twelve
    // is equally consistent with three floats and with four halves plus a
    // pair, and assuming the first turned a live cell into a fan of spikes.
    const std::array<vf::renderer::mesh::InputElementDesc, 1> elements{{
        {"POSITION", 0, vf::renderer::mesh::kFormatR32G32B32Float, 0, 0}}};
    REQUIRE(vf::renderer::mesh::BuildLayoutFromInputElements(
        elements, 12, 0, held.layout) ==
        vf::renderer::mesh::VertexLayoutError::None);
    held.vertices = std::as_bytes(std::span{positions});
    held.indices = indices;
    const std::array<drawstream::AssembledMesh, 1> cache{held};

    raster::DecodedPacket rasterPacket{};
    drawstream::AssemblyResult assembly{};
    REQUIRE(drawstream::AssembleSceneGeometry(packet, cache, rasterPacket,
        assembly) == drawstream::DrawStreamError::None);

    CHECK(assembly.drawnObjects == 1);
    CHECK(assembly.missingMeshes == 1);
    CHECK(assembly.vertices == 3);
    CHECK(assembly.indices == 3);

    // The scene now holds only what can be drawn, and its instances came with
    // it: the two placements of the surviving mesh, not the missing one's.
    CHECK(packet.objects.size() == 1);
    CHECK(packet.instances.size() == 2);
    CHECK(packet.objects[0].objectId == held.identity);
    CHECK(packet.instances[0].model[7] == Catch::Approx(210.0f));
    CHECK(packet.instances[1].model[7] == Catch::Approx(75.0f));

    // The raster packet is self-contained: one draw, covering the mesh it
    // holds, indexed from that mesh's own base.
    REQUIRE(rasterPacket.draws.size() == 1);
    CHECK(rasterPacket.draws[0].firstIndex == 0);
    CHECK(rasterPacket.draws[0].indexCount == 3);
    CHECK(rasterPacket.draws[0].vertexOffset == 0);
    CHECK(rasterPacket.vertices.size() == 3);
    CHECK(rasterPacket.indices.size() == 3);
    // Positions survived the copy as float3 at offset zero.
    CHECK(rasterPacket.vertices[1].position[0] == Catch::Approx(1.0f));
    CHECK(rasterPacket.vertices[2].position[1] == Catch::Approx(1.0f));

    // Every object still points at its own draw after the renumbering.
    for (std::size_t index = 0; index < packet.objects.size(); ++index) {
        CHECK(packet.objects[index].drawIndex == index);
        REQUIRE(packet.objects[index].drawIndex < rasterPacket.draws.size());
    }
    CHECK(scene::ValidateScenePacket(packet) ==
        scene::ScenePacketError::None);
    CHECK(scene::ValidateSceneInstances(packet) ==
        scene::ScenePacketError::None);

    // Nothing cached at all is an empty scene, reported rather than an
    // invalid packet with objects that draw nothing.
    scene::ScenePacket second_{};
    drawstream::TranslationResult ignored{};
    REQUIRE(drawstream::TranslateDrawStream(frame, {}, second_, ignored) ==
        drawstream::DrawStreamError::None);
    raster::DecodedPacket emptyRaster{};
    drawstream::AssemblyResult emptyAssembly{};
    CHECK(drawstream::AssembleSceneGeometry(second_, {}, emptyRaster,
        emptyAssembly) == drawstream::DrawStreamError::EmptyGeometry);
    CHECK(emptyAssembly.missingMeshes == 2);
}

TEST_CASE("PLS_a_draw_with_no_world_transform_is_not_world_geometry",
    "[livescene][drawstream]")
{
    // User-interface and fullscreen passes bind a small constant buffer at
    // vertex-shader slot 0 -- a sixteen-byte viewport constant, for instance.
    // Such a draw has no world matrix of its own, and treating it as world
    // geometry places a screen-space quad somewhere in the cell using
    // whatever matrix the previous world draw left behind. That is the
    // "wrong model placed" failure, and it is refused rather than guessed at.
    drawstream::TranslationLimits limits{};
    auto world = BaseDraw();
    CHECK(drawstream::ValidateDraw(world, limits) ==
        drawstream::DrawStreamError::None);

    auto overlay = BaseDraw();
    overlay.hasTransform = false;
    CHECK(drawstream::ValidateDraw(overlay, limits) ==
        drawstream::DrawStreamError::NoTransform);

    // Such a draw never reaches the scene and never costs a readback.
    drawstream::DrawStreamFrame frame{};
    frame.frameIndex = 3;
    frame.draws.push_back(world);
    frame.draws.push_back(overlay);
    scene::ScenePacket packet{};
    drawstream::TranslationResult result{};
    REQUIRE(drawstream::TranslateDrawStream(frame, limits, packet, result) ==
        drawstream::DrawStreamError::None);
    CHECK(result.objects == 1);
    CHECK(result.rejectedDraws == 1);

    const auto plan = drawstream::PlanMeshExtraction(frame, limits, {}, {});
    CHECK(plan.requests.size() == 1);
}

TEST_CASE("PLS_transforms_the_scene_packet_cannot_carry_are_refused_early",
    "[livescene][drawstream]")
{
    // The scene packet requires an affine bottom row and a positive
    // determinant. Checking that here, rather than discovering it at encode
    // time, is the difference between losing one object and losing the frame:
    // a single mirrored crate among hundreds otherwise blanks the whole cell.
    drawstream::TranslationLimits limits{};

    auto skewed = BaseDraw();
    skewed.model[12] = 0.5f;
    CHECK(drawstream::ValidateDraw(skewed, limits) ==
        drawstream::DrawStreamError::NonAffineTransform);

    auto unscaled = BaseDraw();
    unscaled.model[15] = 2.0f;
    CHECK(drawstream::ValidateDraw(unscaled, limits) ==
        drawstream::DrawStreamError::NonAffineTransform);

    auto mirrored = BaseDraw();
    mirrored.model[0] = -1.0f;
    CHECK(drawstream::ValidateDraw(mirrored, limits) ==
        drawstream::DrawStreamError::MirroredTransform);

    // Whatever survives validation must survive the scene packet, or the
    // check has not earned its place.
    drawstream::DrawStreamFrame frame{};
    frame.frameIndex = 21;
    frame.draws.push_back(BaseDraw());
    frame.draws.push_back(skewed);
    frame.draws.push_back(mirrored);
    scene::ScenePacket packet{};
    drawstream::TranslationResult result{};
    REQUIRE(drawstream::TranslateDrawStream(frame, limits, packet, result) ==
        drawstream::DrawStreamError::None);
    CHECK(result.rejectedDraws == 2);
    CHECK(scene::ValidateScenePacket(packet) ==
        scene::ScenePacketError::None);
    CHECK(scene::ValidateSceneInstances(packet) ==
        scene::ScenePacketError::None);
}

TEST_CASE("PLS_rejections_are_attributed_to_the_rule_that_made_them",
    "[livescene][drawstream]")
{
    // A rejected total says the scene is thin. Only the breakdown says which
    // rule made it thin, and without it a rule that rejects everything is
    // indistinguishable from one that rejects nothing.
    drawstream::DrawStreamFrame frame{};
    frame.frameIndex = 90;
    frame.draws.push_back(BaseDraw());

    auto overlay = BaseDraw();
    overlay.hasTransform = false;
    frame.draws.push_back(overlay);
    frame.draws.push_back(overlay);

    auto mirrored = BaseDraw();
    mirrored.model[0] = -1.0f;
    frame.draws.push_back(mirrored);

    auto strip = BaseDraw();
    strip.indexCount = 901;
    frame.draws.push_back(strip);

    scene::ScenePacket packet{};
    drawstream::TranslationResult result{};
    REQUIRE(drawstream::TranslateDrawStream(frame, {}, packet, result) ==
        drawstream::DrawStreamError::None);

    const auto count = [&](const drawstream::DrawStreamError reason) {
        return result.rejectedByReason[static_cast<std::size_t>(reason)];
    };
    CHECK(result.rejectedDraws == 4);
    CHECK(count(drawstream::DrawStreamError::NoTransform) == 2);
    CHECK(count(drawstream::DrawStreamError::MirroredTransform) == 1);
    CHECK(count(drawstream::DrawStreamError::NotATriangleList) == 1);
    CHECK(count(drawstream::DrawStreamError::None) == 0);
    // The counts account for every rejection, so none is lost to a rule that
    // forgot to name itself.
    std::uint32_t total = 0;
    for (const auto entry : result.rejectedByReason) total += entry;
    CHECK(total == result.rejectedDraws);
}

TEST_CASE("PLS_assembly_carries_the_engine_per_vertex_normal",
    "[livescene][drawstream]")
{
    // Lighting is a function of the normal, so a scene whose vertices all
    // carry the same one shades every pixel of an object identically no
    // matter where the sun is. That reads as "the lighting is broken" rather
    // than as "the normals were never filled in", which is what it is: the
    // vertex fill wrote position, colour and texture coordinates and left the
    // normal at its +Z default for every vertex in the world.
    //
    // The engine declares NORMAL in its own input layouts and the decoder
    // already reads it. The shader takes it in object space and rotates it by
    // the model's upper 3x3, so what has to arrive here is the decoded value
    // itself, untransformed.
    drawstream::DrawStreamFrame frame{};
    frame.frameIndex = 71;
    frame.draws.push_back(BaseDraw(0x2000'0000'0000'0007ull, 0, 3));

    scene::ScenePacket packet{};
    drawstream::TranslationResult translated{};
    REQUIRE(drawstream::TranslateDrawStream(frame, {}, packet, translated) ==
        drawstream::DrawStreamError::None);
    REQUIRE(packet.objects.size() == 1);

    // Three vertices whose normals point three different ways. Distinct on
    // purpose: a fill that wrote one vertex's normal to all three, or that
    // kept the default, is indistinguishable from a correct one when every
    // normal is the same.
    // Byte-encoded, because that is how Fallout 4 stores a normal: four
    // unsigned bytes remapped to [-1, 1]. 255 is +1, 0 is -1, 128 is nearly
    // zero.
    struct Vertex
    {
        float position[3];
        std::uint8_t normal[4];
    };
    const std::array<Vertex, 3> vertices{{
        {{0.0f, 0.0f, 0.0f}, {255, 128, 128, 0}},
        {{1.0f, 0.0f, 0.0f}, {128, 255, 128, 0}},
        {{0.0f, 1.0f, 0.0f}, {128, 128, 0, 0}}}};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};

    drawstream::AssembledMesh held{};
    held.identity = packet.objects[0].objectId;
    held.vertexStride = sizeof(Vertex);
    const std::array<vf::renderer::mesh::InputElementDesc, 2> elements{{
        {"POSITION", 0, vf::renderer::mesh::kFormatR32G32B32Float, 0, 0},
        {"NORMAL", 0, vf::renderer::mesh::kFormatR8G8B8A8Unorm, 0, 12}}};
    REQUIRE(vf::renderer::mesh::BuildLayoutFromInputElements(
        elements, sizeof(Vertex), 0, held.layout) ==
        vf::renderer::mesh::VertexLayoutError::None);
    held.vertices = std::as_bytes(std::span{vertices});
    held.indices = indices;
    const std::array<drawstream::AssembledMesh, 1> cache{held};

    raster::DecodedPacket rasterPacket{};
    drawstream::AssemblyResult assembly{};
    REQUIRE(drawstream::AssembleSceneGeometry(packet, cache, rasterPacket,
        assembly) == drawstream::DrawStreamError::None);
    REQUIRE(rasterPacket.vertices.size() == 3);

    CHECK(rasterPacket.vertices[0].normal[0] > 0.9f);
    CHECK(std::abs(rasterPacket.vertices[0].normal[1]) < 0.02f);
    CHECK(rasterPacket.vertices[1].normal[1] > 0.9f);
    CHECK(std::abs(rasterPacket.vertices[1].normal[0]) < 0.02f);
    CHECK(rasterPacket.vertices[2].normal[2] < -0.9f);
    CHECK(std::abs(rasterPacket.vertices[2].normal[1]) < 0.02f);
}

TEST_CASE("PLS_a_mesh_without_normals_keeps_a_usable_default",
    "[livescene][drawstream]")
{
    // Not every engine layout declares NORMAL. Such a mesh must still shade
    // rather than divide by zero: a zero normal is not "no lighting", it is a
    // division by zero wherever the shading normalises it.
    drawstream::DrawStreamFrame frame{};
    frame.frameIndex = 72;
    frame.draws.push_back(BaseDraw(0x2000'0000'0000'0008ull, 0, 3));

    scene::ScenePacket packet{};
    drawstream::TranslationResult translated{};
    REQUIRE(drawstream::TranslateDrawStream(frame, {}, packet, translated) ==
        drawstream::DrawStreamError::None);

    const std::array<float, 9> positions{
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    drawstream::AssembledMesh held{};
    held.identity = packet.objects[0].objectId;
    held.vertexStride = 12;
    const std::array<vf::renderer::mesh::InputElementDesc, 1> elements{{
        {"POSITION", 0, vf::renderer::mesh::kFormatR32G32B32Float, 0, 0}}};
    REQUIRE(vf::renderer::mesh::BuildLayoutFromInputElements(
        elements, 12, 0, held.layout) ==
        vf::renderer::mesh::VertexLayoutError::None);
    held.vertices = std::as_bytes(std::span{positions});
    held.indices = indices;
    const std::array<drawstream::AssembledMesh, 1> cache{held};

    raster::DecodedPacket rasterPacket{};
    drawstream::AssemblyResult assembly{};
    REQUIRE(drawstream::AssembleSceneGeometry(packet, cache, rasterPacket,
        assembly) == drawstream::DrawStreamError::None);
    REQUIRE(rasterPacket.vertices.size() == 3);

    for (const auto& vertex : rasterPacket.vertices) {
        const auto lengthSquared = vertex.normal[0] * vertex.normal[0] +
            vertex.normal[1] * vertex.normal[1] +
            vertex.normal[2] * vertex.normal[2];
        CHECK(lengthSquared > 0.5f);
    }
}

TEST_CASE("PLS_a_declared_normal_that_decodes_to_zero_keeps_the_default",
    "[livescene][drawstream]")
{
    // Distinct from a mesh that declares no NORMAL at all: here the element
    // is present and the bytes are zero. Copying that through would put a
    // zero-length normal in the packet, and the shading normalises it.
    drawstream::DrawStreamFrame frame{};
    frame.frameIndex = 73;
    frame.draws.push_back(BaseDraw(0x2000'0000'0000'0009ull, 0, 3));

    scene::ScenePacket packet{};
    drawstream::TranslationResult translated{};
    REQUIRE(drawstream::TranslateDrawStream(frame, {}, packet, translated) ==
        drawstream::DrawStreamError::None);

    struct Vertex
    {
        float position[3];
        std::uint8_t normal[4];
    };
    // Byte 128 decodes to almost exactly zero on every axis, which is the
    // degenerate normal a real mesh can carry.
    const std::array<Vertex, 3> vertices{{
        {{0.0f, 0.0f, 0.0f}, {128, 128, 128, 0}},
        {{1.0f, 0.0f, 0.0f}, {128, 128, 128, 0}},
        {{0.0f, 1.0f, 0.0f}, {128, 128, 128, 0}}}};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};

    drawstream::AssembledMesh held{};
    held.identity = packet.objects[0].objectId;
    held.vertexStride = sizeof(Vertex);
    const std::array<vf::renderer::mesh::InputElementDesc, 2> elements{{
        {"POSITION", 0, vf::renderer::mesh::kFormatR32G32B32Float, 0, 0},
        {"NORMAL", 0, vf::renderer::mesh::kFormatR8G8B8A8Unorm, 0, 12}}};
    REQUIRE(vf::renderer::mesh::BuildLayoutFromInputElements(
        elements, sizeof(Vertex), 0, held.layout) ==
        vf::renderer::mesh::VertexLayoutError::None);
    held.vertices = std::as_bytes(std::span{vertices});
    held.indices = indices;
    const std::array<drawstream::AssembledMesh, 1> cache{held};

    raster::DecodedPacket rasterPacket{};
    drawstream::AssemblyResult assembly{};
    REQUIRE(drawstream::AssembleSceneGeometry(packet, cache, rasterPacket,
        assembly) == drawstream::DrawStreamError::None);
    REQUIRE(rasterPacket.vertices.size() == 3);

    for (const auto& vertex : rasterPacket.vertices) {
        const auto lengthSquared = vertex.normal[0] * vertex.normal[0] +
            vertex.normal[1] * vertex.normal[1] +
            vertex.normal[2] * vertex.normal[2];
        CHECK(lengthSquared > 0.5f);
    }
}

TEST_CASE("PLS_assembly_counts_which_vertices_brought_their_own_normal",
    "[livescene][drawstream]")
{
    // Lighting that looks flat has two possible causes that no picture can
    // tell apart: the engine's layouts do not declare NORMAL, so every vertex
    // takes the default, or the normals arrive and something downstream
    // ignores them. Counting the two at the point where the choice is made is
    // what separates them, and a diagnostic that cannot separate them sends
    // the search to the wrong half of the pipeline.
    drawstream::DrawStreamFrame frame{};
    frame.frameIndex = 74;
    frame.draws.push_back(BaseDraw(0x2000'0000'0000'000Aull, 0, 3));

    scene::ScenePacket packet{};
    drawstream::TranslationResult translated{};
    REQUIRE(drawstream::TranslateDrawStream(frame, {}, packet, translated) ==
        drawstream::DrawStreamError::None);

    SECTION("a mesh that declares NORMAL is counted as carrying its own")
    {
        struct Vertex
        {
            float position[3];
            std::uint8_t normal[4];
        };
        const std::array<Vertex, 3> vertices{{
            {{0.0f, 0.0f, 0.0f}, {255, 128, 128, 0}},
            {{1.0f, 0.0f, 0.0f}, {128, 255, 128, 0}},
            {{0.0f, 1.0f, 0.0f}, {128, 128, 0, 0}}}};
        const std::array<std::uint32_t, 3> indices{0, 1, 2};
        drawstream::AssembledMesh held{};
        held.identity = packet.objects[0].objectId;
        held.vertexStride = sizeof(Vertex);
        const std::array<vf::renderer::mesh::InputElementDesc, 2> elements{{
            {"POSITION", 0, vf::renderer::mesh::kFormatR32G32B32Float, 0, 0},
            {"NORMAL", 0, vf::renderer::mesh::kFormatR8G8B8A8Unorm, 0, 12}}};
        REQUIRE(vf::renderer::mesh::BuildLayoutFromInputElements(
            elements, sizeof(Vertex), 0, held.layout) ==
            vf::renderer::mesh::VertexLayoutError::None);
        held.vertices = std::as_bytes(std::span{vertices});
        held.indices = indices;
        const std::array<drawstream::AssembledMesh, 1> cache{held};

        raster::DecodedPacket rasterPacket{};
        drawstream::AssemblyResult assembly{};
        REQUIRE(drawstream::AssembleSceneGeometry(packet, cache, rasterPacket,
            assembly) == drawstream::DrawStreamError::None);
        CHECK(assembly.verticesWithNormals == 3);
        CHECK(assembly.verticesWithoutNormals == 0);
    }

    SECTION("a mesh with no NORMAL is counted as taking the default")
    {
        const std::array<float, 9> positions{
            0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        const std::array<std::uint32_t, 3> indices{0, 1, 2};
        drawstream::AssembledMesh held{};
        held.identity = packet.objects[0].objectId;
        held.vertexStride = 12;
        const std::array<vf::renderer::mesh::InputElementDesc, 1> elements{{
            {"POSITION", 0, vf::renderer::mesh::kFormatR32G32B32Float, 0, 0}}};
        REQUIRE(vf::renderer::mesh::BuildLayoutFromInputElements(
            elements, 12, 0, held.layout) ==
            vf::renderer::mesh::VertexLayoutError::None);
        held.vertices = std::as_bytes(std::span{positions});
        held.indices = indices;
        const std::array<drawstream::AssembledMesh, 1> cache{held};

        raster::DecodedPacket rasterPacket{};
        drawstream::AssemblyResult assembly{};
        REQUIRE(drawstream::AssembleSceneGeometry(packet, cache, rasterPacket,
            assembly) == drawstream::DrawStreamError::None);
        CHECK(assembly.verticesWithNormals == 0);
        CHECK(assembly.verticesWithoutNormals == 3);
    }
}

TEST_CASE("P20_assembly_takes_its_winding_from_the_engine_not_a_constant",
    "[drawstream][phase20]")
{
    // The mirror declared counter-clockwise front faces for every mesh, which
    // is the opposite of D3D11's default and drew every model inside out: the
    // near faces were culled and the far interior kept. The winding has to
    // come from the rasterizer state the engine bound.
    const std::array<float, 9> positions{
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    const std::array<vf::renderer::mesh::InputElementDesc, 1> elements{{
        {"POSITION", 0, vf::renderer::mesh::kFormatR32G32B32Float, 0, 0}}};

    const auto assembleWith = [&](const bool frontCounterClockwise) {
        scene::ScenePacket packet{};
        packet.header.frameId = 1;
        packet.header.viewId = 1;
        scene::OpaqueObjectV1 object{};
        object.objectId = 0x1234;
        object.materialId = 0x5678;
        object.flags = scene::ObjectWritesWorldTarget | scene::ObjectStatic;
        object.boundsMinimum[0] = -1.0f;
        object.boundsMaximum[0] = 1.0f;
        object.model[0] = 1.0f;
        object.model[5] = 1.0f;
        object.model[10] = 1.0f;
        object.model[15] = 1.0f;
        object.geometricNormal[2] = 1.0f;
        object.shadingNormal[2] = 1.0f;
        packet.objects.push_back(object);

        drawstream::AssembledMesh held{};
        held.identity = object.objectId;
        held.vertexStride = 12;
        REQUIRE(vf::renderer::mesh::BuildLayoutFromInputElements(
            elements, 12, 0, held.layout) ==
            vf::renderer::mesh::VertexLayoutError::None);
        held.vertices = std::as_bytes(std::span{positions});
        held.indices = indices;
        held.frontCounterClockwise = frontCounterClockwise;
        const std::array<drawstream::AssembledMesh, 1> cache{held};

        raster::DecodedPacket rasterPacket{};
        drawstream::AssemblyResult assembly{};
        REQUIRE(drawstream::AssembleSceneGeometry(packet, cache, rasterPacket,
            assembly) == drawstream::DrawStreamError::None);
        REQUIRE(rasterPacket.draws.size() == 1);
        return rasterPacket.draws.front().frontFace;
    };

    // Both directions, so neither can be produced by a constant. A single
    // case would pass against a hardcoded value half the time, which is
    // exactly how the original defect survived.
    //
    // Inverted relative to the engine's flag on purpose: D3D declares facing
    // in Y-down screen space and this field is Y-up NDC. Taking the flag from
    // the engine fixed only half of it -- the value was right and the
    // convention was not, so models still drew inside out while the stream
    // carried the engine's own answer.
    CHECK(assembleWith(true) == raster::FrontFace::Clockwise);
    CHECK(assembleWith(false) == raster::FrontFace::CounterClockwise);
}

TEST_CASE("P20_depth_only_draws_are_not_world_geometry",
    "[drawstream][phase20]")
{
    // Measured live: 22,320 of 42,590 draws bind no pixel shader. Those are
    // the depth prepass and the shadow cascades, and a cascade often draws a
    // different LOD mesh -- which takes its own mesh identity, is textured by
    // nothing, and renders as a white duplicate of the object beside it.
    drawstream::DrawRecordV1 draw{};
    draw.vertexBuffer = 0x1000;
    draw.indexBuffer = 0x2000;
    draw.vertexStride = 12;
    draw.indexCount = 3;
    draw.instanceCount = 1;
    draw.hasTransform = true;
    draw.hasPixelShader = true;
    draw.model[0] = 1.0f;
    draw.model[5] = 1.0f;
    draw.model[10] = 1.0f;
    draw.model[15] = 1.0f;

    const drawstream::TranslationLimits limits{};
    REQUIRE(drawstream::ValidateDraw(draw, limits) ==
        drawstream::DrawStreamError::None);

    // The only field changed is the pixel shader, so the refusal cannot be
    // attributed to anything else in the record.
    auto depthOnly = draw;
    depthOnly.hasPixelShader = false;
    CHECK(drawstream::ValidateDraw(depthOnly, limits) ==
        drawstream::DrawStreamError::DepthOnlyPass);

    // Counted under its own reason rather than folded into a total, so a
    // frame that lost its geometry to this rule says so.
    drawstream::DrawStreamFrame frame{};
    frame.draws.push_back(draw);
    frame.draws.push_back(depthOnly);
    scene::ScenePacket packet{};
    drawstream::TranslationResult result{};
    REQUIRE(drawstream::TranslateDrawStream(frame, limits, packet, result) ==
        drawstream::DrawStreamError::None);
    CHECK(result.objects == 1);
    CHECK(result.rejectedDraws == 1);
    CHECK(result.rejectedByReason[static_cast<std::size_t>(
        drawstream::DrawStreamError::DepthOnlyPass)] == 1);
}

TEST_CASE("P20_offscreen_passes_are_not_world_geometry",
    "[drawstream][phase20]")
{
    // Measured live: 4,248 of 6,529 recorded draws ran without the main scene
    // depth bound. The water reflection pass redraws the world through a
    // mirrored camera and the loading screen draws through its own; both
    // arrive as a second scene once re-projected through the world camera.
    drawstream::DrawRecordV1 draw{};
    draw.vertexBuffer = 0x1000;
    draw.indexBuffer = 0x2000;
    draw.vertexStride = 12;
    draw.indexCount = 3;
    draw.instanceCount = 1;
    draw.hasTransform = true;
    draw.hasPixelShader = true;
    draw.sceneDepthBound = true;
    draw.model[0] = 1.0f;
    draw.model[5] = 1.0f;
    draw.model[10] = 1.0f;
    draw.model[15] = 1.0f;

    const drawstream::TranslationLimits limits{};
    REQUIRE(drawstream::ValidateDraw(draw, limits) ==
        drawstream::DrawStreamError::None);

    // Only the depth binding differs, so the refusal is attributable to it.
    auto offscreen = draw;
    offscreen.sceneDepthBound = false;
    CHECK(drawstream::ValidateDraw(offscreen, limits) ==
        drawstream::DrawStreamError::OffscreenPass);

    // Distinct from the depth-only reason: a draw can be both, and the two
    // have different causes, so a frame's breakdown must not merge them.
    auto both = offscreen;
    both.hasPixelShader = false;
    CHECK(drawstream::ValidateDraw(both, limits) ==
        drawstream::DrawStreamError::DepthOnlyPass);

    drawstream::DrawStreamFrame frame{};
    frame.draws.push_back(draw);
    frame.draws.push_back(offscreen);
    scene::ScenePacket packet{};
    drawstream::TranslationResult result{};
    REQUIRE(drawstream::TranslateDrawStream(frame, limits, packet, result) ==
        drawstream::DrawStreamError::None);
    CHECK(result.objects == 1);
    CHECK(result.rejectedByReason[static_cast<std::size_t>(
        drawstream::DrawStreamError::OffscreenPass)] == 1);
}

TEST_CASE("P20_reused_geometry_matches_a_full_rebuild",
    "[drawstream][phase20]")
{
    // Measured live: a settled scene reproduces all 940 meshes byte for byte
    // every frame, and decoding them again costs 454 ms. Reuse is only sound
    // if it is indistinguishable from the rebuild it replaces.
    const std::array<float, 18> positions{
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
        2.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f, 2.0f, 1.0f, 0.0f};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    const std::array<vf::renderer::mesh::InputElementDesc, 1> elements{{
        {"POSITION", 0, vf::renderer::mesh::kFormatR32G32B32Float, 0, 0}}};

    const auto makePacket = []() {
        scene::ScenePacket packet{};
        packet.header.frameId = 1;
        packet.header.viewId = 1;
        for (std::uint32_t slot = 0; slot < 2; ++slot) {
            scene::OpaqueObjectV1 object{};
            object.objectId = 0x1000 + slot;
            object.materialId = 0x2000 + slot;
            object.flags = scene::ObjectWritesWorldTarget | scene::ObjectStatic;
            object.boundsMinimum[0] = -1.0f;
            object.boundsMaximum[0] = 1.0f;
            object.model[0] = 1.0f;
            object.model[5] = 1.0f;
            object.model[10] = 1.0f;
            object.model[15] = 1.0f;
            object.geometricNormal[2] = 1.0f;
            object.shadingNormal[2] = 1.0f;
            packet.objects.push_back(object);
        }
        return packet;
    };

    std::array<drawstream::AssembledMesh, 2> cache{};
    for (std::uint32_t slot = 0; slot < 2; ++slot) {
        cache[slot].identity = 0x1000 + slot;
        cache[slot].vertexStride = 12;
        REQUIRE(vf::renderer::mesh::BuildLayoutFromInputElements(
            elements, 12, 0, cache[slot].layout) ==
            vf::renderer::mesh::VertexLayoutError::None);
        cache[slot].vertices = std::as_bytes(
            std::span{positions.data() + slot * 9, 9});
        cache[slot].indices = indices;
    }

    auto rebuiltScene = makePacket();
    raster::DecodedPacket rebuilt{};
    drawstream::AssemblyResult rebuiltResult{};
    REQUIRE(drawstream::AssembleSceneGeometry(rebuiltScene, cache, rebuilt,
        rebuiltResult, nullptr) == drawstream::DrawStreamError::None);
    REQUIRE(rebuilt.vertices.size() == 6);

    // Second pass over the same mesh set through an arena. The arena is
    // populated by a first pass so the second reuses every slot.
    drawstream::GeometryArena arena{};
    auto warmScene = makePacket();
    raster::DecodedPacket warm{};
    drawstream::AssemblyResult warmResult{};
    REQUIRE(drawstream::AssembleSceneGeometry(warmScene, cache, warm,
        warmResult, &arena) == drawstream::DrawStreamError::None);
    auto reusedScene = makePacket();
    drawstream::AssemblyResult reusedResult{};
    REQUIRE(drawstream::AssembleSceneGeometry(reusedScene, cache, warm,
        reusedResult, &arena) == drawstream::DrawStreamError::None);
    // Reuse must be indistinguishable from the rebuild it replaces.
    REQUIRE(warm.vertices.size() == rebuilt.vertices.size());
    CHECK(std::memcmp(warm.vertices.data(), rebuilt.vertices.data(),
        warm.vertices.size() * sizeof(raster::RasterVertexV3)) == 0);
    CHECK(warm.indices == rebuilt.indices);
    CHECK(warm.draws.size() == rebuilt.draws.size());
    CHECK(warmResult.verticesWithNormals == rebuiltResult.verticesWithNormals);
    CHECK(reusedResult.verticesWithNormals ==
        rebuiltResult.verticesWithNormals);
    CHECK(reusedResult.verticesWithoutNormals ==
        rebuiltResult.verticesWithoutNormals);

    // The draw ranges are what a wrong cursor would corrupt, and a scene made
    // of the right meshes at the wrong offsets still looks like a scene.
    REQUIRE(rebuilt.draws.size() == 2);
    CHECK(rebuilt.draws[0].firstIndex == 0);
    CHECK(rebuilt.draws[0].vertexOffset == 0);
    CHECK(rebuilt.draws[1].firstIndex == 3);
    CHECK(rebuilt.draws[1].vertexOffset == 3);
    CHECK(rebuilt.vertices.size() == 6);
    CHECK(rebuilt.indices.size() == 6);
    CHECK(rebuilt.header.vertexCount == 6);
    CHECK(rebuilt.materials.size() == 2);
    CHECK(reusedScene.objects.size() == rebuiltScene.objects.size());

    // An arena whose slots no longer describe the arrays beside them is
    // discarded rather than trusted: the draw ranges would otherwise point
    // into geometry belonging to something else.
    raster::DecodedPacket stale{};
    stale.vertices.resize(3);
    stale.indices.resize(3);
    auto staleScene = makePacket();
    drawstream::AssemblyResult staleResult{};
    REQUIRE(drawstream::AssembleSceneGeometry(staleScene, cache, stale,
        staleResult, &arena) == drawstream::DrawStreamError::None);
    REQUIRE(stale.vertices.size() == rebuilt.vertices.size());
    CHECK(std::memcmp(stale.vertices.data(), rebuilt.vertices.data(),
        stale.vertices.size() * sizeof(raster::RasterVertexV3)) == 0);
}

TEST_CASE("P20_a_mesh_that_keeps_its_identity_but_changes_bytes_is_re_decoded",
    "[drawstream][phase20]")
{
    // The dangerous case for any geometry cache. A mesh identity is derived
    // from the pooled buffer and the range inside it, so the engine can
    // re-extract different geometry into the same range and keep the identity.
    // Reusing the slot then draws the previous object's geometry under the new
    // one's name -- a scene made of the right objects in the wrong shapes,
    // which reads as a capture bug rather than a cache bug.
    const std::array<float, 9> first{
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const std::array<float, 9> second{
        5.0f, 5.0f, 5.0f, 6.0f, 5.0f, 5.0f, 5.0f, 6.0f, 5.0f};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    const std::array<vf::renderer::mesh::InputElementDesc, 1> elements{{
        {"POSITION", 0, vf::renderer::mesh::kFormatR32G32B32Float, 0, 0}}};

    const auto makePacket = []() {
        scene::ScenePacket packet{};
        packet.header.frameId = 1;
        packet.header.viewId = 1;
        scene::OpaqueObjectV1 object{};
        object.objectId = 0x1000;
        object.materialId = 0x2000;
        object.flags = scene::ObjectWritesWorldTarget | scene::ObjectStatic;
        object.boundsMinimum[0] = -1.0f;
        object.boundsMaximum[0] = 1.0f;
        object.model[0] = 1.0f;
        object.model[5] = 1.0f;
        object.model[10] = 1.0f;
        object.model[15] = 1.0f;
        object.geometricNormal[2] = 1.0f;
        object.shadingNormal[2] = 1.0f;
        packet.objects.push_back(object);
        return packet;
    };
    const auto makeMesh = [&](const std::array<float, 9>& source) {
        drawstream::AssembledMesh mesh{};
        mesh.identity = 0x1000;
        mesh.vertexStride = 12;
        REQUIRE(vf::renderer::mesh::BuildLayoutFromInputElements(
            elements, 12, 0, mesh.layout) ==
            vf::renderer::mesh::VertexLayoutError::None);
        mesh.vertices = std::as_bytes(std::span{source});
        mesh.indices = indices;
        return mesh;
    };

    drawstream::GeometryArena arena{};
    raster::DecodedPacket packet{};
    drawstream::AssemblyResult result{};

    const std::array<drawstream::AssembledMesh, 1> firstSet{makeMesh(first)};
    auto firstScene = makePacket();
    REQUIRE(drawstream::AssembleSceneGeometry(firstScene, firstSet, packet,
        result, &arena) == drawstream::DrawStreamError::None);
    REQUIRE(packet.vertices.size() == 3);
    CHECK(packet.vertices[0].position[0] == 0.0f);

    // Same identity, different bytes.
    const std::array<drawstream::AssembledMesh, 1> secondSet{makeMesh(second)};
    auto secondScene = makePacket();
    REQUIRE(drawstream::AssembleSceneGeometry(secondScene, secondSet, packet,
        result, &arena) == drawstream::DrawStreamError::None);
    REQUIRE(packet.vertices.size() >= 3);
    const auto slot = static_cast<std::size_t>(packet.draws.front().vertexOffset);
    CHECK(packet.vertices[slot].position[0] == 5.0f);
    CHECK(packet.vertices[slot].position[1] == 5.0f);
}

TEST_CASE("P20_assembly_carries_the_engine_cull_mode_into_visibility",
    "[drawstream][phase20]")
{
    // The engine's cull mode was captured all the way to AssembledMesh and
    // then never read, and the scene packet left its visibility records empty
    // -- which the packet defines as "every object is opaque, front-facing
    // only, and unmirrored". Every mirrored object was therefore back-face
    // culled whatever the engine did, so a two-sided model lost its outer
    // shell and showed its interior instead.
    const std::array<float, 9> positions{
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    const std::array<vf::renderer::mesh::InputElementDesc, 1> elements{{
        {"POSITION", 0, vf::renderer::mesh::kFormatR32G32B32Float, 0, 0}}};

    const auto assembleWith = [&](const std::uint32_t cullMode) {
        scene::ScenePacket packet{};
        packet.header.frameId = 1;
        packet.header.viewId = 1;
        scene::OpaqueObjectV1 object{};
        object.objectId = 0x1000;
        object.materialId = 0x2000;
        object.flags = scene::ObjectWritesWorldTarget | scene::ObjectStatic;
        object.boundsMinimum[0] = -1.0f;
        object.boundsMaximum[0] = 1.0f;
        object.model[0] = 1.0f;
        object.model[5] = 1.0f;
        object.model[10] = 1.0f;
        object.model[15] = 1.0f;
        object.geometricNormal[2] = 1.0f;
        object.shadingNormal[2] = 1.0f;
        packet.objects.push_back(object);

        drawstream::AssembledMesh mesh{};
        mesh.identity = object.objectId;
        mesh.vertexStride = 12;
        REQUIRE(vf::renderer::mesh::BuildLayoutFromInputElements(
            elements, 12, 0, mesh.layout) ==
            vf::renderer::mesh::VertexLayoutError::None);
        mesh.vertices = std::as_bytes(std::span{positions});
        mesh.indices = indices;
        mesh.cullMode = cullMode;
        const std::array<drawstream::AssembledMesh, 1> cache{mesh};

        raster::DecodedPacket rasterPacket{};
        drawstream::AssemblyResult assembly{};
        REQUIRE(drawstream::AssembleSceneGeometry(packet, cache, rasterPacket,
            assembly, nullptr) == drawstream::DrawStreamError::None);
        REQUIRE(packet.visibility.size() == packet.objects.size());
        return packet.visibility.front().faceMode;
    };

    // Every mode the engine can declare maps to the matching face mode, and
    // an unobserved state takes D3D11's documented default of culling back.
    CHECK(assembleWith(drawstream::kCullModeNone) ==
        visibility::FaceMode::TwoSided);
    CHECK(assembleWith(drawstream::kCullModeBack) ==
        visibility::FaceMode::FrontOnly);
    CHECK(assembleWith(drawstream::kCullModeFront) ==
        visibility::FaceMode::BackOnly);
    CHECK(assembleWith(drawstream::kCullModeUnknown) ==
        visibility::FaceMode::FrontOnly);
}
