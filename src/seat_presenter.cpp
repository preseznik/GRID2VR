#include "seat_presenter.h"
#include <d3dcompiler.h>
#include <cstring>
#include <utility>

namespace grid2vr {
using Microsoft::WRL::ComPtr;
namespace {
bool SourceFormats(DXGI_FORMAT format,DXGI_FORMAT& typeless,DXGI_FORMAT& unorm) {
    if(format==DXGI_FORMAT_R8G8B8A8_UNORM || format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        typeless=DXGI_FORMAT_R8G8B8A8_TYPELESS; unorm=DXGI_FORMAT_R8G8B8A8_UNORM; return true;
    }
    if(format==DXGI_FORMAT_B8G8R8A8_UNORM || format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        typeless=DXGI_FORMAT_B8G8R8A8_TYPELESS; unorm=DXGI_FORMAT_B8G8R8A8_UNORM; return true;
    }
    if(format==DXGI_FORMAT_R10G10B10A2_UNORM) {
        typeless=DXGI_FORMAT_R10G10B10A2_TYPELESS; unorm=DXGI_FORMAT_R10G10B10A2_UNORM; return true;
    }
    return false;
}
class ContextRestore {
public:
    ContextRestore(ID3D11DeviceContext1* context,ID3DDeviceContextState* isolated) : context_(context) {
        context_->SwapDeviceContextState(isolated,&previous_);
    }
    ~ContextRestore() { context_->SwapDeviceContextState(previous_.Get(),nullptr); }
    ContextRestore(const ContextRestore&)=delete;
    ContextRestore& operator=(const ContextRestore&)=delete;
private:
    ID3D11DeviceContext1* context_;
    ComPtr<ID3DDeviceContextState> previous_;
};
}
void SeatPresenter::Report(const char* message) noexcept { if(logger_) try { logger_(message); } catch(...) { } }
void SeatPresenter::Reset() noexcept {
    sourceView_.Reset(); sourceCopy_.Reset(); state_.Reset(); vertex_.Reset(); pixel_.Reset();
    alphaPixel_.Reset(); compositePixel_.Reset(); compositeBlend_.Reset();
    sampler_.Reset(); raster_.Reset(); depth_.Reset(); context_.Reset(); device_.Reset();
    sourceWidth_=sourceHeight_=0; sourceFormat_=DXGI_FORMAT_UNKNOWN;
}
bool SeatPresenter::Initialize(ID3D11Device* device,NativeEyeDraw draw) noexcept {
    if(!draw)return false;draw_=draw;thread_=GetCurrentThreadId();
    if(!device || device_ || device->GetFeatureLevel()<D3D_FEATURE_LEVEL_11_0) return false;
    try {
        ComPtr<ID3D11Device1> device1;
        ComPtr<ID3D11DeviceContext> immediate; device->GetImmediateContext(&immediate);
        if(FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) || FAILED(immediate.As(&context_))) {
            Report("Texture presenter requires D3D11.1 context-state isolation"); Reset(); return false;
        }
        const auto level=device->GetFeatureLevel();
        if(FAILED(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&state_))) {
            Report("Texture presenter context-state allocation failed"); Reset(); return false;
        }
        static const char shader[]=R"(
Texture2D image:register(t0); SamplerState linearClamp:register(s0);
struct V { float4 position:SV_Position; float2 uv:TEXCOORD; };
V vs(uint id:SV_VertexID) { V v; v.uv=float2((id<<1)&2,id&2); v.position=float4(v.uv*float2(2,-2)+float2(-1,1),0,1); return v; }
float4 ps(V v):SV_Target {
    float3 c=image.Sample(linearClamp,v.uv).rgb;
    return float4(lerp(c/12.92,pow((c+0.055)/1.055,2.4),step(0.04045,c)),1);
}
float4 alpha(V v):SV_Target {
    float4 s=image.Sample(linearClamp,v.uv);
    float3 c=s.a>0 ? saturate(s.rgb/s.a) : 0;
    return float4(lerp(c/12.92,pow((c+0.055)/1.055,2.4),step(0.04045,c)),s.a);
}
float4 composite(V v):SV_Target { return image.Sample(linearClamp,v.uv); }
)";
        ComPtr<ID3DBlob> vs,ps,error;
        if(FAILED(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vs,&error)) ||
           FAILED(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,&error)) ||
           FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex_)) ||
           FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel_))) {
            Report("Texture presenter shader creation failed"); Reset(); return false;
        }
        for(const auto& entry:{std::pair{"alpha",alphaPixel_.GetAddressOf()},std::pair{"composite",compositePixel_.GetAddressOf()}}) {
            ps.Reset();
            if(FAILED(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,entry.first,"ps_5_0",0,0,&ps,&error)) ||
               FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,entry.second))) { Reset(); return false; }
        }
        D3D11_BLEND_DESC blend{}; auto& rt=blend.RenderTarget[0]; rt.BlendEnable=TRUE;
        rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_ONE; rt.DestBlend=rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD; rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        if(FAILED(device->CreateBlendState(&blend,&compositeBlend_))) { Reset(); return false; }
        D3D11_SAMPLER_DESC sample{}; sample.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sample.MaxLOD=D3D11_FLOAT32_MAX;
        D3D11_RASTERIZER_DESC raster{}; raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_NONE; raster.DepthClipEnable=TRUE;
        D3D11_DEPTH_STENCIL_DESC depth{}; depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
        if(FAILED(device->CreateSamplerState(&sample,&sampler_)) || FAILED(device->CreateRasterizerState(&raster,&raster_)) ||
           FAILED(device->CreateDepthStencilState(&depth,&depth_))) { Report("Texture presenter state allocation failed"); Reset(); return false; }
        device_=device; return true;
    } catch(...) { Report("Texture presenter initialization failed"); Reset(); return false; }
}
bool SeatPresenter::PrepareSource(ID3D11Texture2D* source,const D3D11_TEXTURE2D_DESC& desc) {
    DXGI_FORMAT typeless{},unorm{};
    if(!SourceFormats(desc.Format,typeless,unorm)) return false;
    if(!sourceCopy_ || sourceWidth_!=desc.Width || sourceHeight_!=desc.Height || sourceFormat_!=desc.Format) {
        D3D11_TEXTURE2D_DESC copy{}; copy.Width=desc.Width; copy.Height=desc.Height;
        copy.MipLevels=copy.ArraySize=copy.SampleDesc.Count=1; copy.Format=typeless;
        copy.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> replacement; ComPtr<ID3D11ShaderResourceView> view;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=unorm; srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels=1;
        if(FAILED(device_->CreateTexture2D(&copy,nullptr,&replacement)) || FAILED(device_->CreateShaderResourceView(replacement.Get(),&srv,&view))) return false;
        sourceCopy_=replacement; sourceView_=view; sourceWidth_=desc.Width; sourceHeight_=desc.Height; sourceFormat_=desc.Format;
    }
    if(desc.SampleDesc.Count>1) context_->ResolveSubresource(sourceCopy_.Get(),0,source,0,desc.Format);
    else context_->CopyResource(sourceCopy_.Get(),source);
    return true;
}
bool SeatPresenter::Draw(ID3D11Texture2D* source,ID3D11RenderTargetView* target,unsigned width,unsigned height,Mode mode) noexcept {
    return DrawInto(source,target,0,0,width,height,mode,true);
}
bool SeatPresenter::DrawRegion(ID3D11Texture2D* source,ID3D11RenderTargetView* target,unsigned left,unsigned top,unsigned width,unsigned height,Mode mode) noexcept {
    return DrawInto(source,target,left,top,width,height,mode,false);
}
bool SeatPresenter::DrawInto(ID3D11Texture2D* source,ID3D11RenderTargetView* target,unsigned left,unsigned top,unsigned width,unsigned height,Mode mode,bool exact) noexcept {
    if(!draw_ || thread_!=GetCurrentThreadId())return false;
    if(!device_ || !source || !target || !width || !height) return false;
    try {
        D3D11_TEXTURE2D_DESC input{}; source->GetDesc(&input);
        DXGI_FORMAT unused{},unused2{};
        if(!input.Width || !input.Height || input.Width>8192 || input.Height>8192 || input.ArraySize!=1 || input.MipLevels!=1 ||
           !input.SampleDesc.Count || !SourceFormats(input.Format,unused,unused2)) return false;
        ComPtr<ID3D11Device> sourceDevice,targetDevice; source->GetDevice(&sourceDevice); target->GetDevice(&targetDevice);
        if(sourceDevice.Get()!=device_.Get() || targetDevice.Get()!=device_.Get()) return false;
        D3D11_RENDER_TARGET_VIEW_DESC outputView{}; target->GetDesc(&outputView);
        if(outputView.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || !SourceFormats(outputView.Format,unused,unused2)) return false;
        if(mode==Mode::Composite && (outputView.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || outputView.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) return false;
        ComPtr<ID3D11Resource> outputResource; target->GetResource(&outputResource);
        ComPtr<ID3D11Texture2D> output; if(FAILED(outputResource.As(&output)) || output.Get()==source) return false;
        D3D11_TEXTURE2D_DESC outputDesc{}; output->GetDesc(&outputDesc);
        if(outputDesc.ArraySize!=1 || outputDesc.SampleDesc.Count!=1) return false;
        if(exact ? outputDesc.Width!=width || outputDesc.Height!=height :
                   left>=outputDesc.Width || top>=outputDesc.Height || width>outputDesc.Width-left || height>outputDesc.Height-top) return false;
        // Copy/resolve first: game backbuffers need not have shader-resource bind
        // flags, and the presenter must not retain a buffer across ResizeBuffers.
        if(!PrepareSource(source,input)) { Report("Texture presenter source allocation failed"); return false; }
        ContextRestore restore(context_.Get(),state_.Get());
        context_->OMSetRenderTargets(1,&target,nullptr); context_->OMSetDepthStencilState(depth_.Get(),0);
        context_->OMSetBlendState(mode==Mode::Composite ? compositeBlend_.Get() : nullptr,nullptr,0xffffffff);
        D3D11_VIEWPORT viewport{float(left),float(top),float(width),float(height),0,1};
        context_->RSSetViewports(1,&viewport); context_->RSSetState(raster_.Get());
        context_->IASetInputLayout(nullptr); context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_.Get(),nullptr,0);
        context_->PSSetShader(mode==Mode::Opaque ? pixel_.Get() : mode==Mode::StraightAlpha ? alphaPixel_.Get() : compositePixel_.Get(),nullptr,0);
        auto* view=sourceView_.Get(); auto* sampler=sampler_.Get();
        context_->PSSetShaderResources(0,1,&view); context_->PSSetSamplers(0,1,&sampler);
        draw_(context_.Get(),3,0);
        context_->OMSetRenderTargets(0,nullptr,nullptr);
        view=nullptr; context_->PSSetShaderResources(0,1,&view);
        return SUCCEEDED(device_->GetDeviceRemovedReason());
    } catch(...) { Report("Texture presenter draw failed"); return false; }
}
}
