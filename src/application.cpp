#include <algorithm>
#include <cinttypes>

#include <base/math/mathf.h>
#include <base/math/quaternion.h>
#include <base/math/quaternion_util.h>
#include <base/math/vector.h>
#include <base/math/vector_util.h>
#include <core/ecs/intf_entity_manager.h>
#include <core/ecs/intf_system_graph_loader.h>
#include <random>
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
        sceneRng_ = CreateRenderNodeGraph("pt://rendernodegraphs/core3d_rng_scene.rng");
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
        }
        {
            const char* filename = "assets://glTF/DamagedHelmet/glTF/DamagedHelmet.gltf";
            auto loader = graphicsContext_->GetSceneUtil().GetSceneLoader(filename);
            auto result = loader->Load(filename);
            auto importer = loader->CreateSceneImporter(*ecs_);
            importer->ImportResources(result.data, CORE_IMPORT_RESOURCE_FLAG_BITS_ALL);
            const auto& importResult = importer->GetResult();
            importedResources_.push_back(importResult.data);
            importer->ImportScene(0, rootNodeEntity_, CORE_IMPORT_COMPONENT_FLAG_BITS_ALL);
        }
    }

    void OnStop() override {}

    void OnFrame() override
    {
        if (frameCount_ % 300 == 0) {
            static std::mt19937 gen(std::random_device{}());
            std::uniform_real_distribution<float> disZ(-1.0f, 1.0f);
            std::uniform_real_distribution<float> disTheta(0.0f, 2.0f * Math::PI);

            // 在半径为 3.0 的球面上生成随机点
            float r = 3.0f;
            float z = disZ(gen);
            float phi = acos(z);
            float theta = disTheta(gen);

            Math::Vec3 pos(r * sin(phi) * cos(theta), r * sin(phi) * sin(theta), r * cos(phi));

            // 计算看向原点的 View 矩阵
            // 使用 (0, 1, 0) 作为 Up 向量，简单处理极点情况
            Math::Vec3 up = (abs(z) > 0.99f) ? Math::Vec3(1.f, 0.f, 0.f) : Math::Vec3(0.f, 1.f, 0.f);
            Math::Mat4X4 view = Math::LookAtRh(pos, { 0.f, 0.f, 0.f }, up);
            
            // 将 View 矩阵转为 World 矩阵
            Math::Mat4X4 world = Math::Inverse(view);

            // 分解矩阵获取 position 和 orientation
            Math::Vec3 s, t, sk;
            Math::Quat q;
            Math::Vec4 p;
            Math::Decompose(world, s, q, t, sk, p);

            if (transformManager_) {
                auto handle = transformManager_->Write(cameraEntity_);
                if (handle) {
                    handle->position = t;
                    handle->rotation = q;
                }
            }
        }
        frameCount_++;

        UpdateCamera();

        auto* ecs = ecs_.get();
        const bool needRender = engine_->TickFrame(array_view(&ecs, 1));
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
    uint32_t frameCount_ = 0;
};

IApplication* createApplication()
{
    return new MinimalDemo();
}

const char* applicationName = "Minimal Demo";