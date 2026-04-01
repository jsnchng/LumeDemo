#ifndef APPLICATION_INTERFACE_H
#define APPLICATION_INTERFACE_H

#include <core/os/platform_create_info.h>
#include <render/device/intf_device.h>

class IApplication {
public:
    IApplication() = default;
    virtual ~IApplication() = default;

    virtual RENDER_NS::IDevice* OnInit(CORE_NS::PlatformCreateInfo platformCreateInfo) = 0;
    virtual void OnWindowUpdate(RENDER_NS::SwapchainCreateInfo swapchainCreateInfo, int width, int height) = 0;
    virtual void OnWindowDestroy() = 0;
    virtual void OnStart() = 0;
    virtual void OnStop() = 0;
    virtual void OnFrame() = 0;
};

#endif // APPLICATION_INTERFACE_H
