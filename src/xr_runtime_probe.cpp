#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
bool Check(XrResult result,const char* operation) {
    printf("%s=%d\n",operation,result);
    return XR_SUCCEEDED(result);
}
struct Instance {
    XrInstance value{};
    ~Instance() { if(value) xrDestroyInstance(value); }
};
}
int main() {
    puts("GRID2VR OpenXR compatibility query; architecture=x86; session_creation=disabled");
    uint32_t count{};
    if(!Check(xrEnumerateInstanceExtensionProperties(nullptr,0,&count,nullptr),"enumerate_extensions")) return 1;
    std::vector<XrExtensionProperties> extensions(count,{XR_TYPE_EXTENSION_PROPERTIES});
    if(!Check(xrEnumerateInstanceExtensionProperties(nullptr,count,&count,extensions.data()),"read_extensions")) return 1;
    bool d3d11=false;
    for(const auto& extension:extensions)
        if(!strcmp(extension.extensionName,XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) d3d11=true;
    if(!d3d11) { puts("D3D11 extension unavailable"); return 2; }
    const char* enabled=XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(create.applicationInfo.applicationName,"GRID2VR compatibility query");
    create.applicationInfo.apiVersion=XR_MAKE_VERSION(1,0,0);
    create.enabledExtensionCount=1; create.enabledExtensionNames=&enabled;
    Instance instance;
    if(!Check(xrCreateInstance(&create,&instance.value),"create_instance")) return 3;
    XrInstanceProperties runtime{XR_TYPE_INSTANCE_PROPERTIES};
    if(!Check(xrGetInstanceProperties(instance.value,&runtime),"runtime_properties")) return 3;
    printf("runtime=%s version=%u.%u.%u\n",runtime.runtimeName,
        static_cast<unsigned>(XR_VERSION_MAJOR(runtime.runtimeVersion)),
        static_cast<unsigned>(XR_VERSION_MINOR(runtime.runtimeVersion)),
        static_cast<unsigned>(XR_VERSION_PATCH(runtime.runtimeVersion)));
    XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO}; get.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system{};
    if(!Check(xrGetSystem(instance.value,&get,&system),"get_headset_system")) return 4;
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    if(!Check(xrGetSystemProperties(instance.value,system,&properties),"system_properties")) return 4;
    printf("headset=%s orientation_tracking=%u position_tracking=%u\n",properties.systemName,
        properties.trackingProperties.orientationTracking,properties.trackingProperties.positionTracking);
    PFN_xrGetD3D11GraphicsRequirementsKHR requirementsFn{};
    if(!Check(xrGetInstanceProcAddr(instance.value,"xrGetD3D11GraphicsRequirementsKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&requirementsFn)),"get_graphics_requirements_function") || !requirementsFn) return 5;
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if(!Check(requirementsFn(instance.value,system,&requirements),"graphics_requirements")) return 5;
    printf("required_adapter_luid=%08lx:%08lx minimum_feature_level=0x%x\n",
        static_cast<unsigned long>(requirements.adapterLuid.HighPart),requirements.adapterLuid.LowPart,requirements.minFeatureLevel);
    ComPtr<IDXGIFactory1> factory;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 6;
    bool found=false;
    for(UINT i=0;;++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const auto result=factory->EnumAdapters1(i,&adapter);
        if(result==DXGI_ERROR_NOT_FOUND) break;
        if(FAILED(result)) return 6;
        DXGI_ADAPTER_DESC1 description{};
        if(FAILED(adapter->GetDesc1(&description))) return 6;
        if(!memcmp(&description.AdapterLuid,&requirements.adapterLuid,sizeof(LUID))) {
            printf("required_adapter=%ls vendor=0x%x device=0x%x\n",description.Description,description.VendorId,description.DeviceId);
            found=true;
        }
    }
    if(!found) { puts("Required adapter not found"); return 6; }
    if(!Check(xrEnumerateViewConfigurationViews(instance.value,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&count,nullptr),"count_stereo_views") || count!=2) return 7;
    std::vector<XrViewConfigurationView> views(count,{XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if(!Check(xrEnumerateViewConfigurationViews(instance.value,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,count,&count,views.data()),"read_stereo_views") || count!=2) return 7;
    for(uint32_t i=0;i<count;++i)
        printf("eye%u=%ux%u recommended_samples=%u\n",i,views[i].recommendedImageRectWidth,views[i].recommendedImageRectHeight,views[i].recommendedSwapchainSampleCount);
    puts("Query completed. Game-device compatibility, sessions, tracking poses, stereo submission and headset rendering remain untested.");
    return 0;
}
