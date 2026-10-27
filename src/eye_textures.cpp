#include "eye_textures.h"
#include "resolution_limits.h"
#include <utility>
namespace grid2vr {
bool EyeTextures::Capture(ID3D11DeviceContext* context,uint64_t frame,unsigned eye,ID3D11Texture2D* source) {
    auto refuse=[&] { count_=0; collecting_=false; return false; };
    if (!collecting_ || !frame || frame!=frame_ || eye>=target_ || eye!=count_ || !context || !source ||
        context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return refuse();
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    const bool multisampled=desc.SampleDesc.Count>1;
    if (!resolution::ValidScene(desc.Width,desc.Height) ||
        desc.MipLevels!=1 || desc.ArraySize!=1 || desc.SampleDesc.Quality!=0 ||
        (desc.SampleDesc.Count!=1 && desc.SampleDesc.Count!=2 && desc.SampleDesc.Count!=4 && desc.SampleDesc.Count!=8) ||
        (desc.Format!=DXGI_FORMAT_R10G10B10A2_UNORM && desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM)) return refuse();
    Microsoft::WRL::ComPtr<ID3D11Device> sourceDevice,contextDevice;
    source->GetDevice(&sourceDevice); context->GetDevice(&contextDevice);
    if (sourceDevice!=contextDevice || FAILED(contextDevice->GetDeviceRemovedReason())) return refuse();
    const bool compatible=allocated_==target_ && device_==contextDevice && description_.Width==desc.Width &&
        description_.Height==desc.Height && description_.Format==desc.Format &&
        description_.SampleDesc.Count==desc.SampleDesc.Count && description_.SampleDesc.Quality==desc.SampleDesc.Quality;
    if (!compatible) {
        if (eye) return refuse(); // A group cannot change dimensions or sampling halfway through.
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> resolveContext;
        Microsoft::WRL::ComPtr<ID3DDeviceContextState> resolveState;
        if(multisampled) {
            UINT support{},quality{};
            Microsoft::WRL::ComPtr<ID3D11Device1> device1;
            const auto level=contextDevice->GetFeatureLevel();
            if(FAILED(contextDevice->CheckFormatSupport(desc.Format,&support)) || !(support&D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE) ||
               FAILED(contextDevice->CheckMultisampleQualityLevels(desc.Format,desc.SampleDesc.Count,&quality)) || !quality ||
               FAILED(contextDevice.As(&device1)) || FAILED(context->QueryInterface(IID_PPV_ARGS(&resolveContext))) ||
               FAILED(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&resolveState)))return refuse();
        }
        auto copy=desc;copy.SampleDesc={1,0};copy.Usage=D3D11_USAGE_DEFAULT;copy.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        copy.CPUAccessFlags=0;copy.MiscFlags=0;
        std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>,3> replacement;
        for (unsigned i=0;i<target_;++i)
            if (FAILED(contextDevice->CreateTexture2D(&copy,nullptr,&replacement[i]))) return refuse();
        images_=std::move(replacement); device_=contextDevice; description_=desc; allocated_=target_;
        resolveContext_=std::move(resolveContext);resolveState_=std::move(resolveState);
    }
    if(multisampled) {
        // Native scene targets stay multisampled; only the completed eye is
        // resolved. A neutral private state prevents inherited predication or
        // bindings from suppressing the resolve, then restores the whole state.
        Microsoft::WRL::ComPtr<ID3DDeviceContextState> previous;
        resolveContext_->SwapDeviceContextState(resolveState_.Get(),&previous);
        resolveContext_->ResolveSubresource(images_[eye].Get(),0,source,0,desc.Format);
        resolveContext_->SwapDeviceContextState(previous.Get(),nullptr);
    } else context->CopyResource(images_[eye].Get(),source);
    if (FAILED(contextDevice->GetDeviceRemovedReason())) return refuse();
    ++count_; collecting_=count_<target_; return true;
}
}
