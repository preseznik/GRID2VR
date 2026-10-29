#include "impact_interference.h"
#include <d3dcompiler.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
using namespace grid2vr;
using Microsoft::WRL::ComPtr;
void STDMETHODCALLTYPE DispatchNative(ID3D11DeviceContext* c,UINT x,UINT y,UINT z) {c->Dispatch(x,y,z);}
int main(int argc,char** argv) {
    if (argc!=2) return 20;
    std::ifstream file(std::filesystem::path(argv[1]),std::ios::binary);
    const std::vector<unsigned char> shader{std::istreambuf_iterator<char>(file),{}};
    uint64_t hash=14695981039346656037ull;for (auto byte:shader) hash=(hash^byte)*1099511628211ull;
    if (shader.size()!=7872 || hash!=ImpactInterference::shaderHash) return 21;

    const auto dll=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(dll,"D3D11CreateDevice"));
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    if (!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))) return 1;
    std::array<uint32_t,324> original{};for (unsigned i=0;i<original.size();++i) original[i]=0x3f000000+i*17;
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=1296;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA data{original.data(),0,0};ComPtr<ID3D11Buffer> source;
    if (FAILED(device->CreateBuffer(&desc,&data,&source))) return 2;
    auto* binding=source.Get();context->PSSetConstantBuffers(0,1,&binding);context->CSSetConstantBuffers(7,1,&binding);
    const D3D11_VIEWPORT viewport{3,4,17,19,.2f,.8f};context->RSSetViewports(1,&viewport);
    ImpactInterference control;
    if (control.Apply(context.Get(),nullptr) || !control.Apply(context.Get(),DispatchNative) || !control.Pending() ||
        control.Apply(context.Get(),DispatchNative) || control.PassComplete(1,1) || control.PassComplete(0,0)) return 3;
    ComPtr<ID3D11Buffer> filtered,compute;context->PSGetConstantBuffers(0,1,&filtered);context->CSGetConstantBuffers(7,1,&compute);
    D3D11_VIEWPORT actual{};UINT count=1;context->RSGetViewports(&count,&actual);
    if (filtered==source || compute!=source || count!=1 || memcmp(&actual,&viewport,sizeof(viewport))) return 4;
    auto read=[&](ID3D11Buffer* buffer,std::array<uint32_t,324>& bytes) {
        D3D11_BUFFER_DESC copy=desc;copy.BindFlags=0;copy.Usage=D3D11_USAGE_STAGING;copy.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;if (FAILED(device->CreateBuffer(&copy,nullptr,&staging))) return false;
        context->CopyResource(staging.Get(),buffer);D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return false;
        memcpy(bytes.data(),mapped.pData,sizeof(bytes));context->Unmap(staging.Get(),0);return true;
    };
    // Only the gate word changes; the native buffer is untouched.
    auto expected=original;expected[ImpactInterference::gateOffset/4]=ImpactInterference::gateValue;std::array<uint32_t,324> bytes{};
    if (!read(filtered.Get(),bytes) || bytes!=expected || !read(source.Get(),bytes) || bytes!=original) return 5;
    if (!control.Restore(context.Get()) || control.Pending() || control.Restore(context.Get())) return 6;
    filtered.Reset();context->PSGetConstantBuffers(0,1,&filtered);if (filtered!=source) return 7;
    desc.ByteWidth=1280;ComPtr<ID3D11Buffer> wrong;device->CreateBuffer(&desc,nullptr,&wrong);desc.ByteWidth=1296;
    binding=wrong.Get();context->PSSetConstantBuffers(0,1,&binding);
    if (control.Apply(context.Get(),DispatchNative) || control.Pending()) return 8;
    if (!control.PassComplete(0,0) || !control.PassComplete(1,1) || control.PassComplete(0,1) ||
        control.PassComplete(1,0) || control.PassComplete(2,2) || control.PassComplete(1,2)) return 9;

    // Render the shipped shader: the gate must control the glitch, and the
    // patched constants must reproduce the gate-closed pass-through exactly.
    ComPtr<ID3D11PixelShader> interference;
    if (FAILED(device->CreatePixelShader(shader.data(),shader.size(),nullptr,&interference))) return 10;
    constexpr char vertexCode[]="struct O{float4 p:SV_Position;float4 t:TEXCOORD0;};"
        "O main(uint i:SV_VertexID){O o;float2 uv=float2((i<<1)&2,i&2);o.p=float4(uv*float2(2,-2)+float2(-1,1),0,1);o.t=float4(uv,uv);return o;}";
    ComPtr<ID3DBlob> vertexBytes;ComPtr<ID3D11VertexShader> vertex;
    if (FAILED(D3DCompile(vertexCode,sizeof(vertexCode)-1,nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vertexBytes,nullptr)) ||
        FAILED(device->CreateVertexShader(vertexBytes->GetBufferPointer(),vertexBytes->GetBufferSize(),nullptr,&vertex))) return 11;
    constexpr UINT width=128,height=72;
    auto texture=[&](auto fill,ComPtr<ID3D11ShaderResourceView>& view) {
        std::vector<uint32_t> pixels(width*height);
        for (UINT y=0;y<height;++y) for (UINT x=0;x<width;++x) pixels[y*width+x]=fill(x,y);
        D3D11_TEXTURE2D_DESC t{width,height,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE};
        const D3D11_SUBRESOURCE_DATA initial{pixels.data(),width*4,0};ComPtr<ID3D11Texture2D> resource;
        return SUCCEEDED(device->CreateTexture2D(&t,&initial,&resource)) && SUCCEEDED(device->CreateShaderResourceView(resource.Get(),nullptr,&view));
    };
    ComPtr<ID3D11ShaderResourceView> map,scene,offsets;
    // Map alpha 0 selects every pixel for interference whenever the gate is above zero.
    if (!texture([](UINT x,UINT y){return 0x00406080u^(x*2654435761u+y*40503u)&0x00ffffffu;},map) ||
        !texture([](UINT x,UINT y){return 0xff000000u|(x*2)<<16|(y*3)<<8|((x+y)&0xff);},scene) ||
        !texture([](UINT x,UINT y){return 0xff000000u|((x*37+y*11)&0xff)<<8|((x*13+y*29)&0xff);},offsets)) return 12;
    D3D11_SAMPLER_DESC samplerDesc{D3D11_FILTER_MIN_MAG_MIP_POINT,D3D11_TEXTURE_ADDRESS_WRAP,D3D11_TEXTURE_ADDRESS_WRAP,D3D11_TEXTURE_ADDRESS_WRAP};
    samplerDesc.MaxLOD=D3D11_FLOAT32_MAX;ComPtr<ID3D11SamplerState> sampler;
    if (FAILED(device->CreateSamplerState(&samplerDesc,&sampler))) return 13;
    D3D11_TEXTURE2D_DESC targetDesc{width,height,1,1,DXGI_FORMAT_R10G10B10A2_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> target;ComPtr<ID3D11RenderTargetView> targetView;
    if (FAILED(device->CreateTexture2D(&targetDesc,nullptr,&target)) || FAILED(device->CreateRenderTargetView(target.Get(),nullptr,&targetView))) return 14;
    std::array<float,324> constants{};
    for (unsigned i=0;i<constants.size();++i) constants[i]=.25f+.001f*i;
    constants[76*4]=64;constants[77*4]=36;constants[78*4]=1.f/64;constants[79*4]=1.f/36;
    constants[76*4+3]=0;constants[77*4+3]=1;
    auto render=[&](float gate,bool patched,std::vector<uint32_t>& pixels) {
        auto values=constants;values[ImpactInterference::gateOffset/4]=gate;
        const D3D11_SUBRESOURCE_DATA initial{values.data(),0,0};ComPtr<ID3D11Buffer> buffer;
        if (FAILED(device->CreateBuffer(&desc,&initial,&buffer))) return false;
        auto* cb=buffer.Get();context->PSSetConstantBuffers(0,1,&cb);
        ID3D11ShaderResourceView* views[]{map.Get(),scene.Get(),offsets.Get()};context->PSSetShaderResources(0,3,views);
        ID3D11SamplerState* samplers[]{sampler.Get(),sampler.Get(),sampler.Get()};context->PSSetSamplers(3,3,samplers);
        auto* rtv=targetView.Get();context->OMSetRenderTargets(1,&rtv,nullptr);
        const D3D11_VIEWPORT full{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&full);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->IASetInputLayout(nullptr);
        context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(interference.Get(),nullptr,0);
        if (patched && !control.Apply(context.Get(),DispatchNative)) return false;
        context->Draw(3,0);
        if (patched && !control.Restore(context.Get())) return false;
        D3D11_TEXTURE2D_DESC staging=targetDesc;staging.BindFlags=0;staging.Usage=D3D11_USAGE_STAGING;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> copy;if (FAILED(device->CreateTexture2D(&staging,nullptr,&copy))) return false;
        context->CopyResource(copy.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(copy.Get(),0,D3D11_MAP_READ,0,&mapped))) return false;
        pixels.resize(width*height);
        for (UINT y=0;y<height;++y) memcpy(pixels.data()+y*width,static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch,width*4);
        context->Unmap(copy.Get(),0);return true;
    };
    std::vector<uint32_t> closed,open,patched;
    if (!render(0,false,closed) || !render(1,false,open) || !render(1,true,patched)) return 15;
    unsigned changed=0;for (size_t i=0;i<closed.size();++i) changed+=closed[i]!=open[i];
    if (changed<closed.size()/4) return 16; // The open gate must visibly glitch.
    if (patched!=closed) return 17;         // Patched constants take the native pass-through.
    ComPtr<ID3D11Buffer> after;context->PSGetConstantBuffers(0,1,&after);
    if (after==nullptr || control.Pending()) return 18;
    return 0;
}
