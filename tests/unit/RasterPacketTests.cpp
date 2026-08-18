#include "renderer_api/RasterPacket.h"
#include "renderer_core/FrameGraph.h"
#include "renderer_core/RasterGolden.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using namespace vf::renderer::raster;

PacketHeaderV1& Header(std::vector<std::byte>& bytes)
{
    REQUIRE(bytes.size() >= sizeof(PacketHeaderV1));
    return *reinterpret_cast<PacketHeaderV1*>(bytes.data());
}

DecodedPacket Decode(const SyntheticPacketOptions& options = {})
{
    const auto bytes = BuildSyntheticPacket(options);
    REQUIRE_FALSE(bytes.empty());
    DecodedPacket decoded;
    const auto result = DecodePacket(bytes, decoded);
    INFO(ToString(result.error));
    REQUIRE(result);
    return decoded;
}

}

TEST_CASE("phase6 raster packet decodes the indexed synthetic mesh", "[phase6][raster]")
{
    const auto packet = Decode();
    CHECK(packet.header.width == 96);
    CHECK(packet.header.height == 64);
    CHECK(packet.vertices.size() == 3);
    CHECK(packet.indices == std::vector<std::uint32_t>{0, 1, 2});
    REQUIRE(packet.draws.size() == 1);
    CHECK(packet.draws.front().indexCount == 3);
    REQUIRE(packet.materials.size() == 1);
    CHECK(packet.draws.front().materialId == packet.materials.front().resourceId);
}

TEST_CASE("phase6 packet accepts both explicit index widths", "[phase6][raster]")
{
    SyntheticPacketOptions options;
    options.indexType = IndexType::Uint16;
    CHECK(Decode(options).indices == std::vector<std::uint32_t>{0, 1, 2});
    options.indexType = IndexType::Uint32;
    CHECK(Decode(options).indices == std::vector<std::uint32_t>{0, 1, 2});
}

TEST_CASE("phase6 triangle winding is explicit and deterministic", "[phase6][raster]")
{
    RasterVertexV1 a{{-0.5f, -0.5f, 0.5f}, {}};
    RasterVertexV1 b{{0.5f, -0.5f, 0.5f}, {}};
    RasterVertexV1 c{{0.0f, 0.5f, 0.5f}, {}};
    CHECK(ClassifyTriangle(a, b, c) == TriangleWinding::CounterClockwise);
    CHECK(ClassifyTriangle(a, c, b) == TriangleWinding::Clockwise);
    CHECK(ClassifyTriangle(a, a, a) == TriangleWinding::Degenerate);

    SyntheticPacketOptions reversed;
    reversed.reverseWinding = true;
    const auto packet = Decode(reversed);
    CHECK(ClassifyTriangle(
        packet.vertices[packet.indices[0]],
        packet.vertices[packet.indices[1]],
        packet.vertices[packet.indices[2]]) == TriangleWinding::Clockwise);
}

TEST_CASE("phase6 viewport and scissor fail before submission", "[phase6][raster]")
{
    auto bytes = BuildSyntheticPacket();
    Header(bytes).viewportWidth = 0.0f;
    DecodedPacket decoded;
    CHECK(DecodePacket(bytes, decoded).error == PacketError::InvalidViewport);

    bytes = BuildSyntheticPacket();
    Header(bytes).scissorWidth = Header(bytes).width + 1;
    CHECK(DecodePacket(bytes, decoded).error == PacketError::InvalidScissor);
}

TEST_CASE("phase6 packet rejects missing resources and layout drift", "[phase6][raster]")
{
    auto bytes = BuildSyntheticPacket();
    auto& header = Header(bytes);
    auto* draw = reinterpret_cast<RasterDrawV1*>(
        bytes.data() + header.drawsOffset);
    draw->materialId = 0xDEAD'BEEFull;
    DecodedPacket decoded;
    auto result = DecodePacket(bytes, decoded);
    CHECK(result.error == PacketError::MissingMaterial);
    CHECK(result.resourceId == 0xDEAD'BEEFull);

    bytes = BuildSyntheticPacket();
    auto& layoutHeader = Header(bytes);
    auto* material = reinterpret_cast<RasterMaterialV1*>(
        bytes.data() + layoutHeader.materialsOffset);
    material->shaderLayoutHash ^= 1;
    CHECK(DecodePacket(bytes, decoded).error == PacketError::ShaderLayoutMismatch);
}

TEST_CASE("phase6 packet range and index corruption are diagnosed", "[phase6][raster]")
{
    auto bytes = BuildSyntheticPacket();
    Header(bytes).totalSize += 8;
    DecodedPacket decoded;
    CHECK(DecodePacket(bytes, decoded).error == PacketError::SizeMismatch);

    bytes = BuildSyntheticPacket();
    auto& header = Header(bytes);
    std::uint16_t badIndex = 999;
    std::memcpy(bytes.data() + header.indicesOffset, &badIndex, sizeof(badIndex));
    CHECK(DecodePacket(bytes, decoded).error == PacketError::IndexOutOfRange);
}

TEST_CASE("phase6 reference depth keeps the near triangle", "[phase6][raster]")
{
    SyntheticPacketOptions options;
    options.includeOccludedTriangle = true;
    const auto packet = Decode(options);
    RasterImage image;
    REQUIRE(RenderReference(packet, image) == ReferenceRasterError::None);
    REQUIRE(image.pixels.size() == 96 * 64);
    const auto center = image.At(48, 32);
    CHECK(center.a == 255);
    CHECK(center.g > 0);
    CHECK_FALSE(center == Rgba8{188, 0, 188, 255});
}

TEST_CASE("phase6 reference raster has stable exact probes", "[phase6][raster]")
{
    const auto packet = Decode();
    RasterImage first;
    RasterImage second;
    REQUIRE(RenderReference(packet, first) == ReferenceRasterError::None);
    REQUIRE(RenderReference(packet, second) == ReferenceRasterError::None);
    CHECK(first.pixels == second.pixels);
    CHECK(first.At(0, 0) == Rgba8{25, 39, 55, 255});
    CHECK(first.At(95, 63) == Rgba8{25, 39, 55, 255});
    CHECK(first.At(48, 32) != first.At(0, 0));
}

TEST_CASE("phase6 tolerance comparison reports channel error", "[phase6][raster]")
{
    const std::vector<Rgba8> expected{
        {10, 20, 30, 255}, {100, 110, 120, 255}};
    const std::vector<Rgba8> actual{
        {11, 18, 30, 255}, {100, 112, 125, 255}};
    const auto comparison = CompareRaster(expected, actual);
    CHECK(comparison.comparedPixels == 2);
    CHECK(comparison.differingPixels == 2);
    CHECK(comparison.maximumChannelError == 5);
    CHECK(comparison.meanAbsoluteError == 1.25);
    CHECK(comparison.Within(5, 1.25, 2));
    CHECK_FALSE(comparison.Within(4, 1.25, 2));
}

TEST_CASE("phase6 extent generation changes only with extent", "[phase6][raster]")
{
    ExtentGeneration extent;
    CHECK(extent.Update(96, 64));
    CHECK(extent.Generation() == 1);
    CHECK_FALSE(extent.Update(96, 64));
    CHECK(extent.Generation() == 1);
    CHECK(extent.Update(128, 72));
    CHECK(extent.Generation() == 2);
    CHECK(extent.Width() == 128);
    CHECK(extent.Height() == 72);
    CHECK_FALSE(extent.Update(0, 72));
    CHECK(extent.Generation() == 2);
}

TEST_CASE("phase6 upload arena honors alignment and capacity", "[phase6][raster]")
{
    UploadArena arena{256};
    const auto vertices = arena.Allocate(24, 16);
    CHECK(vertices.offset == 0);
    CHECK(vertices.size == 24);
    const auto material = arena.Allocate(12, 64);
    CHECK(material.offset == 64);
    CHECK(material.size == 12);
    CHECK(arena.Used() == 76);
    CHECK_FALSE(arena.Allocate(200, 16));
    arena.Reset();
    CHECK(arena.Used() == 0);
}

TEST_CASE("phase6 frame graph declares upload raster tone-map readback", "[phase6][raster]")
{
    const auto graph = BuildPhase6FrameGraph();
    REQUIRE(graph.passes.size() == 4);
    CHECK(graph.passes[0].pass == GraphPass::Upload);
    CHECK(graph.passes[1].pass == GraphPass::OpaqueRaster);
    CHECK(graph.passes[2].pass == GraphPass::ToneMap);
    CHECK(graph.passes[3].pass == GraphPass::Readback);
    CHECK(ValidateFrameGraph(graph) == GraphError::None);
    CHECK(CountTransitions(graph) == 5);

    auto malformed = graph;
    malformed.passes.erase(malformed.passes.begin());
    CHECK(ValidateFrameGraph(malformed) == GraphError::ReadBeforeWrite);
}

TEST_CASE("phase6 reflected material layout is fixed", "[phase6][raster]")
{
    CHECK(sizeof(RasterMaterialV1) == 48);
    CHECK(offsetof(RasterMaterialV1, baseColor) == 16);
    CHECK(offsetof(RasterMaterialV1, textureIndex) == 32);
    CHECK(RasterMaterialV1{}.shaderLayoutHash == kPhase6ShaderLayoutHash);
    // No captured texture is the common case -- most of the engine's
    // materials never reach a live capture, and a material that defaulted to
    // index 0 would silently sample whatever the frame's first texture
    // happens to be rather than shading from baseColor alone.
    //
    // Pinned against the literal bit pattern, not the symbol: comparing
    // RasterMaterialV1{}.textureIndex to kNoMaterialTexture on both sides of
    // the same equality is invisible to a mutation that redefines the
    // constant itself, since both sides move together.
    CHECK(kNoMaterialTexture == 0xFFFF'FFFFu);
    CHECK(RasterMaterialV1{}.textureIndex == kNoMaterialTexture);
}

TEST_CASE("PM_a_material_texture_index_survives_encode_and_decode",
    "[raster][material]")
{
    SyntheticPacketOptions options;
    auto bytes = BuildSyntheticPacket(options);
    auto& header = Header(bytes);
    auto* materials = reinterpret_cast<RasterMaterialV1*>(
        bytes.data() + header.materialsOffset);
    materials[0].textureIndex = 7;

    DecodedPacket decoded;
    REQUIRE(DecodePacket(bytes, decoded).error == PacketError::None);
    REQUIRE(!decoded.materials.empty());
    CHECK(decoded.materials[0].textureIndex == 7);
}

TEST_CASE("PM_the_no_texture_sentinel_survives_encode_and_decode",
    "[raster][material]")
{
    // The default is the sentinel, and the sentinel has to be a value a real
    // library index can never collide with -- otherwise "no texture" and
    // "texture number kNoMaterialTexture" are the same bit pattern on the
    // wire and a decoder cannot tell them apart.
    SyntheticPacketOptions options;
    auto bytes = BuildSyntheticPacket(options);
    DecodedPacket decoded;
    REQUIRE(DecodePacket(bytes, decoded).error == PacketError::None);
    REQUIRE(!decoded.materials.empty());
    CHECK(decoded.materials[0].textureIndex == kNoMaterialTexture);
}

TEST_CASE("phase6 material registry rejects duplicate stable IDs", "[phase6][raster]")
{
    SyntheticPacketOptions options;
    options.includeOccludedTriangle = true;
    auto bytes = BuildSyntheticPacket(options);
    auto& header = Header(bytes);
    auto* materials = reinterpret_cast<RasterMaterialV1*>(
        bytes.data() + header.materialsOffset);
    materials[1].resourceId = materials[0].resourceId;
    DecodedPacket decoded;
    CHECK(DecodePacket(bytes, decoded).error ==
          PacketError::DuplicateResource);
}

TEST_CASE("PM_camera_relative_ground_quads_encode_but_upright_ones_do_not",
    "[mirror][raster]")
{
    // Packet validation classifies winding from the XY signed area alone.
    // World-space geometry standing upright therefore has *zero* area in the
    // classifier's plane and is refused as degenerate, however well formed it
    // is in three dimensions. The live mirror hit exactly this and reported
    // only "geometry-encode", which cost a game launch to diagnose.
    const auto build = [](const bool upright, DecodedPacket& packet) {
        packet = {};
        packet.header.width = 256;
        packet.header.height = 192;
        packet.header.frameIndex = 1;
        packet.header.viewportWidth = 256.0f;
        packet.header.viewportHeight = 192.0f;
        packet.header.viewportMaxDepth = 1.0f;
        packet.header.scissorWidth = 256;
        packet.header.scissorHeight = 192;
        RasterMaterialV1 material{};
        material.resourceId = 0x4D49'0000'0000'0001ull;
        packet.materials.push_back(material);
        const std::array<std::array<float, 2>, 4> corners{{
            {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}}};
        for (const auto& corner : corners) {
            RasterVertexV3 vertex{};
            vertex.position[0] = 600.0f + corner[0] * 220.0f;
            // Upright places the quad's second axis in Z, which leaves the
            // XY area at zero; the ground plane places it in Y.
            vertex.position[1] = upright ? 600.0f : 600.0f + corner[1] * 220.0f;
            vertex.position[2] = upright ? corner[1] * 220.0f : -180.0f;
            vertex.color[0] = 0.9f;
            vertex.color[1] = 0.3f;
            vertex.color[2] = 0.2f;
            packet.vertices.push_back(vertex);
        }
        const std::array<std::uint32_t, 6> quad{0, 1, 2, 0, 2, 3};
        for (const auto face :
             {FrontFace::CounterClockwise, FrontFace::Clockwise}) {
            RasterDrawV1 draw{};
            draw.materialId = material.resourceId;
            draw.firstIndex =
                static_cast<std::uint32_t>(packet.indices.size());
            draw.indexCount = static_cast<std::uint32_t>(quad.size());
            draw.frontFace = face;
            draw.depthCompare = DepthCompare::Less;
            packet.draws.push_back(draw);
            packet.indices.insert(
                packet.indices.end(), quad.begin(), quad.end());
        }
    };

    DecodedPacket ground;
    build(false, ground);
    std::vector<std::byte> bytes;
    const auto encodedGround = EncodePacket(ground, bytes);
    CHECK(encodedGround.error == PacketError::None);
    CHECK_FALSE(bytes.empty());

    DecodedPacket upright;
    build(true, upright);
    std::vector<std::byte> uprightBytes;
    const auto encodedUpright = EncodePacket(upright, uprightBytes);
    CHECK(encodedUpright.error == PacketError::DegenerateTriangle);
    CHECK(uprightBytes.empty());
}

TEST_CASE("PM_world_space_packets_are_not_wound_in_the_screen_plane",
    "[mirror][raster]")
{
    // Winding is classified from the XY signed area, which is only a winding
    // for vertices that already lie in the screen plane. Camera-relative
    // world geometry is transformed by the view before it has a winding at
    // all, and any real mesh contains triangles standing edge-on in XY. With
    // one rule for both spaces, a single such triangle rejects the entire
    // packet and no captured engine geometry can ever be submitted.
    const auto build = [](const VertexSpace space, DecodedPacket& packet) {
        packet = {};
        packet.header.width = 256;
        packet.header.height = 192;
        packet.header.frameIndex = 1;
        packet.header.viewportWidth = 256.0f;
        packet.header.viewportHeight = 192.0f;
        packet.header.viewportMaxDepth = 1.0f;
        packet.header.scissorWidth = 256;
        packet.header.scissorHeight = 192;
        packet.header.vertexSpace = space;
        RasterMaterialV1 material{};
        material.resourceId = 0x4D49'0000'0000'0002ull;
        packet.materials.push_back(material);
        // Upright in XZ: a perfectly good triangle that has no area at all
        // in the plane the classifier looks at.
        const std::array<std::array<float, 3>, 3> positions{{
            {-100.0f, 500.0f, -100.0f},
            {100.0f, 500.0f, -100.0f},
            {0.0f, 500.0f, 150.0f}}};
        for (const auto& position : positions) {
            RasterVertexV3 vertex{};
            vertex.position[0] = position[0];
            vertex.position[1] = position[1];
            vertex.position[2] = position[2];
            vertex.color[0] = 0.8f;
            vertex.color[1] = 0.8f;
            vertex.color[2] = 0.8f;
            packet.vertices.push_back(vertex);
        }
        packet.indices = {0, 1, 2};
        RasterDrawV1 draw{};
        draw.materialId = material.resourceId;
        draw.firstIndex = 0;
        draw.indexCount = 3;
        draw.frontFace = FrontFace::CounterClockwise;
        draw.depthCompare = DepthCompare::Less;
        packet.draws.push_back(draw);
    };

    // Declared as screen-space, the classifier applies and refuses it.
    DecodedPacket screenSpace;
    build(VertexSpace::ScreenNdc, screenSpace);
    std::vector<std::byte> screenBytes;
    CHECK(EncodePacket(screenSpace, screenBytes).error ==
        PacketError::DegenerateTriangle);

    // Declared as camera-relative world, the same triangle is accepted: its
    // winding is decided after the view transform, not before it.
    DecodedPacket worldSpace;
    build(VertexSpace::CameraRelativeWorld, worldSpace);
    std::vector<std::byte> worldBytes;
    REQUIRE(EncodePacket(worldSpace, worldBytes).error == PacketError::None);
    DecodedPacket decoded;
    REQUIRE(DecodePacket(worldBytes, decoded).error == PacketError::None);
    CHECK(decoded.header.vertexSpace == VertexSpace::CameraRelativeWorld);

    // The default is screen space, so every artifact encoded before this
    // field existed keeps its exact meaning.
    const PacketHeaderV1 defaulted{};
    CHECK(defaulted.vertexSpace == VertexSpace::ScreenNdc);

    // A space the build does not know must not be guessed at.
    auto unknown = worldSpace;
    unknown.header.vertexSpace = static_cast<VertexSpace>(7);
    std::vector<std::byte> unknownBytes;
    CHECK(EncodePacket(unknown, unknownBytes).error ==
        PacketError::InvalidVertexSpace);
}

TEST_CASE("RP_the_vertex_carries_a_normal_and_older_packets_still_read",
    "[raster][packet]")
{
    using namespace vf::renderer::raster;

    // Without a per-vertex normal every surface shades as though it faced the
    // same way, which reads as a lighting fault rather than as absent data --
    // and it makes the shadow, reflection and indirect phases meaningless on
    // captured geometry, because they all start from N.
    static_assert(sizeof(RasterVertexV3) == 48);
    static_assert(offsetof(RasterVertexV3, normal) == 32);

    DecodedPacket packet{};
    packet.header.width = 4;
    packet.header.height = 4;
    packet.header.indexType = IndexType::Uint32;
    packet.header.viewportWidth = 4.0f;
    packet.header.viewportHeight = 4.0f;
    packet.header.viewportMaxDepth = 1.0f;
    packet.header.scissorWidth = 4;
    packet.header.scissorHeight = 4;
    RasterVertexV3 vertex{};
    vertex.position[0] = 1.0f;
    vertex.normal[0] = 0.0f;
    vertex.normal[1] = 0.0f;
    vertex.normal[2] = 1.0f;
    packet.vertices.assign(3, vertex);
    packet.vertices[1].position[0] = 2.0f;
    packet.vertices[2].position[1] = 3.0f;
    packet.vertices[2].normal[1] = 1.0f;
    packet.vertices[2].normal[2] = 0.0f;
    packet.indices = {0, 1, 2};
    RasterMaterialV1 material{};
    material.resourceId = 0x3000;
    packet.materials.push_back(material);
    RasterDrawV1 draw{};
    draw.indexCount = 3;
    draw.materialId = material.resourceId;
    packet.draws.push_back(draw);

    std::vector<std::byte> bytes;
    const auto encoded = EncodePacket(packet, bytes);
    INFO(ToString(encoded.error));
    REQUIRE(encoded);
    // A new field is a new minor version. Encoding it under the old one would
    // hand a reader forty-eight-byte vertices it will stride at thirty-two.
    {
        DecodedPacket check{};
        REQUIRE(DecodePacket(bytes, check));
        // The encoder stamps whichever version is current, not this test's
        // own idea of one -- otherwise this assertion goes stale the next
        // time a field is added anywhere in the packet and starts failing
        // for a reason that has nothing to do with vertex normals.
        CHECK(check.header.versionMinor == kPacketVersionMinor);
    }

    DecodedPacket round{};
    REQUIRE(DecodePacket(bytes, round));
    REQUIRE(round.vertices.size() == 3);
    CHECK(round.vertices[0].normal[2] == 1.0f);
    CHECK(round.vertices[2].normal[1] == 1.0f);
    CHECK(round.vertices[1].position[0] == 2.0f);

    // Both older forms still decode, and their vertices get a normal that is
    // unit length rather than zero: a zero normal is not "no lighting", it is
    // a division by zero wherever the shading normalises it.
    for (const auto minor : {kPacketPhase6VersionMinor,
        kPacketTexCoordVersionMinor}) {
        INFO(minor);
        DecodedPacket old{};
        old.header = packet.header;
        old.header.versionMinor = minor;
        old.vertices = packet.vertices;
        old.indices = packet.indices;
        old.draws = packet.draws;
        old.materials = packet.materials;

        std::vector<std::byte> oldBytes;
        const auto oldEncoded = EncodePacket(old, oldBytes);
        REQUIRE(oldEncoded);

        DecodedPacket migrated{};
        REQUIRE(DecodePacket(oldBytes, migrated));
        // The older layout was written, not the in-memory one.
        CHECK(migrated.header.versionMinor == minor);
        REQUIRE(migrated.vertices.size() == 3);
        CHECK(migrated.vertices[0].position[0] == 1.0f);
        for (const auto& read : migrated.vertices) {
            const auto length = std::sqrt(
                read.normal[0] * read.normal[0] +
                read.normal[1] * read.normal[1] +
                read.normal[2] * read.normal[2]);
            CHECK(length > 0.99f);
            CHECK(length < 1.01f);
        }
    }
}

TEST_CASE("packet byte ceiling follows the device it will be uploaded to")
{
    // A frame is rejected before it is decoded when its declared size is
    // larger than the renderer could ever upload, because the decoder would
    // otherwise reserve that much host memory on the strength of a number the
    // caller supplied. The ceiling that does that job is not a preference: a
    // packet has to fit in one device allocation, so the device's own reported
    // maximum is the only number that is neither arbitrary nor a guess.
    //
    // A fixed ceiling was arbitrary, and it was wrong in the direction that
    // matters. Sixty-four megabytes rejected a real Fallout 4 exterior cell,
    // which measured a hundred and four million bytes: one million eight
    // hundred thousand vertices at forty-eight bytes each plus four and a half
    // million indices. The frame was refused as a malformed contract, and
    // nothing in the message said the packet was merely large.
    SECTION("the device limit is the ceiling when the device reports one")
    {
        constexpr std::uint64_t reported = 3ull << 30;
        CHECK(MaximumPacketBytes(reported) == reported);
    }

    SECTION("a cell that a real device can hold is admitted")
    {
        // The measured size of Sanctuary, against the smallest ceiling any
        // conformant Vulkan device is permitted to report.
        constexpr std::uint64_t measuredCell = 104'938'288ull;
        CHECK(MaximumPacketBytes(kMinimumDeviceAllocationBytes) >=
            measuredCell);
    }

    SECTION("a device that reports nothing falls back to the guaranteed floor")
    {
        // maxMemoryAllocationSize arrives only with maintenance3. Without it
        // the value is zero, and treating zero as the ceiling would reject
        // every frame -- so the fallback is the minimum the specification
        // requires every implementation to support, never a smaller one.
        CHECK(MaximumPacketBytes(0) == kMinimumDeviceAllocationBytes);
    }

    SECTION("a device below the guaranteed floor is not believed")
    {
        // Reporting less than the specification's minimum means the value is
        // not trustworthy, and clamping up to the floor keeps a bad report
        // from silently shrinking what the renderer accepts.
        CHECK(MaximumPacketBytes(kMinimumDeviceAllocationBytes - 1) ==
            kMinimumDeviceAllocationBytes);
        CHECK(MaximumPacketBytes(1) == kMinimumDeviceAllocationBytes);
    }

    SECTION("a more capable device never accepts less")
    {
        // Monotonic in the reported limit. A ceiling that fell as the device
        // grew would make the larger card the one that refused the frame,
        // which reads as a driver fault rather than as this function.
        std::uint64_t previous = 0;
        for (const std::uint64_t reported : {
                 std::uint64_t{0}, std::uint64_t{1},
                 kMinimumDeviceAllocationBytes - 1,
                 kMinimumDeviceAllocationBytes,
                 kMinimumDeviceAllocationBytes + 1, 4ull << 30, 24ull << 30,
                 std::numeric_limits<std::uint64_t>::max()}) {
            INFO(reported);
            const auto ceiling = MaximumPacketBytes(reported);
            CHECK(ceiling >= previous);
            CHECK(ceiling >= kMinimumDeviceAllocationBytes);
            previous = ceiling;
        }
    }
}

TEST_CASE("PM_the_packet_header_can_be_read_without_decoding_the_geometry",
    "[raster][packet]")
{
    // A caller that declares an unchanged generation is telling the backend
    // the geometry sections are the ones it already holds. The header is not
    // among them: it carries the frame index, the extent and the viewport,
    // which change every frame. Reading it alone is what lets the 66 MB
    // behind it be skipped -- measured at 34 ms of a 95 ms mirrored frame.
    using namespace vf::renderer::raster;
    SyntheticPacketOptions options{};
    options.frameIndex = 9;
    const auto bytes = BuildSyntheticPacket(options);
    REQUIRE_FALSE(bytes.empty());

    PacketHeaderV1 header{};
    REQUIRE(DecodePacketHeader(bytes, header));
    CHECK(header.frameIndex == options.frameIndex);
    CHECK(header.width == options.width);
    CHECK(header.height == options.height);

    // And it refuses the same things a full decode refuses, or a caller could
    // hand over anything at all and have its generation believed.
    auto corrupt = bytes;
    corrupt[0] = std::byte{0};
    PacketHeaderV1 rejected{};
    CHECK_FALSE(DecodePacketHeader(corrupt, rejected));
    CHECK_FALSE(DecodePacketHeader(std::span{bytes}.first(4), rejected));
}
