#include "mirror_source_probe.h"
#include <fstream>
using Microsoft::WRL::ComPtr;
namespace grid2vr {
namespace {
bool Is(ID3D11ShaderResourceView* view,D3D11_SRV_DIMENSION dimension) {
    if (!view) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};view->GetDesc(&desc);
    return desc.ViewDimension==dimension;
}
}
bool MirrorSourceProbe::Initialize(ID3D11DeviceContext* context) {
    if (!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    ComPtr<ID3D11Device> device;context->GetDevice(&device);
    if (!device) return false;
    constexpr uint32_t black=0;
    D3D11_TEXTURE2D_DESC desc{1,1,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_IMMUTABLE,D3D11_BIND_SHADER_RESOURCE};
    const D3D11_SUBRESOURCE_DATA flat{&black,4,0};ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc,&flat,&texture)) || FAILED(device->CreateShaderResourceView(texture.Get(),nullptr,&flat_))) return false;
    desc.ArraySize=6;desc.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE;
    std::array<D3D11_SUBRESOURCE_DATA,6> faces{};for (auto& face:faces) face={&black,4,0};
    ComPtr<ID3D11Texture2D> cube;
    if (FAILED(device->CreateTexture2D(&desc,faces.data(),&cube)) || FAILED(device->CreateShaderResourceView(cube.Get(),nullptr,&cube_view_))) return false;
    context_=context;thread_=GetCurrentThreadId();return true;
}
MirrorSourceProbe::Result MirrorSourceProbe::Apply(ID3D11DeviceContext* context,uint64_t hash) {
    const auto* shader=Find(hash);
    if (!shader || pending_ || !context) return Result::Refused;
    if (!context_ && !Initialize(context)) return Result::Failed;
    if (thread_!=GetCurrentThreadId() || context_.Get()!=context) return Result::Refused;
    // Blank one input per phase; the other stays the game's own.
    const std::array<UINT,2> slots{cube_?none:shader->screenSlot,cube_?shader->cubeSlot:none};
    if (slots[0]==none && slots[1]==none) return Result::Refused;
    const std::array<D3D11_SRV_DIMENSION,2> kinds{D3D11_SRV_DIMENSION_TEXTURE2D,D3D11_SRV_DIMENSION_TEXTURECUBE};
    std::array<ComPtr<ID3D11ShaderResourceView>,2> views;
    for (size_t i=0;i<2;++i) {
        if (slots[i]==none) continue;
        context->PSGetShaderResources(slots[i],1,views[i].GetAddressOf());
        // Only replace an input that is bound and of the declared kind.
        if (!Is(views[i].Get(),kinds[i])) return Result::Refused;
    }
    original_=views;slots_=slots;pending_=true;
    const std::array<ID3D11ShaderResourceView*,2> tints{flat_.Get(),cube_view_.Get()};
    bool applied=true;
    for (size_t i=0;i<2;++i) {
        if (slots[i]==none) continue;
        auto* tint=tints[i];context->PSSetShaderResources(slots[i],1,&tint);
        ComPtr<ID3D11ShaderResourceView> bound;context->PSGetShaderResources(slots[i],1,&bound);
        applied&=bound.Get()==tint;
    }
    if (!applied) {Restore(context);return Result::Failed;}
    return Result::Applied;
}
bool MirrorSourceProbe::DumpCube(ID3D11DeviceContext* context,uint64_t hash,const std::filesystem::path& folder,std::string& summary) {
    const auto* shader=Find(hash);
    if (!shader || shader->cubeSlot==none || pending_ || !context || dumped_>=dumpedResources_.size()) return false;
    if (context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    ComPtr<ID3D11ShaderResourceView> view;context->PSGetShaderResources(shader->cubeSlot,1,&view);
    if (!Is(view.Get(),D3D11_SRV_DIMENSION_TEXTURECUBE)) return false;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    for (unsigned i=0;i<dumped_;++i) if (dumpedResources_[i]==resource.Get()) return false;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) return false;
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};view->GetDesc(&viewDesc);
    if (desc.ArraySize<6 || desc.SampleDesc.Count!=1 || desc.Width>4096 || desc.Height>4096) return false;
    D3D11_TEXTURE2D_DESC staging{desc.Width,desc.Height,1,desc.ArraySize,desc.Format,{1,0},D3D11_USAGE_STAGING,0,D3D11_CPU_ACCESS_READ,0};
    ComPtr<ID3D11Device> device;context->GetDevice(&device);ComPtr<ID3D11Texture2D> copy;
    if (FAILED(device->CreateTexture2D(&staging,nullptr,&copy))) return false;
    for (UINT face=0;face<desc.ArraySize;++face)
        context->CopySubresourceRegion(copy.Get(),D3D11CalcSubresource(0,face,1),0,0,0,texture.Get(),D3D11CalcSubresource(0,face,desc.MipLevels),nullptr);
    const unsigned index=dumped_;dumpedResources_[dumped_++]=resource.Get();
    UINT rowPitch{};
    for (UINT face=0;face<desc.ArraySize && face<6;++face) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(copy.Get(),D3D11CalcSubresource(0,face,1),D3D11_MAP_READ,0,&mapped))) return false;
        rowPitch=mapped.RowPitch;
        // Whole mip 0 as laid out: block-compressed formats have Height/4 rows.
        const UINT rows=(desc.Format>=DXGI_FORMAT_BC1_TYPELESS && desc.Format<=DXGI_FORMAT_BC5_SNORM) ||
            (desc.Format>=DXGI_FORMAT_BC6H_TYPELESS && desc.Format<=DXGI_FORMAT_BC7_UNORM_SRGB)?(desc.Height+3)/4:desc.Height;
        std::ofstream file(folder/(L"mirror-cube-"+std::to_wstring(index)+L"-face"+std::to_wstring(face)+L".bin"),std::ios::binary);
        file.write(static_cast<const char*>(mapped.pData),std::streamsize(size_t(mapped.RowPitch)*rows));
        context->Unmap(copy.Get(),D3D11CalcSubresource(0,face,1));
    }
    summary="cube="+std::to_string(index)+" width="+std::to_string(desc.Width)+" height="+std::to_string(desc.Height)+
        " mips="+std::to_string(desc.MipLevels)+" array="+std::to_string(desc.ArraySize)+" format="+std::to_string(desc.Format)+
        " view_format="+std::to_string(viewDesc.Format)+" row_pitch="+std::to_string(rowPitch)+" bind="+std::to_string(desc.BindFlags)+
        " misc="+std::to_string(desc.MiscFlags)+" slot="+std::to_string(shader->cubeSlot);
    return true;
}
bool MirrorSourceProbe::Restore(ID3D11DeviceContext* context) {
    if (!pending_ || context_.Get()!=context || thread_!=GetCurrentThreadId()) return false;
    const std::array<ID3D11ShaderResourceView*,2> tints{flat_.Get(),cube_view_.Get()};
    bool restored=true;
    for (size_t i=0;i<2;++i) {
        if (slots_[i]==none) continue;
        ComPtr<ID3D11ShaderResourceView> before;context->PSGetShaderResources(slots_[i],1,&before);
        restored&=before.Get()==tints[i];
        auto* view=original_[i].Get();context->PSSetShaderResources(slots_[i],1,&view);
        ComPtr<ID3D11ShaderResourceView> after;context->PSGetShaderResources(slots_[i],1,&after);
        restored&=after.Get()==view;
    }
    original_={};slots_={none,none};pending_=false;
    return restored;
}
}
