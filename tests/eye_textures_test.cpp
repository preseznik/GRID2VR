#include "eye_textures.h"
#include "texture_digest.h"
#include <cstdio>
#include <string>
using Microsoft::WRL::ComPtr;
std::string Hash(ID3D11DeviceContext* context,ID3D11Texture2D* texture) {
    uint64_t budget=4096;
    const auto digest=grid2vr::DigestTexture(context,texture,budget);
    return digest.subresources.size()==1 ? digest.subresources[0].sha256:std::string{};
}
int main() {
    auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    if (!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))) return 1;
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=8; desc.Height=4; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R10G10B10A2_UNORM; desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> source;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&source))) return 2;
    grid2vr::EyeTextures pair;
    std::array<uint32_t,32> pixels{};
    ID3D11Texture2D* retainedLeft{};
    for (uint64_t frame=1;frame<=120;++frame) {
        if (!pair.Begin(frame) || pair.Ready(frame) || pair.Get(frame,0)) return 3;
        pixels.fill(0xfff003ff); context->UpdateSubresource(source.Get(),0,nullptr,pixels.data(),32,0);
        const auto left=Hash(context.Get(),source.Get());
        if (left.empty() || !pair.Capture(context.Get(),frame,0,source.Get()) || pair.Get(frame,0)) return 4;
        pixels.fill(0xc00ffc00); context->UpdateSubresource(source.Get(),0,nullptr,pixels.data(),32,0);
        const auto right=Hash(context.Get(),source.Get());
        if (right.empty() || left==right || !pair.Capture(context.Get(),frame,1,source.Get()) || !pair.Ready(frame)) return 5;
        pixels.fill(0); context->UpdateSubresource(source.Get(),0,nullptr,pixels.data(),32,0);
        if (Hash(context.Get(),pair.Get(frame,0))!=left || Hash(context.Get(),pair.Get(frame,1))!=right || pair.Get(frame-1,0)) return 6;
        if (retainedLeft && retainedLeft!=pair.Get(frame,0)) return 7;
        retainedLeft=pair.Get(frame,0);
    }
    if (pair.Begin(120) || pair.Ready(120) || pair.Capture(context.Get(),120,0,source.Get())) return 8;
    pair.Begin(121);
    if (pair.Capture(context.Get(),121,1,source.Get()) || pair.Ready(121)) return 9;
    if (pair.Capture(context.Get(),121,0,source.Get())) return 15;
    pair.Begin(122);
    if (pair.Capture(context.Get(),121,0,source.Get())) return 10;
    pair.Begin(123); pair.Capture(context.Get(),123,0,source.Get());
    desc.Width=16; ComPtr<ID3D11Texture2D> resized;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&resized)) || pair.Capture(context.Get(),123,1,resized.Get()) || pair.Ready(123)) return 11;
    pair.Begin(124);
    if (!pair.Capture(context.Get(),124,0,resized.Get()) || !pair.Capture(context.Get(),124,1,resized.Get())) return 12;
    pair.Begin(125); desc.SampleDesc.Count=4; ComPtr<ID3D11Texture2D> msaa;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&msaa)) || !pair.Capture(context.Get(),125,0,msaa.Get()) ||
        pair.Capture(context.Get(),125,1,resized.Get()) || pair.Ready(125)) return 13;
    ComPtr<ID3D11DeviceContext> deferred; device->CreateDeferredContext(0,&deferred);
    pair.Begin(126);
    if (pair.Capture(deferred.Get(),126,0,source.Get()) || pair.Get(126,0)) return 14;
    for (uint64_t frame=127;frame<131;++frame) {
        if (!pair.Begin(frame,3)) return 16;
        std::array<std::string,3> hashes;
        for (unsigned pass=0;pass<3;++pass) {
            pixels.fill(pass==0?0xfff003ff:pass==1?0xc00ffc00:0xfff00000);
            context->UpdateSubresource(source.Get(),0,nullptr,pixels.data(),32,0);
            hashes[pass]=Hash(context.Get(),source.Get());
            if (!pair.Capture(context.Get(),frame,pass,source.Get()) || !pair.Captured(frame,pass+1)) return 17;
            if (pass<2 && (pair.Ready(frame) || pair.Get(frame,1))) return 18;
        }
        pixels.fill(0);context->UpdateSubresource(source.Get(),0,nullptr,pixels.data(),32,0);
        for (unsigned pass=0;pass<3;++pass)
            if (hashes[pass].empty() || Hash(context.Get(),pair.Get(frame,pass))!=hashes[pass]) return 19;
        if (pair.Get(frame,3) || !pair.Ready(frame)) return 20;
    }
    pair.Begin(131,3);pair.Capture(context.Get(),131,0,source.Get());
    if (pair.Capture(context.Get(),131,2,source.Get()) || pair.Ready(131) || pair.Get(131,0)) return 21;
    if (pair.Begin(132,4)) return 22;
    if (!pair.Begin(133,2) || !pair.Capture(context.Get(),133,0,source.Get()) ||
        !pair.Capture(context.Get(),133,1,source.Get()) || pair.Get(133,2)) return 23;
    pair.Reset();
    if (pair.Ready(133) || pair.Get(133,0) || pair.Begin(133)) return 24;
    if (!pair.Begin(150,2) || !pair.Capture(context.Get(),150,0,source.Get()) ||
        !pair.Capture(context.Get(),150,1,source.Get()) || !pair.Ready(150)) return 25;
    puts("GPU pairs and center/left/right groups retain exact independent contents and reject partial, reordered or stale frames.");
}
