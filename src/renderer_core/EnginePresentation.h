#pragma once

#include "renderer_api/RendererMode.h"

#include <cstdint>
#include <span>

namespace vf::renderer::wsi {

enum class SurfaceFormat : std::uint32_t
{
    Unknown = 0,
    Bgra8Srgb = 1,
    Rgba8Srgb = 2,
    Rgb10A2Unorm = 3,
    Rgba16Sfloat = 4,
};

enum class ColorSpace : std::uint32_t
{
    Unknown = 0,
    SrgbNonlinear = 1,
    ExtendedSrgbLinear = 2,
    Hdr10St2084 = 3,
};

enum class PresentMode : std::uint32_t
{
    Immediate = 0,
    Mailbox = 1,
    FifoRelaxed = 2,
    // Always supported by the specification, which is what makes it the only
    // honest fallback: anything else can be absent on a driver nobody tested.
    Fifo = 3,
};

// What the swap chain reported. Suboptimal is deliberately not an error: the
// image is presentable, it simply no longer matches the surface exactly.
enum class SwapchainStatus : std::uint8_t
{
    Ok = 0,
    Suboptimal = 1,
    OutOfDate = 2,
    SurfaceLost = 3,
    DeviceLost = 4,
    Timeout = 5,
};

enum class SwapchainAction : std::uint8_t
{
    Present = 0,
    // Present this frame, then rebuild. The image is valid.
    PresentThenRecreate = 1,
    RecreateSwapchain = 2,
    RecreateSurface = 3,
    RecreateDevice = 4,
    // Nothing is acquired and nothing is built. A zero-extent swap chain is
    // invalid, and a loop that keeps trying to make one spins.
    SkipFrame = 5,
};

enum class PresentationError : std::uint8_t
{
    None = 0,
    NoSuitableSurface,
    HdrWithoutCapableDisplay,
    HdrWithoutWideFormat,
    NoSupportedPresentMode,
    UnresolvedD3dConsumer,
    ZeroExtent,
};

enum class FullscreenPolicy : std::uint8_t
{
    // The default. Exclusive mode takes the display mode away from everything
    // else on the machine and blanks any overlay that is not a display-driver
    // one, which reaches the user as "the game broke my capture software".
    Borderless = 0,
    ExclusiveOptIn = 1,
};

struct SurfaceCandidate
{
    SurfaceFormat format{SurfaceFormat::Unknown};
    ColorSpace colorSpace{ColorSpace::Unknown};
};

struct DisplayPolicy
{
    bool hdrRequested{};
    bool hdrCapable{};
};

struct SurfaceSelection
{
    SurfaceCandidate candidate{};
    PresentationError error{PresentationError::None};
    bool hdrActive{};
};

// Chosen deterministically rather than by taking the first supported entry.
// First-available differs by driver, so the same build produces a different
// colour on two machines and the difference is not reproducible on either.
[[nodiscard]] SurfaceSelection SelectSurface(
    std::span<const SurfaceCandidate> candidates,
    const DisplayPolicy& policy) noexcept;

struct PresentModeRequest
{
    bool vsync{true};
    // Only when the caller has said it will accept a tear on a late frame.
    // Relaxed FIFO is not a free upgrade; it is a different contract.
    bool tolerateLateTearing{};
};

struct PresentModeSelection
{
    PresentMode mode{PresentMode::Fifo};
    PresentationError error{PresentationError::None};
};

[[nodiscard]] PresentModeSelection SelectPresentMode(
    std::span<const PresentMode> supported,
    const PresentModeRequest& request) noexcept;

struct SurfaceState
{
    SurfaceFormat format{SurfaceFormat::Unknown};
    ColorSpace colorSpace{ColorSpace::Unknown};
    PresentMode presentMode{PresentMode::Fifo};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t displayId{};
};

// Whether moving from one surface state to another needs the swap chain
// rebuilt. Every field here changes what the images mean, not merely how many
// there are.
[[nodiscard]] bool TransitionRequiresRecreate(
    const SurfaceState& previous,
    const SurfaceState& next) noexcept;

[[nodiscard]] SwapchainAction ResolveStatus(
    SwapchainStatus status,
    bool minimized) noexcept;

struct PacingState
{
    double lastPresentSeconds{};
    double smoothedFrameSeconds{};
    bool established{};
};

struct PacingDecision
{
    // How long to yield for. Yielding, never spinning: a spin burns a core to
    // hit a deadline the scheduler would have hit anyway, and it is measured
    // as the renderer being expensive.
    double sleepSeconds{};
    PacingState next{};
    bool present{true};
    // Never true. Idling the device once a frame serialises the pipeline and
    // turns every measurement into a measurement of the wait.
    bool deviceIdle{};
    bool busyWait{};
};

[[nodiscard]] PacingDecision Pace(
    const PacingState& state,
    double nowSeconds,
    double targetFrameSeconds) noexcept;

[[nodiscard]] FullscreenPolicy SelectFullscreenPolicy(
    bool exclusiveRequested,
    bool overlaysPresent) noexcept;

// Everything still reading a D3D resource once Vulkan owns presentation. The
// island is retired when this list is empty of unresolved entries, and not
// before: a reader nobody found keeps reading a resource nobody creates.
enum class ConsumerKind : std::uint8_t
{
    EngineReader = 0,
    Middleware = 1,
    Plugin = 2,
    Capture = 3,
    // Found by an audit that could not attribute it. Blocks retirement.
    Unknown = 4,
};

enum class ResolutionKind : std::uint8_t
{
    NativeBackend = 0,
    ImportedLayer = 1,
    // Deliberately scoped: a facade that keeps the reader working without the
    // real resource behind it, with the scope written down.
    ScopedFacade = 2,
    Unresolved = 3,
};

struct ConsumerRecord
{
    std::uint64_t ownerId{};
    ConsumerKind kind{ConsumerKind::Unknown};
    ResolutionKind resolution{ResolutionKind::Unresolved};
};

struct AuditResult
{
    std::uint32_t resolved{};
    std::uint32_t unresolved{};
    std::uint32_t unattributed{};
    std::uint64_t firstUnresolvedOwner{};

    [[nodiscard]] bool MayRetireD3d() const noexcept
    {
        return unresolved == 0 && unattributed == 0;
    }
};

[[nodiscard]] AuditResult AuditConsumers(
    std::span<const ConsumerRecord> consumers) noexcept;

// Presentation policy is independent of scene and frame-graph semantics, so
// moving between Native and Takeover changes how the image reaches the screen
// and nothing about what is stored. A migration here would make the fallback
// mode unusable as a fallback.
[[nodiscard]] bool SwitchRequiresMigration(
    RendererMode from,
    RendererMode to) noexcept;

[[nodiscard]] const char* ToString(PresentationError error) noexcept;
[[nodiscard]] const char* ToString(PresentMode mode) noexcept;
[[nodiscard]] const char* ToString(SwapchainAction action) noexcept;

}
