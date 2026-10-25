#include "xr_eye_images.h"
#include "tracked_views.h"
#include <algorithm>
namespace grid2vr {
bool XrEyeImages::Check(XrResult result,const char* operation) {
    if (result==XR_SUCCESS) return true;
    if (!failure_) {failure_=operation;result_=result;}ready_=false;return false;
}
bool XrEyeImages::Refuse(const char* reason) {return Check(XR_ERROR_VALIDATION_FAILURE,reason);}
bool XrEyeImages::Initialize(XrSession session,ID3D11Device* device,NativeEyeDraw draw,unsigned width,unsigned height) {
    if (thread_ || !session || !device || !draw) return false;
    thread_=GetCurrentThreadId();width_=width;height_=height;
    if (!width || !height || width>4096 || height>4096 || uint64_t(width)*height>4*1024*1024) return Refuse("image_dimensions");
    uint32_t count{};
    if (!Check(xrEnumerateSwapchainFormats(session,0,&count,nullptr),"format_count")) return false;
    if (!count || count>128) return Refuse("format_count_bound");
    std::vector<int64_t> formats(count);
    if (!Check(xrEnumerateSwapchainFormats(session,count,&count,formats.data()),"formats")) return false;
    if (count!=formats.size()) return Refuse("format_count_changed");
    int64_t format{};
    for (auto preferred:{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB})
        if (!format && std::find(formats.begin(),formats.end(),preferred)!=formats.end()) format=preferred;
    if (!format || !blit_.Initialize(device,draw)) return Refuse("srgb_or_blit_unavailable");
    device->GetImmediateContext(&context_);
    const D3D11_QUERY_DESC completion{D3D11_QUERY_EVENT,0};
    if (FAILED(device->CreateQuery(&completion,&completion_))) return Refuse("completion_query");
    for (auto& eye:eyes_) {
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};create.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        create.format=format;create.sampleCount=create.faceCount=create.arraySize=create.mipCount=1;create.width=width;create.height=height;
        if (!Check(xrCreateSwapchain(session,&create,&eye.chain),"create_swapchain")) {Shutdown();return false;}
        if (!Check(xrEnumerateSwapchainImages(eye.chain,0,&count,nullptr),"image_count")) {Shutdown();return false;}
        if (!count || count>16 || uint64_t(count)*width*height*8>128ull*1024*1024) {Refuse("image_count_bound");Shutdown();return false;}
        eye.images.resize(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});eye.targets.resize(count);
        if (!Check(xrEnumerateSwapchainImages(eye.chain,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data())),"images") || count!=eye.images.size()) {
            Refuse("image_count_changed");Shutdown();return false;
        }
        for (unsigned i=0;i<count;++i) {
            auto* texture=eye.images[i].texture;
            if (!texture) {Refuse("null_image");Shutdown();return false;}
            Microsoft::WRL::ComPtr<ID3D11Device> actualDevice;texture->GetDevice(&actualDevice);
            D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
            D3D11_RENDER_TARGET_VIEW_DESC target{};target.Format=static_cast<DXGI_FORMAT>(format);target.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
            if (actualDevice.Get()!=device || desc.Width!=width || desc.Height!=height || desc.SampleDesc.Count!=1 || desc.MipLevels!=1 || desc.ArraySize!=1 ||
                FAILED(device->CreateRenderTargetView(texture,&target,&eye.targets[i]))) {Refuse("image_device_or_target");Shutdown();return false;}
        }
    }
    initialized_=true;return true;
}
bool XrEyeImages::Release() {
    bool ok=true;
    for (auto& eye:eyes_) if (eye.waited) {
        const XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        ok=Check(xrReleaseSwapchainImage(eye.chain,&release),"release_image") && ok;eye.waited=false;
    }
    return ok;
}
bool XrEyeImages::Copy(uint64_t frame,XrSpace space,const std::array<XrView,2>& views,ID3D11Texture2D* left,ID3D11Texture2D* right) {
    if (thread_!=GetCurrentThreadId()) return false;
    ready_=screenReady_=false;
    if (!initialized_ || failure_ || !frame || frame<=frame_ || !space || !left || !right || left==right) return Refuse("frame_or_eye_pair");
    frame_=frame;
    for (const auto& view:views) {
        std::array<double,4> q;
        if (view.type!=XR_TYPE_VIEW || !TrackingPosition(view.pose.position) || !TrackingOrientation(view.pose.orientation,q)) return Refuse("view_pose");
        for (float a:{view.fov.angleLeft,view.fov.angleRight,view.fov.angleDown,view.fov.angleUp})
            if (!std::isfinite(a) || std::abs(a)>=1.326f || std::abs(std::tan(a))>4) return Refuse("view_fov");
        if (view.fov.angleLeft>=0 || view.fov.angleRight<=0 || view.fov.angleDown>=0 || view.fov.angleUp<=0) return Refuse("view_fov");
    }
    const std::array<ID3D11Texture2D*,2> sources{left,right};
    for (unsigned i=0;i<2;++i) {
        auto& eye=eyes_[i];uint32_t index{};
        const XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!Check(xrAcquireSwapchainImage(eye.chain,&acquire,&index),"acquire_image")) {Release();return false;}
        const XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,nullptr,100000000};
        if (!Check(xrWaitSwapchainImage(eye.chain,&wait),"wait_image")) {Release();return false;}
        eye.waited=true;
        gpuUsed_=true;
        if (index>=eye.targets.size() || !blit_.Draw(i,sources[i],index<eye.targets.size()?eye.targets[index].Get():nullptr)) {
            Refuse("copy_eye");Release();return false;
        }
        projections_[i]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};projections_[i].pose=views[i].pose;projections_[i].fov=views[i].fov;
        projections_[i].subImage.swapchain=eye.chain;projections_[i].subImage.imageRect.extent={static_cast<int32_t>(width_),static_cast<int32_t>(height_)};
    }
    if (!Release()) return false;
    layer_.space=space;layer_.viewCount=2;layer_.views=projections_.data();ready_=true;return true;
}
bool XrEyeImages::CopyScreen(uint64_t frame,XrSpace localSpace,const XrPosef& anchor,ID3D11Texture2D* backbuffer) {
    if (thread_!=GetCurrentThreadId()) return false;
    ready_=screenReady_=false;
    std::array<double,4> normalized{};
    if (!initialized_ || failure_ || !frame || frame<=frame_ || !localSpace || !backbuffer ||
        !TrackingPosition(anchor.position) || !TrackingOrientation(anchor.orientation,normalized)) return Refuse("screen_frame_or_source");
    frame_=frame;
    D3D11_TEXTURE2D_DESC desc{};backbuffer->GetDesc(&desc);
    Microsoft::WRL::ComPtr<ID3D11Device> device,sourceDevice;
    context_->GetDevice(&device);backbuffer->GetDevice(&sourceDevice);
    if (sourceDevice!=device || !desc.Width || !desc.Height || desc.Width>8192 || desc.Height>8192 ||
        uint64_t(desc.Width)*desc.Height>16ull*1024*1024 || desc.MipLevels!=1 || desc.ArraySize!=1 || desc.SampleDesc.Count!=1 ||
        (desc.Format!=DXGI_FORMAT_R10G10B10A2_UNORM && desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM))
        return Refuse("screen_source_description");
    D3D11_TEXTURE2D_DESC previous{};if(screenSource_)screenSource_->GetDesc(&previous);
    if (!screenSource_ || previous.Width!=desc.Width || previous.Height!=desc.Height || previous.Format!=desc.Format) {
        auto copy=desc;copy.Usage=D3D11_USAGE_DEFAULT;copy.BindFlags=D3D11_BIND_SHADER_RESOURCE;copy.CPUAccessFlags=copy.MiscFlags=0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> replacement;
        if (FAILED(device->CreateTexture2D(&copy,nullptr,&replacement))) return Refuse("screen_source_copy");
        screenSource_=std::move(replacement);
    }
    // Never retain the swapchain backbuffer: ResizeBuffers must be able to
    // release it. Copy under neutral predication, then restore native state.
    Microsoft::WRL::ComPtr<ID3D11Predicate> predicate;BOOL predicateValue{};
    context_->GetPredication(&predicate,&predicateValue);context_->SetPredication(nullptr,FALSE);
    context_->CopyResource(screenSource_.Get(),backbuffer);
    context_->SetPredication(predicate.Get(),predicateValue);
    auto& eye=eyes_[0];uint32_t index{};
    const XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!Check(xrAcquireSwapchainImage(eye.chain,&acquire,&index),"acquire_screen")) return false;
    const XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,nullptr,100000000};
    if (!Check(xrWaitSwapchainImage(eye.chain,&wait),"wait_screen")) return false;
    eye.waited=true;gpuUsed_=true;
    if (index>=eye.targets.size() || !blit_.Draw(0,screenSource_.Get(),index<eye.targets.size()?eye.targets[index].Get():nullptr)) {
        Refuse("copy_screen");Release();return false;
    }
    if (!Release()) return false;
    screen_={XR_TYPE_COMPOSITION_LAYER_QUAD};screen_.space=localSpace;screen_.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
    screen_.subImage.swapchain=eye.chain;screen_.subImage.imageRect.extent={static_cast<int32_t>(width_),static_cast<int32_t>(height_)};
    screen_.pose=anchor;
    screen_.size={2.4f,2.4f*static_cast<float>(desc.Height)/static_cast<float>(desc.Width)};
    ready_=screenReady_=true;return true;
}
bool XrEyeImages::Shutdown() {
    if (thread_!=GetCurrentThreadId()) return false;
    ready_=screenReady_=initialized_=false;bool ok=Release();
    // xrDestroySwapchain requires all our commands referencing its images to
    // have completed. A Flush alone submits work but does not prove completion.
    if (gpuUsed_) {
        context_->End(completion_.Get());context_->Flush();
        const auto deadline=GetTickCount64()+2000;
        HRESULT result=S_FALSE;
        do {
            result=context_->GetData(completion_.Get(),nullptr,0,0);
            if (result==S_FALSE) Sleep(1);
        } while (result==S_FALSE && GetTickCount64()<deadline);
        if (result!=S_OK) return Refuse("gpu_shutdown_incomplete");
        gpuUsed_=false;
    }
    for (auto& eye:eyes_) {
        eye.targets.clear();eye.images.clear();
        if (eye.chain) {
            if (Check(xrDestroySwapchain(eye.chain),"destroy_swapchain")) eye.chain=XR_NULL_HANDLE;
            else ok=false;
        }
    }
    return ok;
}
}
