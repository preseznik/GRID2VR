#include "eye_blit.h"
#include <d3dcompiler.h>
#include <cstring>
using Microsoft::WRL::ComPtr;
namespace grid2vr {
bool EyeBlit::Initialize(ID3D11Device* device,NativeEyeDraw draw) {
    if (!device || device_ || !draw) return false;
    ComPtr<ID3D11Device1> device1;ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) || FAILED(context.As(&context_))) return false;
    const auto level=device->GetFeatureLevel();
    if (FAILED(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&state_))) return false;
    // The captured final RGB10 image has display-encoded colour. Decode for
    // an sRGB compositor target; the target's hardware write encodes it once.
    constexpr char shader[]=R"(
Texture2D image:register(t0); SamplerState clampLinear:register(s0);
struct V { float4 position:SV_Position; float2 uv:TEXCOORD; };
V vs(uint id:SV_VertexID) {
    V v;v.uv=float2((id<<1)&2,id&2);
    v.position=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;
}
float4 ps(V v):SV_Target {
    float3 c=image.Sample(clampLinear,v.uv).rgb;
    return float4(lerp(c/12.92,pow((c+.055)/1.055,2.4),step(.04045,c)),1);
})";
    ComPtr<ID3DBlob> vertex,pixel;
    if (FAILED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vertex,nullptr)) ||
        FAILED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&pixel,nullptr)) ||
        FAILED(device->CreateVertexShader(vertex->GetBufferPointer(),vertex->GetBufferSize(),nullptr,&vertex_)) ||
        FAILED(device->CreatePixelShader(pixel->GetBufferPointer(),pixel->GetBufferSize(),nullptr,&pixel_))) return false;
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
    if (FAILED(device->CreateSamplerState(&sampler,&sampler_)) || FAILED(device->CreateRasterizerState(&raster,&raster_)) ||
        FAILED(device->CreateDepthStencilState(&depth,&depth_))) return false;
    device_=device;draw_=draw;thread_=GetCurrentThreadId();return true;
}
bool EyeBlit::Draw(unsigned eye,ID3D11Texture2D* source,ID3D11RenderTargetView* target) {
    if (!device_ || thread_!=GetCurrentThreadId() || eye>=2 || !source || !target) return false;
    ComPtr<ID3D11Device> sourceDevice,targetDevice;source->GetDevice(&sourceDevice);target->GetDevice(&targetDevice);
    if (sourceDevice!=device_ || targetDevice!=device_) return false;
    D3D11_TEXTURE2D_DESC input{};source->GetDesc(&input);
    D3D11_RENDER_TARGET_VIEW_DESC view{};target->GetDesc(&view);
    ComPtr<ID3D11Resource> resource;target->GetResource(&resource);
    ComPtr<ID3D11Texture2D> output;D3D11_TEXTURE2D_DESC dest{};
    if (FAILED(resource.As(&output))) return false;output->GetDesc(&dest);
    if (source==output.Get() || !input.Width || !input.Height || input.MipLevels!=1 || input.ArraySize!=1 || input.SampleDesc.Count!=1 ||
        !(input.BindFlags&D3D11_BIND_SHADER_RESOURCE) ||
        (input.Format!=DXGI_FORMAT_R10G10B10A2_UNORM && input.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && input.Format!=DXGI_FORMAT_B8G8R8A8_UNORM) ||
        !dest.Width || !dest.Height || dest.Width>8192 || dest.Height>8192 || dest.ArraySize!=1 || dest.MipLevels!=1 || dest.SampleDesc.Count!=1 ||
        view.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice!=0 ||
        (view.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && view.Format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) return false;
    if (sources_[eye].Get()!=source) {
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(device_->CreateShaderResourceView(source,nullptr,&srv))) return false;
        sources_[eye]=source;views_[eye]=std::move(srv);
    }
    ComPtr<ID3DDeviceContextState> previous;context_->SwapDeviceContextState(state_.Get(),&previous);
    context_->OMSetRenderTargets(1,&target,nullptr);context_->OMSetDepthStencilState(depth_.Get(),0);
    context_->OMSetBlendState(nullptr,nullptr,0xffffffff);
    const D3D11_VIEWPORT viewport{0,0,static_cast<float>(dest.Width),static_cast<float>(dest.Height),0,1};
    context_->RSSetViewports(1,&viewport);context_->RSSetState(raster_.Get());
    context_->IASetInputLayout(nullptr);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.Get(),nullptr,0);context_->PSSetShader(pixel_.Get(),nullptr,0);
    auto* srv=views_[eye].Get();auto* sampler=sampler_.Get();
    context_->PSSetShaderResources(0,1,&srv);context_->PSSetSamplers(0,1,&sampler);
    draw_(context_.Get(),3,0);
    context_->OMSetRenderTargets(0,nullptr,nullptr);srv=nullptr;context_->PSSetShaderResources(0,1,&srv);
    context_->SwapDeviceContextState(previous.Get(),nullptr);
    return SUCCEEDED(device_->GetDeviceRemovedReason());
}
}
