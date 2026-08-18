#pragma once

#include "SceneShaderLayout.generated.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::lighting {

// The engine has exactly four concrete NiLight subclasses, reconstructed
// from RTTI and recorded in engine_render.md 9.12a. Shadow casting belongs
// to BSShadowLight, which wraps a light rather than being a fifth type, so
// it is a flag here and not a type. Modelling it as a type would make "a
// point light that casts shadows" inexpressible.
enum class LightType : std::uint8_t
{
    Ambient = 0,
    Directional = 1,
    Point = 2,
    Spot = 3,
};

enum LightFlag : std::uint32_t
{
    LightCastsShadows = 1u << 0,
    LightPortalStrict = 1u << 1,
    LightNeverFades = 1u << 2,
};

enum EnvironmentFlag : std::uint32_t
{
    EnvironmentInterior = 1u << 0,
    // Set only when a frame actually supplied a light packet. Without it a
    // zeroed environment would read as "ambient zero" and black out every
    // surface, rather than as "this frame has no lighting" and leave the
    // albedo alone the way every phase before this one did.
    EnvironmentPresent = 1u << 1,
    // Switches the diffuse bounce off for the frame. Every other ray-traced
    // term can already be rendered with and without on the device, which is
    // what lets a contract difference two frames and attribute the change to
    // one term. Without this the bounce arrives with every other term and
    // none of them can be measured alone.
    EnvironmentIndirectDisabled = 1u << 2,
};

enum class LightError : std::uint8_t
{
    None,
    InvalidIdentity,
    InvalidType,
    InvalidRange,
    InvalidAttenuation,
    InvalidCone,
    InvalidIntensity,
    InvalidDirection,
    InvalidFogRange,
    InvalidTransition,
    IncompatibleEnvironments,
    PositionOutOfRange,
    NonFiniteSource,
    TooManyLights,
};

// The most lights one frame's list can hold. Overflow is deterministic and
// reported rather than silent, because a list that drops different lights
// from frame to frame makes a static scene flicker.
inline constexpr std::uint32_t kMaximumActiveLights = 64;
inline constexpr std::uint32_t kMaximumCapturedLights = 4096;
// Beyond this a camera-relative position cannot be narrowed to float without
// losing whole units, which is the same rule the terrain boundary applies.
inline constexpr double kMaximumCameraRelativeDistance = 1.0e7;

struct FogCapture
{
    float nearDistance{};
    float farDistance{};
    std::array<float, 3> color{};
    float power{1.0f};
    float maximum{1.0f};
};

struct LightCapture
{
    std::uint64_t lightId{};
    LightType type{LightType::Point};
    bool castsShadows{};
    bool portalStrict{};
    bool neverFades{};
    std::array<float, 3> diffuse{1.0f, 1.0f, 1.0f};
    float dimmer{1.0f};
    float radius{};
    float constantAttenuation{1.0f};
    float linearAttenuation{};
    float quadraticAttenuation{};
    std::array<float, 3> direction{0.0f, 0.0f, -1.0f};
    // World position in double, because exterior coordinates reach the
    // hundreds of thousands and the subtraction has to happen before the
    // narrowing to float.
    std::array<double, 3> position{};
    float innerConeRadians{};
    float outerConeRadians{};
    float spotExponent{1.0f};
};

struct EnvironmentCapture
{
    bool interior{};
    std::array<float, 3> ambient{};
    std::array<float, 3> sunDirection{0.0f, 0.0f, -1.0f};
    std::array<float, 3> sunColor{1.0f, 1.0f, 1.0f};
    float sunIntensity{};
    std::array<float, 3> moonDirection{0.0f, 0.0f, 1.0f};
    std::array<float, 3> moonColor{1.0f, 1.0f, 1.0f};
    float moonIntensity{};
    FogCapture fog{};
};

struct alignas(16) LightRecordV1
{
    std::uint64_t lightId{};
    std::uint8_t type{};
    std::uint8_t reserved0[3]{};
    std::uint32_t flags{};
    float color[4]{};
    float intensity{};
    float radius{};
    float constantAttenuation{};
    float linearAttenuation{};
    float quadraticAttenuation{};
    float innerConeCosine{};
    float outerConeCosine{};
    float spotExponent{};
    double position[3]{};
    float direction[4]{};
    // Explicit rather than implicit tail padding. A record that reaches the
    // wire must have every byte accounted for, or two builds could disagree
    // about bytes neither of them names.
    std::uint64_t reserved1{};
};

struct alignas(16) EnvironmentRecordV1
{
    std::uint32_t flags{};
    float ambient[3]{};
    float sunDirection[4]{};
    float sunColor[3]{};
    float sunIntensity{};
    float moonDirection[4]{};
    float moonColor[3]{};
    float moonIntensity{};
    float fogColor[3]{};
    float fogNear{};
    float fogFar{};
    float fogPower{};
    float fogMaximum{};
    std::uint32_t reserved0{};
};

// The GPU form is camera relative, so its position is float.
struct alignas(16) GpuLightRecordV1
{
    float color[4]{};
    float position[4]{};
    float direction[4]{};
    float attenuation[4]{};
    float cone[4]{};
};

// The GPU environment is packed into seven vec4s so a shader reads whole
// registers. Mirrored in scene_layout.glsl and asserted from reflection.
struct alignas(16) GpuEnvironmentV1
{
    float ambientAndFogNear[4]{};
    float sunDirectionAndFogFar[4]{};
    float sunColorAndIntensity[4]{};
    float moonDirectionAndIntensity[4]{};
    float moonColorAndFogMaximum[4]{};
    float fogColorAndPower[4]{};
    std::uint32_t flagsAndCount[4]{};
};

// `indirectRays` is the per-pixel bounce count the frame declares. It travels
// in the environment's own flags-and-counts vector because that is what the
// vector is for, alongside the active light count.
[[nodiscard]] GpuEnvironmentV1 BuildGpuEnvironment(
    const EnvironmentRecordV1& record,
    std::uint32_t activeLightCount,
    std::uint32_t indirectRays) noexcept;

// Evaluation from the GPU records, mirroring phase17/lighting.glsl branch
// for branch. The reference evaluates these and not the host records,
// because the host records hold world positions while the GPU records hold
// camera-relative ones: comparing the two would be comparing different
// scenes and calling the difference error.
// The GPU record packs the light type into the colour's fourth component,
// which is exactly what `vfLightType` reads in phase17/lighting.glsl. One
// home for the unpacking, so a reader on either side cannot invent its own.
[[nodiscard]] constexpr LightType ClassifyGpuLight(
    const GpuLightRecordV1& light) noexcept
{
    return static_cast<LightType>(
        static_cast<std::uint32_t>(light.color[3] + 0.5f));
}

[[nodiscard]] std::array<float, 3> EvaluateDirectGpu(
    const GpuLightRecordV1& light,
    const std::array<float, 3>& position,
    const std::array<float, 3>& normal) noexcept;
[[nodiscard]] float EvaluateFogGpu(
    const GpuEnvironmentV1& environment,
    float distance) noexcept;
[[nodiscard]] std::array<float, 3> ShadeSurfaceGpu(
    const GpuEnvironmentV1& environment,
    std::span<const GpuLightRecordV1> lights,
    const std::array<float, 3>& albedo,
    const std::array<float, 3>& position,
    const std::array<float, 3>& normal) noexcept;
// `shadow` holds one visibility term per light, in light order. A term is
// applied only to a light that is cast from somewhere: ambient has no ray
// that could be blocked, and shadowing it would black out the interior of
// every shadow instead of leaving the ambient floor the engine shows. A span
// shorter than the light list leaves the remaining lights lit, which is what
// a device without ray query supplies.
[[nodiscard]] std::array<float, 3> ShadeSurfaceGpu(
    const GpuEnvironmentV1& environment,
    std::span<const GpuLightRecordV1> lights,
    const std::array<float, 3>& albedo,
    const std::array<float, 3>& position,
    const std::array<float, 3>& normal,
    std::span<const float> shadow) noexcept;

struct SurfacePoint
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{0.0f, 0.0f, 1.0f};
};

// `available` is false for the whole of this phase. The shadow term does not
// exist yet, and saying so lets a parity metric mask it instead of comparing
// a lit mirror against a shadowed vanilla frame and calling the difference
// error.
struct DirectLighting
{
    std::array<float, 3> radiance{};
    bool shadowed{};
    bool available{};
};

struct LightSet
{
    std::vector<LightRecordV1> lights;
};

struct LightSelection
{
    std::vector<std::uint64_t> selected;
    std::uint32_t droppedCount{};
    bool overflowed{};
};

inline constexpr std::uint32_t kLightPacketMagic = 0x4C464656u; // "VFFL"
inline constexpr std::uint16_t kLightPacketVersionMajor = 1;
inline constexpr std::uint16_t kLightPacketVersionMinor = 0;
inline constexpr std::uint32_t kLightPacketEndian = 0x01020304u;

enum class LightPacketError : std::uint8_t
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
    TooManyLights,
    InvalidIdentity,
    DuplicateLight,
    InvalidLight,
    InvalidEnvironment,
    AllocationFailure,
};

struct alignas(8) LightPacketHeaderV1
{
    std::uint32_t magic{kLightPacketMagic};
    std::uint16_t versionMajor{kLightPacketVersionMajor};
    std::uint16_t versionMinor{kLightPacketVersionMinor};
    std::uint32_t headerSize{sizeof(LightPacketHeaderV1)};
    std::uint32_t totalSize{};
    std::uint32_t payloadCrc32{};
    std::uint32_t endianMarker{kLightPacketEndian};
    std::uint64_t frameId{};
    std::uint64_t viewId{};
    std::uint32_t lightCount{};
    std::uint32_t lightsOffset{};
    std::uint32_t environmentOffset{};
    std::uint32_t reserved0{};
    std::uint64_t reserved1{};
};

#if defined(_MSC_VER)
#pragma warning(push)
// A host-side container, not a wire record: the tail padding after the
// vector is the compiler honouring the environment record's alignment and
// costs nothing, because this struct is never serialized as-is.
#pragma warning(disable : 4324)
#endif

struct LightPacket
{
    LightPacketHeaderV1 header{};
    std::vector<LightRecordV1> lights;
    EnvironmentRecordV1 environment{};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// False for the whole of Phase 17; ray-traced visibility arrives in Phase 18.
[[nodiscard]] bool ShadowTermAvailable() noexcept;

[[nodiscard]] LightError TranslateLight(
    const LightCapture& capture,
    LightRecordV1& record) noexcept;
[[nodiscard]] LightError TranslateEnvironment(
    const EnvironmentCapture& capture,
    EnvironmentRecordV1& record) noexcept;
[[nodiscard]] LightError BlendEnvironment(
    const EnvironmentRecordV1& from,
    const EnvironmentRecordV1& to,
    float factor,
    EnvironmentRecordV1& blended) noexcept;
[[nodiscard]] LightError BuildGpuLight(
    const LightRecordV1& record,
    const std::array<double, 3>& cameraOrigin,
    GpuLightRecordV1& gpu) noexcept;
[[nodiscard]] LightError SelectActiveLights(
    const LightSet& set,
    const std::array<double, 3>& cameraOrigin,
    LightSelection& selection) noexcept;

[[nodiscard]] float EvaluateAttenuation(
    const LightRecordV1& record,
    float distance) noexcept;
[[nodiscard]] float EvaluateCone(
    const LightRecordV1& record,
    float angleRadians) noexcept;
[[nodiscard]] std::array<float, 3> EvaluateRadiance(
    const LightRecordV1& record) noexcept;
[[nodiscard]] bool Contributes(const LightRecordV1& record) noexcept;
[[nodiscard]] DirectLighting EvaluateDirect(
    const LightRecordV1& record,
    const SurfacePoint& surface) noexcept;
[[nodiscard]] float EvaluateFog(
    const EnvironmentRecordV1& record,
    float distance) noexcept;

[[nodiscard]] LightPacketError ValidateLightPacket(
    const LightPacket& packet) noexcept;
[[nodiscard]] LightPacketError EncodeLightPacket(
    const LightPacket& packet,
    std::vector<std::byte>& bytes) noexcept;
[[nodiscard]] LightPacketError DecodeLightPacket(
    std::span<const std::byte> bytes,
    LightPacket& packet) noexcept;
[[nodiscard]] const char* ToString(LightError error) noexcept;
[[nodiscard]] const char* ToString(LightPacketError error) noexcept;

static_assert(sizeof(LightRecordV1) == 112);
static_assert(sizeof(EnvironmentRecordV1) == 112);
static_assert(sizeof(GpuLightRecordV1) == 80);
static_assert(sizeof(GpuEnvironmentV1) == 112);
static_assert(sizeof(GpuLightRecordV1) == scene::kGpuLightRecordSize);
static_assert(sizeof(GpuEnvironmentV1) == scene::kGpuEnvironmentSize);
static_assert(sizeof(LightPacketHeaderV1) == 64);
static_assert(offsetof(LightRecordV1, position) == 64);

}
