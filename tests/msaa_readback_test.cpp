#include "msaa_readback.h"
#include <d3dcompiler.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
void STDMETHODCALLTYPE Dispatch(ID3D11DeviceContext* c,UINT x,UINT y,UINT z) { c->Dispatch(x,y,z); }
int main() {
    auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    if (!create) return 1;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    if (FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))) return 2;
    const char* code=R"(
float4 vs(uint id:SV_VertexID):SV_Position {
    return float4(id==1 ? 3:-1,id==2 ? -3:1,0,1);
}
float4 ps(float4 p:SV_Position,uint s:SV_SampleIndex):SV_Target {
    uint x=(uint)p.x,y=(uint)p.y;
    return float4((x+s*17)%1024,(x*3+s*29)%1024,(x*7+y+s*31)%1024,(s+y)%4)/float4(1023,1023,1023,3);
}
[numthreads(1,1,1)] void cs() {}
)";
    auto compile=[&](const char* name,const char* profile) {
        ComPtr<ID3DBlob> blob;
        if (FAILED(D3DCompile(code,strlen(code),nullptr,nullptr,nullptr,name,profile,0,0,&blob,nullptr))) throw std::runtime_error("compile");
        return blob;
    };
    auto vb=compile("vs","vs_5_0"),pb=compile("ps","ps_5_0"),cb=compile("cs","cs_5_0");
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps; ComPtr<ID3D11ComputeShader> cs;
    device->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,&vs);
    device->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,&ps);
    device->CreateComputeShader(cb->GetBufferPointer(),cb->GetBufferSize(),nullptr,&cs);
    constexpr UINT width=1024,height=3,samples=4;
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=width; desc.Height=height; desc.MipLevels=1; desc.ArraySize=1;
    desc.SampleDesc.Count=samples; desc.Format=DXGI_FORMAT_R10G10B10A2_UNORM; desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11RenderTargetView> target;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&texture)) || FAILED(device->CreateRenderTargetView(texture.Get(),nullptr,&target))) return 3;
    auto* rtv=target.Get(); context->OMSetRenderTargets(1,&rtv,nullptr);
    D3D11_VIEWPORT viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1}; context->RSSetViewports(1,&viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(),nullptr,0); context->PSSetShader(ps.Get(),nullptr,0);
    context->Draw(3,0);
    auto dummyDesc=desc; dummyDesc.Width=4; dummyDesc.Height=4; dummyDesc.Format=DXGI_FORMAT_R32_UINT; dummyDesc.SampleDesc={1,0};
    dummyDesc.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> dummy,other; ComPtr<ID3D11UnorderedAccessView> uav; ComPtr<ID3D11ShaderResourceView> srv;
    device->CreateTexture2D(&dummyDesc,nullptr,&dummy); device->CreateTexture2D(&dummyDesc,nullptr,&other);
    device->CreateUnorderedAccessView(dummy.Get(),nullptr,&uav); device->CreateShaderResourceView(other.Get(),nullptr,&srv);
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth=16; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> buffer; device->CreateBuffer(&bd,nullptr,&buffer);
    auto* ub=uav.Get(); auto* sb=srv.Get(); auto* bb=buffer.Get();
    context->CSSetUnorderedAccessViews(3,1,&ub,nullptr); context->CSSetShaderResources(0,1,&sb);
    context->CSSetConstantBuffers(0,1,&bb); context->CSSetShader(cs.Get(),nullptr,0);
    grid2vr::MsaaReadback reader; bool restored{},equal=true; UINT readCount{};
    auto read=[&](ID3D11Texture2D* packed,UINT sample) {
        D3D11_TEXTURE2D_DESC d{}; packed->GetDesc(&d); d.Usage=D3D11_USAGE_STAGING; d.BindFlags=0; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&d,nullptr,&staging))) { equal=false; return; }
        context->CopyResource(staging.Get(),packed);
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map))) { equal=false; return; }
        for (UINT y=0;y<height;++y) for (UINT x=0;x<width;++x) {
            uint32_t actual{}; memcpy(&actual,static_cast<const char*>(map.pData)+y*map.RowPitch+x*4,4);
            const uint32_t expected=((x+sample*17)%1024) | (((x*3+sample*29)%1024)<<10) |
                (((x*7+y+sample*31)%1024)<<20) | (((sample+y)%4)<<30);
            if (actual!=expected) equal=false;
        }
        context->Unmap(staging.Get(),0); ++readCount;
    };
    auto checkState=[&]() {
        ComPtr<ID3D11ComputeShader> shader; ComPtr<ID3D11ShaderResourceView> input;
        ComPtr<ID3D11Buffer> parameters; ComPtr<ID3D11UnorderedAccessView> output; ComPtr<ID3D11RenderTargetView> actualTarget;
        context->CSGetShader(&shader,nullptr,nullptr); context->CSGetShaderResources(0,1,&input);
        context->CSGetConstantBuffers(0,1,&parameters); context->CSGetUnorderedAccessViews(3,1,&output);
        context->OMGetRenderTargets(1,&actualTarget,nullptr);
        return shader.Get()==cs.Get() && input.Get()==srv.Get() && parameters.Get()==buffer.Get() &&
            output.Get()==uav.Get() && actualTarget.Get()==target.Get();
    };
    const auto hr=reader.Read(context.Get(),texture.Get(),Dispatch,read,restored);
    if (FAILED(hr) || !restored || !equal || readCount!=samples || !checkState()) {
        printf("sample readback hr=%08lx restored=%u equal=%u samples=%u\n",hr,restored,equal,readCount); return 4;
    }
    // Repeat verifies the diagnostic did not alter the native source.
    readCount=0;
    if (FAILED(reader.Read(context.Get(),texture.Get(),Dispatch,read,restored)) || !equal || readCount!=samples) return 5;
    bool threw=false;
    try { reader.Read(context.Get(),texture.Get(),Dispatch,[](auto*,UINT){throw std::runtime_error("fixture");},restored); }
    catch (const std::runtime_error&) { threw=true; }
    if (!threw || !restored || !checkState()) return 6;
    if (SUCCEEDED(reader.Read(context.Get(),dummy.Get(),Dispatch,read,restored))) return 7;
    puts("All RGB10/alpha codes across four MSAA samples, source preservation, pipeline restoration and refusal pass.");
    return 0;
}
