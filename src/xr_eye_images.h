#pragma once
#include "eye_blit.h"
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <vector>

namespace grid2vr {
// The caller owns Wait/Begin/EndFrame and supplies the exact located views used
// to render both accepted eyes. Shutdown is explicit on the owning render thread.
class XrEyeImages {
public:
    bool Initialize(XrSession session,ID3D11Device* device,NativeEyeDraw draw,unsigned width,unsigned height,bool widescreenMenus=false);
    bool Copy(uint64_t frame,XrSpace space,const std::array<XrView,2>& views,ID3D11Texture2D* left,ID3D11Texture2D* right);
    bool CopyScreen(uint64_t frame,XrSpace localSpace,const XrPosef& anchor,ID3D11Texture2D* backbuffer);
    const XrCompositionLayerProjection* Projection(uint64_t frame) const { return ready_ && !screenReady_ && frame==frame_?&layer_:nullptr; }
    const XrCompositionLayerQuad* Screen(uint64_t frame) const { return ready_ && screenReady_ && frame==frame_?&screen_:nullptr; }
    bool Shutdown();
    const char* Failure() const { return failure_; }
    XrResult LastResult() const { return result_; }
    XrEyeImages()=default;
    XrEyeImages(const XrEyeImages&)=delete;
    XrEyeImages& operator=(const XrEyeImages&)=delete;
private:
    bool Check(XrResult result,const char* operation);
    bool Refuse(const char* reason);
    struct Eye {
        XrSwapchain chain{};
        std::vector<XrSwapchainImageD3D11KHR> images;
        std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> targets;
        bool waited{};
    };
    bool Release();
    std::array<Eye,2> eyes_;
    EyeBlit blit_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Query> completion_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> screenSource_;
    std::array<XrCompositionLayerProjectionView,2> projections_{};
    XrCompositionLayerProjection layer_{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad screen_{XR_TYPE_COMPOSITION_LAYER_QUAD};
    DWORD thread_{};
    unsigned width_{},height_{};
    uint64_t frame_{};
    bool initialized_{},ready_{},screenReady_{},gpuUsed_{},widescreenMenus_{};
    const char* failure_{};
    XrResult result_{XR_SUCCESS};
};
}
