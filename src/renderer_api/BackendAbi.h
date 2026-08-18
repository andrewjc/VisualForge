#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace vf::renderer::abi {

constexpr std::uint32_t kBackendAbiMajor = 1;
constexpr std::uint32_t kBackendAbiPhase4Minor = 0;
constexpr std::uint32_t kBackendAbiPhase5Minor = 1;
constexpr std::uint32_t kBackendAbiPhase6Minor = 2;
constexpr std::uint32_t kBackendAbiPhase8Minor = 3;
constexpr std::uint32_t kBackendAbiPhase9Minor = 4;
constexpr std::uint32_t kBackendAbiPhase10Minor = 5;
constexpr std::uint32_t kBackendAbiPhase11Minor = 6;
constexpr std::uint32_t kBackendAbiPhase13Minor = 7;
constexpr std::uint32_t kBackendAbiPhase14Minor = 8;
constexpr std::uint32_t kBackendAbiPhase16Minor = 9;
constexpr std::uint32_t kBackendAbiPhase17Minor = 10;
constexpr std::uint32_t kBackendAbiPhase20Minor = 11;
constexpr std::uint32_t kBackendAbiPhase23Minor = 12;
constexpr std::uint32_t kBackendAbiTextureLibraryMinor = 13;
// The frame request carries a texture library generation.
constexpr std::uint32_t kBackendAbiLibraryGenerationMinor = 14;
constexpr std::uint32_t kBackendAbiMinor = 15;
constexpr char kBackendQueryExport[] = "VFRenderer_QueryInterface";
constexpr std::uint32_t kBridgeImageCount = 3;

enum class Result : std::uint32_t
{
    Success,
    InvalidArgument,
    AbiMajorMismatch,
    VulkanLoaderUnavailable,
    InstanceCreationFailed,
    ValidationLayerUnavailable,
    AdapterLuidNotFound,
    RequiredCapabilityMissing,
    QueueFamilyNotFound,
    DeviceCreationFailed,
    InternalFailure,
    BridgeUnsupported,
    BridgeAlreadyCreated,
    BridgeNotCreated,
    BridgeCreateFailed,
    BridgeSubmitFailed,
    BridgeStaleEpoch,
    RasterUnsupported,
    RasterAlreadyCreated,
    RasterNotCreated,
    RasterCreateFailed,
    RasterInvalidPacket,
    RasterRenderFailed
};

enum Capability : std::uint64_t
{
    Vulkan12 = 1ull << 0,
    GraphicsComputeQueue = 1ull << 1,
    BufferDeviceAddress = 1ull << 2,
    DescriptorIndexing = 1ull << 3,
    TimelineSemaphore = 1ull << 4,
    Synchronization2 = 1ull << 5,
    AccelerationStructure = 1ull << 6,
    RayTracingPipeline = 1ull << 7,
    BcTextureFormats = 1ull << 8,
    ExternalMemoryWin32 = 1ull << 9,
    ExternalSemaphoreWin32 = 1ull << 10,
    D3d11TextureInterop = 1ull << 11,
    D3d12FenceInterop = 1ull << 12,
    DebugUtils = 1ull << 13
};

constexpr std::uint64_t kRequiredCapabilities =
    Capability::Vulkan12 |
    Capability::GraphicsComputeQueue |
    Capability::BufferDeviceAddress |
    Capability::DescriptorIndexing |
    Capability::TimelineSemaphore |
    Capability::Synchronization2 |
    Capability::AccelerationStructure |
    Capability::RayTracingPipeline |
    Capability::BcTextureFormats |
    Capability::ExternalMemoryWin32 |
    Capability::ExternalSemaphoreWin32;

struct alignas(8) AdapterLuid
{
    std::uint32_t lowPart{};
    std::int32_t highPart{};

    friend bool operator==(const AdapterLuid&, const AdapterLuid&) = default;
};

using LogCallback = void (*)(
    void* userData,
    std::uint32_t level,
    const char* message);

struct alignas(8) HostCallbacksV1
{
    std::uint32_t structSize{};
    std::uint32_t reserved0{};
    void* userData{};
    LogCallback log{};
    std::uint64_t reserved[4]{};
};

struct alignas(8) ProbeRequestV1
{
    std::uint32_t structSize{};
    std::uint32_t enableValidation{};
    AdapterLuid adapterLuid{};
    std::uint64_t requiredCapabilities{kRequiredCapabilities};
    std::uint64_t reserved[4]{};
};

struct alignas(8) CapabilityReportV1
{
    std::uint32_t structSize{};
    Result result{Result::InternalFailure};
    std::uint32_t apiVersion{};
    std::uint32_t driverVersion{};
    std::uint32_t vendorId{};
    std::uint32_t deviceId{};
    std::uint32_t queueFamilyIndex{0xFFFFFFFFu};
    std::uint32_t validationErrorCount{};
    AdapterLuid adapterLuid{};
    std::uint64_t supportedCapabilities{};
    std::uint64_t missingRequiredCapabilities{};
    std::uint32_t maxPerStageSampledImages{};
    std::uint32_t maxDescriptorSetSampledImages{};
    std::uint32_t maxPushConstantsSize{};
    std::uint32_t maxRayRecursionDepth{};
    std::uint32_t shaderGroupHandleSize{};
    std::uint32_t accelerationStructureScratchAlignment{};
    char deviceName[128]{};
    char driverName[64]{};
    std::uint64_t reserved[8]{};
};

enum class BridgeFormat : std::uint32_t
{
    Unknown,
    R8G8B8A8Unorm
};

enum BridgeCreateFlag : std::uint32_t
{
    BridgeCreateValidation = 1u << 0
};

struct alignas(8) BridgeCreateRequestV1
{
    std::uint32_t structSize{};
    std::uint32_t flags{};
    AdapterLuid adapterLuid{};
    std::uint64_t epoch{};
    std::uint32_t width{};
    std::uint32_t height{};
    BridgeFormat format{BridgeFormat::Unknown};
    std::uint32_t imageCount{};
    std::uint64_t fenceHandle{};
    std::uint64_t imageHandles[kBridgeImageCount]{};
    std::uint64_t reserved[4]{};
};

struct alignas(8) BridgePatternRequestV1
{
    std::uint32_t structSize{};
    std::uint32_t imageIndex{};
    std::uint64_t epoch{};
    std::uint64_t releaseValue{};
    std::uint64_t readyValue{};
    std::uint64_t frameIndex{};
    // Rendered pixels to present, tightly packed R8G8B8A8 at the bridge
    // extent. Zero means "generate the built-in test pattern", so a caller
    // that fills only the original prefix behaves exactly as before. Carved
    // out of the former reserved block, so the size does not move.
    std::uint64_t pixelData{};
    std::uint64_t pixelSize{};
    std::uint64_t reserved[2]{};
};

struct alignas(8) BridgeStatusV1
{
    std::uint32_t structSize{};
    Result result{Result::InternalFailure};
    std::uint64_t epoch{};
    std::uint64_t lastReleaseValue{};
    std::uint64_t lastReadyValue{};
    std::uint64_t submissionCount{};
    std::uint32_t validationErrorCount{};
    std::uint32_t imageCount{};
    std::uint64_t reserved[4]{};
};

enum RasterCreateFlag : std::uint32_t
{
    RasterCreateValidation = 1u << 0,
    // Accept any capable device when no device carries the requested LUID.
    // Set by offline tools, never by the in-game path: there the adapter is
    // the one the game already presents on and a different one would render
    // into an image the swap chain cannot import.
    //
    // It exists because a DXGI adapter list is not a list of Vulkan devices.
    // Measured on a machine with a headset runtime installed: two adapters
    // report the same name and the same twenty-three gigabytes, one of them
    // mirrored by the runtime and carrying a LUID no Vulkan device has, and
    // it enumerates first. Nothing visible to the tool distinguishes them.
    RasterCreateAnyAdapter = 1u << 1
};

// Per-frame switches for the post chain. Bloom is off unless asked for,
// because the plan requires a disabled effect to be an exact identity and the
// only way to demonstrate that is to render the same frame both ways.
enum RasterFrameFlag : std::uint32_t
{
    RasterFrameBloom = 1u << 0,
    // Records the frame without compositing its blended layer.
    //
    // Isolation, not a feature switch. Measuring what the blended layer added
    // means rendering the same frame twice and differencing, and the frame
    // has to be the *same* one: the previous baseline removed the transparent
    // table instead, but the acceleration structure excludes blended geometry
    // because that table names it, so the baseline reflected and shadowed
    // from surfaces the composite render did not have. The two then differed
    // by the blended layer plus a different set of occluders, which is not
    // what the comparison claims to measure.
    RasterFrameSuppressTransparentComposite = 1u << 1
};

struct alignas(8) RasterCreateRequestV1
{
    std::uint32_t structSize{};
    std::uint32_t flags{};
    AdapterLuid adapterLuid{};
    std::uint64_t reserved[6]{};
};

struct alignas(8) RasterFrameRequestV1
{
    std::uint32_t structSize{};
    std::uint32_t flags{};
    std::uint64_t packetData{};
    std::uint64_t packetSize{};
    std::uint64_t outputData{};
    std::uint64_t outputCapacity{};
    std::uint32_t outputRowPitch{};
    std::uint32_t reserved0{};
    std::uint64_t textureData{};
    std::uint64_t textureSize{};
    std::uint64_t materialData{};
    std::uint64_t materialSize{};
    std::uint64_t frameData{};
    std::uint64_t frameSize{};
    std::uint64_t sceneData{};
    std::uint64_t sceneSize{};
    std::uint64_t gbufferData{};
    std::uint64_t gbufferCapacity{};
    std::uint64_t deformationData{};
    std::uint64_t deformationSize{};
    std::uint64_t deformationOutputData{};
    std::uint64_t deformationOutputCapacity{};
    std::uint64_t terrainData{};
    std::uint64_t terrainSize{};
    // Phase 16: one family record per scene object, and a float readback of
    // the HDR colour target. The readback is a separate pair so a caller can
    // supply families without also asking for the colour target.
    std::uint64_t familyData{};
    std::uint64_t familySize{};
    std::uint64_t hdrData{};
    std::uint64_t hdrCapacity{};
    // Phase 17: the captured light list and environment for this frame.
    std::uint64_t lightData{};
    std::uint64_t lightSize{};
    // Phase 20: the temporal half of the indirect pass. The current and
    // previous pixel records and the incoming history go in, one result per
    // pixel comes out. Separate from the raster path because accumulation is
    // a pass over pixels rather than over geometry, and a caller that only
    // wants a traced frame should not have to supply a history for it.
    std::uint64_t indirectCurrentData{};
    std::uint64_t indirectPreviousData{};
    std::uint64_t indirectHistoryData{};
    std::uint64_t indirectPixelCount{};
    std::uint64_t indirectResultData{};
    std::uint64_t indirectResultCapacity{};
    // Extent and epoch agreement, which reprojection needs and which cannot be
    // derived from the pixel records: a camera cut invalidates every pixel at
    // once whatever the surfaces say.
    std::uint32_t indirectWidth{};
    std::uint32_t indirectHeight{};
    std::uint32_t indirectEpochMatches{};
    std::uint32_t indirectReserved{};
    // Phase 23: the post chain's bloom rules. Caller data like the light list,
    // not a constant the backend invents: the threshold and the knee decide
    // what blooms, and a backend holding its own copy would be a second
    // source of truth for a value the contract already owns. All zero means
    // the caller declared none and the backend uses the contract defaults.
    float bloomThreshold{};
    float bloomKnee{};
    float bloomIntensity{};
    float bloomReserved{};
    // The frame's captured textures (EngineTexture's texture library packet),
    // resolved per material through RasterMaterialV1::textureIndex. Caller
    // data like the light list: the backend does not invent a fallback
    // texture set of its own. Absent means every material's index is read
    // but nothing is bound for it, which is a decode-time refusal rather
    // than a silently wrong image.
    std::uint64_t textureLibraryData{};
    std::uint64_t textureLibrarySize{};
    // Changes exactly when the library's contents change, and never otherwise.
    //
    // Without it the backend has no cheap way to tell one frame's library from
    // the next, so it decoded the whole packet and hashed it every frame
    // before discovering nothing had changed. At a hundred megabytes that was
    // most of a frame. Zero means the caller does not track a generation and
    // the backend falls back to hashing the bytes.
    std::uint64_t textureLibraryGeneration{};
    // How many rays the diffuse bounce traces per pixel this frame.
    //
    // A declared policy rather than a constant compiled into the shader.
    // Eight is what the contract's oracle integrates and what the fixture is
    // gated against, and it is also a third of a live mirrored frame --
    // measured, by alternating the term off in windows: 102,647 us with it
    // against 68,475 us without. A live frame and a comparison frame do not
    // want the same estimate, and the estimator is unbiased in the count, so
    // this is a knob the frame is entitled to set rather than a quality
    // setting hidden in a shader.
    //
    // Zero means the backend's own default, which is the contract's eight, so
    // a caller built against an older header traces exactly what it did
    // before.
    std::uint32_t indirectRaysPerPixel{};
    std::uint32_t reservedIndirect{};
};

struct alignas(8) RasterStatusV1
{
    std::uint32_t structSize{};
    Result result{Result::InternalFailure};
    std::uint64_t frameIndex{};
    std::uint64_t submissionCount{};
    std::uint64_t extentGeneration{};
    std::uint64_t outputBytes{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t validationErrorCount{};
    std::uint32_t packetError{};
    char diagnostic[96]{};
    std::uint64_t reserved[4]{};
};

using ProbeFunction = Result (*)(
    void* context,
    const ProbeRequestV1* request,
    CapabilityReportV1* report);
using ShutdownFunction = void (*)(void* context);
using CreateBridgeFunction = Result (*)(
    void* context,
    const BridgeCreateRequestV1* request,
    BridgeStatusV1* status);
using SubmitBridgePatternFunction = Result (*)(
    void* context,
    const BridgePatternRequestV1* request,
    BridgeStatusV1* status);
using DestroyBridgeFunction = Result (*)(
    void* context,
    BridgeStatusV1* status);
using CreateRasterFunction = Result (*)(
    void* context,
    const RasterCreateRequestV1* request,
    RasterStatusV1* status);
using RenderRasterFrameFunction = Result (*)(
    void* context,
    const RasterFrameRequestV1* request,
    RasterStatusV1* status);
using DestroyRasterFunction = Result (*)(
    void* context,
    RasterStatusV1* status);

struct alignas(8) BackendApiV1
{
    std::uint32_t structSize{};
    std::uint32_t abiMajor{};
    std::uint32_t abiMinor{};
    std::uint32_t reserved0{};
    void* context{};
    ProbeFunction probe{};
    ShutdownFunction shutdown{};
    std::uint64_t reserved[5]{};
    CreateBridgeFunction createBridge{};
    SubmitBridgePatternFunction submitBridgePattern{};
    DestroyBridgeFunction destroyBridge{};
    CreateRasterFunction createRaster{};
    RenderRasterFrameFunction renderRasterFrame{};
    DestroyRasterFunction destroyRaster{};
};

using QueryInterfaceFunction = Result (*)(
    std::uint32_t hostAbiMajor,
    std::uint32_t hostAbiMinor,
    const HostCallbacksV1* callbacks,
    BackendApiV1* api);

constexpr std::size_t kHostCallbacksV1RequiredSize =
    offsetof(HostCallbacksV1, reserved);
constexpr std::size_t kProbeRequestV1RequiredSize =
    offsetof(ProbeRequestV1, reserved);
constexpr std::size_t kCapabilityReportV1RequiredSize =
    offsetof(CapabilityReportV1, deviceName);
constexpr std::size_t kBackendApiV1RequiredSize =
    offsetof(BackendApiV1, reserved);
constexpr std::size_t kBridgeCreateRequestV1RequiredSize =
    offsetof(BridgeCreateRequestV1, reserved);
constexpr std::size_t kBridgePatternRequestV1RequiredSize =
    offsetof(BridgePatternRequestV1, pixelData);
// A caller that reaches this size may hand the bridge rendered pixels.
constexpr std::size_t kBridgePatternRequestV1PixelRequiredSize =
    offsetof(BridgePatternRequestV1, reserved);
constexpr std::size_t kBridgeStatusV1RequiredSize =
    offsetof(BridgeStatusV1, reserved);
constexpr std::size_t kRasterCreateRequestV1RequiredSize =
    offsetof(RasterCreateRequestV1, reserved);
constexpr std::size_t kRasterFrameRequestV1RequiredSize =
    offsetof(RasterFrameRequestV1, textureData);
constexpr std::size_t kRasterFrameRequestV1TextureRequiredSize =
    offsetof(RasterFrameRequestV1, textureSize) +
    sizeof(RasterFrameRequestV1::textureSize);
constexpr std::size_t kRasterFrameRequestV1MaterialRequiredSize =
    offsetof(RasterFrameRequestV1, materialSize) +
    sizeof(RasterFrameRequestV1::materialSize);
constexpr std::size_t kRasterFrameRequestV1FrameRequiredSize =
    offsetof(RasterFrameRequestV1, frameSize) +
    sizeof(RasterFrameRequestV1::frameSize);
constexpr std::size_t kRasterFrameRequestV1SceneInputRequiredSize =
    offsetof(RasterFrameRequestV1, sceneSize) +
    sizeof(RasterFrameRequestV1::sceneSize);
constexpr std::size_t kRasterFrameRequestV1SceneRequiredSize =
    offsetof(RasterFrameRequestV1, gbufferCapacity) +
    sizeof(RasterFrameRequestV1::gbufferCapacity);
constexpr std::size_t kRasterFrameRequestV1DeformationInputRequiredSize =
    offsetof(RasterFrameRequestV1, deformationSize) +
    sizeof(RasterFrameRequestV1::deformationSize);
constexpr std::size_t kRasterFrameRequestV1DeformationRequiredSize =
    offsetof(RasterFrameRequestV1, deformationOutputCapacity) +
    sizeof(RasterFrameRequestV1::deformationOutputCapacity);
constexpr std::size_t kRasterFrameRequestV1TerrainRequiredSize =
    offsetof(RasterFrameRequestV1, terrainSize) +
    sizeof(RasterFrameRequestV1::terrainSize);
constexpr std::size_t kRasterFrameRequestV1FamilyRequiredSize =
    offsetof(RasterFrameRequestV1, familySize) +
    sizeof(RasterFrameRequestV1::familySize);
constexpr std::size_t kRasterFrameRequestV1HdrRequiredSize =
    offsetof(RasterFrameRequestV1, hdrCapacity) +
    sizeof(RasterFrameRequestV1::hdrCapacity);
constexpr std::size_t kRasterFrameRequestV1LightRequiredSize =
    offsetof(RasterFrameRequestV1, lightSize) +
    sizeof(RasterFrameRequestV1::lightSize);
constexpr std::size_t kRasterFrameRequestV1IndirectRequiredSize =
    offsetof(RasterFrameRequestV1, indirectReserved) +
    sizeof(RasterFrameRequestV1::indirectReserved);
constexpr std::size_t kRasterFrameRequestV1BloomRequiredSize =
    offsetof(RasterFrameRequestV1, bloomReserved) +
    sizeof(RasterFrameRequestV1::bloomReserved);
constexpr std::size_t kRasterFrameRequestV1TextureLibraryRequiredSize =
    offsetof(RasterFrameRequestV1, textureLibrarySize) +
    sizeof(RasterFrameRequestV1::textureLibrarySize);
constexpr std::size_t kRasterFrameRequestV1LibraryGenerationRequiredSize =
    offsetof(RasterFrameRequestV1, textureLibraryGeneration) +
    sizeof(RasterFrameRequestV1::textureLibraryGeneration);
constexpr std::size_t kRasterFrameRequestV1IndirectRaysRequiredSize =
    offsetof(RasterFrameRequestV1, indirectRaysPerPixel) +
    sizeof(RasterFrameRequestV1::indirectRaysPerPixel);
// The contract's count, and the backend's default when a frame declares none.
constexpr std::uint32_t kDefaultIndirectRaysPerPixel = 8;
constexpr std::size_t kRasterStatusV1RequiredSize =
    offsetof(RasterStatusV1, reserved);
constexpr std::size_t kBackendApiV1BridgeRequiredSize =
    offsetof(BackendApiV1, destroyBridge) +
    sizeof(BackendApiV1::destroyBridge);
constexpr std::size_t kBackendApiV1RasterRequiredSize =
    offsetof(BackendApiV1, destroyRaster) +
    sizeof(BackendApiV1::destroyRaster);

static_assert(sizeof(AdapterLuid) == 8);
static_assert(std::is_standard_layout_v<HostCallbacksV1>);
static_assert(std::is_standard_layout_v<ProbeRequestV1>);
static_assert(std::is_standard_layout_v<CapabilityReportV1>);
static_assert(std::is_standard_layout_v<BridgeCreateRequestV1>);
static_assert(std::is_standard_layout_v<BridgePatternRequestV1>);
static_assert(std::is_standard_layout_v<BridgeStatusV1>);
static_assert(std::is_standard_layout_v<RasterCreateRequestV1>);
static_assert(std::is_standard_layout_v<RasterFrameRequestV1>);
static_assert(std::is_standard_layout_v<RasterStatusV1>);
static_assert(std::is_standard_layout_v<BackendApiV1>);

}
