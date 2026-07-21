#pragma once

#include "renderer_core/EngineView.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::camera {

// The engine's camera block is read as opaque bytes and the matrices are
// identified by the one relationship that cannot happen by chance:
// viewProjection == projection * view. Nothing here depends on a guessed
// struct offset, so a build that moves its fields is still readable and a
// block that holds no camera fails closed.
inline constexpr std::size_t kMatrixBytes = 64;
// A safety ceiling on allocation, not a scan limit. It must never truncate a
// real record: the engine's state block is dense, so a small budget is spent
// on leading scalars long before the matrices are reached and the scan then
// reports "no camera" for a record that plainly holds one. Live measurement
// put the camera at +0x1B0 of a 0x3C0 window, past a 96-candidate budget.
inline constexpr std::uint32_t kMaximumCandidates = 4096;
inline constexpr float kDefaultResidualTolerance = 1.0e-3f;

enum class CameraOriginError : std::uint8_t
{
    None,
    // The candidate's linear part is not the same matrix, so it describes a
    // different camera and solving it for a position would produce a number
    // that is wrong without looking wrong.
    ProjectionMismatch,
    // Fewer than three independent directions, so the position is not
    // determined and any answer would be one arbitrary member of a family.
    Singular,
    NonFinite,
};

// How far a candidate's linear part may differ from the camera-relative one
// and still be treated as the same camera. The columns come from the same
// engine matrix and differ only by single-precision rounding through the
// engine's own multiply, so this is a rounding allowance rather than a fit.
inline constexpr float kOriginColumnTolerance = 1.0e-3f;

// Recovers the camera's world position from a view-projection that carries a
// translation, given the camera-relative one that does not.
//
// Fallout 4 publishes per-object transforms in absolute world coordinates and
// a view matrix whose fourth column is zero. Those cannot both feed one
// shader, and a renderer handed both draws the world from the world's origin
// rather than from the camera. The two differ by exactly one translation, so
// the position is recoverable rather than something to be estimated: the
// candidate must equal the camera-relative matrix times a translation of minus
// the position, which fixes the first three columns and leaves four equations
// in the three unknowns of the fourth. Overdetermined on purpose -- the extra
// equation is what makes `residual` able to reject a matrix that merely has a
// plausible fourth column.
[[nodiscard]] CameraOriginError RecoverCameraOrigin(
    const view::Matrix4& cameraRelativeViewProjection,
    const view::Matrix4& candidate,
    std::array<double, 3>& origin,
    float& residual) noexcept;

enum class OriginSelectionError : std::uint8_t
{
    None,
    NoCandidates,
    NoGeometry,
    // No candidate had geometry close enough to be believed. Refused rather
    // than approximated: the least bad candidate would place the whole cell
    // wrong by however far it was out, and the frame would then look rendered
    // instead of looking broken.
    NoneCredible,
};

struct OriginSelection
{
    std::array<double, 3> origin{};
    float nearestDistance{};
    std::uint32_t candidateIndex{};
    // How many instances sat within the neighbourhood radius of the chosen
    // candidate. This is what the choice is made on.
    std::uint32_t neighbours{};
};

// Picks the camera's world position out of a set of candidate triples, using
// the frame's own geometry to say which one it is.
//
// The engine's camera record holds the position, and nothing in the bytes
// marks it: many triples in the record are plausible coordinates. What singles
// the real one out is not its value but its relationship to the frame. The
// loaded cell is built around the player, so the camera is where the geometry
// is densest, and the count of instances within `neighbourhoodRadius` is what
// the candidates are ranked by.
//
// Ranking by the single nearest instance instead was tried and is wrong. The
// record is full of zeroes and a frame contains a few identity-placed quads at
// the origin, so a candidate of all zeroes scores a perfect nearest distance
// and beats the true camera; measured live, that is exactly what it chose.
// Density cannot be fooled that way -- the handful of quads at the origin
// cannot outnumber the cell around the player.
//
// `minimumNeighbours` is what makes the answer refusable. Below it nothing is
// returned, because an approximate origin would misplace the whole cell by
// however far it was out and the frame would look rendered rather than look
// broken. The first of equally dense candidates wins, so an origin cannot
// alternate between two answers and shift the cell every frame.
[[nodiscard]] OriginSelectionError SelectCameraOrigin(
    std::span<const std::array<float, 3>> candidates,
    std::span<const std::array<float, 3>> instanceTranslations,
    float neighbourhoodRadius,
    std::uint32_t minimumNeighbours,
    OriginSelection& selection) noexcept;

enum class CameraError : std::uint8_t
{
    None,
    NoCameraFound,
    InvalidExtent,
    InvalidClipPlanes,
    InvalidIdentity,
    TranslationRejected,
    EncodeFailed,
};

// These are CPU-side diagnostic records, never serialized, so the padding
// the 16-byte aligned matrices introduce is intended rather than a layout
// accident worth reshaping the types for.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

struct CameraScanResult
{
    bool found{};
    // Which engine record this camera was read out of: 0 is the state
    // record's embedded camera, and 1..N are entries of the camera-state
    // cache array. The scanner never sets it; the caller that chose the
    // record does. Identity has to include it, because every cached record
    // holds its camera at the same offset and would otherwise collide.
    std::uint32_t sourceSlot{};
    std::uint32_t viewOffset{};
    std::uint32_t projectionOffset{};
    std::uint32_t viewProjectionOffset{};
    std::uint32_t candidateCount{};
    // The camera's world position, recovered from a translated
    // view-projection found in the same record. False when the record held
    // only the camera-relative form, which is not a scan failure: the caller
    // keeps whatever origin it already had rather than being told no camera
    // was found at all.
    bool originFound{};
    std::uint32_t originOffset{};
    float originResidual{};
    std::array<double, 3> cameraOrigin{};
    // Every three-float triple in the record that could be a world position, at
    // four-byte steps. Which one it is cannot be decided from the bytes, so
    // the scan collects and the caller -- which has the frame's geometry --
    // decides, through SelectCameraOrigin.
    std::vector<std::array<float, 3>> originCandidates;
    view::MatrixStorage storage{view::MatrixStorage::RowMajor};
    view::Matrix4 view{};
    view::Matrix4 projection{};
    view::Matrix4 viewProjection{};
    float residual{};
};

struct CameraObservation
{
    CameraScanResult scan{};
    std::uint32_t outputWidth{};
    std::uint32_t outputHeight{};
    float nearPlane{};
    float farPlane{};
    std::uint64_t frameId{};
    std::uint64_t engineFrameId{};
    std::uint64_t captureSequence{};
    std::uint32_t threadId{};
    std::uint64_t viewId{};
    std::uint64_t cameraId{};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// The engine keeps several cameras live at once, so a capture that reports
// only the best match cannot distinguish a first-person or shadow camera
// from the main world view.
struct CameraSeries
{
    std::vector<CameraScanResult> cameras;
    std::uint32_t outputWidth{};
    std::uint32_t outputHeight{};
    std::uint64_t frameId{};
    std::uint64_t engineFrameId{};
    std::uint64_t captureSequence{};
    std::uint32_t threadId{};
};

[[nodiscard]] CameraScanResult ScanCameraState(
    std::span<const std::byte> block,
    float tolerance = kDefaultResidualTolerance) noexcept;
[[nodiscard]] std::vector<CameraScanResult> ScanCameraStates(
    std::span<const std::byte> block,
    float tolerance = kDefaultResidualTolerance,
    std::size_t maximumCameras = view::kMaximumViews);
[[nodiscard]] CameraError BuildFrameSeries(
    const CameraSeries& series,
    view::FramePacket& packet) noexcept;
[[nodiscard]] CameraError BuildFramePacket(
    const CameraObservation& observation,
    view::FramePacket& packet) noexcept;
[[nodiscard]] const char* ToString(CameraError error) noexcept;

}
