#include "renderer_core/CameraStateScan.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace vf::renderer::camera {

CameraOriginError RecoverCameraOrigin(
    const view::Matrix4& cameraRelativeViewProjection,
    const view::Matrix4& candidate,
    std::array<double, 3>& origin,
    float& residual) noexcept
{
    origin = {};
    residual = 0.0f;
    for (std::size_t index = 0; index < 16; ++index) {
        if (!std::isfinite(cameraRelativeViewProjection.elements[index]) ||
            !std::isfinite(candidate.elements[index])) {
            return CameraOriginError::NonFinite;
        }
    }
    // Only the fourth column may differ: a translation applied on the right
    // leaves the linear part untouched. Checking it first means a candidate
    // from another camera is refused rather than fitted, and fitting one would
    // hand back a position that is wrong without being obviously wrong.
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            const auto index = row * 4 + column;
            if (std::abs(cameraRelativeViewProjection.elements[index] -
                    candidate.elements[index]) > kOriginColumnTolerance) {
                return CameraOriginError::ProjectionMismatch;
            }
        }
    }

    // Four equations, three unknowns. Row r of the candidate's fourth column
    // is the camera-relative one minus that row's linear part dotted with the
    // position, so the system is A * origin = b with A the negated linear part
    // and b the difference of the fourth columns. Accumulated in double: the
    // positions reach six figures and the normal equations square them.
    double normal[3][3]{};
    double projected[3]{};
    for (std::size_t row = 0; row < 4; ++row) {
        double linear[3]{};
        for (std::size_t column = 0; column < 3; ++column) {
            linear[column] = -static_cast<double>(
                cameraRelativeViewProjection.elements[row * 4 + column]);
        }
        const auto difference =
            static_cast<double>(candidate.elements[row * 4 + 3]) -
            static_cast<double>(
                cameraRelativeViewProjection.elements[row * 4 + 3]);
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                normal[i][j] += linear[i] * linear[j];
            }
            projected[i] += linear[i] * difference;
        }
    }

    // Gaussian elimination with partial pivoting on the three-by-three normal
    // matrix. A rank-deficient linear part leaves a vanishing pivot, and that
    // is refused rather than solved: without three independent directions the
    // position is a family, not a value.
    double augmented[3][4]{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            augmented[row][column] = normal[row][column];
        }
        augmented[row][3] = projected[row];
    }
    // Scaled against the largest entry so the threshold means the same thing
    // whether the matrix came from a metre-scale camera or a world-scale one.
    auto scale = 0.0;
    for (const auto& row : normal) {
        for (const auto value : row) scale = std::max(scale, std::abs(value));
    }
    if (!(scale > 0.0)) return CameraOriginError::Singular;
    for (std::size_t pivot = 0; pivot < 3; ++pivot) {
        auto best = pivot;
        for (std::size_t row = pivot + 1; row < 3; ++row) {
            if (std::abs(augmented[row][pivot]) >
                std::abs(augmented[best][pivot])) {
                best = row;
            }
        }
        if (std::abs(augmented[best][pivot]) <= scale * 1.0e-12) {
            return CameraOriginError::Singular;
        }
        if (best != pivot) {
            for (std::size_t column = 0; column < 4; ++column) {
                std::swap(augmented[pivot][column], augmented[best][column]);
            }
        }
        for (std::size_t row = 0; row < 3; ++row) {
            if (row == pivot) continue;
            const auto factor =
                augmented[row][pivot] / augmented[pivot][pivot];
            for (std::size_t column = pivot; column < 4; ++column) {
                augmented[row][column] -= factor * augmented[pivot][column];
            }
        }
    }
    std::array<double, 3> solved{};
    for (std::size_t row = 0; row < 3; ++row) {
        solved[row] = augmented[row][3] / augmented[row][row];
        if (!std::isfinite(solved[row])) return CameraOriginError::NonFinite;
    }

    // The residual over all four equations, which is what the extra one is
    // for. A candidate that is genuinely this matrix translated satisfies
    // every row at once; one that merely has a plausible fourth column cannot,
    // and without the fourth row there would be nothing to tell them apart.
    auto worst = 0.0;
    for (std::size_t row = 0; row < 4; ++row) {
        auto predicted = 0.0;
        for (std::size_t column = 0; column < 3; ++column) {
            predicted -= static_cast<double>(
                cameraRelativeViewProjection.elements[row * 4 + column]) *
                solved[column];
        }
        const auto difference =
            static_cast<double>(candidate.elements[row * 4 + 3]) -
            static_cast<double>(
                cameraRelativeViewProjection.elements[row * 4 + 3]);
        worst = std::max(worst, std::abs(predicted - difference));
    }
    origin = solved;
    residual = static_cast<float>(worst);
    return CameraOriginError::None;
}

OriginSelectionError SelectCameraOrigin(
    const std::span<const std::array<float, 3>> candidates,
    const std::span<const std::array<float, 3>> instanceTranslations,
    const float neighbourhoodRadius,
    const std::uint32_t minimumNeighbours,
    OriginSelection& selection) noexcept
{
    selection = {};
    if (candidates.empty()) return OriginSelectionError::NoCandidates;
    if (instanceTranslations.empty()) return OriginSelectionError::NoGeometry;

    const auto radiusSquared = static_cast<double>(neighbourhoodRadius) *
        static_cast<double>(neighbourhoodRadius);
    std::uint32_t bestNeighbours = 0;
    auto bestDistance = std::numeric_limits<double>::infinity();
    auto found = false;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        // A non-finite candidate needs no test of its own. Its distances come
        // out NaN, and the comparison below is written so that a NaN fails it:
        // that is why it asks whether the new distance is less rather than
        // whether the old one is not. An explicit guard here would be a branch
        // no input can reach, which is worse than nothing because it reads as
        // the thing keeping NaNs out when the comparison is.
        const auto& candidate = candidates[index];
        auto nearest = std::numeric_limits<double>::infinity();
        std::uint32_t neighbours = 0;
        for (const auto& translation : instanceTranslations) {
            auto squared = 0.0;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const auto delta = static_cast<double>(translation[axis]) -
                    static_cast<double>(candidate[axis]);
                squared += delta * delta;
            }
            // A NaN anywhere -- in the candidate or the translation -- fails
            // both of these, so neither needs a guard of its own and neither
            // can be counted or become the nearest.
            if (squared <= radiusSquared) ++neighbours;
            if (squared < nearest) nearest = squared;
        }
        // Density first, and the nearest instance only to break a tie between
        // two equally populated candidates. Strictly greater and strictly
        // less, so the first of equal candidates survives and the choice does
        // not depend on iteration order.
        if (neighbours < bestNeighbours) continue;
        if (neighbours == bestNeighbours && !(nearest < bestDistance)) continue;
        bestNeighbours = neighbours;
        bestDistance = nearest;
        found = true;
        selection.candidateIndex = static_cast<std::uint32_t>(index);
        selection.neighbours = neighbours;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            selection.origin[axis] = static_cast<double>(candidate[axis]);
        }
    }
    if (!found || bestNeighbours < minimumNeighbours ||
        !std::isfinite(bestDistance)) {
        selection = {};
        return OriginSelectionError::NoneCredible;
    }
    selection.nearestDistance = static_cast<float>(std::sqrt(bestDistance));
    return OriginSelectionError::None;
}

namespace {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

struct Candidate
{
    std::uint32_t offset{};
    view::Matrix4 rowMajor{};
    view::Matrix4 columnMajor{};
    bool orthonormalBasis{};
    bool projectionShaped{};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

bool Finite(const float* values, const std::size_t count) noexcept
{
    return std::all_of(values, values + count,
        [](const float value) { return std::isfinite(value); });
}

view::Matrix4 Transpose(const view::Matrix4& matrix) noexcept
{
    view::Matrix4 result{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            result.elements[column * 4 + row] =
                matrix.elements[row * 4 + column];
        }
    }
    return result;
}

// A world-to-camera matrix always carries an orthonormal 3x3 rotation, which
// separates it from the projection and from unrelated float noise.
bool OrthonormalBasis(const view::Matrix4& matrix) noexcept
{
    for (std::size_t row = 0; row < 3; ++row) {
        double length = 0.0;
        for (std::size_t column = 0; column < 3; ++column) {
            const auto value = matrix.elements[row * 4 + column];
            length += static_cast<double>(value) * value;
        }
        if (std::abs(std::sqrt(length) - 1.0) > 1.0e-2) return false;
    }
    for (std::size_t first = 0; first < 3; ++first) {
        for (std::size_t second = first + 1; second < 3; ++second) {
            double dot = 0.0;
            for (std::size_t column = 0; column < 3; ++column) {
                dot += static_cast<double>(
                    matrix.elements[first * 4 + column]) *
                    matrix.elements[second * 4 + column];
            }
            if (std::abs(dot) > 1.0e-2) return false;
        }
    }
    return true;
}

// A perspective projection keeps the w row as the depth selector and has no
// translation in it.
bool ProjectionShaped(const view::Matrix4& matrix) noexcept
{
    const auto row3 = &matrix.elements[12];
    const auto selector = std::abs(row3[2]) > 0.5f && std::abs(row3[3]) < 0.5f;
    const auto orthographic =
        std::abs(row3[3] - 1.0f) < 1.0e-3f && std::abs(row3[2]) < 1.0e-3f;
    if (!selector && !orthographic) return false;
    return std::abs(matrix.elements[0]) > 1.0e-4f &&
        std::abs(matrix.elements[5]) > 1.0e-4f;
}

float Residual(
    const view::Matrix4& product,
    const view::Matrix4& expected) noexcept
{
    float worst = 0.0f;
    for (std::size_t index = 0; index < 16; ++index) {
        const auto scale = std::max(1.0f,
            std::abs(expected.elements[index]));
        worst = std::max(worst,
            std::abs(product.elements[index] - expected.elements[index]) /
                scale);
    }
    return worst;
}

}

CameraScanResult ScanCameraState(
    const std::span<const std::byte> block,
    const float tolerance) noexcept
{
    CameraScanResult result{};
    if (block.size() < kMatrixBytes) return result;
    std::vector<Candidate> candidates;
    try {
        // Reserve for what this block can actually yield rather than for the
        // ceiling, so a small window does not pay for a large allocation.
        const auto possible = (block.size() - kMatrixBytes) / 4 + 1;
        candidates.reserve(std::min<std::size_t>(possible,
            kMaximumCandidates));
    } catch (...) {
        return result;
    }
    for (std::size_t offset = 0;
         offset + kMatrixBytes <= block.size() &&
             candidates.size() < kMaximumCandidates;
         offset += 4) {
        float raw[16]{};
        std::memcpy(raw, block.data() + offset, sizeof(raw));
        if (!Finite(raw, 16)) continue;
        const auto nonZero = std::any_of(std::begin(raw), std::end(raw),
            [](const float value) { return value != 0.0f; });
        if (!nonZero) continue;
        Candidate candidate{};
        candidate.offset = static_cast<std::uint32_t>(offset);
        std::copy(std::begin(raw), std::end(raw),
            std::begin(candidate.rowMajor.elements));
        candidate.columnMajor = Transpose(candidate.rowMajor);
        candidate.orthonormalBasis =
            OrthonormalBasis(candidate.rowMajor) ||
            OrthonormalBasis(candidate.columnMajor);
        candidate.projectionShaped =
            ProjectionShaped(candidate.rowMajor) ||
            ProjectionShaped(candidate.columnMajor);
        candidates.push_back(candidate);
    }
    result.candidateCount = static_cast<std::uint32_t>(candidates.size());

    auto best = std::numeric_limits<float>::infinity();
    for (const auto& viewCandidate : candidates) {
        if (!viewCandidate.orthonormalBasis) continue;
        for (const auto& projectionCandidate : candidates) {
            if (projectionCandidate.offset == viewCandidate.offset) continue;
            if (!projectionCandidate.projectionShaped) continue;
            for (const auto& productCandidate : candidates) {
                if (productCandidate.offset == viewCandidate.offset ||
                    productCandidate.offset == projectionCandidate.offset) {
                    continue;
                }
                for (const auto storage : {
                         view::MatrixStorage::RowMajor,
                         view::MatrixStorage::ColumnMajor}) {
                    const auto pick =
                        [storage](const Candidate& candidate) {
                            return storage == view::MatrixStorage::RowMajor
                                ? candidate.rowMajor : candidate.columnMajor;
                        };
                    const auto viewMatrix = pick(viewCandidate);
                    const auto projection = pick(projectionCandidate);
                    if (!OrthonormalBasis(viewMatrix) ||
                        !ProjectionShaped(projection)) {
                        continue;
                    }
                    const auto product = view::Multiply(
                        projection, viewMatrix);
                    const auto residual = Residual(
                        product, pick(productCandidate));
                    if (residual > tolerance || residual >= best) continue;
                    best = residual;
                    result.found = true;
                    result.viewOffset = viewCandidate.offset;
                    result.projectionOffset = projectionCandidate.offset;
                    result.viewProjectionOffset = productCandidate.offset;
                    result.storage = storage;
                    result.view = viewMatrix;
                    result.projection = projection;
                    result.viewProjection = pick(productCandidate);
                    result.residual = residual;
                }
            }
        }
    }

    // Every triple in the record that could be a world position. Fallout 4
    // stores the camera position as three floats rather than as a second
    // matrix -- measured: no candidate in the record is a translation of the
    // camera-relative view-projection -- and nothing in the bytes says which
    // triple it is. Bounded by the coordinate range a Fallout 4 worldspace can
    // actually reach, which throws out the great majority of the record
    // without ever having to guess at meaning.
    if (result.found) {
        try {
            constexpr float kWorldBound = 4.0e6f;
            for (std::size_t offset = 0; offset + 12 <= block.size();
                 offset += 4) {
                float raw[3]{};
                std::memcpy(raw, block.data() + offset, sizeof(raw));
                if (!Finite(raw, 3)) continue;
                if (std::any_of(std::begin(raw), std::end(raw),
                        [](const float value) {
                            return std::abs(value) > kWorldBound;
                        })) {
                    continue;
                }
                result.originCandidates.push_back({raw[0], raw[1], raw[2]});
            }
        } catch (...) {
            result.originCandidates.clear();
        }
    }

    // With the camera-relative triple settled, look through the same
    // candidates for the translated view-projection that sits beside it. The
    // engine publishes both: per-object transforms are in absolute world
    // coordinates, and a view-projection whose fourth column is zero cannot
    // be what places them, so the matrix that does must also be in this
    // record. Recovering the position from it is what lets a renderer put the
    // world where the camera actually is instead of at the world's origin.
    //
    // Not fatal when nothing matches. A record that genuinely holds only the
    // camera-relative form leaves `originFound` false and the caller keeps
    // whatever it had, rather than a scan that found a camera reporting that
    // it found none.
    if (result.found) {
        auto bestOrigin = std::numeric_limits<float>::infinity();
        for (const auto& candidate : candidates) {
            if (candidate.offset == result.viewProjectionOffset) continue;
            const auto matrix = result.storage == view::MatrixStorage::RowMajor
                ? candidate.rowMajor : candidate.columnMajor;
            std::array<double, 3> origin{};
            float originResidual = 0.0f;
            if (RecoverCameraOrigin(result.viewProjection, matrix, origin,
                    originResidual) != CameraOriginError::None) {
                continue;
            }
            // A candidate identical to the camera-relative matrix recovers the
            // zero it already had. Taking it would report a world origin of
            // zero as a successful recovery, which is the exact state this
            // exists to escape.
            const auto magnitude = std::sqrt(origin[0] * origin[0] +
                origin[1] * origin[1] + origin[2] * origin[2]);
            if (!(magnitude > 1.0)) continue;
            if (originResidual >= bestOrigin) continue;
            bestOrigin = originResidual;
            result.originFound = true;
            result.cameraOrigin = origin;
            result.originOffset = candidate.offset;
            result.originResidual = originResidual;
        }
    }
    return result;
}

std::vector<CameraScanResult> ScanCameraStates(
    const std::span<const std::byte> block,
    const float tolerance,
    const std::size_t maximumCameras)
{
    std::vector<CameraScanResult> results;
    if (maximumCameras == 0) return results;
    // Each accepted camera masks its own bytes so the next pass reports a
    // genuinely different record rather than a shifted view of the same one.
    std::vector<std::byte> working(block.begin(), block.end());
    for (std::size_t index = 0; index < maximumCameras; ++index) {
        const auto found = ScanCameraState(working, tolerance);
        if (!found.found) break;
        results.push_back(found);
        for (const auto offset : {found.viewOffset,
                 found.projectionOffset, found.viewProjectionOffset}) {
            const auto begin = static_cast<std::size_t>(offset);
            const auto end = std::min(working.size(), begin + kMatrixBytes);
            for (auto cursor = begin; cursor < end; ++cursor) {
                working[cursor] = std::byte{0};
            }
        }
    }
    return results;
}

CameraError BuildFrameSeries(
    const CameraSeries& series,
    view::FramePacket& packet) noexcept
{
    packet = {};
    if (series.cameras.empty()) return CameraError::NoCameraFound;
    if (series.cameras.size() > view::kMaximumViews) {
        return CameraError::NoCameraFound;
    }
    try {
        std::uint64_t sequence = 0;
        for (const auto& camera : series.cameras) {
            CameraObservation observation{};
            observation.scan = camera;
            observation.outputWidth = series.outputWidth;
            observation.outputHeight = series.outputHeight;
            view::ClipPlanes planes{};
            if (view::ExtractClipPlanes(camera.projection,
                    view::ProjectionMode::Perspective,
                    view::Handedness::LeftHanded, planes) !=
                view::ViewError::None) {
                continue;
            }
            observation.nearPlane = planes.nearPlane;
            observation.farPlane = planes.farPlane;
            observation.frameId = series.frameId;
            observation.engineFrameId = series.engineFrameId;
            observation.captureSequence = series.captureSequence;
            observation.threadId = series.threadId;
            // Identity follows the record the camera came from and the
            // offset it was found at, so the same engine record keeps the
            // same identity across frames. The record must be part of it:
            // every cached camera-state record holds its camera at the same
            // offset, so offset alone collides across the whole cache.
            observation.viewId = 0xB000'0000'0000'0000ull |
                (static_cast<std::uint64_t>(camera.sourceSlot) << 32) |
                (static_cast<std::uint64_t>(camera.viewOffset) << 8);
            observation.cameraId = 0xB100'0000'0000'0000ull |
                (static_cast<std::uint64_t>(camera.sourceSlot) << 32) |
                (static_cast<std::uint64_t>(camera.projectionOffset) << 8);
            view::FramePacket single{};
            if (BuildFramePacket(observation, single) !=
                CameraError::None) {
                continue;
            }
            packet.header = single.header;
            packet.views.push_back(single.views.front());
            auto pass = single.passes.front();
            pass.sequence = ++sequence;
            pass.viewId = single.views.front().viewId;
            packet.passes.push_back(pass);
        }
        if (packet.views.empty()) return CameraError::NoCameraFound;
        return CameraError::None;
    } catch (...) {
        packet = {};
        return CameraError::EncodeFailed;
    }
}

CameraError BuildFramePacket(
    const CameraObservation& observation,
    view::FramePacket& packet) noexcept
{
    packet = {};
    if (!observation.scan.found) return CameraError::NoCameraFound;
    if (observation.outputWidth == 0 || observation.outputHeight == 0 ||
        observation.outputWidth > view::kMaximumViewExtent ||
        observation.outputHeight > view::kMaximumViewExtent) {
        return CameraError::InvalidExtent;
    }
    if (!std::isfinite(observation.nearPlane) ||
        !std::isfinite(observation.farPlane) ||
        observation.nearPlane <= 0.0f ||
        observation.farPlane <= observation.nearPlane) {
        return CameraError::InvalidClipPlanes;
    }
    if (observation.frameId == 0 || observation.engineFrameId == 0 ||
        observation.threadId == 0) {
        return CameraError::InvalidIdentity;
    }
    try {
        view::CapturedView captured{};
        captured.viewId = observation.viewId != 0
            ? observation.viewId : 0xB000'0000'0000'0001ull;
        captured.cameraId = observation.cameraId != 0
            ? observation.cameraId : 0xB000'0000'0000'0002ull;
        captured.projectionMode = view::ProjectionMode::Perspective;
        captured.handedness = view::Handedness::LeftHanded;
        captured.flags = view::ViewCameraRelative;
        captured.outputWidth = observation.outputWidth;
        captured.outputHeight = observation.outputHeight;
        captured.renderScale = 1.0f;
        captured.nearPlane = observation.nearPlane;
        captured.farPlane = observation.farPlane;
        // Recovered from the projection rather than assumed, so a changed
        // field of view cannot silently disagree with the matrix.
        const auto focal = observation.scan.projection.elements[5];
        captured.verticalFovRadians = focal > 1.0e-4f
            ? 2.0f * std::atan(1.0f / focal) : 1.0471975512f;
        captured.viewport = {0.0f, 0.0f,
            static_cast<float>(observation.outputWidth),
            static_cast<float>(observation.outputHeight), 0.0f, 1.0f};
        captured.scissor = {0, 0,
            observation.outputWidth, observation.outputHeight};

        const auto assign = [](const view::Matrix4& matrix) {
            view::SourceMatrix4 source{};
            source.storage = view::MatrixStorage::RowMajor;
            source.vectors = view::VectorConvention::ColumnVector;
            std::copy(std::begin(matrix.elements), std::end(matrix.elements),
                std::begin(source.elements));
            return source;
        };
        // The world position the view matrix does not carry. Every consumer
        // downstream already narrows against this field -- lights and terrain
        // do it today -- and leaving it zero while the engine's per-object
        // transforms are absolute is what put the whole cell a hundred and
        // twenty thousand units from the eye.
        if (observation.scan.originFound) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                captured.cameraRelativeOrigin[axis] =
                    observation.scan.cameraOrigin[axis];
            }
        }
        captured.view = assign(observation.scan.view);
        captured.projection = assign(observation.scan.projection);
        captured.previousView = captured.view;
        captured.previousProjection = captured.projection;

        view::ViewRecordV1 translated{};
        if (view::TranslateCapturedView(captured, translated) !=
            view::ViewError::None) {
            return CameraError::TranslationRejected;
        }
        packet.header.frameId = observation.frameId;
        packet.header.engineFrameId = observation.engineFrameId;
        packet.header.historyEpoch = 1;
        packet.header.captureSequence = observation.captureSequence != 0
            ? observation.captureSequence : observation.frameId;
        packet.header.captureThreadId = observation.threadId;
        packet.header.renderThreadId = observation.threadId;
        packet.views.push_back(translated);

        view::PassRecordV1 pass{};
        pass.sequence = 1;
        pass.viewId = translated.viewId;
        pass.domain = view::ShaderDomain::Lighting;
        pass.technique = 0;
        pass.renderMode = 0;
        pass.targetId = 2;
        pass.flags = view::PassWritesWorldTarget;
        pass.category = view::ClassifyPass(
            pass.domain, pass.renderMode, pass.flags);
        packet.passes.push_back(pass);
        return CameraError::None;
    } catch (...) {
        packet = {};
        return CameraError::EncodeFailed;
    }
}

const char* ToString(const CameraError error) noexcept
{
    switch (error) {
    case CameraError::None: return "none";
    case CameraError::NoCameraFound: return "no camera found";
    case CameraError::InvalidExtent: return "invalid extent";
    case CameraError::InvalidClipPlanes: return "invalid clip planes";
    case CameraError::InvalidIdentity: return "invalid identity";
    case CameraError::TranslationRejected: return "translation rejected";
    case CameraError::EncodeFailed: return "encode failed";
    }
    return "unknown";
}

}
