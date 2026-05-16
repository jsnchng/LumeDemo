#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
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
#include <render/util/intf_render_frame_util.h>
#include <render/util/intf_render_util.h>
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

    void OnRenderTargetUpdate(int width, int height) override
    {
        if (width > 0 && height > 0) {
            windowWidth_ = static_cast<uint32_t>(width);
            windowHeight_ = static_cast<uint32_t>(height);
            CreateOffscreenBackBuffer(windowWidth_, windowHeight_);

            const auto& sceneUtil = graphicsContext_->GetSceneUtil();
            sceneUtil.UpdateCameraViewport(*ecs_, activeCamera_, { windowWidth_, windowHeight_ }, autoAspect_, originalFov_, orthoScale_);
        }
        else {
            offscreenBackBuffer_ = {};
        }
    }

    void OnRenderTargetDestroy() override
    {
        if (renderContext_) {
            renderContext_->GetDevice().WaitForIdle();
        }
        offscreenBackBuffer_ = {};
    }

    void OnStart() override
    {
        ecs_->Initialize();
        transformManager_ = GetManager<ITransformComponentManager>(*ecs_);
        cameraManager_ = GetManager<ICameraComponentManager>(*ecs_);
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
            auto cameraHandle = cameraManager_->Write(activeCamera_);
            if (cameraHandle) {
                cameraHandle->renderingPipeline = CameraComponent::RenderingPipeline::CUSTOM;
                cameraHandle->customRenderNodeGraphFile = "pt://rendernodegraphs/core3d_rng_cam_scene_deferred.rng";
            }
            
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
            const auto rngs = graphicsContext_->GetRenderNodeGraphs(*ecs_);
            renderer.RenderFrame(rngs);
        }
    }

private:
    void CreateOffscreenBackBuffer(uint32_t width, uint32_t height)
    {
        auto& gpuResourceMgr = renderContext_->GetDevice().GetGpuResourceManager();
        if (RenderHandleUtil::IsValid(offscreenBackBuffer_.GetHandle())) {
            const GpuImageDesc currentDesc = gpuResourceMgr.GetImageDescriptor(offscreenBackBuffer_);
            if ((currentDesc.width == width) && (currentDesc.height == height)) {
                ConfigureOffscreenBackBuffer();
                return;
            }
        }

        GpuImageDesc desc;
        desc.imageType = CORE_IMAGE_TYPE_2D;
        desc.imageViewType = CORE_IMAGE_VIEW_TYPE_2D;
        desc.format = BASE_FORMAT_R8G8B8A8_SRGB;
        desc.imageTiling = CORE_IMAGE_TILING_OPTIMAL;
        desc.usageFlags = CORE_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          CORE_IMAGE_USAGE_SAMPLED_BIT |
                          CORE_IMAGE_USAGE_TRANSFER_SRC_BIT;
        desc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        desc.engineCreationFlags = CORE_ENGINE_IMAGE_CREATION_DYNAMIC_BARRIERS;
        desc.width = width;
        desc.height = height;
        desc.depth = 1u;
        desc.mipCount = 1u;
        desc.layerCount = 1u;
        desc.sampleCountFlags = CORE_SAMPLE_COUNT_1_BIT;

        offscreenBackBuffer_ = gpuResourceMgr.Create("OFFSCREEN_BACKBUFFER", desc);
        if (!RenderHandleUtil::IsValid(offscreenBackBuffer_.GetHandle())) {
            std::cout << "CreateOffscreenBackBuffer: failed to create " << width << "x" << height << " target" << std::endl;
            return;
        }

        ConfigureOffscreenBackBuffer();
    }

    void ConfigureOffscreenBackBuffer()
    {
        IRenderFrameUtil::BackBufferConfiguration config;
        config.backBufferName = "CORE_DEFAULT_BACKBUFFER";
        config.backBufferType = IRenderFrameUtil::BackBufferConfiguration::BackBufferType::GPU_IMAGE;
        config.backBufferHandle = offscreenBackBuffer_;
        config.present = false;
        renderContext_->GetRenderUtil().GetRenderFrameUtil().SetBackBufferConfiguration(config);
    }

    void UpdateCamera()
    {
        if (updateCamera_ && cameraManager_) {
            updateCamera_ = false;
            activeCamera_ = cameraEntity_;
            auto cameraHandle = cameraManager_->Write(activeCamera_);
            if (cameraHandle) {
                cameraHandle->sceneFlags |= CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
                cameraHandle->renderingPipeline = CameraComponent::RenderingPipeline::CUSTOM;
                cameraHandle->customRenderNodeGraphFile = "pt://rendernodegraphs/core3d_rng_cam_scene_deferred.rng";
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

        auto* staging = static_cast<IRenderDataStoreDefaultStaging*>(
            renderDataStoreMgr.GetRenderDataStore("RenderDataStoreDefaultStaging").get());
        if (!staging) {
            std::cout << "SaveOptimizedTextures: staging data store not found" << std::endl;
            return;
        }

        struct ReadbackImage {
            const char* imageName;
            const char* pngPath;
            RenderHandleReference image;
            RenderHandleReference buffer;
            GpuImageDesc desc;
            uint32_t sourceBytesPerPixel { 0u };
        };

        const auto sourceBytesPerPixel = [](Format format) -> uint32_t {
            if (format == BASE_FORMAT_R32G32B32A32_SFLOAT) {
                return 4u * sizeof(float);
            }
            if ((format == BASE_FORMAT_R8G8B8A8_UNORM) || (format == BASE_FORMAT_R8G8B8A8_SRGB) ||
                (format == BASE_FORMAT_B10G11R11_UFLOAT_PACK32)) {
                return 4u;
            }
            return 0u;
        };

        vector<ReadbackImage> readbacks = {
            { "lrTexture", "optimized_albedo.png" },
            { "predicted_color_output", "pc.png" },
            { "color", "gt.png" },
        };

        for (auto& readback : readbacks) {
            readback.image = gpuResourceMgr.GetImageHandle(readback.imageName);
            if (!RenderHandleUtil::IsValid(readback.image.GetHandle())) {
                std::cout << "SaveOptimizedTextures: " << readback.imageName << " not found" << std::endl;
                return;
            }

            readback.desc = gpuResourceMgr.GetImageDescriptor(readback.image);
            readback.sourceBytesPerPixel = sourceBytesPerPixel(readback.desc.format);
            if (readback.sourceBytesPerPixel == 0u) {
                std::cout << "SaveOptimizedTextures: unsupported " << readback.imageName << " format "
                          << static_cast<uint32_t>(readback.desc.format) << std::endl;
                return;
            }

            GpuBufferDesc bufDesc;
            bufDesc.usageFlags = CORE_BUFFER_USAGE_TRANSFER_DST_BIT;
            bufDesc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            bufDesc.byteSize = readback.desc.width * readback.desc.height * readback.sourceBytesPerPixel;
            bufDesc.engineCreationFlags = CORE_ENGINE_BUFFER_CREATION_MAP_OUTSIDE_RENDERER |
                                         CORE_ENGINE_BUFFER_CREATION_CREATE_IMMEDIATE |
                                         CORE_ENGINE_BUFFER_CREATION_DEFERRED_DESTROY;
            readback.buffer = gpuResourceMgr.Create(string("sr_readback_") + readback.imageName, bufDesc);
            if (!RenderHandleUtil::IsValid(readback.buffer.GetHandle())) {
                std::cout << "SaveOptimizedTextures: failed to create readback buffer for "
                          << readback.imageName << std::endl;
                return;
            }

            BufferImageCopy bic {};
            bic.bufferOffset = 0;
            bic.bufferRowLength = 0;
            bic.bufferImageHeight = 0;
            bic.imageSubresource = { CORE_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1u };
            bic.imageOffset = { 0, 0, 0 };
            bic.imageExtent = { readback.desc.width, readback.desc.height, 1u };

            staging->CopyImageToBuffer(readback.image, readback.buffer, bic,
                IRenderDataStoreDefaultStaging::ResourceCopyInfo::END_FRAME);

            std::cout << "SaveOptimizedTextures: scheduled " << readback.imageName << " -> "
                      << readback.pngPath << " (" << readback.desc.width << "x" << readback.desc.height
                      << ", format=" << static_cast<uint32_t>(readback.desc.format) << ")" << std::endl;
        }

        std::cout << "SaveOptimizedTextures: CopyImageToBuffer scheduled, running final RenderFrame..." << std::endl;

        // Run one final render frame to execute the GPU copy
        {
            auto* ecs = ecs_.get();
            engine_->TickFrame(array_view(&ecs, 1));

            IRenderer& renderer = renderContext_->GetRenderer();
            const auto rngs = graphicsContext_->GetRenderNodeGraphs(*ecs_);
            renderer.RenderFrame(rngs);
        }

        std::cout << "SaveOptimizedTextures: Final frame rendered, reading back..." << std::endl;

        // Ensure the GPU has finished the copy operation before we map and read the buffer
        renderContext_->GetDevice().WaitForIdle();

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
        const auto unpackUnsignedFloat = [](uint32_t bits, uint32_t mantissaBits) -> float {
            const uint32_t mantissaMask = (1u << mantissaBits) - 1u;
            const uint32_t mantissa = bits & mantissaMask;
            const uint32_t exponent = bits >> mantissaBits;
            if (exponent == 0u) {
                return std::ldexp(static_cast<float>(mantissa), -static_cast<int>(mantissaBits + 14u));
            }
            if (exponent == 31u) {
                return mantissa ? 0.0f : std::numeric_limits<float>::infinity();
            }
            return std::ldexp(static_cast<float>((1u << mantissaBits) | mantissa),
                static_cast<int>(exponent) - 15 - static_cast<int>(mantissaBits));
        };

        for (const auto& readback : readbacks) {
            void* mappedData = gpuResourceMgr.MapBufferMemory(readback.buffer);
            if (!mappedData) {
                std::cout << "SaveOptimizedTextures: MapBufferMemory failed for "
                          << readback.imageName << std::endl;
                continue;
            }

            const uint32_t w = readback.desc.width;
            const uint32_t h = readback.desc.height;
            const uint32_t pngByteSize = w * h * 4u;
            std::vector<uint8_t> srgbData(pngByteSize);

            if (readback.desc.format == BASE_FORMAT_R32G32B32A32_SFLOAT) {
                const float* rgba32fData = static_cast<const float*>(mappedData);
                for (uint32_t pixel = 0; pixel < w * h; ++pixel) {
                    srgbData[pixel * 4u + 0u] = linearToSrgb8(rgba32fData[pixel * 4u + 0u]);
                    srgbData[pixel * 4u + 1u] = linearToSrgb8(rgba32fData[pixel * 4u + 1u]);
                    srgbData[pixel * 4u + 2u] = linearToSrgb8(rgba32fData[pixel * 4u + 2u]);
                    srgbData[pixel * 4u + 3u] = linearToUnorm8(rgba32fData[pixel * 4u + 3u]);
                }
            } else if (readback.desc.format == BASE_FORMAT_R8G8B8A8_UNORM) {
                const uint8_t* rgba8Data = static_cast<const uint8_t*>(mappedData);
                for (uint32_t pixel = 0; pixel < w * h; ++pixel) {
                    srgbData[pixel * 4u + 0u] = linearToSrgb8(rgba8Data[pixel * 4u + 0u] / 255.0f);
                    srgbData[pixel * 4u + 1u] = linearToSrgb8(rgba8Data[pixel * 4u + 1u] / 255.0f);
                    srgbData[pixel * 4u + 2u] = linearToSrgb8(rgba8Data[pixel * 4u + 2u] / 255.0f);
                    srgbData[pixel * 4u + 3u] = rgba8Data[pixel * 4u + 3u];
                }
            } else if (readback.desc.format == BASE_FORMAT_B10G11R11_UFLOAT_PACK32) {
                const uint32_t* packedData = static_cast<const uint32_t*>(mappedData);
                for (uint32_t pixel = 0; pixel < w * h; ++pixel) {
                    const uint32_t packed = packedData[pixel];
                    srgbData[pixel * 4u + 0u] = linearToSrgb8(unpackUnsignedFloat((packed >> 0u) & 0x7ffu, 6u));
                    srgbData[pixel * 4u + 1u] = linearToSrgb8(unpackUnsignedFloat((packed >> 11u) & 0x7ffu, 6u));
                    srgbData[pixel * 4u + 2u] = linearToSrgb8(unpackUnsignedFloat((packed >> 22u) & 0x3ffu, 5u));
                    srgbData[pixel * 4u + 3u] = 255u;
                }
            } else {
                const uint8_t* rgba8Data = static_cast<const uint8_t*>(mappedData);
                std::copy(rgba8Data, rgba8Data + pngByteSize, srgbData.begin());
            }

            int result = stbi_write_png(readback.pngPath, static_cast<int>(w), static_cast<int>(h),
                4, srgbData.data(), static_cast<int>(w * 4));
            if (result) {
                std::cout << "SaveOptimizedTextures: Saved PNG to " << readback.pngPath << std::endl;
            } else {
                std::cout << "SaveOptimizedTextures: Failed to write " << readback.pngPath << std::endl;
            }

            gpuResourceMgr.UnmapBuffer(readback.buffer);
        }
        std::cout << "SaveOptimizedTextures: Done." << std::endl;
    }

private:
    IEngine::Ptr engine_;
    IEcs::Ptr ecs_;
    IRenderContext::Ptr renderContext_;
    IGraphicsContext::Ptr graphicsContext_;
    RenderHandleReference offscreenBackBuffer_;
    uint32_t windowWidth_ = 1024u;
    uint32_t windowHeight_ = 1024u;
    bool autoAspect_ = false;
    float originalFov_ = 60.0f;
    float  orthoScale_ = 1.0f;
    Entity activeCamera_;
    Entity rootNodeEntity_;
    Entity cameraEntity_;
    ITransformComponentManager* transformManager_ = nullptr;
    ICameraComponentManager* cameraManager_ = nullptr;
    vector<ResourceData> importedResources_;
    bool updateCamera_ = true;
    bool pendingViewSwitchClear_ = false;  // Delayed flag for buffer clearing
};

IApplication* createApplication()
{
    return new MinimalDemo();
}

const char* applicationName = "Minimal Demo";
