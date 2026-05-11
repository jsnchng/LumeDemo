#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include <base/math/mathf.h>
#include <base/math/quaternion.h>
#include <base/math/quaternion_util.h>
#include <base/math/vector.h>
#include <base/math/vector_util.h>
#include <core/ecs/intf_entity_manager.h>
#include <core/ecs/intf_system_graph_loader.h>
#include <base/math/matrix_util.h>

#include <core/engine_info.h>
#include <core/image/intf_image_loader_manager.h>
#include <core/implementation_uids.h>
#include <core/intf_engine.h>
#include <core/io/intf_file_manager.h>
#include <core/log.h>
#include <core/plugin/intf_plugin.h>
#include <core/plugin/intf_plugin_register.h>
#include <render/datastore/intf_render_data_store_manager.h>
#include <render/datastore/intf_render_data_store_default_staging.h>
#include <render/datastore/intf_render_data_store_pod.h>
#include <render/device/intf_device.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/device/intf_shader_manager.h>
#include <render/implementation_uids.h>
#include <render/intf_render_context.h>
#include <render/intf_renderer.h>
#include <render/nodecontext/intf_render_node_graph_manager.h>
#include <render/resource_handle.h>
#if RENDER_HAS_VULKAN_BACKEND
#include <render/vulkan/intf_device_vk.h>
#endif
#include <3d/ecs/components/camera_component.h>
#include <3d/ecs/components/environment_component.h>
#include <3d/ecs/components/fog_component.h>
#include <3d/ecs/components/light_component.h>
#include <3d/ecs/components/material_component.h>
#include <3d/ecs/components/name_component.h>
#include <3d/ecs/components/render_configuration_component.h>
#include <3d/ecs/components/render_handle_component.h>
#include <3d/ecs/components/transform_component.h>
#include <3d/ecs/systems/intf_animation_system.h>
#include <3d/ecs/systems/intf_node_system.h>
#include <3d/implementation_uids.h>
#include <3d/intf_graphics_context.h>
#include <3d/loaders/intf_scene_loader.h>
#include <3d/util/intf_mesh_util.h>
#include <3d/util/intf_picking.h>
#include <3d/util/intf_scene_util.h>

#include <plugintemplate/implementation_uids.h>
#include <plugintemplate/camera_control_system.h>

#include "application_config.h"
#include "application_factory.h"
#include "application_interface.h"

using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;
using namespace CORE3D_NS;
using namespace PT_NS;

class MinimalDemo : public IApplication {
public:
    MinimalDemo() {}
    
    ~MinimalDemo() override = default;

    IDevice* OnInit(PlatformCreateInfo platformCreateInfo) override
    {
        // Engine
        const EngineCreateInfo engineCreateInfo{};
        auto factory = GetInstance<IEngineFactory>(UID_ENGINE_FACTORY);
        engine_ = factory->Create(engineCreateInfo);
        RegisterAppPaths(*engine_);
        engine_->Init();
        
        ecs_ = engine_->CreateEcs();

        // Render
        constexpr Uid uidRender[] = { UID_RENDER_PLUGIN };
        GetPluginRegister().LoadPlugins(uidRender);
        renderContext_ = static_cast<IRenderContext::Ptr>(engine_->GetInterface<IClassFactory>()->CreateInstance(UID_RENDER_CONTEXT));
        DeviceCreateInfo deviceCreateInfo;
#if RENDER_HAS_VULKAN_BACKEND
        BackendExtraVk vkExtra;
        deviceCreateInfo.backendType = DeviceBackendType::VULKAN;
        deviceCreateInfo.backendConfiguration = &vkExtra;
#endif
        RenderCreateInfo renderCreateInfo{};
        renderCreateInfo.deviceCreateInfo = deviceCreateInfo;
        IDevice* device = nullptr;
        const RenderResultCode rcc = renderContext_->Init(renderCreateInfo);
        if (rcc == RenderResultCode::RENDER_SUCCESS) {
            device = &(renderContext_->GetDevice());
        }

        // 3D
        constexpr Uid uid3D[] = { UID_3D_PLUGIN };
        GetPluginRegister().LoadPlugins(uid3D);
        graphicsContext_ = CreateInstance<IGraphicsContext>(*renderContext_->GetInterface<IClassFactory>(), UID_GRAPHICS_CONTEXT);
        graphicsContext_->Init();

        // PluginTemplate
        constexpr Uid uidPT[] = { UID_PT_PLUGIN };
        GetPluginRegister().LoadPlugins(uidPT);

        return device;
    }

    void OnWindowUpdate(SwapchainCreateInfo swapchainCreateInfo, int width, int height) override
    {
        if (width > 0 && height > 0) {
            windowWidth_ = width;
            windowHeight_ = height;
            const auto& sceneUtil = graphicsContext_->GetSceneUtil();
            sceneUtil.UpdateCameraViewport(*ecs_, activeCamera_, { windowWidth_, windowHeight_ }, autoAspect_, originalFov_, orthoScale_);
            renderContext_->GetDevice().CreateSwapchain(swapchainCreateInfo);
        }
        else {
            renderContext_->GetDevice().DestroySwapchain();
        }
    }

    void OnWindowDestroy() override
    {
        renderContext_->GetDevice().DestroySwapchain();
    }

    void OnStart() override
    {
        ecs_->Initialize();
        transformManager_ = GetManager<ITransformComponentManager>(*ecs_);
        cameraManager_ = GetManager<ICameraComponentManager>(*ecs_);
        // .rng indicates that it is the same as the original file in 3d/rendernodegraphs
        sceneRng_ = CreateRenderNodeGraph("3drendernodegraphs://core3d_rng_scene.rng");
        // .json means the file has been changed from that in 3d/rendernodegraphs
        cameraSceneDeferredRNG_ = CreateRenderNodeGraph("pt://rendernodegraphs/core3d_rng_cam_scene_deferred.rng");
        {
            auto* nodeSystem = GetSystem<INodeSystem>(*ecs_);
            auto rootNode = nodeSystem->CreateNode();
            rootNodeEntity_ = rootNode->GetEntity();
            auto rccm = GetManager<IRenderConfigurationComponentManager>(*ecs_);
            rccm->Create(rootNodeEntity_);
            auto handle = rccm->Write(rootNodeEntity_);
            RenderConfigurationComponent& renderConfig = *handle;
            renderConfig.environment = ecs_->GetEntityManager().Create();
            auto ecm = GetManager<IEnvironmentComponentManager>(*ecs_);
            ecm->Create(renderConfig.environment);
            auto envHandle = ecm->Write(renderConfig.environment);
            EnvironmentComponent& envComponent = *envHandle;
            envComponent.background = EnvironmentComponent::Background::CUBEMAP;
        }
        {
            const auto& sceneUtil = graphicsContext_->GetSceneUtil();
            cameraEntity_ = sceneUtil.CreateCamera(*ecs_, Math::Vec3(0.f, 0.f, 3.f), {}, 0.1f, 1000.f, 60.f);
            activeCamera_ = cameraEntity_;
            
            // CameraControlSystem is automatically initialized by the plugin
            // Camera will be controlled by the plugin's CameraControlSystem
        }
        {
            const char* filename = std::getenv("LUME_MODEL_PATH");
            if (!filename || filename[0] == '\0') {
                filename = "assets://glTF/WaterBottle/glTF/WaterBottle.gltf";
            }
            std::cout << "Loading model: " << filename << std::endl;

            auto loader = graphicsContext_->GetSceneUtil().GetSceneLoader(filename);
            auto result = loader->Load(filename);
            auto importer = loader->CreateSceneImporter(*ecs_);
            importer->ImportResources(result.data, CORE_IMPORT_RESOURCE_FLAG_BITS_ALL);
            const auto& importResult = importer->GetResult();
            importedResources_.push_back(importResult.data);
            importer->ImportScene(0, rootNodeEntity_, CORE_IMPORT_COMPONENT_FLAG_BITS_ALL);
        }
    }

    void OnStop() override
    {
        SaveOptimizedTextures();
    }

    void OnFrame() override
    {
        UpdateCamera();

        auto* ecs = ecs_.get();
        const bool needRender = engine_->TickFrame(array_view(&ecs, 1));
        
        // Check if camera view switched and notify render nodes
        CheckViewSwitchAndNotify();
        
        if (needRender) {
            IRenderer& renderer = renderContext_->GetRenderer();
            vector<RenderHandleReference> rngs{ sceneRng_ };
            rngs.emplace_back(cameraSceneDeferredRNG_);
            renderer.RenderFrame(rngs);
        }
    }

private:
    RenderHandleReference CreateRenderNodeGraph(const string_view rngPath)
    {
        IRenderNodeGraphManager& graphManager = renderContext_->GetRenderNodeGraphManager();

        auto loader = &graphManager.GetRenderNodeGraphLoader();
        auto const result = loader->Load(rngPath);
        if (!result.error.empty()) {
            return {};
        }
        return graphManager.Create(
            IRenderNodeGraphManager::RenderNodeGraphUsageType::RENDER_NODE_GRAPH_STATIC, result.desc
        );
    }

    void UpdateCamera()
    {
        if (updateCamera_ && cameraManager_) {
            updateCamera_ = false;
            activeCamera_ = cameraEntity_;
            auto cameraHandle = cameraManager_->Write(activeCamera_);
            if (cameraHandle) {
                cameraHandle->sceneFlags |= CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            }
            const auto& sceneUtil = graphicsContext_->GetSceneUtil();
            sceneUtil.UpdateCameraViewport(*ecs_, activeCamera_, { windowWidth_, windowHeight_ }, false, 60.f, 1.f);
        }
    }
    
    void CheckViewSwitchAndNotify()
    {
        auto* cameraControlSystem = GetSystem<CameraControlSystem>(*ecs_);
        if (cameraControlSystem) {
            // Check if view switch was detected in PREVIOUS frame
            // We delay the Pod flag creation by one frame so that:
            // - Frame N: Camera position updates, new GT image is rendered
            // - Frame N+1: Pod flag is set, buffers are cleared, training starts fresh
            if (pendingViewSwitchClear_) {
                auto& renderDataStoreMgr = renderContext_->GetRenderDataStoreManager();
                auto dataStorePtr = renderDataStoreMgr.GetRenderDataStore("RenderDataStorePod");
                auto* dataStorePod = static_cast<IRenderDataStorePod*>(dataStorePtr.get());
                
                if (dataStorePod) {
                    // Create a flag struct to pass to render nodes
                    struct ViewSwitchFlag {
                        uint32_t shouldClearBuffers = 1;
                    };
                    ViewSwitchFlag flag;
                    flag.shouldClearBuffers = 1;
                    
                    // Set the flag in the Pod data store
                    dataStorePod->CreatePod("SRTraining", "ViewSwitchFlag",
                        array_view<const uint8_t>(reinterpret_cast<const uint8_t*>(&flag), sizeof(flag)));
                    
                    pendingViewSwitchClear_ = false;
                }
            }
            
            // Check if view switched THIS frame - set pending flag for NEXT frame
            if (cameraControlSystem->HasViewSwitched()) {
                pendingViewSwitchClear_ = true;
                cameraControlSystem->ClearViewSwitchedFlag();
            }
        }
    }

    void SaveOptimizedTextures()
    {
        auto& gpuResourceMgr = renderContext_->GetDevice().GetGpuResourceManager();
        auto& renderDataStoreMgr = renderContext_->GetRenderDataStoreManager();

        // Find the LR texture by its registered name
        RenderHandleReference lrTexture = gpuResourceMgr.GetImageHandle("lowres_albedo");
        if (!RenderHandleUtil::IsValid(lrTexture.GetHandle())) {
            std::cout << "SaveOptimizedTextures: lowres_albedo not found" << std::endl;
            return;
        }

        const GpuImageDesc imgDesc = gpuResourceMgr.GetImageDescriptor(lrTexture);
        const uint32_t w = imgDesc.width;
        const uint32_t h = imgDesc.height;
        uint32_t sourceBytesPerPixel = 0u;
        if (imgDesc.format == BASE_FORMAT_R32G32B32A32_SFLOAT) {
            sourceBytesPerPixel = 4u * sizeof(float);
        } else if ((imgDesc.format == BASE_FORMAT_R8G8B8A8_UNORM) || (imgDesc.format == BASE_FORMAT_R8G8B8A8_SRGB)) {
            sourceBytesPerPixel = 4u;
        } else {
            std::cout << "SaveOptimizedTextures: unsupported lowres_albedo format "
                      << static_cast<uint32_t>(imgDesc.format) << std::endl;
            return;
        }
        const uint32_t pngBytesPerPixel = 4u;
        const uint32_t readbackByteSize = w * h * sourceBytesPerPixel;
        const uint32_t pngByteSize = w * h * pngBytesPerPixel;

        std::cout << "SaveOptimizedTextures: texture size " << w << "x" << h
                  << ", format=" << static_cast<uint32_t>(imgDesc.format)
                  << ", readbackByteSize=" << readbackByteSize << std::endl;

        // Create a HOST_VISIBLE readback buffer
        GpuBufferDesc bufDesc;
        bufDesc.usageFlags = CORE_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufDesc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        bufDesc.byteSize = readbackByteSize;
        bufDesc.engineCreationFlags = CORE_ENGINE_BUFFER_CREATION_MAP_OUTSIDE_RENDERER |
                                     CORE_ENGINE_BUFFER_CREATION_CREATE_IMMEDIATE |
                                     CORE_ENGINE_BUFFER_CREATION_DEFERRED_DESTROY;
        RenderHandleReference readbackBuffer = gpuResourceMgr.Create("sr_readback_buffer", bufDesc);
        if (!RenderHandleUtil::IsValid(readbackBuffer.GetHandle())) {
            std::cout << "SaveOptimizedTextures: failed to create readback buffer" << std::endl;
            return;
        }

        // Schedule GPU copy: Image -> Buffer via staging (executed at end of next frame)
        auto* staging = static_cast<IRenderDataStoreDefaultStaging*>(
            renderDataStoreMgr.GetRenderDataStore("RenderDataStoreDefaultStaging").get());
        if (!staging) {
            std::cout << "SaveOptimizedTextures: staging data store not found" << std::endl;
            return;
        }

        BufferImageCopy bic {};
        bic.bufferOffset = 0;
        bic.bufferRowLength = 0;
        bic.bufferImageHeight = 0;
        bic.imageSubresource = { CORE_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1u };
        bic.imageOffset = { 0, 0, 0 };
        bic.imageExtent = { w, h, 1u };

        staging->CopyImageToBuffer(lrTexture, readbackBuffer, bic,
            IRenderDataStoreDefaultStaging::ResourceCopyInfo::END_FRAME);

        std::cout << "SaveOptimizedTextures: CopyImageToBuffer scheduled, running final RenderFrame..." << std::endl;

        // Run one final render frame to execute the GPU copy
        {
            auto* ecs = ecs_.get();
            engine_->TickFrame(array_view(&ecs, 1));

            IRenderer& renderer = renderContext_->GetRenderer();
            vector<RenderHandleReference> rngs{ sceneRng_ };
            rngs.emplace_back(cameraSceneDeferredRNG_);
            renderer.RenderFrame(rngs);
        }

        std::cout << "SaveOptimizedTextures: Final frame rendered, reading back..." << std::endl;

        // Ensure the GPU has finished the copy operation before we map and read the buffer
        renderContext_->GetDevice().WaitForIdle();

        // Now map the buffer and write to file
        void* mappedData = gpuResourceMgr.MapBufferMemory(readbackBuffer);
        if (!mappedData) {
            std::cout << "SaveOptimizedTextures: MapBufferMemory failed" << std::endl;
            return;
        }

        const auto linearToSrgb8 = [](float linear) -> uint8_t {
            linear = std::clamp(linear, 0.0f, 1.0f);
            const float srgb = (linear <= 0.0031308f)
                ? (linear * 12.92f)
                : (1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f);
            return static_cast<uint8_t>(std::round(std::clamp(srgb, 0.0f, 1.0f) * 255.0f));
        };
        const auto linearToUnorm8 = [](float value) -> uint8_t {
            return static_cast<uint8_t>(std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        };

        std::vector<uint8_t> srgbData(pngByteSize);
        if (imgDesc.format == BASE_FORMAT_R32G32B32A32_SFLOAT) {
            const float* rgba32fData = static_cast<const float*>(mappedData);
            for (uint32_t pixel = 0; pixel < w * h; ++pixel) {
                srgbData[pixel * 4u + 0u] = linearToSrgb8(rgba32fData[pixel * 4u + 0u]);
                srgbData[pixel * 4u + 1u] = linearToSrgb8(rgba32fData[pixel * 4u + 1u]);
                srgbData[pixel * 4u + 2u] = linearToSrgb8(rgba32fData[pixel * 4u + 2u]);
                srgbData[pixel * 4u + 3u] = linearToUnorm8(rgba32fData[pixel * 4u + 3u]);
            }
        } else if (imgDesc.format == BASE_FORMAT_R8G8B8A8_UNORM) {
            const uint8_t* rgba8Data = static_cast<const uint8_t*>(mappedData);
            for (uint32_t pixel = 0; pixel < w * h; ++pixel) {
                srgbData[pixel * 4u + 0u] = linearToSrgb8(rgba8Data[pixel * 4u + 0u] / 255.0f);
                srgbData[pixel * 4u + 1u] = linearToSrgb8(rgba8Data[pixel * 4u + 1u] / 255.0f);
                srgbData[pixel * 4u + 2u] = linearToSrgb8(rgba8Data[pixel * 4u + 2u] / 255.0f);
                srgbData[pixel * 4u + 3u] = rgba8Data[pixel * 4u + 3u];
            }
        } else {
            const uint8_t* rgba8Data = static_cast<const uint8_t*>(mappedData);
            std::copy(rgba8Data, rgba8Data + pngByteSize, srgbData.begin());
        }

        // Save as PNG
        const char* pngPath = "optimized_albedo.png";
        int result = stbi_write_png(pngPath, static_cast<int>(w), static_cast<int>(h),
            4, srgbData.data(), static_cast<int>(w * 4));
        if (result) {
            std::cout << "SaveOptimizedTextures: Saved PNG to " << pngPath << std::endl;
        } else {
            std::cout << "SaveOptimizedTextures: Failed to write PNG" << std::endl;
        }

        gpuResourceMgr.UnmapBuffer(readbackBuffer);
        std::cout << "SaveOptimizedTextures: Done." << std::endl;
    }

private:
    IEngine::Ptr engine_;
    IEcs::Ptr ecs_;
    IRenderContext::Ptr renderContext_;
    IGraphicsContext::Ptr graphicsContext_;
    RenderHandleReference sceneRng_;
    RenderHandleReference cameraSceneDeferredRNG_;
    uint32_t windowWidth_;
    uint32_t windowHeight_;
    bool autoAspect_;
    float originalFov_;
    float  orthoScale_;
    Entity activeCamera_;
    Entity rootNodeEntity_;
    Entity cameraEntity_;
    ITransformComponentManager* transformManager_;
    ICameraComponentManager* cameraManager_;
    vector<ResourceData> importedResources_;
    bool updateCamera_ = true;
    bool pendingViewSwitchClear_ = false;  // Delayed flag for buffer clearing
};

IApplication* createApplication()
{
    return new MinimalDemo();
}

const char* applicationName = "Minimal Demo";
