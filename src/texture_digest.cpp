#include "texture_digest.h"
#include <bcrypt.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>

namespace grid2vr {
namespace {
struct Layout { UINT blockWidth{},blockHeight{},bytes{}; };
Layout FormatLayout(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return {1,1,16};
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R32G32_FLOAT: return {1,1,8};
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R16G16_UNORM: return {1,1,4};
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R8G8_UNORM: return {1,1,2};
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_A8_UNORM: return {1,1,1};
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM: return {4,4,8};
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: return {4,4,16};
    default: return {};
    }
}
std::string HashRows(const D3D11_MAPPED_SUBRESOURCE& mapped,UINT rowBytes,UINT rows) {
    if (!mapped.pData || mapped.RowPitch<rowBytes) return {};
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) return {};
    bool ok=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    for (UINT row=0;ok && row<rows;++row)
        ok=BCryptHashData(hash,static_cast<PUCHAR>(mapped.pData)+static_cast<size_t>(row)*mapped.RowPitch,rowBytes,0)>=0;
    std::array<UCHAR,32> digest{};
    if (ok) ok=BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm,0);
    char text[65]{};
    if (ok) for (unsigned i=0;i<32;++i) sprintf_s(text+i*2,3,"%02x",digest[i]);
    return text;
}
}
AlphaCoverageDigest CountTextureAlpha(DXGI_FORMAT format,const D3D11_MAPPED_SUBRESOURCE& mapped,UINT width,UINT height) {
    AlphaCoverageDigest result;
    const bool bc1=format==DXGI_FORMAT_BC1_TYPELESS || format==DXGI_FORMAT_BC1_UNORM || format==DXGI_FORMAT_BC1_UNORM_SRGB;
    const bool bc3=format==DXGI_FORMAT_BC3_TYPELESS || format==DXGI_FORMAT_BC3_UNORM || format==DXGI_FORMAT_BC3_UNORM_SRGB;
    const bool rgba=format==DXGI_FORMAT_R8G8B8A8_UNORM || format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        format==DXGI_FORMAT_B8G8R8A8_UNORM || format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (!bc1 && !bc3 && !rgba) { result.status="unsupported_format"; return result; }
    const uint64_t rowBytes=(bc1 || bc3) ? ((static_cast<uint64_t>(width)+3)/4)*(bc3?16:8) : static_cast<uint64_t>(width)*4;
    const uint64_t rows=(bc1 || bc3) ? (static_cast<uint64_t>(height)+3)/4 : height;
    if (!mapped.pData || !width || !height || width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || mapped.RowPitch<rowBytes ||
        rows*static_cast<uint64_t>(mapped.RowPitch)>std::numeric_limits<size_t>::max()) {
        result.status="invalid_layout"; return result;
    }
    const auto* bytes=static_cast<const unsigned char*>(mapped.pData);
    if (bc1) {
        for (UINT y=0;y<height;y+=4) for (UINT x=0;x<width;x+=4) {
            const auto* block=bytes+static_cast<size_t>(y/4)*mapped.RowPitch+static_cast<size_t>(x/4)*8;
            const UINT endpoint0=block[0] | (static_cast<UINT>(block[1])<<8);
            const UINT endpoint1=block[2] | (static_cast<UINT>(block[3])<<8);
            const UINT columns=std::min(4u,width-x),rowsInBlock=std::min(4u,height-y);
            // BC1 selector 3 is transparent only in the three-colour mode.
            if (endpoint0>endpoint1) { result.atLeastHalf+=columns*rowsInBlock; continue; }
            if (block[4]==255 && block[5]==255 && block[6]==255 && block[7]==255) continue;
            for (UINT by=0;by<rowsInBlock;++by) for (UINT bx=0;bx<columns;++bx)
                if (((block[4+by]>>(2*bx))&3)!=3) ++result.atLeastHalf;
        }
    } else if (bc3) {
        for (UINT y=0;y<height;y+=4) for (UINT x=0;x<width;x+=4) {
            const auto* block=bytes+static_cast<size_t>(y/4)*mapped.RowPitch+static_cast<size_t>(x/4)*16;
            const UINT a=block[0],b=block[1],columns=std::min(4u,width-x),rowsInBlock=std::min(4u,height-y);
            std::array<bool,8> above{};above[0]=a>=128;above[1]=b>=128;
            const UINT denominator=a>b?7:5;
            for(UINT i=2;i<=denominator;++i) {
                const UINT numerator=(denominator+1-i)*a+(i-1)*b;
                // Compare exact interpolated alpha/255 >= 0.5, without first
                // rounding its value to an eight-bit intermediate.
                above[i]=2*numerator>=denominator*255;
            }
            if(a<=b) {above[6]=false;above[7]=true;}
            if(std::all_of(above.begin(),above.end(),[](bool value){return value;})) {
                result.atLeastHalf+=columns*rowsInBlock;continue;
            }
            uint64_t selectors{};
            for(UINT i=0;i<6;++i)selectors|=static_cast<uint64_t>(block[2+i])<<(8*i);
            for(UINT by=0;by<rowsInBlock;++by)for(UINT bx=0;bx<columns;++bx)
                if(above[(selectors>>(3*(by*4+bx)))&7])++result.atLeastHalf;
        }
    } else {
        for (UINT y=0;y<height;++y) for (UINT x=0;x<width;++x) {
            // sRGB affects colour only; alpha remains UNORM.
            if (bytes[static_cast<size_t>(y)*mapped.RowPitch+static_cast<size_t>(x)*4+3]>=128) ++result.atLeastHalf;
        }
    }
    result.texels=static_cast<uint64_t>(width)*height;
    result.belowHalf=result.texels-result.atLeastHalf;
    result.status="ok";
    return result;
}
TextureDigest DigestTexture(ID3D11DeviceContext* context,ID3D11Resource* resource,uint64_t& budget,bool alphaCoverage) {
    TextureDigest result;
    result.alphaCoverageRequested=alphaCoverage;
    if (!resource) return result;
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Texture2D> texture,staging;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) { result.status="not_texture2d"; return result; }
    texture->GetDesc(&result.desc);
    const auto& desc=result.desc;
    const auto layout=FormatLayout(desc.Format);
    if (!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || !layout.bytes || desc.SampleDesc.Count!=1 ||
        !desc.MipLevels || desc.MipLevels>15 || !desc.ArraySize || desc.ArraySize>2048) {
        result.status="unsupported"; return result;
    }
    for (UINT slice=0;slice<desc.ArraySize;++slice) for (UINT mip=0;mip<desc.MipLevels;++mip) {
        const UINT width=std::max(1u,desc.Width>>mip),height=std::max(1u,desc.Height>>mip);
        SubresourceDigest item{mip,slice,((width+layout.blockWidth-1)/layout.blockWidth)*layout.bytes,
            (height+layout.blockHeight-1)/layout.blockHeight,{}};
        if (alphaCoverage) item.alpha.status="not_read";
        result.bytes+=static_cast<uint64_t>(item.rowBytes)*item.rows;
        result.subresources.push_back(item);
    }
    if (result.bytes>64ull*1024*1024 || result.bytes>budget) {
        result.status="budget";
        if (alphaCoverage) for (auto& item:result.subresources) item.alpha.status="budget";
        return result;
    }
    // Charge attempts too so failed maps cannot silently escape the readback cap.
    budget-=result.bytes;
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    auto copy=desc; copy.Usage=D3D11_USAGE_STAGING; copy.BindFlags=0; copy.MiscFlags=0; copy.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&copy,nullptr,&staging))) {
        result.status="allocation_failed";
        if (alphaCoverage) for (auto& item:result.subresources) item.alpha.status="allocation_failed";
        return result;
    }
    context->CopyResource(staging.Get(),texture.Get());
    for (auto& item:result.subresources) {
        const auto subresource=D3D11CalcSubresource(item.mip,item.slice,desc.MipLevels);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(),subresource,D3D11_MAP_READ,0,&mapped))) {
            result.status="map_failed";
            if (alphaCoverage) item.alpha.status="map_failed";
            return result;
        }
        item.sha256=HashRows(mapped,item.rowBytes,item.rows);
        if (alphaCoverage) item.alpha=CountTextureAlpha(desc.Format,mapped,std::max(1u,desc.Width>>item.mip),std::max(1u,desc.Height>>item.mip));
        context->Unmap(staging.Get(),subresource);
        if (item.sha256.empty()) { result.status="hash_failed"; return result; }
    }
    result.status="ok";
    return result;
}
void WriteTextureDigest(std::ostream& out,const TextureDigest& digest) {
    const auto& d=digest.desc;
    out << "{\"status\":\"" << digest.status << "\",\"width\":" << d.Width << ",\"height\":" << d.Height
        << ",\"format\":" << d.Format << ",\"mips\":" << d.MipLevels << ",\"array\":" << d.ArraySize
        << ",\"samples\":" << d.SampleDesc.Count << ",\"quality\":" << d.SampleDesc.Quality
        << ",\"bytes\":" << digest.bytes;
    if (digest.alphaCoverageRequested) out << ",\"alphaCoverage\":{\"threshold\":0.5,\"comparison\":\"greater_equal\","
        "\"scope\":\"whole_mip_unfiltered_not_object_uv_or_pixel_visibility\"}";
    out << ",\"subresources\":[";
    bool comma=false;
    for (const auto& item:digest.subresources) {
        if (comma) out << ','; comma=true;
        out << "{\"mip\":" << item.mip << ",\"slice\":" << item.slice << ",\"rowBytes\":" << item.rowBytes
            << ",\"rows\":" << item.rows << ",\"sha256\":\"" << item.sha256 << "\"";
        if (digest.alphaCoverageRequested) out << ",\"alpha\":{\"status\":\"" << item.alpha.status << "\",\"texels\":" << item.alpha.texels
            << ",\"atLeastHalf\":" << item.alpha.atLeastHalf << ",\"belowHalf\":" << item.alpha.belowHalf << '}';
        out << '}';
    }
    out << "]}";
}
}
