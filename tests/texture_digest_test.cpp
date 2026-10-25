#include "texture_digest.h"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <sstream>

using Microsoft::WRL::ComPtr;
bool AlphaEquals(const grid2vr::AlphaCoverageDigest& alpha,uint64_t texels,uint64_t above) {
    return !strcmp(alpha.status,"ok") && alpha.texels==texels && alpha.atLeastHalf==above && alpha.belowHalf==texels-above;
}
void Bc3Block(unsigned char* block,unsigned char a,unsigned char b,uint64_t selectors) {
    memset(block,0xff,16);block[0]=a;block[1]=b;
    for(UINT i=0;i<6;++i)block[2+i]=static_cast<unsigned char>(selectors>>(8*i));
}
bool CheckBc3Decoder() {
    // Every selector appears twice, including ones spanning bytes and bit 45.
    uint64_t selectors{};
    for(UINT i=0;i<16;++i)selectors|=static_cast<uint64_t>(i%8)<<(3*i);
    unsigned char blocks[40]{};
    D3D11_MAPPED_SUBRESOURCE mapped{blocks,40,40};
    const std::array<bool,8> eight{true,false,true,true,true,false,false,false};
    const std::array<bool,8> six{false,true,false,false,true,true,false,true};
    for(const auto format:{DXGI_FORMAT_BC3_TYPELESS,DXGI_FORMAT_BC3_UNORM,DXGI_FORMAT_BC3_UNORM_SRGB}) {
        for(const auto endpoints:{std::array<unsigned char,2>{255,0},{128,127},{0,255},{127,128}}) {
            const auto& expected=endpoints[0]>endpoints[1]?eight:six;
            Bc3Block(blocks,endpoints[0],endpoints[1],selectors);
            if(!AlphaEquals(grid2vr::CountTextureAlpha(format,mapped,4,4),16,8))return false;
            for(UINT code=0;code<8;++code) {
                uint64_t repeated{};
                for(UINT i=0;i<16;++i)repeated|=static_cast<uint64_t>(code)<<(3*i);
                Bc3Block(blocks,endpoints[0],endpoints[1],repeated);
                if(!AlphaEquals(grid2vr::CountTextureAlpha(format,mapped,4,4),16,expected[code]?16:0))return false;
            }
        }
        Bc3Block(blocks,128,128,selectors);
        if(!AlphaEquals(grid2vr::CountTextureAlpha(format,mapped,4,4),16,14))return false;
        Bc3Block(blocks,127,127,selectors);
        if(!AlphaEquals(grid2vr::CountTextureAlpha(format,mapped,4,4),16,2))return false;
        // One opaque clipped column in the second block; poisoned row padding
        // and the colour endpoints/selectors never contribute to alpha.
        Bc3Block(blocks,0,255,0);Bc3Block(blocks+16,255,0,0);memset(blocks+32,0xff,8);
        if(!AlphaEquals(grid2vr::CountTextureAlpha(format,mapped,5,3),15,3))return false;
        Bc3Block(blocks,128,127,selectors);
        if(!AlphaEquals(grid2vr::CountTextureAlpha(format,mapped,2,2),4,2) ||
            !AlphaEquals(grid2vr::CountTextureAlpha(format,mapped,1,1),1,1))return false;
        mapped.RowPitch=15;
        if(strcmp(grid2vr::CountTextureAlpha(format,mapped,4,4).status,"invalid_layout"))return false;
        mapped.RowPitch=31;
        if(strcmp(grid2vr::CountTextureAlpha(format,mapped,5,3).status,"invalid_layout"))return false;
        mapped.RowPitch=40;
    }
    return true;
}
bool CheckAlphaDecoder() {
    // Two BC1 blocks with eight bytes of poisoned row padding. Only five of
    // their eight texel columns and three of four rows belong to the mip.
    unsigned char bc1[24]{};
    bc1[2]=1; memset(bc1+4,0xff,4); // c0 < c1: selector 3 is transparent.
    bc1[8]=1; memset(bc1+12,0xff,4); // c0 > c1: selector 3 stays opaque.
    memset(bc1+16,0xff,8);
    D3D11_MAPPED_SUBRESOURCE mapped{bc1,24,24};
    for (const auto format:{DXGI_FORMAT_BC1_TYPELESS,DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_BC1_UNORM_SRGB}) {
        if (!AlphaEquals(grid2vr::CountTextureAlpha(format,mapped,5,3),15,3)) return false;
        if (!AlphaEquals(grid2vr::CountTextureAlpha(format,mapped,1,1),1,0)) return false;
    }
    bc1[2]=0; // Equality also selects three-colour/transparent mode.
    if (!AlphaEquals(grid2vr::CountTextureAlpha(DXGI_FORMAT_BC1_UNORM,mapped,4,4),16,0)) return false;
    bc1[0]=1; bc1[1]=0; bc1[2]=0; bc1[3]=1; // Compare the full little-endian endpoints.
    memset(bc1+4,0xe4,4); // Selectors 0,1,2,3 on each row.
    if (!AlphaEquals(grid2vr::CountTextureAlpha(DXGI_FORMAT_BC1_UNORM,mapped,4,4),16,12) ||
        !AlphaEquals(grid2vr::CountTextureAlpha(DXGI_FORMAT_BC1_UNORM,mapped,3,2),6,6)) return false;
    mapped.RowPitch=7;
    if (strcmp(grid2vr::CountTextureAlpha(DXGI_FORMAT_BC1_UNORM,mapped,4,4).status,"invalid_layout")) return false;
    mapped.RowPitch=24;
    if (strcmp(grid2vr::CountTextureAlpha(DXGI_FORMAT_BC1_UNORM,mapped,0,1).status,"invalid_layout")) return false;
    mapped.pData=nullptr;
    if (strcmp(grid2vr::CountTextureAlpha(DXGI_FORMAT_BC1_UNORM,mapped,1,1).status,"invalid_layout")) return false;
    unsigned char rgba[20]={0,0,0,0, 0,0,0,127, 0,0,0,128, 0,0,0,255, 255,255,255,255};
    mapped={rgba,20,20};
    for (const auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB})
        if (!AlphaEquals(grid2vr::CountTextureAlpha(format,mapped,4,1),4,2)) return false;
    mapped.RowPitch=15;
    if (strcmp(grid2vr::CountTextureAlpha(DXGI_FORMAT_R8G8B8A8_UNORM,mapped,4,1).status,"invalid_layout")) return false;
    for (const auto format:{DXGI_FORMAT_BC2_UNORM,DXGI_FORMAT_BC7_UNORM,DXGI_FORMAT_R8G8B8A8_TYPELESS,DXGI_FORMAT_A8_UNORM})
        if (strcmp(grid2vr::CountTextureAlpha(format,mapped,1,1).status,"unsupported_format")) return false;
    return true;
}
bool CheckAlphaTexture(ID3D11Device* device,ID3D11DeviceContext* context,DXGI_FORMAT format,bool bc1) {
    const bool bc3=format==DXGI_FORMAT_BC3_TYPELESS || format==DXGI_FORMAT_BC3_UNORM || format==DXGI_FORMAT_BC3_UNORM_SRGB;
    const bool compressed=bc1 || bc3;
    const UINT width=compressed?12:7,height=compressed?8:5,mips=compressed?4:3,layers=2,elementBytes=bc3?16:bc1?8:4;
    std::vector<std::vector<unsigned char>> bytes(mips*layers);
    std::vector<D3D11_SUBRESOURCE_DATA> data(mips*layers);
    std::vector<uint64_t> texels(mips*layers),above(mips*layers);
    for (UINT layer=0;layer<layers;++layer) for (UINT mip=0;mip<mips;++mip) {
        const auto i=layer*mips+mip,w=std::max(1u,width>>mip),h=std::max(1u,height>>mip);
        const UINT columns=compressed?(w+3)/4:w,rows=compressed?(h+3)/4:h;
        const UINT pitch=columns*elementBytes+16;
        bytes[i].assign(pitch*rows,0xff);
        texels[i]=static_cast<uint64_t>(w)*h;
        const auto kind=i%3;
        for (UINT y=0;y<rows;++y) for (UINT x=0;x<columns;++x) {
            auto* p=bytes[i].data()+y*pitch+x*elementBytes;
            if (bc1) {
                memset(p,0,8); p[kind==0?0:2]=1;
                memset(p+4,kind==2?0xe4:0xff,4);
            } else if(bc3) {
                uint64_t selectors{};
                if(kind==2)for(UINT pixel=0;pixel<16;++pixel)selectors|=static_cast<uint64_t>(pixel%2?0:1)<<(3*pixel);
                Bc3Block(p,kind==0?255:kind==1?0:127,kind==0?0:kind==1?255:128,selectors);
            } else {
                p[3]=kind==0?255:kind==1?0:(x%2?127:128);
            }
        }
        if (kind==0) above[i]=texels[i];
        else if (kind==2) above[i]=static_cast<uint64_t>(bc1?w-w/4:(w+1)/2)*h;
        data[i]={bytes[i].data(),pitch,0};
    }
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=width; desc.Height=height; desc.MipLevels=mips; desc.ArraySize=layers;
    desc.Format=format; desc.SampleDesc.Count=1; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc,data.data(),&texture))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    viewDesc.Format=format==DXGI_FORMAT_BC1_TYPELESS?DXGI_FORMAT_BC1_UNORM:
        format==DXGI_FORMAT_BC3_TYPELESS?DXGI_FORMAT_BC3_UNORM:format;
    viewDesc.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY; viewDesc.Texture2DArray={0,mips,0,layers};
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(device->CreateShaderResourceView(texture.Get(),&viewDesc,&view))) return false;
    auto* binding=view.Get(); context->PSSetShaderResources(3,1,&binding);
    uint64_t budget=1024*1024;
    const auto baseline=grid2vr::DigestTexture(context,texture.Get(),budget);
    const auto digest=grid2vr::DigestTexture(context,texture.Get(),budget,true);
    if (strcmp(baseline.status,"ok") || strcmp(digest.status,"ok") || !digest.alphaCoverageRequested ||
        digest.subresources.size()!=texels.size() || budget!=1024*1024-2*digest.bytes) return false;
    for (size_t i=0;i<texels.size();++i) if (!AlphaEquals(digest.subresources[i].alpha,texels[i],above[i]) ||
        digest.subresources[i].sha256!=baseline.subresources[i].sha256) return false;
    // No resource-pointer cache: update the last, one-texel mip to the opposite alpha.
    auto& last=bytes.back();
    if (bc1) last[4]=static_cast<unsigned char>((last[4]&0xfc)|(above.back()?3:0));
    else if(bc3)last[0]=above.back()?0:255;
    else last[3]=above.back()?0:255;
    context->UpdateSubresource(texture.Get(),mips*layers-1,nullptr,last.data(),data.back().SysMemPitch,0);
    const auto changed=grid2vr::DigestTexture(context,texture.Get(),budget,true);
    if (strcmp(changed.status,"ok") || !AlphaEquals(changed.subresources.back().alpha,1,above.back()?0:1) ||
        changed.subresources.back().sha256==digest.subresources.back().sha256) return false;
    for (size_t i=0;i+1<texels.size();++i) if (changed.subresources[i].sha256!=digest.subresources[i].sha256 ||
        !AlphaEquals(changed.subresources[i].alpha,texels[i],above[i])) return false;
    uint64_t shortBudget=digest.bytes-1;
    const auto refused=grid2vr::DigestTexture(context,texture.Get(),shortBudget,true);
    if (strcmp(refused.status,"budget") || shortBudget!=digest.bytes-1) return false;
    for (const auto& item:refused.subresources) if (strcmp(item.alpha.status,"budget") || item.alpha.texels || !item.sha256.empty()) return false;
    ComPtr<ID3D11ShaderResourceView> actual; context->PSGetShaderResources(3,1,&actual);
    if (actual.Get()!=binding) return false;
    std::ostringstream originalJson,alphaJson;
    grid2vr::WriteTextureDigest(originalJson,baseline); grid2vr::WriteTextureDigest(alphaJson,digest);
    if (originalJson.str().find("alpha")!=std::string::npos || alphaJson.str().find("\"threshold\":0.5")==std::string::npos ||
        alphaJson.str().find("whole_mip_unfiltered_not_object_uv_or_pixel_visibility")==std::string::npos ||
        alphaJson.str().find("\"atLeastHalf\":")==std::string::npos) return false;
    return true;
}
bool Check(ID3D11Device* device,ID3D11DeviceContext* context,DXGI_FORMAT format,UINT w,UINT h,
           UINT mips,UINT layers,UINT block,UINT size,const std::vector<std::string>& expected) {
    std::vector<std::vector<unsigned char>> contents(mips*layers);
    std::vector<D3D11_SUBRESOURCE_DATA> data(mips*layers);
    for (UINT layer=0;layer<layers;++layer) for (UINT mip=0;mip<mips;++mip) {
        const auto i=layer*mips+mip;
        const auto row=((std::max(1u,w>>mip)+block-1)/block)*size;
        const auto rows=(std::max(1u,h>>mip)+block-1)/block;
        contents[i].resize(row*rows);
        for (UINT b=0;b<row*rows;++b) contents[i][b]=static_cast<unsigned char>(mip*17+layer*29+b);
        data[i]={contents[i].data(),row,0};
    }
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=w; desc.Height=h; desc.MipLevels=mips; desc.ArraySize=layers;
    desc.Format=format; desc.SampleDesc.Count=1; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc,data.data(),&texture))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format=format==DXGI_FORMAT_R16_TYPELESS ? DXGI_FORMAT_R16_UNORM:format;
    vd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY; vd.Texture2DArray={0,mips,0,layers};
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(device->CreateShaderResourceView(texture.Get(),&vd,&view))) return false;
    auto* binding=view.Get(); context->PSSetShaderResources(2,1,&binding);
    uint64_t budget=1024*1024;
    auto digest=grid2vr::DigestTexture(context,texture.Get(),budget);
    if (strcmp(digest.status,"ok") || digest.subresources.size()!=expected.size()) return false;
    for (UINT i=0;i<expected.size();++i) {
        if (digest.subresources[i].sha256!=expected[i]) { printf("hash mismatch format=%u sub=%u\n",format,i); return false; }
        if (digest.subresources[i].mip!=i%mips || digest.subresources[i].slice!=i/mips) return false;
    }
    if (budget!=1024*1024-digest.bytes) return false;
    ComPtr<ID3D11ShaderResourceView> actual; context->PSGetShaderResources(2,1,&actual);
    if (actual.Get()!=binding) return false;
    // A later update to the same object must be seen; other mips stay intact.
    contents.back()[0]^=1;
    context->UpdateSubresource(texture.Get(),mips*layers-1,nullptr,contents.back().data(),data.back().SysMemPitch,0);
    auto changed=grid2vr::DigestTexture(context,texture.Get(),budget);
    if (strcmp(changed.status,"ok") || changed.subresources.back().sha256==expected.back()) return false;
    for (UINT i=0;i+1<expected.size();++i) if (changed.subresources[i].sha256!=expected[i]) return false;
    uint64_t shortBudget=digest.bytes-1;
    if (strcmp(grid2vr::DigestTexture(context,texture.Get(),shortBudget).status,"budget") || shortBudget!=digest.bytes-1) return false;
    if (format==DXGI_FORMAT_BC3_UNORM) {
        const auto alpha=grid2vr::DigestTexture(context,texture.Get(),budget,true);
        if (strcmp(alpha.status,"ok")) return false;
        for (const auto& item:alpha.subresources) if (strcmp(item.alpha.status,"ok") ||
            !item.alpha.texels || item.alpha.texels!=item.alpha.atLeastHalf+item.alpha.belowHalf) return false;
    }
    return true;
}
int main() {
    if (!CheckAlphaDecoder()) return 8;
    if (!CheckBc3Decoder()) return 11;
    auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    if (!create) return 1;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    if (FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))) return 2;
    // Reference digests generated independently by Python hashlib over tight rows.
    if (!Check(device.Get(),context.Get(),DXGI_FORMAT_R16_TYPELESS,7,5,3,2,1,2,{
        "5767d69a906d4860db9079eb7e90ab4a543e5cb032fce846554aef6ceb600e1d",
        "3c95550f0abd5fea1f6ee4d91409377074fddf9cd7981417d917a9c92a03fb0c",
        "589d42f7903ca0d7a1bc554c4763957a3bc89a871358f9425f53741362a4cf31",
        "556d678e09a52140458a135dda27ef8155e8128ac582356a20412f96988ba3fc",
        "46bc16cddb57e0d2f2795fd11769da4e0dbebbfeedadd74b6e9363a458aaa9d6",
        "6b161a4056ace4511def9dd155dde47a5456e27eb2fa9d72616aa2798092a8fe"})) return 3;
    if (!Check(device.Get(),context.Get(),DXGI_FORMAT_BC1_UNORM,8,8,4,2,4,8,{
        "630dcd2966c4336691125448bbb25b4ff412a49c732db2c8abc1b8581bd710dd",
        "ccad45ac0b2662a91df84d7b5948daea157adc20dea7734a535beea38e087c8e",
        "5368eaa9c96ea339d3828e954618f9a853e52042f1828df0d2ac356615f4d369",
        "a1e507f0a4dbaf5a7f445318f6a5aaab76ae66bb1f7d42d2272ad7578bcdda6a",
        "04af9c742de708e1fc00539f51f235d5369b58294c1cbedbf83bc2b5e3c91aa0",
        "942234b13d6ca04fefde3ca0442c36deec26562c7c2cd15d731cf1ed24289083",
        "19fe10c27cc06e1c1acc5ae596e0c06415299ed6bc735f50d64da69336f92f6f",
        "86912194063de377f14ee690d64e798822ea3a58d224d8a88a1c255774bc6e5f"})) return 4;
    if (!Check(device.Get(),context.Get(),DXGI_FORMAT_BC3_UNORM,12,8,4,1,4,16,{
        "08359b108fa567f5dcf319fa3434da6abbc1d595f426372666447f09cc5a87dc",
        "0477fd8e15bc68a8d1ce22781fdb90deddd7cdf657e527f95fc98bbe6918337e",
        "a9b96639c79fb8a190fa3f171e353fa019a1fe6c64388268778e14f3bdaf2ad6",
        "0813d225df5e878c22d463b277fe1f5b09f3240c29de9d3bc2dde5e3a55c640b"})) return 5;
    uint64_t budget=1024;
    if (strcmp(grid2vr::DigestTexture(context.Get(),nullptr,budget).status,"unbound")) return 6;
    ComPtr<ID3D11DeviceContext> deferred; device->CreateDeferredContext(0,&deferred);
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=4; desc.Height=4; desc.MipLevels=1; desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=4; desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> msaa;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&msaa)) ||
        strcmp(grid2vr::DigestTexture(context.Get(),msaa.Get(),budget).status,"unsupported") ||
        strcmp(grid2vr::DigestTexture(deferred.Get(),msaa.Get(),budget).status,"unsupported")) return 7;
    for (const auto format:{DXGI_FORMAT_BC1_TYPELESS,DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_BC1_UNORM_SRGB})
        if (!CheckAlphaTexture(device.Get(),context.Get(),format,true)) { printf("BC1 alpha test failed format=%u\n",format); return 9; }
    for (const auto format:{DXGI_FORMAT_BC3_TYPELESS,DXGI_FORMAT_BC3_UNORM,DXGI_FORMAT_BC3_UNORM_SRGB})
        if (!CheckAlphaTexture(device.Get(),context.Get(),format,false)) { printf("BC3 alpha test failed format=%u\n",format); return 12; }
    for (const auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB})
        if (!CheckAlphaTexture(device.Get(),context.Get(),format,false)) { printf("RGBA alpha test failed format=%u\n",format); return 10; }
    puts("GPU mip/array/block/depth hashes, BC1/BC3/RGBA alpha, padding, updates, budget, bindings and refusals pass.");
    return 0;
}
