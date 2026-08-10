#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vf::renderer::water {

// How a transmissive surface obtains what is behind it. The choice is policy,
// not material: the same window uses a screen-space read when nothing better
// is available and a traced ray when the device can afford one, and the
// material must not have to be rewritten for that to change.
enum class RefractionSource : std::uint8_t
{
    // Nothing captured what is behind the surface. Reported rather than
    // approximated: a refraction that invents its background is worse than
    // one that admits it has none.
    Unavailable = 0,
    ScreenSpace = 1,
    Traced = 2,
};

enum class ReflectionMode : std::uint8_t
{
    None = 0,
    ScreenSpace = 1,
    Traced = 2,
};

// What a transmissive surface is, which decides its documented defaults when
// the capture carries no thickness or index of refraction.
enum class TransmissiveClass : std::uint8_t
{
    Water = 0,
    Glass = 1,
    // Recognised as transmissive but not as anything with a documented
    // default. It keeps its vanilla path rather than borrowing another
    // class's constants.
    Unknown = 2,
};

enum class WaterError : std::uint8_t
{
    None,
    InvalidIdentity,
    NonFiniteSource,
    InvalidDepthRange,
    DegeneratePlane,
    InvalidScroll,
    InvalidIor,
    MissingTransmission,
};

// Documented class defaults, used when a capture carries no value. They are
// physical constants, not tuned numbers, and they are used *instead of*
// inferring anything from the rendered image: pixel inference produces a
// different answer every frame the camera moves, which is indistinguishable
// from a flickering material.
inline constexpr float kWaterIor = 1.333f;
inline constexpr float kGlassIor = 1.52f;
inline constexpr float kDefaultGlassThickness = 0.01f;

// One of the three scrolling normal layers the engine animates. Three layers
// at different speeds is what stops water reading as a single sliding texture,
// so the count is part of the contract rather than a quality setting.
struct NormalLayer
{
    std::array<float, 2> scroll{};
    float scale{1.0f};
    float amplitude{1.0f};
};

inline constexpr std::size_t kNormalLayerCount = 3;

struct WaterMaterialV1
{
    std::uint64_t materialId{};
    TransmissiveClass surfaceClass{TransmissiveClass::Water};
    std::array<NormalLayer, kNormalLayerCount> layers{};
    // Shallow, deep, fog and silt, in linear colour.
    std::array<float, 3> shallowColor{};
    std::array<float, 3> deepColor{};
    std::array<float, 3> fogColor{};
    std::array<float, 3> siltColor{};
    // Depth over which shallow becomes deep, in world units.
    float depthRange{};
    // Depth at which the surface fades out entirely, so a shoreline does not
    // end in a hard line across the sand.
    float shorelineDepth{};
    float fogDensity{};
    float siltDensity{};
    // Reflectance at normal incidence and the sharpness of the sparkle
    // highlight.
    float fresnelBias{};
    float sparklePower{};
    // Zero means the capture carried none; the class default is used and the
    // fact is reported.
    float indexOfRefraction{};
    float thickness{};
    bool hasTransmissionMetadata{};
};

// The plane a water surface reflects about. A degenerate one cannot reflect,
// and reflecting about a guessed plane puts the mirrored world somewhere that
// looks deliberate and is wrong.
struct ReflectionPlane
{
    std::array<float, 3> normal{0.0f, 0.0f, 1.0f};
    float height{};
};

struct WaterPolicy
{
    bool tracingAvailable{};
    bool screenSpaceAvailable{true};
    // Above this roughness a traced reflection costs more than it resolves,
    // exactly as the reflection pass decides.
    float tracedReflectionCutoff{0.4f};
};

struct UnderwaterState
{
    bool submerged{};
    // How far below the surface the camera sits. Zero at the boundary, so a
    // transition can be driven continuously rather than snapping.
    float depth{};
    bool crossedThisFrame{};
};

[[nodiscard]] WaterError ValidateMaterial(
    const WaterMaterialV1& material) noexcept;

// The animated normal at a point and time, from all three layers. Returns
// false when the layers cancel to nothing, which is a captured contradiction
// rather than a flat surface.
[[nodiscard]] bool EvaluateNormal(
    const WaterMaterialV1& material,
    const std::array<float, 2>& position,
    float time,
    std::array<float, 3>& normal) noexcept;

// Water colour at a given depth of water beneath the surface.
[[nodiscard]] std::array<float, 3> EvaluateWaterColor(
    const WaterMaterialV1& material,
    float depthBelow) noexcept;

// Coverage at the shoreline. Water that ends abruptly draws a hard line
// across the sand, which is the artefact this exists to prevent.
[[nodiscard]] float ShorelineCoverage(
    const WaterMaterialV1& material,
    float depthBelow) noexcept;

// Schlick against the surface, using the class default when the capture
// carried no index of refraction.
[[nodiscard]] float FresnelReflectance(
    const WaterMaterialV1& material,
    float cosine) noexcept;

// The index of refraction to use, and whether it came from the capture or
// from a documented default. Never inferred from the image.
[[nodiscard]] WaterError ResolveIndexOfRefraction(
    const WaterMaterialV1& material,
    float& ior,
    bool& fromCapture) noexcept;

[[nodiscard]] ReflectionMode SelectReflection(
    const WaterMaterialV1& material,
    const WaterPolicy& policy,
    float roughness) noexcept;

[[nodiscard]] RefractionSource SelectRefraction(
    const WaterMaterialV1& material,
    const WaterPolicy& policy) noexcept;

// Mirrors a position about the reflection plane.
[[nodiscard]] WaterError ReflectAboutPlane(
    const ReflectionPlane& plane,
    const std::array<float, 3>& position,
    std::array<float, 3>& mirrored) noexcept;

// Where the camera is relative to the surface, and whether it just crossed.
// A crossing invalidates every history that assumed the other medium.
[[nodiscard]] UnderwaterState EvaluateUnderwater(
    const ReflectionPlane& plane,
    const std::array<float, 3>& cameraPosition,
    const std::array<float, 3>& previousCameraPosition) noexcept;

// Fog applied to what is seen through the water, by distance travelled in it.
// The reflected and transmitted shares combined. The two weights are a
// partition of one by construction rather than two independently tuned terms:
// anything else either creates light at grazing angles, where Fresnel
// approaches one and a separate transmission is still adding, or loses it
// head-on. Fog applies to the transmitted side only -- fogging the reflection
// would dim the sky by the depth of the water underneath it.
[[nodiscard]] std::array<float, 3> ShadeWater(
    const WaterMaterialV1& material,
    const std::array<float, 3>& reflected,
    const std::array<float, 3>& refracted,
    float cosine,
    float distanceThroughWater) noexcept;

[[nodiscard]] std::array<float, 3> UnderwaterFog(
    const WaterMaterialV1& material,
    const std::array<float, 3>& incoming,
    float distanceThroughWater) noexcept;

[[nodiscard]] const char* ToString(WaterError error) noexcept;
[[nodiscard]] const char* ToString(TransmissiveClass surfaceClass) noexcept;

}
