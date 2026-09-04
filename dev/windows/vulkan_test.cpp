#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR 1
#include <windows.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {
    constexpr wchar_t windowClass[] = L"Shitty.Vulkan.Test";

    LRESULT CALLBACK windowProcedure(
        HWND window,
        UINT message,
        WPARAM wparam,
        LPARAM lparam
    ) {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    struct Window final {
        bool initialize() {
            instance = GetModuleHandleW(nullptr);
            WNDCLASSEXW description{};
            description.cbSize = sizeof(description);
            description.lpfnWndProc = windowProcedure;
            description.hInstance = instance;
            description.lpszClassName = windowClass;
            atom = RegisterClassExW(&description);
            if (atom == 0) {
                return false;
            }
            handle = CreateWindowExW(
                0,
                windowClass,
                L"Shitty Vulkan test",
                WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                320,
                200,
                nullptr,
                nullptr,
                instance,
                nullptr
            );
            return handle != nullptr;
        }

        bool show(int state) const {
            ShowWindow(handle, state);
            UpdateWindow(handle);
            return state == SW_MINIMIZE
                ? IsIconic(handle) != 0
                : IsWindowVisible(handle) != 0;
        }

        bool resize(int width, int height) const {
            return SetWindowPos(
                handle,
                nullptr,
                0,
                0,
                width,
                height,
                SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER
            ) != 0;
        }

        bool dispatch() const {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            return true;
        }

        ~Window() noexcept {
            if (handle != nullptr) {
                DestroyWindow(handle);
            }
            if (atom != 0) {
                UnregisterClassW(windowClass, instance);
            }
        }

        HINSTANCE instance = nullptr;
        HWND handle = nullptr;
        ATOM atom = 0;
    };

    struct Vulkan final {
        struct FrameResources final {
            ~FrameResources() noexcept {
                if (commands != VK_NULL_HANDLE) {
                    vkFreeCommandBuffers(device, pool, 1, &commands);
                }
                if (fence != VK_NULL_HANDLE) {
                    vkDestroyFence(device, fence, nullptr);
                }
                if (finished != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device, finished, nullptr);
                }
                if (available != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device, available, nullptr);
                }
                if (readback != VK_NULL_HANDLE) {
                    vkDestroyBuffer(device, readback, nullptr);
                }
                if (readbackMemory != VK_NULL_HANDLE) {
                    vkFreeMemory(device, readbackMemory, nullptr);
                }
            }

            VkDevice device = VK_NULL_HANDLE;
            VkCommandPool pool = VK_NULL_HANDLE;
            VkSemaphore available = VK_NULL_HANDLE;
            VkSemaphore finished = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            VkCommandBuffer commands = VK_NULL_HANDLE;
            VkBuffer readback = VK_NULL_HANDLE;
            VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
        };

        bool initialize(const Window& window) {
            const char* extensions[] = {
                VK_KHR_SURFACE_EXTENSION_NAME,
                VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
            };
            VkApplicationInfo application{};
            application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            application.pApplicationName = "shitty-vulkan-test";
            application.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo instanceInfo{};
            instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            instanceInfo.pApplicationInfo = &application;
            instanceInfo.enabledExtensionCount = 2;
            instanceInfo.ppEnabledExtensionNames = extensions;
            if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
                return false;
            }

            VkWin32SurfaceCreateInfoKHR surfaceInfo{};
            surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
            surfaceInfo.hinstance = window.instance;
            surfaceInfo.hwnd = window.handle;
            if (vkCreateWin32SurfaceKHR(
                    instance,
                    &surfaceInfo,
                    nullptr,
                    &surface
                ) != VK_SUCCESS) {
                return false;
            }

            uint32_t count = 0;
            if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS
                    || count == 0) {
                return false;
            }
            std::vector<VkPhysicalDevice> devices(count);
            if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) {
                return false;
            }
            for (const VkPhysicalDevice candidate : devices) {
                uint32_t families = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
                std::vector<VkQueueFamilyProperties> properties(families);
                vkGetPhysicalDeviceQueueFamilyProperties(
                    candidate,
                    &families,
                    properties.data()
                );
                for (uint32_t family = 0; family != families; ++family) {
                    VkBool32 present = VK_FALSE;
                    if (vkGetPhysicalDeviceSurfaceSupportKHR(
                            candidate,
                            family,
                            surface,
                            &present
                        ) == VK_SUCCESS
                            && present
                            && (properties[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                        physicalDevice = candidate;
                        queueFamily = family;
                        VkPhysicalDeviceProperties deviceProperties{};
                        vkGetPhysicalDeviceProperties(candidate, &deviceProperties);
                        std::printf("device=%s\n", deviceProperties.deviceName);
                        return createDevice();
                    }
                }
            }
            return false;
        }

        bool createDevice() {
            constexpr float priority = 1.0f;
            VkDeviceQueueCreateInfo queueInfo{};
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = queueFamily;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &priority;
            const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
            VkDeviceCreateInfo deviceInfo{};
            deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            deviceInfo.queueCreateInfoCount = 1;
            deviceInfo.pQueueCreateInfos = &queueInfo;
            deviceInfo.enabledExtensionCount = 1;
            deviceInfo.ppEnabledExtensionNames = extensions;
            if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
                return false;
            }
            vkGetDeviceQueue(device, queueFamily, 0, &queue);
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = queueFamily;
            return vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool)
                == VK_SUCCESS;
        }

        uint32_t memoryType(
            uint32_t allowed,
            VkMemoryPropertyFlags required
        ) const {
            VkPhysicalDeviceMemoryProperties properties{};
            vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
            for (uint32_t index = 0; index != properties.memoryTypeCount; ++index) {
                if ((allowed & (1u << index)) != 0
                        && (properties.memoryTypes[index].propertyFlags & required)
                            == required) {
                    return index;
                }
            }
            return UINT32_MAX;
        }

        bool createSwapchain(const Window& window) {
            if (device == VK_NULL_HANDLE || IsIconic(window.handle) != 0) {
                return false;
            }
            vkDeviceWaitIdle(device);
            if (swapchain != VK_NULL_HANDLE) {
                vkDestroySwapchainKHR(device, swapchain, nullptr);
                swapchain = VK_NULL_HANDLE;
            }
            VkSurfaceCapabilitiesKHR capabilities{};
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                    physicalDevice,
                    surface,
                    &capabilities
                ) != VK_SUCCESS) {
                return false;
            }
            uint32_t formatCount = 0;
            if (vkGetPhysicalDeviceSurfaceFormatsKHR(
                    physicalDevice,
                    surface,
                    &formatCount,
                    nullptr
                ) != VK_SUCCESS || formatCount == 0) {
                return false;
            }
            std::vector<VkSurfaceFormatKHR> formats(formatCount);
            if (vkGetPhysicalDeviceSurfaceFormatsKHR(
                    physicalDevice,
                    surface,
                    &formatCount,
                    formats.data()
                ) != VK_SUCCESS) {
                return false;
            }
            const auto selected = std::find_if(
                formats.begin(),
                formats.end(),
                [](const VkSurfaceFormatKHR& candidate) {
                    return candidate.format == VK_FORMAT_B8G8R8A8_UNORM
                        && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                }
            );
            surfaceFormat = selected == formats.end() ? formats.front() : *selected;
            if (capabilities.currentExtent.width != UINT32_MAX) {
                extent = capabilities.currentExtent;
            } else {
                RECT client{};
                GetClientRect(window.handle, &client);
                extent = {
                    std::clamp(
                        static_cast<uint32_t>(std::max(0L, client.right - client.left)),
                        capabilities.minImageExtent.width,
                        capabilities.maxImageExtent.width
                    ),
                    std::clamp(
                        static_cast<uint32_t>(std::max(0L, client.bottom - client.top)),
                        capabilities.minImageExtent.height,
                        capabilities.maxImageExtent.height
                    ),
                };
            }
            if (extent.width == 0 || extent.height == 0
                    || (capabilities.supportedUsageFlags
                        & (VK_IMAGE_USAGE_TRANSFER_DST_BIT
                            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
                        != (VK_IMAGE_USAGE_TRANSFER_DST_BIT
                            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
                return false;
            }
            uint32_t imageCount = capabilities.minImageCount + 1;
            if (capabilities.maxImageCount != 0) {
                imageCount = std::min(imageCount, capabilities.maxImageCount);
            }
            VkSwapchainCreateInfoKHR swapchainInfo{};
            swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
            swapchainInfo.surface = surface;
            swapchainInfo.minImageCount = imageCount;
            swapchainInfo.imageFormat = surfaceFormat.format;
            swapchainInfo.imageColorSpace = surfaceFormat.colorSpace;
            swapchainInfo.imageExtent = extent;
            swapchainInfo.imageArrayLayers = 1;
            swapchainInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            swapchainInfo.preTransform = capabilities.currentTransform;
            swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            swapchainInfo.clipped = VK_TRUE;
            if (vkCreateSwapchainKHR(
                    device,
                    &swapchainInfo,
                    nullptr,
                    &swapchain
                ) != VK_SUCCESS) {
                return false;
            }
            uint32_t actualCount = 0;
            if (vkGetSwapchainImagesKHR(device, swapchain, &actualCount, nullptr)
                    != VK_SUCCESS || actualCount == 0) {
                return false;
            }
            images.resize(actualCount);
            return vkGetSwapchainImagesKHR(
                device,
                swapchain,
                &actualCount,
                images.data()
            ) == VK_SUCCESS;
        }

        bool present() {
            if (swapchain == VK_NULL_HANDLE) {
                return false;
            }
            FrameResources frame;
            frame.device = device;
            frame.pool = commandPool;
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = 4;
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device, &bufferInfo, nullptr, &frame.readback)
                    != VK_SUCCESS) {
                return false;
            }
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(device, frame.readback, &requirements);
            const uint32_t type = memoryType(
                requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
            );
            if (type == UINT32_MAX) {
                return false;
            }
            VkMemoryAllocateInfo memoryInfo{};
            memoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            memoryInfo.allocationSize = requirements.size;
            memoryInfo.memoryTypeIndex = type;
            if (vkAllocateMemory(
                    device,
                    &memoryInfo,
                    nullptr,
                    &frame.readbackMemory
                ) != VK_SUCCESS
                    || vkBindBufferMemory(
                        device,
                        frame.readback,
                        frame.readbackMemory,
                        0
                    ) != VK_SUCCESS) {
                return false;
            }
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frame.available)
                    != VK_SUCCESS
                    || vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frame.finished)
                    != VK_SUCCESS) {
                return false;
            }
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (vkCreateFence(device, &fenceInfo, nullptr, &frame.fence) != VK_SUCCESS) {
                return false;
            }
            uint32_t imageIndex = 0;
            const VkResult acquired = vkAcquireNextImageKHR(
                device,
                swapchain,
                UINT64_MAX,
                frame.available,
                VK_NULL_HANDLE,
                &imageIndex
            );
            if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
                return false;
            }
            VkCommandBufferAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocateInfo.commandPool = commandPool;
            allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocateInfo.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(device, &allocateInfo, &frame.commands)
                    != VK_SUCCESS) {
                return false;
            }
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkBeginCommandBuffer(frame.commands, &beginInfo) != VK_SUCCESS) {
                return false;
            }
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = images[imageIndex];
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(
                frame.commands,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0,
                0,
                nullptr,
                0,
                nullptr,
                1,
                &barrier
            );
            VkClearColorValue color{};
            color.float32[0] = 0.06f;
            color.float32[1] = 0.2f;
            color.float32[2] = 0.4f;
            color.float32[3] = 1.0f;
            vkCmdClearColorImage(
                frame.commands,
                images[imageIndex],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                &color,
                1,
                &barrier.subresourceRange
            );
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            vkCmdPipelineBarrier(
                frame.commands,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0,
                0,
                nullptr,
                0,
                nullptr,
                1,
                &barrier
            );
            VkBufferImageCopy copy{};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {1, 1, 1};
            vkCmdCopyImageToBuffer(
                frame.commands,
                images[imageIndex],
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                frame.readback,
                1,
                &copy
            );
            VkBufferMemoryBarrier hostRead{};
            hostRead.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            hostRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hostRead.buffer = frame.readback;
            hostRead.size = 4;
            vkCmdPipelineBarrier(
                frame.commands,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_HOST_BIT,
                0,
                0,
                nullptr,
                1,
                &hostRead,
                0,
                nullptr
            );
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = 0;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(
                frame.commands,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0,
                0,
                nullptr,
                0,
                nullptr,
                1,
                &barrier
            );
            if (vkEndCommandBuffer(frame.commands) != VK_SUCCESS) {
                return false;
            }
            constexpr VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &frame.available;
            submit.pWaitDstStageMask = &waitStage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &frame.commands;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &frame.finished;
            if (vkQueueSubmit(queue, 1, &submit, frame.fence) != VK_SUCCESS) {
                return false;
            }
            VkPresentInfoKHR presentation{};
            presentation.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            presentation.waitSemaphoreCount = 1;
            presentation.pWaitSemaphores = &frame.finished;
            presentation.swapchainCount = 1;
            presentation.pSwapchains = &swapchain;
            presentation.pImageIndices = &imageIndex;
            const VkResult presented = vkQueuePresentKHR(queue, &presentation);
            const bool complete = (presented == VK_SUCCESS
                    || presented == VK_SUBOPTIMAL_KHR)
                && vkWaitForFences(device, 1, &frame.fence, VK_TRUE, UINT64_MAX)
                    == VK_SUCCESS;
            vkQueueWaitIdle(queue);
            void* mapped = nullptr;
            if (!complete || vkMapMemory(
                    device,
                    frame.readbackMemory,
                    0,
                    4,
                    0,
                    &mapped
                ) != VK_SUCCESS) {
                return false;
            }
            const auto* pixel = static_cast<const uint8_t*>(mapped);
            const bool bgra = surfaceFormat.format == VK_FORMAT_B8G8R8A8_UNORM
                || surfaceFormat.format == VK_FORMAT_B8G8R8A8_SRGB;
            const uint8_t expected[4] = {
                static_cast<uint8_t>(bgra ? 102 : 15),
                51,
                static_cast<uint8_t>(bgra ? 15 : 102),
                255,
            };
            bool matches = true;
            for (size_t index = 0; index != 4; ++index) {
                const int difference = static_cast<int>(pixel[index])
                    - static_cast<int>(expected[index]);
                matches = matches && difference >= -2 && difference <= 2;
            }
            std::printf(
                "pixel=%u,%u,%u,%u\n",
                pixel[0],
                pixel[1],
                pixel[2],
                pixel[3]
            );
            vkUnmapMemory(device, frame.readbackMemory);
            return matches;
        }

        ~Vulkan() noexcept {
            if (device != VK_NULL_HANDLE) {
                vkDeviceWaitIdle(device);
            }
            if (swapchain != VK_NULL_HANDLE) {
                vkDestroySwapchainKHR(device, swapchain, nullptr);
            }
            if (commandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device, commandPool, nullptr);
            }
            if (device != VK_NULL_HANDLE) {
                vkDestroyDevice(device, nullptr);
            }
            if (surface != VK_NULL_HANDLE) {
                vkDestroySurfaceKHR(instance, surface, nullptr);
            }
            if (instance != VK_NULL_HANDLE) {
                vkDestroyInstance(instance, nullptr);
            }
        }

        VkInstance instance = VK_NULL_HANDLE;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkSurfaceFormatKHR surfaceFormat{};
        VkExtent2D extent{};
        std::vector<VkImage> images;
    };
}

int main() {
    Window window;
    if (!window.initialize()) {
        return 1;
    }
    if (!window.show(SW_SHOW)) {
        return 2;
    }
    Vulkan vulkan;
    if (!vulkan.initialize(window)) {
        return 3;
    }
    if (!vulkan.createSwapchain(window) || !vulkan.present()) {
        return 4;
    }
    if (!window.resize(480, 300)) {
        return 5;
    }
    window.dispatch();
    if (!vulkan.createSwapchain(window) || !vulkan.present()) {
        return 6;
    }
    if (!window.show(SW_MINIMIZE)) {
        return 7;
    }
    window.dispatch();
    if (!window.show(SW_RESTORE)) {
        return 8;
    }
    window.dispatch();
    if (!vulkan.createSwapchain(window) || !vulkan.present()) {
        return 9;
    }
    std::printf("frames=3 extent=%ux%u\n", vulkan.extent.width, vulkan.extent.height);
    return 0;
}
