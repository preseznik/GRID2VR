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
int TestHeadsetGrid(ID3D11Device* device,ID3D11DeviceContext* context,unsigned sceneWidth,unsigned sceneHeight) {
    // Native VPL storage pads width to 16 and uses twice the scene height.
    // The 150% Quest 3 scene needs 306 tile rows, beyond the former 256 cap.
    const unsigned width=(sceneWidth+15)/16*16,height=sceneHeight*2;
    const unsigned groupsX=width/16,groupsY=(height+31)/32;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;
    desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R16_UINT;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11UnorderedAccessView> view;
    if(FAILED(device->CreateTexture2D(&desc,nullptr,&texture)) || FAILED(device->CreateUnorderedAccessView(texture.Get(),nullptr,&view))) return 12;
    // Unterminated tiles stay untouched. Exercise actual sorting at all four
    // corners, including the final partial row, without a full CPU readback.
    const UINT fill[4]{65535,65535,65535,65535};context->ClearUnorderedAccessViewUint(view.Get(),fill);
    std::array<uint16_t,512> input;input.fill(65535);
    const uint16_t prefix[]{7,2,7,1,65535,32,0};std::copy(std::begin(prefix),std::end(prefix),input.begin());
    auto expected=input;std::sort(expected.begin(),expected.begin()+6);
    for(unsigned row:{0u,groupsY-1}) for(unsigned column:{0u,groupsX-1}) {
        const D3D11_BOX box{column*16,row*32,0,(column+1)*16,std::min((row+1)*32,height),1};
        context->UpdateSubresource(texture.Get(),0,&box,input.data(),32,0);
    }
    auto* binding=view.Get();context->CSSetUnorderedAccessViews(1,1,&binding,nullptr);
    grid2vr::VplOrder sorter;bool restored{};
    for(const auto groups: {std::array<UINT,3>{groupsX,0,1},{groupsX,513,1},{513,groupsY,1},{groupsX,groupsY,2}})
        if(sorter.Apply(context,view.Get(),groups[0],groups[1],groups[2],Dispatch,restored)!=E_INVALIDARG || restored) return 13;
    const auto hr=sorter.Apply(context,view.Get(),groupsX,groupsY,1,Dispatch,restored);
    if(FAILED(hr) || !restored) {
        printf("Headset VPL scene=%ux%u texture=%ux%u groups=%ux%u hr=%08lx restored=%u\n",sceneWidth,sceneHeight,width,height,groupsX,groupsY,hr,restored);
        return 14;
    }
    ComPtr<ID3D11UnorderedAccessView> slot0,slot1;
    context->CSGetUnorderedAccessViews(0,1,&slot0);context->CSGetUnorderedAccessViews(1,1,&slot1);
    if(slot0 || slot1.Get()!=view.Get()) return 15;
    binding=nullptr;context->CSSetUnorderedAccessViews(1,1,&binding,nullptr);
    desc.Width=16;desc.Height=32;desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;if(FAILED(device->CreateTexture2D(&desc,nullptr,&staging))) return 16;
    for(unsigned row:{0u,groupsY-1}) for(unsigned column:{0u,groupsX-1}) {
        const D3D11_BOX box{column*16,row*32,0,(column+1)*16,std::min((row+1)*32,height),1};
        context->CopySubresourceRegion(staging.Get(),0,0,0,0,texture.Get(),0,&box);
        D3D11_MAPPED_SUBRESOURCE mapped{};if(FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return 17;
        bool equal=true;
        for(unsigned y=0;y<box.bottom-box.top;++y)
            equal&=!memcmp(expected.data()+y*16,static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch,32);
        context->Unmap(staging.Get(),0);if(!equal) return 18;
    }
    printf("Headset VPL scene=%ux%u texture=%ux%u groups=%ux%u: sorted corner lists, terminators/tails and UAV restoration verified.\n",sceneWidth,sceneHeight,width,height,groupsX,groupsY);
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
    for(const auto scene: {std::array<unsigned,2>{4608,4896},{6144,6528},{4097,4097}}) {
        const int error=TestHeadsetGrid(device.Get(),context.Get(),scene[0],scene[1]);
        if(error) return error;
    }
    puts("GPU VPL sort: full/partial rows, CPU-reference order, duplicates, max prefix, empty/malformed tiles, padding, idempotence and native bindings verified.");
}
