#pragma once

#include "ViewShaderLayout.generated.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::view {

inline constexpr std::uint32_t kFramePacketMagic = 0x57564656u; // "VFVW"
inline constexpr std::uint16_t kFramePacketVersionMajor = 1;
inline constexpr std::uint16_t kFramePacketVersionMinor = 0;
inline constexpr std::uint32_t kFramePacketEndian = 0x01020304u;
inline constexpr std::uint32_t kMaximumViews = 16;
inline constexpr std::uint32_t kMaximumPasses = 65'536;
inline constexpr std::uint32_t kMaximumViewExtent = 16'384;

enum class MatrixStorage : std::uint32_t
{
    RowMajor,
    ColumnMajor,
};

enum class VectorConvention : std::uint32_t
{
    ColumnVector,
    RowVector,
};

enum class Handedness : std::uint32_t
{
    LeftHanded,
    RightHanded,
};

enum class ProjectionMode : std::uint32_t
{
    Perspective,
    Orthographic,
};

enum ViewFlag : std::uint32_t
{
    ViewCameraRelative = 1u << 0,
    ViewUsesJitter = 1u << 1,
    ViewFirstPerson = 1u << 2,
    ViewSpecialProjection = 1u << 3,
};

inline constexpr std::uint32_t kKnownViewFlags =
    ViewCameraRelative | ViewUsesJitter | ViewFirstPerson |
    ViewSpecialProjection;

enum DiscontinuityCause : std::uint32_t
{
    DiscontinuityNone = 0,
    DiscontinuityFirstObservation = 1u << 0,
    DiscontinuityExplicitCut = 1u << 1,
    DiscontinuityTeleport = 1u << 2,
    DiscontinuityLoad = 1u << 3,
    DiscontinuityWorldspace = 1u << 4,
    DiscontinuityTime = 1u << 5,
    DiscontinuityBridge = 1u << 6,
    DiscontinuityFault = 1u << 7,
    DiscontinuitySkippedFrame = 1u << 8,
    DiscontinuityViewIdentity = 1u << 9,
    DiscontinuityProjection = 1u << 10,
    DiscontinuityClipPlanes = 1u << 11,
    DiscontinuityExtent = 1u << 12,
    DiscontinuityRenderScale = 1u << 13,
    DiscontinuityAaMode = 1u << 14,
    DiscontinuitySpecialView = 1u << 15,
};

enum class ShaderDomain : std::uint32_t
{
    Effect = 0,
    Utility = 1,
    DistantTree = 2,
    Particle = 3,
    DeferredPrepass = 4,
    DeferredLight = 5,
    DeferredComposite = 6,
    Sky = 7,
    Lighting = 8,
    BloodSpatter = 9,
    Water = 10,
    FaceCustomization = 11,
    ImageSpace = 12,
};

enum PassFlag : std::uint32_t
{
    PassWritesWorldTarget = 1u << 0,
    PassAlphaTest = 1u << 1,
    PassTransparent = 1u << 2,
    PassTwoSided = 1u << 3,
    PassFirstPerson = 1u << 4,
    PassInterface = 1u << 5,
};

inline constexpr std::uint32_t kKnownPassFlags =
    PassWritesWorldTarget | PassAlphaTest | PassTransparent |
    PassTwoSided | PassFirstPerson | PassInterface;

enum class PassCategory : std::uint32_t
{
    Unknown,
    Opaque,
    AlphaTest,
    Transparent,
    DepthPrepass,
    Shadow,
    LocalMap,
    Occlusion,
    Lod,
    Vats,
    Sky,
    Water,
    ParticleEffect,
    ImageSpace,
    Interface,
};

enum class ViewError : std::uint8_t
{
    None,
    NotImplemented,
    InvalidIdentity,
    WrongThread,
    StaleFrame,
    NonFinite,
    SingularMatrix,
    InconsistentMatrix,
    InvalidProjection,
    InvalidClipPlanes,
    InvalidViewport,
    InvalidScissor,
    InvalidFlags,
    PreviousTransformMismatch,
    EpochOverflow,
};

enum class FramePacketError : std::uint8_t
{
    None,
    NotImplemented,
    TruncatedHeader,
    BadMagic,
    UnsupportedVersion,
    WrongEndian,
    SizeMismatch,
    ChecksumMismatch,
    SectionOutOfBounds,
    MisalignedSection,
    NonZeroPadding,
    WrongThread,
    InvalidView,
    InvalidPass,
    NonMonotonicPass,
    AllocationFailure,
};

struct alignas(16) Matrix4
{
    float elements[16]{};

    friend bool operator==(const Matrix4&, const Matrix4&) = default;
};

struct SourceMatrix4
{
    float elements[16]{};
    MatrixStorage storage{MatrixStorage::RowMajor};
    VectorConvention vectors{VectorConvention::ColumnVector};
};

struct ViewportV1
{
    float x{};
    float y{};
    float width{};
    float height{};
    float minimumDepth{};
    float maximumDepth{1.0f};
};

struct ScissorV1
{
    std::int32_t x{};
    std::int32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct CapturedView
{
    std::uint64_t viewId{};
    std::uint64_t cameraId{};
    ProjectionMode projectionMode{ProjectionMode::Perspective};
    Handedness handedness{Handedness::LeftHanded};
    std::uint32_t flags{};
    std::uint32_t renderMode{};
    std::uint32_t targetId{};
    std::uint32_t outputWidth{};
    std::uint32_t outputHeight{};
    std::uint32_t aaMode{};
    float renderScale{1.0f};
    float nearPlane{0.1f};
    float farPlane{10'000.0f};
    float verticalFovRadians{};
    std::array<float, 2> jitterNdc{};
    std::array<float, 2> previousJitterNdc{};
    std::array<double, 3> cameraRelativeOrigin{};
    std::array<double, 3> previousCameraRelativeOrigin{};
    ViewportV1 viewport{};
    ScissorV1 scissor{};
    SourceMatrix4 view{};
    SourceMatrix4 projection{};
    SourceMatrix4 previousView{};
    SourceMatrix4 previousProjection{};
};

struct alignas(16) ViewRecordV1
{
    std::uint64_t viewId{};
    std::uint64_t cameraId{};
    ProjectionMode projectionMode{ProjectionMode::Perspective};
    Handedness handedness{Handedness::LeftHanded};
    std::uint32_t flags{};
    std::uint32_t renderMode{};
    std::uint32_t targetId{};
    std::uint32_t outputWidth{};
    std::uint32_t outputHeight{};
    std::uint32_t aaMode{};
    float renderScale{1.0f};
    float nearPlane{0.1f};
    float farPlane{10'000.0f};
    float verticalFovRadians{};
    float jitterNdc[2]{};
    float previousJitterNdc[2]{};
    double cameraRelativeOrigin[3]{};
    double previousCameraRelativeOrigin[3]{};
    ViewportV1 viewport{};
    ScissorV1 scissor{};
    std::uint32_t reserved0[2]{};
    Matrix4 view{};
    Matrix4 projection{};
    Matrix4 viewProjection{};
    Matrix4 inverseViewProjection{};
    Matrix4 previousViewProjection{};
    Matrix4 unjitteredViewProjection{};
    Matrix4 previousUnjitteredViewProjection{};
    std::uint32_t reserved1[4]{};
};

struct alignas(8) PassRecordV1
{
    std::uint64_t sequence{};
    std::uint64_t viewId{};
    ShaderDomain domain{ShaderDomain::Lighting};
    PassCategory category{PassCategory::Unknown};
    std::uint32_t technique{};
    std::uint32_t renderMode{};
    std::uint32_t targetId{};
    std::uint32_t flags{};
};

struct alignas(8) FramePacketHeaderV1
{
    std::uint32_t magic{kFramePacketMagic};
    std::uint16_t versionMajor{kFramePacketVersionMajor};
    std::uint16_t versionMinor{kFramePacketVersionMinor};
    std::uint32_t headerSize{sizeof(FramePacketHeaderV1)};
    std::uint32_t totalSize{};
    std::uint32_t payloadCrc32{};
    std::uint32_t endianMarker{kFramePacketEndian};
    std::uint64_t frameId{};
    std::uint64_t engineFrameId{};
    std::uint64_t historyEpoch{};
    std::uint64_t captureSequence{};
    std::uint32_t captureThreadId{};
    std::uint32_t renderThreadId{};
    std::uint32_t flags{};
    std::uint32_t viewCount{};
    std::uint32_t passCount{};
    std::uint32_t viewsOffset{};
    std::uint32_t passesOffset{};
    std::uint32_t reserved{};
    std::uint64_t reserved64{};
};

struct FramePacket
{
    FramePacketHeaderV1 header{};
    std::vector<ViewRecordV1> views;
    std::vector<PassRecordV1> passes;
};

struct ClipPlanes
{
    float nearPlane{};
    float farPlane{};
};

struct ProjectedPoint
{
    float x{};
    float y{};
    float depth{};
    float clipW{};
    bool inside{};
};

struct HistoryUpdate
{
    ViewError error{ViewError::None};
    std::uint64_t epoch{};
    std::uint32_t resetCauses{};
    bool reset{};
};

struct PassCoverage
{
    std::uint32_t classified{};
    std::uint32_t unknown{};
    std::uint32_t unknownWorldWriters{};

    [[nodiscard]] bool TakeoverEligible() const noexcept
    {
        return unknownWorldWriters == 0;
    }
};

class ViewHistoryTracker
{
public:
    [[nodiscard]] HistoryUpdate Observe(
        std::uint64_t frameId,
        const ViewRecordV1& view,
        std::uint32_t explicitCauses = DiscontinuityNone) noexcept;
    [[nodiscard]] std::uint64_t Epoch() const noexcept;

private:
    ViewRecordV1 previous_{};
    std::uint64_t previousFrameId_{};
    std::uint64_t epoch_{};
    bool valid_{};
};

[[nodiscard]] Matrix4 IdentityMatrix() noexcept;
[[nodiscard]] Matrix4 NormalizeSourceMatrix(
    const SourceMatrix4& source) noexcept;
[[nodiscard]] Matrix4 Multiply(
    const Matrix4& left,
    const Matrix4& right) noexcept;
[[nodiscard]] bool Invert(
    const Matrix4& matrix,
    Matrix4& inverse) noexcept;
[[nodiscard]] Matrix4 BuildPerspectiveProjection(
    float verticalFovRadians,
    float aspect,
    float nearPlane,
    float farPlane,
    Handedness handedness,
    std::array<float, 2> jitterNdc = {}) noexcept;
[[nodiscard]] Matrix4 BuildOrthographicProjection(
    float width,
    float height,
    float nearPlane,
    float farPlane,
    Handedness handedness,
    std::array<float, 2> jitterNdc = {}) noexcept;
[[nodiscard]] ViewError ExtractClipPlanes(
    const Matrix4& projection,
    ProjectionMode mode,
    Handedness handedness,
    ClipPlanes& planes) noexcept;
[[nodiscard]] ViewError TranslateCapturedView(
    const CapturedView& captured,
    ViewRecordV1& view) noexcept;
[[nodiscard]] ViewError ValidateView(
    const ViewRecordV1& view) noexcept;
[[nodiscard]] ViewError ProjectWorldPoint(
    const ViewRecordV1& view,
    std::array<double, 3> worldPoint,
    ProjectedPoint& projected) noexcept;
[[nodiscard]] ViewError BuildGpuViewConstants(
    const ViewRecordV1* view,
    std::uint64_t historyEpoch,
    GpuViewConstantsV1& constants) noexcept;
[[nodiscard]] PassCategory ClassifyPass(
    ShaderDomain domain,
    std::uint32_t renderMode,
    std::uint32_t flags) noexcept;
[[nodiscard]] PassCoverage SummarizePassCoverage(
    std::span<const PassRecordV1> passes) noexcept;
[[nodiscard]] FramePacketError EncodeFramePacket(
    const FramePacket& packet,
    std::vector<std::byte>& bytes) noexcept;
[[nodiscard]] FramePacketError DecodeFramePacket(
    std::span<const std::byte> bytes,
    FramePacket& packet) noexcept;
[[nodiscard]] const char* ToString(ViewError error) noexcept;
[[nodiscard]] const char* ToString(FramePacketError error) noexcept;

static_assert(sizeof(Matrix4) == 64);
static_assert(sizeof(ViewportV1) == 24);
static_assert(sizeof(ScissorV1) == 16);
static_assert(sizeof(ViewRecordV1) == 640);
static_assert(offsetof(ViewRecordV1, view) == 176);
static_assert(offsetof(ViewRecordV1, previousViewProjection) == 432);
static_assert(sizeof(PassRecordV1) == 40);
static_assert(sizeof(FramePacketHeaderV1) == 96);


// The sign of the orientation that a view and projection together impose.
//
// Negative means the pair reverses triangle winding. A front face captured
// from the engine is expressed in the engine's own clip space, so a mirror
// that re-projects those triangles through a view which negates an axis must
// invert the declared winding, or it culls the side it meant to keep -- which
// shows as a model rendered inside out, its interior visible where its outer
// shell should be.
//
// Derived from the matrices rather than assumed, so a fixture authored
// directly in packet space and a live capture both get the right answer from
// the same rule.
[[nodiscard]] float ViewOrientationSign(const Matrix4& viewProjection) noexcept;
[[nodiscard]] float ViewOrientationSign(
    const Matrix4& view,
    const Matrix4& projection) noexcept;
}
