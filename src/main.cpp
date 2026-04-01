#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#if defined(RENDER_HAS_VULKAN_BACKEND)
#define GLFW_INCLUDE_VULKAN
#include <render/vulkan/intf_device_vk.h>
#else
#define GLFW_INCLUDE_NONE
#endif

#include <GLFW/glfw3.h>

#include <memory>

#include <core/io/intf_file_manager.h>
#include <core/log.h>
#include <core/os/platform_create_info.h>
#include <core/plugin/intf_plugin_register.h>
#include <render/device/intf_device.h>

#include "application_config.h"
#include "application_factory.h"
#include "application_interface.h"

namespace {
    uint64_t CreateSurface(std::unique_ptr<IApplication>& app, GLFWwindow* windowHandle, RENDER_NS::IDevice* device)
    {
        constexpr uint64_t invalidSurfaceHandle = 0;
#if RENDER_HAS_VULKAN_BACKEND
        if (device->GetBackendType() != RENDER_NS::DeviceBackendType::VULKAN) {
            app->OnStop();
            return invalidSurfaceHandle;
        }
        const auto& platformData = static_cast<const RENDER_NS::DevicePlatformDataVk&>(device->GetPlatformData());
        VkSurfaceKHR surface { VK_NULL_HANDLE };
        if (glfwCreateWindowSurface(platformData.instance, windowHandle, nullptr, &surface) != VK_SUCCESS) {
            app->OnStop();
            return invalidSurfaceHandle;
        }
        return reinterpret_cast<uint64_t>(surface);
#else
        app->OnStop();
        return invalidSurfaceHandle;
#endif
    }

    void DestroySurface(RENDER_NS::IDevice* device, uint64_t surfaceHandle)
    {
#if RENDER_HAS_VULKAN_BACKEND
        if (surfaceHandle == 0) {
            return;
        }
        const auto& platformData = static_cast<const RENDER_NS::DevicePlatformDataVk&>(device->GetPlatformData());
        vkDestroySurfaceKHR(platformData.instance, reinterpret_cast<VkSurfaceKHR>(surfaceHandle), nullptr);
#else
        (void)device;
        (void)surfaceHandle;
#endif
    }
}

void RegisterAppPaths(CORE_NS::IEngine& engine)
{
    auto& fileManager = engine.GetFileManager();

    const BASE_NS::string appDirectory = "file://app/";
    fileManager.RegisterPath("app", appDirectory, true);
    if (fileManager.OpenDirectory(appDirectory) == nullptr) {
        const auto _ = fileManager.CreateDirectory(appDirectory);
    }

    const BASE_NS::string cacheDirectory = "app://cache/";
    fileManager.RegisterPath("cache", cacheDirectory, true);
    if (fileManager.OpenDirectory(cacheDirectory) == nullptr) {
        const auto _ = fileManager.CreateDirectory(cacheDirectory);
    }

    const BASE_NS::string sharedDirectory = "app://shared/";
    fileManager.RegisterPath("shared", sharedDirectory, true);
    if (fileManager.OpenDirectory(sharedDirectory) == nullptr) {
        const auto _ = fileManager.CreateDirectory(sharedDirectory);
    }

    const BASE_NS::string assetsDirectory = "app://assets/";
    fileManager.RegisterPath("assets", assetsDirectory, true);
    if (fileManager.OpenDirectory(assetsDirectory) == nullptr) {
        const auto _ = fileManager.CreateDirectory(assetsDirectory);
    }
}

int main() {
    constexpr int width = 1600;
    constexpr int height = 900;

    const CORE_NS::PlatformCreateInfo platformCreateInfo{};
    CORE_NS::CreatePluginRegistry(platformCreateInfo);

    if (!glfwInit()) {
        return -1;
    }

#if RENDER_HAS_VULKAN_BACKEND
    if (!glfwVulkanSupported()) {
        glfwTerminate();
        return -1;
    }
#endif

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(width, height, applicationName, nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return -1;
    }

    std::unique_ptr<IApplication> app(createApplication());
    RENDER_NS::IDevice* device = app->OnInit(platformCreateInfo);

    RENDER_NS::SwapchainCreateInfo swapchainCreateInfo;
    swapchainCreateInfo.surfaceHandle = CreateSurface(app, window, device);
    swapchainCreateInfo.swapchainFlags = RENDER_NS::SwapchainFlagBits::CORE_SWAPCHAIN_COLOR_BUFFER_BIT |
                                         RENDER_NS::SwapchainFlagBits::CORE_SWAPCHAIN_DEPTH_BUFFER_BIT |
                                         RENDER_NS::SwapchainFlagBits::CORE_SWAPCHAIN_VSYNC_BIT |
                                         RENDER_NS::SwapchainFlagBits::CORE_SWAPCHAIN_SRGB_BIT;
    
    app->OnWindowUpdate(swapchainCreateInfo, width, height);
    app->OnStart();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        app->OnFrame();
    }

    app->OnStop();
    app->OnWindowDestroy();
    DestroySurface(device, swapchainCreateInfo.surfaceHandle);
    app.reset();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}