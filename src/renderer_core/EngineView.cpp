#include "renderer_core/EngineView.h"

#include "renderer_trace/Crc32.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace vf::renderer::view {

namespace {

constexpr float kMatrixTolerance = 2.0e-4f;
constexpr float kScalarTolerance = 1.0e-4f;

std::size_t MatrixIndex(
    const std::size_t row,
    const std::size_t column) noexcept
{
    return row * 4 + column;
}

bool Finite(const float value) noexcept
{
    return std::isfinite(value);
}

bool Finite(const double value) noexcept
{
    return std::isfinite(value);
}

bool MatrixFinite(const Matrix4& matrix) noexcept
{
    return std::all_of(std::begin(matrix.elements),
        std::end(matrix.elements), [](const float value) {
            return Finite(value);
        });
}

bool MatrixNear(
    const Matrix4& left,
    const Matrix4& right,
    const float tolerance = kMatrixTolerance) noexcept
{
    for (std::size_t index = 0; index < 16; ++index) {
        if (std::abs(left.elements[index] - right.elements[index]) >
            tolerance) {
            return false;
        }
    }
    return true;
}

bool ScalarNear(
    const float left,
    const float right,
    const float tolerance = kScalarTolerance) noexcept
{
    const auto scale = std::max({1.0f, std::abs(left), std::abs(right)});
    return std::abs(left - right) <= tolerance * scale;
}

bool DoubleNear(const double left, const double right) noexcept
{
    const auto scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) <= 1.0e-9 * scale;
}

std::size_t AlignUp(
    const std::size_t value,
    const std::size_t alignment) noexcept
{
    if (alignment == 0 || value >
        std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

bool CheckedMultiply(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result) noexcept
{
    if (left != 0 && right >
        std::numeric_limits<std::size_t>::max() / left) {
        result = std::numeric_limits<std::size_t>::max();
        return false;
    }
    result = left * right;
    return true;
}

bool CheckedAdd(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result) noexcept
{
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        result = std::numeric_limits<std::size_t>::max();
        return false;
    }
    result = left + right;
    return true;
}

bool ValidProjectionMode(const ProjectionMode mode) noexcept
{
    return mode == ProjectionMode::Perspective ||
        mode == ProjectionMode::Orthographic;
}

bool ValidHandedness(const Handedness handedness) noexcept
{
    return handedness == Handedness::LeftHanded ||
        handedness == Handedness::RightHanded;
}

bool ValidShaderDomain(const ShaderDomain domain) noexcept
{
    return static_cast<std::uint32_t>(domain) <=
        static_cast<std::uint32_t>(ShaderDomain::ImageSpace);
}

bool ValidPassCategory(const PassCategory category) noexcept
{
    return static_cast<std::uint32_t>(category) <=
        static_cast<std::uint32_t>(PassCategory::Interface);
}

Matrix4 RemoveProjectionJitter(
    Matrix4 projection,
    const ProjectionMode mode,
    const Handedness handedness,
    const std::array<float, 2> jitter) noexcept
{
    if (mode == ProjectionMode::Perspective) {
        const auto sign = handedness == Handedness::LeftHanded
            ? 1.0f : -1.0f;
        projection.elements[MatrixIndex(0, 2)] -= sign * jitter[0];
        projection.elements[MatrixIndex(1, 2)] -= sign * jitter[1];
    } else {
        projection.elements[MatrixIndex(0, 3)] -= jitter[0];
        projection.elements[MatrixIndex(1, 3)] -= jitter[1];
    }
    return projection;
}

Matrix4 D3DToVulkanProjection(const Matrix4& projection) noexcept
{
    auto conversion = IdentityMatrix();
    conversion.elements[MatrixIndex(1, 1)] = -1.0f;
    return Multiply(conversion, projection);
}

ViewError ValidateViewportAndScissor(const ViewRecordV1& view) noexcept
{
    const auto& viewport = view.viewport;
    if (!Finite(viewport.x) || !Finite(viewport.y) ||
        !Finite(viewport.width) || !Finite(viewport.height) ||
        !Finite(viewport.minimumDepth) || !Finite(viewport.maximumDepth) ||
        viewport.x < 0.0f || viewport.y < 0.0f ||
        viewport.width <= 0.0f || viewport.height <= 0.0f ||
        viewport.x + viewport.width >
            static_cast<float>(view.outputWidth) + kScalarTolerance ||
        viewport.y + viewport.height >
            static_cast<float>(view.outputHeight) + kScalarTolerance ||
        viewport.minimumDepth < 0.0f || viewport.maximumDepth > 1.0f ||
        viewport.minimumDepth >= viewport.maximumDepth) {
        return ViewError::InvalidViewport;
    }
    const auto& scissor = view.scissor;
    const auto right = static_cast<std::int64_t>(scissor.x) +
        static_cast<std::int64_t>(scissor.width);
    const auto bottom = static_cast<std::int64_t>(scissor.y) +
        static_cast<std::int64_t>(scissor.height);
    if (scissor.x < 0 || scissor.y < 0 || scissor.width == 0 ||
        scissor.height == 0 || right > view.outputWidth ||
        bottom > view.outputHeight) {
        return ViewError::InvalidScissor;
    }
    return ViewError::None;
}

FramePacketError ValidatePacket(const FramePacket& packet) noexcept
{
    if (packet.header.frameId == 0 || packet.header.engineFrameId == 0 ||
        packet.header.captureSequence == 0 ||
        packet.header.captureThreadId == 0 ||
        packet.header.renderThreadId == 0) {
        return FramePacketError::InvalidView;
    }
    if (packet.header.captureThreadId != packet.header.renderThreadId) {
        return FramePacketError::WrongThread;
    }
    if (packet.header.flags != 0 || packet.views.empty() ||
        packet.views.size() > kMaximumViews ||
        packet.passes.size() > kMaximumPasses) {
        return FramePacketError::InvalidView;
    }
    for (std::size_t index = 0; index < packet.views.size(); ++index) {
        if (ValidateView(packet.views[index]) != ViewError::None) {
            return FramePacketError::InvalidView;
        }
        for (std::size_t other = 0; other < index; ++other) {
            if (packet.views[other].viewId == packet.views[index].viewId) {
                return FramePacketError::InvalidView;
            }
        }
    }
    std::uint64_t previousSequence{};
    for (const auto& pass : packet.passes) {
        if (pass.sequence == 0 || pass.sequence <= previousSequence) {
            return FramePacketError::NonMonotonicPass;
        }
        previousSequence = pass.sequence;
        if (!ValidShaderDomain(pass.domain) ||
            !ValidPassCategory(pass.category) ||
            (pass.flags & ~kKnownPassFlags) != 0 ||
            ClassifyPass(pass.domain, pass.renderMode, pass.flags) !=
                pass.category ||
            std::none_of(packet.views.begin(), packet.views.end(),
                [&pass](const ViewRecordV1& view) {
                    return view.viewId == pass.viewId;
                })) {
            return FramePacketError::InvalidPass;
        }
    }
    return FramePacketError::None;
}

}

Matrix4 IdentityMatrix() noexcept
{
    Matrix4 result{};
    for (std::size_t index = 0; index < 4; ++index) {
        result.elements[MatrixIndex(index, index)] = 1.0f;
    }
    return result;
}

Matrix4 NormalizeSourceMatrix(const SourceMatrix4& source) noexcept
{
    Matrix4 result{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            const auto sourceIndex = source.storage == MatrixStorage::RowMajor
                ? MatrixIndex(row, column) : MatrixIndex(column, row);
            const auto destinationIndex =
                source.vectors == VectorConvention::ColumnVector
                ? MatrixIndex(row, column) : MatrixIndex(column, row);
            result.elements[destinationIndex] = source.elements[sourceIndex];
        }
    }
    return result;
}

Matrix4 Multiply(const Matrix4& left, const Matrix4& right) noexcept
{
    Matrix4 result{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            double value{};
            for (std::size_t inner = 0; inner < 4; ++inner) {
                value += static_cast<double>(
                    left.elements[MatrixIndex(row, inner)]) *
                    right.elements[MatrixIndex(inner, column)];
            }
            result.elements[MatrixIndex(row, column)] =
                static_cast<float>(value);
        }
    }
    return result;
}

bool Invert(const Matrix4& matrix, Matrix4& inverse) noexcept
{
    inverse = {};
    if (!MatrixFinite(matrix)) return false;
    double augmented[4][8]{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            augmented[row][column] =
                matrix.elements[MatrixIndex(row, column)];
        }
        augmented[row][4 + row] = 1.0;
    }
    for (std::size_t column = 0; column < 4; ++column) {
        auto pivot = column;
        for (std::size_t row = column + 1; row < 4; ++row) {
            if (std::abs(augmented[row][column]) >
                std::abs(augmented[pivot][column])) {
                pivot = row;
            }
        }
        if (std::abs(augmented[pivot][column]) <= 1.0e-12) return false;
        if (pivot != column) {
            for (std::size_t index = 0; index < 8; ++index) {
                std::swap(augmented[pivot][index],
                    augmented[column][index]);
            }
        }
        const auto divisor = augmented[column][column];
        for (double& value : augmented[column]) value /= divisor;
        for (std::size_t row = 0; row < 4; ++row) {
            if (row == column) continue;
            const auto factor = augmented[row][column];
            for (std::size_t index = 0; index < 8; ++index) {
                augmented[row][index] -= factor * augmented[column][index];
            }
        }
    }
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            const auto value = augmented[row][4 + column];
            if (!std::isfinite(value) ||
                std::abs(value) > std::numeric_limits<float>::max()) {
                inverse = {};
                return false;
            }
            inverse.elements[MatrixIndex(row, column)] =
                static_cast<float>(value);
        }
    }
    return true;
}

Matrix4 BuildPerspectiveProjection(
    const float verticalFovRadians,
    const float aspect,
    const float nearPlane,
    const float farPlane,
    const Handedness handedness,
    const std::array<float, 2> jitterNdc) noexcept
{
    Matrix4 result{};
    if (!Finite(verticalFovRadians) || !Finite(aspect) ||
        !Finite(nearPlane) || !Finite(farPlane) ||
        !Finite(jitterNdc[0]) || !Finite(jitterNdc[1]) ||
        !ValidHandedness(handedness) || aspect <= 0.0f ||
        verticalFovRadians <= 0.0f ||
        verticalFovRadians >= 3.14159265359f ||
        nearPlane <= 0.0f || farPlane <= nearPlane) {
        return result;
    }
    const auto yScale = 1.0f / std::tan(verticalFovRadians * 0.5f);
    result.elements[MatrixIndex(0, 0)] = yScale / aspect;
    result.elements[MatrixIndex(1, 1)] = yScale;
    const auto sign = handedness == Handedness::LeftHanded ? 1.0f : -1.0f;
    result.elements[MatrixIndex(0, 2)] = sign * jitterNdc[0];
    result.elements[MatrixIndex(1, 2)] = sign * jitterNdc[1];
    if (handedness == Handedness::LeftHanded) {
        result.elements[MatrixIndex(2, 2)] =
            farPlane / (farPlane - nearPlane);
        result.elements[MatrixIndex(2, 3)] =
            -nearPlane * farPlane / (farPlane - nearPlane);
        result.elements[MatrixIndex(3, 2)] = 1.0f;
    } else {
        result.elements[MatrixIndex(2, 2)] =
            farPlane / (nearPlane - farPlane);
        result.elements[MatrixIndex(2, 3)] =
            nearPlane * farPlane / (nearPlane - farPlane);
        result.elements[MatrixIndex(3, 2)] = -1.0f;
    }
    return result;
}

Matrix4 BuildOrthographicProjection(
    const float width,
    const float height,
    const float nearPlane,
    const float farPlane,
    const Handedness handedness,
    const std::array<float, 2> jitterNdc) noexcept
{
    Matrix4 result{};
    if (!Finite(width) || !Finite(height) || !Finite(nearPlane) ||
        !Finite(farPlane) || !Finite(jitterNdc[0]) ||
        !Finite(jitterNdc[1]) || !ValidHandedness(handedness) ||
        width <= 0.0f || height <= 0.0f || nearPlane < 0.0f ||
        farPlane <= nearPlane) {
        return result;
    }
    result.elements[MatrixIndex(0, 0)] = 2.0f / width;
    result.elements[MatrixIndex(1, 1)] = 2.0f / height;
    result.elements[MatrixIndex(0, 3)] = jitterNdc[0];
    result.elements[MatrixIndex(1, 3)] = jitterNdc[1];
    if (handedness == Handedness::LeftHanded) {
        result.elements[MatrixIndex(2, 2)] = 1.0f / (farPlane - nearPlane);
        result.elements[MatrixIndex(2, 3)] =
            -nearPlane / (farPlane - nearPlane);
    } else {
        result.elements[MatrixIndex(2, 2)] = 1.0f / (nearPlane - farPlane);
        result.elements[MatrixIndex(2, 3)] =
            nearPlane / (nearPlane - farPlane);
    }
    result.elements[MatrixIndex(3, 3)] = 1.0f;
    return result;
}

ViewError ExtractClipPlanes(
    const Matrix4& projection,
    const ProjectionMode mode,
    const Handedness handedness,
    ClipPlanes& planes) noexcept
{
    planes = {};
    if (!MatrixFinite(projection)) return ViewError::NonFinite;
    if (!ValidProjectionMode(mode) || !ValidHandedness(handedness)) {
        return ViewError::InvalidProjection;
    }
    const auto a = projection.elements[MatrixIndex(2, 2)];
    const auto b = projection.elements[MatrixIndex(2, 3)];
    if (std::abs(a) <= std::numeric_limits<float>::epsilon()) {
        return ViewError::InvalidProjection;
    }
    if (mode == ProjectionMode::Perspective) {
        const auto expectedSign = handedness == Handedness::LeftHanded
            ? 1.0f : -1.0f;
        if (!ScalarNear(projection.elements[MatrixIndex(3, 2)],
                expectedSign) ||
            std::abs(projection.elements[MatrixIndex(3, 3)]) >
                kScalarTolerance) {
            return ViewError::InvalidProjection;
        }
        if (handedness == Handedness::LeftHanded) {
            if (std::abs(a - 1.0f) <=
                std::numeric_limits<float>::epsilon()) {
                return ViewError::InvalidProjection;
            }
            planes.nearPlane = -b / a;
            planes.farPlane = -b / (a - 1.0f);
        } else {
            if (std::abs(a + 1.0f) <=
                std::numeric_limits<float>::epsilon()) {
                return ViewError::InvalidProjection;
            }
            planes.nearPlane = b / a;
            planes.farPlane = b / (a + 1.0f);
        }
    } else {
        if (std::abs(projection.elements[MatrixIndex(3, 3)] - 1.0f) >
                kScalarTolerance ||
            std::abs(projection.elements[MatrixIndex(3, 2)]) >
                kScalarTolerance) {
            return ViewError::InvalidProjection;
        }
        if (handedness == Handedness::LeftHanded) {
            planes.nearPlane = -b / a;
            planes.farPlane = (1.0f - b) / a;
        } else {
            planes.nearPlane = b / a;
            planes.farPlane = planes.nearPlane - 1.0f / a;
        }
    }
    if (!Finite(planes.nearPlane) || !Finite(planes.farPlane) ||
        (mode == ProjectionMode::Perspective && planes.nearPlane <= 0.0f) ||
        (mode == ProjectionMode::Orthographic && planes.nearPlane < 0.0f) ||
        planes.farPlane <= planes.nearPlane) {
        planes = {};
        return ViewError::InvalidClipPlanes;
    }
    return ViewError::None;
}

ViewError TranslateCapturedView(
    const CapturedView& captured,
    ViewRecordV1& view) noexcept
{
    view = {};
    if (!Finite(captured.renderScale) || !Finite(captured.nearPlane) ||
        !Finite(captured.farPlane) ||
        !Finite(captured.verticalFovRadians) ||
        !Finite(captured.jitterNdc[0]) ||
        !Finite(captured.jitterNdc[1]) ||
        !Finite(captured.previousJitterNdc[0]) ||
        !Finite(captured.previousJitterNdc[1]) ||
        !std::all_of(captured.cameraRelativeOrigin.begin(),
            captured.cameraRelativeOrigin.end(),
            [](const double value) { return Finite(value); }) ||
        !std::all_of(captured.previousCameraRelativeOrigin.begin(),
            captured.previousCameraRelativeOrigin.end(),
            [](const double value) { return Finite(value); })) {
        return ViewError::NonFinite;
    }
    const auto sourceView = NormalizeSourceMatrix(captured.view);
    const auto sourceProjection = NormalizeSourceMatrix(captured.projection);
    const auto previousView = NormalizeSourceMatrix(captured.previousView);
    const auto previousProjection =
        NormalizeSourceMatrix(captured.previousProjection);
    if (!MatrixFinite(sourceView) || !MatrixFinite(sourceProjection) ||
        !MatrixFinite(previousView) || !MatrixFinite(previousProjection)) {
        return ViewError::NonFinite;
    }
    ClipPlanes extracted{};
    const auto clipResult = ExtractClipPlanes(sourceProjection,
        captured.projectionMode, captured.handedness, extracted);
    if (clipResult != ViewError::None) return clipResult;
    if (!ScalarNear(extracted.nearPlane, captured.nearPlane, 5.0e-4f) ||
        !ScalarNear(extracted.farPlane, captured.farPlane, 5.0e-4f)) {
        return ViewError::InvalidClipPlanes;
    }

    view.viewId = captured.viewId;
    view.cameraId = captured.cameraId;
    view.projectionMode = captured.projectionMode;
    view.handedness = captured.handedness;
    view.flags = captured.flags;
    view.renderMode = captured.renderMode;
    view.targetId = captured.targetId;
    view.outputWidth = captured.outputWidth;
    view.outputHeight = captured.outputHeight;
    view.aaMode = captured.aaMode;
    view.renderScale = captured.renderScale;
    view.nearPlane = captured.nearPlane;
    view.farPlane = captured.farPlane;
    view.verticalFovRadians = captured.verticalFovRadians;
    view.jitterNdc[0] = captured.jitterNdc[0];
    view.jitterNdc[1] = -captured.jitterNdc[1];
    view.previousJitterNdc[0] = captured.previousJitterNdc[0];
    view.previousJitterNdc[1] = -captured.previousJitterNdc[1];
    std::copy(captured.cameraRelativeOrigin.begin(),
        captured.cameraRelativeOrigin.end(), view.cameraRelativeOrigin);
    std::copy(captured.previousCameraRelativeOrigin.begin(),
        captured.previousCameraRelativeOrigin.end(),
        view.previousCameraRelativeOrigin);
    view.viewport = captured.viewport;
    view.scissor = captured.scissor;
    view.view = sourceView;
    view.projection = D3DToVulkanProjection(sourceProjection);
    view.viewProjection = Multiply(view.projection, view.view);
    if (!Invert(view.viewProjection, view.inverseViewProjection)) {
        view = {};
        return ViewError::SingularMatrix;
    }
    view.previousViewProjection = Multiply(
        D3DToVulkanProjection(previousProjection), previousView);
    const auto unjitteredProjection = RemoveProjectionJitter(
        sourceProjection, captured.projectionMode, captured.handedness,
        captured.jitterNdc);
    const auto previousUnjitteredProjection = RemoveProjectionJitter(
        previousProjection, captured.projectionMode, captured.handedness,
        captured.previousJitterNdc);
    view.unjitteredViewProjection = Multiply(
        D3DToVulkanProjection(unjitteredProjection), sourceView);
    view.previousUnjitteredViewProjection = Multiply(
        D3DToVulkanProjection(previousUnjitteredProjection), previousView);
    const auto validation = ValidateView(view);
    if (validation != ViewError::None) view = {};
    return validation;
}

ViewError ValidateView(const ViewRecordV1& view) noexcept
{
    if (view.viewId == 0 || view.cameraId == 0 || view.outputWidth == 0 ||
        view.outputHeight == 0 || view.outputWidth > kMaximumViewExtent ||
        view.outputHeight > kMaximumViewExtent) {
        return ViewError::InvalidIdentity;
    }
    if (!ValidProjectionMode(view.projectionMode) ||
        !ValidHandedness(view.handedness)) {
        return ViewError::InvalidProjection;
    }
    if ((view.flags & ~kKnownViewFlags) != 0) {
        return ViewError::InvalidFlags;
    }
    if (!Finite(view.renderScale) || !Finite(view.nearPlane) ||
        !Finite(view.farPlane) || !Finite(view.verticalFovRadians) ||
        !Finite(view.jitterNdc[0]) || !Finite(view.jitterNdc[1]) ||
        !Finite(view.previousJitterNdc[0]) ||
        !Finite(view.previousJitterNdc[1]) ||
        view.renderScale <= 0.0f || view.renderScale > 2.0f ||
        std::abs(view.jitterNdc[0]) > 2.0f ||
        std::abs(view.jitterNdc[1]) > 2.0f ||
        std::abs(view.previousJitterNdc[0]) > 2.0f ||
        std::abs(view.previousJitterNdc[1]) > 2.0f) {
        return ViewError::NonFinite;
    }
    for (std::size_t index = 0; index < 3; ++index) {
        if (!Finite(view.cameraRelativeOrigin[index]) ||
            !Finite(view.previousCameraRelativeOrigin[index])) {
            return ViewError::NonFinite;
        }
    }
    const auto viewportResult = ValidateViewportAndScissor(view);
    if (viewportResult != ViewError::None) return viewportResult;
    if (!MatrixFinite(view.view) || !MatrixFinite(view.projection) ||
        !MatrixFinite(view.viewProjection) ||
        !MatrixFinite(view.inverseViewProjection) ||
        !MatrixFinite(view.previousViewProjection) ||
        !MatrixFinite(view.unjitteredViewProjection) ||
        !MatrixFinite(view.previousUnjitteredViewProjection)) {
        return ViewError::NonFinite;
    }
    const auto expectedViewProjection = Multiply(view.projection, view.view);
    if (!MatrixNear(expectedViewProjection, view.viewProjection)) {
        return ViewError::InconsistentMatrix;
    }
    const auto identity = Multiply(
        view.viewProjection, view.inverseViewProjection);
    if (!MatrixNear(identity, IdentityMatrix(), 8.0e-4f)) {
        return ViewError::SingularMatrix;
    }
    ClipPlanes extracted{};
    const auto clipResult = ExtractClipPlanes(view.projection,
        view.projectionMode, view.handedness, extracted);
    if (clipResult != ViewError::None) return clipResult;
    if (!ScalarNear(extracted.nearPlane, view.nearPlane, 5.0e-4f) ||
        !ScalarNear(extracted.farPlane, view.farPlane, 5.0e-4f)) {
        return ViewError::InvalidClipPlanes;
    }
    if (view.projectionMode == ProjectionMode::Perspective) {
        const auto yScale = std::abs(
            view.projection.elements[MatrixIndex(1, 1)]);
        if (yScale <= std::numeric_limits<float>::epsilon()) {
            return ViewError::InvalidProjection;
        }
        const auto fov = 2.0f * std::atan(1.0f / yScale);
        if (view.verticalFovRadians <= 0.0f ||
            !ScalarNear(fov, view.verticalFovRadians, 5.0e-4f)) {
            return ViewError::InvalidProjection;
        }
    } else if (std::abs(view.verticalFovRadians) > kScalarTolerance) {
        return ViewError::InvalidProjection;
    }
    if (std::any_of(std::begin(view.reserved0), std::end(view.reserved0),
            [](const std::uint32_t value) { return value != 0; }) ||
        std::any_of(std::begin(view.reserved1), std::end(view.reserved1),
            [](const std::uint32_t value) { return value != 0; })) {
        return ViewError::InvalidFlags;
    }
    return ViewError::None;
}

ViewError ProjectWorldPoint(
    const ViewRecordV1& view,
    const std::array<double, 3> worldPoint,
    ProjectedPoint& projected) noexcept
{
    projected = {};
    const auto validation = ValidateView(view);
    if (validation != ViewError::None) return validation;
    if (!std::all_of(worldPoint.begin(), worldPoint.end(),
            [](const double value) { return Finite(value); })) {
        return ViewError::NonFinite;
    }
    double input[4]{worldPoint[0], worldPoint[1], worldPoint[2], 1.0};
    if ((view.flags & ViewCameraRelative) != 0) {
        for (std::size_t index = 0; index < 3; ++index) {
            input[index] -= view.cameraRelativeOrigin[index];
        }
    }
    double clip[4]{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            clip[row] += static_cast<double>(
                view.viewProjection.elements[MatrixIndex(row, column)]) *
                input[column];
        }
        if (!std::isfinite(clip[row])) return ViewError::NonFinite;
    }
    if (std::abs(clip[3]) <= 1.0e-12) {
        return ViewError::InvalidProjection;
    }
    const auto ndcX = static_cast<float>(clip[0] / clip[3]);
    const auto ndcY = static_cast<float>(clip[1] / clip[3]);
    const auto ndcZ = static_cast<float>(clip[2] / clip[3]);
    projected.x = view.viewport.x +
        (ndcX * 0.5f + 0.5f) * view.viewport.width;
    projected.y = view.viewport.y +
        (ndcY * 0.5f + 0.5f) * view.viewport.height;
    projected.depth = view.viewport.minimumDepth + ndcZ *
        (view.viewport.maximumDepth - view.viewport.minimumDepth);
    projected.clipW = static_cast<float>(clip[3]);
    projected.inside = clip[3] > 0.0 && ndcX >= -1.0f && ndcX <= 1.0f &&
        ndcY >= -1.0f && ndcY <= 1.0f && ndcZ >= 0.0f && ndcZ <= 1.0f;
    return ViewError::None;
}

ViewError BuildGpuViewConstants(
    const ViewRecordV1* const view,
    const std::uint64_t historyEpoch,
    GpuViewConstantsV1& constants) noexcept
{
    constants = {};
    if (view == nullptr) {
        const auto identity = IdentityMatrix();
        std::copy(std::begin(identity.elements), std::end(identity.elements),
            std::begin(constants.viewProjectionRows));
        std::copy(std::begin(identity.elements), std::end(identity.elements),
            std::begin(constants.previousViewProjectionRows));
        std::copy(std::begin(identity.elements), std::end(identity.elements),
            std::begin(constants.unjitteredViewProjectionRows));
        std::copy(std::begin(identity.elements), std::end(identity.elements),
            std::begin(constants.inverseViewProjectionRows));
        return ViewError::None;
    }
    const auto validation = ValidateView(*view);
    if (validation != ViewError::None) return validation;
    std::copy(std::begin(view->viewProjection.elements),
        std::end(view->viewProjection.elements),
        std::begin(constants.viewProjectionRows));
    // Carried so a pass with no geometry can recover the ray a pixel stands
    // for. The record already holds it; it simply never reached the device.
    std::copy(std::begin(view->inverseViewProjection.elements),
        std::end(view->inverseViewProjection.elements),
        std::begin(constants.inverseViewProjectionRows));
    std::copy(std::begin(view->previousViewProjection.elements),
        std::end(view->previousViewProjection.elements),
        std::begin(constants.previousViewProjectionRows));
    std::copy(std::begin(view->unjitteredViewProjection.elements),
        std::end(view->unjitteredViewProjection.elements),
        std::begin(constants.unjitteredViewProjectionRows));
    constants.clipAndJitter[0] = view->nearPlane;
    constants.clipAndJitter[1] = view->farPlane;
    constants.clipAndJitter[2] = view->jitterNdc[0];
    constants.clipAndJitter[3] = view->jitterNdc[1];
    constants.viewport[0] = view->viewport.x;
    constants.viewport[1] = view->viewport.y;
    constants.viewport[2] = view->viewport.width;
    constants.viewport[3] = view->viewport.height;
    constants.identifiers[0] = kGpuViewProjectionEnabled;
    constants.identifiers[1] = view->flags;
    constants.identifiers[2] = static_cast<std::uint32_t>(view->viewId);
    constants.identifiers[3] = static_cast<std::uint32_t>(historyEpoch);
    return ViewError::None;
}

PassCategory ClassifyPass(
    const ShaderDomain domain,
    const std::uint32_t renderMode,
    const std::uint32_t flags) noexcept
{
    if (!ValidShaderDomain(domain) || (flags & ~kKnownPassFlags) != 0) {
        return PassCategory::Unknown;
    }
    if ((flags & PassInterface) != 0) return PassCategory::Interface;
    if (renderMode == 0x0D) return PassCategory::DepthPrepass;
    if (renderMode == 0x0E) return PassCategory::Occlusion;
    if (renderMode >= 0x0F && renderMode <= 0x11) {
        return PassCategory::Shadow;
    }
    if (renderMode == 0x12) return PassCategory::LocalMap;
    if (renderMode >= 0x15 && renderMode <= 0x17) {
        return PassCategory::Lod;
    }
    if (renderMode >= 0x21 && renderMode <= 0x23) {
        return PassCategory::Vats;
    }
    if (domain == ShaderDomain::ImageSpace) return PassCategory::ImageSpace;
    if (domain == ShaderDomain::Sky) return PassCategory::Sky;
    if (domain == ShaderDomain::Water) return PassCategory::Water;
    if (domain == ShaderDomain::Particle ||
        domain == ShaderDomain::BloodSpatter) {
        return PassCategory::ParticleEffect;
    }
    if ((flags & PassTransparent) != 0) return PassCategory::Transparent;
    if ((flags & PassAlphaTest) != 0) return PassCategory::AlphaTest;
    if ((flags & PassWritesWorldTarget) != 0 &&
        (domain == ShaderDomain::Lighting ||
         domain == ShaderDomain::DistantTree ||
         domain == ShaderDomain::DeferredPrepass ||
         domain == ShaderDomain::FaceCustomization)) {
        return PassCategory::Opaque;
    }
    return PassCategory::Unknown;
}

PassCoverage SummarizePassCoverage(
    const std::span<const PassRecordV1> passes) noexcept
{
    PassCoverage coverage{};
    for (const auto& pass : passes) {
        if (pass.category == PassCategory::Unknown) {
            ++coverage.unknown;
            if ((pass.flags & PassWritesWorldTarget) != 0) {
                ++coverage.unknownWorldWriters;
            }
        } else {
            ++coverage.classified;
        }
    }
    return coverage;
}

FramePacketError EncodeFramePacket(
    const FramePacket& packet,
    std::vector<std::byte>& bytes) noexcept
{
    bytes.clear();
    const auto validation = ValidatePacket(packet);
    if (validation != FramePacketError::None) return validation;
    try {
        FramePacketHeaderV1 header{};
        header.frameId = packet.header.frameId;
        header.engineFrameId = packet.header.engineFrameId;
        header.historyEpoch = packet.header.historyEpoch;
        header.captureSequence = packet.header.captureSequence;
        header.captureThreadId = packet.header.captureThreadId;
        header.renderThreadId = packet.header.renderThreadId;
        header.flags = packet.header.flags;
        header.viewCount = static_cast<std::uint32_t>(packet.views.size());
        header.passCount = static_cast<std::uint32_t>(packet.passes.size());
        const auto viewsOffset = AlignUp(sizeof(header), alignof(ViewRecordV1));
        std::size_t viewBytes{};
        std::size_t viewsEnd{};
        if (viewsOffset == std::numeric_limits<std::size_t>::max() ||
            !CheckedMultiply(packet.views.size(), sizeof(ViewRecordV1),
                viewBytes) ||
            !CheckedAdd(viewsOffset, viewBytes, viewsEnd)) {
            return FramePacketError::AllocationFailure;
        }
        const auto passesOffset = AlignUp(viewsEnd, alignof(PassRecordV1));
        std::size_t passBytes{};
        std::size_t totalSize{};
        if (passesOffset == std::numeric_limits<std::size_t>::max() ||
            !CheckedMultiply(packet.passes.size(), sizeof(PassRecordV1),
                passBytes) ||
            !CheckedAdd(passesOffset, passBytes, totalSize) ||
            totalSize > std::numeric_limits<std::uint32_t>::max()) {
            return FramePacketError::AllocationFailure;
        }
        header.viewsOffset = static_cast<std::uint32_t>(viewsOffset);
        header.passesOffset = static_cast<std::uint32_t>(passesOffset);
        header.totalSize = static_cast<std::uint32_t>(totalSize);
        bytes.assign(totalSize, std::byte{0});
        if (!packet.views.empty()) {
            std::memcpy(bytes.data() + viewsOffset, packet.views.data(),
                viewBytes);
        }
        if (!packet.passes.empty()) {
            std::memcpy(bytes.data() + passesOffset, packet.passes.data(),
                passBytes);
        }
        header.payloadCrc32 = trace::Crc32(
            std::span<const std::byte>{bytes}.subspan(sizeof(header)));
        std::memcpy(bytes.data(), &header, sizeof(header));
        return FramePacketError::None;
    } catch (...) {
        bytes.clear();
        return FramePacketError::AllocationFailure;
    }
}

FramePacketError DecodeFramePacket(
    const std::span<const std::byte> bytes,
    FramePacket& packet) noexcept
{
    packet = {};
    if (bytes.size() < sizeof(FramePacketHeaderV1)) {
        return FramePacketError::TruncatedHeader;
    }
    FramePacketHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kFramePacketMagic) return FramePacketError::BadMagic;
    if (header.versionMajor != kFramePacketVersionMajor ||
        header.versionMinor > kFramePacketVersionMinor) {
        return FramePacketError::UnsupportedVersion;
    }
    if (header.endianMarker != kFramePacketEndian) {
        return FramePacketError::WrongEndian;
    }
    if (header.headerSize != sizeof(header)) {
        return FramePacketError::SectionOutOfBounds;
    }
    if (header.totalSize != bytes.size()) {
        return FramePacketError::SizeMismatch;
    }
    if (header.reserved != 0 || header.reserved64 != 0) {
        return FramePacketError::NonZeroPadding;
    }
    if (header.viewCount == 0 || header.viewCount > kMaximumViews ||
        header.passCount > kMaximumPasses) {
        return FramePacketError::SectionOutOfBounds;
    }
    const auto expectedViews = AlignUp(
        sizeof(header), alignof(ViewRecordV1));
    std::size_t viewBytes{};
    std::size_t viewsEnd{};
    if (!CheckedMultiply(header.viewCount, sizeof(ViewRecordV1), viewBytes) ||
        !CheckedAdd(expectedViews, viewBytes, viewsEnd)) {
        return FramePacketError::SectionOutOfBounds;
    }
    const auto expectedPasses = AlignUp(viewsEnd, alignof(PassRecordV1));
    std::size_t passBytes{};
    std::size_t expectedTotal{};
    if (!CheckedMultiply(header.passCount, sizeof(PassRecordV1), passBytes) ||
        !CheckedAdd(expectedPasses, passBytes, expectedTotal) ||
        header.viewsOffset != expectedViews ||
        header.passesOffset != expectedPasses || expectedTotal != bytes.size()) {
        return (header.viewsOffset % alignof(ViewRecordV1) != 0 ||
                header.passesOffset % alignof(PassRecordV1) != 0)
            ? FramePacketError::MisalignedSection
            : FramePacketError::SectionOutOfBounds;
    }
    if (!std::all_of(bytes.begin() + sizeof(header),
            bytes.begin() + expectedViews,
            [](const std::byte value) { return value == std::byte{0}; }) ||
        !std::all_of(bytes.begin() + viewsEnd,
            bytes.begin() + expectedPasses,
            [](const std::byte value) { return value == std::byte{0}; })) {
        return FramePacketError::NonZeroPadding;
    }
    if (trace::Crc32(bytes.subspan(sizeof(header))) !=
        header.payloadCrc32) {
        return FramePacketError::ChecksumMismatch;
    }
    try {
        FramePacket candidate{};
        candidate.header = header;
        candidate.views.resize(header.viewCount);
        candidate.passes.resize(header.passCount);
        std::memcpy(candidate.views.data(), bytes.data() + expectedViews,
            viewBytes);
        if (passBytes != 0) {
            std::memcpy(candidate.passes.data(),
                bytes.data() + expectedPasses, passBytes);
        }
        const auto validation = ValidatePacket(candidate);
        if (validation != FramePacketError::None) return validation;
        packet = std::move(candidate);
        return FramePacketError::None;
    } catch (...) {
        packet = {};
        return FramePacketError::AllocationFailure;
    }
}

HistoryUpdate ViewHistoryTracker::Observe(
    const std::uint64_t frameId,
    const ViewRecordV1& view,
    const std::uint32_t explicitCauses) noexcept
{
    HistoryUpdate update{};
    update.epoch = epoch_;
    const auto validation = ValidateView(view);
    if (validation != ViewError::None) {
        update.error = validation;
        return update;
    }
    if (frameId == 0 || (valid_ && frameId <= previousFrameId_)) {
        update.error = ViewError::StaleFrame;
        return update;
    }
    std::uint32_t causes = explicitCauses;
    if (!valid_) {
        causes |= DiscontinuityFirstObservation;
    } else {
        if (frameId != previousFrameId_ + 1) {
            causes |= DiscontinuitySkippedFrame;
        }
        if (view.viewId != previous_.viewId ||
            view.cameraId != previous_.cameraId ||
            view.handedness != previous_.handedness) {
            causes |= DiscontinuityViewIdentity;
        }
        if (view.projectionMode != previous_.projectionMode ||
            !ScalarNear(view.verticalFovRadians,
                previous_.verticalFovRadians)) {
            causes |= DiscontinuityProjection;
        }
        if (!ScalarNear(view.nearPlane, previous_.nearPlane) ||
            !ScalarNear(view.farPlane, previous_.farPlane)) {
            causes |= DiscontinuityClipPlanes;
        }
        if (view.outputWidth != previous_.outputWidth ||
            view.outputHeight != previous_.outputHeight ||
            !ScalarNear(view.viewport.width, previous_.viewport.width) ||
            !ScalarNear(view.viewport.height, previous_.viewport.height)) {
            causes |= DiscontinuityExtent;
        }
        if (!ScalarNear(view.renderScale, previous_.renderScale)) {
            causes |= DiscontinuityRenderScale;
        }
        if (view.aaMode != previous_.aaMode) {
            causes |= DiscontinuityAaMode;
        }
        constexpr auto specialMask = ViewFirstPerson | ViewSpecialProjection;
        if ((view.flags & specialMask) != (previous_.flags & specialMask) ||
            view.renderMode != previous_.renderMode ||
            view.targetId != previous_.targetId) {
            causes |= DiscontinuitySpecialView;
        }
        if (causes == DiscontinuityNone) {
            auto previousStateMatches = MatrixNear(
                view.previousViewProjection, previous_.viewProjection);
            for (std::size_t index = 0; index < 3; ++index) {
                previousStateMatches = previousStateMatches && DoubleNear(
                    view.previousCameraRelativeOrigin[index],
                    previous_.cameraRelativeOrigin[index]);
            }
            previousStateMatches = previousStateMatches &&
                ScalarNear(view.previousJitterNdc[0],
                    previous_.jitterNdc[0]) &&
                ScalarNear(view.previousJitterNdc[1],
                    previous_.jitterNdc[1]);
            if (!previousStateMatches) {
                update.error = ViewError::PreviousTransformMismatch;
                return update;
            }
        }
    }
    if (causes != DiscontinuityNone) {
        if (epoch_ == std::numeric_limits<std::uint64_t>::max()) {
            update.error = ViewError::EpochOverflow;
            return update;
        }
        ++epoch_;
        update.reset = true;
    }
    previous_ = view;
    previousFrameId_ = frameId;
    valid_ = true;
    update.epoch = epoch_;
    update.resetCauses = causes;
    return update;
}

std::uint64_t ViewHistoryTracker::Epoch() const noexcept
{
    return epoch_;
}

const char* ToString(const ViewError error) noexcept
{
    switch (error) {
    case ViewError::None: return "none";
    case ViewError::NotImplemented: return "not-implemented";
    case ViewError::InvalidIdentity: return "invalid-identity";
    case ViewError::WrongThread: return "wrong-thread";
    case ViewError::StaleFrame: return "stale-frame";
    case ViewError::NonFinite: return "non-finite";
    case ViewError::SingularMatrix: return "singular-matrix";
    case ViewError::InconsistentMatrix: return "inconsistent-matrix";
    case ViewError::InvalidProjection: return "invalid-projection";
    case ViewError::InvalidClipPlanes: return "invalid-clip-planes";
    case ViewError::InvalidViewport: return "invalid-viewport";
    case ViewError::InvalidScissor: return "invalid-scissor";
    case ViewError::InvalidFlags: return "invalid-flags";
    case ViewError::PreviousTransformMismatch:
        return "previous-transform-mismatch";
    case ViewError::EpochOverflow: return "epoch-overflow";
    }
    return "unknown";
}

const char* ToString(const FramePacketError error) noexcept
{
    switch (error) {
    case FramePacketError::None: return "none";
    case FramePacketError::NotImplemented: return "not-implemented";
    case FramePacketError::TruncatedHeader: return "truncated-header";
    case FramePacketError::BadMagic: return "bad-magic";
    case FramePacketError::UnsupportedVersion: return "unsupported-version";
    case FramePacketError::WrongEndian: return "wrong-endian";
    case FramePacketError::SizeMismatch: return "size-mismatch";
    case FramePacketError::ChecksumMismatch: return "checksum-mismatch";
    case FramePacketError::SectionOutOfBounds: return "section-out-of-bounds";
    case FramePacketError::MisalignedSection: return "misaligned-section";
    case FramePacketError::NonZeroPadding: return "non-zero-padding";
    case FramePacketError::WrongThread: return "wrong-thread";
    case FramePacketError::InvalidView: return "invalid-view";
    case FramePacketError::InvalidPass: return "invalid-pass";
    case FramePacketError::NonMonotonicPass: return "non-monotonic-pass";
    case FramePacketError::AllocationFailure: return "allocation-failure";
    }
    return "unknown";
}


float ViewOrientationSign(const Matrix4& viewProjection) noexcept
{
    // Orientation is decided by the linear part. The determinant of the
    // upper-left 3x3 is negative exactly when the transform reverses
    // handedness, and therefore reverses the winding of every triangle.
    const auto& m = viewProjection.elements;
    const auto determinant =
        static_cast<double>(m[0]) *
            (static_cast<double>(m[5]) * m[10] -
             static_cast<double>(m[6]) * m[9]) -
        static_cast<double>(m[1]) *
            (static_cast<double>(m[4]) * m[10] -
             static_cast<double>(m[6]) * m[8]) +
        static_cast<double>(m[2]) *
            (static_cast<double>(m[4]) * m[9] -
             static_cast<double>(m[5]) * m[8]);
    // Zero is degenerate rather than flipped. Reporting +1 keeps a collapsed
    // matrix from silently inverting every model in the frame.
    if (!std::isfinite(determinant) || determinant == 0.0) return 1.0f;
    return determinant < 0.0 ? -1.0f : 1.0f;
}

float ViewOrientationSign(
    const Matrix4& view,
    const Matrix4& projection) noexcept
{
    // Row-major with row vectors, so a point is transformed as v * view * proj
    // and the combined linear part is the product in that order.
    float combined[16]{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += view.elements[row * 4 + k] *
                    projection.elements[k * 4 + column];
            }
            combined[row * 4 + column] = sum;
        }
    }
    // Delegated so both overloads decide orientation by exactly one rule.
    Matrix4 product{};
    std::copy(std::begin(combined), std::end(combined),
        std::begin(product.elements));
    return ViewOrientationSign(product);
}
}
