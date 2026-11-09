#include "floor_reflection.h"
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
// Half-float RGBA so reflected highlights can exceed 1 and reach the glow term.
ComPtr<ID3D11ShaderResourceView> Texture(float scale,unsigned seed) {
    std::vector<float> pixels(width*height*4);
    for (UINT y=0;y<height;++y) for (UINT x=0;x<width;++x) for (UINT c=0;c<4;++c) {
        const unsigned v=(x*2654435761u+y*40503u+c*977u+seed*7919u)>>7;
        pixels[(y*width+x)*4+c]=c==3 ? 1.f:scale*float(v%997)/996.f;
    }
    D3D11_TEXTURE2D_DESC desc{width,height,1,1,DXGI_FORMAT_R32G32B32A32_FLOAT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE};
    const D3D11_SUBRESOURCE_DATA data{pixels.data(),width*16,0};ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> view;
    return SUCCEEDED(device->CreateTexture2D(&desc,&data,&texture)) && SUCCEEDED(device->CreateShaderResourceView(texture.Get(),nullptr,&view)) ? view:nullptr;
}
}
int main(int argc,char** argv) {
    if (argc!=3) return 20;
    const std::array<std::vector<unsigned char>,2> code{Load(argv[1]),Load(argv[2])};
    if (code[0].size()!=11592 || Fnv(code[0])!=FloorReflection::shaders[0].hash) return 21;
    if (code[1].size()!=11828 || Fnv(code[1])!=FloorReflection::shaders[1].hash) return 22;
    if (FloorReflection::Find(0x2d81beb22b54d6ccull) || FloorReflection::Find(0)) return 23;

    const auto dll=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(dll,"D3D11CreateDevice"));
    if (!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))) return 1;

    // Inputs in the shaders' register order: world position, normal, uv/gi uv,
    // view vector, fog colour, tangent and bitangent.
    constexpr char vertexCode[]=
        "struct O{float4 p:SV_Position;float3 w:TEXCOORD0;float4 n:TEXCOORD1;float4 t:TEXCOORD2;float3 v:TEXCOORD4;"
        "float4 f:COLOR1;float3 a:TEXCOORD5;float3 b:TEXCOORD6;};"
        "O main(uint i:SV_VertexID){O o;float2 uv=float2((i<<1)&2,i&2);o.p=float4(uv*float2(2,-2)+float2(-1,1),0,1);"
        "o.w=float3(uv.x*20,0,uv.y*20);o.n=float4(.1*uv.x,1,.1*uv.y,0);o.t=float4(uv*3,uv*.5);"
        "o.v=float3(.4-.6*uv.x,-.7,.6-.5*uv.y);o.f=float4(0,0,0,1);o.a=float3(1,0,0);o.b=float3(0,0,1);return o;}";
    ComPtr<ID3DBlob> vertexBytes;ComPtr<ID3D11VertexShader> vertex;
    if (FAILED(D3DCompile(vertexCode,sizeof(vertexCode)-1,nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vertexBytes,nullptr)) ||
        FAILED(device->CreateVertexShader(vertexBytes->GetBufferPointer(),vertexBytes->GetBufferSize(),nullptr,&vertex))) return 2;
    D3D11_SAMPLER_DESC samplerDesc{D3D11_FILTER_MIN_MAG_MIP_POINT,D3D11_TEXTURE_ADDRESS_WRAP,D3D11_TEXTURE_ADDRESS_WRAP,D3D11_TEXTURE_ADDRESS_WRAP};
    samplerDesc.MaxLOD=D3D11_FLOAT32_MAX;ComPtr<ID3D11SamplerState> sampler;
    if (FAILED(device->CreateSamplerState(&samplerDesc,&sampler))) return 3;
    D3D11_TEXTURE2D_DESC targetDesc{width,height,1,1,DXGI_FORMAT_R32G32B32A32_FLOAT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> target;ComPtr<ID3D11RenderTargetView> targetView;
    if (FAILED(device->CreateTexture2D(&targetDesc,nullptr,&target)) || FAILED(device->CreateRenderTargetView(target.Get(),nullptr,&targetView))) return 4;

    // Shared native inputs: identity colour encoding, fog off, alpha test off,
    // spot light out of range. Every texture slot has its own pattern.
    std::vector<float> perFrame(24*4),targetConstants{float(width),float(height),1.f/width,1.f/height},alpha{-1,0,0,0},camera(20*4);
    for (size_t i=0;i<perFrame.size();++i) perFrame[i]=.2f+.003f*float(i%41);
    perFrame[0]=0;perFrame[1]=1;perFrame[2]=1;perFrame[3]=0;perFrame[10*4+3]=1;
    camera[19*4]=1; // leftEye selects TReflectionMap.
    std::array<ComPtr<ID3D11ShaderResourceView>,8> textures;
    for (unsigned slot=0;slot<textures.size();++slot) if (!(textures[slot]=Texture(1,slot))) return 5;
    const auto bright=Texture(3,11),black=Texture(0,0);
    if (!bright || !black) return 6;
    const auto frame=Constants(perFrame),viewport=Constants(targetConstants),alphaRef=Constants(alpha),cameraParams=Constants(camera);
    if (!frame || !viewport || !alphaRef || !cameraParams) return 7;

    FloorReflection control;
    for (unsigned index=0;index<2;++index) {
        const auto& shader=FloorReflection::shaders[index];
        ComPtr<ID3D11PixelShader> pixel;
        if (FAILED(device->CreatePixelShader(code[index].data(),code[index].size(),nullptr,&pixel))) return 30+index;
        // A larger native $Globals allocation than declared is accepted as is.
        std::vector<float> globals(shader.constantBytes/4+(index?4:0));
        for (size_t i=0;i<globals.size();++i) globals[i]=.15f+.002f*float(i%53);
        globals[7*4+3]=0;  // spot radius: no spot contribution
        globals[shader.constantBytes/16*4-4+0]=1;  // objectLightTweaks.x
        const unsigned fresnel=FloorReflection::fresnelOffset/4;
        globals[fresnel]=.35f;globals[fresnel+1]=.95f;globals[fresnel+2]=2;globals[fresnel+3]=.05f;
        auto plain=globals;plain[fresnel]=0;plain[fresnel+1]=0;
        const auto reflective=Constants(globals),neutral=Constants(plain);
        if (!reflective || !neutral) return 40+index;

        auto render=[&](ID3D11Buffer* constants,ID3D11ShaderResourceView* reflection,bool patched,std::vector<float>& pixels) {
            ID3D11Buffer* buffers[6]{constants,frame.Get(),viewport.Get(),cameraParams.Get(),nullptr,alphaRef.Get()};
            context->PSSetConstantBuffers(0,6,buffers);
            ID3D11ShaderResourceView* views[8]{};
            for (unsigned slot=0;slot<8;++slot) views[slot]=textures[slot].Get();
            views[shader.reflectionSlot]=views[shader.reflectionSlot+1]=reflection;
            context->PSSetShaderResources(0,8,views);
            ID3D11SamplerState* samplers[11]{};for (auto& s:samplers) s=sampler.Get();context->PSSetSamplers(0,11,samplers);
            auto* rtv=targetView.Get();context->OMSetRenderTargets(1,&rtv,nullptr);
            const float clear[4]{};context->ClearRenderTargetView(rtv,clear);
            const D3D11_VIEWPORT full{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&full);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->IASetInputLayout(nullptr);
            context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);
            if (patched && !control.Apply(context.Get(),shader.hash,DispatchNative)) return false;
            context->Draw(3,0);
            if (patched && !control.Restore(context.Get())) return false;
            // Restoration returns the game's exact bindings.
            ComPtr<ID3D11Buffer> cb0;context->PSGetConstantBuffers(0,1,&cb0);
            ID3D11ShaderResourceView* bound[2]{};context->PSGetShaderResources(shader.reflectionSlot,2,bound);
            const bool same=cb0.Get()==constants && bound[0]==reflection && bound[1]==reflection;
            for (auto* view:bound) if (view) view->Release();
            if (!same || control.Pending()) return false;
            D3D11_TEXTURE2D_DESC staging=targetDesc;staging.BindFlags=0;staging.Usage=D3D11_USAGE_STAGING;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> copy;if (FAILED(device->CreateTexture2D(&staging,nullptr,&copy))) return false;
            context->CopyResource(copy.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(copy.Get(),0,D3D11_MAP_READ,0,&mapped))) return false;
            pixels.resize(width*height*4);
            for (UINT y=0;y<height;++y) memcpy(pixels.data()+y*width*4,static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch,width*16);
            context->Unmap(copy.Get(),0);return true;
        };
        std::vector<float> withoutReflection,native,patched;
        if (!render(neutral.Get(),black.Get(),false,withoutReflection)) return 50+index;
        if (!render(reflective.Get(),bright.Get(),false,native)) return 52+index;
        if (!render(reflective.Get(),bright.Get(),true,patched)) return 54+index;
        unsigned lit=0,reflected=0;
        for (size_t i=0;i<native.size();i+=4) {
            lit+=withoutReflection[i]+withoutReflection[i+1]+withoutReflection[i+2]>.01f;
            reflected+=std::fabs(native[i]-withoutReflection[i])+std::fabs(native[i+1]-withoutReflection[i+1])+std::fabs(native[i+2]-withoutReflection[i+2])>.01f;
        }
        if (lit<width*height/2) return 60+index;          // The surface itself must render.
        if (reflected<width*height/2) return 62+index;    // The native reflection must be visible.
        if (memcmp(patched.data(),withoutReflection.data(),patched.size()*4)) return 64+index; // Exactly the reflection-free surface.

        // Refusals change no binding.
        ComPtr<ID3D11Buffer> before;context->PSGetConstantBuffers(0,1,&before);
        if (control.Apply(context.Get(),shader.hash,nullptr) || control.Apply(context.Get(),0x2d81beb22b54d6ccull,DispatchNative) ||
            control.Pending() || control.Restore(context.Get())) return 70+index;
        if (!control.Apply(context.Get(),shader.hash,DispatchNative) || control.Apply(context.Get(),shader.hash,DispatchNative) ||
            !control.Restore(context.Get()) || control.Restore(context.Get())) return 72+index;
        ComPtr<ID3D11Buffer> after;context->PSGetConstantBuffers(0,1,&after);
        if (after!=before) return 74+index;
        // The native buffer itself is never written.
        std::vector<float> truncated(shader.constantBytes/4-4);
        const auto shortBuffer=Constants(truncated);auto* binding=shortBuffer.Get();context->PSSetConstantBuffers(0,1,&binding);
        if (control.Apply(context.Get(),shader.hash,DispatchNative) || control.Pending()) return 76+index;
        after.Reset();context->PSGetConstantBuffers(0,1,&after);if (after!=shortBuffer) return 78+index;
    }
    // Compute bindings and viewport outside the owned state are untouched.
    const auto marker=Constants(std::vector<float>(64,1.f));auto* cs=marker.Get();context->CSSetConstantBuffers(7,1,&cs);
    const D3D11_VIEWPORT odd{3,4,17,19,.2f,.8f};context->RSSetViewports(1,&odd);
    std::vector<float> globals(FloorReflection::shaders[0].constantBytes/4,.5f);
    const auto native=Constants(globals);auto* cb0=native.Get();context->PSSetConstantBuffers(0,1,&cb0);
    if (!control.Apply(context.Get(),FloorReflection::shaders[0].hash,DispatchNative)) return 80;
    ComPtr<ID3D11Buffer> computeAfter;context->CSGetConstantBuffers(7,1,&computeAfter);
    D3D11_VIEWPORT actual{};UINT count=1;context->RSGetViewports(&count,&actual);
    if (computeAfter!=marker || count!=1 || memcmp(&actual,&odd,sizeof(odd)) || !control.Restore(context.Get())) return 81;
    return 0;
}
