#include "renderer_api/RasterPacket.h"
#include "renderer_core/EngineMesh.h"
#include "renderer_core/EngineVertex.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <vector>

namespace {

using namespace vf::renderer::mesh;
namespace vertex = vf::renderer::mesh;

constexpr std::uint64_t Flag(const unsigned bit)
{
    return std::uint64_t{1} << bit;
}

constexpr std::uint64_t SetNibble(
    std::uint64_t value, const unsigned bit, const std::uint8_t nibble)
{
    const auto mask = std::uint64_t{0xF} << bit;
    return (value & ~mask) | (static_cast<std::uint64_t>(nibble) << bit);
}

std::array<std::uint64_t, 6> ObservedStaticLayouts()
{
    std::array<std::uint64_t, 6> layouts{};
    layouts[0] = 2 | Flag(44); // HALF4 position

    layouts[1] = 3 | Flag(44) | Flag(45);
    layouts[1] = SetNibble(layouts[1], 8, 2); // UV0 at byte 8

    layouts[2] = 5 | Flag(44) | Flag(45) | Flag(47) | Flag(48);
    layouts[2] = SetNibble(layouts[2], 8, 2);
    layouts[2] = SetNibble(layouts[2], 16, 3);
    layouts[2] = SetNibble(layouts[2], 20, 4);

    layouts[3] = 9 | Flag(44) | Flag(45) | Flag(47) | Flag(48) |
        Flag(49) | Flag(50);
    layouts[3] = SetNibble(layouts[3], 8, 2);
    layouts[3] = SetNibble(layouts[3], 16, 3);
    layouts[3] = SetNibble(layouts[3], 20, 4);
    layouts[3] = SetNibble(layouts[3], 24, 5);
    layouts[3] = SetNibble(layouts[3], 28, 6);

    layouts[4] = 6 | Flag(44) | Flag(45) | Flag(46) | Flag(49);
    layouts[4] = SetNibble(layouts[4], 8, 2);
    layouts[4] = SetNibble(layouts[4], 12, 3);
    layouts[4] = SetNibble(layouts[4], 24, 5);
    layouts[5] = 0x000BB00605430208ull; // live capture 2026-08-15
    return layouts;
}

float RandomFloat(std::mt19937& random, const float low, const float high)
{
    return std::uniform_real_distribution<float>{low, high}(random);
}

DecodedEngineVertex RandomVertex(std::mt19937& random)
{
    DecodedEngineVertex vertex{};
    for (auto& value : vertex.position) {
        value = RandomFloat(random, -64.0f, 64.0f);
    }
    for (auto& value : vertex.texCoord0) {
        value = RandomFloat(random, -4.0f, 4.0f);
    }
    for (auto& value : vertex.texCoord1) {
        value = RandomFloat(random, -4.0f, 4.0f);
    }
    for (auto& value : vertex.normal) {
        value = RandomFloat(random, -1.0f, 1.0f);
    }
    for (auto& value : vertex.tangent) {
        value = RandomFloat(random, -1.0f, 1.0f);
    }
    for (auto& value : vertex.color) {
        value = RandomFloat(random, 0.0f, 1.0f);
    }
    for (auto& value : vertex.skinWeights) {
        value = RandomFloat(random, 0.0f, 1.0f);
    }
    for (auto& value : vertex.landscape0) {
        value = RandomFloat(random, 0.0f, 1.0f);
    }
    for (auto& value : vertex.landscape1) {
        value = RandomFloat(random, 0.0f, 1.0f);
    }
    for (auto& value : vertex.boneIndices) {
        value = static_cast<std::uint8_t>(random() & 0xFFu);
    }
    return vertex;
}

void CheckNear(const float actual, const float expected, const float tolerance)
{
    CHECK(std::abs(actual - expected) <= tolerance);
}

}

TEST_CASE("phase7 observed VertexDesc layouts have exact attribute tables", "[phase7][mesh]")
{
    const auto rawLayouts = ObservedStaticLayouts();
    for (const auto raw : rawLayouts) {
        EngineVertexLayout layout;
        INFO("desc=" << raw);
        REQUIRE(ParseEngineVertexLayout(raw, layout) == VertexLayoutError::None);
        CHECK(layout.raw == raw);
        CHECK(layout.stride == (raw & 0xFu) * 4u);
        REQUIRE(layout.Find(VertexSemantic::Position));
        CHECK(layout.Find(VertexSemantic::Position)->storage ==
              VertexStorage::Half4);
    }
    EngineVertexLayout standard;
    REQUIRE(ParseEngineVertexLayout(rawLayouts[2], standard) ==
            VertexLayoutError::None);
    CHECK(standard.Find(VertexSemantic::TexCoord0)->offset == 8);
    CHECK(standard.Find(VertexSemantic::Normal)->offset == 12);
    CHECK(standard.Find(VertexSemantic::Tangent)->offset == 16);
}

TEST_CASE("phase7 randomized pack unpack parity covers observed layouts", "[phase7][mesh]")
{
    std::mt19937 random{0xF047u};
    for (const auto raw : ObservedStaticLayouts()) {
        EngineVertexLayout layout;
        REQUIRE(ParseEngineVertexLayout(raw, layout) == VertexLayoutError::None);
        std::vector<DecodedEngineVertex> source(257);
        std::generate(source.begin(), source.end(), [&] { return RandomVertex(random); });
        std::vector<std::byte> packed;
        REQUIRE(PackEngineVertices(layout, source, packed) == VertexDecodeError::None);
        CHECK(packed.size() == source.size() * layout.stride);

        for (std::size_t index = 0; index < source.size(); ++index) {
            DecodedEngineVertex decoded{};
            REQUIRE(DecodeEngineVertex(layout, packed, index, decoded) ==
                    VertexDecodeError::None);
            CheckNear(decoded.position[0], source[index].position[0], 0.04f);
            CheckNear(decoded.position[1], source[index].position[1], 0.04f);
            CheckNear(decoded.position[2], source[index].position[2], 0.04f);
            if (layout.Find(VertexSemantic::TexCoord0)) {
                CheckNear(decoded.texCoord0[0], source[index].texCoord0[0], 0.004f);
                CheckNear(decoded.texCoord0[1], source[index].texCoord0[1], 0.004f);
            }
            if (layout.Find(VertexSemantic::Normal)) {
                for (std::size_t component = 0; component < 4; ++component) {
                    CheckNear(decoded.normal[component], source[index].normal[component], 0.008f);
                }
            }
            if (layout.Find(VertexSemantic::Color)) {
                for (std::size_t component = 0; component < 4; ++component) {
                    CheckNear(decoded.color[component], source[index].color[component], 0.004f);
                }
            }
            if (layout.Find(VertexSemantic::SkinWeights)) {
                CHECK(decoded.boneIndices == source[index].boneIndices);
            }
        }

        std::vector<DecodedEngineVertex> decoded(source.size());
        for (std::size_t index = 0; index < decoded.size(); ++index) {
            REQUIRE(DecodeEngineVertex(layout, packed, index, decoded[index]) ==
                    VertexDecodeError::None);
        }
        std::vector<std::byte> repacked;
        REQUIRE(PackEngineVertices(layout, decoded, repacked) ==
                VertexDecodeError::None);
        CHECK(repacked == packed);
    }
}

TEST_CASE("phase7 layout and stream corruption fail closed", "[phase7][mesh]")
{
    EngineVertexLayout layout;
    CHECK(ParseEngineVertexLayout(2, layout) == VertexLayoutError::MissingPosition);

    auto overlap = std::uint64_t{3} | Flag(44) | Flag(45);
    overlap = SetNibble(overlap, 8, 1);
    CHECK(ParseEngineVertexLayout(overlap, layout) ==
          VertexLayoutError::AttributeOverlap);

    auto outside = std::uint64_t{2} | Flag(44) | Flag(45);
    outside = SetNibble(outside, 8, 2);
    CHECK(ParseEngineVertexLayout(outside, layout) ==
          VertexLayoutError::AttributeOutOfBounds);

    const auto raw = ObservedStaticLayouts()[1];
    REQUIRE(ParseEngineVertexLayout(raw, layout) == VertexLayoutError::None);
    const std::array<std::byte, 11> truncated{};
    DecodedEngineVertex decoded{};
    CHECK(DecodeEngineVertex(layout, truncated, 0, decoded) ==
          VertexDecodeError::TruncatedStream);
    CHECK(DecodeEngineVertex(layout, {}, 1, decoded) ==
          VertexDecodeError::VertexOutOfRange);
}

TEST_CASE("phase7 mesh packet round trips raw engine payload", "[phase7][mesh]")
{
    const auto raw = ObservedStaticLayouts()[1];
    EngineVertexLayout layout;
    REQUIRE(ParseEngineVertexLayout(raw, layout) == VertexLayoutError::None);
    std::vector<DecodedEngineVertex> vertices(4);
    vertices[0].position = {-2.0f, -1.0f, 0.0f, 1.0f};
    vertices[1].position = {3.0f, -1.0f, 0.0f, 1.0f};
    vertices[2].position = {3.0f, 4.0f, 1.0f, 1.0f};
    vertices[3].position = {-2.0f, 4.0f, 1.0f, 1.0f};

    CapturedMesh source{};
    source.resourceId = 0x7000'0000'0000'0001ull;
    source.generation = 7;
    source.vertexDesc = raw;
    source.stride = layout.stride;
    source.vertexCount = static_cast<std::uint32_t>(vertices.size());
    source.indices = {0, 1, 2, 0, 2, 3};
    source.firstIndex = 0;
    source.indexCount = 6;
    source.baseVertex = 0;
    REQUIRE(PackEngineVertices(layout, vertices, source.vertexBytes) ==
            VertexDecodeError::None);

    std::vector<std::byte> encoded;
    REQUIRE(EncodeCapturedMesh(source, encoded) == MeshPacketError::None);
    CapturedMesh decoded;
    REQUIRE(DecodeCapturedMesh(encoded, decoded) == MeshPacketError::None);
    CHECK(decoded.resourceId == source.resourceId);
    CHECK(decoded.generation == source.generation);
    CHECK(decoded.vertexDesc == source.vertexDesc);
    CHECK(decoded.vertexBytes == source.vertexBytes);
    CHECK(decoded.indices == source.indices);
    CHECK(decoded.firstIndex == 0);
    CHECK(decoded.indexCount == 6);
}

TEST_CASE("phase7 captured mesh translates range base winding and bounds", "[phase7][mesh]")
{
    const auto raw = ObservedStaticLayouts()[0];
    EngineVertexLayout layout;
    REQUIRE(ParseEngineVertexLayout(raw, layout) == VertexLayoutError::None);
    std::vector<DecodedEngineVertex> vertices(5);
    vertices[0].position = {-9.0f, -9.0f, 0.0f, 1.0f};
    vertices[1].position = {-2.0f, -1.0f, 3.0f, 1.0f};
    vertices[2].position = {4.0f, -1.0f, 3.0f, 1.0f};
    vertices[3].position = {1.0f, 5.0f, 3.0f, 1.0f};
    vertices[4].position = {9.0f, 9.0f, 0.0f, 1.0f};

    CapturedMesh mesh{};
    mesh.resourceId = 0x7000'0000'0000'0002ull;
    mesh.generation = 1;
    mesh.vertexDesc = raw;
    mesh.stride = layout.stride;
    mesh.vertexCount = 5;
    mesh.indices = {0, 0, 1, 2};
    mesh.firstIndex = 1;
    mesh.indexCount = 3;
    mesh.baseVertex = 1;
    REQUIRE(PackEngineVertices(layout, vertices, mesh.vertexBytes) ==
            VertexDecodeError::None);

    std::vector<std::byte> rasterBytes;
    MeshTranslationReport report{};
    REQUIRE(TranslateCapturedMesh(mesh, 128, 96, rasterBytes, report) ==
            MeshPacketError::None);
    CHECK(report.sourceBounds.minimum == std::array<float, 3>{-2.0f, -1.0f, 3.0f});
    CHECK(report.sourceBounds.maximum == std::array<float, 3>{4.0f, 5.0f, 3.0f});
    CHECK(report.translatedVertexCount == 3);
    CHECK(report.translatedIndexCount == 3);

    vf::renderer::raster::DecodedPacket raster;
    REQUIRE(vf::renderer::raster::DecodePacket(rasterBytes, raster));
    CHECK(raster.indices == std::vector<std::uint32_t>{0, 1, 2});
    REQUIRE(raster.draws.size() == 1);
    CHECK(raster.draws[0].firstIndex == 0);
    CHECK(raster.draws[0].indexCount == 3);
    CHECK(raster.draws[0].vertexOffset == 0);
}

TEST_CASE("phase7 malformed mesh ranges never reach raster submission", "[phase7][mesh]")
{
    CapturedMesh mesh{};
    mesh.resourceId = 1;
    mesh.generation = 1;
    mesh.vertexDesc = ObservedStaticLayouts()[0];
    mesh.stride = 8;
    mesh.vertexCount = 1;
    mesh.vertexBytes.resize(8);
    mesh.indices = {0, 1, 2};
    mesh.indexCount = 3;
    std::vector<std::byte> raster;
    MeshTranslationReport report{};
    CHECK(TranslateCapturedMesh(mesh, 64, 64, raster, report) ==
          MeshPacketError::VertexOutOfRange);
    CHECK(raster.empty());

    std::vector<std::byte> encoded;
    REQUIRE(EncodeCapturedMesh(mesh, encoded) == MeshPacketError::None);
    encoded.pop_back();
    CapturedMesh decoded;
    CHECK(DecodeCapturedMesh(encoded, decoded) == MeshPacketError::SizeMismatch);
}

TEST_CASE("PVL_a_pooled_layout_is_taken_from_the_engines_own_input_elements",
    "[livescene][vertex]")
{
    // The pooled draw stream carries no engine vertex descriptor -- only a
    // D3D stride -- so the layout has to come from the input layout the engine
    // created. Reading position as three floats because the stride is large
    // enough for three floats is what turned a cell into a fan of spikes:
    // Fallout 4 stores position as four halves, and half bit patterns read as
    // float32 collapse almost every vertex onto the origin.
    const std::array<vertex::InputElementDesc, 5> elements{{
        {"POSITION", 0, vertex::kFormatR16G16B16A16Float, 0, 0},
        {"TEXCOORD", 0, vertex::kFormatR16G16Float, 0, 8},
        {"NORMAL", 0, vertex::kFormatR8G8B8A8Unorm, 0, 12},
        {"TANGENT", 0, vertex::kFormatR8G8B8A8Unorm, 0, 16},
        {"COLOR", 0, vertex::kFormatR8G8B8A8Unorm, 0, 20}}};

    vertex::EngineVertexLayout layout{};
    REQUIRE(vertex::BuildLayoutFromInputElements(elements, 32, 0, layout) ==
        vertex::VertexLayoutError::None);

    const auto* const position =
        layout.Find(vertex::VertexSemantic::Position);
    REQUIRE(position != nullptr);
    CHECK(position->storage == vertex::VertexStorage::Half4);
    CHECK(position->offset == 0);
    CHECK(position->byteSize == 8);
    CHECK(layout.Find(vertex::VertexSemantic::Normal) != nullptr);
    CHECK(layout.Find(vertex::VertexSemantic::Color) != nullptr);

    // A half4 position decodes to the value the engine wrote, not to a
    // denormal. 0x3C00 is 1.0, 0xC000 is -2.0, 0x4900 is 10.0.
    std::array<std::byte, 32> raw{};
    const std::array<std::uint16_t, 4> halves{0x3C00, 0xC000, 0x4900, 0x3C00};
    std::memcpy(raw.data(), halves.data(), sizeof(halves));
    vertex::DecodedEngineVertex decoded{};
    REQUIRE(vertex::DecodeEngineVertex(layout, raw, 0, decoded) ==
        vertex::VertexDecodeError::None);
    CHECK(decoded.position[0] == Catch::Approx(1.0f));
    CHECK(decoded.position[1] == Catch::Approx(-2.0f));
    CHECK(decoded.position[2] == Catch::Approx(10.0f));

    // And a full-precision layout is decoded as floats rather than forced
    // through the half path. Both formats exist in one game, so a decoder
    // that assumes either one is wrong for half the meshes.
    const std::array<vertex::InputElementDesc, 2> full{{
        {"POSITION", 0, vertex::kFormatR32G32B32Float, 0, 0},
        {"TEXCOORD", 0, vertex::kFormatR32G32Float, 0, 12}}};
    vertex::EngineVertexLayout precise{};
    REQUIRE(vertex::BuildLayoutFromInputElements(full, 20, 0, precise) ==
        vertex::VertexLayoutError::None);
    CHECK(precise.Find(vertex::VertexSemantic::Position)->storage ==
        vertex::VertexStorage::Float3);

    std::array<std::byte, 20> preciseRaw{};
    const std::array<float, 3> exact{1234.5f, -67.25f, 0.125f};
    std::memcpy(preciseRaw.data(), exact.data(), sizeof(exact));
    vertex::DecodedEngineVertex preciseDecoded{};
    REQUIRE(vertex::DecodeEngineVertex(precise, preciseRaw, 0,
        preciseDecoded) == vertex::VertexDecodeError::None);
    CHECK(preciseDecoded.position[0] == Catch::Approx(1234.5f));
    CHECK(preciseDecoded.position[1] == Catch::Approx(-67.25f));
    CHECK(preciseDecoded.position[2] == Catch::Approx(0.125f));

    // Elements for another input slot belong to a different stream and must
    // not be folded into this one's offsets.
    const std::array<vertex::InputElementDesc, 2> split{{
        {"POSITION", 0, vertex::kFormatR16G16B16A16Float, 0, 0},
        {"TEXCOORD", 0, vertex::kFormatR16G16Float, 1, 0}}};
    vertex::EngineVertexLayout slotZero{};
    REQUIRE(vertex::BuildLayoutFromInputElements(split, 8, 0, slotZero) ==
        vertex::VertexLayoutError::None);
    CHECK(slotZero.attributes.size() == 1);

    // A layout with no position is not a geometry layout at all.
    const std::array<vertex::InputElementDesc, 1> noPosition{{
        {"TEXCOORD", 0, vertex::kFormatR16G16Float, 0, 0}}};
    vertex::EngineVertexLayout empty{};
    CHECK(vertex::BuildLayoutFromInputElements(noPosition, 8, 0, empty) ==
        vertex::VertexLayoutError::MissingPosition);

    // An attribute that runs past the stride would read the next vertex.
    const std::array<vertex::InputElementDesc, 2> overrun{{
        {"POSITION", 0, vertex::kFormatR16G16B16A16Float, 0, 0},
        {"TEXCOORD", 0, vertex::kFormatR16G16Float, 0, 6}}};
    vertex::EngineVertexLayout past{};
    CHECK(vertex::BuildLayoutFromInputElements(overrun, 8, 0, past) ==
        vertex::VertexLayoutError::AttributeOutOfBounds);
}
