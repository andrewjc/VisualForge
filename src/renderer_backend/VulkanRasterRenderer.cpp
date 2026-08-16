#include "renderer_backend/VulkanRasterRenderer.h"

#include "renderer_api/RasterPacket.h"
#include "renderer_core/EngineDeformation.h"
#include "renderer_core/EngineIndirect.h"
#include "renderer_core/EnginePostChain.h"
#include "renderer_core/EngineMaterial.h"
#include "renderer_core/EngineAcceleration.h"
#include "renderer_core/EngineLighting.h"
#include "renderer_core/EngineMaterialFamily.h"
#include "renderer_core/EngineScene.h"
#include "renderer_core/EngineTransparency.h"
#include "renderer_core/EngineWater.h"
#include "renderer_core/EngineTerrain.h"
#include "renderer_core/EngineTexture.h"
#include "renderer_core/EngineView.h"
#include "renderer_core/EngineVertex.h"
#include "renderer_core/EngineVisibility.h"
#include "renderer_core/FrameGraph.h"
#include "renderer_trace/Crc32.h"

#include <vulkan/vulkan.h>

#include "accumulate.comp.spv.inc"
#include "deform.comp.spv.inc"
#include "mesh.frag.spv.inc"
#include "mesh.vert.spv.inc"
#include "material.frag.spv.inc"
#include "scene.frag.spv.inc"
#include "scene.vert.spv.inc"
#include "alpha_depth.frag.spv.inc"
#include "alpha_scene.frag.spv.inc"
#include "family_scene.frag.spv.inc"
#include "family_scene_rq.frag.spv.inc"
#include "terrain.frag.spv.inc"
#include "terrain.vert.spv.inc"
#include "tone_map.frag.spv.inc"
#include "tone_map.vert.spv.inc"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace vf::renderer::backend {

namespace {

// StraightAlpha, Premultiplied, Additive and Multiply. Opaque is not a
// blended pipeline and never reaches the transparent pass.
constexpr std::size_t kBlendedPipelineCount = 4;

// Mirrors the push block in phase06/tone_map.frag. Bloom's threshold and knee
// travel with the exposure because both are decisions about the display side
// of the chain and both have to reach the same pass.
struct TonePushConstants
{
    float exposure{1.0f};
    float bloomThreshold{};
    float bloomKnee{};
    float bloomIntensity{};
};

// Mirrors the push block in phase20/accumulate_layout.glsl. The extent and
// the epoch cannot ride in a per-pixel record: a camera cut invalidates every
// pixel at once whatever the surfaces say.
struct IndirectPushConstants
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t pixelCount{};
    std::uint32_t epochMatches{};
};

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kOutputFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr std::uint64_t kGpuTimeoutNanoseconds = 30'000'000'000ull;
constexpr std::uint64_t kMaximumTexturePacketBytes =
    64ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaximumMaterialPacketBytes =
    192ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaximumFramePacketBytes =
    16ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaximumScenePacketBytes =
    16ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaximumDeformationPacketBytes =
    64ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaximumTerrainPacketBytes =
    256ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaximumFamilyPacketBytes =
    32ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaximumLightPacketBytes =
    8ull * 1024ull * 1024ull;
// kHdrFormat is R16G16B16A16_SFLOAT: four half-floats per pixel.
constexpr VkDeviceSize kHdrPixelBytes = 8;
// Slot 3 of the sampled resources is the landscape layer array. Slots 0..2
// stay the material bundle's textures at bindings 1..3.
constexpr std::size_t kTerrainLayerTextureSlot = 3;
// Deformed output is sub-allocated from a ring so a frame never overwrites
// bytes an earlier submission is still reading.
constexpr VkDeviceSize kDeformRingAlignment = 256;
constexpr VkDeviceSize kMinimumDeformRingBytes = 1ull * 1024ull * 1024ull;
// The world clear color is shared by the HDR attachment and the mirrored
// G-buffer so uncovered pixels agree with the CPU oracle exactly.
constexpr std::array<float, 4> kWorldClearColor{
    0.01f, 0.021f, 0.04f, 1.0f};
// Mirrored G-buffer planes, in the interleave order of GBufferPixelV1.
// Full float and integer precision keeps the readback exactly comparable
// with the CPU oracle instead of quantizing evidence away.
constexpr std::array<VkFormat, scene::kSceneGBufferPlaneCount>
    kGBufferFormats{
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_R32G32B32A32_UINT,
        // The reactive mask. Full float like the shading planes so the
        // readback compares exactly against the oracle rather than through
        // a quantisation nobody can account for.
        VK_FORMAT_R32G32B32A32_SFLOAT,
    };

struct DebugState
{
    std::atomic<std::uint32_t> errors{0};
    abi::HostCallbacksV1 callbacks{};
};

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData)
{
    auto* state = static_cast<DebugState*>(userData);
    if (state == nullptr) {
        return VK_FALSE;
    }
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        state->errors.fetch_add(1, std::memory_order_relaxed);
    }
    if ((severity &
         (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) != 0 &&
        state->callbacks.log != nullptr) {
        state->callbacks.log(
            state->callbacks.userData,
            (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0
                ? 3u : 2u,
            callbackData != nullptr && callbackData->pMessage != nullptr
                ? callbackData->pMessage
                : "Vulkan raster validation message");
    }
    return VK_FALSE;
}

template <class T, class Enumerate>
bool EnumerateVector(std::vector<T>& output, Enumerate enumerate)
{
    std::uint32_t count{};
    auto result = enumerate(&count, nullptr);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return false;
    }
    output.resize(count);
    while (count != 0) {
        result = enumerate(&count, output.data());
        if (result == VK_SUCCESS) {
            output.resize(count);
            return true;
        }
        if (result != VK_INCOMPLETE) {
            return false;
        }
        output.resize(count);
    }
    return true;
}

bool HasExtension(
    const std::span<const VkExtensionProperties> extensions,
    const char* name) noexcept
{
    return std::any_of(extensions.begin(), extensions.end(),
        [name](const VkExtensionProperties& extension) {
            return std::strcmp(extension.extensionName, name) == 0;
        });
}

bool HasLayer(
    const std::span<const VkLayerProperties> layers,
    const char* name) noexcept
{
    return std::any_of(layers.begin(), layers.end(),
        [name](const VkLayerProperties& layer) {
            return std::strcmp(layer.layerName, name) == 0;
        });
}

abi::AdapterLuid ReadLuid(
    const VkPhysicalDeviceIDProperties& properties) noexcept
{
    abi::AdapterLuid luid{};
    static_assert(sizeof(luid) == VK_LUID_SIZE);
    std::memcpy(&luid, properties.deviceLUID, sizeof(luid));
    return luid;
}

std::uint32_t FindMemoryType(
    const VkPhysicalDeviceMemoryProperties& properties,
    const std::uint32_t typeBits,
    const VkMemoryPropertyFlags required,
    const VkMemoryPropertyFlags preferred = 0) noexcept
{
    auto fallback = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t index = 0;
         index < properties.memoryTypeCount; ++index) {
        const auto flags = properties.memoryTypes[index].propertyFlags;
        if ((typeBits & (1u << index)) == 0 ||
            (flags & required) != required) {
            continue;
        }
        if ((flags & preferred) == preferred) {
            return index;
        }
        if (fallback == std::numeric_limits<std::uint32_t>::max()) {
            fallback = index;
        }
    }
    return fallback;
}

std::size_t AlignUp(
    const std::size_t value,
    const std::size_t alignment) noexcept
{
    if (alignment == 0 || value >
        std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

bool CheckedAdd(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result) noexcept
{
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        result = std::numeric_limits<std::size_t>::max();
        return false;
    }
    result = left + right;
    return true;
}

bool CheckedMultiply(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result) noexcept
{
    if (left != 0 && right >
        std::numeric_limits<std::size_t>::max() / left) {
        result = std::numeric_limits<std::size_t>::max();
        return false;
    }
    result = left * right;
    return true;
}

void InitializeStatus(
    abi::RasterStatusV1& status,
    const abi::Result result,
    const char* diagnostic = "") noexcept
{
    const auto capacity = std::min<std::size_t>(status.structSize, sizeof(status));
    std::memset(&status, 0, capacity);
    status.structSize = sizeof(status);
    status.result = result;
    if (capacity >= offsetof(abi::RasterStatusV1, diagnostic) + 1) {
        strncpy_s(status.diagnostic, sizeof(status.diagnostic),
            diagnostic == nullptr ? "" : diagnostic, _TRUNCATE);
    }
}

VkCompareOp ToVkCompare(const raster::DepthCompare compare) noexcept
{
    switch (compare) {
    case raster::DepthCompare::Less: return VK_COMPARE_OP_LESS;
    case raster::DepthCompare::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case raster::DepthCompare::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_NEVER;
}

std::size_t PipelineIndex(
    const raster::FrontFace face,
    const raster::DepthCompare compare) noexcept
{
    return static_cast<std::size_t>(face) * 3 +
        static_cast<std::size_t>(compare);
}

VkFormat ToVkTextureFormat(const texture::TextureFormat format) noexcept
{
    using enum texture::TextureFormat;
    switch (format) {
    case R8Unorm: return VK_FORMAT_R8_UNORM;
    case R8G8Unorm: return VK_FORMAT_R8G8_UNORM;
    case R8G8B8A8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case R8G8B8A8UnormSrgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case B8G8R8A8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case B8G8R8A8UnormSrgb: return VK_FORMAT_B8G8R8A8_SRGB;
    case BC1Unorm: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case BC1UnormSrgb: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case BC2Unorm: return VK_FORMAT_BC2_UNORM_BLOCK;
    case BC2UnormSrgb: return VK_FORMAT_BC2_SRGB_BLOCK;
    case BC3Unorm: return VK_FORMAT_BC3_UNORM_BLOCK;
    case BC3UnormSrgb: return VK_FORMAT_BC3_SRGB_BLOCK;
    case BC4Unorm: return VK_FORMAT_BC4_UNORM_BLOCK;
    case BC4Snorm: return VK_FORMAT_BC4_SNORM_BLOCK;
    case BC5Unorm: return VK_FORMAT_BC5_UNORM_BLOCK;
    case BC5Snorm: return VK_FORMAT_BC5_SNORM_BLOCK;
    case BC6HUf16: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case BC6HSf16: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case BC7Unorm: return VK_FORMAT_BC7_UNORM_BLOCK;
    case BC7UnormSrgb: return VK_FORMAT_BC7_SRGB_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
    }
}

VkFilter ToVkFilter(const texture::TextureFilter filter) noexcept
{
    return filter == texture::TextureFilter::Linear
        ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

VkSamplerMipmapMode ToVkMipFilter(
    const texture::TextureFilter filter) noexcept
{
    return filter == texture::TextureFilter::Linear
        ? VK_SAMPLER_MIPMAP_MODE_LINEAR
        : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

VkSamplerAddressMode ToVkAddress(
    const texture::TextureAddressMode address) noexcept
{
    switch (address) {
    case texture::TextureAddressMode::Wrap:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case texture::TextureAddressMode::Mirror:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case texture::TextureAddressMode::Clamp:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case texture::TextureAddressMode::Border:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case texture::TextureAddressMode::MirrorOnce:
        return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    }
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

VkCompareOp ToVkCompare(const texture::TextureCompareOp compare) noexcept
{
    switch (compare) {
    case texture::TextureCompareOp::Never: return VK_COMPARE_OP_NEVER;
    case texture::TextureCompareOp::Less: return VK_COMPARE_OP_LESS;
    case texture::TextureCompareOp::Equal: return VK_COMPARE_OP_EQUAL;
    case texture::TextureCompareOp::LessOrEqual:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case texture::TextureCompareOp::Greater: return VK_COMPARE_OP_GREATER;
    case texture::TextureCompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case texture::TextureCompareOp::GreaterOrEqual:
        return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case texture::TextureCompareOp::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

bool ToVkBorderColor(
    const float (&color)[4], VkBorderColor& border) noexcept
{
    const auto equal = [&color](
        const float r, const float g, const float b, const float a) {
        return color[0] == r && color[1] == g &&
            color[2] == b && color[3] == a;
    };
    if (equal(0.0f, 0.0f, 0.0f, 0.0f)) {
        border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        return true;
    }
    if (equal(0.0f, 0.0f, 0.0f, 1.0f)) {
        border = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        return true;
    }
    if (equal(1.0f, 1.0f, 1.0f, 1.0f)) {
        border = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        return true;
    }
    return false;
}

}

struct VulkanRasterRenderer::Impl
{
    struct Image
    {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    };

    struct Buffer
    {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        void* mapped{};
        VkDeviceSize capacity{};
        bool coherent{};
    };

    struct UploadLayout
    {
        VkDeviceSize vertexOffset{};
        VkDeviceSize indexOffset{};
        VkDeviceSize materialOffset{};
        VkDeviceSize materialStride{};
        VkDeviceSize phase9StaticOffset{};
        VkDeviceSize phase9DynamicOffset{};
        VkDeviceSize phase10ViewOffset{};
        VkDeviceSize phase11ObjectsOffset{};
        VkDeviceSize phase11ObjectBytes{};
        VkDeviceSize phase12InstancesOffset{};
        VkDeviceSize phase12InstanceBytes{};
        VkDeviceSize phase15VisibilityOffset{};
        VkDeviceSize phase15VisibilityBytes{};
        VkDeviceSize phase16FamilyOffset{};
        VkDeviceSize phase16FamilyBytes{};
        VkDeviceSize phase17LightOffset{};
        VkDeviceSize phase17LightBytes{};
        VkDeviceSize phase17EnvironmentOffset{};
        VkDeviceSize phase17EnvironmentBytes{};
        VkDeviceSize phase14CellsOffset{};
        VkDeviceSize phase14CellBytes{};
        VkDeviceSize phase14LayersOffset{};
        VkDeviceSize phase14LayerBytes{};
        VkDeviceSize phase14VertexOffset{};
        VkDeviceSize phase14VertexBytes{};
        VkDeviceSize phase14IndexOffset{};
        VkDeviceSize phase14IndexBytes{};
        std::array<std::vector<VkDeviceSize>, 4>
            textureSubresourceOffsets;
        VkDeviceSize totalSize{};
    };

    struct SampledResource
    {
        Image image;
        VkSampler sampler{VK_NULL_HANDLE};
        texture::CapturedTexture source;
        std::uint64_t signature{};
        bool uploadPending{};
    };

    abi::HostCallbacksV1 callbacks{};
    DebugState debug;
    VkInstance instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT messenger{VK_NULL_HANDLE};
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger{};
    PFN_vkCmdBeginDebugUtilsLabelEXT beginLabel{};
    PFN_vkCmdEndDebugUtilsLabelEXT endLabel{};
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkPhysicalDeviceProperties properties{};
    // maxMemoryAllocationSize, which bounds the largest frame packet the
    // renderer can accept. Zero until the device has been chosen and queried,
    // and MaximumPacketBytes treats zero as "not reported".
    std::uint64_t maximumAllocationBytes{};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    std::uint32_t queueFamily{0xFFFFFFFFu};
    VkCommandPool commandPool{VK_NULL_HANDLE};
    VkCommandBuffer command{VK_NULL_HANDLE};
    VkFence completion{VK_NULL_HANDLE};
    VkDescriptorSetLayout materialSetLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout toneSetLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout deformSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet materialSet{VK_NULL_HANDLE};
    VkDescriptorSet toneSet{VK_NULL_HANDLE};
    VkDescriptorSet deformSet{VK_NULL_HANDLE};
    VkPipelineLayout meshPipelineLayout{VK_NULL_HANDLE};
    VkPipelineLayout scenePipelineLayout{VK_NULL_HANDLE};
    VkPipelineLayout terrainPipelineLayout{VK_NULL_HANDLE};
    VkPipelineLayout deformPipelineLayout{VK_NULL_HANDLE};
    VkPipeline deformPipeline{VK_NULL_HANDLE};
    // Phase 20: temporal accumulation, a pass over pixels rather than over
    // geometry and so compute like the deformation pass above.
    VkDescriptorSetLayout indirectSetLayout{VK_NULL_HANDLE};
    VkDescriptorSet indirectSet{VK_NULL_HANDLE};
    VkPipelineLayout indirectPipelineLayout{VK_NULL_HANDLE};
    VkPipeline indirectPipeline{VK_NULL_HANDLE};
    VkPipelineLayout tonePipelineLayout{VK_NULL_HANDLE};
    std::array<VkPipeline, 6> meshPipelines{};
    std::array<VkPipeline, 6> materialPipelines{};
    std::array<VkPipeline, 6> scenePipelines{};
    // Terrain has one raster state: back-face culled, depth-less. The engine
    // supplies the cells, so there is no per-draw state to permute.
    VkPipeline terrainPipeline{VK_NULL_HANDLE};
    // Alpha-tested geometry sets cull mode, front face, and depth compare
    // dynamically, so one pipeline covers every permutation the packet can
    // ask for instead of twenty-four static variants. The depth pipeline runs
    // the identical discard and writes depth only.
    VkPipeline alphaScenePipeline{VK_NULL_HANDLE};
    // Phase 16: one pipeline for all eight shader classes. What differs
    // between families is record data, not a technique permutation.
    VkPipeline familyScenePipeline{VK_NULL_HANDLE};
    // One per blended mode, indexed by blend::BlendMode minus one. Blend
    // factors are pipeline state in core Vulkan, so a single pipeline cannot
    // serve additive and multiply.
    std::array<VkPipeline, kBlendedPipelineCount> blendedScenePipelines{};
    VkPipeline alphaDepthPipeline{VK_NULL_HANDLE};
    VkPipeline tonePipeline{VK_NULL_HANDLE};
    VkSampler sampler{VK_NULL_HANDLE};
    std::array<SampledResource, 4> sampledResources;
    material::MaterialReplayBundle materialBundle;
    material::MaterialGpuRecords materialRecords;
    view::ViewRecordV1 viewRecord;
    view::GpuViewConstantsV1 viewConstants;
    scene::ScenePacket scenePacket;
    material::FamilyPacket familyPacket;
    lighting::LightPacket lightPacket;
    terrain::TerrainPacket terrainPacket;
    std::vector<terrain::GpuTerrainCellV1> terrainCellRecords;
    deform::DeformationPacket deformPacket;
    deform::TopologyRegistry deformTopology;
    deform::DynamicRing deformRing{0, kDeformRingAlignment};
    deform::RingAllocation deformVertexRange{};
    deform::RingAllocation deformPreviousRange{};
    Buffer deformInput;
    Buffer deformOutput;
    Buffer deformReadback;
    // Phase 20: the temporal pass reads three per-pixel arrays and writes a
    // fourth. Host visible on both sides, because the caller supplies the
    // history and reads the result back in the same frame.
    Buffer indirectInput;
    Buffer indirectOutput;
    std::uint32_t indirectPixelCount{};
    std::uint32_t indirectWidth_{};
    std::uint32_t indirectHeight_{};
    std::uint32_t indirectEpochMatches_{};
    // The frame flags the caller declared, kept for the passes that are
    // recorded after the request is out of scope.
    std::uint32_t frameFlags{};
    // The post rules the caller declared for this frame.
    struct BloomRequest
    {
        float threshold{};
        float knee{};
        float intensity{};
    } bloomRequest;
    raster::ExtentGeneration extent;
    Image hdr;
    // The colour target copied before the blended pass. A refractive surface
    // samples this rather than the live target, so what it shows does not
    // depend on which refractive draws happened to precede it.
    Image refraction;
    Image depth;
    Image output;
    std::array<Image, scene::kSceneGBufferPlaneCount> gbuffer;
    Buffer readback;
    Buffer gbufferReadback;
    // Phase 16 float colour readback. Sized with the extent it belongs to.
    Buffer hdrReadback;
    Buffer upload;
    std::uint64_t submissions{};
    bool extentInitialized{};
    bool fenceSubmitted{};
    bool phase9MaterialActive{};
    bool phase10ViewActive{};
    bool phase11SceneActive{};
    bool phase13DeformActive{};
    bool phase14TerrainActive{};
    bool phase16FamilyActive{};
    bool phase17LightingActive{};
    // Phase 18: ray query is optional. A device without it still creates and
    // the mirror falls back to unshadowed lighting rather than failing.
    bool rayQuerySupported{};
    PFN_vkCreateAccelerationStructureKHR createAccelerationStructure{};
    PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure{};
    PFN_vkGetAccelerationStructureBuildSizesKHR
        getAccelerationStructureBuildSizes{};
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuildAccelerationStructures{};
    PFN_vkGetAccelerationStructureDeviceAddressKHR
        getAccelerationStructureDeviceAddress{};
    PFN_vkGetBufferDeviceAddress getBufferDeviceAddress{};
    VkAccelerationStructureKHR blas{VK_NULL_HANDLE};
    VkAccelerationStructureKHR tlas{VK_NULL_HANDLE};
    Buffer blasStorage;
    Buffer tlasStorage;
    Buffer accelScratch;
    Buffer instanceBuffer;
    bool tlasReady{};
    bool accelBuildPending{};
    // One geometry per drawn instance, each carrying that instance's model
    // rows as a build-time transform. The packet's vertices are local space,
    // so a single identity geometry would trace a scene whose objects all sit
    // on top of each other at the origin.
    std::vector<VkAccelerationStructureGeometryKHR> pendingBlasGeometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> pendingBlasRanges;
    std::vector<std::uint32_t> pendingBlasPrimitiveCounts;
    Buffer accelTransforms;
    Buffer accelGeometryObjects;
    VkAccelerationStructureGeometryKHR pendingTlasGeometry{};
    VkAccelerationStructureBuildGeometryInfoKHR pendingBlasBuild{};
    VkAccelerationStructureBuildGeometryInfoKHR pendingTlasBuild{};
    VkAccelerationStructureBuildRangeInfoKHR pendingTlasRange{};
    // Set from the frame request before recording, because the float colour
    // copy costs a full-frame transfer that earlier phases must not pay.
    bool hdrReadbackRequested{};
    bool samplerAnisotropySupported{};
    // Per-attachment blend state. The transparent pass blends the HDR target
    // while leaving the G-buffer planes untouched, and without this feature
    // every attachment must share one blend state -- which would composite
    // particles into the G-buffer and make the reflection and indirect passes
    // treat them as opaque surfaces.
    bool independentBlendSupported{};
    bool ready{};

    void FillStatus(
        abi::RasterStatusV1& status,
        abi::Result result,
        const char* diagnostic = "") const noexcept;
    void Reset() noexcept;
    void DestroyExtent() noexcept;
    void DestroySampledTexture() noexcept;
    void DestroyBuffer(Buffer& buffer) noexcept;
    [[nodiscard]] abi::Result WaitForSubmission() noexcept;
    [[nodiscard]] abi::Result CreateInstance(bool validation) noexcept;
    [[nodiscard]] abi::Result CreateDevice(
        abi::AdapterLuid luid,
        bool anyAdapter) noexcept;
    [[nodiscard]] abi::Result CreateCoreObjects() noexcept;
    [[nodiscard]] abi::Result CreatePipelines() noexcept;
    [[nodiscard]] abi::Result CreateExtent(
        std::uint32_t width, std::uint32_t height) noexcept;
    [[nodiscard]] abi::Result CreateSceneAttachments() noexcept;
    [[nodiscard]] abi::Result CreateImage(
        VkFormat format,
        VkImageUsageFlags usage,
        VkImageAspectFlags aspect,
        Image& image) noexcept;
    [[nodiscard]] abi::Result PrepareSampledTexture(
        const texture::CapturedTexture& source,
        std::uint64_t signature,
        std::size_t slot) noexcept;
    [[nodiscard]] abi::Result CreateDeviceBuffer(
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        Buffer& buffer) noexcept;
    [[nodiscard]] abi::Result BuildAccelerationStructures(
        const raster::DecodedPacket& packet,
        const UploadLayout& layout) noexcept;
    void DestroyAccelerationStructures() noexcept;
    [[nodiscard]] abi::Result CreateHostBuffer(
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        Buffer& buffer,
        bool deviceAddress = false) noexcept;
    [[nodiscard]] UploadLayout BuildUploadLayout(
        const raster::DecodedPacket& packet) const noexcept;
    [[nodiscard]] abi::Result UploadPacket(
        const raster::DecodedPacket& packet,
        const UploadLayout& layout) noexcept;
    [[nodiscard]] abi::Result RecordAndSubmit(
        const raster::DecodedPacket& packet,
        const UploadLayout& layout) noexcept;
    // Both alpha passes issue their draws through this, so the depth prepass
    // and the colour pass cannot diverge in geometry, dynamic state, or push
    // constants — only in the pipeline that is bound.
    [[nodiscard]] abi::Result RecordAlphaDraws(
        const raster::DecodedPacket& packet,
        const UploadLayout& layout,
        const std::vector<std::size_t>& objects,
        bool depthOnly) noexcept;
    [[nodiscard]] abi::Result CopyOutput(
        const abi::RasterFrameRequestV1& request,
        const raster::DecodedPacket& packet) noexcept;
    [[nodiscard]] abi::Result CopySceneOutput(
        const abi::RasterFrameRequestV1& request) noexcept;
    [[nodiscard]] abi::Result CopyHdrOutput(
        const abi::RasterFrameRequestV1& request) noexcept;
    [[nodiscard]] abi::Result PrepareIndirect(
        const abi::RasterFrameRequestV1& request) noexcept;
    void RecordIndirect() noexcept;
    [[nodiscard]] abi::Result CopyIndirectResults(
        const abi::RasterFrameRequestV1& request) noexcept;
    [[nodiscard]] abi::Result PrepareDeformation(
        std::uint64_t timelineValue) noexcept;
    [[nodiscard]] abi::Result CopyDeformationOutput(
        const abi::RasterFrameRequestV1& request) noexcept;
};

void VulkanRasterRenderer::Impl::FillStatus(
    abi::RasterStatusV1& status,
    const abi::Result result,
    const char* diagnostic) const noexcept
{
    InitializeStatus(status, result, diagnostic);
    status.submissionCount = submissions;
    status.extentGeneration = extent.Generation();
    status.width = extent.Width();
    status.height = extent.Height();
    status.validationErrorCount =
        debug.errors.load(std::memory_order_acquire);
}

void VulkanRasterRenderer::Impl::DestroyBuffer(Buffer& buffer) noexcept
{
    if (device == VK_NULL_HANDLE) {
        buffer = {};
        return;
    }
    if (buffer.mapped != nullptr && buffer.memory != VK_NULL_HANDLE) {
        vkUnmapMemory(device, buffer.memory);
    }
    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buffer.buffer, nullptr);
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, buffer.memory, nullptr);
    }
    buffer = {};
}

void VulkanRasterRenderer::Impl::DestroyExtent() noexcept
{
    if (device == VK_NULL_HANDLE) {
        hdr = {};
        refraction = {};
        depth = {};
        output = {};
        gbuffer = {};
        readback = {};
        gbufferReadback = {};
        hdrReadback = {};
        extentInitialized = false;
        return;
    }
    DestroyBuffer(readback);
    DestroyBuffer(gbufferReadback);
    DestroyBuffer(hdrReadback);
    for (auto& plane : gbuffer) {
        if (plane.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, plane.view, nullptr);
        }
        if (plane.image != VK_NULL_HANDLE) {
            vkDestroyImage(device, plane.image, nullptr);
        }
        if (plane.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, plane.memory, nullptr);
        }
        plane = {};
    }
    for (auto* resource : {&output, &depth, &hdr, &refraction}) {
        if (resource->view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, resource->view, nullptr);
        }
        if (resource->image != VK_NULL_HANDLE) {
            vkDestroyImage(device, resource->image, nullptr);
        }
        if (resource->memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, resource->memory, nullptr);
        }
        *resource = {};
    }
    extentInitialized = false;
}

void VulkanRasterRenderer::Impl::DestroySampledTexture() noexcept
{
    for (auto& resource : sampledResources) {
        if (device != VK_NULL_HANDLE) {
            if (resource.sampler != VK_NULL_HANDLE) {
                vkDestroySampler(device, resource.sampler, nullptr);
            }
            if (resource.image.view != VK_NULL_HANDLE) {
                vkDestroyImageView(device, resource.image.view, nullptr);
            }
            if (resource.image.image != VK_NULL_HANDLE) {
                vkDestroyImage(device, resource.image.image, nullptr);
            }
            if (resource.image.memory != VK_NULL_HANDLE) {
                vkFreeMemory(device, resource.image.memory, nullptr);
            }
        }
        resource = {};
    }
    materialBundle = {};
    materialRecords = {};
    phase9MaterialActive = false;
}

abi::Result VulkanRasterRenderer::Impl::WaitForSubmission() noexcept
{
    if (!fenceSubmitted) {
        return abi::Result::Success;
    }
    const auto result = vkWaitForFences(
        device, 1, &completion, VK_TRUE, kGpuTimeoutNanoseconds);
    if (result != VK_SUCCESS) {
        return abi::Result::RasterRenderFailed;
    }
    fenceSubmitted = false;
    return abi::Result::Success;
}

void VulkanRasterRenderer::Impl::Reset() noexcept
{
    if (device != VK_NULL_HANDLE) {
        static_cast<void>(vkDeviceWaitIdle(device));
        fenceSubmitted = false;
        DestroyExtent();
        DestroySampledTexture();
        DestroyAccelerationStructures();
        DestroyBuffer(upload);
        DestroyBuffer(deformInput);
        DestroyBuffer(deformOutput);
        DestroyBuffer(deformReadback);
        DestroyBuffer(indirectInput);
        DestroyBuffer(indirectOutput);
        if (deformPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, deformPipeline, nullptr);
            deformPipeline = VK_NULL_HANDLE;
        }
        if (indirectPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, indirectPipeline, nullptr);
            indirectPipeline = VK_NULL_HANDLE;
        }
        if (indirectPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, indirectPipelineLayout, nullptr);
            indirectPipelineLayout = VK_NULL_HANDLE;
        }
        if (deformPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, deformPipelineLayout, nullptr);
            deformPipelineLayout = VK_NULL_HANDLE;
        }
        if (tonePipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, tonePipeline, nullptr);
            tonePipeline = VK_NULL_HANDLE;
        }
        for (auto& pipeline : meshPipelines) {
            if (pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, pipeline, nullptr);
                pipeline = VK_NULL_HANDLE;
            }
        }
        for (auto& pipeline : materialPipelines) {
            if (pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, pipeline, nullptr);
                pipeline = VK_NULL_HANDLE;
            }
        }
        for (auto& pipeline : scenePipelines) {
            if (pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, pipeline, nullptr);
                pipeline = VK_NULL_HANDLE;
            }
        }
        if (terrainPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, terrainPipeline, nullptr);
            terrainPipeline = VK_NULL_HANDLE;
        }
        for (auto* pipeline : {&alphaScenePipeline, &alphaDepthPipeline,
            &familyScenePipeline}) {
            if (*pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, *pipeline, nullptr);
                *pipeline = VK_NULL_HANDLE;
            }
        }
        for (auto& pipeline : blendedScenePipelines) {
            if (pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, pipeline, nullptr);
                pipeline = VK_NULL_HANDLE;
            }
        }
        if (tonePipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, tonePipelineLayout, nullptr);
            tonePipelineLayout = VK_NULL_HANDLE;
        }
        if (meshPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, meshPipelineLayout, nullptr);
            meshPipelineLayout = VK_NULL_HANDLE;
        }
        if (scenePipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, scenePipelineLayout, nullptr);
            scenePipelineLayout = VK_NULL_HANDLE;
        }
        if (terrainPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, terrainPipelineLayout, nullptr);
            terrainPipelineLayout = VK_NULL_HANDLE;
        }
        if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, sampler, nullptr);
            sampler = VK_NULL_HANDLE;
        }
        if (descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            descriptorPool = VK_NULL_HANDLE;
        }
        if (toneSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, toneSetLayout, nullptr);
            toneSetLayout = VK_NULL_HANDLE;
        }
        if (materialSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, materialSetLayout, nullptr);
            materialSetLayout = VK_NULL_HANDLE;
        }
        if (deformSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, deformSetLayout, nullptr);
            deformSetLayout = VK_NULL_HANDLE;
        }
        if (indirectSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, indirectSetLayout, nullptr);
            indirectSetLayout = VK_NULL_HANDLE;
        }
        if (completion != VK_NULL_HANDLE) {
            vkDestroyFence(device, completion, nullptr);
            completion = VK_NULL_HANDLE;
        }
        if (commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, commandPool, nullptr);
            commandPool = VK_NULL_HANDLE;
            command = VK_NULL_HANDLE;
        }
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (messenger != VK_NULL_HANDLE && destroyMessenger != nullptr &&
        instance != VK_NULL_HANDLE) {
        destroyMessenger(instance, messenger, nullptr);
        messenger = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
    physicalDevice = VK_NULL_HANDLE;
    queue = VK_NULL_HANDLE;
    queueFamily = 0xFFFFFFFFu;
    materialSet = VK_NULL_HANDLE;
    toneSet = VK_NULL_HANDLE;
    deformSet = VK_NULL_HANDLE;
    viewRecord = {};
    viewConstants = {};
    phase10ViewActive = false;
    scenePacket = {};
    phase11SceneActive = false;
    terrainPacket = {};
    terrainCellRecords.clear();
    phase14TerrainActive = false;
    deformPacket = {};
    deformTopology = {};
    deformRing = deform::DynamicRing{0, kDeformRingAlignment};
    deformVertexRange = {};
    deformPreviousRange = {};
    phase13DeformActive = false;
    samplerAnisotropySupported = false;
    independentBlendSupported = false;
    beginLabel = nullptr;
    endLabel = nullptr;
    ready = false;
}

abi::Result VulkanRasterRenderer::Impl::CreateInstance(
    const bool validation) noexcept
{
    std::uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS ||
        loaderVersion < VK_API_VERSION_1_3) {
        return abi::Result::VulkanLoaderUnavailable;
    }
    std::vector<VkExtensionProperties> extensions;
    std::vector<VkLayerProperties> layers;
    if (!EnumerateVector(extensions,
            [](std::uint32_t* count, VkExtensionProperties* values) {
                return vkEnumerateInstanceExtensionProperties(
                    nullptr, count, values);
            }) ||
        !EnumerateVector(layers,
            [](std::uint32_t* count, VkLayerProperties* values) {
                return vkEnumerateInstanceLayerProperties(count, values);
            })) {
        return abi::Result::InstanceCreationFailed;
    }
    const auto hasDebug = HasExtension(
        extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (validation &&
        (!hasDebug || !HasLayer(layers, kValidationLayer))) {
        return abi::Result::ValidationLayerUnavailable;
    }

    debug.callbacks = callbacks;
    VkDebugUtilsMessengerCreateInfoEXT debugInfo{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debugInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debugInfo.pfnUserCallback = DebugCallback;
    debugInfo.pUserData = &debug;

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "VisualForgePacketReplay";
    application.applicationVersion = 1;
    application.pEngineName = "VisualForge";
    application.engineVersion = 1;
    application.apiVersion = VK_API_VERSION_1_3;
    const char* layer = kValidationLayer;
    const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkInstanceCreateInfo createInfo{
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pNext = validation ? &debugInfo : nullptr;
    createInfo.pApplicationInfo = &application;
    createInfo.enabledLayerCount = validation ? 1u : 0u;
    createInfo.ppEnabledLayerNames = validation ? &layer : nullptr;
    createInfo.enabledExtensionCount = validation ? 1u : 0u;
    createInfo.ppEnabledExtensionNames = validation ? &extension : nullptr;
    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
        return abi::Result::InstanceCreationFailed;
    }
    if (validation) {
        const auto createMessenger =
            reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance,
                    "vkCreateDebugUtilsMessengerEXT"));
        destroyMessenger =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance,
                    "vkDestroyDebugUtilsMessengerEXT"));
        if (createMessenger == nullptr || destroyMessenger == nullptr ||
            createMessenger(instance, &debugInfo, nullptr, &messenger) !=
                VK_SUCCESS) {
            return abi::Result::ValidationLayerUnavailable;
        }
    }
    return abi::Result::Success;
}

abi::Result VulkanRasterRenderer::Impl::CreateDevice(
    const abi::AdapterLuid requiredLuid,
    const bool anyAdapter) noexcept
{
    std::vector<VkPhysicalDevice> devices;
    if (!EnumerateVector(devices,
            [this](std::uint32_t* count, VkPhysicalDevice* values) {
                return vkEnumeratePhysicalDevices(instance, count, values);
            })) {
        return abi::Result::AdapterLuidNotFound;
    }
    VkPhysicalDeviceVulkan13Features selected13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    // The first capable device, kept in case nothing carries the requested
    // LUID and the caller said any would do.
    VkPhysicalDevice fallback{VK_NULL_HANDLE};
    VkPhysicalDeviceProperties fallbackProperties{};
    std::uint64_t fallbackAllocationBytes{};
    for (const auto candidate : devices) {
        VkPhysicalDeviceProperties2 properties2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        VkPhysicalDeviceIDProperties id{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        // Carries maxMemoryAllocationSize, which is what bounds a frame
        // packet: the vertices and indices are uploaded as one allocation, so
        // a packet larger than this could never be drawn whatever else is
        // true of it. Core since Vulkan 1.1 and this renderer requires 1.3.
        VkPhysicalDeviceMaintenance3Properties maintenance3{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
        properties2.pNext = &id;
        id.pNext = &maintenance3;
        vkGetPhysicalDeviceProperties2(candidate, &properties2);
        // The requested adapter, or -- when the caller said any would do --
        // the first one enumerated. A DXGI adapter list is not a list of
        // Vulkan devices: a machine with a headset runtime installed can
        // enumerate a mirrored adapter reporting the same name and the same
        // memory as the real card, carrying a LUID no Vulkan device has, and
        // it can enumerate first. Only a caller that does not have to share
        // an image with D3D may take this path.
        //
        // An exact match always wins, even when any adapter would do: the
        // fallback is taken only after the whole list has been searched, so a
        // caller that named a real device still gets it however the mirrored
        // one happens to be ordered.
        const auto matches = id.deviceLUIDValid == VK_TRUE &&
            ReadLuid(id) == requiredLuid;
        if (!matches) {
            if (!anyAdapter || fallback != VK_NULL_HANDLE) continue;
            fallback = candidate;
            fallbackProperties = properties2.properties;
            fallbackAllocationBytes = static_cast<std::uint64_t>(
                maintenance3.maxMemoryAllocationSize);
            continue;
        }
        physicalDevice = candidate;
        properties = properties2.properties;
        maximumAllocationBytes =
            static_cast<std::uint64_t>(maintenance3.maxMemoryAllocationSize);
        VkPhysicalDeviceFeatures2 features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &selected13;
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        samplerAnisotropySupported =
            features.features.samplerAnisotropy == VK_TRUE;
        independentBlendSupported =
            features.features.independentBlend == VK_TRUE;
        break;
    }
    if (physicalDevice == VK_NULL_HANDLE && fallback != VK_NULL_HANDLE) {
        physicalDevice = fallback;
        properties = fallbackProperties;
        maximumAllocationBytes = fallbackAllocationBytes;
        VkPhysicalDeviceFeatures2 features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &selected13;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &features);
        samplerAnisotropySupported =
            features.features.samplerAnisotropy == VK_TRUE;
        independentBlendSupported =
            features.features.independentBlend == VK_TRUE;
    }
    if (physicalDevice == VK_NULL_HANDLE) {
        return abi::Result::AdapterLuidNotFound;
    }
    if (selected13.dynamicRendering == VK_FALSE ||
        selected13.synchronization2 == VK_FALSE) {
        return abi::Result::RasterUnsupported;
    }

    const struct FormatRequirement
    {
        VkFormat format;
        VkFormatFeatureFlags features;
    } requirements[]{
        {kHdrFormat, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT},
        {kDepthFormat, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT},
        {kOutputFormat, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT},
    };
    for (const auto& requirement : requirements) {
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(
            physicalDevice, requirement.format, &formatProperties);
        if ((formatProperties.optimalTilingFeatures & requirement.features) !=
            requirement.features) {
            return abi::Result::RasterUnsupported;
        }
    }

    std::uint32_t queueCount{};
    vkGetPhysicalDeviceQueueFamilyProperties(
        physicalDevice, &queueCount, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
        physicalDevice, &queueCount, queues.data());
    for (std::uint32_t index = 0; index < queueCount; ++index) {
        if (queues[index].queueCount != 0 &&
            (queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            queueFamily = index;
            break;
        }
    }
    if (queueFamily == 0xFFFFFFFFu) {
        return abi::Result::QueueFamilyNotFound;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    // Ray query needs the acceleration-structure extension, its deferred-host
    // dependency, and buffer device addresses. They are enabled only when the
    // device advertises them, so a device without ray tracing still creates
    // and the mirror falls back to unshadowed lighting rather than failing.
    std::uint32_t availableCount{};
    static_cast<void>(vkEnumerateDeviceExtensionProperties(
        physicalDevice, nullptr, &availableCount, nullptr));
    std::vector<VkExtensionProperties> available(availableCount);
    static_cast<void>(vkEnumerateDeviceExtensionProperties(
        physicalDevice, nullptr, &availableCount, available.data()));
    const auto hasExtension = [&available](const char* name) {
        return std::any_of(available.begin(), available.end(),
            [name](const VkExtensionProperties& properties) {
                return std::strcmp(properties.extensionName, name) == 0;
            });
    };
    std::vector<const char*> deviceExtensions;
    rayQuerySupported =
        hasExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
        hasExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
        hasExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    if (rayQuerySupported) {
        deviceExtensions.push_back(
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        deviceExtensions.push_back(
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        deviceExtensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    }

    VkPhysicalDeviceVulkan13Features enabled13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    enabled13.dynamicRendering = VK_TRUE;
    enabled13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features enabled12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enabledAccel{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR enabledRayQuery{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    if (rayQuerySupported) {
        enabled12.bufferDeviceAddress = VK_TRUE;
        enabledAccel.accelerationStructure = VK_TRUE;
        enabledRayQuery.rayQuery = VK_TRUE;
        enabled13.pNext = &enabled12;
        enabled12.pNext = &enabledAccel;
        enabledAccel.pNext = &enabledRayQuery;
    }
    VkPhysicalDeviceFeatures enabledFeatures{};
    enabledFeatures.independentBlend = independentBlendSupported
        ? VK_TRUE : VK_FALSE;
    enabledFeatures.samplerAnisotropy = samplerAnisotropySupported
        ? VK_TRUE : VK_FALSE;
    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.pNext = &enabled13;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.pEnabledFeatures = &enabledFeatures;
    createInfo.enabledExtensionCount =
        static_cast<std::uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.empty()
        ? nullptr : deviceExtensions.data();
    if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) !=
        VK_SUCCESS) {
        return abi::Result::DeviceCreationFailed;
    }
    if (rayQuerySupported) {
        // Extension entry points are not in the loader's core table.
        createAccelerationStructure =
            reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
                vkGetDeviceProcAddr(
                    device, "vkCreateAccelerationStructureKHR"));
        destroyAccelerationStructure =
            reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
                vkGetDeviceProcAddr(
                    device, "vkDestroyAccelerationStructureKHR"));
        getAccelerationStructureBuildSizes =
            reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
                vkGetDeviceProcAddr(
                    device, "vkGetAccelerationStructureBuildSizesKHR"));
        cmdBuildAccelerationStructures =
            reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
                vkGetDeviceProcAddr(
                    device, "vkCmdBuildAccelerationStructuresKHR"));
        getAccelerationStructureDeviceAddress =
            reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
                vkGetDeviceProcAddr(
                    device, "vkGetAccelerationStructureDeviceAddressKHR"));
        getBufferDeviceAddress =
            reinterpret_cast<PFN_vkGetBufferDeviceAddress>(
                vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddress"));
        // A missing entry point after a successful enable would fault on
        // first use, so the capability is withdrawn instead.
        rayQuerySupported = createAccelerationStructure != nullptr &&
            destroyAccelerationStructure != nullptr &&
            getAccelerationStructureBuildSizes != nullptr &&
            cmdBuildAccelerationStructures != nullptr &&
            getAccelerationStructureDeviceAddress != nullptr &&
            getBufferDeviceAddress != nullptr;
    }
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(
        physicalDevice, &memoryProperties);
    beginLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(device, "vkCmdBeginDebugUtilsLabelEXT"));
    endLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(device, "vkCmdEndDebugUtilsLabelEXT"));
    return abi::Result::Success;
}

abi::Result VulkanRasterRenderer::Impl::CreateCoreObjects() noexcept
{
    VkCommandPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) !=
        VK_SUCCESS) {
        return abi::Result::RasterCreateFailed;
    }
    VkCommandBufferAllocateInfo commandInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &commandInfo, &command) !=
        VK_SUCCESS) {
        return abi::Result::RasterCreateFailed;
    }
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device, &fenceInfo, nullptr, &completion) !=
        VK_SUCCESS) {
        return abi::Result::RasterCreateFailed;
    }

    // Sixteen always, plus the top level and the geometry-to-object table
    // when ray query is enabled.
    std::array<VkDescriptorSetLayoutBinding, 19> materialBindings{};
    materialBindings[0].binding = raster::kMaterialDescriptorBinding;
    materialBindings[0].descriptorType =
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    materialBindings[0].descriptorCount = 1;
    materialBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    materialBindings[1].binding = raster::kBaseTextureDescriptorBinding;
    materialBindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    materialBindings[1].descriptorCount = 1;
    materialBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    materialBindings[2].binding = 2;
    materialBindings[2].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    materialBindings[2].descriptorCount = 1;
    materialBindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    materialBindings[3] = materialBindings[2];
    materialBindings[3].binding = 3;
    materialBindings[4].binding = 4;
    materialBindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    materialBindings[4].descriptorCount = 1;
    materialBindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    materialBindings[5] = materialBindings[4];
    materialBindings[5].binding = 5;
    materialBindings[6].binding = view::kViewDescriptorBinding;
    materialBindings[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    materialBindings[6].descriptorCount = 1;
    materialBindings[6].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    materialBindings[7] = materialBindings[4];
    materialBindings[7].binding = scene::kSceneObjectDescriptorBinding;
    materialBindings[7].stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    materialBindings[8] = materialBindings[7];
    materialBindings[8].binding = scene::kSceneInstanceDescriptorBinding;
    materialBindings[9] = materialBindings[7];
    materialBindings[9].binding = terrain::kTerrainCellDescriptorBinding;
    materialBindings[10] = materialBindings[7];
    materialBindings[10].binding = terrain::kTerrainLayerDescriptorBinding;
    materialBindings[11].binding = terrain::kTerrainLayerTextureBinding;
    materialBindings[11].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    materialBindings[11].descriptorCount = 1;
    materialBindings[11].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    materialBindings[12] = materialBindings[7];
    materialBindings[12].binding = scene::kSceneVisibilityDescriptorBinding;
    materialBindings[13] = materialBindings[7];
    materialBindings[13].binding = scene::kSceneFamilyDescriptorBinding;
    materialBindings[14] = materialBindings[7];
    materialBindings[14].binding = scene::kSceneLightDescriptorBinding;
    materialBindings[15] = materialBindings[7];
    materialBindings[15].binding = scene::kSceneEnvironmentDescriptorBinding;
    // The top-level structure the shadow ray query traces against. Declared
    // only when the device supports ray query, because a descriptor type the
    // device never enabled is a layout creation error.
    std::uint32_t materialBindingCount = 16;
    if (rayQuerySupported) {
        materialBindings[16].binding = scene::kSceneTlasDescriptorBinding;
        materialBindings[16].descriptorType =
            VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        materialBindings[16].descriptorCount = 1;
        materialBindings[16].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // The geometry-to-object table a reflection hit is shaded through.
        materialBindings[17] = materialBindings[7];
        materialBindings[17].binding =
            scene::kSceneGeometryObjectDescriptorBinding;
        materialBindingCount = 18;
    }
    // The colour target as it stood before any refractive draw. A refractive
    // surface that samples the live target instead sees whatever refractive
    // surfaces happened to be drawn before it, so two panes of glass show
    // each other and the result depends on draw order rather than on depth.
    materialBindings[materialBindingCount].binding =
        scene::kSceneRefractionDescriptorBinding;
    materialBindings[materialBindingCount].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    materialBindings[materialBindingCount].descriptorCount = 1;
    materialBindings[materialBindingCount].stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT;
    ++materialBindingCount;
    VkDescriptorSetLayoutCreateInfo materialLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    materialLayoutInfo.bindingCount = materialBindingCount;
    materialLayoutInfo.pBindings = materialBindings.data();
    if (vkCreateDescriptorSetLayout(device, &materialLayoutInfo, nullptr,
            &materialSetLayout) != VK_SUCCESS) {
        return abi::Result::RasterCreateFailed;
    }

    std::array<VkDescriptorSetLayoutBinding,
        deform::kDeformBindingCount> deformBindings{};
    for (std::uint32_t index = 0; index < deformBindings.size(); ++index) {
        deformBindings[index].binding = index;
        deformBindings[index].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        deformBindings[index].descriptorCount = 1;
        deformBindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo deformLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    deformLayoutInfo.bindingCount =
        static_cast<std::uint32_t>(deformBindings.size());
    deformLayoutInfo.pBindings = deformBindings.data();
    if (vkCreateDescriptorSetLayout(device, &deformLayoutInfo, nullptr,
            &deformSetLayout) != VK_SUCCESS) {
        return abi::Result::RasterCreateFailed;
    }

    std::array<VkDescriptorSetLayoutBinding,
        gi::kIndirectBindingCount> indirectBindings{};
    for (std::uint32_t index = 0; index < indirectBindings.size(); ++index) {
        indirectBindings[index].binding = index;
        indirectBindings[index].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        indirectBindings[index].descriptorCount = 1;
        indirectBindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo indirectSetLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    indirectSetLayoutInfo.bindingCount =
        static_cast<std::uint32_t>(indirectBindings.size());
    indirectSetLayoutInfo.pBindings = indirectBindings.data();
    if (vkCreateDescriptorSetLayout(device, &indirectSetLayoutInfo, nullptr,
            &indirectSetLayout) != VK_SUCCESS) {
        return abi::Result::RasterCreateFailed;
    }

    VkDescriptorSetLayoutBinding toneBinding{};
    toneBinding.binding = 0;
    toneBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    toneBinding.descriptorCount = 1;
    toneBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo toneLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    toneLayoutInfo.bindingCount = 1;
    toneLayoutInfo.pBindings = &toneBinding;
    if (vkCreateDescriptorSetLayout(device, &toneLayoutInfo, nullptr,
            &toneSetLayout) != VK_SUCCESS) {
        return abi::Result::RasterCreateFailed;
    }

    const std::array poolSizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5},
        // Ten scene buffers when ray query adds the geometry-to-object table.
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            10 + deform::kDeformBindingCount + gi::kIndirectBindingCount},
        // Last so the count can drop it on a device without ray query, where
        // the type is not a valid pool entry.
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
    };
    VkDescriptorPoolCreateInfo descriptorPoolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    // Scene, material, deformation, and now temporal accumulation.
    descriptorPoolInfo.maxSets = 4;
    descriptorPoolInfo.poolSizeCount = static_cast<std::uint32_t>(
        rayQuerySupported ? poolSizes.size() : poolSizes.size() - 1);
    descriptorPoolInfo.pPoolSizes = poolSizes.data();
    if (vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr,
            &descriptorPool) != VK_SUCCESS) {
        return abi::Result::RasterCreateFailed;
    }
    const std::array layouts{
        materialSetLayout, toneSetLayout, deformSetLayout,
        indirectSetLayout};
    std::array<VkDescriptorSet, 4> sets{};
    VkDescriptorSetAllocateInfo setInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = descriptorPool;
    setInfo.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
    setInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device, &setInfo, sets.data()) != VK_SUCCESS) {
        return abi::Result::RasterCreateFailed;
    }
    materialSet = sets[0];
    toneSet = sets[1];
    deformSet = sets[2];
    indirectSet = sets[3];

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &sampler) !=
        VK_SUCCESS) {
        return abi::Result::RasterCreateFailed;
    }
    return CreatePipelines();
}

abi::Result VulkanRasterRenderer::Impl::CreatePipelines() noexcept
{
    using namespace shaders;
    const auto createModule = [this](
        const std::uint8_t* bytes,
        const std::size_t size,
        VkShaderModule& module) {
        VkShaderModuleCreateInfo info{
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = size;
        info.pCode = reinterpret_cast<const std::uint32_t*>(bytes);
        return vkCreateShaderModule(device, &info, nullptr, &module) ==
            VK_SUCCESS;
    };
    VkShaderModule meshVertex{};
    VkShaderModule meshFragment{};
    VkShaderModule materialFragment{};
    VkShaderModule sceneVertex{};
    VkShaderModule sceneFragment{};
    VkShaderModule terrainVertex{};
    VkShaderModule terrainFragment{};
    VkShaderModule alphaSceneFragment{};
    VkShaderModule alphaDepthFragment{};
    VkShaderModule familySceneFragment{};
    VkShaderModule toneVertex{};
    VkShaderModule toneFragment{};
    const auto cleanupModules = [this, &meshVertex, &meshFragment,
                                 &materialFragment, &sceneVertex,
                                 &sceneFragment, &terrainVertex,
                                 &terrainFragment, &alphaSceneFragment,
                                 &alphaDepthFragment, &familySceneFragment,
                                 &toneVertex, &toneFragment]() {
        for (const auto module : {
                 meshVertex, meshFragment, materialFragment,
                 sceneVertex, sceneFragment,
                 terrainVertex, terrainFragment,
                 alphaSceneFragment, alphaDepthFragment,
                 familySceneFragment,
                 toneVertex, toneFragment}) {
            if (module != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device, module, nullptr);
            }
        }
    };
    if (!createModule(kPhase6_mesh_vert, kPhase6_mesh_vertSize, meshVertex) ||
        !createModule(kPhase6_mesh_frag, kPhase6_mesh_fragSize, meshFragment) ||
        !createModule(kPhase9_material_frag,
            kPhase9_material_fragSize, materialFragment) ||
        !createModule(kPhase11_scene_vert,
            kPhase11_scene_vertSize, sceneVertex) ||
        !createModule(kPhase11_scene_frag,
            kPhase11_scene_fragSize, sceneFragment) ||
        !createModule(kPhase14_terrain_vert,
            kPhase14_terrain_vertSize, terrainVertex) ||
        !createModule(kPhase14_terrain_frag,
            kPhase14_terrain_fragSize, terrainFragment) ||
        !createModule(kPhase15_alpha_scene_frag,
            kPhase15_alpha_scene_fragSize, alphaSceneFragment) ||
        !createModule(kPhase15_alpha_depth_frag,
            kPhase15_alpha_depth_fragSize, alphaDepthFragment) ||
        // The ray-query variant when the device enabled the extension, and
        // the plain one otherwise. The capability is baked into the module,
        // so a device without it could not create a pipeline from the first.
        !createModule(
            rayQuerySupported ? kPhase18_family_scene_rq_frag
                              : kPhase16_family_scene_frag,
            rayQuerySupported ? kPhase18_family_scene_rq_fragSize
                              : kPhase16_family_scene_fragSize,
            familySceneFragment) ||
        !createModule(kPhase6_tone_map_vert,
            kPhase6_tone_map_vertSize, toneVertex) ||
        !createModule(kPhase6_tone_map_frag,
            kPhase6_tone_map_fragSize, toneFragment)) {
        cleanupModules();
        return abi::Result::RasterCreateFailed;
    }

    VkPipelineLayoutCreateInfo meshLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    meshLayoutInfo.setLayoutCount = 1;
    meshLayoutInfo.pSetLayouts = &materialSetLayout;
    if (vkCreatePipelineLayout(device, &meshLayoutInfo, nullptr,
            &meshPipelineLayout) != VK_SUCCESS) {
        cleanupModules();
        return abi::Result::RasterCreateFailed;
    }
    const VkPushConstantRange scenePushRange{
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(scene::ScenePushConstantsV1)};
    VkPipelineLayoutCreateInfo sceneLayoutInfo = meshLayoutInfo;
    sceneLayoutInfo.pushConstantRangeCount = 1;
    sceneLayoutInfo.pPushConstantRanges = &scenePushRange;
    if (vkCreatePipelineLayout(device, &sceneLayoutInfo, nullptr,
            &scenePipelineLayout) != VK_SUCCESS) {
        cleanupModules();
        return abi::Result::RasterCreateFailed;
    }
    const VkPushConstantRange terrainPushRange{
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(terrain::TerrainPushConstantsV1)};
    VkPipelineLayoutCreateInfo terrainLayoutInfo = meshLayoutInfo;
    terrainLayoutInfo.pushConstantRangeCount = 1;
    terrainLayoutInfo.pPushConstantRanges = &terrainPushRange;
    if (vkCreatePipelineLayout(device, &terrainLayoutInfo, nullptr,
            &terrainPipelineLayout) != VK_SUCCESS) {
        cleanupModules();
        return abi::Result::RasterCreateFailed;
    }
    VkPipelineLayoutCreateInfo toneLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    toneLayoutInfo.setLayoutCount = 1;
    toneLayoutInfo.pSetLayouts = &toneSetLayout;
    // Exposure reaches the curve as a push constant. It is applied before
    // the curve, so the whole range shifts rather than the highlights alone.
    const VkPushConstantRange tonePushRange{
        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TonePushConstants)};
    toneLayoutInfo.pushConstantRangeCount = 1;
    toneLayoutInfo.pPushConstantRanges = &tonePushRange;
    if (vkCreatePipelineLayout(device, &toneLayoutInfo, nullptr,
            &tonePipelineLayout) != VK_SUCCESS) {
        cleanupModules();
        return abi::Result::RasterCreateFailed;
    }

    const VkVertexInputBindingDescription vertexBinding{
        0, sizeof(raster::RasterVertexV3), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array vertexAttributes{
        VkVertexInputAttributeDescription{
            0, 0, VK_FORMAT_R32G32B32_SFLOAT,
            offsetof(raster::RasterVertexV3, position)},
        VkVertexInputAttributeDescription{
            1, 0, VK_FORMAT_R32G32B32_SFLOAT,
            offsetof(raster::RasterVertexV3, color)},
        VkVertexInputAttributeDescription{
            2, 0, VK_FORMAT_R32G32_SFLOAT,
            offsetof(raster::RasterVertexV3, texCoord)},
        // The shading normal. Everything downstream -- shadows, reflections,
        // indirect -- starts from N, so without this every surface shades as
        // though it faced the same way and each of those phases produces a
        // uniform result that looks like the phase being broken.
        VkVertexInputAttributeDescription{
            3, 0, VK_FORMAT_R32G32B32_SFLOAT,
            offsetof(raster::RasterVertexV3, normal)},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &vertexBinding;
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<std::uint32_t>(vertexAttributes.size());
    vertexInput.pVertexAttributeDescriptions = vertexAttributes.data();
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const std::array dynamicStates{
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount =
        static_cast<std::uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();
    const std::array meshStages{
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
            meshVertex, "main", nullptr},
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
            meshFragment, "main", nullptr},
    };
    const std::array materialStages{
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
            meshVertex, "main", nullptr},
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
            materialFragment, "main", nullptr},
    };
    const std::array sceneStages{
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
            sceneVertex, "main", nullptr},
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
            sceneFragment, "main", nullptr},
    };
    // The Phase 11 mirror renders the compatibility color target plus the
    // four G-buffer planes, so attachment writes stay rasterization
    // ordered instead of racing between draws.
    std::array<VkFormat, 1 + scene::kSceneGBufferPlaneCount> sceneFormats{};
    sceneFormats[0] = kHdrFormat;
    std::copy(kGBufferFormats.begin(), kGBufferFormats.end(),
        sceneFormats.begin() + 1);
    std::array<VkPipelineColorBlendAttachmentState,
        1 + scene::kSceneGBufferPlaneCount> sceneBlendAttachments{};
    sceneBlendAttachments.fill(blendAttachment);
    VkPipelineColorBlendStateCreateInfo sceneBlend = blend;
    sceneBlend.attachmentCount =
        static_cast<std::uint32_t>(sceneBlendAttachments.size());
    sceneBlend.pAttachments = sceneBlendAttachments.data();
    VkPipelineRenderingCreateInfo sceneRendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    sceneRendering.colorAttachmentCount =
        static_cast<std::uint32_t>(sceneFormats.size());
    sceneRendering.pColorAttachmentFormats = sceneFormats.data();
    sceneRendering.depthAttachmentFormat = kDepthFormat;
    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &kHdrFormat;
    rendering.depthAttachmentFormat = kDepthFormat;
    VkGraphicsPipelineCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.pNext = &rendering;
    pipelineInfo.stageCount = static_cast<std::uint32_t>(meshStages.size());
    pipelineInfo.pStages = meshStages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = meshPipelineLayout;
    for (const auto face : {
             raster::FrontFace::CounterClockwise,
             raster::FrontFace::Clockwise}) {
        // Packet winding is classified in mathematical NDC coordinates.
        // A positive-height Vulkan viewport maps +Y downward in framebuffer
        // coordinates, so the hardware front-face convention is inverted.
        rasterization.frontFace = face == raster::FrontFace::CounterClockwise
            ? VK_FRONT_FACE_CLOCKWISE
            : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        for (const auto compare : {
                 raster::DepthCompare::Less,
                 raster::DepthCompare::LessOrEqual,
                 raster::DepthCompare::Always}) {
            depthStencil.depthCompareOp = ToVkCompare(compare);
            if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                    &pipelineInfo, nullptr,
                    &meshPipelines[PipelineIndex(face, compare)]) !=
                VK_SUCCESS) {
                cleanupModules();
                return abi::Result::RasterCreateFailed;
            }
            pipelineInfo.pStages = materialStages.data();
            if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                    &pipelineInfo, nullptr,
                    &materialPipelines[PipelineIndex(face, compare)]) !=
                VK_SUCCESS) {
                cleanupModules();
                return abi::Result::RasterCreateFailed;
            }
            pipelineInfo.pStages = sceneStages.data();
            pipelineInfo.layout = scenePipelineLayout;
            pipelineInfo.pNext = &sceneRendering;
            pipelineInfo.pColorBlendState = &sceneBlend;
            if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                    &pipelineInfo, nullptr,
                    &scenePipelines[PipelineIndex(face, compare)]) !=
                VK_SUCCESS) {
                cleanupModules();
                return abi::Result::RasterCreateFailed;
            }
            pipelineInfo.pNext = &rendering;
            pipelineInfo.pColorBlendState = &blend;
            pipelineInfo.pStages = meshStages.data();
            pipelineInfo.layout = meshPipelineLayout;
        }
    }

    // Terrain is its own vertex format: cell-relative position, normal,
    // vertex color, and the engine's eight land-data channels.
    const VkVertexInputBindingDescription terrainBinding{
        0, sizeof(terrain::LandscapeVertexV1), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array terrainAttributes{
        VkVertexInputAttributeDescription{
            0, 0, VK_FORMAT_R32G32B32_SFLOAT,
            offsetof(terrain::LandscapeVertexV1, position)},
        VkVertexInputAttributeDescription{
            1, 0, VK_FORMAT_R32G32B32_SFLOAT,
            offsetof(terrain::LandscapeVertexV1, normal)},
        VkVertexInputAttributeDescription{
            2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
            offsetof(terrain::LandscapeVertexV1, color)},
        VkVertexInputAttributeDescription{
            3, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
            offsetof(terrain::LandscapeVertexV1, channels)},
        VkVertexInputAttributeDescription{
            4, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
            offsetof(terrain::LandscapeVertexV1, channels) +
                4 * sizeof(float)},
    };
    VkPipelineVertexInputStateCreateInfo terrainVertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    terrainVertexInput.vertexBindingDescriptionCount = 1;
    terrainVertexInput.pVertexBindingDescriptions = &terrainBinding;
    terrainVertexInput.vertexAttributeDescriptionCount =
        static_cast<std::uint32_t>(terrainAttributes.size());
    terrainVertexInput.pVertexAttributeDescriptions =
        terrainAttributes.data();
    const std::array terrainStages{
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
            terrainVertex, "main", nullptr},
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
            terrainFragment, "main", nullptr},
    };
    // Terrain quads are wound so their framebuffer-space signed area is
    // negative, which is the counter-clockwise face under Vulkan's Y-down
    // framebuffer convention.
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    pipelineInfo.stageCount =
        static_cast<std::uint32_t>(terrainStages.size());
    pipelineInfo.pStages = terrainStages.data();
    pipelineInfo.pVertexInputState = &terrainVertexInput;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &sceneBlend;
    pipelineInfo.pNext = &sceneRendering;
    pipelineInfo.layout = terrainPipelineLayout;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
            &pipelineInfo, nullptr, &terrainPipeline) != VK_SUCCESS) {
        cleanupModules();
        return abi::Result::RasterCreateFailed;
    }
    pipelineInfo.pNext = &rendering;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pVertexInputState = &vertexInput;

    // Alpha-tested geometry. Cull mode, front face, and depth compare are
    // dynamic (core in Vulkan 1.3) so the packet's per-draw state does not
    // need a static pipeline permutation, and two-sided objects only differ
    // by a cull-mode command.
    const std::array alphaDynamicStates{
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    };
    VkPipelineDynamicStateCreateInfo alphaDynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    alphaDynamic.dynamicStateCount =
        static_cast<std::uint32_t>(alphaDynamicStates.size());
    alphaDynamic.pDynamicStates = alphaDynamicStates.data();
    const std::array alphaSceneStages{
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
            sceneVertex, "main", nullptr},
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
            alphaSceneFragment, "main", nullptr},
    };
    const std::array alphaDepthStages{
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
            sceneVertex, "main", nullptr},
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
            alphaDepthFragment, "main", nullptr},
    };
    VkPipelineDepthStencilStateCreateInfo alphaDepthStencil = depthStencil;
    alphaDepthStencil.depthTestEnable = VK_TRUE;
    alphaDepthStencil.depthWriteEnable = VK_TRUE;
    alphaDepthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    pipelineInfo.stageCount =
        static_cast<std::uint32_t>(alphaDepthStages.size());
    pipelineInfo.pStages = alphaDepthStages.data();
    pipelineInfo.pDepthStencilState = &alphaDepthStencil;
    pipelineInfo.pDynamicState = &alphaDynamic;
    pipelineInfo.layout = scenePipelineLayout;
    VkPipelineRenderingCreateInfo alphaDepthRendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    alphaDepthRendering.colorAttachmentCount = 0;
    alphaDepthRendering.depthAttachmentFormat = kDepthFormat;
    VkPipelineColorBlendStateCreateInfo emptyBlend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    pipelineInfo.pNext = &alphaDepthRendering;
    pipelineInfo.pColorBlendState = &emptyBlend;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
            &pipelineInfo, nullptr, &alphaDepthPipeline) != VK_SUCCESS) {
        cleanupModules();
        return abi::Result::RasterCreateFailed;
    }
    // The color pass repeats the prepass discard and only keeps fragments
    // whose depth already matches, so a silhouette that disagreed between the
    // two passes would erase itself instead of quietly differing.
    VkPipelineDepthStencilStateCreateInfo alphaColorDepth = alphaDepthStencil;
    alphaColorDepth.depthWriteEnable = VK_FALSE;
    alphaColorDepth.depthCompareOp = VK_COMPARE_OP_EQUAL;
    pipelineInfo.stageCount =
        static_cast<std::uint32_t>(alphaSceneStages.size());
    pipelineInfo.pStages = alphaSceneStages.data();
    pipelineInfo.pDepthStencilState = &alphaColorDepth;
    pipelineInfo.pNext = &sceneRendering;
    pipelineInfo.pColorBlendState = &sceneBlend;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
            &pipelineInfo, nullptr, &alphaScenePipeline) != VK_SUCCESS) {
        cleanupModules();
        return abi::Result::RasterCreateFailed;
    }

    // Specialized material families draw as ordinary opaque geometry — depth
    // written, compare from dynamic state — but through one shader that
    // dispatches on the family record. Twenty-one families therefore cost a
    // single pipeline rather than a permutation each.
    const std::array familySceneStages{
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
            sceneVertex, "main", nullptr},
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
            familySceneFragment, "main", nullptr},
    };
    pipelineInfo.stageCount =
        static_cast<std::uint32_t>(familySceneStages.size());
    pipelineInfo.pStages = familySceneStages.data();
    pipelineInfo.pDepthStencilState = &alphaDepthStencil;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
            &pipelineInfo, nullptr, &familyScenePipeline) != VK_SUCCESS) {
        cleanupModules();
        return abi::Result::RasterCreateFailed;
    }

    // One pipeline per blend mode. Blend factors are pipeline state rather
    // than dynamic state in core Vulkan, so a single pipeline cannot serve
    // additive and multiply; building them up front is what keeps the
    // transparent pass from creating pipelines inside a frame.
    std::array<VkPipelineColorBlendAttachmentState,
        1 + scene::kSceneGBufferPlaneCount> blendedAttachments{};
    blendedAttachments.fill(blendAttachment);
    // Only the HDR attachment and the reactive mask are written. A transparent
    // fragment in the shading planes would make the reflection and indirect
    // passes treat a particle as an opaque surface, and every ray behind it
    // would stop there.
    //
    // The reactive plane is the exception, and it is the whole point of the
    // plane: it records that a transparent effect decided this pixel, which is
    // exactly what the shading planes must not say. Combined with max rather
    // than overwritten, so a pixel is as reactive as the most reactive draw
    // that touched it -- with overwrite the last draw wins and a dim smoke
    // puff drawn after a bright spark erases the spark's claim on the pixel.
    for (std::size_t plane = 1; plane + 1 < blendedAttachments.size();
         ++plane) {
        blendedAttachments[plane].colorWriteMask = 0;
    }
    auto& reactiveAttachment = blendedAttachments.back();
    reactiveAttachment.blendEnable = VK_TRUE;
    reactiveAttachment.colorBlendOp = VK_BLEND_OP_MAX;
    reactiveAttachment.alphaBlendOp = VK_BLEND_OP_MAX;
    reactiveAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    reactiveAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    reactiveAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    reactiveAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    VkPipelineColorBlendStateCreateInfo blendedBlend = sceneBlend;
    blendedBlend.pAttachments = blendedAttachments.data();

    VkPipelineDepthStencilStateCreateInfo blendedDepth = alphaDepthStencil;
    // Tested against the opaque scene but never written. Writing occludes the
    // transparent draws behind it, and the layer collapses to whichever
    // happened to be drawn first.
    blendedDepth.depthWriteEnable = VK_FALSE;

    pipelineInfo.pColorBlendState = &blendedBlend;
    pipelineInfo.pDepthStencilState = &blendedDepth;
    // Without per-attachment blend state every attachment must share one,
    // so the blend would apply to the G-buffer planes as well. The pipelines
    // are simply not built, and the transparent pass declines rather than
    // compositing particles into the surface data the ray passes read.
    for (std::size_t mode = 0;
        independentBlendSupported && mode < kBlendedPipelineCount; ++mode) {
        // Indexed by blend::BlendMode minus one: Opaque is not a blended
        // pipeline and never reaches this pass.
        auto& attachment = blendedAttachments[0];
        attachment.blendEnable = VK_TRUE;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor =
            VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        switch (static_cast<blend::BlendMode>(mode + 1)) {
        case blend::BlendMode::StraightAlpha:
            attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            attachment.dstColorBlendFactor =
                VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case blend::BlendMode::Premultiplied:
            // Already scaled by its own alpha. Scaling again is the double
            // darkening that shows around every edge.
            attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.dstColorBlendFactor =
                VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case blend::BlendMode::Additive:
            // The destination keeps its full share, which is what makes fire
            // brighten what is behind it rather than replacing it.
            attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case blend::BlendMode::Multiply:
            // Darkens and can never brighten, which is what a scorch mark
            // needs. dst*(src*a + (1-a)) written as the two factors Vulkan
            // offers: the destination scaled by the source, plus the
            // destination scaled by one minus the source alpha.
            attachment.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
            attachment.dstColorBlendFactor =
                VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case blend::BlendMode::Opaque:
            break;
        }
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                &pipelineInfo, nullptr,
                &blendedScenePipelines[mode]) != VK_SUCCESS) {
            cleanupModules();
            return abi::Result::RasterCreateFailed;
        }
    }

    pipelineInfo.pNext = &rendering;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = meshPipelineLayout;

    const std::array toneStages{
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
            toneVertex, "main", nullptr},
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
            toneFragment, "main", nullptr},
    };
    VkPipelineVertexInputStateCreateInfo emptyVertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rendering.pColorAttachmentFormats = &kOutputFormat;
    rendering.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    pipelineInfo.stageCount = static_cast<std::uint32_t>(toneStages.size());
    pipelineInfo.pStages = toneStages.data();
    pipelineInfo.pVertexInputState = &emptyVertexInput;
    pipelineInfo.pDepthStencilState = nullptr;
    pipelineInfo.layout = tonePipelineLayout;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
            &pipelineInfo, nullptr, &tonePipeline) != VK_SUCCESS) {
        cleanupModules();
        return abi::Result::RasterCreateFailed;
    }
    VkShaderModule indirectModule{};
    if (!createModule(kPhase20_accumulate_comp,
            kPhase20_accumulate_compSize, indirectModule)) {
        cleanupModules();
        return abi::Result::RasterCreateFailed;
    }
    const VkPushConstantRange indirectPushRange{
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(IndirectPushConstants)};
    VkPipelineLayoutCreateInfo indirectLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    indirectLayoutInfo.setLayoutCount = 1;
    indirectLayoutInfo.pSetLayouts = &indirectSetLayout;
    indirectLayoutInfo.pushConstantRangeCount = 1;
    indirectLayoutInfo.pPushConstantRanges = &indirectPushRange;
    if (vkCreatePipelineLayout(device, &indirectLayoutInfo, nullptr,
            &indirectPipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, indirectModule, nullptr);
        cleanupModules();
        return abi::Result::RasterCreateFailed;
    }
    VkComputePipelineCreateInfo indirectPipelineInfo{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    indirectPipelineInfo.stage = VkPipelineShaderStageCreateInfo{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, indirectModule, "main",
        nullptr};
    indirectPipelineInfo.layout = indirectPipelineLayout;
    const auto indirectCreated = vkCreateComputePipelines(device,
        VK_NULL_HANDLE, 1, &indirectPipelineInfo, nullptr, &indirectPipeline);
    vkDestroyShaderModule(device, indirectModule, nullptr);
    if (indirectCreated != VK_SUCCESS) {
        cleanupModules();
        return abi::Result::RasterCreateFailed;
    }

    VkShaderModule deformModule{};
    if (!createModule(kPhase13_deform_comp, kPhase13_deform_compSize,
            deformModule)) {
        cleanupModules();
        return abi::Result::RasterCreateFailed;
    }
    const VkPushConstantRange deformPushRange{
        VK_SHADER_STAGE_COMPUTE_BIT, 0,
        sizeof(deform::DeformPushConstantsV1)};
    VkPipelineLayoutCreateInfo deformLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    deformLayoutInfo.setLayoutCount = 1;
    deformLayoutInfo.pSetLayouts = &deformSetLayout;
    deformLayoutInfo.pushConstantRangeCount = 1;
    deformLayoutInfo.pPushConstantRanges = &deformPushRange;
    if (vkCreatePipelineLayout(device, &deformLayoutInfo, nullptr,
            &deformPipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, deformModule, nullptr);
        cleanupModules();
        return abi::Result::RasterCreateFailed;
    }
    VkComputePipelineCreateInfo deformPipelineInfo{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    deformPipelineInfo.stage = VkPipelineShaderStageCreateInfo{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, deformModule, "main",
        nullptr};
    deformPipelineInfo.layout = deformPipelineLayout;
    const auto deformCreated = vkCreateComputePipelines(device,
        VK_NULL_HANDLE, 1, &deformPipelineInfo, nullptr, &deformPipeline);
    vkDestroyShaderModule(device, deformModule, nullptr);
    if (deformCreated != VK_SUCCESS) {
        cleanupModules();
        return abi::Result::RasterCreateFailed;
    }

    cleanupModules();
    return abi::Result::Success;
}

abi::Result VulkanRasterRenderer::Impl::CreateImage(
    const VkFormat format,
    const VkImageUsageFlags usage,
    const VkImageAspectFlags aspect,
    Image& resource) noexcept
{
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.Width(), extent.Height(), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageInfo, nullptr, &resource.image) !=
        VK_SUCCESS) {
        return abi::Result::RasterCreateFailed;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, resource.image, &requirements);
    const auto memoryType = FindMemoryType(
        memoryProperties, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryType == std::numeric_limits<std::uint32_t>::max()) {
        return abi::Result::RasterCreateFailed;
    }
    VkMemoryAllocateInfo allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(device, &allocation, nullptr, &resource.memory) !=
            VK_SUCCESS ||
        vkBindImageMemory(device, resource.image, resource.memory, 0) !=
            VK_SUCCESS) {
        return abi::Result::RasterCreateFailed;
    }
    VkImageViewCreateInfo viewInfo{
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = resource.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, &resource.view) !=
        VK_SUCCESS) {
        return abi::Result::RasterCreateFailed;
    }
    resource.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return abi::Result::Success;
}

abi::Result VulkanRasterRenderer::Impl::PrepareSampledTexture(
    const texture::CapturedTexture& source,
    const std::uint64_t signature,
    const std::size_t slot) noexcept
{
    if (slot >= sampledResources.size()) {
        return abi::Result::RasterInvalidPacket;
    }
    auto& resource = sampledResources[slot];
    if (resource.image.image != VK_NULL_HANDLE &&
        resource.signature == signature) {
        return abi::Result::Success;
    }
    const auto destroyResource = [this](SampledResource& value) {
        if (value.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, value.sampler, nullptr);
        }
        if (value.image.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, value.image.view, nullptr);
        }
        if (value.image.image != VK_NULL_HANDLE) {
            vkDestroyImage(device, value.image.image, nullptr);
        }
        if (value.image.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, value.image.memory, nullptr);
        }
        value = {};
    };
    // Landscape layers arrive as one captured 2D array; everything else is a
    // single-layer 2D texture. The slot and the dimension have to agree so a
    // layer array can never be bound where a flat texture is sampled.
    const auto isLayerArray =
        source.dimension == texture::TextureDimension::Texture2DArray;
    if ((!isLayerArray &&
            source.dimension != texture::TextureDimension::Texture2D) ||
        source.arrayLayers == 0 || source.depth != 1) {
        return abi::Result::RasterInvalidPacket;
    }
    if (isLayerArray != (slot == kTerrainLayerTextureSlot)) {
        return abi::Result::RasterInvalidPacket;
    }
    if (!isLayerArray && source.arrayLayers != 1) {
        return abi::Result::RasterInvalidPacket;
    }
    const auto format = ToVkTextureFormat(source.viewFormat);
    if (format == VK_FORMAT_UNDEFINED) {
        return abi::Result::RasterInvalidPacket;
    }
    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(
        physicalDevice, format, &formatProperties);
    constexpr VkFormatFeatureFlags requiredFormatFeatures =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((formatProperties.optimalTilingFeatures & requiredFormatFeatures) !=
        requiredFormatFeatures) {
        return abi::Result::RasterUnsupported;
    }
    if (source.sampler.anisotropyEnable != 0 &&
        !samplerAnisotropySupported) {
        return abi::Result::RasterUnsupported;
    }

    VkBorderColor border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    const auto usesBorder =
        source.sampler.addressU == texture::TextureAddressMode::Border ||
        source.sampler.addressV == texture::TextureAddressMode::Border ||
        source.sampler.addressW == texture::TextureAddressMode::Border;
    if (usesBorder && !ToVkBorderColor(source.sampler.borderColor, border)) {
        return abi::Result::RasterInvalidPacket;
    }

    destroyResource(resource);
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {source.width, source.height, 1};
    imageInfo.mipLevels = source.mipLevels;
    imageInfo.arrayLayers = source.arrayLayers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageInfo, nullptr, &resource.image.image) !=
        VK_SUCCESS) {
        destroyResource(resource);
        return abi::Result::RasterCreateFailed;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, resource.image.image, &requirements);
    const auto memoryType = FindMemoryType(
        memoryProperties, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryType == std::numeric_limits<std::uint32_t>::max()) {
        destroyResource(resource);
        return abi::Result::RasterCreateFailed;
    }
    VkMemoryAllocateInfo allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(device, &allocation, nullptr,
            &resource.image.memory) != VK_SUCCESS ||
        vkBindImageMemory(device, resource.image.image,
            resource.image.memory, 0) != VK_SUCCESS) {
        destroyResource(resource);
        return abi::Result::RasterCreateFailed;
    }
    VkImageViewCreateInfo viewInfo{
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = resource.image.image;
    // The terrain slot is always sampled as an array so a one-layer landscape
    // still matches the shader's sampler2DArray declaration.
    viewInfo.viewType = slot == kTerrainLayerTextureSlot
        ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = source.residentBaseMip;
    viewInfo.subresourceRange.levelCount = source.residentMipCount;
    viewInfo.subresourceRange.layerCount = source.arrayLayers;
    if (vkCreateImageView(device, &viewInfo, nullptr,
            &resource.image.view) != VK_SUCCESS) {
        destroyResource(resource);
        return abi::Result::RasterCreateFailed;
    }
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = ToVkFilter(source.sampler.magFilter);
    samplerInfo.minFilter = ToVkFilter(source.sampler.minFilter);
    samplerInfo.mipmapMode = ToVkMipFilter(source.sampler.mipFilter);
    samplerInfo.addressModeU = ToVkAddress(source.sampler.addressU);
    samplerInfo.addressModeV = ToVkAddress(source.sampler.addressV);
    samplerInfo.addressModeW = ToVkAddress(source.sampler.addressW);
    samplerInfo.mipLodBias = source.sampler.mipLodBias;
    samplerInfo.anisotropyEnable = source.sampler.anisotropyEnable != 0
        ? VK_TRUE : VK_FALSE;
    samplerInfo.maxAnisotropy = source.sampler.maxAnisotropy;
    samplerInfo.compareEnable = source.sampler.comparisonEnable != 0
        ? VK_TRUE : VK_FALSE;
    samplerInfo.compareOp = ToVkCompare(source.sampler.compareOp);
    samplerInfo.minLod = source.sampler.minLod;
    samplerInfo.maxLod = std::min(
        source.sampler.maxLod,
        static_cast<float>(source.residentMipCount - 1));
    samplerInfo.borderColor = border;
    if (vkCreateSampler(device, &samplerInfo, nullptr,
            &resource.sampler) != VK_SUCCESS) {
        destroyResource(resource);
        return abi::Result::RasterCreateFailed;
    }
    try {
        resource.source = source;
    } catch (...) {
        destroyResource(resource);
        return abi::Result::InternalFailure;
    }
    resource.signature = signature;
    resource.image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    resource.uploadPending = true;

    VkDescriptorImageInfo descriptor{};
    descriptor.sampler = resource.sampler;
    descriptor.imageView = resource.image.view;
    descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = materialSet;
    write.dstBinding = slot == kTerrainLayerTextureSlot
        ? terrain::kTerrainLayerTextureBinding
        : raster::kBaseTextureDescriptorBinding +
            static_cast<std::uint32_t>(slot);
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &descriptor;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    return abi::Result::Success;
}

// Builds one bottom-level structure holding a geometry per drawn instance and
// one top-level structure holding a single identity instance.
//
// The per-geometry transform is the load-bearing part. `scene.vert` reads the
// packet's vertices as *local* space and multiplies them by the instance's
// model rows to reach the camera-relative space the fragment is shaded in. A
// structure built straight from those vertices would therefore describe a
// scene with every object collapsed onto the origin, and the shadows traced
// against it would be geometrically unrelated to the image. Vulkan's
// `transformData` applies the same model rows at build time, which puts the
// structure in exactly the space the rays are cast in.
abi::Result VulkanRasterRenderer::Impl::BuildAccelerationStructures(
    const raster::DecodedPacket& packet,
    const UploadLayout& layout) noexcept
{
    if (!rayQuerySupported) return abi::Result::Success;
    const auto triangleCount =
        static_cast<std::uint32_t>(packet.indices.size() / 3);
    if (triangleCount == 0) return abi::Result::Success;

    // Each failure names the step that produced it. A bare
    // "raster frame failed" describes the symptom and costs a debugging
    // cycle to narrow, which this build has already cost twice.
    const auto fail = [this](const char* step, const abi::Result result) {
        if (callbacks.log != nullptr) {
            std::string message{"acceleration build failed at "};
            message += step;
            callbacks.log(callbacks.userData, 3u, message.c_str());
        }
        return result;
    };

    const auto bufferAddress = [this](const VkBuffer buffer) {
        VkBufferDeviceAddressInfo info{
            VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        info.buffer = buffer;
        return getBufferDeviceAddress(device, &info);
    };

    // One entry per drawn instance: the index range it draws and the model
    // rows that place it. Rows 0 to 2 of the record's row-major 4x4 are
    // exactly a VkTransformMatrixKHR, so the same numbers `scene.vert` reads
    // are the numbers the build applies.
    struct GeometryPlan
    {
        std::uint32_t objectIndex{};
        std::uint32_t firstIndex{};
        std::uint32_t indexCount{};
        std::int32_t vertexOffset{};
        std::array<float, 12> transform{
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f};
    };
    std::vector<GeometryPlan> plans;
    const auto planDraw = [&](const std::uint32_t objectIndex,
                              const std::uint32_t drawIndex,
                              const float* model) {
        if (drawIndex >= packet.draws.size()) return;
        // Blended geometry is not an occluder. A particle or a pane of glass
        // that casts a hard opaque shadow is wrong, and it is wrong in a way
        // that reads as the shadow pass being broken rather than as the wrong
        // geometry being in the acceleration structure.
        if (std::any_of(scenePacket.transparent.begin(),
                scenePacket.transparent.end(),
                [objectIndex](const scene::TransparentDrawRecordV1& record) {
                    return record.objectIndex == objectIndex;
                })) {
            return;
        }
        const auto& draw = packet.draws[drawIndex];
        if (draw.indexCount < 3) return;
        GeometryPlan plan{};
        plan.objectIndex = objectIndex;
        plan.firstIndex = draw.firstIndex;
        plan.indexCount = draw.indexCount;
        plan.vertexOffset = draw.vertexOffset;
        if (model != nullptr) {
            std::copy_n(model, 12, plan.transform.begin());
        }
        plans.push_back(plan);
    };
    if (!scenePacket.objects.empty()) {
        if (scenePacket.instances.empty()) {
            // A version 1.0 scene has one implicit instance per object, and
            // the object carries the model rows itself.
            for (std::size_t index = 0; index < scenePacket.objects.size();
                 ++index) {
                planDraw(static_cast<std::uint32_t>(index),
                    scenePacket.objects[index].drawIndex,
                    scenePacket.objects[index].model);
            }
        } else {
            for (const auto& record : scenePacket.instances) {
                if (record.objectIndex >= scenePacket.objects.size()) {
                    continue;
                }
                planDraw(record.objectIndex,
                    scenePacket.objects[record.objectIndex].drawIndex,
                    record.model);
            }
        }
    }
    if (plans.empty()) {
        // No scene table: the packet is one draw stream whose vertices are
        // already in the space they are shaded in.
        GeometryPlan plan{};
        plan.indexCount = static_cast<std::uint32_t>(packet.indices.size());
        plans.push_back(plan);
    }

    // What actually went into the structure. A reflection that finds nothing
    // has two possible causes -- the ray missed, or the geometry was never
    // there -- and without this line they are indistinguishable from outside.
    if (callbacks.log != nullptr) {
        std::string message{"acceleration plans="};
        message += std::to_string(plans.size());
        message += " objects=";
        message += std::to_string(scenePacket.objects.size());
        message += " instances=";
        message += std::to_string(scenePacket.instances.size());
        callbacks.log(callbacks.userData, 1u, message.c_str());
    }
    const auto transformBytes =
        plans.size() * sizeof(VkTransformMatrixKHR);
    if (accelTransforms.capacity < transformBytes) {
        DestroyBuffer(accelTransforms);
        const auto created = CreateHostBuffer(transformBytes,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            accelTransforms, true);
        if (created != abi::Result::Success) {
            return fail("transform-buffer", created);
        }
    }
    for (std::size_t index = 0; index < plans.size(); ++index) {
        std::memcpy(
            static_cast<std::byte*>(accelTransforms.mapped) +
                index * sizeof(VkTransformMatrixKHR),
            plans[index].transform.data(), sizeof(VkTransformMatrixKHR));
    }

    // A ray query recovers which geometry it hit but has no vertex attributes
    // bound, so without this table a reflection hit cannot be shaded at all.
    const auto objectTableBytes = plans.size() * sizeof(std::uint32_t);
    if (accelGeometryObjects.capacity < objectTableBytes) {
        DestroyBuffer(accelGeometryObjects);
        const auto created = CreateHostBuffer(objectTableBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, accelGeometryObjects);
        if (created != abi::Result::Success) {
            return fail("geometry-object-table", created);
        }
        VkDescriptorBufferInfo tableInfo{};
        tableInfo.buffer = accelGeometryObjects.buffer;
        tableInfo.offset = 0;
        tableInfo.range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet tableWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        tableWrite.dstSet = materialSet;
        tableWrite.dstBinding = scene::kSceneGeometryObjectDescriptorBinding;
        tableWrite.descriptorCount = 1;
        tableWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        tableWrite.pBufferInfo = &tableInfo;
        vkUpdateDescriptorSets(device, 1, &tableWrite, 0, nullptr);
    }
    for (std::size_t index = 0; index < plans.size(); ++index) {
        const auto objectIndex = plans[index].objectIndex;
        std::memcpy(
            static_cast<std::byte*>(accelGeometryObjects.mapped) +
                index * sizeof(std::uint32_t),
            &objectIndex, sizeof(objectIndex));
    }

    const auto indexSize =
        packet.header.indexType == raster::IndexType::Uint16
            ? sizeof(std::uint16_t) : sizeof(std::uint32_t);
    // Every other host buffer in this renderer flushes when the memory type
    // is not coherent; these two are read by the build on the GPU and need
    // the same treatment.
    const auto flushHost = [this](const Buffer& buffer) {
        if (buffer.coherent) return true;
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = buffer.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        return vkFlushMappedMemoryRanges(device, 1, &range) == VK_SUCCESS;
    };
    if (!flushHost(accelTransforms)) {
        return fail("transform-flush", abi::Result::RasterRenderFailed);
    }
    const auto transformAddress = bufferAddress(accelTransforms.buffer);
    pendingBlasGeometries.assign(plans.size(),
        VkAccelerationStructureGeometryKHR{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR});
    pendingBlasRanges.assign(plans.size(),
        VkAccelerationStructureBuildRangeInfoKHR{});
    pendingBlasPrimitiveCounts.assign(plans.size(), 0u);
    for (std::size_t index = 0; index < plans.size(); ++index) {
        const auto& plan = plans[index];
        auto& geometry = pendingBlasGeometries[index];
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        // Opaque here because this fixture carries no cutout occluders. A
        // cutout would clear this flag and confirm candidates through the
        // coverage rule, which is what accel::RequiresAnyHit decides.
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& triangles = geometry.geometry.triangles;
        triangles.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress =
            bufferAddress(upload.buffer) + layout.vertexOffset;
        triangles.vertexStride = sizeof(raster::RasterVertexV3);
        triangles.maxVertex =
            static_cast<std::uint32_t>(packet.vertices.size()) - 1;
        // The same rule the draw uses. Hardcoding UINT32 here read this
        // packet's 16-bit indices as 32-bit, turning {0,1,2} into 65536, and
        // the vertex fetch that followed faulted the device.
        triangles.indexType =
            packet.header.indexType == raster::IndexType::Uint16
                ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress =
            bufferAddress(upload.buffer) + layout.indexOffset;
        triangles.transformData.deviceAddress = transformAddress;

        auto& range = pendingBlasRanges[index];
        range.primitiveCount = plan.indexCount / 3;
        // A byte offset into the index buffer, not an index count.
        range.primitiveOffset =
            static_cast<std::uint32_t>(plan.firstIndex * indexSize);
        range.firstVertex = static_cast<std::uint32_t>(plan.vertexOffset);
        range.transformOffset = static_cast<std::uint32_t>(
            index * sizeof(VkTransformMatrixKHR));
        pendingBlasPrimitiveCounts[index] = range.primitiveCount;
    }

    VkAccelerationStructureBuildGeometryInfoKHR blasBuild{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    blasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blasBuild.flags =
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    blasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    blasBuild.geometryCount =
        static_cast<std::uint32_t>(pendingBlasGeometries.size());
    blasBuild.pGeometries = pendingBlasGeometries.data();
    VkAccelerationStructureBuildSizesInfoKHR blasSizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    getAccelerationStructureBuildSizes(device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &blasBuild,
        pendingBlasPrimitiveCounts.data(), &blasSizes);

    if (blasStorage.capacity < blasSizes.accelerationStructureSize) {
        // Only the bottom level and its storage. Calling the full teardown
        // here also destroyed the transform buffer that was created and
        // filled a few lines above, leaving every geometry's transformData
        // pointing at freed memory: geometry 0 survived on whatever the
        // allocator had not reused yet and the rest were flung out of the
        // scene, so exactly one object was ever traceable.
        if (blas != VK_NULL_HANDLE && destroyAccelerationStructure != nullptr) {
            destroyAccelerationStructure(device, blas, nullptr);
            blas = VK_NULL_HANDLE;
        }
        DestroyBuffer(blasStorage);
        const auto created = CreateDeviceBuffer(
            blasSizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            blasStorage);
        if (created != abi::Result::Success) {
            return fail("blas-storage", created);
        }
    }
    if (accelScratch.capacity < blasSizes.buildScratchSize) {
        DestroyBuffer(accelScratch);
        const auto created = CreateDeviceBuffer(blasSizes.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            accelScratch);
        if (created != abi::Result::Success) {
            return fail("blas-scratch", created);
        }
    }
    if (blas == VK_NULL_HANDLE) {
        VkAccelerationStructureCreateInfoKHR createInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        createInfo.buffer = blasStorage.buffer;
        createInfo.size = blasSizes.accelerationStructureSize;
        createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (createAccelerationStructure(device, &createInfo, nullptr,
                &blas) != VK_SUCCESS) {
            return fail("blas-create", abi::Result::RasterCreateFailed);
        }
    }
    blasBuild.dstAccelerationStructure = blas;
    blasBuild.scratchData.deviceAddress = bufferAddress(accelScratch.buffer);

    // One identity instance: the mirror's geometry is already camera
    // relative, so the transform the TLAS needs is the identity.
    VkAccelerationStructureDeviceAddressInfoKHR blasAddressInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    blasAddressInfo.accelerationStructure = blas;
    VkAccelerationStructureInstanceKHR tlasInstance{};
    tlasInstance.transform.matrix[0][0] = 1.0f;
    tlasInstance.transform.matrix[1][1] = 1.0f;
    tlasInstance.transform.matrix[2][2] = 1.0f;
    tlasInstance.mask = accel::kInstanceMaskAll;
    // An occluder shadows from either face, so culling is disabled rather
    // than letting a back-facing triangle stop blocking light.
    tlasInstance.flags =
        VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    tlasInstance.accelerationStructureReference =
        getAccelerationStructureDeviceAddress(device, &blasAddressInfo);
    if (instanceBuffer.capacity < sizeof(tlasInstance)) {
        DestroyBuffer(instanceBuffer);
        const auto created = CreateHostBuffer(sizeof(tlasInstance),
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            instanceBuffer, true);
        if (created != abi::Result::Success) {
            return fail("instance-buffer", created);
        }
    }
    std::memcpy(instanceBuffer.mapped, &tlasInstance, sizeof(tlasInstance));
    if (!flushHost(instanceBuffer)) {
        return fail("instance-flush", abi::Result::RasterRenderFailed);
    }

    VkAccelerationStructureGeometryKHR tlasGeometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeometry.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tlasGeometry.geometry.instances.data.deviceAddress =
        bufferAddress(instanceBuffer.buffer);
    VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasBuild.flags =
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasBuild.geometryCount = 1;
    tlasBuild.pGeometries = &tlasGeometry;
    const std::uint32_t instanceCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR tlasSizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    getAccelerationStructureBuildSizes(device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlasBuild,
        &instanceCount, &tlasSizes);
    if (tlasStorage.capacity < tlasSizes.accelerationStructureSize) {
        DestroyBuffer(tlasStorage);
        if (tlas != VK_NULL_HANDLE) {
            destroyAccelerationStructure(device, tlas, nullptr);
            tlas = VK_NULL_HANDLE;
        }
        const auto created = CreateDeviceBuffer(
            tlasSizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            tlasStorage);
        if (created != abi::Result::Success) {
            return fail("tlas-storage", created);
        }
    }
    if (accelScratch.capacity < tlasSizes.buildScratchSize) {
        DestroyBuffer(accelScratch);
        const auto created = CreateDeviceBuffer(tlasSizes.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            accelScratch);
        if (created != abi::Result::Success) {
            return fail("tlas-scratch", created);
        }
        blasBuild.scratchData.deviceAddress =
            bufferAddress(accelScratch.buffer);
    }
    if (tlas == VK_NULL_HANDLE) {
        VkAccelerationStructureCreateInfoKHR createInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        createInfo.buffer = tlasStorage.buffer;
        createInfo.size = tlasSizes.accelerationStructureSize;
        createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (createAccelerationStructure(device, &createInfo, nullptr,
                &tlas) != VK_SUCCESS) {
            return fail("tlas-create", abi::Result::RasterCreateFailed);
        }
    }
    tlasBuild.dstAccelerationStructure = tlas;
    tlasBuild.scratchData.deviceAddress = bufferAddress(accelScratch.buffer);

    // The builds are recorded into the frame's own command buffer rather
    // than submitted separately. One submission means no second fence to
    // wait on and no command buffer that can be freed while still pending,
    // and the barrier below is what orders the trace after the build.
    pendingBlasBuild = blasBuild;
    // Repointed at the member storage: `blasBuild` is a local, and recording
    // reads these after this function has returned.
    pendingBlasBuild.pGeometries = pendingBlasGeometries.data();
    pendingTlasGeometry = tlasGeometry;
    pendingTlasBuild = tlasBuild;
    pendingTlasBuild.pGeometries = &pendingTlasGeometry;
    pendingTlasRange = {};
    pendingTlasRange.primitiveCount = instanceCount;
    accelBuildPending = true;

    VkWriteDescriptorSetAccelerationStructureKHR accelWrite{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    accelWrite.accelerationStructureCount = 1;
    accelWrite.pAccelerationStructures = &tlas;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.pNext = &accelWrite;
    write.dstSet = materialSet;
    write.dstBinding = scene::kSceneTlasDescriptorBinding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    tlasReady = true;
    return abi::Result::Success;
}

void VulkanRasterRenderer::Impl::DestroyAccelerationStructures() noexcept
{
    if (tlas != VK_NULL_HANDLE && destroyAccelerationStructure != nullptr) {
        destroyAccelerationStructure(device, tlas, nullptr);
        tlas = VK_NULL_HANDLE;
    }
    if (blas != VK_NULL_HANDLE && destroyAccelerationStructure != nullptr) {
        destroyAccelerationStructure(device, blas, nullptr);
        blas = VK_NULL_HANDLE;
    }
    DestroyBuffer(blasStorage);
    DestroyBuffer(tlasStorage);
    DestroyBuffer(accelScratch);
    DestroyBuffer(instanceBuffer);
    DestroyBuffer(accelTransforms);
    DestroyBuffer(accelGeometryObjects);
    // The pending build describes structures that no longer exist, so it must
    // not be recorded. Only the flag is cleared: the geometry and range
    // arrays are plain data refilled by every build, and this function is
    // called from the middle of one -- clearing them there emptied the ranges
    // after they had been filled, and the bottom level was built with zero
    // primitives. An empty structure is traceable and hits nothing, so the
    // shadows simply never appeared.
    accelBuildPending = false;
    tlasReady = false;
}

// Device-local and never mapped. Acceleration-structure storage and build
// scratch are written only by the GPU, and on a discrete adapter host-visible
// memory for them is either unsupported or far slower.
abi::Result VulkanRasterRenderer::Impl::CreateDeviceBuffer(
    const VkDeviceSize size,
    const VkBufferUsageFlags usage,
    Buffer& buffer) noexcept
{
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer.buffer) !=
        VK_SUCCESS) {
        return abi::Result::RasterCreateFailed;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
    const auto memoryType = FindMemoryType(
        memoryProperties, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryType == std::numeric_limits<std::uint32_t>::max()) {
        return abi::Result::RasterCreateFailed;
    }
    VkMemoryAllocateInfo allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    VkMemoryAllocateFlagsInfo allocationFlags{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    allocationFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    allocation.pNext = &allocationFlags;
    if (vkAllocateMemory(device, &allocation, nullptr, &buffer.memory) !=
            VK_SUCCESS ||
        vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0) !=
            VK_SUCCESS) {
        return abi::Result::RasterCreateFailed;
    }
    buffer.mapped = nullptr;
    buffer.coherent = false;
    buffer.capacity = size;
    return abi::Result::Success;
}

abi::Result VulkanRasterRenderer::Impl::CreateHostBuffer(
    const VkDeviceSize size,
    const VkBufferUsageFlags usage,
    Buffer& buffer,
    const bool deviceAddress) noexcept
{
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer.buffer) !=
        VK_SUCCESS) {
        return abi::Result::RasterCreateFailed;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
    const auto memoryType = FindMemoryType(
        memoryProperties, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
            VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (memoryType == std::numeric_limits<std::uint32_t>::max()) {
        return abi::Result::RasterCreateFailed;
    }
    const auto flags =
        memoryProperties.memoryTypes[memoryType].propertyFlags;
    buffer.coherent =
        (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    VkMemoryAllocateInfo allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    // A buffer whose device address is taken must say so at allocation time;
    // asking for the address afterwards is a validation error.
    VkMemoryAllocateFlagsInfo allocationFlags{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    if (deviceAddress) {
        allocationFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        allocation.pNext = &allocationFlags;
    }
    if (vkAllocateMemory(device, &allocation, nullptr, &buffer.memory) !=
            VK_SUCCESS ||
        vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0) !=
            VK_SUCCESS ||
        vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0,
            &buffer.mapped) != VK_SUCCESS) {
        return abi::Result::RasterCreateFailed;
    }
    buffer.capacity = size;
    return abi::Result::Success;
}

abi::Result VulkanRasterRenderer::Impl::CreateExtent(
    const std::uint32_t width,
    const std::uint32_t height) noexcept
{
    if (width == extent.Width() && height == extent.Height() &&
        hdr.image != VK_NULL_HANDLE) {
        return abi::Result::Success;
    }
    const auto dimensionsChanged =
        width != extent.Width() || height != extent.Height();
    DestroyExtent();
    if (dimensionsChanged && !extent.Update(width, height)) {
        return abi::Result::RasterCreateFailed;
    }
    auto result = CreateImage(kHdrFormat,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            // Phase 16 reads the float colour target back so emission can be
            // compared at its authored magnitude instead of an 8-bit clamp.
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, hdr);
    if (result == abi::Result::Success) {
        result = CreateImage(kHdrFormat,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, refraction);
    }
    if (result == abi::Result::Success) {
        result = CreateImage(kDepthFormat,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT, depth);
    }
    if (result == abi::Result::Success) {
        result = CreateImage(kOutputFormat,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, output);
    }
    const auto readbackSize = static_cast<VkDeviceSize>(width) * height * 4;
    if (result == abi::Result::Success) {
        result = CreateHostBuffer(readbackSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback);
    }
    if (result == abi::Result::Success) {
        // Four half-floats per pixel, which is what kHdrFormat stores.
        result = CreateHostBuffer(
            static_cast<VkDeviceSize>(width) * height * kHdrPixelBytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, hdrReadback);
    }
    if (result != abi::Result::Success) {
        DestroyExtent();
        return result;
    }
    // The refraction source is bound once per extent, not per frame: the
    // image is reused every frame and only its contents change.
    VkDescriptorImageInfo refractionInfo{};
    refractionInfo.sampler = sampler;
    refractionInfo.imageView = refraction.view;
    refractionInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet refractionWrite{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    refractionWrite.dstSet = materialSet;
    refractionWrite.dstBinding = scene::kSceneRefractionDescriptorBinding;
    refractionWrite.descriptorCount = 1;
    refractionWrite.descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    refractionWrite.pImageInfo = &refractionInfo;
    vkUpdateDescriptorSets(device, 1, &refractionWrite, 0, nullptr);

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = sampler;
    imageInfo.imageView = hdr.view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = toneSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    extentInitialized = false;
    return abi::Result::Success;
}

abi::Result VulkanRasterRenderer::Impl::PrepareDeformation(
    const std::uint64_t timelineValue) noexcept
{
    if (!phase13DeformActive) return abi::Result::Success;
    const auto vertexCount = deformPacket.vertices.size();
    const auto vertexBytes = vertexCount * sizeof(raster::RasterVertexV3);
    const auto previousBytes = vertexCount * deform::kGpuPreviousPositionSize;
    std::vector<std::byte> inputBytes;
    // Every deformation input section is packed once into a host buffer the
    // compute pass reads; the packet itself never crosses to the GPU.
    const auto influenceBytes = vertexCount * sizeof(deform::DeformVertexV1);
    const auto boneBytes =
        deformPacket.bones.size() * sizeof(deform::BoneTransformV1);
    const auto targetBytes = std::max<std::size_t>(
        deformPacket.morphTargets.size() * sizeof(deform::MorphTargetV1),
        sizeof(deform::MorphTargetV1));
    const auto deltaBytes = std::max<std::size_t>(
        deformPacket.morphDeltas.size() * sizeof(deform::MorphDeltaV1),
        sizeof(deform::MorphDeltaV1));
    const auto alignTo = [](const std::size_t value) {
        return (value + 255) & ~std::size_t{255};
    };
    const std::size_t influenceOffset = 0;
    const auto boneOffset = alignTo(influenceOffset + influenceBytes);
    const auto previousBoneOffset = alignTo(boneOffset + boneBytes);
    const auto targetOffset = alignTo(previousBoneOffset + boneBytes);
    const auto deltaOffset = alignTo(targetOffset + targetBytes);
    const auto inputSize = alignTo(deltaOffset + deltaBytes);
    try {
        inputBytes.assign(inputSize, std::byte{0});
    } catch (...) {
        return abi::Result::InternalFailure;
    }
    std::memcpy(inputBytes.data() + influenceOffset,
        deformPacket.vertices.data(), influenceBytes);
    std::memcpy(inputBytes.data() + boneOffset,
        deformPacket.bones.data(), boneBytes);
    std::memcpy(inputBytes.data() + previousBoneOffset,
        deformPacket.previousBones.data(), boneBytes);
    if (!deformPacket.morphTargets.empty()) {
        std::memcpy(inputBytes.data() + targetOffset,
            deformPacket.morphTargets.data(),
            deformPacket.morphTargets.size() *
                sizeof(deform::MorphTargetV1));
    }
    if (!deformPacket.morphDeltas.empty()) {
        std::memcpy(inputBytes.data() + deltaOffset,
            deformPacket.morphDeltas.data(),
            deformPacket.morphDeltas.size() * sizeof(deform::MorphDeltaV1));
    }

    if (deformInput.capacity < inputSize) {
        DestroyBuffer(deformInput);
        const auto created = CreateHostBuffer(inputSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, deformInput);
        if (created != abi::Result::Success) return created;
    }
    std::memcpy(deformInput.mapped, inputBytes.data(), inputSize);
    if (!deformInput.coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = deformInput.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        if (vkFlushMappedMemoryRanges(device, 1, &range) != VK_SUCCESS) {
                return abi::Result::RasterRenderFailed;
        }
    }

    const auto ringBytes = std::max<VkDeviceSize>(
        kMinimumDeformRingBytes,
        static_cast<VkDeviceSize>(vertexBytes + previousBytes) * 4);
    if (deformOutput.capacity < ringBytes) {
        DestroyBuffer(deformOutput);
        const auto created = CreateHostBuffer(ringBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            deformOutput);
        if (created != abi::Result::Success) return created;
        deformRing = deform::DynamicRing{ringBytes, kDeformRingAlignment};
    }
    if (deformReadback.capacity < vertexBytes + previousBytes) {
        DestroyBuffer(deformReadback);
        const auto created = CreateHostBuffer(vertexBytes + previousBytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, deformReadback);
        if (created != abi::Result::Success) return created;
    }

    // The ring refuses to hand out bytes an earlier submission still reads.
    deformRing.Retire(submissions);
    if (!deformRing.Allocate(vertexBytes, timelineValue,
            deformVertexRange) ||
        !deformRing.Allocate(previousBytes, timelineValue,
            deformPreviousRange)) {
        return abi::Result::RasterRenderFailed;
    }

    const std::array<VkDescriptorBufferInfo,
        deform::kDeformBindingCount> bufferInfos{
        VkDescriptorBufferInfo{upload.buffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{deformInput.buffer, influenceOffset,
            influenceBytes},
        VkDescriptorBufferInfo{deformInput.buffer, boneOffset, boneBytes},
        VkDescriptorBufferInfo{deformInput.buffer, previousBoneOffset,
            boneBytes},
        VkDescriptorBufferInfo{deformInput.buffer, targetOffset,
            targetBytes},
        VkDescriptorBufferInfo{deformInput.buffer, deltaOffset, deltaBytes},
        VkDescriptorBufferInfo{deformOutput.buffer,
            deformVertexRange.offset, vertexBytes},
        VkDescriptorBufferInfo{deformOutput.buffer,
            deformPreviousRange.offset, previousBytes},
    };
    std::array<VkWriteDescriptorSet,
        deform::kDeformBindingCount> writes{};
    for (std::uint32_t index = 0; index < writes.size(); ++index) {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = deformSet;
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[index].pBufferInfo = &bufferInfos[index];
    }
    vkUpdateDescriptorSets(device,
        static_cast<std::uint32_t>(writes.size()), writes.data(),
        0, nullptr);
    return abi::Result::Success;
}

// The temporal pass's buffers: current pixels, previous pixels and incoming
// history packed into one upload, and the results into another. Host visible
// on both sides because the caller hands over a history and reads the result
// back within the same frame; there is no persistent device-side history to
// keep, which is what a shipping renderer would have and a verification
// vertical deliberately does not.
abi::Result VulkanRasterRenderer::Impl::PrepareIndirect(
    const abi::RasterFrameRequestV1& request) noexcept
{
    indirectPixelCount = 0;
    if (request.structSize < abi::kRasterFrameRequestV1IndirectRequiredSize ||
        request.indirectPixelCount == 0 ||
        request.indirectCurrentData == 0 ||
        request.indirectPreviousData == 0 ||
        request.indirectHistoryData == 0 ||
        request.indirectResultData == 0) {
        return abi::Result::Success;
    }
    const auto pixels =
        static_cast<std::size_t>(request.indirectPixelCount);
    // The extent has to describe the buffer it came with. A width and height
    // whose product is not the pixel count would make the shader recover the
    // wrong row for every pixel past the first, which reads as reprojection
    // being broken rather than as two inputs disagreeing.
    if (static_cast<std::uint64_t>(request.indirectWidth) *
            request.indirectHeight != request.indirectPixelCount) {
        return abi::Result::InvalidArgument;
    }
    std::size_t pixelBytes{};
    std::size_t historyBytes{};
    std::size_t resultBytes{};
    if (!CheckedMultiply(pixels, sizeof(gi::GpuIndirectPixelV1), pixelBytes) ||
        !CheckedMultiply(pixels, sizeof(gi::GpuIndirectHistoryV1),
            historyBytes) ||
        !CheckedMultiply(pixels, sizeof(gi::GpuIndirectResultV1),
            resultBytes)) {
        return abi::Result::InvalidArgument;
    }
    if (request.indirectResultCapacity < resultBytes) {
        return abi::Result::InvalidArgument;
    }
    const auto uploadBytes = pixelBytes * 2 + historyBytes;

    if (indirectInput.capacity < uploadBytes) {
        DestroyBuffer(indirectInput);
        const auto created = CreateHostBuffer(uploadBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, indirectInput);
        if (created != abi::Result::Success) return created;
    }
    if (indirectOutput.capacity < resultBytes) {
        DestroyBuffer(indirectOutput);
        const auto created = CreateHostBuffer(resultBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, indirectOutput);
        if (created != abi::Result::Success) return created;
    }

    auto* const destination = static_cast<std::byte*>(indirectInput.mapped);
    if (destination == nullptr) return abi::Result::RasterRenderFailed;
    std::memcpy(destination, reinterpret_cast<const void*>(
        static_cast<std::uintptr_t>(request.indirectCurrentData)), pixelBytes);
    std::memcpy(destination + pixelBytes, reinterpret_cast<const void*>(
        static_cast<std::uintptr_t>(request.indirectPreviousData)),
        pixelBytes);
    std::memcpy(destination + pixelBytes * 2, reinterpret_cast<const void*>(
        static_cast<std::uintptr_t>(request.indirectHistoryData)),
        historyBytes);

    const std::array<VkDescriptorBufferInfo, gi::kIndirectBindingCount>
        bufferInfos{
            VkDescriptorBufferInfo{indirectInput.buffer, 0, pixelBytes},
            VkDescriptorBufferInfo{indirectInput.buffer, pixelBytes,
                pixelBytes},
            VkDescriptorBufferInfo{indirectInput.buffer, pixelBytes * 2,
                historyBytes},
            VkDescriptorBufferInfo{indirectOutput.buffer, 0, resultBytes},
        };
    std::array<VkWriteDescriptorSet, gi::kIndirectBindingCount> writes{};
    for (std::uint32_t index = 0; index < writes.size(); ++index) {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = indirectSet;
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[index].pBufferInfo = &bufferInfos[index];
    }
    vkUpdateDescriptorSets(device,
        static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    indirectPixelCount = static_cast<std::uint32_t>(pixels);
    indirectWidth_ = request.indirectWidth;
    indirectHeight_ = request.indirectHeight;
    indirectEpochMatches_ = request.indirectEpochMatches;
    return abi::Result::Success;
}

void VulkanRasterRenderer::Impl::RecordIndirect() noexcept
{
    if (indirectPixelCount == 0) return;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
        indirectPipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
        indirectPipelineLayout, 0, 1, &indirectSet, 0, nullptr);
    IndirectPushConstants push{};
    push.width = indirectWidth_;
    push.height = indirectHeight_;
    push.pixelCount = indirectPixelCount;
    push.epochMatches = indirectEpochMatches_;
    vkCmdPushConstants(command, indirectPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    constexpr std::uint32_t kWorkgroupSize = 64;
    const auto groups =
        (indirectPixelCount + kWorkgroupSize - 1) / kWorkgroupSize;
    vkCmdDispatch(command, groups, 1, 1);
    // The host reads the result through a coherent mapping, so the barrier
    // that matters is the one making the shader's writes visible to the host
    // rather than one between two device stages.
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command, &dependency);
}

abi::Result VulkanRasterRenderer::Impl::CopyIndirectResults(
    const abi::RasterFrameRequestV1& request) noexcept
{
    if (indirectPixelCount == 0) return abi::Result::Success;
    const auto* const source =
        static_cast<const std::byte*>(indirectOutput.mapped);
    if (source == nullptr) return abi::Result::RasterRenderFailed;
    const auto bytes = static_cast<std::size_t>(indirectPixelCount) *
        sizeof(gi::GpuIndirectResultV1);
    std::memcpy(reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(request.indirectResultData)),
        source, bytes);
    return abi::Result::Success;
}

abi::Result VulkanRasterRenderer::Impl::CopyDeformationOutput(
    const abi::RasterFrameRequestV1& request) noexcept
{
    const auto vertexCount = deformPacket.vertices.size();
    const auto required = vertexCount * deform::kGpuDeformOutputSize;
    if (!phase13DeformActive || vertexCount == 0 ||
        request.deformationOutputData == 0 ||
        request.deformationOutputCapacity < required ||
        deformReadback.mapped == nullptr) {
        return abi::Result::InvalidArgument;
    }
    if (!deformReadback.coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = deformReadback.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        if (vkInvalidateMappedMemoryRanges(device, 1, &range) != VK_SUCCESS) {
                return abi::Result::RasterRenderFailed;
        }
    }
    const auto* source =
        static_cast<const std::byte*>(deformReadback.mapped);
    const auto* previousSource = source +
        vertexCount * sizeof(raster::RasterVertexV3);
    auto* destination = reinterpret_cast<std::byte*>(
        static_cast<std::uintptr_t>(request.deformationOutputData));
    for (std::size_t index = 0; index < vertexCount; ++index) {
        raster::RasterVertexV3 vertex{};
        std::memcpy(&vertex,
            source + index * sizeof(raster::RasterVertexV3),
            sizeof(vertex));
        float current[4]{vertex.position[0], vertex.position[1],
            vertex.position[2], 1.0f};
        std::memcpy(destination + index * deform::kGpuDeformOutputSize,
            current, sizeof(current));
        std::memcpy(destination + index * deform::kGpuDeformOutputSize +
                sizeof(current),
            previousSource + index * deform::kGpuPreviousPositionSize,
            deform::kGpuPreviousPositionSize);
    }
    return abi::Result::Success;
}

abi::Result VulkanRasterRenderer::Impl::CreateSceneAttachments() noexcept
{
    // The mirrored planes cost four extra render targets, so they are
    // created the first time a scene packet arrives and released with the
    // extent they belong to.
    const auto requiredBytes = static_cast<VkDeviceSize>(extent.Width()) *
        extent.Height() * scene::kGpuGBufferPixelSize;
    if (requiredBytes == 0) {
        return abi::Result::RasterCreateFailed;
    }
    if (gbuffer.front().image != VK_NULL_HANDLE &&
        gbufferReadback.capacity >= requiredBytes) {
        return abi::Result::Success;
    }
    auto result = abi::Result::Success;
    for (std::size_t plane = 0;
         result == abi::Result::Success && plane < gbuffer.size(); ++plane) {
        if (gbuffer[plane].image != VK_NULL_HANDLE) continue;
        result = CreateImage(kGBufferFormats[plane],
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, gbuffer[plane]);
    }
    if (result == abi::Result::Success &&
        gbufferReadback.capacity < requiredBytes) {
        DestroyBuffer(gbufferReadback);
        result = CreateHostBuffer(requiredBytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, gbufferReadback);
    }
    return result;
}

VulkanRasterRenderer::Impl::UploadLayout
VulkanRasterRenderer::Impl::BuildUploadLayout(
    const raster::DecodedPacket& packet) const noexcept
{
    UploadLayout layout{};
    std::size_t vertexBytes{};
    std::size_t indexBytes{};
    if (!CheckedMultiply(packet.vertices.size(),
            sizeof(raster::RasterVertexV3), vertexBytes) ||
        !CheckedMultiply(packet.indices.size(),
            static_cast<std::size_t>(packet.header.indexType), indexBytes)) {
        layout.totalSize = std::numeric_limits<VkDeviceSize>::max();
        return layout;
    }
    const auto uniformAlignment = std::max<std::size_t>(
        static_cast<std::size_t>(
            properties.limits.minUniformBufferOffsetAlignment),
        alignof(raster::GpuMaterialConstants));
    layout.vertexOffset = 0;
    layout.indexOffset = AlignUp(vertexBytes, 4);
    std::size_t indexEnd{};
    if (layout.indexOffset == std::numeric_limits<std::size_t>::max() ||
        !CheckedAdd(static_cast<std::size_t>(layout.indexOffset),
            indexBytes, indexEnd)) {
        layout.totalSize = std::numeric_limits<VkDeviceSize>::max();
        return layout;
    }
    layout.materialOffset = AlignUp(indexEnd, uniformAlignment);
    layout.materialStride = AlignUp(
        sizeof(raster::GpuMaterialConstants), uniformAlignment);
    std::size_t materialBytes{};
    std::size_t materialEnd{};
    if (layout.materialOffset == std::numeric_limits<std::size_t>::max() ||
        layout.materialStride == std::numeric_limits<std::size_t>::max() ||
        !CheckedMultiply(packet.materials.size(),
            static_cast<std::size_t>(layout.materialStride),
            materialBytes) ||
        !CheckedAdd(static_cast<std::size_t>(layout.materialOffset),
            materialBytes, materialEnd)) {
        layout.totalSize = std::numeric_limits<VkDeviceSize>::max();
        return layout;
    }
    try {
        auto cursor = materialEnd;
        if (phase9MaterialActive) {
            cursor = AlignUp(cursor, 16);
            if (cursor == std::numeric_limits<std::size_t>::max()) {
                layout.totalSize =
                    std::numeric_limits<VkDeviceSize>::max();
                return layout;
            }
            layout.phase9StaticOffset = cursor;
            if (!CheckedAdd(cursor,
                    sizeof(material::GpuMaterialStaticV1), cursor)) {
                layout.totalSize =
                    std::numeric_limits<VkDeviceSize>::max();
                return layout;
            }
            cursor = AlignUp(cursor, 16);
            if (cursor == std::numeric_limits<std::size_t>::max()) {
                layout.totalSize =
                    std::numeric_limits<VkDeviceSize>::max();
                return layout;
            }
            layout.phase9DynamicOffset = cursor;
            if (!CheckedAdd(cursor,
                    sizeof(material::GpuMaterialDynamicV1), cursor)) {
                layout.totalSize =
                    std::numeric_limits<VkDeviceSize>::max();
                return layout;
            }
        }
        cursor = AlignUp(cursor, uniformAlignment);
        if (cursor == std::numeric_limits<std::size_t>::max()) {
            layout.totalSize = std::numeric_limits<VkDeviceSize>::max();
            return layout;
        }
        layout.phase10ViewOffset = cursor;
        if (!CheckedAdd(cursor, sizeof(view::GpuViewConstantsV1), cursor)) {
            layout.totalSize = std::numeric_limits<VkDeviceSize>::max();
            return layout;
        }
        if (phase11SceneActive) {
            const auto storageAlignment = std::max<std::size_t>(
                static_cast<std::size_t>(
                    properties.limits.minStorageBufferOffsetAlignment),
                alignof(scene::OpaqueObjectV1));
            std::size_t objectBytes{};
            cursor = AlignUp(cursor, storageAlignment);
            if (cursor == std::numeric_limits<std::size_t>::max() ||
                !CheckedMultiply(scenePacket.objects.size(),
                    sizeof(scene::OpaqueObjectV1), objectBytes) ||
                objectBytes == 0) {
                layout.totalSize =
                    std::numeric_limits<VkDeviceSize>::max();
                return layout;
            }
            layout.phase11ObjectsOffset = cursor;
            layout.phase11ObjectBytes = objectBytes;
            if (!CheckedAdd(cursor, objectBytes, cursor)) {
                layout.totalSize =
                    std::numeric_limits<VkDeviceSize>::max();
                return layout;
            }
            // Version 1.0 scenes have one implicit instance per object, so
            // the instance table is always present for the shader.
            const auto instanceCount = scenePacket.instances.empty()
                ? scenePacket.objects.size() : scenePacket.instances.size();
            std::size_t instanceBytes{};
            cursor = AlignUp(cursor, storageAlignment);
            if (cursor == std::numeric_limits<std::size_t>::max() ||
                !CheckedMultiply(instanceCount, sizeof(scene::InstanceV1),
                    instanceBytes) ||
                instanceBytes == 0) {
                layout.totalSize =
                    std::numeric_limits<VkDeviceSize>::max();
                return layout;
            }
            layout.phase12InstancesOffset = cursor;
            layout.phase12InstanceBytes = instanceBytes;
            if (!CheckedAdd(cursor, instanceBytes, cursor)) {
                layout.totalSize =
                    std::numeric_limits<VkDeviceSize>::max();
                return layout;
            }
            // A scene without a captured visibility table still needs a valid
            // binding, so one implicit opaque record per object is uploaded.
            std::size_t visibilityBytes{};
            cursor = AlignUp(cursor, storageAlignment);
            if (cursor == std::numeric_limits<std::size_t>::max() ||
                !CheckedMultiply(scenePacket.objects.size(),
                    sizeof(visibility::VisibilityRecordV1),
                    visibilityBytes) ||
                visibilityBytes == 0) {
                layout.totalSize =
                    std::numeric_limits<VkDeviceSize>::max();
                return layout;
            }
            layout.phase15VisibilityOffset = cursor;
            layout.phase15VisibilityBytes = visibilityBytes;
            if (!CheckedAdd(cursor, visibilityBytes, cursor)) {
                layout.totalSize =
                    std::numeric_limits<VkDeviceSize>::max();
                return layout;
            }
            // A scene without a captured family table still needs a valid
            // binding, so one implicit ordinary-surface record per object is
            // uploaded, exactly as visibility does.
            std::size_t familyBytes{};
            cursor = AlignUp(cursor, std::max<std::size_t>(
                storageAlignment, alignof(material::GpuFamilyRecordV1)));
            if (cursor == std::numeric_limits<std::size_t>::max() ||
                !CheckedMultiply(scenePacket.objects.size(),
                    sizeof(material::GpuFamilyRecordV1), familyBytes) ||
                familyBytes == 0) {
                layout.totalSize =
                    std::numeric_limits<VkDeviceSize>::max();
                return layout;
            }
            layout.phase16FamilyOffset = cursor;
            layout.phase16FamilyBytes = familyBytes;
            if (!CheckedAdd(cursor, familyBytes, cursor)) {
                layout.totalSize =
                    std::numeric_limits<VkDeviceSize>::max();
                return layout;
            }
            // A scene without a captured light list still needs both
            // bindings, so an empty list uploads one inert record and an
            // environment whose active count is zero.
            const auto lightCount = std::max<std::size_t>(
                lightPacket.lights.size(), 1);
            std::size_t lightBytes{};
            cursor = AlignUp(cursor, std::max<std::size_t>(
                storageAlignment, alignof(lighting::GpuLightRecordV1)));
            if (cursor == std::numeric_limits<std::size_t>::max() ||
                !CheckedMultiply(lightCount,
                    sizeof(lighting::GpuLightRecordV1), lightBytes)) {
                layout.totalSize =
                    std::numeric_limits<VkDeviceSize>::max();
                return layout;
            }
            layout.phase17LightOffset = cursor;
            layout.phase17LightBytes = lightBytes;
            if (!CheckedAdd(cursor, lightBytes, cursor)) {
                layout.totalSize =
                    std::numeric_limits<VkDeviceSize>::max();
                return layout;
            }
            cursor = AlignUp(cursor, std::max<std::size_t>(
                storageAlignment, alignof(lighting::GpuEnvironmentV1)));
            if (cursor == std::numeric_limits<std::size_t>::max()) {
                layout.totalSize =
                    std::numeric_limits<VkDeviceSize>::max();
                return layout;
            }
            layout.phase17EnvironmentOffset = cursor;
            layout.phase17EnvironmentBytes =
                sizeof(lighting::GpuEnvironmentV1);
            if (!CheckedAdd(cursor, sizeof(lighting::GpuEnvironmentV1),
                    cursor)) {
                layout.totalSize =
                    std::numeric_limits<VkDeviceSize>::max();
                return layout;
            }
        }
        if (phase14TerrainActive) {
            const auto storageAlignment = std::max<std::size_t>(
                static_cast<std::size_t>(
                    properties.limits.minStorageBufferOffsetAlignment),
                alignof(terrain::GpuTerrainCellV1));
            const auto reserve = [&cursor, &layout](
                const std::size_t alignment,
                const std::size_t bytes,
                VkDeviceSize& offset,
                VkDeviceSize& size) {
                cursor = AlignUp(cursor, alignment);
                if (cursor == std::numeric_limits<std::size_t>::max() ||
                    bytes == 0 || !CheckedAdd(cursor, bytes, cursor)) {
                    layout.totalSize =
                        std::numeric_limits<VkDeviceSize>::max();
                    return false;
                }
                size = bytes;
                offset = cursor - bytes;
                return true;
            };
            std::size_t cellBytes{};
            std::size_t layerBytes{};
            std::size_t terrainVertexBytes{};
            std::size_t terrainIndexBytes{};
            if (!CheckedMultiply(terrainCellRecords.size(),
                    sizeof(terrain::GpuTerrainCellV1), cellBytes) ||
                !CheckedMultiply(terrainPacket.layers.size(),
                    sizeof(terrain::LandscapeLayerV1), layerBytes) ||
                !CheckedMultiply(terrainPacket.vertices.size(),
                    sizeof(terrain::LandscapeVertexV1),
                    terrainVertexBytes) ||
                !CheckedMultiply(terrainPacket.indices.size(),
                    sizeof(std::uint32_t), terrainIndexBytes)) {
                layout.totalSize = std::numeric_limits<VkDeviceSize>::max();
                return layout;
            }
            if (!reserve(storageAlignment, cellBytes,
                    layout.phase14CellsOffset, layout.phase14CellBytes) ||
                !reserve(storageAlignment, layerBytes,
                    layout.phase14LayersOffset, layout.phase14LayerBytes) ||
                !reserve(alignof(terrain::LandscapeVertexV1),
                    terrainVertexBytes, layout.phase14VertexOffset,
                    layout.phase14VertexBytes) ||
                !reserve(4, terrainIndexBytes, layout.phase14IndexOffset,
                    layout.phase14IndexBytes)) {
                return layout;
            }
        }
        for (std::size_t slot = 0;
             slot < sampledResources.size(); ++slot) {
            const auto& resource = sampledResources[slot];
            if (!resource.uploadPending) continue;
            layout.textureSubresourceOffsets[slot].reserve(
                resource.source.subresources.size());
            for (const auto& subresource : resource.source.subresources) {
                cursor = AlignUp(cursor, 16);
                if (cursor == std::numeric_limits<std::size_t>::max() ||
                    subresource.bytes.size() >
                        std::numeric_limits<std::size_t>::max() - cursor) {
                    layout.totalSize =
                        std::numeric_limits<VkDeviceSize>::max();
                    return layout;
                }
                layout.textureSubresourceOffsets[slot].push_back(cursor);
                cursor += subresource.bytes.size();
            }
        }
        layout.totalSize = cursor;
    } catch (...) {
        layout.totalSize = std::numeric_limits<VkDeviceSize>::max();
    }
    return layout;
}

abi::Result VulkanRasterRenderer::Impl::UploadPacket(
    const raster::DecodedPacket& packet,
    const UploadLayout& layout) noexcept
{
    if (layout.totalSize == 0 ||
        layout.totalSize == std::numeric_limits<VkDeviceSize>::max() ||
        layout.materialOffset > std::numeric_limits<std::uint32_t>::max() ||
        layout.totalSize > std::numeric_limits<std::uint32_t>::max()) {
        return abi::Result::RasterInvalidPacket;
    }
    if (upload.capacity < layout.totalSize) {
        DestroyBuffer(upload);
        // The same buffer feeds the acceleration-structure build, so it also
        // needs the build-input and device-address usages when ray query is
        // available. Adding them unconditionally would fail on a device that
        // never enabled the extension.
        auto uploadUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (rayQuerySupported) {
            uploadUsage |=
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        }
        const auto result = CreateHostBuffer(layout.totalSize, uploadUsage,
            upload, rayQuerySupported);
        if (result != abi::Result::Success) {
            return result;
        }
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = upload.buffer;
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(raster::GpuMaterialConstants);
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = materialSet;
        write.dstBinding = raster::kMaterialDescriptorBinding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    auto* destination = static_cast<std::byte*>(upload.mapped);
    std::memcpy(destination + layout.vertexOffset,
        packet.vertices.data(),
        packet.vertices.size() * sizeof(raster::RasterVertexV3));
    if (packet.header.indexType == raster::IndexType::Uint16) {
        for (std::size_t index = 0; index < packet.indices.size(); ++index) {
            const auto narrowed = static_cast<std::uint16_t>(
                packet.indices[index]);
            std::memcpy(destination + layout.indexOffset +
                    index * sizeof(narrowed),
                &narrowed, sizeof(narrowed));
        }
    } else {
        std::memcpy(destination + layout.indexOffset,
            packet.indices.data(),
            packet.indices.size() * sizeof(std::uint32_t));
    }
    for (std::size_t index = 0; index < packet.materials.size(); ++index) {
        raster::GpuMaterialConstants constants{};
        std::memcpy(constants.baseColor,
            packet.materials[index].baseColor,
            sizeof(constants.baseColor));
        std::memcpy(destination + layout.materialOffset +
                index * layout.materialStride,
            &constants, sizeof(constants));
    }
    if (phase9MaterialActive) {
        std::memcpy(destination + layout.phase9StaticOffset,
            &materialRecords.staticRecord,
            sizeof(materialRecords.staticRecord));
        std::memcpy(destination + layout.phase9DynamicOffset,
            &materialRecords.dynamicRecord,
            sizeof(materialRecords.dynamicRecord));
        const std::array bufferInfos{
            VkDescriptorBufferInfo{upload.buffer,
                layout.phase9StaticOffset,
                sizeof(material::GpuMaterialStaticV1)},
            VkDescriptorBufferInfo{upload.buffer,
                layout.phase9DynamicOffset,
                sizeof(material::GpuMaterialDynamicV1)},
        };
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (std::size_t index = 0; index < writes.size(); ++index) {
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = materialSet;
            writes[index].dstBinding = 4u + static_cast<std::uint32_t>(index);
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[index].pBufferInfo = &bufferInfos[index];
        }
        vkUpdateDescriptorSets(device,
            static_cast<std::uint32_t>(writes.size()), writes.data(),
            0, nullptr);
    }
    if (phase11SceneActive) {
        if (layout.phase11ObjectBytes !=
            scenePacket.objects.size() * sizeof(scene::OpaqueObjectV1)) {
            return abi::Result::RasterInvalidPacket;
        }
        std::memcpy(destination + layout.phase11ObjectsOffset,
            scenePacket.objects.data(), layout.phase11ObjectBytes);
        std::size_t written = 0;
        for (std::size_t objectIndex = 0;
             objectIndex < scenePacket.objects.size(); ++objectIndex) {
            const auto range = scene::ObjectInstanceRange(
                scenePacket, objectIndex);
            for (std::uint32_t element = 0; element < range.count;
                 ++element, ++written) {
                // Instance transforms arrive camera-relative by contract, so
                // there is nothing to narrow here. A capture whose engine
                // supplies absolute transforms narrows them once, where it
                // builds the packet, rather than every consumer of the packet
                // having to know which of the two it was handed.
                const auto record = scene::ResolveInstance(
                    scenePacket, objectIndex, range.first + element);
                std::memcpy(destination + layout.phase12InstancesOffset +
                        written * sizeof(record),
                    &record, sizeof(record));
            }
        }
        if (written * sizeof(scene::InstanceV1) !=
            layout.phase12InstanceBytes) {
            return abi::Result::RasterInvalidPacket;
        }
        // Version 1.0 and 1.1 scenes resolve to an implicit opaque record, so
        // the shader always has a valid per-object entry to read.
        for (std::size_t objectIndex = 0;
             objectIndex < scenePacket.objects.size(); ++objectIndex) {
            const auto record = scene::ResolveVisibility(
                scenePacket, objectIndex);
            std::memcpy(destination + layout.phase15VisibilityOffset +
                    objectIndex * sizeof(record),
                &record, sizeof(record));
        }
        // An object with no captured family resolves to the ordinary lit
        // surface, so the shader always has a valid per-object entry.
        for (std::size_t objectIndex = 0;
             objectIndex < scenePacket.objects.size(); ++objectIndex) {
            const auto record = material::BuildFamilyGpuRecord(
                material::ResolveFamilyRecord(familyPacket,
                    scenePacket.objects[objectIndex].objectId));
            std::memcpy(destination + layout.phase16FamilyOffset +
                    objectIndex * sizeof(record),
                &record, sizeof(record));
        }
        // The captured light list, or one inert record when the frame has
        // none, so the binding is always valid. Light positions are narrowed
        // against the same camera origin the geometry uses, in double, so a
        // light and the surface it lights agree about where they are.
        const std::array<double, 3> cameraOrigin{
            viewRecord.cameraRelativeOrigin[0],
            viewRecord.cameraRelativeOrigin[1],
            viewRecord.cameraRelativeOrigin[2]};
        {
            const auto activeLights = std::min<std::size_t>(
                lightPacket.lights.size(), lighting::kMaximumActiveLights);
            for (std::size_t index = 0;
                 index * sizeof(lighting::GpuLightRecordV1) <
                     layout.phase17LightBytes;
                 ++index) {
                lighting::GpuLightRecordV1 record{};
                if (index < activeLights) {
                    if (lighting::BuildGpuLight(lightPacket.lights[index],
                            cameraOrigin, record) !=
                        lighting::LightError::None) {
                        return abi::Result::RasterInvalidPacket;
                    }
                }
                std::memcpy(destination + layout.phase17LightOffset +
                        index * sizeof(record), &record, sizeof(record));
            }
            // Built only when the frame actually carried a light packet. A
            // zeroed environment has the "present" bit clear, which the
            // shader reads as "this frame has no lighting" and leaves the
            // albedo alone, exactly as every phase before this one did.
            const auto environment = phase17LightingActive
                ? lighting::BuildGpuEnvironment(lightPacket.environment,
                      static_cast<std::uint32_t>(activeLights))
                : lighting::GpuEnvironmentV1{};
            std::memcpy(destination + layout.phase17EnvironmentOffset,
                &environment, sizeof(environment));
        }
        const std::array sceneBufferInfos{
            VkDescriptorBufferInfo{upload.buffer,
                layout.phase11ObjectsOffset, layout.phase11ObjectBytes},
            VkDescriptorBufferInfo{upload.buffer,
                layout.phase12InstancesOffset, layout.phase12InstanceBytes},
            VkDescriptorBufferInfo{upload.buffer,
                layout.phase15VisibilityOffset,
                layout.phase15VisibilityBytes},
            VkDescriptorBufferInfo{upload.buffer,
                layout.phase16FamilyOffset, layout.phase16FamilyBytes},
            VkDescriptorBufferInfo{upload.buffer,
                layout.phase17LightOffset, layout.phase17LightBytes},
            VkDescriptorBufferInfo{upload.buffer,
                layout.phase17EnvironmentOffset,
                layout.phase17EnvironmentBytes},
        };
        const std::array sceneBindings{
            scene::kSceneObjectDescriptorBinding,
            scene::kSceneInstanceDescriptorBinding,
            scene::kSceneVisibilityDescriptorBinding,
            scene::kSceneFamilyDescriptorBinding,
            scene::kSceneLightDescriptorBinding,
            scene::kSceneEnvironmentDescriptorBinding,
        };
        std::array<VkWriteDescriptorSet, 6> sceneWrites{};
        for (std::size_t index = 0; index < sceneWrites.size(); ++index) {
            sceneWrites[index].sType =
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            sceneWrites[index].dstSet = materialSet;
            sceneWrites[index].dstBinding = sceneBindings[index];
            sceneWrites[index].descriptorCount = 1;
            sceneWrites[index].descriptorType =
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            sceneWrites[index].pBufferInfo = &sceneBufferInfos[index];
        }
        vkUpdateDescriptorSets(device,
            static_cast<std::uint32_t>(sceneWrites.size()),
            sceneWrites.data(), 0, nullptr);
    }
    if (phase14TerrainActive) {
        std::memcpy(destination + layout.phase14CellsOffset,
            terrainCellRecords.data(),
            static_cast<std::size_t>(layout.phase14CellBytes));
        // The layer table is uploaded verbatim; the GPU record and the
        // captured record are the same 32 bytes.
        std::memcpy(destination + layout.phase14LayersOffset,
            terrainPacket.layers.data(),
            static_cast<std::size_t>(layout.phase14LayerBytes));
        std::memcpy(destination + layout.phase14VertexOffset,
            terrainPacket.vertices.data(),
            static_cast<std::size_t>(layout.phase14VertexBytes));
        std::memcpy(destination + layout.phase14IndexOffset,
            terrainPacket.indices.data(),
            static_cast<std::size_t>(layout.phase14IndexBytes));
        const std::array terrainBufferInfos{
            VkDescriptorBufferInfo{upload.buffer,
                layout.phase14CellsOffset, layout.phase14CellBytes},
            VkDescriptorBufferInfo{upload.buffer,
                layout.phase14LayersOffset, layout.phase14LayerBytes},
        };
        const std::array terrainBindings{
            terrain::kTerrainCellDescriptorBinding,
            terrain::kTerrainLayerDescriptorBinding,
        };
        std::array<VkWriteDescriptorSet, 2> terrainWrites{};
        for (std::size_t index = 0; index < terrainWrites.size(); ++index) {
            terrainWrites[index].sType =
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            terrainWrites[index].dstSet = materialSet;
            terrainWrites[index].dstBinding = terrainBindings[index];
            terrainWrites[index].descriptorCount = 1;
            terrainWrites[index].descriptorType =
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            terrainWrites[index].pBufferInfo = &terrainBufferInfos[index];
        }
        vkUpdateDescriptorSets(device,
            static_cast<std::uint32_t>(terrainWrites.size()),
            terrainWrites.data(), 0, nullptr);
    }
    std::memcpy(destination + layout.phase10ViewOffset,
        &viewConstants, sizeof(viewConstants));
    const VkDescriptorBufferInfo viewBufferInfo{
        upload.buffer,
        layout.phase10ViewOffset,
        sizeof(view::GpuViewConstantsV1)};
    VkWriteDescriptorSet viewWrite{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    viewWrite.dstSet = materialSet;
    viewWrite.dstBinding = view::kViewDescriptorBinding;
    viewWrite.descriptorCount = 1;
    viewWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    viewWrite.pBufferInfo = &viewBufferInfo;
    vkUpdateDescriptorSets(device, 1, &viewWrite, 0, nullptr);
    for (std::size_t slot = 0;
         slot < sampledResources.size(); ++slot) {
        const auto& resource = sampledResources[slot];
        if (!resource.uploadPending) continue;
        if (layout.textureSubresourceOffsets[slot].size() !=
            resource.source.subresources.size()) {
            return abi::Result::RasterInvalidPacket;
        }
        for (std::size_t index = 0;
             index < resource.source.subresources.size(); ++index) {
            const auto& subresource = resource.source.subresources[index];
            std::memcpy(destination +
                    layout.textureSubresourceOffsets[slot][index],
                subresource.bytes.data(), subresource.bytes.size());
        }
    }
    if (!upload.coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = upload.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        if (vkFlushMappedMemoryRanges(device, 1, &range) != VK_SUCCESS) {
                return abi::Result::RasterRenderFailed;
        }
    }
    return abi::Result::Success;
}

abi::Result VulkanRasterRenderer::Impl::RecordAlphaDraws(
    const raster::DecodedPacket& packet,
    const UploadLayout& layout,
    const std::vector<std::size_t>& objects,
    const bool depthOnly) noexcept
{
    const raster::MaterialRegistry registry{packet.materials};
    std::uint32_t firstInstance = 0;
    for (std::size_t index = 0; index < scenePacket.objects.size(); ++index) {
        const auto range = scene::ObjectInstanceRange(scenePacket, index);
        const auto selected = std::find(objects.begin(), objects.end(),
            index) != objects.end();
        if (!selected) {
            firstInstance += range.count;
            continue;
        }
        const auto& object = scenePacket.objects[index];
        const auto drawIndex = static_cast<std::size_t>(object.drawIndex);
        if (drawIndex >= packet.draws.size()) {
            return abi::Result::RasterInvalidPacket;
        }
        const auto& draw = packet.draws[drawIndex];
        const auto materialIndex = registry.IndexOf(draw.materialId);
        if (materialIndex == std::numeric_limits<std::size_t>::max()) {
            return abi::Result::RasterInvalidPacket;
        }
        const auto dynamicOffset64 = layout.materialOffset +
            materialIndex * layout.materialStride;
        if (dynamicOffset64 > std::numeric_limits<std::uint32_t>::max()) {
            return abi::Result::RasterInvalidPacket;
        }
        const auto dynamicOffset =
            static_cast<std::uint32_t>(dynamicOffset64);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
            scenePipelineLayout, raster::kMaterialDescriptorSet, 1,
            &materialSet, 1, &dynamicOffset);
        // A two-sided surface must not cull, and a mirrored instance
        // reverses winding, so both come from the captured record rather
        // than from a pipeline permutation.
        const auto record = scene::ResolveVisibility(scenePacket, index);
        vkCmdSetCullMode(command,
            record.faceMode == visibility::FaceMode::TwoSided
                ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
        const auto effectiveFace = visibility::EffectiveFrontFace(
            draw.frontFace, record.modelDeterminant);
        vkCmdSetFrontFace(command,
            effectiveFace == raster::FrontFace::CounterClockwise
                ? VK_FRONT_FACE_CLOCKWISE
                : VK_FRONT_FACE_COUNTER_CLOCKWISE);
        // The prepass establishes depth; the colour pass may only keep what
        // the prepass already wrote.
        vkCmdSetDepthCompareOp(command, depthOnly
            ? ToVkCompare(draw.depthCompare) : VK_COMPARE_OP_EQUAL);
        const scene::ScenePushConstantsV1 push{
            static_cast<std::uint32_t>(index), firstInstance};
        vkCmdPushConstants(command, scenePipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(push), &push);
        vkCmdDrawIndexed(command, draw.indexCount,
            std::max<std::uint32_t>(range.count, 1),
            draw.firstIndex, draw.vertexOffset, 0);
        firstInstance += range.count;
    }
    return abi::Result::Success;
}

abi::Result VulkanRasterRenderer::Impl::RecordAndSubmit(
    const raster::DecodedPacket& packet,
    const UploadLayout& layout) noexcept
{
    auto result = WaitForSubmission();
    if (result != abi::Result::Success) {
        return result;
    }
    // Built here, after the upload buffer exists and holds this frame's
    // geometry. Building earlier would take the device address of a buffer
    // that has not been created yet, let alone filled.
    {
        const auto built = BuildAccelerationStructures(packet, layout);
        if (built != abi::Result::Success) return built;
    }
    if (vkResetFences(device, 1, &completion) != VK_SUCCESS ||
        vkResetCommandBuffer(command, 0) != VK_SUCCESS) {
        return abi::Result::RasterRenderFailed;
    }
    VkCommandBufferBeginInfo beginInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &beginInfo) != VK_SUCCESS) {
        return abi::Result::RasterRenderFailed;
    }

    if (accelBuildPending) {
        // The bottom level first, then an explicit dependency, then the top
        // level that reads it. Precise stages rather than an all-commands
        // barrier, which the plan calls out as bring-up debt to avoid.
        const VkAccelerationStructureBuildRangeInfoKHR* blasRanges =
            pendingBlasRanges.data();
        cmdBuildAccelerationStructures(command, 1, &pendingBlasBuild,
            &blasRanges);
        VkMemoryBarrier2 blasBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        blasBarrier.srcStageMask =
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        blasBarrier.srcAccessMask =
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        blasBarrier.dstStageMask =
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        // Read and write: the two builds share one scratch buffer, so the
        // top level's scratch writes are a write-after-write hazard against
        // the bottom level's, not only a read-after-write one.
        blasBarrier.dstAccessMask =
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        VkDependencyInfo blasDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        blasDependency.memoryBarrierCount = 1;
        blasDependency.pMemoryBarriers = &blasBarrier;
        vkCmdPipelineBarrier2(command, &blasDependency);
        const VkAccelerationStructureBuildRangeInfoKHR* tlasRanges =
            &pendingTlasRange;
        cmdBuildAccelerationStructures(command, 1, &pendingTlasBuild,
            &tlasRanges);
        // The fragment stage traces the finished top level.
        VkMemoryBarrier2 traceBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        traceBarrier.srcStageMask =
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        traceBarrier.srcAccessMask =
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        traceBarrier.dstStageMask =
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        traceBarrier.dstAccessMask =
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        VkDependencyInfo traceDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        traceDependency.memoryBarrierCount = 1;
        traceDependency.pMemoryBarriers = &traceBarrier;
        vkCmdPipelineBarrier2(command, &traceDependency);
        accelBuildPending = false;
    }

    const auto recordDeformation = [this]() {
        // Skin, morph, and wind run once per frame before rasterization,
        // and the deformed stream becomes the vertex input for this frame.
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
            deformPipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
            deformPipelineLayout, 0, 1, &deformSet, 0, nullptr);
        deform::DeformPushConstantsV1 push{};
        push.vertexCount = static_cast<std::uint32_t>(
            deformPacket.vertices.size());
        push.morphTargetCount = static_cast<std::uint32_t>(
            deformPacket.morphTargets.size());
        push.amplitude = deformPacket.header.wind.amplitude;
        push.frequency = deformPacket.header.wind.frequency;
        push.time = deformPacket.header.wind.time;
        push.previousTime = deformPacket.header.wind.previousTime;
        std::copy(std::begin(deformPacket.header.wind.direction),
            std::end(deformPacket.header.wind.direction),
            std::begin(push.direction));
        vkCmdPushConstants(command, deformPipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        const auto groups = (push.vertexCount +
            deform::kDeformWorkgroupSize - 1) /
            deform::kDeformWorkgroupSize;
        vkCmdDispatch(command, groups, 1, 1);
        std::array<VkBufferMemoryBarrier2, 2> deformBarriers{};
        for (auto& barrier : deformBarriers) {
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = deformOutput.buffer;
        }
        deformBarriers[0].dstStageMask =
            VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
            VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        deformBarriers[0].dstAccessMask =
            VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
            VK_ACCESS_2_TRANSFER_READ_BIT;
        deformBarriers[0].offset = deformVertexRange.offset;
        deformBarriers[0].size = deformVertexRange.size;
        deformBarriers[1].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        deformBarriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        deformBarriers[1].offset = deformPreviousRange.offset;
        deformBarriers[1].size = deformPreviousRange.size;
        VkDependencyInfo deformDependency{
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        deformDependency.bufferMemoryBarrierCount =
            static_cast<std::uint32_t>(deformBarriers.size());
        deformDependency.pBufferMemoryBarriers = deformBarriers.data();
        vkCmdPipelineBarrier2(command, &deformDependency);
    };

    const auto makeImageBarrier = [](
        const Image& image,
        const VkPipelineStageFlags2 sourceStage,
        const VkAccessFlags2 sourceAccess,
        const VkImageLayout oldLayout,
        const VkPipelineStageFlags2 destinationStage,
        const VkAccessFlags2 destinationAccess,
        const VkImageLayout newLayout,
        const VkImageAspectFlags aspect) {
        VkImageMemoryBarrier2 barrier{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = sourceStage;
        barrier.srcAccessMask = sourceAccess;
        barrier.dstStageMask = destinationStage;
        barrier.dstAccessMask = destinationAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.image;
        barrier.subresourceRange.aspectMask = aspect;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        return barrier;
    };
    for (std::size_t slot = 0;
         slot < sampledResources.size(); ++slot) {
        const auto& resource = sampledResources[slot];
        if (!resource.uploadPending) continue;
        if (layout.textureSubresourceOffsets[slot].size() !=
            resource.source.subresources.size()) {
            static_cast<void>(vkEndCommandBuffer(command));
            return abi::Result::RasterInvalidPacket;
        }
        VkImageMemoryBarrier2 toTransfer{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        toTransfer.srcAccessMask = VK_ACCESS_2_NONE;
        toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = resource.image.image;
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.baseMipLevel =
            resource.source.residentBaseMip;
        toTransfer.subresourceRange.levelCount =
            resource.source.residentMipCount;
        toTransfer.subresourceRange.layerCount =
            resource.source.arrayLayers;
        VkDependencyInfo transferDependency{
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        transferDependency.imageMemoryBarrierCount = 1;
        transferDependency.pImageMemoryBarriers = &toTransfer;
        vkCmdPipelineBarrier2(command, &transferDependency);
        for (std::size_t index = 0;
             index < resource.source.subresources.size(); ++index) {
            const auto& subresource = resource.source.subresources[index];
            VkBufferImageCopy copy{};
            copy.bufferOffset =
                layout.textureSubresourceOffsets[slot][index];
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.mipLevel = subresource.mipLevel;
            copy.imageSubresource.baseArrayLayer = subresource.arrayLayer;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {
                subresource.width, subresource.height, 1};
            vkCmdCopyBufferToImage(command, upload.buffer,
                resource.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &copy);
        }
        VkImageMemoryBarrier2 toSample = toTransfer;
        toSample.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toSample.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toSample.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toSample.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toSample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        transferDependency.pImageMemoryBarriers = &toSample;
        vkCmdPipelineBarrier2(command, &transferDependency);
    }
    // The refraction source is sampled by every scene pipeline, because the
    // shader references the binding whether or not a given draw is
    // refractive. An image still UNDEFINED at submit is a layout error even
    // when nothing reads it, so it is made readable once and stays that way
    // between the copies that refill it.
    if (refraction.image != VK_NULL_HANDLE &&
        refraction.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        const auto initial = makeImageBarrier(refraction,
            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
            refraction.layout,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
        VkDependencyInfo initialDependency{
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        initialDependency.imageMemoryBarrierCount = 1;
        initialDependency.pImageMemoryBarriers = &initial;
        vkCmdPipelineBarrier2(command, &initialDependency);
        refraction.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    const auto previousHdrStage = extentInitialized
        ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
        : VK_PIPELINE_STAGE_2_NONE;
    const auto previousHdrAccess = extentInitialized
        ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE;
    const auto previousDepthStage = extentInitialized
        ? VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
        : VK_PIPELINE_STAGE_2_NONE;
    const auto previousDepthAccess = extentInitialized
        ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
        : VK_ACCESS_2_NONE;
    const auto previousOutputStage = extentInitialized
        ? VK_PIPELINE_STAGE_2_TRANSFER_BIT
        : VK_PIPELINE_STAGE_2_NONE;
    const auto previousOutputAccess = extentInitialized
        ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_NONE;
    std::vector<VkImageMemoryBarrier2> attachmentBarriers;
    try {
        attachmentBarriers.reserve(3 + gbuffer.size());
    } catch (...) {
        static_cast<void>(vkEndCommandBuffer(command));
        return abi::Result::InternalFailure;
    }
    attachmentBarriers.push_back(makeImageBarrier(hdr,
        previousHdrStage, previousHdrAccess, hdr.layout,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT));
    attachmentBarriers.push_back(makeImageBarrier(depth,
        previousDepthStage, previousDepthAccess, depth.layout,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_DEPTH_BIT));
    attachmentBarriers.push_back(makeImageBarrier(output,
        previousOutputStage, previousOutputAccess, output.layout,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT));
    if (phase11SceneActive) {
        const auto previousGBufferStage = extentInitialized
            ? VK_PIPELINE_STAGE_2_TRANSFER_BIT
            : VK_PIPELINE_STAGE_2_NONE;
        const auto previousGBufferAccess = extentInitialized
            ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_NONE;
        for (const auto& plane : gbuffer) {
            attachmentBarriers.push_back(makeImageBarrier(plane,
                previousGBufferStage, previousGBufferAccess, plane.layout,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT));
        }
    }
    VkDependencyInfo attachmentDependency{
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    attachmentDependency.imageMemoryBarrierCount =
        static_cast<std::uint32_t>(attachmentBarriers.size());
    attachmentDependency.pImageMemoryBarriers = attachmentBarriers.data();
    vkCmdPipelineBarrier2(command, &attachmentDependency);

    const auto beginRegion = [this](const char* name,
                                    const std::array<float, 4>& color) {
        if (beginLabel == nullptr) {
            return;
        }
        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = name;
        std::copy(color.begin(), color.end(), label.color);
        beginLabel(command, &label);
    };
    const auto endRegion = [this]() {
        if (endLabel != nullptr) {
            endLabel(command);
        }
    };

    if (phase13DeformActive) {
        beginRegion("phase13.deform", {0.2f, 0.9f, 0.75f, 1.0f});
        recordDeformation();
        endRegion();
    }

    // Temporal accumulation. Ahead of rasterization because it consumes the
    // previous frame's traced samples rather than this frame's, and putting it
    // after would make the pass depend on work it does not read.
    if (indirectPixelCount != 0) {
        beginRegion("phase20.indirect-accumulate", {0.9f, 0.7f, 0.2f, 1.0f});
        RecordIndirect();
        endRegion();
    }

    beginRegion(phase14TerrainActive
        ? "phase14.terrain-mirror"
        : phase11SceneActive
        ? "phase11.opaque-scene-mirror"
        : phase10ViewActive
            ? "phase10.captured-view-raster"
            : phase9MaterialActive
                ? "phase09.authored-material-raster"
                : "phase06.opaque-raster",
        phase14TerrainActive
            ? std::array<float, 4>{0.45f, 0.7f, 0.2f, 1.0f}
            : phase11SceneActive
            ? std::array<float, 4>{0.6f, 0.25f, 0.85f, 1.0f}
            : phase10ViewActive
            ? std::array<float, 4>{0.35f, 0.8f, 0.3f, 1.0f}
            : phase9MaterialActive
            ? std::array<float, 4>{0.85f, 0.35f, 0.12f, 1.0f}
            : std::array<float, 4>{0.15f, 0.45f, 0.95f, 1.0f});
    VkRenderingAttachmentInfo colorAttachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAttachment.imageView = hdr.view;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = {{
        kWorldClearColor[0], kWorldClearColor[1],
        kWorldClearColor[2], kWorldClearColor[3]}};
    VkRenderingAttachmentInfo depthAttachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttachment.imageView = depth.view;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue.depthStencil = {1.0f, 0};

    // Alpha-tested objects run a depth prepass so the colour pass can test
    // EQUAL: if the two passes ever disagreed on a cutout silhouette the
    // colour fragment would fail the test and erase itself, which makes a
    // disagreement visible in the G-buffer instead of silently differing.
    std::vector<std::size_t> alphaObjects;
    if (phase11SceneActive) {
        try {
            for (std::size_t index = 0; index < scenePacket.objects.size();
                 ++index) {
                const auto record = scene::ResolveVisibility(
                    scenePacket, index);
                if (record.alpha.classification ==
                    visibility::AlphaClass::Tested) {
                    alphaObjects.push_back(index);
                }
            }
        } catch (...) {
            static_cast<void>(vkEndCommandBuffer(command));
            return abi::Result::InternalFailure;
        }
    }
    const auto runPrepass = !alphaObjects.empty();
    // The mirrored planes are cleared to the same world state the CPU
    // oracle starts from, so uncovered pixels compare exactly.
    std::array<VkRenderingAttachmentInfo,
        1 + scene::kSceneGBufferPlaneCount> sceneAttachments{};
    sceneAttachments[0] = colorAttachment;
    for (std::size_t plane = 0; plane < gbuffer.size(); ++plane) {
        auto& attachment = sceneAttachments[plane + 1];
        attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachment.imageView = gbuffer[plane].view;
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    sceneAttachments[1].clearValue.color = {{
        kWorldClearColor[0], kWorldClearColor[1],
        kWorldClearColor[2], kWorldClearColor[3]}};
    sceneAttachments[2].clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    sceneAttachments[3].clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    sceneAttachments[4].clearValue.color.uint32[0] = 0;
    // Nothing transparent has been composited yet, so no pixel is reactive.
    sceneAttachments[5].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {extent.Width(), extent.Height()};
    rendering.layerCount = 1;
    // Terrain writes the same mirrored planes as the scene pass, so either
    // one arms the multi-target path.
    const auto mirrorActive = phase11SceneActive || phase14TerrainActive;
    const VkViewport prepassViewport{
        packet.header.viewportX,
        packet.header.viewportY,
        packet.header.viewportWidth,
        packet.header.viewportHeight,
        packet.header.viewportMinDepth,
        packet.header.viewportMaxDepth,
    };
    const VkRect2D prepassScissor{
        {packet.header.scissorX, packet.header.scissorY},
        {packet.header.scissorWidth, packet.header.scissorHeight},
    };
    // Vertex and index bindings are command-buffer state rather than render
    // pass state, so they are bound once here and serve the depth prepass as
    // well as the passes that follow.
    // Deformed frames rasterize the compute output instead of the packet's
    // bind-pose stream.
    const VkBuffer boundVertexBuffer = phase13DeformActive
        ? deformOutput.buffer : upload.buffer;
    const VkDeviceSize boundVertexOffset = phase13DeformActive
        ? deformVertexRange.offset : layout.vertexOffset;
    vkCmdBindVertexBuffers(command, 0, 1, &boundVertexBuffer,
        &boundVertexOffset);
    vkCmdBindIndexBuffer(command, upload.buffer, layout.indexOffset,
        packet.header.indexType == raster::IndexType::Uint16
            ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    if (runPrepass) {
        beginRegion("phase15.alpha-depth-prepass", {0.9f, 0.7f, 0.2f, 1.0f});
        VkRenderingInfo prepass{VK_STRUCTURE_TYPE_RENDERING_INFO};
        prepass.renderArea.extent = {extent.Width(), extent.Height()};
        prepass.layerCount = 1;
        prepass.colorAttachmentCount = 0;
        prepass.pDepthAttachment = &depthAttachment;
        vkCmdBeginRendering(command, &prepass);
        vkCmdSetViewport(command, 0, 1, &prepassViewport);
        vkCmdSetScissor(command, 0, 1, &prepassScissor);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
            alphaDepthPipeline);
        const auto prepassResult = RecordAlphaDraws(packet, layout,
            alphaObjects, true);
        vkCmdEndRendering(command);
        endRegion();
        if (prepassResult != abi::Result::Success) {
            static_cast<void>(vkEndCommandBuffer(command));
            return prepassResult;
        }
        // Depth now holds the cutout silhouette, so the main pass must keep
        // it rather than clearing it away.
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    }
    rendering.colorAttachmentCount = mirrorActive
        ? static_cast<std::uint32_t>(sceneAttachments.size()) : 1;
    rendering.pColorAttachments = mirrorActive
        ? sceneAttachments.data() : &colorAttachment;
    rendering.pDepthAttachment = &depthAttachment;
    vkCmdBeginRendering(command, &rendering);
    const VkViewport viewport{
        packet.header.viewportX,
        packet.header.viewportY,
        packet.header.viewportWidth,
        packet.header.viewportHeight,
        packet.header.viewportMinDepth,
        packet.header.viewportMaxDepth,
    };
    const VkRect2D scissor{
        {packet.header.scissorX, packet.header.scissorY},
        {packet.header.scissorWidth, packet.header.scissorHeight},
    };
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    const raster::MaterialRegistry registry{packet.materials};
    // Phase 11 expands one shared draw list into per-object work. Opaque
    // objects are submitted in packet order; depth alone resolves them.
    const auto drawCount = phase11SceneActive
        ? scenePacket.objects.size() : packet.draws.size();
    std::uint32_t firstInstance = 0;
    for (std::size_t index = 0; index < drawCount; ++index) {
        const auto drawIndex = phase11SceneActive
            ? static_cast<std::size_t>(scenePacket.objects[index].drawIndex)
            : index;
        const auto instanceRange = phase11SceneActive
            ? scene::ObjectInstanceRange(scenePacket, index)
            : scene::InstanceRange{0, 1};
        if (drawIndex >= packet.draws.size()) {
            vkCmdEndRendering(command);
            static_cast<void>(vkEndCommandBuffer(command));
            return abi::Result::RasterInvalidPacket;
        }
        // Alpha-tested objects are drawn by the alpha colour pass after this
        // loop, against the depth the prepass already established.
        if (phase11SceneActive && !alphaObjects.empty() &&
            std::find(alphaObjects.begin(), alphaObjects.end(), index) !=
                alphaObjects.end()) {
            firstInstance += instanceRange.count;
            continue;
        }
        // An object the transparent table claims is drawn by the blended pass
        // and by nothing else. Drawing it opaque here as well would write its
        // depth and its G-buffer, so the blended draw that follows would be
        // compositing over itself -- and the blend would cancel out to
        // nothing, which is indistinguishable from a pass that never ran.
        if (phase11SceneActive && !scenePacket.transparent.empty()) {
            const auto claimed = std::any_of(
                scenePacket.transparent.begin(),
                scenePacket.transparent.end(),
                [index](const scene::TransparentDrawRecordV1& record) {
                    return record.objectIndex == index;
                });
            if (claimed) {
                firstInstance += instanceRange.count;
                continue;
            }
        }
        const auto& draw = packet.draws[drawIndex];
        const auto materialIndex = registry.IndexOf(draw.materialId);
        if (materialIndex == std::numeric_limits<std::size_t>::max()) {
            vkCmdEndRendering(command);
            static_cast<void>(vkEndCommandBuffer(command));
            return abi::Result::RasterInvalidPacket;
        }
        const auto dynamicOffset64 = layout.materialOffset +
            materialIndex * layout.materialStride;
        if (dynamicOffset64 > std::numeric_limits<std::uint32_t>::max()) {
            vkCmdEndRendering(command);
            static_cast<void>(vkEndCommandBuffer(command));
            return abi::Result::RasterInvalidPacket;
        }
        const auto dynamicOffset =
            static_cast<std::uint32_t>(dynamicOffset64);
        const auto pipelineIndex = PipelineIndex(
            draw.frontFace, draw.depthCompare);
        // A frame carrying material families draws its opaque geometry
        // through the family pipeline. Its per-draw state is dynamic, so the
        // static face/compare permutation is set by command instead.
        const auto useFamilyPipeline =
            phase11SceneActive && phase16FamilyActive;
        if (useFamilyPipeline) {
            const auto record = scene::ResolveVisibility(scenePacket, index);
            vkCmdSetCullMode(command,
                record.faceMode == visibility::FaceMode::TwoSided
                    ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
            const auto effectiveFace = visibility::EffectiveFrontFace(
                draw.frontFace, record.modelDeterminant);
            vkCmdSetFrontFace(command,
                effectiveFace == raster::FrontFace::CounterClockwise
                    ? VK_FRONT_FACE_CLOCKWISE
                    : VK_FRONT_FACE_COUNTER_CLOCKWISE);
            vkCmdSetDepthCompareOp(command,
                ToVkCompare(draw.depthCompare));
        }
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
            useFamilyPipeline
                ? familyScenePipeline
                : phase11SceneActive
                    ? scenePipelines[pipelineIndex]
                    : phase9MaterialActive
                        ? materialPipelines[pipelineIndex]
                        : meshPipelines[pipelineIndex]);
        vkCmdBindDescriptorSets(command,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            phase11SceneActive ? scenePipelineLayout : meshPipelineLayout,
            raster::kMaterialDescriptorSet, 1, &materialSet,
            1, &dynamicOffset);
        if (phase11SceneActive) {
            const scene::ScenePushConstantsV1 push{
                static_cast<std::uint32_t>(index), firstInstance};
            vkCmdPushConstants(command, scenePipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(push), &push);
        }
        // Repeated meshes become instances of one draw rather than
        // duplicated draw calls.
        vkCmdDrawIndexed(command, draw.indexCount,
            std::max<std::uint32_t>(instanceRange.count, 1),
            draw.firstIndex, draw.vertexOffset, 0);
        firstInstance += instanceRange.count;
    }

    // Blended draws, after every opaque one, sorted back to front. Sorting is
    // not a preference here: alpha blending is order dependent, so a frame
    // that draws them in packet order composites a near particle underneath a
    // far one and the result changes with the capture order rather than with
    // the scene.
    if (phase11SceneActive && !scenePacket.transparent.empty() &&
        blendedScenePipelines[0] != VK_NULL_HANDLE) {
        // The colour target as the opaque pass left it, copied before any
        // blended draw. A refractive surface samples this rather than the
        // live target, so what shows through it does not depend on which
        // refractive draws happened to precede it -- two panes of glass would
        // otherwise show each other, and which one won would change with the
        // sort. A copy cannot be recorded inside a render pass instance, so
        // the pass ends here and resumes with load rather than clear; a clear
        // would discard everything the opaque pass just drew.
        vkCmdEndRendering(command);
        const auto toTransferSource = makeImageBarrier(hdr,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
        const auto toTransferDestination = makeImageBarrier(refraction,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, refraction.layout,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
        const std::array snapshotBarriers{
            toTransferSource, toTransferDestination};
        VkDependencyInfo snapshotDependency{
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        snapshotDependency.imageMemoryBarrierCount =
            static_cast<std::uint32_t>(snapshotBarriers.size());
        snapshotDependency.pImageMemoryBarriers = snapshotBarriers.data();
        vkCmdPipelineBarrier2(command, &snapshotDependency);

        VkImageCopy region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.extent = {extent.Width(), extent.Height(), 1};
        vkCmdCopyImage(command, hdr.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, refraction.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        const auto backToAttachment = makeImageBarrier(hdr,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
        const auto toSampled = makeImageBarrier(refraction,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
        const std::array resumeBarriers{backToAttachment, toSampled};
        VkDependencyInfo resumeDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        resumeDependency.imageMemoryBarrierCount =
            static_cast<std::uint32_t>(resumeBarriers.size());
        resumeDependency.pImageMemoryBarriers = resumeBarriers.data();
        vkCmdPipelineBarrier2(command, &resumeDependency);
        refraction.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        auto resumed = rendering;
        auto resumedColor = sceneAttachments;
        for (auto& attachment : resumedColor) {
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        }
        auto resumedDepth = depthAttachment;
        resumedDepth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        resumed.pColorAttachments = resumedColor.data();
        resumed.pDepthAttachment = &resumedDepth;
        vkCmdBeginRendering(command, &resumed);

        beginRegion("phase21.transparent", {0.35f, 0.75f, 0.95f, 1.0f});
        std::vector<blend::TransparentDrawV1> blended;
        std::vector<std::uint32_t> blendedObjects;
        blended.reserve(scenePacket.transparent.size());
        blendedObjects.reserve(scenePacket.transparent.size());
        std::vector<const scene::TransparentDrawRecordV1*> blendedProjections;
        blendedProjections.reserve(scenePacket.transparent.size());
        for (const auto& record : scenePacket.transparent) {
            blend::TransparentDrawV1 entry{};
            entry.drawId = record.drawId;
            entry.materialId = record.materialId;
            entry.blend = static_cast<blend::BlendMode>(record.blend);
            entry.domain = static_cast<blend::EffectDomain>(record.domain);
            entry.sortDepth = record.sortDepth;
            entry.softFade = record.softFade;
            entry.dissolve = record.dissolve;
            entry.dissolveFalloff = record.dissolveFalloff;
            // The receiver mask the record declared. Dropped here before, which
            // left every decal projecting onto every surface it reached.
            // Narrowed deliberately: a stencil value is eight bits wide, and the
            // packet field is thirty-two only because that is what the record
            // aligns to. Values that do not fit are refused when the packet is
            // validated, so the cast here cannot silently drop one.
            entry.stencilReceiverMask =
                static_cast<std::uint8_t>(record.stencilReceiverMask);
            entry.stencilReference =
                static_cast<std::uint8_t>(record.stencilReference);
            // A draw the contract refuses is skipped rather than drawn under
            // a guessed rule: an unclassified effect keeps its vanilla path.
            if (blend::ValidateDraw(entry) != blend::TransparencyError::None) {
                continue;
            }
            blended.push_back(entry);
            blendedObjects.push_back(record.objectIndex);
            // Kept alongside, because the projection volume lives on the packet
            // record rather than on the contract type: the host contract takes
            // it as a separate argument, so the draw does not carry it.
            blendedProjections.push_back(&record);
        }
        // The contract's own order, so the backend and the oracle cannot sort
        // differently and disagree about which layer is on top.
        std::vector<std::size_t> order(blended.size());
        for (std::size_t slot = 0; slot < order.size(); ++slot) {
            order[slot] = slot;
        }
        std::stable_sort(order.begin(), order.end(),
            [&blended](const std::size_t left, const std::size_t right) {
                return blend::SortsBefore(
                    blend::MakeSortKey(blended[left]),
                    blend::MakeSortKey(blended[right]));
            });

        for (const auto slot : order) {
            const auto mode = static_cast<std::size_t>(blended[slot].blend);
            if (mode == 0 || mode > kBlendedPipelineCount) continue;
            const auto objectIndex = blendedObjects[slot];
            if (objectIndex >= packet.draws.size()) continue;
            const auto& blendedDraw = packet.draws[objectIndex];
            const auto instanceRange =
                scene::ObjectInstanceRange(scenePacket, objectIndex);
            const auto materialIndex =
                registry.IndexOf(blendedDraw.materialId);
            if (materialIndex >= packet.materials.size()) continue;
            const auto blendedOffset64 = layout.materialOffset +
                materialIndex * layout.materialStride;
            if (blendedOffset64 >
                std::numeric_limits<std::uint32_t>::max()) {
                continue;
            }
            const auto blendedOffset =
                static_cast<std::uint32_t>(blendedOffset64);
            const auto record =
                scene::ResolveVisibility(scenePacket, objectIndex);
            vkCmdSetCullMode(command,
                record.faceMode == visibility::FaceMode::TwoSided
                    ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
            const auto effectiveFace = visibility::EffectiveFrontFace(
                blendedDraw.frontFace, record.modelDeterminant);
            vkCmdSetFrontFace(command,
                effectiveFace == raster::FrontFace::CounterClockwise
                    ? VK_FRONT_FACE_CLOCKWISE
                    : VK_FRONT_FACE_COUNTER_CLOCKWISE);
            // Equal depth still passes. A decal or scorch mark is coplanar
            // with the surface it sits on, and a strict less-than rejects
            // every one of its fragments -- the pass then runs, reports no
            // error, and draws nothing. Blended draws never write depth, so
            // admitting the tie cannot cause z-fighting.
            vkCmdSetDepthCompareOp(command,
                blendedDraw.depthCompare == raster::DepthCompare::Less
                    ? VK_COMPARE_OP_LESS_OR_EQUAL
                    : ToVkCompare(blendedDraw.depthCompare));
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                blendedScenePipelines[mode - 1]);
            vkCmdBindDescriptorSets(command,
                VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipelineLayout,
                raster::kMaterialDescriptorSet, 1, &materialSet,
                1, &blendedOffset);
            scene::ScenePushConstantsV1 push{};
            push.objectIndex = objectIndex;
            push.firstInstance = instanceRange.first;
            // The blend this draw is composited with, so the fragment shader
            // can weigh how much of the pixel it decides. An additive spark
            // owns its pixel whatever its alpha says, and the reactive mask
            // has to say so or an upscaler reconstructs the spark from
            // history that never contained it.
            push.blend = static_cast<std::uint32_t>(mode);
            // The volume this draw projects into, straight from the record
            // that declared it. A range of zero means the draw projects
            // nothing, which is every transparent draw that is not a decal.
            const auto& projection = *blendedProjections[slot];
            push.decalRange = projection.decalRange;
            push.decalRadius = projection.decalRadius;
            push.decalReceiverMask = projection.stencilReceiverMask;
            push.decalReference = projection.stencilReference;
            std::copy(std::begin(projection.decalOrigin),
                std::end(projection.decalOrigin),
                std::begin(push.decalOrigin));
            std::copy(std::begin(projection.decalAxis),
                std::end(projection.decalAxis),
                std::begin(push.decalAxis));
            // Refractive draws read the snapshot; every other blended mode
            // composites straight over what is already there.
            if (blended[slot].domain == blend::EffectDomain::Refractive) {
                push.refractive = 1;
                push.indexOfRefraction = water::kWaterIor;
            }
            vkCmdPushConstants(command, scenePipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(push), &push);
            vkCmdDrawIndexed(command, blendedDraw.indexCount,
                std::max<std::uint32_t>(instanceRange.count, 1),
                blendedDraw.firstIndex, blendedDraw.vertexOffset, 0u);
        }
        endRegion();
    }

    if (runPrepass) {
        // Same geometry and state as the prepass, differing only in the
        // bound pipeline, which tests EQUAL against the depth it wrote.
        beginRegion("phase15.alpha-tested", {0.95f, 0.55f, 0.15f, 1.0f});
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
            alphaScenePipeline);
        const auto alphaResult = RecordAlphaDraws(packet, layout,
            alphaObjects, false);
        endRegion();
        if (alphaResult != abi::Result::Success) {
            vkCmdEndRendering(command);
            static_cast<void>(vkEndCommandBuffer(command));
            return alphaResult;
        }
    }
    if (phase14TerrainActive) {
        // Terrain shares the render pass and depth buffer with the scene
        // pass. Cells are submitted in captured order; the engine already
        // made the LOD decision, so nothing here re-culls them.
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
            terrainPipeline);
        const std::uint32_t terrainDynamicOffset =
            static_cast<std::uint32_t>(layout.materialOffset);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
            terrainPipelineLayout, raster::kMaterialDescriptorSet, 1,
            &materialSet, 1, &terrainDynamicOffset);
        vkCmdBindVertexBuffers(command, 0, 1, &upload.buffer,
            &layout.phase14VertexOffset);
        vkCmdBindIndexBuffer(command, upload.buffer,
            layout.phase14IndexOffset, VK_INDEX_TYPE_UINT32);
        for (std::size_t index = 0; index < terrainPacket.cells.size();
             ++index) {
            const auto& cell = terrainPacket.cells[index];
            const terrain::TerrainPushConstantsV1 push{
                static_cast<std::uint32_t>(index), 0};
            vkCmdPushConstants(command, terrainPipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(push), &push);
            vkCmdDrawIndexed(command, cell.indexCount, 1,
                cell.firstIndex, 0, 0);
        }
    }
    vkCmdEndRendering(command);
    endRegion();

    if (phase13DeformActive) {
        const auto vertexBytes = deformPacket.vertices.size() *
            sizeof(raster::RasterVertexV3);
        const auto previousBytes = deformPacket.vertices.size() *
            deform::kGpuPreviousPositionSize;
        const std::array<VkBufferCopy, 2> copies{
            VkBufferCopy{deformVertexRange.offset, 0, vertexBytes},
            VkBufferCopy{deformPreviousRange.offset, vertexBytes,
                previousBytes},
        };
        vkCmdCopyBuffer(command, deformOutput.buffer, deformReadback.buffer,
            static_cast<std::uint32_t>(copies.size()), copies.data());
        VkBufferMemoryBarrier2 deformHostBarrier{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        deformHostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        deformHostBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        deformHostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        deformHostBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        deformHostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        deformHostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        deformHostBarrier.buffer = deformReadback.buffer;
        deformHostBarrier.offset = 0;
        deformHostBarrier.size = VK_WHOLE_SIZE;
        VkDependencyInfo deformHostDependency{
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        deformHostDependency.bufferMemoryBarrierCount = 1;
        deformHostDependency.pBufferMemoryBarriers = &deformHostBarrier;
        vkCmdPipelineBarrier2(command, &deformHostDependency);
    }

    if (mirrorActive) {
        std::array<VkImageMemoryBarrier2,
            scene::kSceneGBufferPlaneCount> gbufferBarriers{};
        for (std::size_t plane = 0; plane < gbuffer.size(); ++plane) {
            gbufferBarriers[plane] = makeImageBarrier(gbuffer[plane],
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT);
        }
        VkDependencyInfo gbufferDependency{
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        gbufferDependency.imageMemoryBarrierCount =
            static_cast<std::uint32_t>(gbufferBarriers.size());
        gbufferDependency.pImageMemoryBarriers = gbufferBarriers.data();
        vkCmdPipelineBarrier2(command, &gbufferDependency);
        const auto planePixels = static_cast<VkDeviceSize>(
            extent.Width()) * extent.Height();
        for (std::size_t plane = 0; plane < gbuffer.size(); ++plane) {
            VkBufferImageCopy planeCopy{};
            planeCopy.bufferOffset = planePixels *
                scene::kSceneGBufferPlaneSize * plane;
            planeCopy.imageSubresource.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            planeCopy.imageSubresource.layerCount = 1;
            planeCopy.imageExtent = {
                extent.Width(), extent.Height(), 1};
            vkCmdCopyImageToBuffer(command, gbuffer[plane].image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                gbufferReadback.buffer, 1, &planeCopy);
        }
        VkBufferMemoryBarrier2 gbufferHostBarrier{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        gbufferHostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        gbufferHostBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        gbufferHostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        gbufferHostBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        gbufferHostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        gbufferHostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        gbufferHostBarrier.buffer = gbufferReadback.buffer;
        gbufferHostBarrier.offset = 0;
        gbufferHostBarrier.size = VK_WHOLE_SIZE;
        VkDependencyInfo gbufferHostDependency{
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        gbufferHostDependency.bufferMemoryBarrierCount = 1;
        gbufferHostDependency.pBufferMemoryBarriers = &gbufferHostBarrier;
        vkCmdPipelineBarrier2(command, &gbufferHostDependency);
    }

    // The float colour target is copied out before the tone pass consumes
    // it, so emission is compared at its authored magnitude rather than
    // after an 8-bit tone-mapped clamp.
    if (hdrReadbackRequested) {
        const auto hdrToTransfer = makeImageBarrier(hdr,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
        VkDependencyInfo hdrTransferDependency{
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        hdrTransferDependency.imageMemoryBarrierCount = 1;
        hdrTransferDependency.pImageMemoryBarriers = &hdrToTransfer;
        vkCmdPipelineBarrier2(command, &hdrTransferDependency);
        VkBufferImageCopy hdrCopy{};
        hdrCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        hdrCopy.imageSubresource.layerCount = 1;
        hdrCopy.imageExtent = {extent.Width(), extent.Height(), 1};
        vkCmdCopyImageToBuffer(command, hdr.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hdrReadback.buffer, 1,
            &hdrCopy);
        VkBufferMemoryBarrier2 hdrHostBarrier{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        hdrHostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        hdrHostBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        hdrHostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        hdrHostBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        hdrHostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hdrHostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hdrHostBarrier.buffer = hdrReadback.buffer;
        hdrHostBarrier.offset = 0;
        hdrHostBarrier.size = VK_WHOLE_SIZE;
        VkDependencyInfo hdrHostDependency{
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        hdrHostDependency.bufferMemoryBarrierCount = 1;
        hdrHostDependency.pBufferMemoryBarriers = &hdrHostBarrier;
        vkCmdPipelineBarrier2(command, &hdrHostDependency);
    }

    // The source layout depends on whether the readback copy already moved
    // the image out of the colour-attachment layout.
    const auto hdrToSample = makeImageBarrier(hdr,
        hdrReadbackRequested
            ? VK_PIPELINE_STAGE_2_TRANSFER_BIT
            : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        hdrReadbackRequested
            ? VK_ACCESS_2_TRANSFER_READ_BIT
            : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        hdrReadbackRequested
            ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
            : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT);
    VkDependencyInfo sampleDependency{
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    sampleDependency.imageMemoryBarrierCount = 1;
    sampleDependency.pImageMemoryBarriers = &hdrToSample;
    vkCmdPipelineBarrier2(command, &sampleDependency);

    beginRegion("phase06.tone-map", {0.95f, 0.55f, 0.1f, 1.0f});
    VkRenderingAttachmentInfo outputAttachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    outputAttachment.imageView = output.view;
    outputAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    outputAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    outputAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    outputAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &outputAttachment;
    rendering.pDepthAttachment = nullptr;
    vkCmdBeginRendering(command, &rendering);
    const VkViewport outputViewport{0.0f, 0.0f,
        static_cast<float>(extent.Width()),
        static_cast<float>(extent.Height()), 0.0f, 1.0f};
    const VkRect2D outputScissor{{0, 0},
        {extent.Width(), extent.Height()}};
    vkCmdSetViewport(command, 0, 1, &outputViewport);
    vkCmdSetScissor(command, 0, 1, &outputScissor);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
        tonePipeline);
    vkCmdBindDescriptorSets(command,
        VK_PIPELINE_BIND_POINT_GRAPHICS, tonePipelineLayout,
        0, 1, &toneSet, 0, nullptr);
    TonePushConstants tonePush{};
    // One, until the adaptation state of the post chain is wired: the
    // contract's AdaptExposure has no home on the device yet, and a value
    // invented here would be a second source of truth for it.
    tonePush.exposure = 1.0f;
    // The contract's own defaults, so the device and the oracle threshold at
    // the same place. An intensity of zero leaves the image bit-identical,
    // which is what lets a caller turn bloom off and compare.
    // The caller's rules when it declared any, the contract's defaults when
    // it did not. A threshold of zero is not a declaration -- it is what an
    // untouched request holds -- so the intensity is what says whether the
    // caller filled these in at all.
    post::BloomRules bloomRules{};
    if (bloomRequest.intensity != 0.0f) {
        bloomRules.threshold = bloomRequest.threshold;
        bloomRules.knee = bloomRequest.knee;
        bloomRules.intensity = bloomRequest.intensity;
    }
    const auto toneBloomEnabled =
        (this->frameFlags & abi::RasterFrameBloom) != 0;
    tonePush.bloomThreshold = bloomRules.threshold;
    tonePush.bloomKnee = bloomRules.knee;
    tonePush.bloomIntensity =
        toneBloomEnabled ? bloomRules.intensity : 0.0f;
    vkCmdPushConstants(command, tonePipelineLayout,
        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(tonePush), &tonePush);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRendering(command);
    endRegion();

    const auto outputToCopy = makeImageBarrier(output,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT);
    VkDependencyInfo copyDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    copyDependency.imageMemoryBarrierCount = 1;
    copyDependency.pImageMemoryBarriers = &outputToCopy;
    vkCmdPipelineBarrier2(command, &copyDependency);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {extent.Width(), extent.Height(), 1};
    vkCmdCopyImageToBuffer(command, output.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readback.buffer, 1, &copy);
    VkBufferMemoryBarrier2 hostBarrier{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    hostBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.buffer = readback.buffer;
    hostBarrier.offset = 0;
    hostBarrier.size = VK_WHOLE_SIZE;
    VkDependencyInfo hostDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    hostDependency.bufferMemoryBarrierCount = 1;
    hostDependency.pBufferMemoryBarriers = &hostBarrier;
    vkCmdPipelineBarrier2(command, &hostDependency);
    if (vkEndCommandBuffer(command) != VK_SUCCESS) {
        return abi::Result::RasterRenderFailed;
    }
    VkCommandBufferSubmitInfo commandSubmit{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    commandSubmit.commandBuffer = command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &commandSubmit;
    if (vkQueueSubmit2(queue, 1, &submit, completion) != VK_SUCCESS) {
        return abi::Result::RasterRenderFailed;
    }
    fenceSubmitted = true;
    result = WaitForSubmission();
    if (result != abi::Result::Success) {
        return result;
    }
    hdr.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depth.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    output.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (phase11SceneActive) {
        for (auto& plane : gbuffer) {
            plane.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
    }
    for (auto& resource : sampledResources) {
        if (resource.uploadPending) {
            resource.image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            resource.uploadPending = false;
        }
    }
    extentInitialized = true;
    ++submissions;
    return abi::Result::Success;
}

abi::Result VulkanRasterRenderer::Impl::CopyOutput(
    const abi::RasterFrameRequestV1& request,
    const raster::DecodedPacket& packet) noexcept
{
    if (!readback.coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = readback.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        if (vkInvalidateMappedMemoryRanges(device, 1, &range) != VK_SUCCESS) {
            return abi::Result::RasterRenderFailed;
        }
    }
    auto* destination = reinterpret_cast<std::byte*>(
        static_cast<std::uintptr_t>(request.outputData));
    const auto* source = static_cast<const std::byte*>(readback.mapped);
    const auto sourcePitch = static_cast<std::size_t>(packet.header.width) * 4;
    for (std::uint32_t row = 0; row < packet.header.height; ++row) {
        std::memcpy(
            destination + static_cast<std::size_t>(row) *
                request.outputRowPitch,
            source + static_cast<std::size_t>(row) * sourcePitch,
            sourcePitch);
    }
    return abi::Result::Success;
}

abi::Result VulkanRasterRenderer::Impl::CopySceneOutput(
    const abi::RasterFrameRequestV1& request) noexcept
{
    const auto pixelCount = static_cast<std::size_t>(extent.Width()) *
        extent.Height();
    const auto requiredBytes = pixelCount * scene::kGpuGBufferPixelSize;
    if ((!phase11SceneActive && !phase14TerrainActive) || pixelCount == 0 ||
        request.gbufferData == 0 ||
        request.gbufferCapacity < requiredBytes ||
        gbufferReadback.mapped == nullptr ||
        gbufferReadback.capacity < requiredBytes) {
        return abi::Result::InvalidArgument;
    }
    if (!gbufferReadback.coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = gbufferReadback.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        if (vkInvalidateMappedMemoryRanges(device, 1, &range) != VK_SUCCESS) {
            return abi::Result::RasterRenderFailed;
        }
    }
    // The planes are copied back separately and interleaved into the
    // reflected pixel record the CPU oracle compares against.
    auto* destination = reinterpret_cast<std::byte*>(
        static_cast<std::uintptr_t>(request.gbufferData));
    const auto* source =
        static_cast<const std::byte*>(gbufferReadback.mapped);
    for (std::size_t plane = 0; plane < gbuffer.size(); ++plane) {
        const auto* planeSource = source +
            pixelCount * scene::kSceneGBufferPlaneSize * plane;
        for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
            std::memcpy(
                destination + pixel * scene::kGpuGBufferPixelSize +
                    plane * scene::kSceneGBufferPlaneSize,
                planeSource + pixel * scene::kSceneGBufferPlaneSize,
                scene::kSceneGBufferPlaneSize);
        }
    }
    return abi::Result::Success;
}

abi::Result VulkanRasterRenderer::Impl::CopyHdrOutput(
    const abi::RasterFrameRequestV1& request) noexcept
{
    const auto pixelCount = static_cast<std::size_t>(extent.Width()) *
        extent.Height();
    // Four floats per pixel on the host side; the device stores four halves.
    const auto requiredBytes = pixelCount * 4 * sizeof(float);
    const auto sourceBytes = pixelCount * kHdrPixelBytes;
    if (pixelCount == 0 || request.hdrData == 0 ||
        request.hdrCapacity < requiredBytes ||
        hdrReadback.mapped == nullptr ||
        hdrReadback.capacity < sourceBytes) {
        return abi::Result::InvalidArgument;
    }
    if (!hdrReadback.coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = hdrReadback.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        if (vkInvalidateMappedMemoryRanges(device, 1, &range) != VK_SUCCESS) {
            return abi::Result::RasterRenderFailed;
        }
    }
    auto* destination = reinterpret_cast<float*>(
        static_cast<std::uintptr_t>(request.hdrData));
    const auto* source =
        static_cast<const std::uint16_t*>(hdrReadback.mapped);
    for (std::size_t lane = 0; lane < pixelCount * 4; ++lane) {
        destination[lane] = mesh::HalfToFloat(source[lane]);
    }
    return abi::Result::Success;
}

VulkanRasterRenderer::VulkanRasterRenderer()
    : impl_(std::make_unique<Impl>())
{}

VulkanRasterRenderer::~VulkanRasterRenderer()
{
    impl_->Reset();
}

abi::Result VulkanRasterRenderer::Create(
    const abi::HostCallbacksV1& callbacks,
    const abi::RasterCreateRequestV1& request,
    abi::RasterStatusV1& status) noexcept
{
    if (impl_->ready) {
        impl_->FillStatus(status, abi::Result::RasterAlreadyCreated,
            "raster session already created");
        return abi::Result::RasterAlreadyCreated;
    }
    if (request.structSize < abi::kRasterCreateRequestV1RequiredSize ||
        status.structSize < abi::kRasterStatusV1RequiredSize ||
        callbacks.structSize < abi::kHostCallbacksV1RequiredSize ||
        callbacks.log == nullptr ||
        (request.flags &
            ~(abi::RasterCreateValidation |
                abi::RasterCreateAnyAdapter)) != 0) {
        InitializeStatus(status, abi::Result::InvalidArgument,
            "invalid raster create contract");
        return abi::Result::InvalidArgument;
    }
    impl_->callbacks = callbacks;
    impl_->debug.callbacks = callbacks;
    impl_->debug.errors.store(0, std::memory_order_release);
    const auto graph = raster::BuildPhase6FrameGraph();
    if (raster::ValidateFrameGraph(graph) != raster::GraphError::None) {
        impl_->FillStatus(status, abi::Result::InternalFailure,
            "phase6 frame graph invalid");
        return abi::Result::InternalFailure;
    }
    auto result = impl_->CreateInstance(
        (request.flags & abi::RasterCreateValidation) != 0);
    const char* stage = "instance";
    if (result == abi::Result::Success) {
        stage = "device";
        result = impl_->CreateDevice(request.adapterLuid,
            (request.flags & abi::RasterCreateAnyAdapter) != 0);
    }
    if (result == abi::Result::Success) {
        stage = "core objects";
        result = impl_->CreateCoreObjects();
    }
    if (result != abi::Result::Success) {
        // Which stage, because one message for three of them turns a pipeline
        // that failed to compile into something indistinguishable from a
        // machine with no Vulkan device.
        char detail[96]{};
        std::snprintf(detail, sizeof(detail),
            "raster %s creation failed result=%u", stage,
            static_cast<unsigned>(result));
        impl_->FillStatus(status, result, detail);
        impl_->Reset();
        return result;
    }
    impl_->ready = true;
    impl_->FillStatus(status, abi::Result::Success, "ready");
    return abi::Result::Success;
}

abi::Result VulkanRasterRenderer::Render(
    const abi::RasterFrameRequestV1& request,
    abi::RasterStatusV1& status) noexcept
{
    if (!impl_->ready) {
        InitializeStatus(status, abi::Result::RasterNotCreated,
            "raster session not created");
        return abi::Result::RasterNotCreated;
    }
    if (request.structSize < abi::kRasterFrameRequestV1RequiredSize ||
        status.structSize < abi::kRasterStatusV1RequiredSize ||
        request.packetData == 0 || request.packetSize == 0 ||
        request.packetSize > std::numeric_limits<std::size_t>::max() ||
        request.outputData == 0) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "invalid raster frame contract");
        return abi::Result::InvalidArgument;
    }
    // Separated from the malformed-contract check above because the two have
    // nothing in common but their result. A packet that is merely large is
    // well formed, and reporting it as a contract violation sent a whole
    // investigation after a corrupt encoder: a Fallout 4 exterior cell
    // measures around a hundred megabytes in this format, and the ceiling it
    // was refused by was a fixed sixty-four that named neither number.
    if (request.packetSize >
        raster::MaximumPacketBytes(impl_->maximumAllocationBytes)) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "raster packet exceeds the device allocation ceiling");
        return abi::Result::InvalidArgument;
    }

    const auto* packetAddress = reinterpret_cast<const std::byte*>(
        static_cast<std::uintptr_t>(request.packetData));
    raster::DecodedPacket packet;
    const auto packetResult = raster::DecodePacket(
        std::span{packetAddress,
            static_cast<std::size_t>(request.packetSize)}, packet);
    if (!packetResult) {
        impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
            raster::ToString(packetResult.error));
        status.packetError = static_cast<std::uint32_t>(packetResult.error);
        return abi::Result::RasterInvalidPacket;
    }
    const auto minimumPitch = static_cast<std::uint64_t>(
        packet.header.width) * 4;
    const auto requiredBytes = static_cast<std::uint64_t>(
        request.outputRowPitch) * packet.header.height;
    if (request.outputRowPitch < minimumPitch ||
        requiredBytes > request.outputCapacity) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "output buffer is too small");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }

    texture::CapturedTexture sampledSource;
    std::uint64_t sampledSignature = 0xF8A1'0000'0000'0001ull;
    const auto textureFieldsAvailable = request.structSize >=
        abi::kRasterFrameRequestV1TextureRequiredSize;
    const auto materialFieldsAvailable = request.structSize >=
        abi::kRasterFrameRequestV1MaterialRequiredSize;
    const auto frameFieldsAvailable = request.structSize >=
        abi::kRasterFrameRequestV1FrameRequiredSize;
    const auto sceneFieldsAvailable = request.structSize >=
        abi::kRasterFrameRequestV1SceneInputRequiredSize;
    const auto gbufferFieldsAvailable = request.structSize >=
        abi::kRasterFrameRequestV1SceneRequiredSize;
    const auto hasTexture = textureFieldsAvailable &&
        request.textureData != 0 && request.textureSize != 0;
    const auto hasMaterial = materialFieldsAvailable &&
        request.materialData != 0 && request.materialSize != 0;
    const auto hasFrame = frameFieldsAvailable &&
        request.frameData != 0 && request.frameSize != 0;
    const auto hasScene = sceneFieldsAvailable &&
        request.sceneData != 0 && request.sceneSize != 0;
    const auto hasGBufferOutput = gbufferFieldsAvailable &&
        request.gbufferData != 0 && request.gbufferCapacity != 0;
    const auto deformationFieldsAvailable = request.structSize >=
        abi::kRasterFrameRequestV1DeformationInputRequiredSize;
    const auto deformationOutputAvailable = request.structSize >=
        abi::kRasterFrameRequestV1DeformationRequiredSize;
    const auto hasDeformation = deformationFieldsAvailable &&
        request.deformationData != 0 && request.deformationSize != 0;
    const auto hasDeformationOutput = deformationOutputAvailable &&
        request.deformationOutputData != 0 &&
        request.deformationOutputCapacity != 0;
    const auto terrainFieldsAvailable = request.structSize >=
        abi::kRasterFrameRequestV1TerrainRequiredSize;
    const auto hasTerrain = terrainFieldsAvailable &&
        request.terrainData != 0 && request.terrainSize != 0;
    const auto familyFieldsAvailable = request.structSize >=
        abi::kRasterFrameRequestV1FamilyRequiredSize;
    const auto hasFamily = familyFieldsAvailable &&
        request.familyData != 0 && request.familySize != 0;
    const auto hdrFieldsAvailable = request.structSize >=
        abi::kRasterFrameRequestV1HdrRequiredSize;
    const auto hasHdrReadback = hdrFieldsAvailable &&
        request.hdrData != 0 && request.hdrCapacity != 0;
    const auto lightFieldsAvailable = request.structSize >=
        abi::kRasterFrameRequestV1LightRequiredSize;
    const auto hasLights = lightFieldsAvailable &&
        request.lightData != 0 && request.lightSize != 0;
    if (textureFieldsAvailable &&
        ((request.textureData == 0) != (request.textureSize == 0))) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "incomplete texture frame contract");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    if (materialFieldsAvailable &&
        ((request.materialData == 0) != (request.materialSize == 0))) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "incomplete material frame contract");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    if (frameFieldsAvailable &&
        ((request.frameData == 0) != (request.frameSize == 0))) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "incomplete captured-frame contract");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    if (sceneFieldsAvailable &&
        ((request.sceneData == 0) != (request.sceneSize == 0))) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "incomplete scene frame contract");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    if (gbufferFieldsAvailable &&
        ((request.gbufferData == 0) != (request.gbufferCapacity == 0))) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "incomplete scene G-buffer contract");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    if (hasTexture && hasMaterial) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "texture and material frame contracts are mutually exclusive");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    if (hasScene && !hasFrame) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "scene packets require a captured view");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    if (hasGBufferOutput && !hasScene) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "G-buffer output requires a scene packet");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    if (deformationFieldsAvailable &&
        ((request.deformationData == 0) != (request.deformationSize == 0))) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "incomplete deformation contract");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    if (deformationOutputAvailable &&
        ((request.deformationOutputData == 0) !=
            (request.deformationOutputCapacity == 0))) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "incomplete deformation output contract");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    if (hasDeformationOutput && !hasDeformation) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "deformation output requires a deformation packet");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    if (terrainFieldsAvailable &&
        ((request.terrainData == 0) != (request.terrainSize == 0))) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "incomplete terrain contract");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    if (familyFieldsAvailable &&
        ((request.familyData == 0) != (request.familySize == 0))) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "incomplete material family contract");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    if (hdrFieldsAvailable &&
        ((request.hdrData == 0) != (request.hdrCapacity == 0))) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "incomplete hdr readback contract");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    if (lightFieldsAvailable &&
        ((request.lightData == 0) != (request.lightSize == 0))) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "incomplete lighting contract");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    // Light positions are narrowed against the captured camera origin, so a
    // light list cannot be placed without the view that supplies it.
    if (hasLights && !hasFrame) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "lighting requires a captured view");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    // Family records are addressed per scene object, so they cannot be
    // placed without the scene that names those objects.
    if (hasFamily && !hasScene) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "material families require a scene packet");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    // Terrain is rendered camera relative, so it cannot be placed without the
    // captured view that supplies the camera origin.
    if (hasTerrain && !hasFrame) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "terrain packets require a captured view");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    // The landscape layer array travels in the captured-texture slot, so a
    // terrain frame cannot also carry a material bundle.
    if (hasTerrain && (!hasTexture || hasMaterial)) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "terrain frames carry their layer array as the captured texture");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }
    const auto gbufferBytes = static_cast<std::uint64_t>(
        packet.header.width) * packet.header.height *
        sizeof(scene::GBufferPixelV1);
    if (hasGBufferOutput && request.gbufferCapacity < gbufferBytes) {
        impl_->FillStatus(status, abi::Result::InvalidArgument,
            "scene G-buffer output is too small");
        status.frameIndex = packet.header.frameIndex;
        return abi::Result::InvalidArgument;
    }

    view::FramePacket decodedFrame;
    view::ViewRecordV1 decodedView{};
    view::GpuViewConstantsV1 decodedViewConstants{};
    if (hasFrame) {
        if (request.frameSize > kMaximumFramePacketBytes ||
            request.frameSize > std::numeric_limits<std::size_t>::max()) {
            impl_->FillStatus(status, abi::Result::InvalidArgument,
                "captured frame packet exceeds limit");
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::InvalidArgument;
        }
        const auto* frameAddress = reinterpret_cast<const std::byte*>(
            static_cast<std::uintptr_t>(request.frameData));
        const std::span frameBytes{frameAddress,
            static_cast<std::size_t>(request.frameSize)};
        const auto frameResult = view::DecodeFramePacket(
            frameBytes, decodedFrame);
        if (frameResult != view::FramePacketError::None) {
            impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                view::ToString(frameResult));
            status.packetError = static_cast<std::uint32_t>(frameResult);
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::RasterInvalidPacket;
        }
        if (decodedFrame.views.size() != 1 ||
            decodedFrame.header.frameId != packet.header.frameIndex) {
            impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                "captured frame does not own raster frame");
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::RasterInvalidPacket;
        }
        decodedView = decodedFrame.views.front();
        if (decodedView.outputWidth != packet.header.width ||
            decodedView.outputHeight != packet.header.height ||
            decodedView.viewport.x != packet.header.viewportX ||
            decodedView.viewport.y != packet.header.viewportY ||
            decodedView.viewport.width != packet.header.viewportWidth ||
            decodedView.viewport.height != packet.header.viewportHeight ||
            decodedView.viewport.minimumDepth !=
                packet.header.viewportMinDepth ||
            decodedView.viewport.maximumDepth !=
                packet.header.viewportMaxDepth ||
            decodedView.scissor.x != packet.header.scissorX ||
            decodedView.scissor.y != packet.header.scissorY ||
            decodedView.scissor.width != packet.header.scissorWidth ||
            decodedView.scissor.height != packet.header.scissorHeight ||
            view::BuildGpuViewConstants(&decodedView,
                decodedFrame.header.historyEpoch,
                decodedViewConstants) != view::ViewError::None) {
            impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                "captured view disagrees with raster extent/state");
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::RasterInvalidPacket;
        }
    } else if (view::BuildGpuViewConstants(
            nullptr, 0, decodedViewConstants) != view::ViewError::None) {
        impl_->FillStatus(status, abi::Result::InternalFailure,
            "identity view constants failed");
        return abi::Result::InternalFailure;
    }

    deform::DeformationPacket decodedDeformation;
    if (hasDeformation) {
        if (request.deformationSize > kMaximumDeformationPacketBytes ||
            request.deformationSize >
                std::numeric_limits<std::size_t>::max()) {
            impl_->FillStatus(status, abi::Result::InvalidArgument,
                "deformation packet exceeds limit");
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::InvalidArgument;
        }
        const auto* deformAddress = reinterpret_cast<const std::byte*>(
            static_cast<std::uintptr_t>(request.deformationData));
        const auto deformResult = deform::DecodeDeformationPacket(
            std::span{deformAddress,
                static_cast<std::size_t>(request.deformationSize)},
            decodedDeformation);
        if (deformResult != deform::DeformError::None) {
            impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                deform::ToString(deformResult));
            status.packetError = static_cast<std::uint32_t>(deformResult);
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::RasterInvalidPacket;
        }
        // The deformed stream replaces the packet's bind-pose vertices, so
        // the two must describe the same topology.
        if (decodedDeformation.vertices.size() != packet.vertices.size()) {
            impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                "deformation does not match the raster vertex stream");
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::RasterInvalidPacket;
        }
        const auto topology = impl_->deformTopology.Observe(
            decodedDeformation);
        if (topology != deform::DeformError::None) {
            impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                deform::ToString(topology));
            status.packetError = static_cast<std::uint32_t>(topology);
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::RasterInvalidPacket;
        }
    }

    scene::ScenePacket decodedScene;
    if (hasScene) {
        if (request.sceneSize > kMaximumScenePacketBytes ||
            request.sceneSize > std::numeric_limits<std::size_t>::max()) {
            impl_->FillStatus(status, abi::Result::InvalidArgument,
                "scene packet exceeds limit");
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::InvalidArgument;
        }
        const auto* sceneAddress = reinterpret_cast<const std::byte*>(
            static_cast<std::uintptr_t>(request.sceneData));
        const std::span sceneBytes{sceneAddress,
            static_cast<std::size_t>(request.sceneSize)};
        const auto sceneResult = scene::DecodeScenePacket(
            sceneBytes, decodedScene);
        if (sceneResult != scene::ScenePacketError::None) {
            impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                scene::ToString(sceneResult));
            status.packetError = static_cast<std::uint32_t>(sceneResult);
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::RasterInvalidPacket;
        }
        const auto rasterAssociation = scene::ValidateSceneAgainstRaster(
            decodedScene, packet, packet.header.frameIndex,
            decodedView.viewId);
        if (rasterAssociation != scene::ScenePacketError::None) {
            impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                scene::ToString(rasterAssociation));
            status.packetError =
                static_cast<std::uint32_t>(rasterAssociation);
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::RasterInvalidPacket;
        }
        // Engine pass accounting gates the mirror: a frame that still owns
        // an unclassified world-target writer can never be armed.
        scene::SceneCoverage coverage{};
        const auto passAccounting = scene::ValidateSceneAgainstFrame(
            decodedScene, decodedFrame, coverage);
        if (passAccounting != scene::ScenePacketError::None ||
            !coverage.MirrorEligible()) {
            impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                passAccounting != scene::ScenePacketError::None
                    ? scene::ToString(passAccounting)
                    : "scene is not mirror eligible");
            status.packetError = static_cast<std::uint32_t>(passAccounting);
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::RasterInvalidPacket;
        }
    }

    material::FamilyPacket decodedFamilies;
    if (hasFamily) {
        if (request.familySize > kMaximumFamilyPacketBytes ||
            request.familySize > std::numeric_limits<std::size_t>::max()) {
            impl_->FillStatus(status, abi::Result::InvalidArgument,
                "family packet exceeds limit");
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::InvalidArgument;
        }
        const auto* const familyBase = reinterpret_cast<const std::byte*>(
            static_cast<std::uintptr_t>(request.familyData));
        const auto familyResult = material::DecodeFamilyPacket(
            std::span<const std::byte>{familyBase,
                static_cast<std::size_t>(request.familySize)},
            decodedFamilies);
        if (familyResult != material::FamilyPacketError::None) {
            impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                material::ToString(familyResult));
            status.packetError = static_cast<std::uint32_t>(familyResult);
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::RasterInvalidPacket;
        }
        // A family table that names an object the scene does not own would
        // silently apply to nothing, so it is refused rather than ignored.
        for (const auto& record : decodedFamilies.records) {
            const auto owned = std::any_of(
                decodedScene.objects.begin(), decodedScene.objects.end(),
                [&record](const scene::OpaqueObjectV1& object) {
                    return object.objectId == record.objectId;
                });
            if (!owned) {
                impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                    "family record names an object the scene does not own");
                status.frameIndex = packet.header.frameIndex;
                return abi::Result::RasterInvalidPacket;
            }
        }
    }

    lighting::LightPacket decodedLights;
    if (hasLights) {
        if (request.lightSize > kMaximumLightPacketBytes ||
            request.lightSize > std::numeric_limits<std::size_t>::max()) {
            impl_->FillStatus(status, abi::Result::InvalidArgument,
                "light packet exceeds limit");
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::InvalidArgument;
        }
        const auto* const lightBase = reinterpret_cast<const std::byte*>(
            static_cast<std::uintptr_t>(request.lightData));
        const auto lightResult = lighting::DecodeLightPacket(
            std::span<const std::byte>{lightBase,
                static_cast<std::size_t>(request.lightSize)},
            decodedLights);
        if (lightResult != lighting::LightPacketError::None) {
            impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                lighting::ToString(lightResult));
            status.packetError = static_cast<std::uint32_t>(lightResult);
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::RasterInvalidPacket;
        }
    }

    terrain::TerrainPacket decodedTerrain;
    std::vector<terrain::GpuTerrainCellV1> decodedTerrainCells;
    if (hasTerrain) {
        if (request.terrainSize > kMaximumTerrainPacketBytes ||
            request.terrainSize > std::numeric_limits<std::size_t>::max()) {
            impl_->FillStatus(status, abi::Result::InvalidArgument,
                "terrain packet exceeds limit");
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::InvalidArgument;
        }
        const auto* terrainAddress = reinterpret_cast<const std::byte*>(
            static_cast<std::uintptr_t>(request.terrainData));
        const auto terrainResult = terrain::DecodeTerrainPacket(
            std::span{terrainAddress,
                static_cast<std::size_t>(request.terrainSize)},
            decodedTerrain);
        if (terrainResult != terrain::TerrainError::None) {
            impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                terrain::ToString(terrainResult));
            status.packetError = static_cast<std::uint32_t>(terrainResult);
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::RasterInvalidPacket;
        }
        if (decodedTerrain.header.frameId != packet.header.frameIndex ||
            decodedTerrain.header.viewId != decodedView.viewId) {
            impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                "terrain packet does not own this frame and view");
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::RasterInvalidPacket;
        }
        const std::span<const double, 3> cameraOrigin{
            decodedView.cameraRelativeOrigin, 3};
        const auto cellResult = terrain::BuildGpuTerrainCells(
            decodedTerrain, cameraOrigin, decodedTerrainCells);
        if (cellResult != terrain::TerrainError::None) {
            impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                terrain::ToString(cellResult));
            status.packetError = static_cast<std::uint32_t>(cellResult);
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::RasterInvalidPacket;
        }
    }

    material::MaterialReplayBundle decodedMaterial;
    material::MaterialGpuRecords decodedRecords{};
    std::array<std::uint64_t, 3> materialSignatures{};
    if (hasMaterial) {
        if (request.materialSize > kMaximumMaterialPacketBytes ||
            request.materialSize > std::numeric_limits<std::size_t>::max()) {
            impl_->FillStatus(status, abi::Result::InvalidArgument,
                "material packet exceeds limit");
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::InvalidArgument;
        }
        const auto* materialAddress = reinterpret_cast<const std::byte*>(
            static_cast<std::uintptr_t>(request.materialData));
        const std::span materialBytes{materialAddress,
            static_cast<std::size_t>(request.materialSize)};
        const auto materialResult = material::DecodeMaterialReplayBundle(
            materialBytes, decodedMaterial);
        if (materialResult != material::MaterialPacketError::None) {
            impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                material::ToString(materialResult));
            status.packetError = static_cast<std::uint32_t>(materialResult);
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::RasterInvalidPacket;
        }
        try {
            material::BindlessTextureTable descriptors;
            for (std::size_t index = 0;
                 index < decodedMaterial.textures.size(); ++index) {
                if (descriptors.Register(
                        decodedMaterial.textures[index].resourceId,
                        decodedMaterial.textures[index].generation,
                        static_cast<std::uint32_t>(index)) !=
                    material::DescriptorError::None) {
                    impl_->FillStatus(status,
                        abi::Result::RasterInvalidPacket,
                        "duplicate material texture identity");
                    status.frameIndex = packet.header.frameIndex;
                    return abi::Result::RasterInvalidPacket;
                }
            }
            const auto recordsResult = material::BuildMaterialGpuRecords(
                decodedMaterial.material, descriptors,
                decodedMaterial.transferVersion, decodedRecords);
            if (recordsResult != material::MaterialError::None) {
                impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                    material::ToString(recordsResult));
                status.packetError =
                    static_cast<std::uint32_t>(recordsResult);
                status.frameIndex = packet.header.frameIndex;
                return abi::Result::RasterInvalidPacket;
            }
        } catch (...) {
            impl_->FillStatus(status, abi::Result::InternalFailure,
                "material descriptor allocation failed");
            return abi::Result::InternalFailure;
        }
        const auto packetCrc = vf::renderer::trace::Crc32(materialBytes);
        for (std::size_t index = 0;
             index < materialSignatures.size(); ++index) {
            materialSignatures[index] =
                decodedMaterial.textures[index].resourceId ^
                (static_cast<std::uint64_t>(
                    decodedMaterial.textures[index].generation) << 32) ^
                (static_cast<std::uint64_t>(index + 1) << 56) ^ packetCrc;
        }
    } else if (hasTexture) {
        if (request.textureSize > kMaximumTexturePacketBytes ||
            request.textureSize > std::numeric_limits<std::size_t>::max()) {
            impl_->FillStatus(status, abi::Result::InvalidArgument,
                "texture packet exceeds limit");
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::InvalidArgument;
        }
        const auto* textureAddress = reinterpret_cast<const std::byte*>(
            static_cast<std::uintptr_t>(request.textureData));
        const std::span textureBytes{textureAddress,
            static_cast<std::size_t>(request.textureSize)};
        const auto textureResult = texture::DecodeCapturedTexture(
            textureBytes, sampledSource);
        if (textureResult != texture::TexturePacketError::None) {
            impl_->FillStatus(status, abi::Result::RasterInvalidPacket,
                texture::ToString(textureResult));
            status.packetError =
                static_cast<std::uint32_t>(textureResult);
            status.frameIndex = packet.header.frameIndex;
            return abi::Result::RasterInvalidPacket;
        }
        sampledSignature = sampledSource.resourceId ^
            (static_cast<std::uint64_t>(sampledSource.generation) << 32) ^
            (static_cast<std::uint64_t>(request.textureSize) << 1) ^
            vf::renderer::trace::Crc32(textureBytes);
    } else {
        try {
            sampledSource = texture::MakeFallbackTexture(
                texture::FallbackTextureRole::White);
        } catch (...) {
            impl_->FillStatus(status, abi::Result::InternalFailure,
                "fallback texture allocation failed");
            return abi::Result::InternalFailure;
        }
    }

    auto result = impl_->WaitForSubmission();
    if (result == abi::Result::Success) {
        impl_->viewRecord = decodedView;
        impl_->viewConstants = decodedViewConstants;
        impl_->phase10ViewActive = hasFrame;
        try {
            impl_->scenePacket = std::move(decodedScene);
            impl_->phase11SceneActive = hasScene;
            impl_->familyPacket = std::move(decodedFamilies);
            impl_->phase16FamilyActive = hasFamily;
            impl_->lightPacket = std::move(decodedLights);
            impl_->phase17LightingActive = hasLights;
            impl_->hdrReadbackRequested = hasHdrReadback;
            impl_->deformPacket = std::move(decodedDeformation);
            impl_->phase13DeformActive = hasDeformation;
            impl_->terrainPacket = std::move(decodedTerrain);
            impl_->terrainCellRecords = std::move(decodedTerrainCells);
            impl_->phase14TerrainActive = hasTerrain;
        } catch (...) {
            impl_->phase11SceneActive = false;
            impl_->phase13DeformActive = false;
            impl_->phase14TerrainActive = false;
            result = abi::Result::InternalFailure;
        }
    }
    if (result == abi::Result::Success && hasMaterial) {
        try {
            impl_->materialBundle = std::move(decodedMaterial);
            impl_->materialRecords = decodedRecords;
            impl_->phase9MaterialActive = true;
        } catch (...) {
            result = abi::Result::InternalFailure;
        }
        for (std::size_t index = 0;
             result == abi::Result::Success &&
             index < impl_->materialBundle.textures.size(); ++index) {
            result = impl_->PrepareSampledTexture(
                impl_->materialBundle.textures[index],
                materialSignatures[index], index);
        }
    } else if (result == abi::Result::Success) {
        impl_->phase9MaterialActive = false;
        if (hasTerrain) {
            // The captured texture is the landscape layer array. The base
            // texture slot keeps a neutral fallback so every pipeline that
            // shares this descriptor set still has a valid binding.
            result = impl_->PrepareSampledTexture(
                sampledSource, sampledSignature, kTerrainLayerTextureSlot);
            if (result == abi::Result::Success) {
                try {
                    const auto fallback = texture::MakeFallbackTexture(
                        texture::FallbackTextureRole::White);
                    result = impl_->PrepareSampledTexture(
                        fallback, 0xF8A1'0000'0000'0001ull, 0);
                } catch (...) {
                    result = abi::Result::InternalFailure;
                }
            }
        } else {
            result = impl_->PrepareSampledTexture(
                sampledSource, sampledSignature, 0);
        }
    }
    if (result == abi::Result::Success) {
        result = impl_->CreateExtent(
            packet.header.width, packet.header.height);
    }
    if (result == abi::Result::Success && (hasScene || hasTerrain)) {
        result = impl_->CreateSceneAttachments();
    }
    const auto uploadLayout = impl_->BuildUploadLayout(packet);
    if (result == abi::Result::Success) {
        result = impl_->UploadPacket(packet, uploadLayout);
    }
    if (result == abi::Result::Success) {
        result = impl_->PrepareDeformation(impl_->submissions + 1);
    }
    // Prepared before the submission that records it, like the deformation
    // above. A frame that supplies no history simply leaves the pixel count at
    // zero and the pass records nothing.
    impl_->frameFlags = request.flags;
    if (request.structSize >= abi::kRasterFrameRequestV1BloomRequiredSize) {
        impl_->bloomRequest.threshold = request.bloomThreshold;
        impl_->bloomRequest.knee = request.bloomKnee;
        impl_->bloomRequest.intensity = request.bloomIntensity;
    }
    if (result == abi::Result::Success) {
        result = impl_->PrepareIndirect(request);
    }
    if (result == abi::Result::Success) {
        result = impl_->RecordAndSubmit(packet, uploadLayout);
    }
    if (result == abi::Result::Success) {
        result = impl_->CopyOutput(request, packet);
    }
    if (result == abi::Result::Success && hasGBufferOutput) {
        result = impl_->CopySceneOutput(request);
    }
    if (result == abi::Result::Success && hasHdrReadback) {
        result = impl_->CopyHdrOutput(request);
    }
    if (result == abi::Result::Success && hasDeformationOutput) {
        result = impl_->CopyDeformationOutput(request);
    }
    if (result == abi::Result::Success) {
        result = impl_->CopyIndirectResults(request);
    }
    impl_->FillStatus(status, result,
        result == abi::Result::Success ? "rendered" : "raster frame failed");
    status.frameIndex = packet.header.frameIndex;
    status.outputBytes = minimumPitch * packet.header.height;
    return result;
}

abi::Result VulkanRasterRenderer::Destroy(
    abi::RasterStatusV1& status) noexcept
{
    if (!impl_->ready) {
        InitializeStatus(status, abi::Result::RasterNotCreated,
            "raster session not created");
        return abi::Result::RasterNotCreated;
    }
    const auto result = impl_->WaitForSubmission();
    if (result != abi::Result::Success) {
        impl_->FillStatus(status, result, "raster retirement failed");
        return result;
    }
    impl_->FillStatus(status, abi::Result::Success, "destroyed");
    impl_->Reset();
    return abi::Result::Success;
}

}
