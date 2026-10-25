#include "vpl_order.h"
#include <d3dcompiler.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
void STDMETHODCALLTYPE Dispatch(ID3D11DeviceContext* c,UINT x,UINT y,UINT z) { c->Dispatch(x,y,z); }
int TestRows(const ComPtr<ID3D11Device>& device,const ComPtr<ID3D11DeviceContext>& context,unsigned height) {
    HRESULT hr{};
    constexpr unsigned width=64;
    const unsigned lastCapacity=(height-32)*16;
    std::vector<uint16_t> input(width*height,12345),expected;
    auto at=[](unsigned tile,unsigned i) { return ((tile/4)*32+(i/16))*width+(tile%4)*16+i%16; };
    std::array<std::vector<uint16_t>,8> lists;
    lists[0]={7,2,7,1,65535,32}; lists[1]={}; lists[2]={4}; lists[3]={9,8,7,6};
    for (unsigned i=0;i<lastCapacity-1;++i) lists[4].push_back(static_cast<uint16_t>(lastCapacity-1-i));
    for (unsigned i=0;i<lastCapacity;++i) lists[5].push_back(static_cast<uint16_t>(lastCapacity-i)); // no terminator: untouched
    lists[6]={5,5,5}; lists[7]={8,1,4,2,6,3,7,5};
    for (unsigned tile=0;tile<8;++tile) {
        for (unsigned i=0;i<lists[tile].size();++i) input[at(tile,i)]=lists[tile][i];
        if (lists[tile].size()<(tile<4 ? 512:lastCapacity)) input[at(tile,static_cast<unsigned>(lists[tile].size()))]=0;
    }
    expected=input;
    for (unsigned tile=0;tile<8;++tile) {
        if (lists[tile].size()==(tile<4 ? 512:lastCapacity)) continue;
        std::sort(lists[tile].begin(),lists[tile].end());
        for (unsigned i=0;i<lists[tile].size();++i) expected[at(tile,i)]=lists[tile][i];
    }
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=width; desc.Height=height; desc.MipLevels=1; desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R16_UINT; desc.SampleDesc.Count=1; desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    D3D11_SUBRESOURCE_DATA data{input.data(),width*2,0};
    ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11UnorderedAccessView> view;
    if (FAILED(device->CreateTexture2D(&desc,&data,&texture)) || FAILED(device->CreateUnorderedAccessView(texture.Get(),nullptr,&view))) return 3;
    const char* source="[numthreads(1,1,1)] void main() {}";
    ComPtr<ID3DBlob> code; ComPtr<ID3D11ComputeShader> native;
    if (FAILED(D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&code,nullptr)) ||
        FAILED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&native))) return 4;
    context->CSSetShader(native.Get(),nullptr,0);
    auto* binding=view.Get(); context->CSSetUnorderedAccessViews(1,1,&binding,nullptr);
    grid2vr::VplOrder sorter; bool restored{};
    if (SUCCEEDED(sorter.Apply(context.Get(),view.Get(),5,2,1,Dispatch,restored))) return 5;
    if (SUCCEEDED(sorter.Apply(context.Get(),view.Get(),4,3,1,Dispatch,restored)) ||
        SUCCEEDED(sorter.Apply(context.Get(),view.Get(),4,1,1,Dispatch,restored))) return 5;
    hr=sorter.Apply(context.Get(),view.Get(),4,2,1,Dispatch,restored);
    if (FAILED(hr) || !restored) { printf("GPU sort failed: %08lx restored=%u\n",hr,restored); return 6; }
    ComPtr<ID3D11ComputeShader> actual; context->CSGetShader(&actual,nullptr,nullptr);
    ComPtr<ID3D11UnorderedAccessView> slot0,slot1;
    context->CSGetUnorderedAccessViews(0,1,&slot0); context->CSGetUnorderedAccessViews(1,1,&slot1);
    if (actual.Get()!=native.Get() || slot0 || slot1.Get()!=view.Get()) return 7;
    auto stagingDesc=desc; stagingDesc.Usage=D3D11_USAGE_STAGING; stagingDesc.BindFlags=0; stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc,nullptr,&staging))) return 8;
    for (unsigned repeat=0;repeat<2;++repeat) {
        if (repeat && FAILED(sorter.Apply(context.Get(),view.Get(),4,2,1,Dispatch,restored))) return 9;
        context->CopyResource(staging.Get(),texture.Get()); D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return 10;
        bool equal=true;
        for (unsigned y=0;y<height;++y) equal=equal && !memcmp(expected.data()+y*width,static_cast<uint8_t*>(mapped.pData)+y*mapped.RowPitch,width*2);
        context->Unmap(staging.Get(),0);
        if (!equal) { puts("Sorted GPU data differs from CPU reference, including untouched tail."); return 11; }
    }
    return 0;
}
int main() {
    // Explicit system path: the proxy in the test directory is not the device factory.
    auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    if (!create) return 1;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    if (FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context))) return 2;
    for (unsigned height:{64u,48u,33u}) {
        const int error=TestRows(device,context,height);
        if (error) { printf("VPL row height=%u failed code=%d\n",height,error); return error; }
    }
    puts("GPU VPL sort: full/partial rows, CPU-reference order, duplicates, max prefix, empty/malformed tiles, padding, idempotence and native bindings verified.");
}
