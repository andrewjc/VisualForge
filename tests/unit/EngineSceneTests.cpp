#include "renderer_core/EngineScene.h"
#include "renderer_core/EngineTransparency.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using namespace vf::renderer;
namespace blend = vf::renderer::blend;

view::SourceMatrix4 SourceFromCanonical(const view::Matrix4& matrix)
{
    view::SourceMatrix4 source{};
    source.storage = view::MatrixStorage::RowMajor;
    source.vectors = view::VectorConvention::ColumnVector;
    std::copy(std::begin(matrix.elements), std::end(matrix.elements),
        std::begin(source.elements));
    return source;
}

view::ViewRecordV1 BuildView()
{
    view::CapturedView captured{};
    captured.viewId = 0x4101;
    captured.cameraId = 0x4201;
    captured.projectionMode = view::ProjectionMode::Perspective;
    captured.handedness = view::Handedness::LeftHanded;
    captured.flags = view::ViewCameraRelative;
    captured.outputWidth = 96;
    captured.outputHeight = 64;
    captured.renderScale = 1.0f;
    captured.nearPlane = 0.1f;
    captured.farPlane = 100.0f;
    captured.verticalFovRadians = 1.0471975512f;
    captured.viewport = {0.0f, 0.0f, 96.0f, 64.0f, 0.0f, 1.0f};
    captured.scissor = {0, 0, 96, 64};
    const auto identity = view::IdentityMatrix();
    const auto projection = view::BuildPerspectiveProjection(
        captured.verticalFovRadians, 1.5f,
        captured.nearPlane, captured.farPlane,
        captured.handedness);
    captured.view = SourceFromCanonical(identity);
    captured.projection = SourceFromCanonical(projection);
    captured.previousView = SourceFromCanonical(identity);
    captured.previousProjection = SourceFromCanonical(projection);
    view::ViewRecordV1 translated{};
    REQUIRE(view::TranslateCapturedView(captured, translated) ==
        view::ViewError::None);
    return translated;
}

raster::DecodedPacket BuildRaster()
{
    raster::DecodedPacket packet{};
    packet.header.frameIndex = 11;
    packet.header.width = 96;
    packet.header.height = 64;
    packet.header.viewportWidth = 96.0f;
    packet.header.viewportHeight = 64.0f;
    packet.header.viewportMaxDepth = 1.0f;
    packet.header.scissorWidth = 96;
    packet.header.scissorHeight = 64;
    packet.header.indexType = raster::IndexType::Uint16;

    const std::array<std::array<float, 3>, 3> positions{{
        {-0.45f, -0.40f, 0.0f},
        {0.0f, 0.50f, 0.0f},
        {0.45f, -0.40f, 0.0f},
    }};
    const std::array<std::array<float, 3>, 3> colors{{
        {0.9f, 0.2f, 0.1f},
        {0.2f, 0.9f, 0.1f},
        {0.1f, 0.2f, 0.9f},
    }};
    for (std::uint32_t object = 0; object < 3; ++object) {
        for (std::uint32_t vertex = 0; vertex < 3; ++vertex) {
            raster::RasterVertexV3 value{};
            std::copy(positions[vertex].begin(), positions[vertex].end(),
                value.position);
            std::copy(colors[(vertex + object) % colors.size()].begin(),
                colors[(vertex + object) % colors.size()].end(),
                value.color);
            value.texCoord[0] = vertex == 2 ? 1.0f : 0.0f;
            value.texCoord[1] = vertex == 1 ? 0.0f : 1.0f;
            // The same -Z these objects declare. Left at the +Z default the
            // vertices would contradict their own object record, and the
            // g-buffer would report a normal the fixture never described.
            value.normal[0] = 0.0f;
            value.normal[1] = 0.0f;
            value.normal[2] = -1.0f;
            packet.vertices.push_back(value);
            packet.indices.push_back(object * 3 + vertex);
        }
        const auto materialId = 0x5201ull + object;
        packet.materials.push_back({materialId,
            raster::kPhase6ShaderLayoutHash,
            {1.0f - 0.1f * static_cast<float>(object),
             0.8f + 0.05f * static_cast<float>(object),
             0.7f + 0.1f * static_cast<float>(object), 1.0f}});
        packet.draws.push_back({materialId, object * 3, 3, 0,
            raster::FrontFace::CounterClockwise,
            raster::DepthCompare::Less, 0});
    }
    return packet;
}

void SetTransform(
    scene::OpaqueObjectV1& object,
    const float scale,
    const float x,
    const float y,
    const float z)
{
    std::fill(std::begin(object.model), std::end(object.model), 0.0f);
    object.model[0] = scale;
    object.model[5] = scale;
    object.model[10] = scale;
    object.model[3] = x;
    object.model[7] = y;
    object.model[11] = z;
    object.model[15] = 1.0f;
    std::copy(std::begin(object.model), std::end(object.model),
        std::begin(object.previousModel));
}

view::PassRecordV1 BuildPass(
    const std::uint64_t sequence,
    const view::ShaderDomain domain,
    const std::uint32_t flags)
{
    view::PassRecordV1 pass{};
    pass.sequence = sequence;
    pass.viewId = 0x4101;
    pass.domain = domain;
    pass.technique = 0x1234;
    pass.renderMode = 0;
    pass.targetId = 2;
    pass.flags = flags;
    pass.category = view::ClassifyPass(
        pass.domain, pass.renderMode, pass.flags);
    return pass;
}

view::FramePacket BuildFrame()
{
    view::FramePacket packet{};
    packet.header.frameId = 11;
    packet.header.engineFrameId = 0xE00Bull;
    packet.header.historyEpoch = 1;
    packet.header.captureSequence = 77;
    packet.header.captureThreadId = 9;
    packet.header.renderThreadId = 9;
    packet.views.push_back(BuildView());
    // Two opaque world passes are mirrored; the sky pass is a declared
    // unsupported class that Phase 11 defers without losing accounting.
    packet.passes.push_back(BuildPass(1, view::ShaderDomain::Lighting,
        view::PassWritesWorldTarget));
    packet.passes.push_back(BuildPass(2, view::ShaderDomain::DistantTree,
        view::PassWritesWorldTarget));
    packet.passes.push_back(BuildPass(3, view::ShaderDomain::Sky,
        view::PassWritesWorldTarget));
    return packet;
}

scene::ScenePacket BuildScene()
{
    scene::ScenePacket packet{};
    packet.header.frameId = 11;
    packet.header.viewId = 0x4101;
    packet.header.captureSequence = 77;
    packet.header.captureThreadId = 9;
    packet.header.renderThreadId = 9;
    for (std::uint32_t index = 0; index < 3; ++index) {
        scene::OpaqueObjectV1 object{};
        object.objectId = 0x5101ull + index;
        object.materialId = 0x5201ull + index;
        object.drawIndex = index;
        object.passSequence = index == 2 ? 2 : 1;
        object.flags = scene::ObjectWritesWorldTarget | scene::ObjectStatic;
        object.roughness = 0.2f + 0.25f * static_cast<float>(index);
        object.boundsMinimum[0] = -0.45f;
        object.boundsMinimum[1] = -0.40f;
        object.boundsMaximum[0] = 0.45f;
        object.boundsMaximum[1] = 0.50f;
        object.geometricNormal[2] = -1.0f;
        object.shadingNormal[2] = -1.0f;
        packet.objects.push_back(object);
    }
    // Objects 0 and 1 project to the same coverage. Object 0 is nearer and
    // must win regardless of opaque submission order.
    SetTransform(packet.objects[0], 1.0f, -0.35f, 0.0f, 2.0f);
    SetTransform(packet.objects[1], 2.0f, -0.70f, 0.0f, 4.0f);
    SetTransform(packet.objects[2], 0.9f, 1.15f, 0.1f, 3.0f);
    return packet;
}

std::uint64_t ReadId(const std::uint32_t (&id)[2])
{
    return static_cast<std::uint64_t>(id[0]) |
        (static_cast<std::uint64_t>(id[1]) << 32);
}

scene::InstanceV1 BuildInstanceRecord(
    const std::uint64_t objectId,
    const std::uint32_t objectIndex,
    const float scale,
    const float x,
    const float y,
    const float z)
{
    scene::InstanceV1 instance{};
    instance.objectId = objectId;
    instance.objectIndex = objectIndex;
    instance.flags = scene::InstanceStatic;
    instance.model[0] = scale;
    instance.model[5] = scale;
    instance.model[10] = scale;
    instance.model[3] = x;
    instance.model[7] = y;
    instance.model[11] = z;
    instance.model[15] = 1.0f;
    std::copy(std::begin(instance.model), std::end(instance.model),
        std::begin(instance.previousModel));
    instance.parameters[0] = 1.0f;
    instance.parameters[1] = 1.0f;
    instance.parameters[2] = 1.0f;
    instance.parameters[3] = 1.0f;
    return instance;
}

scene::ScenePacket BuildInstancedScene()
{
    auto packet = BuildScene();
    // Object 0 repeats twice; the remaining objects render once each. The
    // runs stay contiguous so one draw can cover an object's instances.
    packet.instances.push_back(
        BuildInstanceRecord(0x6101, 0, 1.0f, -0.35f, 0.0f, 2.0f));
    packet.instances.push_back(
        BuildInstanceRecord(0x6102, 0, 0.6f, 0.60f, 0.05f, 2.4f));
    packet.instances.push_back(
        BuildInstanceRecord(0x6103, 1, 2.0f, -0.70f, 0.0f, 4.0f));
    packet.instances.push_back(
        BuildInstanceRecord(0x6104, 2, 0.9f, 1.15f, 0.1f, 3.0f));
    packet.instances[1].parameters[0] = 0.5f;
    packet.instances[1].parameters[1] = 0.25f;
    return packet;
}

}

TEST_CASE("P11_scene_pass_accounting_binds_objects_to_classified_world_passes",
    "[phase11][scene]")
{
    const auto frame = BuildFrame();
    const auto source = BuildScene();
    scene::SceneCoverage coverage{};
    REQUIRE(scene::ValidateSceneAgainstFrame(source, frame, coverage) ==
        scene::ScenePacketError::None);
    CHECK(coverage.worldWritingPasses == 3);
    CHECK(coverage.opaquePasses == 2);
    CHECK(coverage.mirroredPasses == 2);
    CHECK(coverage.deferredClasses == 1);
    CHECK(coverage.unknownWorldWriters == 0);
    CHECK(coverage.unmirroredOpaquePasses == 0);
    CHECK(coverage.MirrorEligible());

    auto invalid = source;
    invalid.objects[0].passSequence = 0;
    CHECK(scene::ValidateScenePacket(invalid) ==
        scene::ScenePacketError::InvalidIdentity);

    invalid = source;
    invalid.objects[0].passSequence = 99;
    CHECK(scene::ValidateSceneAgainstFrame(invalid, frame, coverage) ==
        scene::ScenePacketError::UnknownPass);

    // The sky pass is classified but is not an opaque world pass, so an
    // object may not claim it while vanilla still owns that class.
    invalid = source;
    invalid.objects[0].passSequence = 3;
    CHECK(scene::ValidateSceneAgainstFrame(invalid, frame, coverage) ==
        scene::ScenePacketError::PassClassMismatch);

    invalid = source;
    invalid.objects.pop_back();
    CHECK(scene::ValidateSceneAgainstFrame(invalid, frame, coverage) ==
        scene::ScenePacketError::UncoveredPass);
    CHECK(coverage.unmirroredOpaquePasses == 1);
    CHECK_FALSE(coverage.MirrorEligible());

    auto unclassified = frame;
    unclassified.passes.push_back(BuildPass(4, view::ShaderDomain::Effect,
        view::PassWritesWorldTarget));
    REQUIRE(unclassified.passes.back().category ==
        view::PassCategory::Unknown);
    CHECK(scene::ValidateSceneAgainstFrame(source, unclassified, coverage) ==
        scene::ScenePacketError::UnclassifiedWorldWriter);
    CHECK(coverage.unknownWorldWriters == 1);
    CHECK_FALSE(coverage.MirrorEligible());

    auto otherView = frame;
    otherView.passes[0].viewId = 0x9999;
    CHECK(scene::ValidateSceneAgainstFrame(source, otherView, coverage) ==
        scene::ScenePacketError::UnknownPass);

    auto otherFrame = frame;
    otherFrame.header.frameId = 12;
    CHECK(scene::ValidateSceneAgainstFrame(source, otherFrame, coverage) ==
        scene::ScenePacketError::FrameMismatch);

    auto missingView = frame;
    missingView.views[0].viewId = 0x4102;
    CHECK(scene::ValidateSceneAgainstFrame(source, missingView, coverage) ==
        scene::ScenePacketError::ViewMismatch);
}

TEST_CASE("P11_scene_packet_is_pointer_free_deterministic_and_checksummed",
    "[phase11][scene]")
{
    const auto source = BuildScene();
    std::vector<std::byte> first;
    std::vector<std::byte> second;
    REQUIRE(scene::EncodeScenePacket(source, first) ==
        scene::ScenePacketError::None);
    REQUIRE(scene::EncodeScenePacket(source, second) ==
        scene::ScenePacketError::None);
    CHECK(first == second);
    CHECK(first.size() == sizeof(scene::ScenePacketHeaderV1) +
        source.objects.size() * sizeof(scene::OpaqueObjectV1));

    scene::ScenePacket decoded;
    REQUIRE(scene::DecodeScenePacket(first, decoded) ==
        scene::ScenePacketError::None);
    CHECK(decoded.header.frameId == 11);
    CHECK(decoded.header.viewId == 0x4101);
    REQUIRE(decoded.objects.size() == 3);
    CHECK(decoded.objects[2].objectId == 0x5103);
    CHECK(decoded.objects[2].drawIndex == 2);

    first.back() ^= std::byte{0x40};
    CHECK(scene::DecodeScenePacket(first, decoded) ==
        scene::ScenePacketError::ChecksumMismatch);
}

TEST_CASE("P11_scene_validation_rejects_duplicate_identity_draw_and_state_drift",
    "[phase11][scene]")
{
    const auto rasterPacket = BuildRaster();
    auto source = BuildScene();
    REQUIRE(scene::ValidateScenePacket(source) ==
        scene::ScenePacketError::None);
    REQUIRE(scene::ValidateSceneAgainstRaster(
        source, rasterPacket, 11, 0x4101) ==
        scene::ScenePacketError::None);

    auto invalid = source;
    invalid.objects[1].objectId = invalid.objects[0].objectId;
    CHECK(scene::ValidateScenePacket(invalid) ==
        scene::ScenePacketError::DuplicateObject);

    invalid = source;
    invalid.objects[1].drawIndex = invalid.objects[0].drawIndex;
    CHECK(scene::ValidateScenePacket(invalid) ==
        scene::ScenePacketError::DuplicateDraw);

    invalid = source;
    invalid.objects[0].roughness = 1.1f;
    CHECK(scene::ValidateScenePacket(invalid) ==
        scene::ScenePacketError::InvalidRoughness);

    invalid = source;
    invalid.objects[0].model[0] = 0.0f;
    CHECK(scene::ValidateScenePacket(invalid) ==
        scene::ScenePacketError::InvalidTransform);

    invalid = source;
    invalid.objects[0].boundsMinimum[0] = 2.0f;
    CHECK(scene::ValidateScenePacket(invalid) ==
        scene::ScenePacketError::InvalidBounds);

    invalid = source;
    invalid.objects[0].materialId = 0xDEAD;
    CHECK(scene::ValidateSceneAgainstRaster(
        invalid, rasterPacket, 11, 0x4101) ==
        scene::ScenePacketError::MissingMaterial);
    CHECK(scene::ValidateSceneAgainstRaster(
        source, rasterPacket, 12, 0x4101) ==
        scene::ScenePacketError::FrameMismatch);
    CHECK(scene::ValidateSceneAgainstRaster(
        source, rasterPacket, 11, 0x9999) ==
        scene::ScenePacketError::ViewMismatch);
}

TEST_CASE("P11_object_models_and_view_expand_shared_draws_into_projected_geometry",
    "[phase11][scene]")
{
    const auto source = BuildRaster();
    const auto capturedView = BuildView();
    const auto capturedScene = BuildScene();
    raster::DecodedPacket projected;
    REQUIRE(scene::ProjectScenePacket(
        source, capturedView, capturedScene, projected) ==
        scene::ScenePacketError::None);
    REQUIRE(projected.draws.size() == capturedScene.objects.size());
    REQUIRE(projected.vertices.size() == 9);
    REQUIRE(projected.indices.size() == 9);
    for (const auto& vertex : projected.vertices) {
        CHECK(std::isfinite(vertex.position[0]));
        CHECK(std::isfinite(vertex.position[1]));
        CHECK(vertex.position[2] >= 0.0f);
        CHECK(vertex.position[2] <= 1.0f);
    }
    CHECK(projected.vertices[0].position[0] !=
        Catch::Approx(source.vertices[0].position[0]));
}

TEST_CASE("P11_opaque_scene_gbuffer_preserves_ids_normals_depth_and_order_independence",
    "[phase11][scene]")
{
    const auto rasterPacket = BuildRaster();
    const auto capturedView = BuildView();
    auto capturedScene = BuildScene();
    raster::DecodedPacket projected;
    REQUIRE(scene::ProjectScenePacket(
        rasterPacket, capturedView, capturedScene, projected) ==
        scene::ScenePacketError::None);
    scene::GBufferImage first;
    REQUIRE(scene::RenderReferenceGBuffer(
        projected, capturedScene, first) ==
        scene::ScenePacketError::None);
    REQUIRE(first.pixels.size() == 96u * 64u);

    std::uint64_t nearPixels{};
    std::uint64_t occludedFarPixels{};
    std::uint64_t thirdPixels{};
    std::uint64_t backgroundPixels{};
    for (const auto& pixel : first.pixels) {
        const auto objectId = ReadId(pixel.objectId);
        if (objectId == 0x5101) {
            ++nearPixels;
            CHECK(ReadId(pixel.materialId) == 0x5201);
            CHECK(pixel.geometricNormalRoughness[2] ==
                Catch::Approx(-1.0f));
            CHECK(pixel.geometricNormalRoughness[3] ==
                Catch::Approx(0.2f));
            CHECK(pixel.shadingNormalDepth[3] < 1.0f);
        } else if (objectId == 0x5102) {
            ++occludedFarPixels;
        } else if (objectId == 0x5103) {
            ++thirdPixels;
        } else {
            CHECK(objectId == 0);
            ++backgroundPixels;
        }
    }
    CHECK(nearPixels > 50);
    CHECK(occludedFarPixels == 0);
    CHECK(thirdPixels > 20);
    CHECK(backgroundPixels > 1'000);

    std::reverse(capturedScene.objects.begin(),
        capturedScene.objects.end());
    REQUIRE(scene::ProjectScenePacket(
        rasterPacket, capturedView, capturedScene, projected) ==
        scene::ScenePacketError::None);
    scene::GBufferImage reversed;
    REQUIRE(scene::RenderReferenceGBuffer(
        projected, capturedScene, reversed) ==
        scene::ScenePacketError::None);
    const auto comparison = scene::CompareGBuffer(
        first.pixels, reversed.pixels);
    CHECK(comparison.Within(0.0f, 0.0, 0));
    CHECK(comparison.identityMismatches == 0);
}

TEST_CASE("P12_instanced_scene_packets_round_trip_and_reject_malformed_runs",
    "[phase12][scene]")
{
    const auto legacy = BuildScene();
    std::vector<std::byte> legacyBytes;
    REQUIRE(scene::EncodeScenePacket(legacy, legacyBytes) ==
        scene::ScenePacketError::None);
    scene::ScenePacket decodedLegacy;
    REQUIRE(scene::DecodeScenePacket(legacyBytes, decodedLegacy) ==
        scene::ScenePacketError::None);
    // A scene without instance records still encodes as version 1.0.
    CHECK(decodedLegacy.header.versionMinor == 0);
    CHECK(decodedLegacy.instances.empty());

    const auto source = BuildInstancedScene();
    std::vector<std::byte> bytes;
    REQUIRE(scene::EncodeScenePacket(source, bytes) ==
        scene::ScenePacketError::None);
    CHECK(bytes.size() == sizeof(scene::ScenePacketHeaderV1) +
        source.objects.size() * sizeof(scene::OpaqueObjectV1) +
        source.instances.size() * sizeof(scene::InstanceV1));
    scene::ScenePacket decoded;
    REQUIRE(scene::DecodeScenePacket(bytes, decoded) ==
        scene::ScenePacketError::None);
    CHECK(decoded.header.versionMinor == 1);
    REQUIRE(decoded.instances.size() == 4);
    CHECK(decoded.instances[1].objectId == 0x6102);
    CHECK(decoded.instances[3].objectIndex == 2);

    auto invalid = source;
    invalid.instances[0].objectIndex = 7;
    CHECK(scene::ValidateScenePacket(invalid) ==
        scene::ScenePacketError::InvalidInstance);

    // Runs must stay contiguous so one draw covers an object's instances.
    invalid = source;
    std::swap(invalid.instances[1], invalid.instances[2]);
    CHECK(scene::ValidateScenePacket(invalid) ==
        scene::ScenePacketError::InvalidInstance);

    invalid = source;
    invalid.instances[2].objectId = invalid.instances[0].objectId;
    CHECK(scene::ValidateScenePacket(invalid) ==
        scene::ScenePacketError::DuplicateInstance);

    invalid = source;
    invalid.instances.pop_back();
    CHECK(scene::ValidateScenePacket(invalid) ==
        scene::ScenePacketError::UncoveredObject);

    invalid = source;
    invalid.instances[0].model[0] = 0.0f;
    CHECK(scene::ValidateScenePacket(invalid) ==
        scene::ScenePacketError::InvalidTransform);

    invalid = source;
    invalid.instances[0].parameters[3] = 1.5f;
    CHECK(scene::ValidateScenePacket(invalid) ==
        scene::ScenePacketError::InvalidParameters);

    invalid = source;
    invalid.instances[0].flags = 0;
    CHECK(scene::ValidateScenePacket(invalid) ==
        scene::ScenePacketError::InvalidFlags);
}

TEST_CASE("P15_reference_gbuffer_honours_alpha_cutout_and_two_sided_frames",
    "[phase15][scene]")
{
    const auto rasterPacket = BuildRaster();
    const auto capturedView = BuildView();
    auto capturedScene = BuildScene();

    // A base-colour texture whose alpha is opaque on one half and clear on
    // the other, so a cutout produces a real silhouette rather than an
    // all-or-nothing object.
    texture::CapturedTexture cutout{};
    cutout.resourceId = 0x8000'0000'0000'15A1ull;
    cutout.generation = 1;
    cutout.width = 2;
    cutout.height = 2;
    cutout.resourceFormat = texture::TextureFormat::R8G8B8A8Unorm;
    cutout.viewFormat = texture::TextureFormat::R8G8B8A8Unorm;
    cutout.sampler.minFilter = texture::TextureFilter::Nearest;
    cutout.sampler.magFilter = texture::TextureFilter::Nearest;
    cutout.sampler.mipFilter = texture::TextureFilter::Nearest;
    cutout.sampler.maxLod = 0.0f;
    texture::TextureSubresource level{};
    level.width = 2;
    level.height = 2;
    level.rowPitch = 8;
    level.slicePitch = 16;
    level.bytes.resize(16);
    const std::array<std::uint8_t, 4> alphas{255, 0, 255, 0};
    for (std::size_t texel = 0; texel < 4; ++texel) {
        level.bytes[texel * 4 + 0] = std::byte{255};
        level.bytes[texel * 4 + 1] = std::byte{255};
        level.bytes[texel * 4 + 2] = std::byte{255};
        level.bytes[texel * 4 + 3] = static_cast<std::byte>(alphas[texel]);
    }
    cutout.subresources.push_back(std::move(level));

    // Object 0 opaque control, object 1 alpha tested, object 2 two-sided.
    for (std::size_t index = 0; index < capturedScene.objects.size();
         ++index) {
        visibility::VisibilityRecordV1 record{};
        record.objectId = capturedScene.objects[index].objectId;
        record.materialId = capturedScene.objects[index].materialId;
        record.alpha.constantAlpha = 1.0f;
        record.alpha.fade = 1.0f;
        record.modelDeterminant = 1.0f;
        record.faceMode = visibility::FaceMode::FrontOnly;
        // Object 0 is the large visible one, so it carries the cutout.
        // Object 1 is deliberately occluded by the Phase 11 fixture and has
        // no coverage either way. Object 2 is the visible two-sided control.
        if (index == 0) {
            record.alpha.classification = visibility::AlphaClass::Tested;
            record.alpha.source = visibility::AlphaSource::BaseColorTexture;
            record.alpha.reference = 0.5f;
        } else {
            record.alpha.classification = visibility::AlphaClass::Opaque;
            record.alpha.source = visibility::AlphaSource::None;
        }
        if (index == 2) record.faceMode = visibility::FaceMode::TwoSided;
        capturedScene.visibility.push_back(record);
    }
    REQUIRE(scene::ValidateScenePacket(capturedScene) ==
        scene::ScenePacketError::None);

    raster::DecodedPacket projected;
    REQUIRE(scene::ProjectScenePacket(
        rasterPacket, capturedView, capturedScene, projected) ==
        scene::ScenePacketError::None);

    // Without a texture every surface stays opaque, which is what keeps the
    // Phase 11 and 12 references byte-identical.
    scene::GBufferImage opaque;
    REQUIRE(scene::RenderReferenceGBuffer(projected, capturedScene, opaque) ==
        scene::ScenePacketError::None);

    scene::GBufferImage tested;
    REQUIRE(scene::RenderReferenceGBuffer(
        projected, capturedScene, &cutout, tested) ==
        scene::ScenePacketError::None);

    const auto covered = [](const scene::GBufferImage& image,
                            const std::uint64_t objectId) {
        return std::count_if(image.pixels.begin(), image.pixels.end(),
            [objectId](const scene::GBufferPixelV1& pixel) {
                return (static_cast<std::uint64_t>(pixel.objectId[0]) |
                    (static_cast<std::uint64_t>(pixel.objectId[1]) << 32)) ==
                    objectId;
            });
    };
    const auto testedId = capturedScene.objects[0].objectId;
    const auto controlId = capturedScene.objects[2].objectId;

    // The alpha-tested object loses coverage where the texture is clear, but
    // is not erased entirely: a cutout has to leave a silhouette.
    CHECK(covered(tested, testedId) < covered(opaque, testedId));
    CHECK(covered(tested, testedId) > 0);
    // The opaque control sits behind the cutout, so it can only be revealed
    // by it, never hidden.
    CHECK(covered(tested, controlId) >= covered(opaque, controlId));

    // Every surviving shading normal stays in its geometric hemisphere,
    // including on a two-sided object's back faces.
    for (const auto& pixel : tested.pixels) {
        const auto objectId = ReadId(pixel.objectId);
        if (objectId == 0) continue;
        const auto dot =
            pixel.geometricNormalRoughness[0] * pixel.shadingNormalDepth[0] +
            pixel.geometricNormalRoughness[1] * pixel.shadingNormalDepth[1] +
            pixel.geometricNormalRoughness[2] * pixel.shadingNormalDepth[2];
        CHECK(dot > 0.0f);
    }
}

TEST_CASE("P15_stored_opacity_comes_from_coverage_not_from_sampled_alpha",
    "[phase15][scene]")
{
    // The G-buffer's opacity channel is the coverage decision's own opacity.
    // Sampling it straight from the base texture instead lets an alpha
    // channel that holds a mask or a height silently make a solid surface
    // transparent, and lets a cutout survivor claim it is semi-transparent
    // when the test that admitted it was binary.
    const auto rasterPacket = BuildRaster();
    const auto capturedView = BuildView();

    // Uniformly clear alpha with opaque white colour. Any surface that reads
    // opacity from this texture reads zero.
    texture::CapturedTexture clearAlpha{};
    clearAlpha.resourceId = 0x8000'0000'0000'15A2ull;
    clearAlpha.generation = 1;
    clearAlpha.width = 1;
    clearAlpha.height = 1;
    clearAlpha.resourceFormat = texture::TextureFormat::R8G8B8A8Unorm;
    clearAlpha.viewFormat = texture::TextureFormat::R8G8B8A8Unorm;
    clearAlpha.sampler.minFilter = texture::TextureFilter::Nearest;
    clearAlpha.sampler.magFilter = texture::TextureFilter::Nearest;
    clearAlpha.sampler.mipFilter = texture::TextureFilter::Nearest;
    clearAlpha.sampler.maxLod = 0.0f;
    texture::TextureSubresource level{};
    level.width = 1;
    level.height = 1;
    level.rowPitch = 4;
    level.slicePitch = 4;
    level.bytes = {std::byte{255}, std::byte{255}, std::byte{255},
        std::byte{0}};
    clearAlpha.subresources.push_back(std::move(level));

    const auto classify = [&](const visibility::AlphaClass classification,
                              const float reference,
                              scene::GBufferImage& image) {
        auto capturedScene = BuildScene();
        for (std::size_t index = 0; index < capturedScene.objects.size();
             ++index) {
            visibility::VisibilityRecordV1 record{};
            record.objectId = capturedScene.objects[index].objectId;
            record.materialId = capturedScene.objects[index].materialId;
            record.alpha.classification = classification;
            record.alpha.source = classification == visibility::AlphaClass::
                    Opaque
                ? visibility::AlphaSource::None
                : visibility::AlphaSource::BaseColorTexture;
            record.alpha.reference = reference;
            record.alpha.constantAlpha = 1.0f;
            record.alpha.fade = 1.0f;
            record.modelDeterminant = 1.0f;
            capturedScene.visibility.push_back(record);
        }
        REQUIRE(scene::ValidateScenePacket(capturedScene) ==
            scene::ScenePacketError::None);
        raster::DecodedPacket projected;
        REQUIRE(scene::ProjectScenePacket(
            rasterPacket, capturedView, capturedScene, projected) ==
            scene::ScenePacketError::None);
        REQUIRE(scene::RenderReferenceGBuffer(
            projected, capturedScene, &clearAlpha, image) ==
            scene::ScenePacketError::None);
        return capturedScene.objects[0].objectId;
    };

    // An opaque surface never consults alpha, even a zero one, so it keeps
    // both its coverage and a stored opacity of one.
    scene::GBufferImage opaque;
    const auto opaqueId =
        classify(visibility::AlphaClass::Opaque, 0.0f, opaque);
    std::uint64_t opaquePixels = 0;
    for (const auto& pixel : opaque.pixels) {
        if (ReadId(pixel.objectId) != opaqueId) continue;
        ++opaquePixels;
        CHECK(pixel.albedo[3] == Catch::Approx(1.0f));
    }
    CHECK(opaquePixels > 0);

    // A cutout admits a fragment or discards it; nothing survives partially,
    // so a survivor is fully opaque.
    scene::GBufferImage tested;
    const auto testedId =
        classify(visibility::AlphaClass::Tested, 0.0f, tested);
    std::uint64_t testedPixels = 0;
    for (const auto& pixel : tested.pixels) {
        if (ReadId(pixel.objectId) != testedId) continue;
        ++testedPixels;
        CHECK(pixel.albedo[3] == Catch::Approx(1.0f));
    }
    CHECK(testedPixels > 0);

    // Only a blended surface stores a partial opacity, and it stores the
    // effective alpha rather than the raw sample.
    scene::GBufferImage blended;
    const auto blendedId =
        classify(visibility::AlphaClass::Blended, 0.0f, blended);
    std::uint64_t blendedPixels = 0;
    for (const auto& pixel : blended.pixels) {
        if (ReadId(pixel.objectId) != blendedId) continue;
        ++blendedPixels;
        CHECK(pixel.albedo[3] == Catch::Approx(0.0f));
    }
    CHECK(blendedPixels > 0);

    // The colour channels do take the texture, which is what the opacity
    // channel deliberately does not do.
    for (const auto& pixel : opaque.pixels) {
        if (ReadId(pixel.objectId) != opaqueId) continue;
        CHECK(pixel.albedo[0] > 0.0f);
        break;
    }
}

TEST_CASE("P15_scene_visibility_section_appends_without_prefix_drift",
    "[phase15][scene]")
{
    // Every earlier scene artifact must stay byte-identical, so a scene with
    // no visibility records still encodes at its old minimum version.
    const auto legacy = BuildScene();
    std::vector<std::byte> legacyBytes;
    REQUIRE(scene::EncodeScenePacket(legacy, legacyBytes) ==
        scene::ScenePacketError::None);
    CHECK(legacyBytes.size() == sizeof(scene::ScenePacketHeaderV1) +
        legacy.objects.size() * sizeof(scene::OpaqueObjectV1));
    scene::ScenePacket decodedLegacy;
    REQUIRE(scene::DecodeScenePacket(legacyBytes, decodedLegacy) ==
        scene::ScenePacketError::None);
    CHECK(decodedLegacy.header.versionMinor == 0);
    CHECK(decodedLegacy.visibility.empty());
    CHECK(decodedLegacy.header.visibilityCount == 0);
    CHECK(decodedLegacy.header.visibilityOffset == 0);

    auto source = BuildScene();
    for (std::size_t index = 0; index < source.objects.size(); ++index) {
        visibility::VisibilityRecordV1 record{};
        record.objectId = source.objects[index].objectId;
        record.materialId = source.objects[index].materialId;
        record.alpha.classification = index == 1
            ? visibility::AlphaClass::Tested : visibility::AlphaClass::Opaque;
        record.alpha.source = index == 1
            ? visibility::AlphaSource::BaseColorTexture
            : visibility::AlphaSource::None;
        record.alpha.reference = index == 1 ? 128.0f / 255.0f : 0.0f;
        record.alpha.constantAlpha = 1.0f;
        record.alpha.fade = 1.0f;
        record.faceMode = index == 2
            ? visibility::FaceMode::TwoSided
            : visibility::FaceMode::FrontOnly;
        record.modelDeterminant = index == 2 ? -1.0f : 1.0f;
        source.visibility.push_back(record);
    }
    REQUIRE(scene::ValidateScenePacket(source) ==
        scene::ScenePacketError::None);

    std::vector<std::byte> bytes;
    REQUIRE(scene::EncodeScenePacket(source, bytes) ==
        scene::ScenePacketError::None);
    CHECK(bytes.size() == sizeof(scene::ScenePacketHeaderV1) +
        source.objects.size() * sizeof(scene::OpaqueObjectV1) +
        source.visibility.size() * sizeof(visibility::VisibilityRecordV1));
    scene::ScenePacket decoded;
    REQUIRE(scene::DecodeScenePacket(bytes, decoded) ==
        scene::ScenePacketError::None);
    CHECK(decoded.header.versionMinor == 2);
    REQUIRE(decoded.visibility.size() == source.objects.size());
    CHECK(decoded.visibility[1].alpha.classification ==
        visibility::AlphaClass::Tested);
    CHECK(decoded.visibility[1].alpha.reference ==
        Catch::Approx(128.0f / 255.0f));
    CHECK(decoded.visibility[2].faceMode == visibility::FaceMode::TwoSided);
    CHECK(decoded.visibility[2].modelDeterminant == Catch::Approx(-1.0f));

    std::vector<std::byte> reEncoded;
    REQUIRE(scene::EncodeScenePacket(decoded, reEncoded) ==
        scene::ScenePacketError::None);
    CHECK(reEncoded == bytes);

    // The header still occupies the same 96 bytes it always has, so the
    // Phase 11 and 12 prefixes cannot have moved.
    CHECK(sizeof(scene::ScenePacketHeaderV1) == 96);
    CHECK(offsetof(scene::ScenePacketHeaderV1, visibilityCount) == 80);
    CHECK(offsetof(scene::ScenePacketHeaderV1, visibilityOffset) == 84);

    // A partial visibility table cannot be resolved per object, so it is
    // refused rather than defaulted.
    auto partial = source;
    partial.visibility.pop_back();
    CHECK(scene::ValidateScenePacket(partial) ==
        scene::ScenePacketError::UncoveredObject);

    // Every record must name the object it belongs to, in object order.
    auto misordered = source;
    std::swap(misordered.visibility[0], misordered.visibility[1]);
    CHECK(scene::ValidateScenePacket(misordered) ==
        scene::ScenePacketError::InvalidVisibility);

    auto unclassified = source;
    unclassified.visibility[0].alpha.classification =
        visibility::AlphaClass::Unclassified;
    CHECK(scene::ValidateScenePacket(unclassified) ==
        scene::ScenePacketError::UnclassifiedWorldWriter);

    auto singular = source;
    singular.visibility[0].modelDeterminant = 0.0f;
    CHECK(scene::ValidateScenePacket(singular) ==
        scene::ScenePacketError::InvalidVisibility);
}

TEST_CASE("P12_instanced_projection_and_gbuffer_expand_every_instance",
    "[phase12][scene]")
{
    const auto rasterPacket = BuildRaster();
    const auto capturedView = BuildView();
    const auto capturedScene = BuildInstancedScene();
    raster::DecodedPacket projected;
    REQUIRE(scene::ProjectScenePacket(
        rasterPacket, capturedView, capturedScene, projected) ==
        scene::ScenePacketError::None);
    CHECK(projected.draws.size() == capturedScene.instances.size());
    CHECK(projected.vertices.size() == 3 * capturedScene.instances.size());

    scene::GBufferImage image;
    REQUIRE(scene::RenderReferenceGBuffer(
        projected, capturedScene, image) ==
        scene::ScenePacketError::None);

    std::uint64_t counts[4]{};
    for (const auto& pixel : image.pixels) {
        const auto objectId = ReadId(pixel.objectId);
        for (std::size_t index = 0; index < 4; ++index) {
            if (objectId == capturedScene.instances[index].objectId) {
                ++counts[index];
                CHECK(ReadId(pixel.materialId) ==
                    capturedScene.objects[
                        capturedScene.instances[index].objectIndex]
                        .materialId);
            }
        }
    }
    // The repeated instance is visible in its own right; the enclosed
    // instance of object 1 stays fully occluded.
    CHECK(counts[0] > 50);
    CHECK(counts[1] > 20);
    CHECK(counts[2] == 0);
    CHECK(counts[3] > 20);

    // Per-instance material data modulates only its own instance.
    bool sawTintedPixel = false;
    for (std::uint32_t y = 0; y < image.height && !sawTintedPixel; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto& pixel = image.At(x, y);
            if (ReadId(pixel.objectId) !=
                capturedScene.instances[1].objectId) {
                continue;
            }
            CHECK(pixel.albedo[0] <= 0.5f);
            sawTintedPixel = true;
            break;
        }
    }
    CHECK(sawTintedPixel);
}

TEST_CASE("P11_the_gbuffer_normal_follows_the_vertices_not_the_object",
    "[phase11][scene]")
{
    // The point of a per-vertex normal is that it varies across a triangle.
    // A fixture whose vertex normals all equal the object's own normal cannot
    // tell the two apart, so a test built on one passes whichever the
    // renderer reads -- which is exactly how a change like this ships with no
    // coverage at all.
    auto source = BuildRaster();
    const auto view = BuildView();
    const auto packet = BuildScene();

    // A normal that leans a different way at each corner, and unmistakably
    // away from the object's declared -Z.
    const std::array<std::array<float, 3>, 3> leaning{{
        {0.0f, 0.0f, -1.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f}}};
    for (std::size_t vertex = 0; vertex < 3; ++vertex) {
        std::copy(leaning[vertex].begin(), leaning[vertex].end(),
            source.vertices[vertex].normal);
    }

    raster::DecodedPacket projected{};
    REQUIRE(scene::ProjectScenePacket(source, view, packet, projected) ==
        scene::ScenePacketError::None);
    scene::GBufferImage image{};
    REQUIRE(scene::RenderReferenceGBuffer(projected, packet, image) ==
        scene::ScenePacketError::None);

    // Collect the distinct normals the first object covered.
    std::vector<std::array<float, 3>> observed;
    for (const auto& pixel : image.pixels) {
        if (ReadId(pixel.objectId) == 0) continue;
        const std::array<float, 3> normal{
            pixel.geometricNormalRoughness[0],
            pixel.geometricNormalRoughness[1],
            pixel.geometricNormalRoughness[2]};
        const auto seen = std::any_of(observed.begin(), observed.end(),
            [&normal](const std::array<float, 3>& other) {
                return std::abs(other[0] - normal[0]) < 1.0e-3f &&
                    std::abs(other[1] - normal[1]) < 1.0e-3f &&
                    std::abs(other[2] - normal[2]) < 1.0e-3f;
            });
        if (!seen) observed.push_back(normal);
    }

    // More than one, because the normal is interpolated rather than constant
    // per object. With the object's own axis there would be exactly one per
    // object, and the whole surface would shade as a single flat plane.
    CHECK(observed.size() > 4);

    // And at least one covered pixel must point somewhere the object's own
    // declared normal never does. A count alone would still pass if every
    // interpolated value happened to land on that axis, which is precisely
    // the case a fixture built from the object normal cannot distinguish.
    const auto& declared = packet.objects.front().geometricNormal;
    const auto leansAway = std::any_of(observed.begin(), observed.end(),
        [&declared](const std::array<float, 3>& normal) {
            return std::abs(normal[0] - declared[0]) > 0.1f ||
                std::abs(normal[1] - declared[1]) > 0.1f ||
                std::abs(normal[2] - declared[2]) > 0.1f;
        });
    CHECK(leansAway);

    // Every stored normal is still unit length: an interpolated normal that
    // is not renormalised shortens toward the middle of a triangle, which
    // darkens it in a way that reads as a lighting fault.
    for (const auto& normal : observed) {
        const auto length = std::sqrt(normal[0] * normal[0] +
            normal[1] * normal[1] + normal[2] * normal[2]);
        CHECK(length == Catch::Approx(1.0f).margin(1.0e-3));
    }
}

TEST_CASE("P21_the_scene_carries_its_transparent_draws_and_older_ones_do_not",
    "[phase21][scene]")
{
    // Transparency is a property of the frame, not of the material, because
    // the same material can be drawn blended in one pass and opaque in
    // another. The scene has to carry the blend state the engine sorted by,
    // or the composite invents an order and the layers land wrong.
    auto packet = BuildScene();
    REQUIRE_FALSE(packet.objects.empty());

    scene::TransparentDrawRecordV1 record{};
    record.drawId = 0x2101;
    record.materialId = packet.objects.front().materialId;
    record.objectIndex = 0;
    record.blend = static_cast<std::uint32_t>(blend::BlendMode::Additive);
    record.sortDepth = 12.5f;
    record.softFade = 4.0f;
    record.dissolve = 0.25f;
    record.dissolveFalloff = 0.1f;
    packet.transparent.push_back(record);

    std::vector<std::byte> bytes;
    REQUIRE(scene::EncodeScenePacket(packet, bytes) ==
        scene::ScenePacketError::None);

    scene::ScenePacket decoded{};
    REQUIRE(scene::DecodeScenePacket(bytes, decoded) ==
        scene::ScenePacketError::None);
    // A section that did not exist before is a new minor version, so a reader
    // that predates it refuses the packet rather than reading the bytes at
    // whatever they used to mean. At least that version rather than exactly
    // it: the section's records have grown since, and pinning the number here
    // would fail on every later addition while catching none of the drift the
    // refusal below is actually about.
    CHECK(decoded.header.versionMinor >=
        scene::kScenePacketTransparencyVersionMinor);
    REQUIRE(decoded.transparent.size() == 1);
    CHECK(decoded.transparent.front().drawId == 0x2101);
    CHECK(decoded.transparent.front().blend ==
        static_cast<std::uint32_t>(blend::BlendMode::Additive));
    CHECK(decoded.transparent.front().sortDepth == Catch::Approx(12.5f));
    CHECK(decoded.transparent.front().softFade == Catch::Approx(4.0f));

    // A scene with no transparent draws still encodes at the older version,
    // so every capture taken before this section existed round-trips byte for
    // byte rather than acquiring an empty section it never had.
    auto opaqueOnly = BuildScene();
    std::vector<std::byte> opaqueBytes;
    REQUIRE(scene::EncodeScenePacket(opaqueOnly, opaqueBytes) ==
        scene::ScenePacketError::None);
    scene::ScenePacket opaqueDecoded{};
    REQUIRE(scene::DecodeScenePacket(opaqueBytes, opaqueDecoded) ==
        scene::ScenePacketError::None);
    CHECK(opaqueDecoded.header.versionMinor <
        scene::kScenePacketTransparencyVersionMinor);
    CHECK(opaqueDecoded.transparent.empty());

    // A draw naming an object the scene does not have would composite over
    // nothing, so it is refused at encode rather than at draw time.
    auto dangling = packet;
    dangling.transparent.front().objectIndex =
        static_cast<std::uint32_t>(dangling.objects.size());
    std::vector<std::byte> danglingBytes;
    CHECK(scene::EncodeScenePacket(dangling, danglingBytes) !=
        scene::ScenePacketError::None);
}

TEST_CASE("instances are narrowed against the camera origin in double")
{
    // Fallout 4 places objects in absolute world coordinates while its view
    // matrix carries no translation. Measured in Sanctuary, that put every
    // instance about a hundred and twenty thousand units from the rendered
    // origin with nothing below eye level. Narrowing against the recovered
    // camera position is what reconciles the two, and it is the same rule
    // ProjectWorldPoint already applies to world points.
    scene::InstanceV1 instance{};
    // A rotation, so the test can tell a translation change from a wholesale
    // overwrite of the matrix.
    instance.model[0] = 0.6f;
    instance.model[1] = -0.8f;
    instance.model[4] = 0.8f;
    instance.model[5] = 0.6f;
    instance.model[10] = 1.0f;
    instance.model[15] = 1.0f;
    instance.model[3] = -78'400.0f;
    instance.model[7] = 86'100.0f;
    instance.model[11] = 7'840.0f;
    instance.previousModel[3] = -78'395.0f;
    instance.previousModel[7] = 86'098.0f;
    instance.previousModel[11] = 7'839.0f;
    instance.previousModel[15] = 1.0f;

    SECTION("a zero origin leaves the instance exactly as it was")
    {
        const std::array<double, 3> origin{0.0, 0.0, 0.0};
        const auto narrowed = scene::NarrowInstance(instance, origin);
        for (std::size_t index = 0; index < 16; ++index) {
            INFO(index);
            CHECK(narrowed.model[index] == instance.model[index]);
            CHECK(narrowed.previousModel[index] ==
                instance.previousModel[index]);
        }
    }

    SECTION("only the translation column moves")
    {
        const std::array<double, 3> origin{-78'000.0, 86'000.0, 7'800.0};
        const auto narrowed = scene::NarrowInstance(instance, origin);
        CHECK(narrowed.model[3] == Catch::Approx(-400.0).margin(0.05));
        CHECK(narrowed.model[7] == Catch::Approx(100.0).margin(0.05));
        CHECK(narrowed.model[11] == Catch::Approx(40.0).margin(0.05));
        // The rotation is not the camera's business. A narrowing that touched
        // it would turn every object in the cell as the player walked.
        CHECK(narrowed.model[0] == instance.model[0]);
        CHECK(narrowed.model[1] == instance.model[1]);
        CHECK(narrowed.model[4] == instance.model[4]);
        CHECK(narrowed.model[5] == instance.model[5]);
        CHECK(narrowed.model[10] == instance.model[10]);
        CHECK(narrowed.model[15] == instance.model[15]);
    }

    SECTION("the previous transform is narrowed by the same origin")
    {
        // Motion vectors are the difference between the two. Narrowing one and
        // not the other would make every static object appear to move by the
        // camera position, which is a hundred thousand units of false motion
        // and would smear the whole frame rather than look like an error.
        const std::array<double, 3> origin{-78'000.0, 86'000.0, 7'800.0};
        const auto narrowed = scene::NarrowInstance(instance, origin);
        CHECK(narrowed.previousModel[3] == Catch::Approx(-395.0).margin(0.05));
        CHECK(narrowed.previousModel[7] == Catch::Approx(98.0).margin(0.05));
        CHECK(narrowed.previousModel[11] == Catch::Approx(39.0).margin(0.05));
    }

    SECTION("identity is preserved so the instance still names its object")
    {
        instance.objectId = 0x1234'5678'9ABC'DEF0ull;
        instance.objectIndex = 7;
        instance.flags = scene::InstanceStatic;
        instance.parameters[2] = 0.25f;
        const std::array<double, 3> origin{1.0, 2.0, 3.0};
        const auto narrowed = scene::NarrowInstance(instance, origin);
        CHECK(narrowed.objectId == instance.objectId);
        CHECK(narrowed.objectIndex == instance.objectIndex);
        CHECK(narrowed.flags == instance.flags);
        CHECK(narrowed.parameters[2] == instance.parameters[2]);
    }

    SECTION("the origin's precision survives the subtraction")
    {
        // What the double arithmetic actually buys, stated precisely rather
        // than generously. Two floats near a hundred and twenty thousand
        // subtract exactly even in single precision -- the result is small and
        // the difference of two nearby floats is representable -- so the
        // saving is not in the object positions. It is in the origin: the
        // camera position is recovered as a double, and at that magnitude a
        // float can only hold it to about eight thousandths. Rounding it to
        // float before subtracting discards the rest, and the whole cell
        // shifts by the roundoff each time the camera moves far enough to
        // change which float the origin lands on.
        //
        // A hundred and twenty thousand and a thousandth is a position a float
        // cannot represent, so the two arithmetics give visibly different
        // answers here and only the wider one gives the right one.
        const std::array<double, 3> origin{120'000.001, 0.0, 0.0};
        scene::InstanceV1 instanceAt{};
        instanceAt.model[15] = 1.0f;
        instanceAt.model[3] = 120'000.25f;

        const auto narrowed = scene::NarrowInstance(instanceAt, origin);
        CHECK(narrowed.model[3] == Catch::Approx(0.249).margin(1.0e-4));
        // The answer single precision would have produced, which differs by
        // the part of the origin it could not hold.
        CHECK(narrowed.model[3] != 0.25f);
    }

    SECTION("a non-finite origin leaves the instance untouched")
    {
        const std::array<double, 3> origin{
            std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0};
        const auto narrowed = scene::NarrowInstance(instance, origin);
        CHECK(narrowed.model[3] == instance.model[3]);
        CHECK(narrowed.model[7] == instance.model[7]);
        CHECK(narrowed.model[11] == instance.model[11]);
    }
}

TEST_CASE("the reactive mask is compared like every other plane")
{
    // A plane the comparison does not read is a plane nothing verifies. The
    // reactive mask was added as a fifth attachment and the whole contract
    // suite still passed while the device wrote one and the oracle wrote
    // zero -- the difference simply was not looked at.
    std::vector<scene::GBufferPixelV1> expected(4);
    for (auto& pixel : expected) {
        pixel.albedo[0] = 0.5f;
        pixel.reactive = 1.0f;
    }
    auto actual = expected;

    SECTION("identical pixels compare equal")
    {
        const auto comparison = scene::CompareGBuffer(expected, actual);
        CHECK(comparison.differingPixels == 0);
        CHECK(comparison.maximumAbsoluteError == 0.0f);
    }

    SECTION("a pixel whose reactive mask differs is counted")
    {
        // The value a transparent draw would produce where the oracle
        // expected an opaque one. Nothing else about the pixel moves, so only
        // a comparison that reads the plane can notice.
        actual[2].reactive = 0.25f;
        const auto comparison = scene::CompareGBuffer(expected, actual);
        CHECK(comparison.differingPixels == 1);
        CHECK(comparison.maximumAbsoluteError == Catch::Approx(0.75f));
        // Not an identity mismatch: the pixel still names the same object.
        CHECK(comparison.identityMismatches == 0);
    }
}

TEST_CASE("a decal carries the volume it projects into")
{
    // A decal is a transparent draw that projects onto whatever surface it
    // reaches, and the volume it reaches is not recoverable from anything else
    // in the packet: without an origin, an axis, a range and a radius there is
    // nothing for a shader to project, and a decal drawn as an ordinary
    // blended quad hangs in the air instead of lying on the wall.
    scene::ScenePacket packet = BuildScene();
    scene::TransparentDrawRecordV1 decal{};
    decal.drawId = 0x9001;
    decal.materialId = packet.objects[0].materialId;
    decal.objectIndex = 0;
    decal.blend = static_cast<std::uint32_t>(blend::BlendMode::Multiply);
    decal.domain = static_cast<std::uint32_t>(blend::EffectDomain::Decal);
    decal.sortDepth = 2.0f;
    const std::array<float, 3> origin{1.5f, -2.5f, 3.5f};
    const std::array<float, 3> direction{0.0f, 0.0f, -1.0f};
    std::copy(origin.begin(), origin.end(), decal.decalOrigin);
    std::copy(direction.begin(), direction.end(), decal.decalAxis);
    decal.decalRange = 48.0f;
    decal.decalRadius = 12.0f;
    // The engine marks its receivers. Without this a decal lands on the sky
    // and on characters walking past it.
    decal.stencilReceiverMask = 0x7;
    decal.stencilReference = 0x5;
    packet.transparent.push_back(decal);
    packet.header.transparentCount = 1;

    SECTION("the projection survives an encode and a decode")
    {
        std::vector<std::byte> bytes;
        REQUIRE(scene::EncodeScenePacket(packet, bytes) ==
            scene::ScenePacketError::None);
        scene::ScenePacket decoded{};
        REQUIRE(scene::DecodeScenePacket(bytes, decoded) ==
            scene::ScenePacketError::None);
        REQUIRE(decoded.transparent.size() == 1);
        const auto& read = decoded.transparent.front();
        for (std::size_t axis = 0; axis < 3; ++axis) {
            INFO(axis);
            CHECK(read.decalOrigin[axis] == decal.decalOrigin[axis]);
            CHECK(read.decalAxis[axis] == decal.decalAxis[axis]);
            CHECK(read.decalOrigin[axis] == origin[axis]);
        }
        CHECK(read.decalRange == decal.decalRange);
        CHECK(read.decalRadius == decal.decalRadius);
        CHECK(read.stencilReceiverMask == decal.stencilReceiverMask);
        CHECK(read.stencilReference == decal.stencilReference);
        // Everything the record already carried is still where it was.
        CHECK(read.drawId == decal.drawId);
        CHECK(read.blend == decal.blend);
        CHECK(read.domain == decal.domain);
        CHECK(read.sortDepth == decal.sortDepth);
    }

    SECTION("a packet written before the projection existed is refused")
    {
        // The record grew, so the same bytes mean different things at the two
        // versions. A reader that took a shorter record at the longer stride
        // would not fail: it would read each draw at the wrong offset and
        // report a decal volume assembled from its neighbour's fields.
        std::vector<std::byte> bytes;
        REQUIRE(scene::EncodeScenePacket(packet, bytes) ==
            scene::ScenePacketError::None);
        auto header = *reinterpret_cast<scene::ScenePacketHeaderV1*>(
            bytes.data());
        CHECK(header.versionMinor >= scene::kScenePacketDecalVersionMinor);
        header.versionMinor = scene::kScenePacketTransparencyVersionMinor;
        std::memcpy(bytes.data(), &header, sizeof(header));
        scene::ScenePacket decoded{};
        CHECK(scene::DecodeScenePacket(bytes, decoded) !=
            scene::ScenePacketError::None);
    }
}
