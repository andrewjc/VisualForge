#pragma once

#include "renderer_core/EngineLighting.h"
#include "renderer_core/EngineVisibility.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::accel {

// What a geometry's alpha state means to a ray. Opacity decides which hit
// groups run: an opaque geometry marked non-opaque runs an any-hit shader
// for intersections it will never reject, and a cutout marked opaque loses
// its holes and casts a solid shadow.
enum class GeometryOpacity : std::uint8_t
{
    Opaque = 0,
    AlphaTested = 1,
    Blended = 2,
};

enum class BuildMode : std::uint8_t
{
    Rebuild = 0,
    Update = 1,
};

// Vulkan's instance record carries an 8-bit mask and a 24-bit custom index.
inline constexpr std::uint32_t kInstanceMaskOpaque = 0x01;
inline constexpr std::uint32_t kInstanceMaskAlphaTested = 0x02;
inline constexpr std::uint32_t kInstanceMaskAll = 0xFF;
inline constexpr std::uint32_t kMaximumCustomIndex = (1u << 24) - 1u;
inline constexpr std::uint32_t kMaximumInstances = 262'144;

enum class AccelError : std::uint8_t
{
    None,
    InvalidIdentity,
    InvalidAlignment,
    EmptyGeometry,
    InvalidBounds,
    SingularTransform,
    NonFiniteSource,
    InvalidMask,
    CustomIndexOutOfRange,
    TooManyInstances,
};

struct DeviceLimits
{
    // The live capability report gives 128 for scratch on this device.
    std::uint32_t scratchAlignment{128};
    std::uint32_t structureAlignment{256};
};

struct BuildSizes
{
    std::uint64_t structureBytes{};
    std::uint64_t buildScratchBytes{};
    // Zero for a static structure: it is never refitted, so reserving update
    // scratch for it would be pure waste.
    std::uint64_t updateScratchBytes{};
};

struct Bounds
{
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
};

struct GeometryDescV1
{
    std::uint64_t geometryId{};
    std::uint32_t vertexCount{};
    std::uint32_t indexCount{};
    std::uint32_t firstIndex{};
    std::int32_t vertexOffset{};
    GeometryOpacity opacity{GeometryOpacity::Opaque};
};

struct BlasDescV1
{
    std::uint64_t blasId{};
    std::uint32_t generation{};
    bool dynamic{};
    std::vector<GeometryDescV1> geometries;
    std::array<float, 3> boundsMinimum{};
    std::array<float, 3> boundsMaximum{};
};

// Row-major 3x4 affine, matching Vulkan's VkTransformMatrixKHR.
struct alignas(16) InstanceDescV1
{
    std::uint64_t instanceId{};
    std::uint64_t blasId{};
    std::array<float, 12> transform{};
    std::uint32_t mask{kInstanceMaskOpaque};
    std::uint32_t customIndex{};
    std::uint32_t flags{};
    std::uint32_t reserved0{};
};

inline constexpr std::uint32_t kAccelPacketMagic = 0x53414656u; // "VFAS"
inline constexpr std::uint16_t kAccelPacketVersionMajor = 1;
inline constexpr std::uint16_t kAccelPacketVersionMinor = 0;
inline constexpr std::uint32_t kAccelPacketEndian = 0x01020304u;

enum class AccelPacketError : std::uint8_t
{
    None,
    TruncatedHeader,
    BadMagic,
    UnsupportedVersion,
    WrongEndian,
    SizeMismatch,
    ChecksumMismatch,
    SectionOutOfBounds,
    MisalignedSection,
    NonZeroPadding,
    TooManyInstances,
    InvalidIdentity,
    DuplicateInstance,
    InvalidInstance,
    AllocationFailure,
};

struct alignas(8) AccelPacketHeaderV1
{
    std::uint32_t magic{kAccelPacketMagic};
    std::uint16_t versionMajor{kAccelPacketVersionMajor};
    std::uint16_t versionMinor{kAccelPacketVersionMinor};
    std::uint32_t headerSize{sizeof(AccelPacketHeaderV1)};
    std::uint32_t totalSize{};
    std::uint32_t payloadCrc32{};
    std::uint32_t endianMarker{kAccelPacketEndian};
    std::uint64_t frameId{};
    std::uint64_t viewId{};
    std::uint32_t instanceCount{};
    std::uint32_t instancesOffset{};
};

struct AccelPacket
{
    AccelPacketHeaderV1 header{};
    std::vector<InstanceDescV1> instances;
};

// A structure the schedule has issued. Tracing against a retired handle is a
// use-after-free that validation may not catch, so the handle carries the
// generation that identifies it.
struct AccelHandle
{
    std::uint32_t generation{};

    [[nodiscard]] friend bool operator==(
        const AccelHandle&, const AccelHandle&) = default;
};

// Owns which structures may be traced and when their scratch may be freed.
// Centralised rather than spread across the backend, so "is this traceable"
// has exactly one answer.
class AccelSchedule
{
public:
    [[nodiscard]] AccelHandle BeginBuild(std::uint32_t generation) noexcept;
    void Retire(AccelHandle handle) noexcept;
    void SignalCompleted(
        AccelHandle handle,
        std::uint64_t timelineValue) noexcept;
    [[nodiscard]] bool Traceable(AccelHandle handle) const noexcept;
    // Destruction waits on the timeline: a structure still in flight owns
    // scratch the GPU is reading.
    [[nodiscard]] bool CanDestroy(
        AccelHandle handle,
        std::uint64_t completedTimelineValue) const noexcept;

private:
    struct Entry
    {
        std::uint32_t generation{};
        std::uint64_t completedValue{};
        bool retired{};
        bool completed{};
    };

    std::vector<Entry> entries_;
};

[[nodiscard]] AccelError QueryBuildSizes(
    const BlasDescV1& blas,
    const DeviceLimits& limits,
    BuildSizes& sizes) noexcept;
[[nodiscard]] BuildMode DecideBuild(
    const BlasDescV1& previous,
    const BlasDescV1& next) noexcept;
[[nodiscard]] bool ShouldCompact(const BlasDescV1& blas) noexcept;
[[nodiscard]] AccelError ValidateInstance(
    const InstanceDescV1& instance) noexcept;
[[nodiscard]] float InstanceDeterminant(
    const InstanceDescV1& instance) noexcept;
[[nodiscard]] AccelError TransformBounds(
    const BlasDescV1& blas,
    const InstanceDescV1& instance,
    Bounds& worldBounds) noexcept;
[[nodiscard]] bool RequiresAnyHit(const GeometryDescV1& geometry) noexcept;
[[nodiscard]] bool ParticipatesInShadows(
    const GeometryDescV1& geometry) noexcept;
// Reuses visibility::EvaluateCoverage rather than writing a second alpha
// test. Two rules would drift, and a shadow silhouette disagreeing with the
// surface silhouette is the artefact this phase exists to avoid.
[[nodiscard]] bool ConfirmAlphaCandidate(
    const visibility::AlphaStateV1& alpha,
    float sampledAlpha,
    std::uint32_t pixelX,
    std::uint32_t pixelY) noexcept;
// Offsets along the *geometric* normal: a shading normal can point into the
// surface and would push the origin below it. The offset scales with
// distance because float spacing does.
[[nodiscard]] std::array<float, 3> OffsetRayOrigin(
    const std::array<float, 3>& position,
    const std::array<float, 3>& geometricNormal,
    float scale) noexcept;

#if defined(_MSC_VER)
#pragma warning(push)
// Host-side geometry, not a wire record: the padding is the compiler
// honouring the embedded alpha state's alignment and costs nothing here.
#pragma warning(disable : 4324)
#endif

// One occluder triangle in camera-relative space, plus what the ray needs to
// decide whether it actually blocks light.
struct ShadowTriangle
{
    std::array<float, 3> a{};
    std::array<float, 3> b{};
    std::array<float, 3> c{};
    GeometryOpacity opacity{GeometryOpacity::Opaque};
    // Consulted only for an alpha-tested triangle, through the Phase 15
    // coverage rule rather than a second alpha test.
    visibility::AlphaStateV1 alpha{};
    std::array<float, 3> alphaAtVertex{1.0f, 1.0f, 1.0f};
};

struct ShadowRay
{
    std::array<float, 3> origin{};
    std::array<float, 3> direction{};
    float minimumDistance{};
    float maximumDistance{};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// A directional light has no position, so its shadow ray must reach past
// anything the scene can hold. The measured world camera reports a far plane
// near 353,468 units, so this clears the whole visible world without being
// infinite, which a builder cannot bound.
inline constexpr float kDirectionalShadowDistance = 1.0e6f;

// One rule for the ray a light casts. The GPU ray query and the CPU oracle
// read the same light record, so if they disagreed about the origin offset,
// the direction, or how far the ray reaches, the shadow the mirror draws
// would not be the shadow the comparison predicts. Mirrored branch for
// branch by `vfShadowRayForLight` in phase17/lighting.glsl.
[[nodiscard]] ShadowRay ShadowRayForLight(
    const lighting::GpuLightRecordV1& light,
    const std::array<float, 3>& position,
    const std::array<float, 3>& geometricNormal) noexcept;

struct ShadowResult
{
    bool occluded{};
    float distance{};
    std::uint32_t testedTriangles{};
    std::uint32_t alphaCandidates{};
};

// The CPU oracle the GPU ray query is compared against. Möller-Trumbore, with
// alpha-tested candidates confirmed through the same coverage function the
// raster pass uses, so a shadow silhouette cannot disagree with the surface
// silhouette that produced it.
[[nodiscard]] ShadowResult TraceShadowRay(
    std::span<const ShadowTriangle> triangles,
    const ShadowRay& ray,
    std::uint32_t pixelX,
    std::uint32_t pixelY) noexcept;

// True from this phase on. Phase 17 declared it unavailable so parity could
// mask it; supplying it means the mask must come off, or a broken shadow
// would be hidden from every comparison that follows.
[[nodiscard]] bool ShadowTermAvailable() noexcept;
[[nodiscard]] bool ShadowMaskRequired() noexcept;

[[nodiscard]] AccelPacketError ValidateAccelPacket(
    const AccelPacket& packet) noexcept;
[[nodiscard]] AccelPacketError EncodeAccelPacket(
    const AccelPacket& packet,
    std::vector<std::byte>& bytes) noexcept;
[[nodiscard]] AccelPacketError DecodeAccelPacket(
    std::span<const std::byte> bytes,
    AccelPacket& packet) noexcept;
[[nodiscard]] const char* ToString(AccelError error) noexcept;
[[nodiscard]] const char* ToString(AccelPacketError error) noexcept;

static_assert(sizeof(InstanceDescV1) == 80);
static_assert(sizeof(AccelPacketHeaderV1) == 48);

}
