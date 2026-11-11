#include "lens_flare.h"
#include <d3dcompiler.h>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
using namespace grid2vr;
using Microsoft::WRL::ComPtr;
void STDMETHODCALLTYPE DispatchNative(ID3D11DeviceContext* c,UINT x,UINT y,UINT z) {c->Dispatch(x,y,z);}
namespace {
ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
constexpr UINT width=96,height=64;
std::vector<unsigned char> Load(const char* path) {
    std::ifstream file(std::filesystem::path(path),std::ios::binary);
    return {std::istreambuf_iterator<char>(file),{}};
}
uint64_t Fnv(const std::vector<unsigned char>& bytes) {
    uint64_t hash=14695981039346656037ull;for (auto byte:bytes) hash=(hash^byte)*1099511628211ull;return hash;
}
ComPtr<ID3D11Buffer> Constants(const std::vector<float>& values) {
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=UINT(values.size()*4);desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA data{values.data(),0,0};ComPtr<ID3D11Buffer> buffer;
    return SUCCEEDED(device->CreateBuffer(&desc,&data,&buffer)) ? buffer:nullptr;
}
ComPtr<ID3D11ShaderResourceView> Texture(float scale,unsigned seed) {
    std::vector<float> pixels(width*height*4);
    for (UINT y=0;y<height;++y) for (UINT x=0;x<width;++x) for (UINT c=0;c<4;++c) {
        const unsigned v=(x*2654435761u+y*40503u+c*977u+seed*7919u)>>7;
        pixels[(y*width+x)*4+c]=scale*float(v%997)/996.f;
    }
    D3D11_TEXTURE2D_DESC desc{width,height,1,1,DXGI_FORMAT_R32G32B32A32_FLOAT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE};
    const D3D11_SUBRESOURCE_DATA data{pixels.data(),width*16,0};ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> view;
    return SUCCEEDED(device->CreateTexture2D(&desc,&data,&texture)) && SUCCEEDED(device->CreateShaderResourceView(texture.Get(),nullptr,&view)) ? view:nullptr;
}
}
int main(int argc,char** argv) {
    if (argc!=5) return 20;
    std::array<std::vector<unsigned char>,4> code;for (size_t i=0;i<4;++i) code[i]=Load(argv[i+1]);
    constexpr std::array<size_t,4> sizes{6940,7256,7084,2556};
    for (size_t i=0;i<4;++i) if (code[i].size()!=sizes[i] || Fnv(code[i])!=LensFlareGhosts::shaders[i].hash) return 21+int(i);
    if (LensFlareGhosts::Find(0xcfc3cf441f08e879ull) || LensFlareGhosts::Find(0)) return 25; // fp_uniFinal_hq is not a lens pass.
    for (size_t i=0;i<4;++i) if ((LensFlareGhosts::shaders[i].effect==LensFlareGhosts::Effect::Dust)!=(i==3)) return 26;

    const auto dll=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(dll,"D3D11CreateDevice"));
    if (!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))) return 1;
    constexpr char vertexCode[]="struct O{float4 p:SV_Position;float2 t:TEXCOORD0;};"
        "O main(uint i:SV_VertexID){O o;float2 uv=float2((i<<1)&2,i&2);o.p=float4(uv*float2(2,-2)+float2(-1,1),0,1);o.t=uv;return o;}";
    ComPtr<ID3DBlob> vertexBytes;ComPtr<ID3D11VertexShader> vertex;
    if (FAILED(D3DCompile(vertexCode,sizeof(vertexCode)-1,nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vertexBytes,nullptr)) ||
        FAILED(device->CreateVertexShader(vertexBytes->GetBufferPointer(),vertexBytes->GetBufferSize(),nullptr,&vertex))) return 2;
    D3D11_SAMPLER_DESC samplerDesc{D3D11_FILTER_MIN_MAG_MIP_POINT,D3D11_TEXTURE_ADDRESS_CLAMP,D3D11_TEXTURE_ADDRESS_CLAMP,D3D11_TEXTURE_ADDRESS_CLAMP};
    samplerDesc.MaxLOD=D3D11_FLOAT32_MAX;ComPtr<ID3D11SamplerState> sampler;
    if (FAILED(device->CreateSamplerState(&samplerDesc,&sampler))) return 3;
    D3D11_TEXTURE2D_DESC targetDesc{width,height,1,1,DXGI_FORMAT_R32G32B32A32_FLOAT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> target;ComPtr<ID3D11RenderTargetView> targetView;
    if (FAILED(device->CreateTexture2D(&targetDesc,nullptr,&target)) || FAILED(device->CreateRenderTargetView(target.Get(),nullptr,&targetView))) return 4;
    std::array<ComPtr<ID3D11ShaderResourceView>,3> textures{Texture(3,1),Texture(1,2),Texture(1,3)};
    for (auto& t:textures) if (!t) return 5;

    LensFlareGhosts control;
    for (unsigned index=0;index<3;++index) {
        const auto& shader=LensFlareGhosts::shaders[index];
        ComPtr<ID3D11PixelShader> pixel;
        if (FAILED(device->CreatePixelShader(code[index].data(),code[index].size(),nullptr,&pixel))) return 30+index;
        // The game's UniFinal $Globals: the ghost scale/offset rows (208..271)
        // mirror and shrink the image, tint rows 272..319 colour each ghost,
        // row 320 holds the thresholds. A larger native allocation is fine.
        std::vector<float> globals(shader.constantBytes/4+(index==0?8:0));
        for (size_t i=0;i<globals.size();++i) globals[i]=.2f+.004f*float(i%29);
        const float scale[4]{-1.f,-.6f,-.35f,.5f},offset[4]{1.f,.8f,.675f,.25f};
        for (int k=0;k<4;++k) {globals[13*4+k]=scale[k];globals[14*4+k]=scale[k];globals[15*4+k]=offset[k];globals[16*4+k]=offset[k];}
        for (int k=0;k<4;++k) globals[20*4+k]=.1f; // thresholds
        auto plainValues=globals;
        for (UINT b=LensFlareGhosts::tintOffset;b<LensFlareGhosts::tintOffset+LensFlareGhosts::tintBytes;b+=4) plainValues[b/4]=0;
        const auto flared=Constants(globals),plain=Constants(plainValues);
        if (!flared || !plain) return 40+index;

        auto render=[&](ID3D11Buffer* constants,bool patched,std::vector<float>& pixels) {
            ID3D11Buffer* buffers[1]{constants};context->PSSetConstantBuffers(0,1,buffers);
            ID3D11ShaderResourceView* views[3]{textures[0].Get(),textures[1].Get(),textures[2].Get()};context->PSSetShaderResources(0,3,views);
            ID3D11SamplerState* samplers[6]{};for (auto& s:samplers) s=sampler.Get();context->PSSetSamplers(0,6,samplers);
            auto* rtv=targetView.Get();context->OMSetRenderTargets(1,&rtv,nullptr);
            const float clear[4]{};context->ClearRenderTargetView(rtv,clear);
            const D3D11_VIEWPORT full{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&full);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->IASetInputLayout(nullptr);
            context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);
            if (patched && (!control.Apply(context.Get(),shader.hash,DispatchNative) || !control.Pending())) return false;
            context->Draw(3,0);
            if (patched && !control.Restore(context.Get())) return false;
            ComPtr<ID3D11Buffer> cb0;context->PSGetConstantBuffers(0,1,&cb0);
            if (cb0.Get()!=constants || control.Pending()) return false;
            D3D11_TEXTURE2D_DESC staging=targetDesc;staging.BindFlags=0;staging.Usage=D3D11_USAGE_STAGING;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> copy;if (FAILED(device->CreateTexture2D(&staging,nullptr,&copy))) return false;
            context->CopyResource(copy.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(copy.Get(),0,D3D11_MAP_READ,0,&mapped))) return false;
            pixels.resize(width*height*4);
            for (UINT y=0;y<height;++y) memcpy(pixels.data()+y*width*4,static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch,width*16);
            context->Unmap(copy.Get(),0);return true;
        };
        std::vector<float> withoutGhosts,native,patched;
        if (!render(plain.Get(),false,withoutGhosts)) return 50+index;
        if (!render(flared.Get(),false,native)) return 53+index;
        if (!render(flared.Get(),true,patched)) return 56+index;
        unsigned ghosted=0,carried=0;
        for (size_t i=0;i<native.size();i+=4) {
            ghosted+=std::fabs(native[i]-withoutGhosts[i])+std::fabs(native[i+1]-withoutGhosts[i+1])+std::fabs(native[i+2]-withoutGhosts[i+2])>.01f;
            carried+=withoutGhosts[i]+withoutGhosts[i+1]+withoutGhosts[i+2]>.01f;
        }
        if (ghosted<width*height/2) return 60+index;                  // The native ghosts must be visible.
        if (index>0 && carried<width*height/2) return 63+index;       // M/M2 still carry their accumulated/bloom input.
        if (memcmp(patched.data(),withoutGhosts.data(),patched.size()*4)) return 66+index; // Exactly the ghost-free pass.

        ComPtr<ID3D11Buffer> before;context->PSGetConstantBuffers(0,1,&before);
        if (control.Apply(context.Get(),shader.hash,nullptr) || control.Apply(context.Get(),0xcfc3cf441f08e879ull,DispatchNative) ||
            control.Pending() || control.Restore(context.Get())) return 70+index;
        if (!control.Apply(context.Get(),shader.hash,DispatchNative) || control.Apply(context.Get(),shader.hash,DispatchNative) ||
            !control.Restore(context.Get()) || control.Restore(context.Get())) return 73+index;
        ComPtr<ID3D11Buffer> after;context->PSGetConstantBuffers(0,1,&after);
        if (after!=before) return 76+index;
        const auto shortBuffer=Constants(std::vector<float>(shader.constantBytes/4-4));auto* binding=shortBuffer.Get();
        context->PSSetConstantBuffers(0,1,&binding);
        if (control.Apply(context.Get(),shader.hash,DispatchNative) || control.Pending()) return 79+index;
    }

    // LensDust.psh.main: dustMap at screen UV, scaled by a sun-reflection term
    // and clamped to [lensdust_params1.x, .y]. Patched, the colour is exactly
    // zero and the alpha is the dust texture's own, as with both bounds at 0.
    {
        const auto& shader=LensFlareGhosts::shaders[3];
        ComPtr<ID3D11PixelShader> pixel;
        if (FAILED(device->CreatePixelShader(code[3].data(),code[3].size(),nullptr,&pixel))) return 90;
        const std::vector<float> dustValues{1.f,2.f,3.f,.3f, .05f,4.f,0.f,0.f, 0.f,0.f,-1.f,0.f, 1.f,.8f,.6f,0.f};
        auto clampedValues=dustValues;
        for (UINT b=LensFlareGhosts::dustClampOffset;b<LensFlareGhosts::dustClampOffset+LensFlareGhosts::dustClampBytes;b+=4) clampedValues[b/4]=0;
        std::vector<float> camera(15*4);
        for (int k=0;k<3;++k) camera[(8+k)*4+k]=1.f; // view rows: identity; eyePositionWS (row 14) at the origin.
        const auto dusty=Constants(dustValues),clamped=Constants(clampedValues),cameraBuffer=Constants(camera);
        if (!dusty || !clamped || !cameraBuffer) return 91;
        auto render=[&](ID3D11Buffer* constants,bool patched,std::vector<float>& pixels) {
            ID3D11Buffer* buffers[4]{constants,nullptr,nullptr,cameraBuffer.Get()};context->PSSetConstantBuffers(0,4,buffers);
            ID3D11ShaderResourceView* views[1]{textures[1].Get()};context->PSSetShaderResources(0,1,views);
            ID3D11SamplerState* samplers[6]{};for (auto& s:samplers) s=sampler.Get();context->PSSetSamplers(0,6,samplers);
            auto* rtv=targetView.Get();context->OMSetRenderTargets(1,&rtv,nullptr);
            const float clear[4]{};context->ClearRenderTargetView(rtv,clear);
            const D3D11_VIEWPORT full{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&full);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->IASetInputLayout(nullptr);
            context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);
            if (patched && (!control.Apply(context.Get(),shader.hash,DispatchNative) || !control.Pending())) return false;
            context->Draw(3,0);
            if (patched && !control.Restore(context.Get())) return false;
            ComPtr<ID3D11Buffer> cb0,cb3;context->PSGetConstantBuffers(0,1,&cb0);context->PSGetConstantBuffers(3,1,&cb3);
            if (cb0.Get()!=constants || cb3!=cameraBuffer || control.Pending()) return false;
            D3D11_TEXTURE2D_DESC staging=targetDesc;staging.BindFlags=0;staging.Usage=D3D11_USAGE_STAGING;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> copy;if (FAILED(device->CreateTexture2D(&staging,nullptr,&copy))) return false;
            context->CopyResource(copy.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(copy.Get(),0,D3D11_MAP_READ,0,&mapped))) return false;
            pixels.resize(width*height*4);
            for (UINT y=0;y<height;++y) memcpy(pixels.data()+y*width*4,static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch,width*16);
            context->Unmap(copy.Get(),0);return true;
        };
        std::vector<float> withoutDust,native,patched;
        if (!render(clamped.Get(),false,withoutDust)) return 92;
        if (!render(dusty.Get(),false,native)) return 93;
        if (!render(dusty.Get(),true,patched)) return 94;
        unsigned lit=0,coloured=0;
        for (size_t i=0;i<native.size();i+=4) {
            lit+=native[i]+native[i+1]+native[i+2]>.01f;
            coloured+=patched[i]!=0.f || patched[i+1]!=0.f || patched[i+2]!=0.f || patched[i+3]!=native[i+3];
        }
        if (lit<width*height/2) return 95;                                   // The native dust must be visible.
        if (coloured) return 96;                                             // Colour zero, alpha unchanged.
        if (memcmp(patched.data(),withoutDust.data(),patched.size()*4)) return 97; // Exactly the clamped draw.
        const auto shortBuffer=Constants(std::vector<float>(shader.constantBytes/4-4));auto* binding=shortBuffer.Get();
        context->PSSetConstantBuffers(0,1,&binding);
        if (control.Apply(context.Get(),shader.hash,DispatchNative) || control.Pending()) return 98;
    }
    return 0;
}
