#include "seat_xr.h"
#include "seat_panel.h"
#include "seat_presenter.h"
#include <algorithm>
#include <vector>

namespace grid2vr::seat {
struct XrPanel::Impl {
    XrSwapchain chain{};
    XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> targets;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<ID3D11Query> completion;
    SeatPresenter presenter;
    DWORD thread{};
    uint32_t index{};
    uint64_t opening{};
    bool enabled{},acquired{},waited{},ready{},used{};
};
XrPanel::XrPanel()=default;
// An incomplete drain must retain its D3D/XR ownership until process teardown;
// never release GPU objects from DllMain or destroy a still-acquired image.
XrPanel::~XrPanel(){(void)impl_.release();}
bool XrPanel::Initialize(XrSession session,XrSpace local,ID3D11Device* device,NativeEyeDraw draw) {
    if(impl_ || !session || !local || !device || !draw)return false;
    impl_=std::make_unique<Impl>();auto& s=*impl_;s.thread=GetCurrentThreadId();device->GetImmediateContext(&s.context);
    uint32_t count{};
    if(xrEnumerateSwapchainFormats(session,0,&count,nullptr)!=XR_SUCCESS || !count || count>128)return false;
    std::vector<int64_t> formats(count);
    if(xrEnumerateSwapchainFormats(session,count,&count,formats.data())!=XR_SUCCESS || count!=formats.size() ||
       std::find(formats.begin(),formats.end(),DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)==formats.end())return false;
    XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};create.format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    create.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    create.sampleCount=create.faceCount=create.arraySize=create.mipCount=1;create.width=PanelImage::Width;create.height=PanelImage::Height;
    if(xrCreateSwapchain(session,&create,&s.chain)!=XR_SUCCESS || xrEnumerateSwapchainImages(s.chain,0,&count,nullptr)!=XR_SUCCESS || !count || count>16)return false;
    s.images.resize(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if(xrEnumerateSwapchainImages(s.chain,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(s.images.data()))!=XR_SUCCESS || count!=s.images.size())return false;
    for(const auto& image:s.images) {
        if(!image.texture)return false;
        Microsoft::WRL::ComPtr<ID3D11Device> actual;image.texture->GetDevice(&actual);D3D11_TEXTURE2D_DESC desc{};image.texture->GetDesc(&desc);
        if(actual.Get()!=device || desc.Width!=PanelImage::Width || desc.Height!=PanelImage::Height || desc.ArraySize!=1 || desc.MipLevels!=1 || desc.SampleDesc.Count!=1 ||
           (desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && desc.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS))return false;
        D3D11_RENDER_TARGET_VIEW_DESC view{};view.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;view.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
        if(FAILED(device->CreateRenderTargetView(image.texture,&view,&target)))return false;
        s.targets.push_back(std::move(target));
    }
    const D3D11_QUERY_DESC completion{D3D11_QUERY_EVENT,0};
    if(FAILED(device->CreateQuery(&completion,&s.completion)) || !s.presenter.Initialize(device,draw))return false;
    s.layer.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT|XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    s.layer.space=local;s.layer.eyeVisibility=XR_EYE_VISIBILITY_BOTH;s.layer.subImage.swapchain=s.chain;
    s.layer.subImage.imageRect.extent={PanelImage::Width,PanelImage::Height};s.layer.size={.6f,.8f};s.enabled=true;return true;
}
const XrCompositionLayerQuad* XrPanel::Layer(ID3D11Texture2D* source,const XrPosef& anchor,uint64_t opening) {
    if(!impl_)return nullptr;auto& s=*impl_;
    if(!s.enabled || s.thread!=GetCurrentThreadId() || !source)return nullptr;
    if(opening!=s.opening){s.opening=opening;s.layer.pose=anchor;s.ready=false;}
    if(!s.acquired) {
        const XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if(xrAcquireSwapchainImage(s.chain,&acquire,&s.index)!=XR_SUCCESS){s.enabled=false;return nullptr;}
        s.acquired=true;
    }
    const XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,nullptr,0};
    const auto waited=xrWaitSwapchainImage(s.chain,&wait);
    if(waited==XR_TIMEOUT_EXPIRED)return s.ready?&s.layer:nullptr;
    if(waited!=XR_SUCCESS){s.enabled=false;return nullptr;}
    s.waited=true;
    if(s.index>=s.targets.size()){s.enabled=false;return nullptr;}
    if(!s.presenter.Draw(source,s.targets[s.index].Get(),PanelImage::Width,PanelImage::Height,SeatPresenter::Mode::StraightAlpha)){s.enabled=false;return nullptr;}
    s.used=true;
    const XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if(xrReleaseSwapchainImage(s.chain,&release)!=XR_SUCCESS){s.enabled=false;return nullptr;}
    s.acquired=s.waited=false;s.ready=true;return &s.layer;
}
bool XrPanel::Enabled() const{return impl_ && impl_->enabled;}
bool XrPanel::Shutdown() {
    if(!impl_)return true;auto& s=*impl_;
    if(s.thread!=GetCurrentThreadId())return false;
    s.enabled=false;
    if(s.acquired) {
        const XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,nullptr,100000000};
        if(!s.waited && xrWaitSwapchainImage(s.chain,&wait)!=XR_SUCCESS)return false;
        s.waited=true;const XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if(xrReleaseSwapchainImage(s.chain,&release)!=XR_SUCCESS)return false;s.acquired=s.waited=false;
    }
    if(s.used) {
        s.context->End(s.completion.Get());s.context->Flush();const auto deadline=GetTickCount64()+2000;HRESULT result;
        do{result=s.context->GetData(s.completion.Get(),nullptr,0,0);if(result==S_FALSE)Sleep(1);}while(result==S_FALSE && GetTickCount64()<deadline);
        if(result!=S_OK)return false;
    }
    s.targets.clear();s.presenter.Reset();
    if(s.chain && xrDestroySwapchain(s.chain)!=XR_SUCCESS)return false;
    impl_.reset();return true;
}
}
