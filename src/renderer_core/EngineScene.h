#pragma once

#include "SceneShaderLayout.generated.h"
#include "renderer_api/RasterPacket.h"
#include "renderer_core/EngineLighting.h"
#include "renderer_core/EngineAcceleration.h"
#include "renderer_core/EngineIndirect.h"
#include "renderer_core/EngineReflection.h"
#include "renderer_core/EngineMaterialFamily.h"
#include "renderer_core/EngineTexture.h"
#include "renderer_core/EngineView.h"
#include "renderer_core/EngineVisibility.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::scene {

inline constexpr std::uint32_t kScenePacketMagic = 0x43534656u; // "VFSC"
inline constexpr std::uint16_t kScenePacketVersionMajor = 1;
// Version 1.0 carries one implicit instance per object. Version 1.1 adds an
// explicit instance section and 1.2 adds a per-object visibility section. A
// scene encodes at the lowest version that can represent it, so existing
// captures stay byte-identical.
inline constexpr std::uint16_t kScenePacketVersionMinor = 4;
inline constexpr std::uint16_t kScenePacketInstanceVersionMinor = 1;
inline constexpr std::uint16_t kScenePacketVisibilityVersionMinor = 2;
// The transparent draw table. A section that did not exist before is a new
// minor version, so a reader that predates it refuses the packet rather than
// reading those bytes at whatever they used to mean.
inline constexpr std::uint16_t kScenePacketTransparencyVersionMinor = 3;
// The decal projection on each transparent draw. The record grew rather than
// gaining a section of its own, because the volume belongs to the draw that
// projects it and a parallel list would have to be kept in step by hand. A
// wider record means the same bytes mean different things at the two
// versions, so a packet written before it is refused rather than read at the
// new stride -- which would not fail, it would assemble each draw's volume
// out of its neighbour's fields.
inline constexpr std::uint16_t kScenePacketDecalVersionMinor = 4;
inline constexpr std::uint32_t kScenePacketEndian = 0x01020304u;
inline constexpr std::uint32_t kMaximumOpaqueObjects = 65'536;
inline constexpr std::uint32_t kMaximumSceneInstances = 262'144;

enum ObjectFlag : std::uint32_t
{
    ObjectWritesWorldTarget = 1u << 0,
    ObjectStatic = 1u << 1,
};

inline constexpr std::uint32_t kKnownObjectFlags =
    ObjectWritesWorldTarget | ObjectStatic;

enum InstanceFlag : std::uint32_t
{
    InstanceStatic = 1u << 0,
};

inline constexpr std::uint32_t kKnownInstanceFlags = InstanceStatic;

enum class ScenePacketError : std::uint8_t
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
    InvalidIdentity,
    InvalidFlags,
    InvalidTransform,
    InvalidBounds,
    InvalidNormal,
    InvalidRoughness,
    DuplicateObject,
    DuplicateDraw,
    DuplicateInstance,
    InvalidInstance,
    InvalidVisibility,
    InvalidParameters,
    UncoveredObject,
    InvalidDraw,
    MissingMaterial,
    FrameMismatch,
    ViewMismatch,
    UnknownPass,
    PassClassMismatch,
    UnclassifiedWorldWriter,
    UncoveredPass,
    // A transparent record carrying a field outside the range every consumer
    // of it can represent -- a stencil value wider than the eight bits stencil
    // actually has, which would be silently narrowed downstream.
    InvalidTransparentDraw,
    AllocationFailure,
};

struct alignas(16) OpaqueObjectV1
{
    std::uint64_t objectId{};
    std::uint64_t materialId{};
    std::uint32_t drawIndex{};
    std::uint32_t flags{ObjectWritesWorldTarget | ObjectStatic};
    float roughness{1.0f};
    std::uint32_t passSequence{};
    float model[16]{};
    float previousModel[16]{};
    float boundsMinimum[4]{};
    float boundsMaximum[4]{};
    float geometricNormal[4]{};
    float shadingNormal[4]{};
};

struct alignas(16) InstanceV1
{
    std::uint64_t objectId{};
    std::uint32_t objectIndex{};
    std::uint32_t flags{InstanceStatic};
    float model[16]{};
    float previousModel[16]{};
    float parameters[4]{1.0f, 1.0f, 1.0f, 1.0f};
};

struct alignas(8) ScenePacketHeaderV1
{
    std::uint32_t magic{kScenePacketMagic};
    std::uint16_t versionMajor{kScenePacketVersionMajor};
    std::uint16_t versionMinor{kScenePacketVersionMinor};
    std::uint32_t headerSize{sizeof(ScenePacketHeaderV1)};
    std::uint32_t totalSize{};
    std::uint32_t payloadCrc32{};
    std::uint32_t endianMarker{kScenePacketEndian};
    std::uint64_t frameId{};
    std::uint64_t viewId{};
    std::uint64_t captureSequence{};
    std::uint32_t captureThreadId{};
    std::uint32_t renderThreadId{};
    std::uint32_t objectCount{};
    std::uint32_t objectsOffset{};
    std::uint32_t flags{};
    std::uint32_t instanceCount{};
    std::uint32_t instancesOffset{};
    std::uint32_t reserved0{};
    // Carved out of the former reserved pair, so the header still occupies
    // exactly 96 bytes and no earlier field moves.
    std::uint32_t visibilityCount{};
    std::uint32_t visibilityOffset{};
    // Carved out of the last reserved qword, the same way the visibility
    // pair was carved out of the one before it, so the header still occupies
    // exactly 96 bytes and no earlier field moves.
    std::uint32_t transparentCount{};
    std::uint32_t transparentOffset{};
};

// One blended draw the engine issued, with the blend state it sorted by.
// Transparency is a property of the frame rather than of the material: the
// same material is drawn blended in one pass and opaque in another, so the
// state has to travel with the frame or the composite invents an order.
struct alignas(16) TransparentDrawRecordV1
{
    std::uint64_t drawId{};
    std::uint64_t materialId{};
    // Which object of this scene the draw covers.
    std::uint32_t objectIndex{};
    // blend::BlendMode, stored as its raw value so the packet does not depend
    // on the enum's definition to be read.
    std::uint32_t blend{};
    // View-space depth of the sort origin. Carried rather than recomputed:
    // transparent geometry has no single depth, and the engine already chose
    // one to sort by.
    float sortDepth{};
    float softFade{};
    float dissolve{};
    float dissolveFalloff{};
    std::uint32_t domain{};
    std::uint32_t flags{};
    // The volume this draw projects into, for the decal domains. A decal is
    // not a quad hanging in the air: it is a projection onto whatever surface
    // it reaches, and none of that is recoverable from the rest of the
    // record. A range of zero means the draw projects nothing and is composited
    // as ordinary blended geometry.
    float decalOrigin[3]{};
    float decalRange{};
    float decalAxis[3]{0.0f, 0.0f, -1.0f};
    float decalRadius{};
    // Which surfaces the engine marked as receivers. A decal that ignores
    // this lands on the sky and on characters walking past it.
    std::uint32_t stencilReceiverMask{};
    std::uint32_t stencilReference{};
    std::uint32_t reserved[2]{};
};

// What a ray-query hit needs that the query itself cannot report.
//
// A query returns a geometry index and a barycentric pair and nothing else --
// no vertex attributes, no material. Everything a hit shades with is
// therefore resolved on the host, once per geometry, and read back through
// this table. Mirrored by `GpuGeometryRecordV1` in scene_layout.glsl.
struct alignas(16) GpuGeometryRecordV1
{
    std::uint32_t objectIndex{};
    // Absolute element offsets into the frame's shared upload buffer, so the
    // shader needs no per-frame base and cannot apply the wrong one.
    std::uint32_t firstIndexElement{};
    std::uint32_t firstVertexFloat{};
    std::uint32_t textureIndex{};
    std::uint32_t flags{};
    std::uint32_t reserved[3]{};
};

// Set when the frame's indices are 16-bit, which the shader unpacks two to a
// word. Both widths occur -- the fixtures encode 16-bit and the live mirror
// emits 32-bit -- and reading one as the other walks a triangle list that
// does not exist.
inline constexpr std::uint32_t kGeometryIndexIs16Bit = 1u << 0;
// Set when this geometry runs an alpha test, which is also when the structure
// leaves it non-opaque so a query reports candidates instead of committing
// them. Mirrors `kVfGeometryAlphaTested` in phase11/scene_layout.glsl.
inline constexpr std::uint32_t kGeometryAlphaTested = 1u << 1;

static_assert(sizeof(GpuGeometryRecordV1) == kGpuGeometryRecordSize);

struct ScenePacket
{

    ScenePacketHeaderV1 header{};
    std::vector<OpaqueObjectV1> objects;
    // Empty means one implicit instance per object using the object's own
    // transform. Otherwise every object owns a contiguous run of instances.
    std::vector<InstanceV1> instances;
    // Empty means the frame has no blended draws at all, which is what keeps
    // an opaque capture encoding at the older version.
    std::vector<TransparentDrawRecordV1> transparent;
    // Empty means every object is opaque, front-facing only, and unmirrored.
    // Otherwise there is exactly one record per object, in object order.
    std::vector<visibility::VisibilityRecordV1> visibility;
};

struct alignas(16) GBufferPixelV1
{
    float albedo[4]{};
    float geometricNormalRoughness[4]{};
    float shadingNormalDepth[4]{};
    std::uint32_t objectId[2]{};
    std::uint32_t materialId[2]{};
    // How much of this pixel a transparent effect decides. An upscaler that
    // does not know this reconstructs a particle from history that never
    // contained it, which is the ghost trailing every spark and muzzle flash.
    // The rest of the row is reserved rather than removed: a sixteen-byte
    // plane is what the readback interleaves, and a narrower one would make
    // this record disagree with the attachment it is copied from.
    float reactive{};
    float reserved[3]{};
};

struct GBufferImage
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<GBufferPixelV1> pixels;

    [[nodiscard]] const GBufferPixelV1& At(
        std::uint32_t x,
        std::uint32_t y) const;
};

// Engine pass accounting for one mirrored view. It is deliberately kept
// separate from backend draw construction so a frame can never be armed
// while a world-target writer is still unclassified.
struct SceneCoverage
{
    std::uint32_t worldWritingPasses{};
    std::uint32_t opaquePasses{};
    std::uint32_t mirroredPasses{};
    std::uint32_t deferredClasses{};
    std::uint32_t unknownWorldWriters{};
    std::uint32_t unmirroredOpaquePasses{};

    [[nodiscard]] bool MirrorEligible() const noexcept
    {
        return unknownWorldWriters == 0 && unmirroredOpaquePasses == 0;
    }
};

struct GBufferComparison
{
    // Plane 0 albedo, 1 geometric normal and roughness, 2 shading normal and
    // depth, 3 reactive; and the channel within it.
    std::uint32_t worstGroup{};
    std::uint32_t worstChannel{};
    float worstExpected{};
    float worstActual{};
    // Which object owned the worst pixel. "Some pixels differ" is not a lead;
    // "this object's pixels differ" is.
    std::uint64_t worstObjectId{};
    std::uint64_t comparedPixels{};
    std::uint64_t differingPixels{};
    std::uint64_t identityMismatches{};
    float maximumAbsoluteError{};
    double meanAbsoluteError{};

    [[nodiscard]] bool Within(
        float maximumError,
        double maximumMeanError,
        std::uint64_t maximumDifferingPixels,
        std::uint64_t maximumIdentityMismatches = 0) const noexcept;
};

struct InstanceRange
{
    std::uint32_t first{};
    std::uint32_t count{};
};

[[nodiscard]] ScenePacketError ValidateScenePacket(
    const ScenePacket& packet) noexcept;
[[nodiscard]] ScenePacketError ValidateSceneInstances(
    const ScenePacket& packet) noexcept;
[[nodiscard]] ScenePacketError ValidateSceneVisibility(
    const ScenePacket& packet) noexcept;
// Version 1.0 and 1.1 scenes expose an implicit opaque, front-only record so
// every consumer sees a single resolution rule.
[[nodiscard]] visibility::VisibilityRecordV1 ResolveVisibility(
    const ScenePacket& packet,
    std::size_t objectIndex) noexcept;
// Version 1.0 scenes expose one implicit instance per object so every
// consumer sees a single expansion rule.
[[nodiscard]] InstanceRange ObjectInstanceRange(
    const ScenePacket& packet,
    std::size_t objectIndex) noexcept;
// Moves an instance from absolute world coordinates into coordinates relative
// to the camera, which is the space a camera-relative view matrix expects.
//
// Fallout 4 publishes both -- absolute per-object transforms and a view whose
// fourth column is zero -- and a renderer handed the pair draws the cell from
// the world's origin. Only the translation moves; the rotation belongs to the
// object and would turn the whole cell with the player if it were touched.
// The subtraction is done in double for the origin's sake rather than the
// object's. Two floats near a hundred and twenty thousand difference exactly
// even in single precision, so the object positions lose nothing either way;
// the camera position does. It is recovered as a double, and at that magnitude
// a float holds it only to about eight thousandths, so rounding it before
// subtracting shifts the entire cell by the discarded part every time the
// camera moves far enough to land on a different float.
//
// A non-finite origin returns the instance unchanged rather than poisoning
// every transform in the frame with a NaN.
[[nodiscard]] InstanceV1 NarrowInstance(
    const InstanceV1& instance,
    std::span<const double, 3> cameraOrigin) noexcept;

[[nodiscard]] InstanceV1 ResolveInstance(
    const ScenePacket& packet,
    std::size_t objectIndex,
    std::uint32_t instanceIndex) noexcept;
[[nodiscard]] ScenePacketError ValidateSceneAgainstRaster(
    const ScenePacket& scene,
    const raster::DecodedPacket& rasterPacket,
    std::uint64_t expectedFrameId,
    std::uint64_t expectedViewId) noexcept;
[[nodiscard]] ScenePacketError ValidateSceneAgainstFrame(
    const ScenePacket& scene,
    const view::FramePacket& frame,
    SceneCoverage& coverage) noexcept;
[[nodiscard]] ScenePacketError EncodeScenePacket(
    const ScenePacket& packet,
    std::vector<std::byte>& bytes) noexcept;
[[nodiscard]] ScenePacketError DecodeScenePacket(
    std::span<const std::byte> bytes,
    ScenePacket& packet) noexcept;
[[nodiscard]] ScenePacketError ProjectScenePacket(
    const raster::DecodedPacket& source,
    const view::ViewRecordV1& view,
    const ScenePacket& scene,
    raster::DecodedPacket& projected) noexcept;
// Projection stores clip-space positions, so the camera-relative world
// position each vertex had is otherwise lost. Lighting needs it: a light and
// the surface it lights must be measured in the same space. `positions` ends
// up parallel to `projected.vertices`.
[[nodiscard]] ScenePacketError ProjectScenePacket(
    const raster::DecodedPacket& source,
    const view::ViewRecordV1& view,
    const ScenePacket& scene,
    raster::DecodedPacket& projected,
    std::vector<std::array<float, 3>>* cameraRelativePositions) noexcept;
// And the reciprocal of each vertex's clip w, which is what makes an
// attribute interpolate the way hardware interpolates it. Screen-space
// barycentrics weight a value linearly across the *picture*; a perspective
// projection needs it linear across the *surface*, and the two agree only
// while the attribute is constant or the triangle faces the camera. Every
// fixture here keeps varying attributes off rotated geometry for exactly
// that reason, which is a limit on what can be tested rather than a
// property of the renderer.
[[nodiscard]] ScenePacketError ProjectScenePacket(
    const raster::DecodedPacket& source,
    const view::ViewRecordV1& view,
    const ScenePacket& scene,
    raster::DecodedPacket& projected,
    std::vector<std::array<float, 3>>* cameraRelativePositions,
    std::vector<float>* inverseW) noexcept;
[[nodiscard]] ScenePacketError RenderReferenceGBuffer(
    const raster::DecodedPacket& projected,
    const ScenePacket& scene,
    GBufferImage& image) noexcept;
// Alpha-aware form. A null texture means every surface samples opaque white,
// which is exactly what the three-argument form does, so Phase 11 and 12
// references stay byte-identical.
[[nodiscard]] ScenePacketError RenderReferenceGBuffer(
    const raster::DecodedPacket& projected,
    const ScenePacket& scene,
    const texture::CapturedTexture* baseColor,
    GBufferImage& image) noexcept;

// Everything the reference may sample or dispatch on. Every member absent is
// exactly the earlier forms' behaviour, which is what keeps this a
// generalization rather than a second renderer.
struct ReferenceInputs
{
    const texture::CapturedTexture* baseColor{};
    const texture::CapturedTexture* normalMap{};
    // The material.s third texture slot, which a material declaring
    // MaterialSlotRole::GlowMap uses as its emission mask. Absent means the
    // mask is white, which leaves a declared emission colour unmodulated --
    // exactly what every material without a glow map already gets.
    const texture::CapturedTexture* glowMap{};
    const material::FamilyPacket* families{};
    // Absent means no lights and no environment, which shades exactly as the
    // earlier phases did and keeps their references byte-identical. These are
    // the *GPU* records deliberately: the host records hold world positions
    // while the GPU records hold camera-relative ones, so evaluating the host
    // form here would compare two different scenes.
    std::span<const lighting::GpuLightRecordV1> lights{};
    const lighting::GpuEnvironmentV1* environment{};
    // Parallel to the projected vertices. Projection keeps clip space, so
    // without these the reference would light a surface at its screen
    // position while the shader lights it at its world position.
    std::span<const std::array<float, 3>> vertexPositions{};
    // Parallel to `vertexPositions`. Present means attributes interpolate
    // perspective-correctly, the way the device interpolates them; absent
    // means screen-space, which agrees only while the attribute is constant
    // across the triangle or the triangle faces the camera.
    std::span<const float> inverseW{};
    // The height field a parallax-occlusion material marches. Absent means no
    // material in the frame marches one, which is the state every fixture
    // before this one was in.
    const texture::CapturedTexture* heightMap{};
    // Occluders in the same camera-relative space as the lights and the
    // shaded positions, which is the space the top-level structure is built
    // in. Absent means an unshadowed reference: exactly what a device without
    // ray query renders, and exactly what phase 17 compared against.
    std::span<const accel::ShadowTriangle> occluders{};
    // Reflection geometry, carrying each object's normal and albedo so a
    // reflection hit can be shaded. Absent means an unreflected reference,
    // which is what a device without ray query renders.
    std::span<const reflect::ReflectionTriangle> reflectionGeometry{};
    reflect::ReflectionPolicy reflectionPolicy{};
    // One bounce of diffuse indirect over the same geometry. Absent means an
    // unlit-by-bounce reference, which is what a device without ray query
    // renders and what every phase before this one compared against.
    bool indirectEnabled{};
    gi::IndirectRules indirectRules{};
    gi::QualityPreset indirectPreset{};
};

// The reference traces one shadow term per light into a fixed buffer rather
// than allocating per pixel. A frame carrying more lights than this shades
// the remainder unshadowed, which is visible in the comparison rather than
// silent, and matches the light count the packet already caps at.
inline constexpr std::size_t kMaximumReferenceLights = 64;

// Linear HDR colour, which is where emission lands. It is a separate image
// because the G-buffer pixel record is a captured artifact whose 64 bytes
// cannot grow without invalidating every earlier comparison.
struct HdrImage
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::array<float, 4>> pixels;

    [[nodiscard]] const std::array<float, 4>& At(
        std::uint32_t x,
        std::uint32_t y) const;
};

// Family-aware form. `hdr` may be null when only the G-buffer is compared.
[[nodiscard]] ScenePacketError RenderReferenceGBuffer(
    const raster::DecodedPacket& projected,
    const ScenePacket& scene,
    const ReferenceInputs& inputs,
    GBufferImage& image,
    HdrImage* hdr) noexcept;
[[nodiscard]] GBufferComparison CompareGBuffer(
    std::span<const GBufferPixelV1> expected,
    std::span<const GBufferPixelV1> actual) noexcept;
[[nodiscard]] const char* ToString(ScenePacketError error) noexcept;

static_assert(sizeof(ScenePacketHeaderV1) == 96);
static_assert(offsetof(ScenePacketHeaderV1, visibilityCount) == 80);
static_assert(offsetof(ScenePacketHeaderV1, visibilityOffset) == 84);
static_assert(sizeof(InstanceV1) == 160);
static_assert(sizeof(InstanceV1) == kGpuSceneInstanceSize);
static_assert(offsetof(InstanceV1, model) == 16);
static_assert(offsetof(InstanceV1, previousModel) == 80);
static_assert(offsetof(InstanceV1, parameters) == 144);
static_assert(sizeof(OpaqueObjectV1) == 224);
static_assert(sizeof(OpaqueObjectV1) == kGpuOpaqueObjectSize);
static_assert(offsetof(OpaqueObjectV1, model) == 32);
static_assert(offsetof(OpaqueObjectV1, previousModel) == 96);
static_assert(offsetof(OpaqueObjectV1, boundsMinimum) == 160);
static_assert(offsetof(OpaqueObjectV1, geometricNormal) == 192);
static_assert(sizeof(GBufferPixelV1) == 80);
static_assert(sizeof(GBufferPixelV1) == kGpuGBufferPixelSize);
static_assert(offsetof(GBufferPixelV1, objectId) == 48);

}
