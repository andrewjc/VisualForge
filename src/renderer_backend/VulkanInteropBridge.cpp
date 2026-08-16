#include "renderer_backend/VulkanInteropBridge.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace vf::renderer::backend {

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

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
                : "Vulkan bridge validation message");
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
         index < properties.memoryTypeCount;
         ++index) {
        if ((typeBits & (1u << index)) == 0 ||
            (properties.memoryTypes[index].propertyFlags & required) !=
                required) {
            continue;
        }
        if ((properties.memoryTypes[index].propertyFlags & preferred) ==
            preferred) {
            return index;
        }
        if (fallback == std::numeric_limits<std::uint32_t>::max()) {
            fallback = index;
        }
    }
    return fallback;
}

void InitializeStatus(
    abi::BridgeStatusV1& status,
    const abi::Result result) noexcept
{
    const auto capacity = std::min<std::size_t>(
        status.structSize, sizeof(status));
    std::memset(&status, 0, capacity);
    status.structSize = sizeof(status);
    status.result = result;
}

}

struct VulkanInteropBridge::Impl
{
    struct Image
    {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkBuffer upload{VK_NULL_HANDLE};
        VkDeviceMemory uploadMemory{VK_NULL_HANDLE};
        void* mapped{};
        VkCommandBuffer command{VK_NULL_HANDLE};
        VkFence completion{VK_NULL_HANDLE};
        std::uint64_t lastReady{};
        bool uploadCoherent{};
        bool initialized{};
    };

    abi::HostCallbacksV1 callbacks{};
    DebugState debug;
    VkInstance instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT messenger{VK_NULL_HANDLE};
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger{};
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    std::uint32_t queueFamily{0xFFFFFFFFu};
    VkSemaphore fenceSemaphore{VK_NULL_HANDLE};
    VkCommandPool commandPool{VK_NULL_HANDLE};
    std::array<Image, abi::kBridgeImageCount> images{};
    std::uint64_t epoch{};
    std::uint64_t lastRelease{};
    std::uint64_t lastReady{};
    std::uint64_t submissions{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t imageCount{};
    VkDeviceSize uploadSize{};
    bool ready{};

    void FillStatus(
        abi::BridgeStatusV1& status,
        abi::Result result) const noexcept;
    void Reset() noexcept;
    [[nodiscard]] abi::Result CreateInstance(bool validation) noexcept;
    [[nodiscard]] abi::Result CreateDevice(
        abi::AdapterLuid requiredLuid) noexcept;
    [[nodiscard]] abi::Result ImportFence(HANDLE handle) noexcept;
    [[nodiscard]] abi::Result CreateCommandPool() noexcept;
    [[nodiscard]] abi::Result ImportImage(
        Image& resource,
        HANDLE handle) noexcept;
    [[nodiscard]] abi::Result CreateUpload(Image& resource) noexcept;
    [[nodiscard]] abi::Result FlushUpload(const Image& resource) noexcept;
};

void VulkanInteropBridge::Impl::FillStatus(
    abi::BridgeStatusV1& status,
    const abi::Result result) const noexcept
{
    InitializeStatus(status, result);
    status.epoch = epoch;
    status.lastReleaseValue = lastRelease;
    status.lastReadyValue = lastReady;
    status.submissionCount = submissions;
    status.validationErrorCount =
        debug.errors.load(std::memory_order_acquire);
    status.imageCount = imageCount;
}

void VulkanInteropBridge::Impl::Reset() noexcept
{
    if (device != VK_NULL_HANDLE) {
        static_cast<void>(vkDeviceWaitIdle(device));
        for (auto& resource : images) {
            if (resource.mapped != nullptr &&
                resource.uploadMemory != VK_NULL_HANDLE) {
                vkUnmapMemory(device, resource.uploadMemory);
                resource.mapped = nullptr;
            }
            if (resource.upload != VK_NULL_HANDLE) {
                vkDestroyBuffer(device, resource.upload, nullptr);
                resource.upload = VK_NULL_HANDLE;
            }
            if (resource.uploadMemory != VK_NULL_HANDLE) {
                vkFreeMemory(device, resource.uploadMemory, nullptr);
                resource.uploadMemory = VK_NULL_HANDLE;
            }
            if (resource.image != VK_NULL_HANDLE) {
                vkDestroyImage(device, resource.image, nullptr);
                resource.image = VK_NULL_HANDLE;
            }
            if (resource.memory != VK_NULL_HANDLE) {
                vkFreeMemory(device, resource.memory, nullptr);
                resource.memory = VK_NULL_HANDLE;
            }
            if (resource.completion != VK_NULL_HANDLE) {
                vkDestroyFence(device, resource.completion, nullptr);
                resource.completion = VK_NULL_HANDLE;
            }
            resource.command = VK_NULL_HANDLE;
            resource.initialized = false;
        }
        if (commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, commandPool, nullptr);
            commandPool = VK_NULL_HANDLE;
        }
        if (fenceSemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, fenceSemaphore, nullptr);
            fenceSemaphore = VK_NULL_HANDLE;
        }
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (messenger != VK_NULL_HANDLE &&
        destroyMessenger != nullptr && instance != VK_NULL_HANDLE) {
        destroyMessenger(instance, messenger, nullptr);
        messenger = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
    ready = false;
    physicalDevice = VK_NULL_HANDLE;
    queue = VK_NULL_HANDLE;
    queueFamily = 0xFFFFFFFFu;
    imageCount = 0;
}

abi::Result VulkanInteropBridge::Impl::CreateInstance(
    const bool validation) noexcept
{
    std::uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS ||
        loaderVersion < VK_API_VERSION_1_3) {
        return abi::Result::VulkanLoaderUnavailable;
    }

    std::vector<VkExtensionProperties> extensions;
    std::vector<VkLayerProperties> layers;
    if (!EnumerateVector(
            extensions,
            [](std::uint32_t* count, VkExtensionProperties* values) {
                return vkEnumerateInstanceExtensionProperties(
                    nullptr, count, values);
            }) ||
        !EnumerateVector(
            layers,
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
    application.pApplicationName = "VisualForgeInterop";
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
                vkGetInstanceProcAddr(
                    instance, "vkCreateDebugUtilsMessengerEXT"));
        destroyMessenger =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(
                    instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (createMessenger == nullptr || destroyMessenger == nullptr ||
            createMessenger(
                instance,
                &debugInfo,
                nullptr,
                &messenger) != VK_SUCCESS) {
            return abi::Result::ValidationLayerUnavailable;
        }
    }
    return abi::Result::Success;
}

abi::Result VulkanInteropBridge::Impl::CreateDevice(
    const abi::AdapterLuid requiredLuid) noexcept
{
    std::vector<VkPhysicalDevice> devices;
    if (!EnumerateVector(
            devices,
            [this](std::uint32_t* count, VkPhysicalDevice* values) {
                return vkEnumeratePhysicalDevices(
                    instance, count, values);
            })) {
        return abi::Result::AdapterLuidNotFound;
    }

    VkPhysicalDeviceVulkan12Features selected12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features selected13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    for (const auto candidate : devices) {
        VkPhysicalDeviceProperties2 properties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        VkPhysicalDeviceIDProperties id{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        properties.pNext = &id;
        vkGetPhysicalDeviceProperties2(candidate, &properties);
        if (id.deviceLUIDValid == VK_FALSE ||
            ReadLuid(id) != requiredLuid) {
            continue;
        }
        physicalDevice = candidate;
        VkPhysicalDeviceFeatures2 features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &selected12;
        selected12.pNext = &selected13;
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        break;
    }
    if (physicalDevice == VK_NULL_HANDLE) {
        return abi::Result::AdapterLuidNotFound;
    }
    if (selected12.timelineSemaphore == VK_FALSE ||
        selected13.synchronization2 == VK_FALSE) {
        return abi::Result::BridgeUnsupported;
    }

    std::vector<VkExtensionProperties> extensions;
    if (!EnumerateVector(
            extensions,
            [this](
                std::uint32_t* count,
                VkExtensionProperties* values) {
                return vkEnumerateDeviceExtensionProperties(
                    physicalDevice, nullptr, count, values);
            }) ||
        !HasExtension(
            extensions, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) ||
        !HasExtension(
            extensions,
            VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME)) {
        return abi::Result::BridgeUnsupported;
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

    VkPhysicalDeviceExternalImageFormatInfo externalImage{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    externalImage.handleType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkPhysicalDeviceImageFormatInfo2 imageInfo{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    imageInfo.pNext = &externalImage;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.type = VK_IMAGE_TYPE_2D;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkExternalImageFormatProperties externalProperties{
        VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 imageProperties{
        VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    imageProperties.pNext = &externalProperties;
    if (vkGetPhysicalDeviceImageFormatProperties2(
            physicalDevice,
            &imageInfo,
            &imageProperties) != VK_SUCCESS ||
        (externalProperties.externalMemoryProperties
             .externalMemoryFeatures &
         VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
        return abi::Result::BridgeUnsupported;
    }

    VkSemaphoreTypeCreateInfo semaphoreType{
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    semaphoreType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkPhysicalDeviceExternalSemaphoreInfo semaphoreInfo{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
    semaphoreInfo.pNext = &semaphoreType;
    semaphoreInfo.handleType =
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    VkExternalSemaphoreProperties semaphoreProperties{
        VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    vkGetPhysicalDeviceExternalSemaphoreProperties(
        physicalDevice, &semaphoreInfo, &semaphoreProperties);
    if ((semaphoreProperties.externalSemaphoreFeatures &
         VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) == 0) {
        return abi::Result::BridgeUnsupported;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan12Features enabled12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enabled12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceVulkan13Features enabled13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    enabled13.synchronization2 = VK_TRUE;
    enabled12.pNext = &enabled13;
    constexpr std::array requiredExtensions{
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
    };
    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.pNext = &enabled12;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount =
        static_cast<std::uint32_t>(requiredExtensions.size());
    createInfo.ppEnabledExtensionNames = requiredExtensions.data();
    if (vkCreateDevice(
            physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS) {
        return abi::Result::DeviceCreationFailed;
    }
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(
        physicalDevice, &memoryProperties);
    return abi::Result::Success;
}

abi::Result VulkanInteropBridge::Impl::ImportFence(
    const HANDLE handle) noexcept
{
    VkSemaphoreTypeCreateInfo type{
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = 0;
    VkSemaphoreCreateInfo createInfo{
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    createInfo.pNext = &type;
    if (vkCreateSemaphore(
            device, &createInfo, nullptr, &fenceSemaphore) != VK_SUCCESS) {
        return abi::Result::BridgeCreateFailed;
    }
    const auto importSemaphore =
        reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
            vkGetDeviceProcAddr(
                device, "vkImportSemaphoreWin32HandleKHR"));
    if (importSemaphore == nullptr) {
        return abi::Result::BridgeUnsupported;
    }
    VkImportSemaphoreWin32HandleInfoKHR importInfo{
        VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    importInfo.semaphore = fenceSemaphore;
    importInfo.handleType =
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    importInfo.handle = handle;
    if (importSemaphore(device, &importInfo) != VK_SUCCESS) {
        return abi::Result::BridgeCreateFailed;
    }
    return abi::Result::Success;
}

abi::Result VulkanInteropBridge::Impl::CreateCommandPool() noexcept
{
    VkCommandPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    if (vkCreateCommandPool(
            device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
        return abi::Result::BridgeCreateFailed;
    }
    std::array<VkCommandBuffer, abi::kBridgeImageCount> commands{};
    VkCommandBufferAllocateInfo allocation{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = commandPool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = imageCount;
    if (vkAllocateCommandBuffers(
            device, &allocation, commands.data()) != VK_SUCCESS) {
        return abi::Result::BridgeCreateFailed;
    }
    for (std::uint32_t index = 0; index < imageCount; ++index) {
        images[index].command = commands[index];
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(
                device,
                &fenceInfo,
                nullptr,
                &images[index].completion) != VK_SUCCESS) {
            return abi::Result::BridgeCreateFailed;
        }
    }
    return abi::Result::Success;
}

abi::Result VulkanInteropBridge::Impl::ImportImage(
    Image& resource,
    const HANDLE handle) noexcept
{
    VkExternalMemoryImageCreateInfo externalInfo{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    externalInfo.handleTypes =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.pNext = &externalInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(
            device, &imageInfo, nullptr, &resource.image) != VK_SUCCESS) {
        return abi::Result::BridgeCreateFailed;
    }

    const auto getHandleProperties =
        reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
            vkGetDeviceProcAddr(
                device, "vkGetMemoryWin32HandlePropertiesKHR"));
    if (getHandleProperties == nullptr) {
        return abi::Result::BridgeUnsupported;
    }
    VkMemoryWin32HandlePropertiesKHR handleProperties{
        VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    if (getHandleProperties(
            device,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
            handle,
            &handleProperties) != VK_SUCCESS) {
        return abi::Result::BridgeCreateFailed;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, resource.image, &requirements);
    const auto memoryType = FindMemoryType(
        memoryProperties,
        requirements.memoryTypeBits & handleProperties.memoryTypeBits,
        0,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryType == std::numeric_limits<std::uint32_t>::max()) {
        return abi::Result::BridgeCreateFailed;
    }

    VkMemoryDedicatedAllocateInfo dedicated{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = resource.image;
    VkImportMemoryWin32HandleInfoKHR importInfo{
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    importInfo.pNext = &dedicated;
    importInfo.handleType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    importInfo.handle = handle;
    VkMemoryAllocateInfo allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = &importInfo;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(
            device, &allocation, nullptr, &resource.memory) != VK_SUCCESS ||
        vkBindImageMemory(
            device, resource.image, resource.memory, 0) != VK_SUCCESS) {
        return abi::Result::BridgeCreateFailed;
    }
    return abi::Result::Success;
}

abi::Result VulkanInteropBridge::Impl::CreateUpload(
    Image& resource) noexcept
{
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = uploadSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(
            device, &bufferInfo, nullptr, &resource.upload) != VK_SUCCESS) {
        return abi::Result::BridgeCreateFailed;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, resource.upload, &requirements);
    const auto memoryType = FindMemoryType(
        memoryProperties,
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memoryType == std::numeric_limits<std::uint32_t>::max()) {
        return abi::Result::BridgeCreateFailed;
    }
    resource.uploadCoherent =
        (memoryProperties.memoryTypes[memoryType].propertyFlags &
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    VkMemoryAllocateInfo allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(
            device,
            &allocation,
            nullptr,
            &resource.uploadMemory) != VK_SUCCESS ||
        vkBindBufferMemory(
            device,
            resource.upload,
            resource.uploadMemory,
            0) != VK_SUCCESS ||
        vkMapMemory(
            device,
            resource.uploadMemory,
            0,
            uploadSize,
            0,
            &resource.mapped) != VK_SUCCESS) {
        return abi::Result::BridgeCreateFailed;
    }

    auto* pixels = static_cast<std::uint8_t*>(resource.mapped);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto top = y < height / 2;
            const auto left = x < width / 2;
            std::array<std::uint8_t, 4> color{};
            if (top && left) {
                color = {255, 0, 0, 255};
            } else if (top) {
                color = {0, 255, 0, 255};
            } else if (left) {
                color = {0, 0, 255, 255};
            } else {
                color = {255, 255, 0, 255};
            }
            std::memcpy(
                pixels +
                    (static_cast<std::size_t>(y) * width + x) * 4,
                color.data(),
                color.size());
        }
    }
    return FlushUpload(resource);
}

abi::Result VulkanInteropBridge::Impl::FlushUpload(
    const Image& resource) noexcept
{
    if (resource.uploadCoherent) {
        return abi::Result::Success;
    }
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = resource.uploadMemory;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    return vkFlushMappedMemoryRanges(device, 1, &range) == VK_SUCCESS
        ? abi::Result::Success
        : abi::Result::BridgeSubmitFailed;
}

VulkanInteropBridge::VulkanInteropBridge()
    : impl_(std::make_unique<Impl>())
{}

VulkanInteropBridge::~VulkanInteropBridge()
{
    impl_->Reset();
}

abi::Result VulkanInteropBridge::Create(
    const abi::HostCallbacksV1& callbacks,
    const abi::BridgeCreateRequestV1& request,
    abi::BridgeStatusV1& status) noexcept
{
    InitializeStatus(status, abi::Result::InternalFailure);
    if (impl_->ready) {
        impl_->FillStatus(status, abi::Result::BridgeAlreadyCreated);
        return abi::Result::BridgeAlreadyCreated;
    }
    if (request.structSize < abi::kBridgeCreateRequestV1RequiredSize ||
        request.epoch == 0 || request.width == 0 ||
        request.height == 0 ||
        request.format != abi::BridgeFormat::R8G8B8A8Unorm ||
        request.imageCount != abi::kBridgeImageCount ||
        request.fenceHandle == 0) {
        status.result = abi::Result::InvalidArgument;
        return abi::Result::InvalidArgument;
    }
    for (const auto handle : request.imageHandles) {
        if (handle == 0) {
            status.result = abi::Result::InvalidArgument;
            return abi::Result::InvalidArgument;
        }
    }

    try {
        impl_->callbacks = callbacks;
        impl_->epoch = request.epoch;
        impl_->width = request.width;
        impl_->height = request.height;
        impl_->imageCount = request.imageCount;
        const auto pixelCount =
            static_cast<std::uint64_t>(request.width) * request.height;
        if (pixelCount >
            std::numeric_limits<VkDeviceSize>::max() / 4) {
            impl_->FillStatus(status, abi::Result::InvalidArgument);
            return abi::Result::InvalidArgument;
        }
        impl_->uploadSize = static_cast<VkDeviceSize>(pixelCount * 4);

        auto result = impl_->CreateInstance(
            (request.flags & abi::BridgeCreateValidation) != 0);
        if (result == abi::Result::Success) {
            result = impl_->CreateDevice(request.adapterLuid);
        }
        if (result == abi::Result::Success) {
            result = impl_->ImportFence(reinterpret_cast<HANDLE>(
                request.fenceHandle));
        }
        if (result == abi::Result::Success) {
            result = impl_->CreateCommandPool();
        }
        for (std::uint32_t index = 0;
             result == abi::Result::Success &&
                 index < request.imageCount;
             ++index) {
            result = impl_->ImportImage(
                impl_->images[index],
                reinterpret_cast<HANDLE>(request.imageHandles[index]));
            if (result == abi::Result::Success) {
                result = impl_->CreateUpload(impl_->images[index]);
            }
        }
        if (result != abi::Result::Success) {
            impl_->FillStatus(status, result);
            impl_->Reset();
            return result;
        }
        impl_->ready = true;
        impl_->FillStatus(status, abi::Result::Success);
        return abi::Result::Success;
    } catch (...) {
        impl_->FillStatus(status, abi::Result::InternalFailure);
        impl_->Reset();
        return abi::Result::InternalFailure;
    }
}

abi::Result VulkanInteropBridge::SubmitPattern(
    const abi::BridgePatternRequestV1& request,
    abi::BridgeStatusV1& status) noexcept
{
    if (!impl_->ready) {
        InitializeStatus(status, abi::Result::BridgeNotCreated);
        return abi::Result::BridgeNotCreated;
    }
    if (request.structSize < abi::kBridgePatternRequestV1RequiredSize ||
        request.imageIndex >= impl_->imageCount ||
        request.releaseValue == 0 ||
        request.readyValue != request.releaseValue + 1 ||
        request.releaseValue <= impl_->lastReady) {
        impl_->FillStatus(status, abi::Result::InvalidArgument);
        return abi::Result::InvalidArgument;
    }
    if (request.epoch != impl_->epoch) {
        impl_->FillStatus(status, abi::Result::BridgeStaleEpoch);
        return abi::Result::BridgeStaleEpoch;
    }

    auto& resource = impl_->images[request.imageIndex];
    if (resource.lastReady != 0) {
        if (vkGetFenceStatus(
                impl_->device, resource.completion) != VK_SUCCESS ||
            vkResetFences(
                impl_->device, 1, &resource.completion) != VK_SUCCESS) {
            impl_->FillStatus(status, abi::Result::BridgeSubmitFailed);
            return abi::Result::BridgeSubmitFailed;
        }
    }
    auto* encoded = static_cast<std::uint8_t*>(resource.mapped);
    // Rendered pixels replace the built-in pattern when the caller supplies
    // them, which is what lets the bridge present the mirrored scene rather
    // than only proving the transport works. A caller that fills just the
    // original prefix still gets the pattern.
    const auto pixelBytes = static_cast<std::uint64_t>(impl_->width) *
        impl_->height * 4ull;
    const auto hasPixels =
        request.structSize >= abi::kBridgePatternRequestV1PixelRequiredSize &&
        request.pixelData != 0 && request.pixelSize != 0;
    if (hasPixels) {
        if (request.pixelSize != pixelBytes) {
            impl_->FillStatus(status, abi::Result::InvalidArgument);
            return abi::Result::InvalidArgument;
        }
        std::memcpy(encoded,
            reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(request.pixelData)),
            static_cast<std::size_t>(pixelBytes));
    } else {
        encoded[0] = static_cast<std::uint8_t>(request.frameIndex);
        encoded[1] = static_cast<std::uint8_t>(request.frameIndex >> 8);
        encoded[2] = static_cast<std::uint8_t>(request.frameIndex >> 16);
        encoded[3] = 255;
    }
    auto result = impl_->FlushUpload(resource);
    if (result != abi::Result::Success) {
        impl_->FillStatus(status, result);
        return result;
    }

    if (vkResetCommandBuffer(resource.command, 0) != VK_SUCCESS) {
        impl_->FillStatus(status, abi::Result::BridgeSubmitFailed);
        return abi::Result::BridgeSubmitFailed;
    }
    VkCommandBufferBeginInfo beginInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(resource.command, &beginInfo) != VK_SUCCESS) {
        impl_->FillStatus(status, abi::Result::BridgeSubmitFailed);
        return abi::Result::BridgeSubmitFailed;
    }

    VkImageMemoryBarrier2 acquire{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    acquire.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    acquire.srcAccessMask = VK_ACCESS_2_NONE;
    acquire.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    acquire.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    acquire.oldLayout = resource.initialized
        ? VK_IMAGE_LAYOUT_GENERAL
        : VK_IMAGE_LAYOUT_UNDEFINED;
    acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    acquire.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    acquire.image = resource.image;
    acquire.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    acquire.subresourceRange.levelCount = 1;
    acquire.subresourceRange.layerCount = 1;
    VkDependencyInfo acquireDependency{
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    acquireDependency.imageMemoryBarrierCount = 1;
    acquireDependency.pImageMemoryBarriers = &acquire;
    vkCmdPipelineBarrier2(resource.command, &acquireDependency);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {impl_->width, impl_->height, 1};
    vkCmdCopyBufferToImage(
        resource.command,
        resource.upload,
        resource.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy);

    VkImageMemoryBarrier2 release{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    release.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    release.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    release.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    release.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    release.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    release.image = resource.image;
    release.subresourceRange = acquire.subresourceRange;
    VkDependencyInfo releaseDependency{
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    releaseDependency.imageMemoryBarrierCount = 1;
    releaseDependency.pImageMemoryBarriers = &release;
    vkCmdPipelineBarrier2(resource.command, &releaseDependency);
    if (vkEndCommandBuffer(resource.command) != VK_SUCCESS) {
        impl_->FillStatus(status, abi::Result::BridgeSubmitFailed);
        return abi::Result::BridgeSubmitFailed;
    }

    VkSemaphoreSubmitInfo wait{
        VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = impl_->fenceSemaphore;
    wait.value = request.releaseValue;
    wait.stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    VkCommandBufferSubmitInfo command{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    command.commandBuffer = resource.command;
    VkSemaphoreSubmitInfo signal{
        VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = impl_->fenceSemaphore;
    signal.value = request.readyValue;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    if (vkQueueSubmit2(
            impl_->queue, 1, &submit, resource.completion) != VK_SUCCESS) {
        impl_->FillStatus(status, abi::Result::BridgeSubmitFailed);
        return abi::Result::BridgeSubmitFailed;
    }
    resource.initialized = true;
    resource.lastReady = request.readyValue;
    impl_->lastRelease = request.releaseValue;
    impl_->lastReady = request.readyValue;
    ++impl_->submissions;
    impl_->FillStatus(status, abi::Result::Success);
    return abi::Result::Success;
}

abi::Result VulkanInteropBridge::Destroy(
    abi::BridgeStatusV1& status) noexcept
{
    if (!impl_->ready) {
        InitializeStatus(status, abi::Result::BridgeNotCreated);
        return abi::Result::BridgeNotCreated;
    }
    impl_->FillStatus(status, abi::Result::Success);
    impl_->Reset();
    return abi::Result::Success;
}

}
