#include "renderer_core/CameraStateScan.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using namespace vf::renderer;

view::Matrix4 BuildView()
{
    // Orthonormal basis with a translation, the shape a world-to-camera
    // matrix always has.
    const auto angle = 0.7f;
    view::Matrix4 matrix{};
    matrix.elements[0] = std::cos(angle);
    matrix.elements[2] = std::sin(angle);
    matrix.elements[3] = 125.0f;
    matrix.elements[5] = 1.0f;
    matrix.elements[7] = -40.0f;
    matrix.elements[8] = -std::sin(angle);
    matrix.elements[10] = std::cos(angle);
    matrix.elements[11] = 512.0f;
    matrix.elements[15] = 1.0f;
    return matrix;
}

view::Matrix4 BuildProjection()
{
    return view::BuildPerspectiveProjection(
        1.0471975512f, 16.0f / 9.0f, 15.0f, 40'000.0f,
        view::Handedness::LeftHanded);
}

void WriteMatrix(
    std::vector<std::byte>& block,
    const std::size_t offset,
    const view::Matrix4& matrix,
    const bool columnMajor = false)
{
    std::array<float, 16> raw{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            const auto source = matrix.elements[row * 4 + column];
            if (columnMajor) {
                raw[column * 4 + row] = source;
            } else {
                raw[row * 4 + column] = source;
            }
        }
    }
    std::memcpy(block.data() + offset, raw.data(), sizeof(raw));
}

std::vector<std::byte> BuildBlock(
    const bool columnMajor,
    const std::size_t viewOffset = 0x60,
    const std::size_t projectionOffset = 0xA0,
    const std::size_t viewProjectionOffset = 0xE0)
{
    std::vector<std::byte> block(0x250, std::byte{0});
    // Leading scalars stand in for viewport, depth range, and the camera
    // basis vectors that precede the matrices.
    const std::array<float, 8> leading{
        0.0f, 0.0f, 1280.0f, 720.0f, 0.0f, 1.0f, 0.0f, 1.0f};
    std::memcpy(block.data(), leading.data(), sizeof(leading));

    const auto viewMatrix = BuildView();
    const auto projection = BuildProjection();
    const auto viewProjection = view::Multiply(projection, viewMatrix);
    WriteMatrix(block, viewOffset, viewMatrix, columnMajor);
    WriteMatrix(block, projectionOffset, projection, columnMajor);
    WriteMatrix(block, viewProjectionOffset, viewProjection, columnMajor);
    return block;
}

}

TEST_CASE("P10L_camera_scan_finds_the_consistent_view_projection_triple",
    "[live][camera]")
{
    const auto block = BuildBlock(false);
    const auto result = camera::ScanCameraState(block);
    REQUIRE(result.found);
    CHECK(result.viewOffset == 0x60);
    CHECK(result.projectionOffset == 0xA0);
    CHECK(result.viewProjectionOffset == 0xE0);
    CHECK(result.storage == view::MatrixStorage::RowMajor);
    CHECK(result.residual < 1.0e-3f);

    // The recovered matrices must reproduce the engine values, not merely
    // point at the right bytes.
    const auto expectedView = BuildView();
    for (std::size_t index = 0; index < 16; ++index) {
        CHECK(result.view.elements[index] ==
            Catch::Approx(expectedView.elements[index]).margin(1.0e-4));
    }
}

TEST_CASE("P10L_camera_scan_detects_column_major_storage", "[live][camera]")
{
    const auto block = BuildBlock(true);
    const auto result = camera::ScanCameraState(block);
    REQUIRE(result.found);
    CHECK(result.storage == view::MatrixStorage::ColumnMajor);
    CHECK(result.viewOffset == 0x60);
    const auto expectedProjection = BuildProjection();
    for (std::size_t index = 0; index < 16; ++index) {
        CHECK(result.projection.elements[index] ==
            Catch::Approx(expectedProjection.elements[index])
                .margin(1.0e-4));
    }
}

TEST_CASE("P10L_camera_scan_locates_matrices_at_any_aligned_offset",
    "[live][camera]")
{
    const auto block = BuildBlock(false, 0x10, 0x150, 0x200);
    const auto result = camera::ScanCameraState(block);
    REQUIRE(result.found);
    CHECK(result.viewOffset == 0x10);
    CHECK(result.projectionOffset == 0x150);
    CHECK(result.viewProjectionOffset == 0x200);
}

TEST_CASE("P10L_camera_scan_reaches_a_camera_late_in_a_dense_record",
    "[live][camera]")
{
    // Reproduces the live failure. The engine's real record is dense: almost
    // every 4-byte offset before the camera holds finite, non-zero data, so a
    // scan with a small candidate budget spends it long before reaching the
    // matrices and reports "no camera" for a record that plainly has one.
    // Measured live: camera at +0x1B0/+0x1F0/+0x230 in a 0x3C0 window.
    std::vector<std::byte> block(0x3C0, std::byte{0});
    std::vector<float> filler(0x1B0 / sizeof(float));
    for (std::size_t index = 0; index < filler.size(); ++index) {
        // Plausible engine scalars: finite, non-zero, and not a camera.
        filler[index] = 0.5f + static_cast<float>(index % 37) * 0.25f;
    }
    std::memcpy(block.data(), filler.data(), filler.size() * sizeof(float));
    const auto viewMatrix = BuildView();
    const auto projection = BuildProjection();
    const auto viewProjection = view::Multiply(projection, viewMatrix);
    WriteMatrix(block, 0x1B0, viewMatrix, true);
    WriteMatrix(block, 0x1F0, projection, true);
    WriteMatrix(block, 0x230, viewProjection, true);

    const auto result = camera::ScanCameraState(block);
    REQUIRE(result.found);
    CHECK(result.viewOffset == 0x1B0);
    CHECK(result.projectionOffset == 0x1F0);
    CHECK(result.viewProjectionOffset == 0x230);
    CHECK(result.storage == view::MatrixStorage::ColumnMajor);
    // The candidate count must reflect what was actually examined, so a
    // failure can be diagnosed instead of reporting a hardcoded zero.
    CHECK(result.candidateCount > 96);
}

TEST_CASE("P10L_camera_scan_fails_closed_without_a_consistent_triple",
    "[live][camera]")
{
    // Random-looking but finite data must not be mistaken for a camera.
    std::vector<std::byte> noise(0x250, std::byte{0});
    for (std::size_t index = 0; index < noise.size() / 4; ++index) {
        const auto value = 0.5f + 0.25f * static_cast<float>(index % 7);
        std::memcpy(noise.data() + index * 4, &value, sizeof(value));
    }
    CHECK_FALSE(camera::ScanCameraState(noise).found);

    // A block holding only a view and a projection, with no product, is not
    // enough to identify either.
    std::vector<std::byte> partial(0x250, std::byte{0});
    WriteMatrix(partial, 0x60, BuildView());
    WriteMatrix(partial, 0xA0, BuildProjection());
    CHECK_FALSE(camera::ScanCameraState(partial).found);

    std::vector<std::byte> tiny(32, std::byte{0});
    CHECK_FALSE(camera::ScanCameraState(tiny).found);

    // Non-finite values anywhere in a candidate disqualify it.
    auto poisoned = BuildBlock(false);
    const auto infinity = std::numeric_limits<float>::infinity();
    std::memcpy(poisoned.data() + 0x60, &infinity, sizeof(infinity));
    CHECK_FALSE(camera::ScanCameraState(poisoned).found);
}

TEST_CASE("P10L_camera_scan_reports_every_distinct_camera_in_the_record",
    "[live][camera]")
{
    // The engine keeps more than one camera live at a time, so a capture
    // that reports only the first one cannot tell a first-person camera
    // from the main world view.
    std::vector<std::byte> block(0x3C0, std::byte{0});
    const auto firstView = BuildView();
    const auto firstProjection = view::BuildPerspectiveProjection(
        1.0471975512f, 16.0f / 9.0f, 15.0f, 40'000.0f,
        view::Handedness::LeftHanded);
    WriteMatrix(block, 0x50, firstView);
    WriteMatrix(block, 0x90, firstProjection);
    WriteMatrix(block, 0xD0, view::Multiply(firstProjection, firstView));

    auto secondView = BuildView();
    secondView.elements[7] = 90.0f;
    const auto secondProjection = view::BuildPerspectiveProjection(
        0.6981317f, 16.0f / 9.0f, 40.0f, 400.0f,
        view::Handedness::LeftHanded);
    WriteMatrix(block, 0x220, secondView);
    WriteMatrix(block, 0x260, secondProjection);
    WriteMatrix(block, 0x2A0, view::Multiply(secondProjection, secondView));

    const auto results = camera::ScanCameraStates(block);
    REQUIRE(results.size() >= 2u);

    // Each reported camera must be internally consistent and distinct.
    for (const auto& result : results) {
        CHECK(result.found);
        CHECK(result.residual < 1.0e-3f);
    }
    const auto hasOffset = [&results](const std::uint32_t offset) {
        return std::any_of(results.begin(), results.end(),
            [offset](const camera::CameraScanResult& result) {
                return result.viewOffset == offset;
            });
    };
    CHECK(hasOffset(0x50));
    CHECK(hasOffset(0x220));

    // Clip planes are what separate a world camera from a near-field one.
    view::ClipPlanes planes{};
    const auto wide = std::find_if(results.begin(), results.end(),
        [](const camera::CameraScanResult& result) {
            return result.viewOffset == 0x50;
        });
    REQUIRE(wide != results.end());
    REQUIRE(view::ExtractClipPlanes(wide->projection,
        view::ProjectionMode::Perspective, view::Handedness::LeftHanded,
        planes) == view::ViewError::None);
    CHECK(planes.farPlane > 10'000.0f);
}

TEST_CASE("P10L_camera_series_builds_one_view_per_discovered_camera",
    "[live][camera]")
{
    std::vector<std::byte> block(0x3C0, std::byte{0});
    const auto firstView = BuildView();
    const auto firstProjection = BuildProjection();
    WriteMatrix(block, 0x50, firstView);
    WriteMatrix(block, 0x90, firstProjection);
    WriteMatrix(block, 0xD0, view::Multiply(firstProjection, firstView));
    auto secondView = BuildView();
    secondView.elements[3] = 900.0f;
    const auto secondProjection = view::BuildPerspectiveProjection(
        0.6981317f, 16.0f / 9.0f, 40.0f, 400.0f,
        view::Handedness::LeftHanded);
    WriteMatrix(block, 0x220, secondView);
    WriteMatrix(block, 0x260, secondProjection);
    WriteMatrix(block, 0x2A0, view::Multiply(secondProjection, secondView));

    const auto results = camera::ScanCameraStates(block);
    REQUIRE(results.size() >= 2);

    camera::CameraSeries series{};
    series.cameras.assign(results.begin(), results.end());
    series.outputWidth = 1280;
    series.outputHeight = 720;
    series.frameId = 4;
    series.engineFrameId = 0xE004;
    series.threadId = 7;

    view::FramePacket packet{};
    REQUIRE(camera::BuildFrameSeries(series, packet) ==
        camera::CameraError::None);
    CHECK(packet.views.size() == results.size());
    // Distinct identities, or a consumer cannot tell the cameras apart.
    for (std::size_t index = 1; index < packet.views.size(); ++index) {
        CHECK(packet.views[index].viewId != packet.views[0].viewId);
    }
    std::vector<std::byte> bytes;
    REQUIRE(view::EncodeFramePacket(packet, bytes) ==
        view::FramePacketError::None);
}

TEST_CASE("P10L_cameras_from_separate_records_keep_separate_identities",
    "[live][camera]")
{
    // Observed live: BSGraphics::State +0x140 is a camera-state cache array
    // of five records, and every one of them holds its camera at the same
    // +0x050 offset. Deriving identity from the offset alone made all five
    // collide, the frame packet was refused for duplicate views, and the
    // whole capture failed with "encode" — which named the symptom and not
    // the cause. The record a camera came from is therefore part of its
    // identity.
    std::vector<std::byte> record(0x250, std::byte{0});
    const auto viewMatrix = BuildView();
    const auto projection = BuildProjection();
    WriteMatrix(record, 0x50, viewMatrix);
    WriteMatrix(record, 0x90, projection);
    WriteMatrix(record, 0xD0, view::Multiply(projection, viewMatrix));

    const auto found = camera::ScanCameraStates(record);
    REQUIRE_FALSE(found.empty());

    camera::CameraSeries series{};
    // The same record scanned five times, exactly as five cache entries
    // holding cameras at the same offset would appear.
    for (std::uint32_t slot = 0; slot < 5; ++slot) {
        auto copy = found.front();
        copy.sourceSlot = slot;
        series.cameras.push_back(copy);
    }
    series.outputWidth = 1280;
    series.outputHeight = 720;
    series.frameId = 9;
    series.engineFrameId = 0xE009;
    series.threadId = 3;

    view::FramePacket packet{};
    REQUIRE(camera::BuildFrameSeries(series, packet) ==
        camera::CameraError::None);
    REQUIRE(packet.views.size() == series.cameras.size());
    for (std::size_t index = 0; index < packet.views.size(); ++index) {
        for (std::size_t other = 0; other < index; ++other) {
            CHECK(packet.views[index].viewId !=
                packet.views[other].viewId);
            CHECK(packet.views[index].cameraId !=
                packet.views[other].cameraId);
        }
    }
    // The whole point: identical cameras from distinct records must encode.
    std::vector<std::byte> bytes;
    CHECK(view::EncodeFramePacket(packet, bytes) ==
        view::FramePacketError::None);
}

TEST_CASE("P10L_camera_scan_translates_into_a_validated_view_record",
    "[live][camera]")
{
    const auto block = BuildBlock(false);
    const auto result = camera::ScanCameraState(block);
    REQUIRE(result.found);

    camera::CameraObservation observation{};
    observation.scan = result;
    observation.outputWidth = 1280;
    observation.outputHeight = 720;
    observation.nearPlane = 15.0f;
    observation.farPlane = 40'000.0f;
    observation.frameId = 9;
    observation.engineFrameId = 0xE009;
    observation.threadId = 21988;

    view::FramePacket packet{};
    REQUIRE(camera::BuildFramePacket(observation, packet) ==
        camera::CameraError::None);
    REQUIRE(packet.views.size() == 1);
    CHECK(view::ValidateView(packet.views.front()) == view::ViewError::None);
    CHECK(packet.views.front().outputWidth == 1280);
    CHECK(packet.header.frameId == 9);

    std::vector<std::byte> bytes;
    REQUIRE(view::EncodeFramePacket(packet, bytes) ==
        view::FramePacketError::None);
    view::FramePacket decoded;
    REQUIRE(view::DecodeFramePacket(bytes, decoded) ==
        view::FramePacketError::None);
    CHECK(decoded.views.size() == 1);

    camera::CameraObservation invalid = observation;
    invalid.outputWidth = 0;
    CHECK(camera::BuildFramePacket(invalid, packet) ==
        camera::CameraError::InvalidExtent);
    invalid = observation;
    invalid.scan.found = false;
    CHECK(camera::BuildFramePacket(invalid, packet) ==
        camera::CameraError::NoCameraFound);
}

namespace {

// The camera-relative view-projection multiplied by a translation of minus the
// camera position, which is what a full view-projection is. Built here rather
// than by hand so the test cannot encode the same mistake as the code.
view::Matrix4 TranslateViewProjection(
    const view::Matrix4& cameraRelative,
    const double x,
    const double y,
    const double z)
{
    view::Matrix4 translation{};
    translation.elements[0] = 1.0f;
    translation.elements[5] = 1.0f;
    translation.elements[10] = 1.0f;
    translation.elements[15] = 1.0f;
    translation.elements[3] = static_cast<float>(-x);
    translation.elements[7] = static_cast<float>(-y);
    translation.elements[11] = static_cast<float>(-z);
    return view::Multiply(cameraRelative, translation);
}

}

TEST_CASE("the camera world position is recovered from a translated view")
{
    // Fallout 4 hands out per-object transforms in absolute world coordinates
    // and a view matrix that carries no translation, which cannot both feed
    // one shader. Measured in the cell, every instance sat about a hundred and
    // twenty thousand units from the origin with the tenth and ninetieth
    // percentiles four per cent apart, and nothing at all below eye level:
    // the origin was the world's, not the camera's. Recovering the camera
    // position is what reconciles the two, and it is recovered rather than
    // estimated because an estimate would place the whole cell wrong by
    // however far the estimate was out.
    const auto projection = BuildProjection();
    view::Matrix4 rotation{};
    {
        // A rotation-only view, exactly the shape the engine was measured to
        // publish: an orthonormal basis with a zero fourth column.
        const auto angle = 0.9f;
        rotation.elements[0] = std::cos(angle);
        rotation.elements[2] = std::sin(angle);
        rotation.elements[5] = 1.0f;
        rotation.elements[8] = -std::sin(angle);
        rotation.elements[10] = std::cos(angle);
        rotation.elements[15] = 1.0f;
    }
    const auto cameraRelative = view::Multiply(projection, rotation);

    SECTION("a translated candidate yields the position that produced it")
    {
        // Sanctuary's rough distance from the world origin, so the test
        // exercises the magnitudes that actually occur rather than small ones
        // where a scale error would not show.
        const double expected[3]{-78'412.5, 86'104.25, 7'836.9};
        const auto candidate = TranslateViewProjection(
            cameraRelative, expected[0], expected[1], expected[2]);

        std::array<double, 3> origin{};
        float residual = -1.0f;
        REQUIRE(camera::RecoverCameraOrigin(cameraRelative, candidate, origin,
            residual) == camera::CameraOriginError::None);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            INFO(axis);
            // A tenth of a unit against magnitudes near a hundred thousand:
            // the matrices are single precision, so the recovery cannot be
            // exact, and a tolerance tight enough to fail on rounding would
            // reject correct answers.
            CHECK(std::abs(origin[axis] - expected[axis]) < 0.1);
        }
        CHECK(residual >= 0.0f);
    }

    SECTION("an untranslated candidate yields the origin itself")
    {
        std::array<double, 3> origin{1.0, 2.0, 3.0};
        float residual = -1.0f;
        REQUIRE(camera::RecoverCameraOrigin(cameraRelative, cameraRelative,
            origin, residual) == camera::CameraOriginError::None);
        for (const auto axis : origin) {
            CHECK(std::abs(axis) < 1.0e-3);
        }
    }

    SECTION("a candidate from a different projection is refused")
    {
        // Only the fourth column may differ. A candidate whose linear part is
        // not the same matrix describes a different camera, and solving it for
        // a position would return a number that is wrong without being
        // obviously wrong -- which is worse than returning nothing.
        auto other = TranslateViewProjection(cameraRelative, 10.0, 20.0, 30.0);
        other.elements[0] *= 1.5f;
        std::array<double, 3> origin{};
        float residual = -1.0f;
        CHECK(camera::RecoverCameraOrigin(cameraRelative, other, origin,
            residual) == camera::CameraOriginError::ProjectionMismatch);
    }

    SECTION("a rank-deficient linear part is refused rather than solved")
    {
        // Without three independent directions the position is not determined,
        // and the normal equations would hand back whichever member of the
        // solution family the arithmetic happened to land on.
        std::array<double, 3> origin{};
        float residual = -1.0f;

        // Nothing at all in the linear part.
        view::Matrix4 flat{};
        flat.elements[15] = 1.0f;
        const auto flatCandidate = TranslateViewProjection(flat, 5.0, 6.0, 7.0);
        CHECK(camera::RecoverCameraOrigin(flat, flatCandidate, origin,
            residual) == camera::CameraOriginError::Singular);

        // Two independent directions rather than none, which is the case that
        // actually reaches the elimination: the matrix is not empty, every
        // row is populated, and the third axis is still undetermined because
        // no row says anything about it. A check that only rejects the empty
        // matrix would pass this and return a third coordinate invented by
        // whichever way the arithmetic rounded.
        view::Matrix4 rankTwo{};
        rankTwo.elements[0] = 0.8f;
        rankTwo.elements[1] = -0.6f;
        rankTwo.elements[4] = 0.6f;
        rankTwo.elements[5] = 0.8f;
        rankTwo.elements[8] = 1.4f;
        rankTwo.elements[9] = 0.2f;
        rankTwo.elements[12] = -0.3f;
        rankTwo.elements[13] = 0.9f;
        rankTwo.elements[15] = 1.0f;
        const auto rankTwoCandidate =
            TranslateViewProjection(rankTwo, 5.0, 6.0, 7.0);
        CHECK(camera::RecoverCameraOrigin(rankTwo, rankTwoCandidate, origin,
            residual) == camera::CameraOriginError::Singular);
    }

    SECTION("a non-finite candidate is refused")
    {
        auto candidate = TranslateViewProjection(cameraRelative, 1.0, 2.0, 3.0);
        candidate.elements[3] =
            std::numeric_limits<float>::quiet_NaN();
        std::array<double, 3> origin{};
        float residual = -1.0f;
        CHECK(camera::RecoverCameraOrigin(cameraRelative, candidate, origin,
            residual) == camera::CameraOriginError::NonFinite);
    }

    SECTION("the residual catches a fourth column its rows cannot agree on")
    {
        // What the residual can and cannot do, stated exactly, because a
        // fourth equation over three unknowns sounds like it checks everything
        // and does not.
        //
        // A view-projection is a projection times a rotation, and in that
        // product the third and fourth rows have parallel linear parts -- both
        // are the rotation's third row, one of them scaled. The system
        // therefore has a one-dimensional left null space, and only the
        // component of an error along it produces any residual at all.
        // Everything else is absorbed by moving the recovered position, which
        // is a property of the arithmetic and not a defect to be fixed.
        //
        // The direction it does catch is the one that matters: a candidate
        // whose depth row and whose w row disagree about the same translation
        // is not a translation of this matrix by anything.
        const auto genuine =
            TranslateViewProjection(cameraRelative, 400.0, -250.0, 90.0);
        std::array<double, 3> trueOrigin{};
        float trueResidual = -1.0f;
        REQUIRE(camera::RecoverCameraOrigin(cameraRelative, genuine,
            trueOrigin, trueResidual) == camera::CameraOriginError::None);
        CHECK(trueResidual < 1.0e-2f);

        auto disagreeing = genuine;
        disagreeing.elements[15] += 250.0f;
        std::array<double, 3> disagreeingOrigin{};
        float disagreeingResidual = -1.0f;
        REQUIRE(camera::RecoverCameraOrigin(cameraRelative, disagreeing,
            disagreeingOrigin, disagreeingResidual) ==
            camera::CameraOriginError::None);
        CHECK(disagreeingResidual > trueResidual);
        CHECK(disagreeingResidual > 1.0f);
        // All four equations are fitted, not three solved exactly with the
        // fourth kept back to check them. The distinction is invisible on
        // consistent input and is exactly this on inconsistent input: a fit
        // spreads the disagreement into the answer, where a three-row solve
        // would return the same position it did before and report the trouble
        // only through the residual. Fitting is what makes the recovery
        // independent of knowing which three rows happen to be independent.
        auto moved = 0.0;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            moved = std::max(moved,
                std::abs(disagreeingOrigin[axis] - trueOrigin[axis]));
        }
        CHECK(moved > 1.0e-3);
    }

    SECTION("an error inside the solvable directions moves the answer")
    {
        // The other half of the same fact, asserted rather than left implicit:
        // perturbing a projection row's fourth column produces no residual,
        // and the recovered position absorbs it instead. A caller that treats
        // a small residual as proof the candidate is the right matrix is
        // relying on something this cannot tell it, so the strict equality of
        // the first three columns is what carries that weight.
        const auto genuine =
            TranslateViewProjection(cameraRelative, 400.0, -250.0, 90.0);
        std::array<double, 3> trueOrigin{};
        float trueResidual = -1.0f;
        REQUIRE(camera::RecoverCameraOrigin(cameraRelative, genuine,
            trueOrigin, trueResidual) == camera::CameraOriginError::None);

        auto shifted = genuine;
        shifted.elements[7] += 250.0f;
        std::array<double, 3> shiftedOrigin{};
        float shiftedResidual = -1.0f;
        REQUIRE(camera::RecoverCameraOrigin(cameraRelative, shifted,
            shiftedOrigin, shiftedResidual) ==
            camera::CameraOriginError::None);
        CHECK(shiftedResidual < 1.0e-2f);
        auto moved = 0.0;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            moved = std::max(moved,
                std::abs(shiftedOrigin[axis] - trueOrigin[axis]));
        }
        CHECK(moved > 1.0);
    }
}


TEST_CASE("the camera origin is the candidate the geometry is densest around")
{
    // The engine's camera record holds the camera's world position, and
    // nothing in the bytes says which triple it is. What distinguishes it is
    // its relationship to the frame: the loaded cell is built around the
    // player, so the camera is where the geometry is.
    //
    // A cell's worth of objects around the true position, plus two placed at
    // the origin the way an identity-transformed screen quad is. Both are what
    // a real frame contains and the pair is what the rule has to separate.
    std::vector<std::array<float, 3>> instances;
    for (int step = 0; step < 40; ++step) {
        const auto offset = static_cast<float>(step) * 30.0f;
        instances.push_back({-77'000.0f + offset, 92'000.0f, 7'840.0f});
    }
    instances.push_back({0.0f, 0.0f, 1.0f});
    instances.push_back({0.0f, 0.0f, 0.0f});

    SECTION("a zero triple with a quad on it loses to the populated camera")
    {
        // The live failure, exactly. Ranked by the single nearest instance,
        // the all-zero candidate scores a perfect zero against the quad at the
        // origin and wins, and the whole cell is then narrowed by nothing.
        // Measured: it chose [0,0,1] with a nearest distance of 0.00 out of a
        // hundred and forty-six candidates.
        const std::vector<std::array<float, 3>> candidates{
            {0.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, 1.0f},
            {-77'000.0f, 92'000.0f, 7'840.0f},
        };
        camera::OriginSelection selection{};
        REQUIRE(camera::SelectCameraOrigin(candidates, instances, 4'096.0f, 8,
            selection) == camera::OriginSelectionError::None);
        CHECK(selection.candidateIndex == 2);
        CHECK(selection.origin[0] == Catch::Approx(-77'000.0));
        CHECK(selection.neighbours >= 8);
    }

    SECTION("too little around any candidate is refused, not approximated")
    {
        // Returning the least bad candidate would misplace the whole cell by
        // however far it was out, and the frame would look rendered rather
        // than look broken -- which is the failure this exists to end.
        const std::vector<std::array<float, 3>> candidates{
            {0.0f, 0.0f, 0.0f},
            {500'000.0f, 500'000.0f, 500'000.0f},
        };
        camera::OriginSelection selection{};
        CHECK(camera::SelectCameraOrigin(candidates, instances, 4'096.0f, 8,
            selection) == camera::OriginSelectionError::NoneCredible);
    }

    SECTION("an empty side of the comparison is named rather than guessed")
    {
        camera::OriginSelection selection{};
        const std::vector<std::array<float, 3>> candidates{
            {-77'000.0f, 92'000.0f, 7'840.0f}};
        CHECK(camera::SelectCameraOrigin({}, instances, 4'096.0f, 8,
            selection) == camera::OriginSelectionError::NoCandidates);
        CHECK(camera::SelectCameraOrigin(candidates, {}, 4'096.0f, 8,
            selection) == camera::OriginSelectionError::NoGeometry);
    }

    SECTION("a non-finite candidate cannot win")
    {
        // Every comparison against a NaN is false, so it is counted by nothing
        // and is nearest to nothing.
        const auto notANumber = std::numeric_limits<float>::quiet_NaN();
        const std::vector<std::array<float, 3>> candidates{
            {notANumber, 0.0f, 0.0f},
            {-77'000.0f, 92'000.0f, 7'840.0f},
        };
        camera::OriginSelection selection{};
        REQUIRE(camera::SelectCameraOrigin(candidates, instances, 4'096.0f, 8,
            selection) == camera::OriginSelectionError::None);
        CHECK(selection.candidateIndex == 1);
    }

    SECTION("the first of two equally dense candidates is chosen every time")
    {
        // Determinism matters more than which: an origin alternating between
        // two equally good answers would shift the cell every frame.
        const std::vector<std::array<float, 3>> candidates{
            {-77'000.0f, 92'000.0f, 7'840.0f},
            {-77'000.0f, 92'000.0f, 7'840.0f},
        };
        camera::OriginSelection first{};
        camera::OriginSelection second{};
        REQUIRE(camera::SelectCameraOrigin(candidates, instances, 4'096.0f, 8,
            first) == camera::OriginSelectionError::None);
        REQUIRE(camera::SelectCameraOrigin(candidates, instances, 4'096.0f, 8,
            second) == camera::OriginSelectionError::None);
        CHECK(first.candidateIndex == 0);
        CHECK(second.candidateIndex == first.candidateIndex);
    }
}
