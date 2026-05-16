#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <cstdlib>
#include <memory>
#include <iostream>

#include <core/io/intf_file_manager.h>
#include <core/log.h>
#include <core/os/platform_create_info.h>
#include <core/plugin/intf_plugin_register.h>
#include <render/device/intf_device.h>
#if RENDER_HAS_VULKAN_BACKEND
#include <render/vulkan/intf_device_vk.h>
#endif

#include "application_config.h"
#include "application_factory.h"
#include "application_interface.h"

namespace {
    struct RenderDocApi {
        void (*GetAPIVersion)(int* major, int* minor, int* patch);
        void (*SetCaptureOptionU32)(int option, uint32_t value);
        void (*SetCaptureOptionF32)(int option, float value);
        uint32_t (*GetCaptureOptionU32)(int option);
        float (*GetCaptureOptionF32)(int option);
        void (*SetFocusToggleKeys)(void* keys, int num);
        void (*SetCaptureKeys)(void* keys, int num);
        uint32_t (*GetOverlayBits)();
        void (*MaskOverlayBits)(uint32_t andMask, uint32_t orMask);
        void (*RemoveHooks)();
        void (*UnloadCrashHandler)();
        void (*SetCaptureFilePathTemplate)(const char* pathTemplate);
        const char* (*GetCaptureFilePathTemplate)();
        uint32_t (*GetNumCaptures)();
        uint32_t (*GetCapture)(uint32_t idx, char* filename, uint32_t* pathLength, uint64_t* timestamp);
        void (*TriggerCapture)();
        uint32_t (*IsTargetControlConnected)();
        void (*LaunchReplayUI)(uint32_t connectTargetControl, const char* cmdline);
        void (*SetActiveWindow)(void* device, void* wndHandle);
        void (*StartFrameCapture)(void* device, void* wndHandle);
        uint32_t (*IsFrameCapturing)();
        uint32_t (*EndFrameCapture)(void* device, void* wndHandle);
    };

    RenderDocApi* LoadRenderDocApi()
    {
#ifdef _WIN32
        HMODULE module = GetModuleHandleA("renderdoc.dll");
        if (!module) {
            return nullptr;
        }

        using GetApiFn = int (*)(int version, void** outApiPointers);
        auto getApi = reinterpret_cast<GetApiFn>(GetProcAddress(module, "RENDERDOC_GetAPI"));
        if (!getApi) {
            return nullptr;
        }

        void* api = nullptr;
        constexpr int renderDocApiVersion = 10600; // eRENDERDOC_API_Version_1_6_0
        if (getApi(renderDocApiVersion, &api) != 1) {
            return nullptr;
        }
        return static_cast<RenderDocApi*>(api);
#else
        return nullptr;
#endif
    }

    uint32_t ReadEnvUInt(const char* name, uint32_t defaultValue)
    {
        const char* value = std::getenv(name);
        if (!value || value[0] == '\0') {
            return defaultValue;
        }

        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if ((end == value) || (parsed == 0UL)) {
            return defaultValue;
        }
        return static_cast<uint32_t>(parsed);
    }

    bool ReadOptionalEnvUInt(const char* name, uint32_t& outValue)
    {
        const char* value = std::getenv(name);
        if (!value || value[0] == '\0') {
            return false;
        }

        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if ((end == value) || (parsed == 0UL)) {
            return false;
        }

        outValue = static_cast<uint32_t>(parsed);
        return true;
    }

    void ConfigureRenderDocCapturePath(RenderDocApi* renderDoc)
    {
        if (!renderDoc) {
            return;
        }

        const char* capturePath = std::getenv("LUME_RENDERDOC_CAPTURE_PATH");
        if (capturePath && capturePath[0] != '\0') {
            renderDoc->SetCaptureFilePathTemplate(capturePath);
        }
    }

    void* GetRenderDocDevicePointer(RENDER_NS::IDevice* device)
    {
#if RENDER_HAS_VULKAN_BACKEND
        if (device && device->GetBackendType() == RENDER_NS::DeviceBackendType::VULKAN) {
            const auto& platformData = static_cast<const RENDER_NS::DevicePlatformDataVk&>(device->GetPlatformData());
            return reinterpret_cast<void*>(platformData.device);
        }
#else
        (void)device;
#endif
        return nullptr;
    }

    void PrintLatestRenderDocCapture(RenderDocApi* renderDoc)
    {
        if (!renderDoc) {
            return;
        }

        const uint32_t captureCount = renderDoc->GetNumCaptures();
        std::cout << "RenderDoc: captures recorded=" << captureCount << std::endl;
        if (captureCount == 0u) {
            return;
        }

        char filename[1024] {};
        uint32_t pathLength = static_cast<uint32_t>(sizeof(filename));
        uint64_t timestamp = 0;
        if (renderDoc->GetCapture(captureCount - 1u, filename, &pathLength, &timestamp) == 1u) {
            std::cout << "RenderDoc: latest capture " << filename << std::endl;
        }
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
    const uint32_t width = ReadEnvUInt("LUME_OFFSCREEN_WIDTH", 1024);
    const uint32_t height = ReadEnvUInt("LUME_OFFSCREEN_HEIGHT", 1024);
    const uint32_t frameCount = ReadEnvUInt("LUME_FRAME_COUNT", 100);

    const CORE_NS::PlatformCreateInfo platformCreateInfo{};
    CORE_NS::CreatePluginRegistry(platformCreateInfo);

    RenderDocApi* renderDoc = LoadRenderDocApi();
    ConfigureRenderDocCapturePath(renderDoc);
    uint32_t renderDocCaptureFrame = 0;
    const bool captureFrameConfigured = ReadOptionalEnvUInt("LUME_RENDERDOC_CAPTURE_FRAME", renderDocCaptureFrame);
    if (renderDoc && !captureFrameConfigured) {
        renderDocCaptureFrame = 20;
    }
    if (renderDoc) {
        int major = 0;
        int minor = 0;
        int patch = 0;
        renderDoc->GetAPIVersion(&major, &minor, &patch);
        std::cout << "RenderDoc API detected: " << major << "." << minor << "." << patch << std::endl;
        std::cout << "RenderDoc: capture frame=" << renderDocCaptureFrame
                  << (captureFrameConfigured ? " (from LUME_RENDERDOC_CAPTURE_FRAME)" : " (default)") << std::endl;
        if (renderDocCaptureFrame > frameCount) {
            std::cout << "RenderDoc: capture frame is greater than LUME_FRAME_COUNT; no capture will run" << std::endl;
        }
    } else if (renderDocCaptureFrame != 0u) {
        std::cout << "RenderDoc capture requested, but renderdoc.dll is not loaded. Launch from RenderDoc." << std::endl;
    }

    std::unique_ptr<IApplication> app(createApplication());
    RENDER_NS::IDevice* device = app->OnInit(platformCreateInfo);
    void* renderDocDevice = GetRenderDocDevicePointer(device);
    
    app->OnRenderTargetUpdate(static_cast<int>(width), static_cast<int>(height));
    app->OnStart();

    std::cout << "Running offscreen render: " << width << "x" << height
              << ", frames=" << frameCount << std::endl;
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        const uint32_t frameNumber = frame + 1u;
        const bool captureThisFrame = renderDoc && (renderDocCaptureFrame == frameNumber);
        if (captureThisFrame) {
            std::cout << "RenderDoc: starting capture on frame " << frameNumber << std::endl;
            renderDoc->StartFrameCapture(renderDocDevice, nullptr);
        }

        app->OnFrame();

        if (captureThisFrame) {
            const uint32_t result = renderDoc->EndFrameCapture(renderDocDevice, nullptr);
            std::cout << "RenderDoc: finished capture on frame " << frameNumber
                      << ", result=" << result << std::endl;
            PrintLatestRenderDocCapture(renderDoc);
        }
    }

    app->OnStop();
    app->OnRenderTargetDestroy();
    app.reset();

    return 0;
}
