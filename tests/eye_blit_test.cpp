#include "eye_blit.h"
#include <cstdio>
#include <thread>
using Microsoft::WRL::ComPtr;
void STDMETHODCALLTYPE DrawNative(ID3D11DeviceContext* context,UINT count,UINT first) {context->Draw(count,first);}
int main() {
    const auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    if (!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))) return 1;
    grid2vr::EyeBlit blit;
    if (blit.Initialize(device.Get(),nullptr) || !blit.Initialize(device.Get(),DrawNative)) return 2;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=2;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R10G10B10A2_UNORM;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    const UINT pixels[]{0xc00003ff,0xc00ffc00,0xfff00000,0xe0080200};
    D3D11_SUBRESOURCE_DATA data{pixels,8,0};ComPtr<ID3D11Texture2D> source;
    if (FAILED(device->CreateTexture2D(&desc,&data,&source))) return 3;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> output,gameTarget;ComPtr<ID3D11RenderTargetView> target,gameView;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&output)) || FAILED(device->CreateTexture2D(&desc,nullptr,&gameTarget)) ||
        FAILED(device->CreateRenderTargetView(output.Get(),nullptr,&target)) || FAILED(device->CreateRenderTargetView(gameTarget.Get(),nullptr,&gameView))) return 4;
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=64;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants;if (FAILED(device->CreateBuffer(&bd,nullptr,&constants))) return 5;
    ComPtr<ID3D11ShaderResourceView> srv;if (FAILED(device->CreateShaderResourceView(source.Get(),nullptr,&srv))) return 6;
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;if (FAILED(device->CreateSamplerState(&sd,&sampler))) return 7;
    D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION_PREDICATE,0};ComPtr<ID3D11Predicate> predicate;
    if (FAILED(device->CreatePredicate(&qd,&predicate))) return 8;
    context->Begin(predicate.Get());context->End(predicate.Get());context->Flush();
    BOOL queryResult{};HRESULT queryStatus=S_FALSE;
    for (unsigned i=0;i<100 && queryStatus==S_FALSE;++i) {
        queryStatus=context->GetData(predicate.Get(),&queryResult,sizeof(queryResult),0);
        if (queryStatus==S_FALSE) Sleep(1);
    }
    if (queryStatus!=S_OK || queryResult) return 16;
    auto* rtv=gameView.Get();context->OMSetRenderTargets(1,&rtv,nullptr);
    auto* buffer=constants.Get();context->VSSetConstantBuffers(3,1,&buffer);context->PSSetConstantBuffers(4,1,&buffer);context->CSSetConstantBuffers(12,1,&buffer);
    auto* view=srv.Get();context->PSSetShaderResources(13,1,&view);context->CSSetShaderResources(7,1,&view);
    auto* state=sampler.Get();context->PSSetSamplers(7,1,&state);
    const D3D11_VIEWPORT viewport{3,2,1,2,.2f,.8f};context->RSSetViewports(1,&viewport);
    const D3D11_RECT scissor{1,2,3,4};context->RSSetScissorRects(1,&scissor);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    context->SetPredication(predicate.Get(),TRUE);
    auto preserved=[&] {
        ComPtr<ID3D11RenderTargetView> color;context->OMGetRenderTargets(1,&color,nullptr);
        if (color!=gameView) return false;
        ComPtr<ID3D11Buffer> cb;context->VSGetConstantBuffers(3,1,&cb);if (cb!=constants) return false;
        cb.Reset();context->PSGetConstantBuffers(4,1,&cb);if (cb!=constants) return false;
        cb.Reset();context->CSGetConstantBuffers(12,1,&cb);if (cb!=constants) return false;
        ComPtr<ID3D11ShaderResourceView> input;context->PSGetShaderResources(13,1,&input);if (input!=srv) return false;
        input.Reset();context->CSGetShaderResources(7,1,&input);if (input!=srv) return false;
        ComPtr<ID3D11SamplerState> s;context->PSGetSamplers(7,1,&s);if (s!=sampler) return false;
        D3D11_VIEWPORT actual{};UINT count=1;context->RSGetViewports(&count,&actual);if (count!=1 || memcmp(&actual,&viewport,sizeof(actual))) return false;
        D3D11_RECT rect{};count=1;context->RSGetScissorRects(&count,&rect);if (count!=1 || memcmp(&rect,&scissor,sizeof(rect))) return false;
        D3D11_PRIMITIVE_TOPOLOGY topology{};context->IAGetPrimitiveTopology(&topology);if (topology!=D3D11_PRIMITIVE_TOPOLOGY_LINELIST) return false;
        ComPtr<ID3D11Predicate> p;BOOL value{};context->GetPredication(&p,&value);return p==predicate && value==TRUE;
    };
    if (!blit.Draw(0,source.Get(),target.Get()) || !preserved() || !blit.Draw(1,source.Get(),target.Get()) || !preserved()) return 9;
    // Read the four distinguishable corners: catches flips and accidental
    // double gamma conversion, not merely successful API calls.
    desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;if (FAILED(device->CreateTexture2D(&desc,nullptr,&staging))) return 10;
    // Readback must not itself inherit the game's conditional-render query.
    context->SetPredication(nullptr,FALSE);
    context->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return 11;
    const unsigned char expected[][4]{{255,0,0,255},{0,255,0,255},{0,0,255,255},{128,128,128,255}};
    bool colors=true;
    for (unsigned y=0;y<2;++y) for (unsigned x=0;x<2;++x) for (unsigned c=0;c<4;++c) {
        const int actual=static_cast<const unsigned char*>(mapped.pData)[y*mapped.RowPitch+4*x+c];
        colors&=std::abs(actual-int(expected[2*y+x][c]))<=1;
        if (std::abs(actual-int(expected[2*y+x][c]))>1) printf("Pixel %u,%u channel %u: actual=%d expected=%u\n",x,y,c,actual,expected[2*y+x][c]);
    }
    context->Unmap(staging.Get(),0);context->SetPredication(predicate.Get(),TRUE);if (!colors) return 12;
    if (blit.Draw(2,source.Get(),target.Get()) || blit.Draw(0,nullptr,target.Get()) || !preserved()) return 13;
    bool foreignThread=true;std::thread other([&] {foreignThread=blit.Draw(0,source.Get(),target.Get());});other.join();
    if (foreignThread || !preserved()) return 14;
    desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=0;desc.BindFlags=D3D11_BIND_RENDER_TARGET;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D11Texture2D> wrongTexture;ComPtr<ID3D11RenderTargetView> wrongView;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&wrongTexture)) || FAILED(device->CreateRenderTargetView(wrongTexture.Get(),nullptr,&wrongView)) ||
        blit.Draw(0,source.Get(),wrongView.Get()) || !preserved()) return 15;
    puts("Eye blit: RGB10 to sRGB colours/orientation, independent eyes, full-context state restoration and invalid target/thread refusal passed.");
}
