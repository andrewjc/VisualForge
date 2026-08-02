#pragma once

#include "SceneShaderLayout.generated.h"
#include "renderer_api/RasterPacket.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::visibility {

// The engine's ordered dither is a 4x4 Bayer matrix, so a fade resolves to
// one of seventeen coverage levels per tile.
inline constexpr std::uint32_t kDitherExtent = 4;
inline constexpr std::uint32_t kMaximumAlphaMipLevels = 16;
inline constexpr std::uint32_t kMaximumCoverageSamples = 16;

// Which inputs the alpha test actually consults. This is captured, not
// assumed: a surface that consults nothing cannot discard.
enum class AlphaSource : std::uint8_t
{
    None,
    BaseColorTexture,
    MaterialConstant,
    VertexColor,
    TextureAndConstant,
};

enum class AlphaClass : std::uint8_t
{
    Opaque,
    Tested,
    // Sorted transparency. Classified here so it can be excluded from the
    // opaque raster class rather than silently rendered as a cutout.
    Blended,
    Unclassified,
};

enum class FaceMode : std::uint8_t
{
    FrontOnly,
    BackOnly,
    TwoSided,
};

enum VisibilityFlag : std::uint32_t
{
    AlphaToCoverage = 1u << 0,
    DitherFade = 1u << 1,
};

inline constexpr std::uint32_t kKnownVisibilityFlags =
    AlphaToCoverage | DitherFade;

enum class VisibilityError : std::uint8_t
{
    None,
    InvalidIdentity,
    InvalidFlags,
    InvalidCutoff,
    InvalidFade,
    InvalidFaceMode,
    InvalidDeterminant,
    InvalidNormalFrame,
    InvalidMipChain,
    UnclassifiedAlpha,
    BlendedNotSupported,
    AllocationFailure,
};

// What the engine's alpha property records at a draw. The blend function
// values are recorded verbatim and never interpreted by this phase.
struct AlphaPropertyCapture
{
    bool blendEnabled{};
    bool testEnabled{};
    bool alphaToCoverage{};
    bool ditherFade{};
    std::uint8_t testReference{};
    std::uint32_t sourceBlend{};
    std::uint32_t destinationBlend{};
    float fade{1.0f};
};

struct alignas(16) AlphaStateV1
{
    AlphaSource source{AlphaSource::None};
    AlphaClass classification{AlphaClass::Opaque};
    std::uint16_t reserved0{};
    std::uint32_t flags{};
    float reference{};
    float constantAlpha{1.0f};
    float fade{1.0f};
    std::uint32_t reserved1[3]{};
};

struct alignas(16) VisibilityRecordV1
{
    std::uint64_t objectId{};
    std::uint64_t materialId{};
    AlphaStateV1 alpha{};
    FaceMode faceMode{FaceMode::FrontOnly};
    std::uint8_t reserved0[3]{};
    float modelDeterminant{1.0f};
    std::uint64_t reserved1{};
};

// Rendering state a coverage decision depends on. The depth prepass and the
// color pass differ only in this flag, and coverage must ignore it.
struct CoverageContext
{
    std::uint32_t pixelX{};
    std::uint32_t pixelY{};
    std::uint32_t sampleCount{1};
    bool depthOnly{};
};

struct CoverageResult
{
    bool covered{};
    float coverage{};
};

struct AlphaMipLevel
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<float> alpha;
};

struct ShadingFrameInput
{
    std::array<float, 3> geometricNormal{0.0f, 0.0f, 1.0f};
    std::array<float, 3> shadingNormal{0.0f, 0.0f, 1.0f};
    std::array<float, 3> tangent{1.0f, 0.0f, 0.0f};
    std::array<float, 3> bitangent{0.0f, 1.0f, 0.0f};
};

struct ShadingFrame
{
    std::array<float, 3> geometricNormal{};
    std::array<float, 3> shadingNormal{};
    std::array<float, 3> tangent{};
    std::array<float, 3> bitangent{};
    // The face was shaded from its back and both normals were flipped.
    bool flipped{};
    // The transform mirrors, so the tangent basis handedness was corrected.
    bool mirrored{};
    // The shading normal had fallen below the geometric horizon and was
    // lifted back onto it instead of lighting the surface from behind.
    bool liftedToHorizon{};
};

[[nodiscard]] VisibilityError ClassifyAlphaState(
    const AlphaPropertyCapture& capture,
    AlphaSource observedSource,
    AlphaStateV1& state) noexcept;
[[nodiscard]] CoverageResult EvaluateCoverage(
    const AlphaStateV1& state,
    float sampledAlpha,
    const CoverageContext& context) noexcept;
[[nodiscard]] float DitherThreshold(
    std::uint32_t pixelX,
    std::uint32_t pixelY) noexcept;
[[nodiscard]] float AlphaCoverage(
    const AlphaMipLevel& level,
    float reference,
    float scale) noexcept;
// Mip 0 defines the target coverage; every coarser level gets the scale that
// brings its coverage closest to it, which is what stops cutouts from
// dissolving with distance.
[[nodiscard]] VisibilityError ComputeAlphaCoverageScales(
    std::span<const AlphaMipLevel> chain,
    float reference,
    std::vector<float>& scales) noexcept;
[[nodiscard]] VisibilityError ResolveShadingFrame(
    FaceMode faceMode,
    bool backFacing,
    float modelDeterminant,
    const ShadingFrameInput& input,
    ShadingFrame& frame) noexcept;
[[nodiscard]] raster::FrontFace EffectiveFrontFace(
    raster::FrontFace declared,
    float modelDeterminant) noexcept;
[[nodiscard]] VisibilityError ValidateVisibilityRecord(
    const VisibilityRecordV1& record) noexcept;
// Additional gate for the opaque raster class this phase renders.
[[nodiscard]] VisibilityError ValidateOpaqueRasterClass(
    const VisibilityRecordV1& record) noexcept;
[[nodiscard]] const char* ToString(VisibilityError error) noexcept;

static_assert(sizeof(AlphaStateV1) == 32);
static_assert(sizeof(VisibilityRecordV1) == 64);
static_assert(offsetof(VisibilityRecordV1, alpha) == 16);
static_assert(offsetof(AlphaStateV1, flags) == 4);
static_assert(offsetof(AlphaStateV1, reference) == 8);
static_assert(offsetof(VisibilityRecordV1, faceMode) == 48);
static_assert(offsetof(VisibilityRecordV1, modelDeterminant) == 52);
static_assert(sizeof(VisibilityRecordV1) == scene::kGpuVisibilityRecordSize);

}
