#include "renderer_backend/VulkanProbe.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace vf::renderer::backend {

namespace {

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
                ? 3u
                : 2u,
            callbackData != nullptr && callbackData->pMessage != nullptr
                ? callbackData->pMessage
                : "Vulkan validation message");
    }
    return VK_FALSE;
}

class ProbeResources
{
public:
    ProbeResources() = default;
    ~ProbeResources()
    {
        Reset();
    }

    ProbeResources(const ProbeResources&) = delete;
    ProbeResources& operator=(const ProbeResources&) = delete;

    void Reset() noexcept
    {
        if (device != VK_NULL_HANDLE) {
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (messenger != VK_NULL_HANDLE &&
            destroyMessenger != nullptr &&
            instance != VK_NULL_HANDLE) {
            destroyMessenger(instance, messenger, nullptr);
            messenger = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
    }

    VkInstance instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT messenger{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger{};
};

abi::Result Fail(
    abi::CapabilityReportV1& report,
    const abi::Result result) noexcept
{
    report.result = result;
    return result;
}

bool HasExtension(
    const std::span<const VkExtensionProperties> extensions,
    const char* name) noexcept
{
    return std::any_of(
        extensions.begin(),
        extensions.end(),
        [name](const VkExtensionProperties& extension) {
            return std::strcmp(extension.extensionName, name) == 0;
        });
}

bool HasLayer(
    const std::span<const VkLayerProperties> layers,
    const char* name) noexcept
{
    return std::any_of(
        layers.begin(),
        layers.end(),
        [name](const VkLayerProperties& layer) {
            return std::strcmp(layer.layerName, name) == 0;
        });
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

bool SupportsBcFormats(const VkPhysicalDevice device) noexcept
{
    constexpr std::array formats{
        VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
        VK_FORMAT_BC3_UNORM_BLOCK,
        VK_FORMAT_BC5_UNORM_BLOCK,
        VK_FORMAT_BC7_UNORM_BLOCK,
    };
    for (const auto format : formats) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(device, format, &properties);
        if ((properties.optimalTilingFeatures &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
            return false;
        }
    }
    return true;
}

bool SupportsD3d11TextureImport(const VkPhysicalDevice device) noexcept
{
    VkPhysicalDeviceExternalImageFormatInfo externalInfo{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    externalInfo.handleType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkPhysicalDeviceImageFormatInfo2 formatInfo{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    formatInfo.pNext = &externalInfo;
    formatInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    formatInfo.type = VK_IMAGE_TYPE_2D;
    formatInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    formatInfo.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkExternalImageFormatProperties externalProperties{
        VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 properties{
        VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    properties.pNext = &externalProperties;
    if (vkGetPhysicalDeviceImageFormatProperties2(
            device, &formatInfo, &properties) != VK_SUCCESS) {
        return false;
    }
    const auto& memory =
        externalProperties.externalMemoryProperties;
    return (memory.externalMemoryFeatures &
            VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0 &&
        (memory.compatibleHandleTypes &
         VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT) != 0;
}

bool SupportsD3d12FenceImport(const VkPhysicalDevice device) noexcept
{
    VkPhysicalDeviceExternalSemaphoreInfo info{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
    info.handleType =
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    VkExternalSemaphoreProperties properties{
        VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    vkGetPhysicalDeviceExternalSemaphoreProperties(
        device, &info, &properties);
    return (properties.externalSemaphoreFeatures &
            VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0 &&
        (properties.compatibleHandleTypes &
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT) != 0;
}

abi::AdapterLuid ReadLuid(
    const VkPhysicalDeviceIDProperties& properties) noexcept
{
    abi::AdapterLuid luid{};
    static_assert(sizeof(luid) == VK_LUID_SIZE);
    std::memcpy(&luid, properties.deviceLUID, sizeof(luid));
    return luid;
}

void CopyOptionalText(
    abi::CapabilityReportV1& report,
    const std::size_t capacity,
    const char* deviceName,
    const char* driverName) noexcept
{
    if (capacity >=
        offsetof(abi::CapabilityReportV1, deviceName) +
            sizeof(report.deviceName)) {
        strncpy_s(
            report.deviceName,
            sizeof(report.deviceName),
            deviceName,
            _TRUNCATE);
    }
    if (capacity >=
        offsetof(abi::CapabilityReportV1, driverName) +
            sizeof(report.driverName)) {
        strncpy_s(
            report.driverName,
            sizeof(report.driverName),
            driverName,
            _TRUNCATE);
    }
}

}

abi::Result ProbeVulkan(
    const abi::HostCallbacksV1& callbacks,
    const abi::ProbeRequestV1& request,
    abi::CapabilityReportV1& report) noexcept
{
    const auto reportCapacity =
        std::min<std::size_t>(report.structSize, sizeof(report));
    std::memset(&report, 0, reportCapacity);
    report.structSize = sizeof(report);
    report.result = abi::Result::InternalFailure;
    report.queueFamilyIndex = 0xFFFFFFFFu;

    try {
        std::uint32_t loaderVersion = VK_API_VERSION_1_0;
        if (vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS ||
            loaderVersion < VK_API_VERSION_1_2) {
            return Fail(report, abi::Result::VulkanLoaderUnavailable);
        }

        std::vector<VkExtensionProperties> instanceExtensions;
        if (!EnumerateVector(
                instanceExtensions,
                [](std::uint32_t* count, VkExtensionProperties* values) {
                    return vkEnumerateInstanceExtensionProperties(
                        nullptr, count, values);
                })) {
            return Fail(report, abi::Result::InstanceCreationFailed);
        }
        const auto hasDebugUtils = HasExtension(
            instanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        std::vector<VkLayerProperties> layers;
        if (!EnumerateVector(
                layers,
                [](std::uint32_t* count, VkLayerProperties* values) {
                    return vkEnumerateInstanceLayerProperties(
                        count, values);
                })) {
            return Fail(report, abi::Result::InstanceCreationFailed);
        }
        constexpr const char* validationLayer =
            "VK_LAYER_KHRONOS_validation";
        const auto validationRequested = request.enableValidation != 0;
        if (validationRequested &&
            (!HasLayer(layers, validationLayer) || !hasDebugUtils)) {
            return Fail(report, abi::Result::ValidationLayerUnavailable);
        }

        DebugState debugState;
        debugState.callbacks = callbacks;
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
        debugInfo.pUserData = &debugState;

        const std::uint32_t requestedVersion = std::min(
            loaderVersion, static_cast<std::uint32_t>(VK_API_VERSION_1_3));
        VkApplicationInfo application{
            VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "VisualForgeRenderer";
        application.applicationVersion = 1;
        application.pEngineName = "VisualForge";
        application.engineVersion = 1;
        application.apiVersion = requestedVersion;

        const char* enabledLayer = validationLayer;
        const char* enabledInstanceExtension =
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkInstanceCreateInfo instanceInfo{
            VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pNext = validationRequested ? &debugInfo : nullptr;
        instanceInfo.pApplicationInfo = &application;
        instanceInfo.enabledLayerCount = validationRequested ? 1u : 0u;
        instanceInfo.ppEnabledLayerNames =
            validationRequested ? &enabledLayer : nullptr;
        instanceInfo.enabledExtensionCount =
            validationRequested ? 1u : 0u;
        instanceInfo.ppEnabledExtensionNames =
            validationRequested ? &enabledInstanceExtension : nullptr;

        ProbeResources resources;
        if (vkCreateInstance(
                &instanceInfo, nullptr, &resources.instance) != VK_SUCCESS) {
            return Fail(report, abi::Result::InstanceCreationFailed);
        }

        if (validationRequested) {
            const auto createMessenger =
                reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(
                        resources.instance,
                        "vkCreateDebugUtilsMessengerEXT"));
            resources.destroyMessenger =
                reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(
                        resources.instance,
                        "vkDestroyDebugUtilsMessengerEXT"));
            if (createMessenger == nullptr ||
                resources.destroyMessenger == nullptr ||
                createMessenger(
                    resources.instance,
                    &debugInfo,
                    nullptr,
                    &resources.messenger) != VK_SUCCESS) {
                return Fail(
                    report, abi::Result::ValidationLayerUnavailable);
            }
        }

        std::vector<VkPhysicalDevice> devices;
        if (!EnumerateVector(
                devices,
                [&resources](
                    std::uint32_t* count,
                    VkPhysicalDevice* values) {
                    return vkEnumeratePhysicalDevices(
                        resources.instance, count, values);
                })) {
            return Fail(report, abi::Result::AdapterLuidNotFound);
        }

        VkPhysicalDevice selected = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties2 selectedProperties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        VkPhysicalDeviceIDProperties selectedId{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceDriverProperties selectedDriver{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR selectedRtProperties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
        VkPhysicalDeviceAccelerationStructurePropertiesKHR selectedAsProperties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceVulkan12Features selected12{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features selected13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR selectedAsFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR selectedRtFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};

        for (const auto device : devices) {
            VkPhysicalDeviceProperties2 properties{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            VkPhysicalDeviceIDProperties id{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceDriverProperties driver{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProperties{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
            VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
            properties.pNext = &id;
            id.pNext = &driver;
            driver.pNext = &rtProperties;
            rtProperties.pNext = &asProperties;
            vkGetPhysicalDeviceProperties2(device, &properties);
            if (id.deviceLUIDValid == VK_FALSE ||
                ReadLuid(id) != request.adapterLuid) {
                continue;
            }

            selected = device;
            selectedProperties = properties;
            selectedId = id;
            selectedDriver = driver;
            selectedRtProperties = rtProperties;
            selectedAsProperties = asProperties;
            selectedProperties.pNext = &selectedId;
            selectedId.pNext = &selectedDriver;
            selectedDriver.pNext = &selectedRtProperties;
            selectedRtProperties.pNext = &selectedAsProperties;

            VkPhysicalDeviceFeatures2 features{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &selected12;
            selected12.pNext = &selected13;
            selected13.pNext = &selectedAsFeatures;
            selectedAsFeatures.pNext = &selectedRtFeatures;
            vkGetPhysicalDeviceFeatures2(device, &features);
            break;
        }

        if (selected == VK_NULL_HANDLE) {
            return Fail(report, abi::Result::AdapterLuidNotFound);
        }

        std::vector<VkExtensionProperties> deviceExtensions;
        if (!EnumerateVector(
                deviceExtensions,
                [selected](
                    std::uint32_t* count,
                    VkExtensionProperties* values) {
                    return vkEnumerateDeviceExtensionProperties(
                        selected, nullptr, count, values);
                })) {
            return Fail(
                report, abi::Result::RequiredCapabilityMissing);
        }

        std::uint32_t queueCount{};
        vkGetPhysicalDeviceQueueFamilyProperties(
            selected, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueProperties(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(
            selected, &queueCount, queueProperties.data());
        std::uint32_t selectedQueue = 0xFFFFFFFFu;
        for (std::uint32_t index = 0; index < queueCount; ++index) {
            constexpr auto requiredFlags =
                VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if (queueProperties[index].queueCount != 0 &&
                (queueProperties[index].queueFlags & requiredFlags) ==
                    requiredFlags) {
                selectedQueue = index;
                break;
            }
        }

        constexpr std::array requiredDeviceExtensions{
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        };
        const auto hasAllDeviceExtensions = std::all_of(
            requiredDeviceExtensions.begin(),
            requiredDeviceExtensions.end(),
            [&deviceExtensions](const char* extension) {
                return HasExtension(deviceExtensions, extension);
            });

        std::uint64_t capabilities{};
        if (selectedProperties.properties.apiVersion >=
            VK_API_VERSION_1_2) {
            capabilities |= abi::Capability::Vulkan12;
        }
        if (selectedQueue != 0xFFFFFFFFu) {
            capabilities |= abi::Capability::GraphicsComputeQueue;
        }
        if (selected12.bufferDeviceAddress != VK_FALSE) {
            capabilities |= abi::Capability::BufferDeviceAddress;
        }
        if (selected12.descriptorIndexing != VK_FALSE &&
            selected12.runtimeDescriptorArray != VK_FALSE &&
            selected12.descriptorBindingPartiallyBound != VK_FALSE &&
            selected12.shaderSampledImageArrayNonUniformIndexing !=
                VK_FALSE) {
            capabilities |= abi::Capability::DescriptorIndexing;
        }
        if (selected12.timelineSemaphore != VK_FALSE) {
            capabilities |= abi::Capability::TimelineSemaphore;
        }
        if (selected13.synchronization2 != VK_FALSE) {
            capabilities |= abi::Capability::Synchronization2;
        }
        if (selectedAsFeatures.accelerationStructure != VK_FALSE &&
            HasExtension(
                deviceExtensions,
                VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
            HasExtension(
                deviceExtensions,
                VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) {
            capabilities |= abi::Capability::AccelerationStructure;
        }
        if (selectedRtFeatures.rayTracingPipeline != VK_FALSE &&
            HasExtension(
                deviceExtensions,
                VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)) {
            capabilities |= abi::Capability::RayTracingPipeline;
        }
        if (SupportsBcFormats(selected)) {
            capabilities |= abi::Capability::BcTextureFormats;
        }
        if (HasExtension(
                deviceExtensions,
                VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME)) {
            capabilities |= abi::Capability::ExternalMemoryWin32;
        }
        if (HasExtension(
                deviceExtensions,
                VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME)) {
            capabilities |= abi::Capability::ExternalSemaphoreWin32;
        }
        if (SupportsD3d11TextureImport(selected)) {
            capabilities |= abi::Capability::D3d11TextureInterop;
        }
        if (SupportsD3d12FenceImport(selected)) {
            capabilities |= abi::Capability::D3d12FenceInterop;
        }
        if (hasDebugUtils) {
            capabilities |= abi::Capability::DebugUtils;
        }

        report.apiVersion = selectedProperties.properties.apiVersion;
        report.driverVersion =
            selectedProperties.properties.driverVersion;
        report.vendorId = selectedProperties.properties.vendorID;
        report.deviceId = selectedProperties.properties.deviceID;
        report.queueFamilyIndex = selectedQueue;
        report.adapterLuid = ReadLuid(selectedId);
        report.supportedCapabilities = capabilities;
        report.missingRequiredCapabilities =
            request.requiredCapabilities & ~capabilities;
        report.maxPerStageSampledImages =
            selectedProperties.properties.limits
                .maxPerStageDescriptorSampledImages;
        report.maxDescriptorSetSampledImages =
            selectedProperties.properties.limits
                .maxDescriptorSetSampledImages;
        report.maxPushConstantsSize =
            selectedProperties.properties.limits.maxPushConstantsSize;
        report.maxRayRecursionDepth =
            selectedRtProperties.maxRayRecursionDepth;
        report.shaderGroupHandleSize =
            selectedRtProperties.shaderGroupHandleSize;
        report.accelerationStructureScratchAlignment =
            static_cast<std::uint32_t>(
                std::min<VkDeviceSize>(
                    selectedAsProperties
                        .minAccelerationStructureScratchOffsetAlignment,
                    std::numeric_limits<std::uint32_t>::max()));
        CopyOptionalText(
            report,
            reportCapacity,
            selectedProperties.properties.deviceName,
            selectedDriver.driverName);

        if (!hasAllDeviceExtensions ||
            report.missingRequiredCapabilities != 0) {
            return Fail(
                report, abi::Result::RequiredCapabilityMissing);
        }
        if (selectedQueue == 0xFFFFFFFFu) {
            return Fail(report, abi::Result::QueueFamilyNotFound);
        }

        const float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = selectedQueue;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;

        VkPhysicalDeviceVulkan12Features enabled12{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        enabled12.bufferDeviceAddress = VK_TRUE;
        enabled12.descriptorIndexing = VK_TRUE;
        enabled12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        enabled12.descriptorBindingPartiallyBound = VK_TRUE;
        enabled12.runtimeDescriptorArray = VK_TRUE;
        enabled12.timelineSemaphore = VK_TRUE;
        VkPhysicalDeviceVulkan13Features enabled13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        enabled13.synchronization2 = VK_TRUE;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR enabledAs{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        enabledAs.accelerationStructure = VK_TRUE;
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR enabledRt{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        enabledRt.rayTracingPipeline = VK_TRUE;
        enabled12.pNext = &enabled13;
        enabled13.pNext = &enabledAs;
        enabledAs.pNext = &enabledRt;

        VkDeviceCreateInfo deviceInfo{
            VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.pNext = &enabled12;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount =
            static_cast<std::uint32_t>(
                requiredDeviceExtensions.size());
        deviceInfo.ppEnabledExtensionNames =
            requiredDeviceExtensions.data();
        if (vkCreateDevice(
                selected, &deviceInfo, nullptr, &resources.device) !=
            VK_SUCCESS) {
            return Fail(report, abi::Result::DeviceCreationFailed);
        }

        resources.Reset();
        report.validationErrorCount =
            debugState.errors.load(std::memory_order_acquire);
        report.result = abi::Result::Success;
        return abi::Result::Success;
    } catch (...) {
        return Fail(report, abi::Result::InternalFailure);
    }
}

}
