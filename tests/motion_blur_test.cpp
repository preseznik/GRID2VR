#include "motion_blur.h"
#include <array>
#include <cstring>
using namespace grid2vr;
using Microsoft::WRL::ComPtr;
void STDMETHODCALLTYPE DispatchNative(ID3D11DeviceContext* c,UINT x,UINT y,UINT z) {c->Dispatch(x,y,z);}
int main() {
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
    MotionBlurControl control;
    if (control.Apply(context.Get(),123,DispatchNative) || !control.Apply(context.Get(),0x6e44f31d1ebda08dull,DispatchNative) || !control.Pending() || control.PassComplete(1,1) || control.PassComplete(0,0)) return 3;
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
    auto expected=original;expected[0]=expected[1]=0;std::array<uint32_t,324> bytes{};
    if (!read(filtered.Get(),bytes) || bytes!=expected || !read(source.Get(),bytes) || bytes!=original) return 5;
    if (!control.Restore(context.Get()) || control.Pending()) return 6;
    filtered.Reset();context->PSGetConstantBuffers(0,1,&filtered);if (filtered!=source) return 7;
    for (auto hash:{0xed4ddaac90aff677ull,0xb02f3b98b3596660ull,0xcfc3cf441f08e879ull})
        if (!control.Apply(context.Get(),hash,DispatchNative) || !control.Restore(context.Get())) return 8;
    desc.ByteWidth=1280;ComPtr<ID3D11Buffer> wrong;device->CreateBuffer(&desc,nullptr,&wrong);
    binding=wrong.Get();context->PSSetConstantBuffers(0,1,&binding);
    if (control.Apply(context.Get(),0x6e44f31d1ebda08dull,DispatchNative) || control.Pending()) return 9;
    if (!control.PassComplete(0,0) || !control.PassComplete(1,1) ||
        control.PassComplete(0,1) || control.PassComplete(1,0) ||
        control.PassComplete(2,2) || control.PassComplete(1,2)) return 10;
    return 0;
}
