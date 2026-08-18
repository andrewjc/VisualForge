#pragma once

#include "renderer_core/EngineLighting.h"
#include "renderer_core/EngineReflection.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vf::renderer::gi {

// Where one bounce of diffuse indirect light came from. Recorded for the same
// reason the reflection source is: black because nothing was hit and black
// because the sky is black are different failures, and only one is a bug.
enum class IndirectSource : std::uint8_t
{
    Skipped = 0,
    Geometry = 1,
    Environment = 2,
    Unresolved = 3,
};

enum class IndirectError : std::uint8_t
{
    None,
    DegenerateNormal,
    NonFiniteSource,
    InvalidResolution,
    InvalidHistory,
};

// Why a temporal sample was refused. Reported rather than folded into a
// single boolean, because "the camera cut" and "this pixel is newly visible"
// need different responses and look identical in a boolean.
enum class RejectReason : std::uint8_t
{
    Accepted = 0,
    Epoch = 1,
    OffScreen = 2,
    Depth = 3,
    Normal = 4,
    Object = 5,
    Material = 6,
};

// Correctness parameters, kept apart from quality presets on purpose: the
// plan requires a preset change not to alter what is captured or how a
// surface is identified. Everything here changes the *answer*; everything in
// QualityPreset changes only how long it takes to get there.
struct IndirectRules
{
    // A single sample brighter than this is a firefly: one unlucky path that
    // survives temporal accumulation for seconds as a bright dot. Clamped
    // rather than discarded, because discarding biases the mean darker while
    // clamping bounds the variance.
    float radianceClamp{8.0f};
    // Relative depth agreement. Absolute epsilons fail at distance, where a
    // pixel's depth changes by metres between frames without moving.
    float depthTolerance{0.05f};
    // Cosine of the largest normal change a reprojected sample may survive.
    float normalCosineTolerance{0.9f};
    // Whether object and material identity must match. They are separate
    // because two objects can share a material and one object can change
    // material without moving.
    bool requireObjectMatch{true};
    bool requireMaterialMatch{true};
};

struct QualityPreset
{
    std::uint32_t raysPerPixel{1};
    // Half resolution traces a quarter of the rays. The mapping has to be
    // explicit: a full-resolution pixel that reads the wrong low-resolution
    // sample smears indirect light across an edge.
    bool halfResolution{false};
    // The longest history a pixel may accumulate. Longer converges further
    // but responds later; this is a quality trade, not a correctness one.
    std::uint32_t maximumHistoryLength{32};
};

struct SurfaceSample
{
    std::array<float, 3> position{};
    std::array<float, 3> geometricNormal{0.0f, 0.0f, 1.0f};
    std::array<float, 3> albedo{0.5f, 0.5f, 0.5f};
    float depth{};
    std::uint64_t objectId{};
    std::uint64_t materialId{};
};

// One pixel's accumulated indirect light. Mean and second moment together,
// because variance is what a denoiser needs and recovering it from the mean
// alone is impossible.
struct HistorySample
{
    std::array<float, 3> mean{};
    std::array<float, 3> secondMoment{};
    std::uint32_t length{};
};

struct ReprojectionResult
{
    RejectReason reason{RejectReason::Accepted};
    std::uint32_t sourceX{};
    std::uint32_t sourceY{};
};

// Cosine-weighted hemisphere direction. Cosine weighted rather than uniform
// because the diffuse integral already carries a cosine: sampling uniformly
// and multiplying by it wastes most rays near the horizon where they
// contribute least.
[[nodiscard]] IndirectError SampleDiffuseDirection(
    const std::array<float, 3>& normal,
    const std::array<float, 2>& sample,
    std::array<float, 3>& direction) noexcept;

// Bounds one sample without biasing the mean darker than a discard would.
[[nodiscard]] std::array<float, 3> ClampRadiance(
    const std::array<float, 3>& radiance,
    const IndirectRules& rules) noexcept;

// Indirect light must exclude what the direct pass already added, or every
// lit surface is counted twice and interiors bloom.
[[nodiscard]] std::array<float, 3> SeparateIndirect(
    const std::array<float, 3>& total,
    const std::array<float, 3>& direct) noexcept;

// Where a pixel was last frame, and whether that sample may be believed.
[[nodiscard]] ReprojectionResult Reproject(
    const SurfaceSample& current,
    const SurfaceSample& previous,
    const std::array<float, 2>& motion,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height,
    const IndirectRules& rules,
    const reflect::ReflectionHistoryKey& previousEpoch,
    const reflect::ReflectionHistoryKey& currentEpoch) noexcept;

// Folds one new sample into a history. A rejected reprojection resets rather
// than blends: blending a rejected sample is exactly the trail the gate
// forbids.
[[nodiscard]] HistorySample Accumulate(
    const HistorySample& history,
    const std::array<float, 3>& sample,
    RejectReason reason,
    const QualityPreset& preset) noexcept;

// Per-channel variance from the accumulated moments.
[[nodiscard]] std::array<float, 3> Variance(
    const HistorySample& history) noexcept;

// Which low-resolution sample a full-resolution pixel reads. Explicit because
// an off-by-one here smears indirect light across every edge in the frame.
[[nodiscard]] IndirectError MapToTraceResolution(
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height,
    const QualityPreset& preset,
    std::uint32_t& traceX,
    std::uint32_t& traceY,
    std::uint32_t& traceWidth,
    std::uint32_t& traceHeight) noexcept;

// The complete one-bounce diffuse term for a surface, traced against the same
// geometry the shadow and reflection passes use.
[[nodiscard]] std::array<float, 3> EvaluateIndirect(
    const SurfaceSample& surface,
    const IndirectRules& rules,
    const QualityPreset& preset,
    std::span<const reflect::ReflectionTriangle> geometry,
    std::span<const lighting::GpuLightRecordV1> lights,
    const lighting::GpuEnvironmentV1& environment,
    std::uint32_t pixelX,
    std::uint32_t pixelY,
    std::uint32_t frameIndex,
    IndirectSource& source,
    // Optional summary of what every bounce ray of this pixel hit. The
    // diffuse bounce casts eight rays where the reflection casts one, and
    // nothing reports their hits, so a contract cannot tell a shading
    // disagreement from eight independent chances at an intersector one.
    float* hitProbe = nullptr) noexcept;

[[nodiscard]] const char* ToString(IndirectError error) noexcept;
[[nodiscard]] const char* ToString(RejectReason reason) noexcept;

// The device forms of the temporal pass. Accumulation runs over pixels rather
// than over geometry, so it is a compute pass with one record in and one out,
// and these are what the shader reads. Mirrored by phase20/accumulate_layout;
// a stride that disagrees with the shader neither fails to compile nor trips
// validation, it just reads every pixel at the wrong address.
struct alignas(16) GpuIndirectPixelV1
{
    // Padded to a four-float boundary because std430 rounds a vec3 up to one,
    // and the depth rides in the slot the padding would otherwise waste.
    float normal[3]{0.0f, 0.0f, 1.0f};
    float depth{};
    float radiance[3]{};
    float radiancePad{};
    float motion[2]{};
    // Both halves of each identity. Narrowed to thirty-two bits, two objects
    // whose low words agree would reproject into each other -- a smear across
    // the seam between those two objects and nowhere else in the frame.
    std::uint32_t objectId[2]{};
    std::uint32_t materialId[2]{};
    std::uint32_t reserved[2]{};
};

struct alignas(16) GpuIndirectHistoryV1
{
    float mean[3]{};
    float meanPad{};
    float secondMoment[3]{};
    std::uint32_t samples{};
};

struct alignas(16) GpuIndirectResultV1
{
    float mean[3]{};
    // The reason the incoming history was or was not believed, so a frame that
    // will not converge can be read as which gate rejected it rather than as
    // "the filter is slow".
    std::uint32_t reason{};
    float variance[3]{};
    std::uint32_t samples{};
};

// One storage buffer each: current pixels, previous pixels, incoming history,
// outgoing results.
inline constexpr std::uint32_t kIndirectBindingCount = 4;

[[nodiscard]] GpuIndirectPixelV1 BuildGpuIndirectPixel(
    const SurfaceSample& surface,
    const std::array<float, 2>& motion,
    const std::array<float, 3>& radiance) noexcept;

[[nodiscard]] GpuIndirectHistoryV1 BuildGpuIndirectHistory(
    const HistorySample& history) noexcept;

[[nodiscard]] HistorySample ReadGpuIndirectHistory(
    const GpuIndirectHistoryV1& record) noexcept;

static_assert(sizeof(GpuIndirectPixelV1) == 64);
static_assert(sizeof(GpuIndirectHistoryV1) == 32);
static_assert(sizeof(GpuIndirectResultV1) == 32);

}
