#include "flicker_pixels.h"
#include <array>
#include <cstdio>
using Microsoft::WRL::ComPtr;
int main() {
    auto library=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(library,"D3D11CreateDevice"));
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> c;
    if(!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&c)))return 1;
    D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=2;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET;
    std::array<uint32_t,4> pixels{};D3D11_SUBRESOURCE_DATA initial{pixels.data(),8,0};
    ComPtr<ID3D11Texture2D> target;if(FAILED(device->CreateTexture2D(&d,&initial,&target)))return 2;
    ComPtr<ID3D11RenderTargetView> view;if(FAILED(device->CreateRenderTargetView(target.Get(),nullptr,&view)))return 3;
    auto* raw=view.Get();c->OMSetRenderTargets(1,&raw,nullptr);
    grid2vr::flicker::PixelWitness witness;
    witness.Before(c.Get());
    pixels={0xff0000ff,0xff0000ff,0xff000000,0}; // Third pixel changes only alpha.
    c->UpdateSubresource(target.Get(),0,nullptr,pixels.data(),8,0);witness.After(c.Get());
    pixels[1]=0;c->UpdateSubresource(target.Get(),0,nullptr,pixels.data(),8,0);
    const auto report=witness.Finish(c.Get(),target.Get());
    for(const char* text:{"\"status\":\"ok\"","\"changedPixels\":2","\"retainedPixels\":1","\"revertedPixels\":1","\"otherPixels\":0",
        "\"bounds\":[0,0,2,1]","\"maskRuns\":[[0,0,2]]","\"maskTruncated\":false","\"finalSourceSame\":true"})
        if(report.find(text)==std::string::npos) {puts(report.c_str());return 4;}
    ComPtr<ID3D11RenderTargetView> actual;c->OMGetRenderTargets(1,&actual,nullptr);if(actual!=view)return 5;
    witness.Before(c.Get());witness.After(c.Get());
    if(witness.Finish(c.Get(),nullptr).find("\"changedPixels\":0")==std::string::npos)return 6;
    if(witness.Finish(c.Get(),nullptr).find("\"status\":\"not_captured\"")==std::string::npos)return 7;
    ComPtr<ID3D11DeviceContext> deferred;device->CreateDeferredContext(0,&deferred);witness.Before(deferred.Get());
    if(witness.Finish(c.Get(),nullptr).find("\"status\":\"context\"")==std::string::npos)return 8;
    puts("Passive RGB masks distinguish later retention/reversion, ignore alpha-only changes and preserve target bindings.");
}
