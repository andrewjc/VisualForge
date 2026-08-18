#include "renderer_core/EngineView.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using namespace vf::renderer::view;

SourceMatrix4 SourceFromCanonical(
    const Matrix4& canonical,
    const MatrixStorage storage = MatrixStorage::RowMajor,
    const VectorConvention vectors = VectorConvention::ColumnVector)
{
    SourceMatrix4 source{};
    source.storage = storage;
    source.vectors = vectors;
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            const auto conceptual = vectors == VectorConvention::ColumnVector
                ? canonical.elements[row * 4 + column]
                : canonical.elements[column * 4 + row];
            const auto index = storage == MatrixStorage::RowMajor
                ? row * 4 + column : column * 4 + row;
            source.elements[index] = conceptual;
        }
    }
    return source;
}

CapturedView Fixture(
    const Handedness handedness = Handedness::LeftHanded,
    const ProjectionMode mode = ProjectionMode::Perspective)
{
    CapturedView captured{};
    captured.viewId = 0x1001;
    captured.cameraId = 0x2001;
    captured.projectionMode = mode;
    captured.handedness = handedness;
    captured.outputWidth = 800;
    captured.outputHeight = 600;
    captured.renderScale = 1.0f;
    captured.nearPlane = 1.0f;
    captured.farPlane = 101.0f;
    captured.verticalFovRadians = mode == ProjectionMode::Perspective
        ? 1.57079632679f : 0.0f;
    captured.viewport = {0.0f, 0.0f, 800.0f, 600.0f, 0.0f, 1.0f};
    captured.scissor = {0, 0, 800, 600};
    const auto identity = IdentityMatrix();
    const auto projection = mode == ProjectionMode::Perspective
        ? BuildPerspectiveProjection(captured.verticalFovRadians,
            4.0f / 3.0f, captured.nearPlane, captured.farPlane,
            handedness)
        : BuildOrthographicProjection(8.0f, 6.0f,
            captured.nearPlane, captured.farPlane, handedness);
    captured.view = SourceFromCanonical(identity);
    captured.projection = SourceFromCanonical(projection);
    captured.previousView = SourceFromCanonical(identity);
    captured.previousProjection = SourceFromCanonical(projection);
    return captured;
}

ViewRecordV1 Translate(const CapturedView& captured)
{
    ViewRecordV1 view{};
    const auto result = TranslateCapturedView(captured, view);
    INFO(ToString(result));
    REQUIRE(result == ViewError::None);
    return view;
}

void CheckMatrixApprox(const Matrix4& actual, const Matrix4& expected)
{
    for (std::size_t index = 0; index < 16; ++index) {
        CHECK(actual.elements[index] == Catch::Approx(
            expected.elements[index]).margin(1.0e-6f));
    }
}

}

TEST_CASE("P10_matrix_storage_and_vector_order_normalize_once", "[phase10][view]")
{
    Matrix4 canonical{};
    for (std::size_t index = 0; index < 16; ++index) {
        canonical.elements[index] = static_cast<float>(index + 1);
    }
    for (const auto storage :
         {MatrixStorage::RowMajor, MatrixStorage::ColumnMajor}) {
        for (const auto vectors :
             {VectorConvention::ColumnVector,
              VectorConvention::RowVector}) {
            CheckMatrixApprox(NormalizeSourceMatrix(
                SourceFromCanonical(canonical, storage, vectors)), canonical);
        }
    }
}

TEST_CASE("P10_projection_modes_and_handedness_extract_exact_clip_planes", "[phase10][view]")
{
    for (const auto handedness :
         {Handedness::LeftHanded, Handedness::RightHanded}) {
        for (const auto mode :
             {ProjectionMode::Perspective, ProjectionMode::Orthographic}) {
            const auto captured = Fixture(handedness, mode);
            const auto projection = NormalizeSourceMatrix(captured.projection);
            ClipPlanes planes{};
            REQUIRE(ExtractClipPlanes(projection, mode, handedness, planes) ==
                ViewError::None);
            CHECK(planes.nearPlane == Catch::Approx(1.0f).margin(1.0e-5f));
            CHECK(planes.farPlane == Catch::Approx(101.0f).margin(1.0e-3f));
            const auto view = Translate(captured);
            CHECK(ValidateView(view) == ViewError::None);
        }
    }
}

TEST_CASE("P10_world_to_screen_applies_one_explicit_D3D_to_Vulkan_Y_conversion", "[phase10][view]")
{
    for (const auto handedness :
         {Handedness::LeftHanded, Handedness::RightHanded}) {
        const auto view = Translate(Fixture(handedness));
        ProjectedPoint projected{};
        const auto z = handedness == Handedness::LeftHanded ? 10.0 : -10.0;
        REQUIRE(ProjectWorldPoint(view, {2.0, 1.0, z}, projected) ==
            ViewError::None);
        CHECK(projected.x == Catch::Approx(460.0f).margin(1.0e-3f));
        CHECK(projected.y == Catch::Approx(270.0f).margin(1.0e-3f));
        CHECK(projected.depth == Catch::Approx(0.909f).margin(1.0e-4f));
        CHECK(projected.inside);
    }
}

TEST_CASE("P10_camera_relative_origin_and_jitter_remain_explicit", "[phase10][view]")
{
    auto captured = Fixture();
    captured.flags = ViewCameraRelative | ViewUsesJitter;
    captured.cameraRelativeOrigin = {1'000'000.0, -2'000'000.0, 300.0};
    captured.previousCameraRelativeOrigin = captured.cameraRelativeOrigin;
    captured.jitterNdc = {0.01f, -0.02f};
    captured.previousJitterNdc = captured.jitterNdc;
    const auto projection = BuildPerspectiveProjection(
        captured.verticalFovRadians, 4.0f / 3.0f,
        captured.nearPlane, captured.farPlane,
        captured.handedness, captured.jitterNdc);
    captured.projection = SourceFromCanonical(projection);
    captured.previousProjection = SourceFromCanonical(projection);
    const auto view = Translate(captured);
    CHECK(view.jitterNdc[0] == Catch::Approx(0.01f));
    CHECK(view.jitterNdc[1] == Catch::Approx(0.02f));
    ProjectedPoint projected{};
    REQUIRE(ProjectWorldPoint(view,
        {1'000'000.0, -2'000'000.0, 310.0}, projected) == ViewError::None);
    CHECK(projected.x == Catch::Approx(404.0f).margin(1.0e-3f));
    CHECK(projected.y == Catch::Approx(306.0f).margin(1.0e-3f));
}

TEST_CASE("P10_invalid_nonfinite_singular_and_viewport_records_fail_closed", "[phase10][view]")
{
    auto captured = Fixture();
    captured.nearPlane = std::numeric_limits<float>::quiet_NaN();
    ViewRecordV1 view{};
    CHECK(TranslateCapturedView(captured, view) == ViewError::NonFinite);

    captured = Fixture();
    captured.view = {};
    CHECK(TranslateCapturedView(captured, view) == ViewError::SingularMatrix);

    captured = Fixture();
    captured.viewport.width = 801.0f;
    CHECK(TranslateCapturedView(captured, view) == ViewError::InvalidViewport);

    captured = Fixture();
    captured.scissor.x = -1;
    CHECK(TranslateCapturedView(captured, view) == ViewError::InvalidScissor);
}

TEST_CASE("P10_history_epoch_resets_once_per_transition_and_rejects_stale_history", "[phase10][view]")
{
    const auto first = Translate(Fixture());
    ViewHistoryTracker history;
    auto update = history.Observe(10, first);
    REQUIRE(update.error == ViewError::None);
    CHECK(update.reset);
    CHECK(update.epoch == 1);
    CHECK(update.resetCauses == DiscontinuityFirstObservation);

    auto moving = Fixture();
    moving.view.elements[3] = -1.0f;
    const auto second = Translate(moving);
    update = history.Observe(11, second);
    REQUIRE(update.error == ViewError::None);
    CHECK_FALSE(update.reset);
    CHECK(update.epoch == 1);

    auto resized = moving;
    resized.outputWidth = 1024;
    resized.viewport.width = 1024.0f;
    resized.scissor.width = 1024;
    resized.verticalFovRadians = 1.2f;
    resized.projection = SourceFromCanonical(BuildPerspectiveProjection(
        resized.verticalFovRadians, 1024.0f / 600.0f,
        resized.nearPlane, resized.farPlane, resized.handedness));
    const auto third = Translate(resized);
    update = history.Observe(12, third,
        DiscontinuityExplicitCut | DiscontinuityTeleport);
    REQUIRE(update.error == ViewError::None);
    CHECK(update.reset);
    CHECK(update.epoch == 2);
    CHECK((update.resetCauses & DiscontinuityExtent) != 0);
    CHECK((update.resetCauses & DiscontinuityProjection) != 0);

    CHECK(history.Observe(12, third).error == ViewError::StaleFrame);

    auto stalePrevious = resized;
    stalePrevious.previousView = stalePrevious.view;
    stalePrevious.previousView.elements[3] = 99.0f;
    CHECK(history.Observe(13, Translate(stalePrevious)).error ==
        ViewError::PreviousTransformMismatch);
}

TEST_CASE("P10_pass_classification_keeps_unknown_world_writers_visible", "[phase10][view]")
{
    CHECK(ClassifyPass(ShaderDomain::Lighting, 0,
        PassWritesWorldTarget) == PassCategory::Opaque);
    CHECK(ClassifyPass(ShaderDomain::Lighting, 0,
        PassWritesWorldTarget | PassAlphaTest) == PassCategory::AlphaTest);
    CHECK(ClassifyPass(ShaderDomain::Lighting, 0x0F,
        PassWritesWorldTarget) == PassCategory::Shadow);
    CHECK(ClassifyPass(ShaderDomain::Water, 0,
        PassWritesWorldTarget) == PassCategory::Water);
    CHECK(ClassifyPass(ShaderDomain::ImageSpace, 0, 0) ==
        PassCategory::ImageSpace);

    const std::array passes{
        PassRecordV1{1, 1, ShaderDomain::Lighting,
            PassCategory::Opaque, 0, 0, 2, PassWritesWorldTarget},
        PassRecordV1{2, 1, ShaderDomain::Utility,
            PassCategory::Unknown, 0xDEAD, 0x24, 99,
            PassWritesWorldTarget},
    };
    const auto coverage = SummarizePassCoverage(passes);
    CHECK(coverage.classified == 1);
    CHECK(coverage.unknown == 1);
    CHECK(coverage.unknownWorldWriters == 1);
    CHECK_FALSE(coverage.TakeoverEligible());
}

TEST_CASE("P10_frame_packet_is_pointer_free_deterministic_and_thread_owned", "[phase10][view]")
{
    FramePacket source{};
    source.header.frameId = 44;
    source.header.engineFrameId = 91;
    source.header.historyEpoch = 3;
    source.header.captureSequence = 700;
    source.header.captureThreadId = 12;
    source.header.renderThreadId = 12;
    source.views.push_back(Translate(Fixture()));
    source.passes.push_back({1, source.views[0].viewId,
        ShaderDomain::Lighting, PassCategory::Opaque,
        0x1234, 0, 2, PassWritesWorldTarget});

    std::vector<std::byte> first;
    std::vector<std::byte> second;
    REQUIRE(EncodeFramePacket(source, first) == FramePacketError::None);
    REQUIRE(EncodeFramePacket(source, second) == FramePacketError::None);
    CHECK(first == second);
    REQUIRE(first.size() >= sizeof(FramePacketHeaderV1));
    FramePacketHeaderV1 header{};
    std::memcpy(&header, first.data(), sizeof(header));
    CHECK(header.viewsOffset % 16 == 0);
    CHECK(header.passesOffset % 8 == 0);

    FramePacket decoded{};
    REQUIRE(DecodeFramePacket(first, decoded) == FramePacketError::None);
    CHECK(decoded.header.frameId == source.header.frameId);
    CHECK(decoded.views.size() == 1);
    CHECK(decoded.passes.size() == 1);
    CHECK(decoded.views[0].cameraId == source.views[0].cameraId);

    auto wrongThread = source;
    wrongThread.header.captureThreadId = 13;
    CHECK(EncodeFramePacket(wrongThread, second) ==
        FramePacketError::WrongThread);

    first.back() ^= std::byte{0x10};
    CHECK(DecodeFramePacket(first, decoded) ==
        FramePacketError::ChecksumMismatch);
}

TEST_CASE("P10_view_orientation_sign_detects_a_winding_reversal",
    "[view][phase10]")
{
    using namespace vf::renderer::view;
    const auto identity = [] {
        Matrix4 m{};
        m.elements[0] = 1.0f;
        m.elements[5] = 1.0f;
        m.elements[10] = 1.0f;
        m.elements[15] = 1.0f;
        return m;
    };

    // Nothing is flipped, so a triangle keeps the winding it was authored
    // with. This is the case a packet fixture is in.
    CHECK(ViewOrientationSign(identity(), identity()) == 1.0f);

    // A projection that negates Y reverses every triangle. This is the case a
    // live capture is in, and getting it wrong culls the outer shell of every
    // single-sided model and shows its interior instead.
    auto flipY = identity();
    flipY.elements[5] = -1.0f;
    CHECK(ViewOrientationSign(identity(), flipY) == -1.0f);
    CHECK(ViewOrientationSign(flipY, identity()) == -1.0f);

    // Two reversals compose back to none: a mirrored view through a mirrored
    // projection is not flipped, and inverting twice would be as wrong as not
    // inverting once.
    CHECK(ViewOrientationSign(flipY, flipY) == 1.0f);

    // A scale is not a flip, however uneven.
    auto scaled = identity();
    scaled.elements[0] = 4.0f;
    scaled.elements[5] = 0.25f;
    scaled.elements[10] = 9.0f;
    CHECK(ViewOrientationSign(scaled, identity()) == 1.0f);

    // A collapsed matrix is degenerate, not reversed. Reporting a flip here
    // would invert every model in the frame on a matrix that renders nothing.
    Matrix4 zero{};
    CHECK(ViewOrientationSign(zero, identity()) == 1.0f);
}
