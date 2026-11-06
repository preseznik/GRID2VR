#include "sponsor_depth.h"
#include "guarded_hooks.h"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace grid2vr {
namespace {
using Microsoft::WRL::ComPtr;
using namespace sponsordepth;
constexpr GUID textureTag{0x5b1d7c42,0x3e8a,0x4f61,{0x9a,0x2c,0x41,0x7e,0x0d,0x93,0xb6,0x15}};
constexpr GUID viewTag{0x5b1d7c43,0x3e8a,0x4f61,{0x9a,0x2c,0x41,0x7e,0x0d,0x93,0xb6,0x15}};
constexpr size_t cacheLimit=64;
constexpr const char* catalog[]{
#include "sponsor_board_catalog.inc"
};
std::string Hash(const unsigned char* data,size_t bytes) {
    BCRYPT_ALG_HANDLE alg{};BCRYPT_HASH_HANDLE hash{};std::array<UCHAR,32> result{};
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return {};
    bool ok=BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)>=0;
    if(ok)ok=BCryptHashData(hash,const_cast<PUCHAR>(data),static_cast<ULONG>(bytes),0)>=0 &&
        BCryptFinishHash(hash,result.data(),static_cast<ULONG>(result.size()),0)>=0;
    if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(alg,0);
    std::string text;
    if(ok)for(auto b:result) {text+="0123456789abcdef"[b>>4];text+="0123456789abcdef"[b&15];}
    return text;
}
// Mip 0 rows without driver padding, read with guarded copies from the game's upload.
std::string HashMip0(const D3D11_SUBRESOURCE_DATA* data) {
    D3D11_SUBRESOURCE_DATA mip{};
    if(!data || !ReadMemory(data,&mip,sizeof(mip)) || !mip.pSysMem || mip.SysMemPitch<mip0Pitch || mip.SysMemPitch>1024*1024)return {};
    std::vector<unsigned char> tight(mip0Bytes);
    for(UINT row=0;row<mip0Rows;++row) {
        const auto address=reinterpret_cast<uintptr_t>(mip.pSysMem)+static_cast<uint64_t>(row)*mip.SysMemPitch;
        if(address>UINTPTR_MAX || !ReadMemory(reinterpret_cast<const void*>(static_cast<uintptr_t>(address)),tight.data()+size_t(row)*mip0Pitch,mip0Pitch))return {};
    }
    return Hash(tight.data(),tight.size());
}
}
void SponsorDepth::Configure(const std::filesystem::path& run) noexcept {
    try {
        const auto request=run/L"sponsor-depth-bias.request";
        if(!std::filesystem::is_regular_file(request))return;
        folder=run;
        if(std::filesystem::file_size(request)>256)throw std::runtime_error("request_size");
        std::ifstream input(request,std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)),{});
        Settings parsed;if(!ParseSettings(text,parsed))throw std::runtime_error("request_settings");
        settings=parsed;identities.assign(std::begin(catalog),std::end(catalog));
        if(identities.empty())throw std::runtime_error("empty_catalog");
        enabled=true;
    } catch(const std::exception& e) {enabled=false;refusal=e.what();}
    catch(...) {enabled=false;refusal="request";}
    Summary();
}
void SponsorDepth::HookFailed(const char* reason) noexcept {
    enabled=false;try {refusal=reason?reason:"hook";}catch(...) {}Summary();
}
void SponsorDepth::TextureCreated(const D3D11_TEXTURE2D_DESC* requested,const D3D11_SUBRESOURCE_DATA* data,ID3D11Texture2D* texture) noexcept {
    if(!Enabled() || !requested || !texture)return;
    try {
        D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
        if(!Candidate(d))return;
        ++candidates;
        if(!data) {++withoutData;return;}
        const auto hash=HashMip0(data);
        if(hash.empty() || std::find(identities.begin(),identities.end(),hash)==identities.end())return;
        const UINT tag=1;
        if(SUCCEEDED(texture->SetPrivateData(textureTag,sizeof(tag),&tag)) && matched.fetch_add(1)<4)Summary();
    } catch(...) {}
}
void SponsorDepth::ViewCreated(ID3D11Resource* resource,ID3D11ShaderResourceView* view) noexcept {
    if(!Enabled() || !resource || !view)return;
    UINT tag{},size=sizeof(tag);
    if(FAILED(resource->GetPrivateData(textureTag,&size,&tag)) || tag!=1)return;
    if(FAILED(view->SetPrivateData(viewTag,sizeof(tag),&tag)))return;
    try {
        std::lock_guard lock(cacheMutex);
        const auto count=retainedViewCount.load(std::memory_order_relaxed);
        const auto end=retainedViews.begin()+count;
        if(std::find_if(retainedViews.begin(),end,[&](const auto& saved){return saved.Get()==view;})==end) {
            if(count<viewCacheLimit) {
                retainedViews[count]=view;
                retainedViewCount.store(count+1,std::memory_order_release);
            } else viewCacheFull.store(true,std::memory_order_release);
        }
    } catch(...) {viewCacheFull.store(true,std::memory_order_release);}
    ++taggedViews;
}
bool SponsorDepth::Sponsor(ID3D11ShaderResourceView* view) const noexcept {
    if(!view)return false;
    const auto count=retainedViewCount.load(std::memory_order_acquire);
    for(unsigned i=0;i<count;++i)if(retainedViews[i].Get()==view)return true;
    if(!viewCacheFull.load(std::memory_order_acquire))return false;
    UINT tag{},size=sizeof(tag);
    return SUCCEEDED(view->GetPrivateData(viewTag,&size,&tag)) && tag==1;
}
SponsorDepth::Token SponsorDepth::Begin(ID3D11DeviceContext* context) noexcept {
    Token token;
    if(!Enabled() || !context || !taggedViews.load())return token;
    try {
        std::array<ID3D11ShaderResourceView*,viewSlots> views{};
        context->PSGetShaderResources(0,viewSlots,views.data());
        bool sponsor=false;
        for(auto* view:views) {sponsor=sponsor || Sponsor(view);if(view)view->Release();}
        if(!sponsor)return token;
        ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11DepthStencilView> depth;
        context->OMGetRenderTargets(1,&target,&depth);
        if(!target || !depth) {++skippedTarget;return token;}
        ComPtr<ID3D11DepthStencilState> depthState;UINT reference{};
        context->OMGetDepthStencilState(&depthState,&reference);
        D3D11_DEPTH_STENCIL_DESC depthDesc=DefaultDepthStencil();if(depthState)depthState->GetDesc(&depthDesc);
        const int sign=BiasSign(depthDesc);
        if(!sign) {++skippedDepthTest;return token;}
        ComPtr<ID3D11RasterizerState> original;context->RSGetState(&original);
        ComPtr<ID3D11RasterizerState> biased;
        {
            std::lock_guard lock(cacheMutex);
            const auto found=std::find_if(cache.begin(),cache.end(),[&](const Cached& c){return c.original.Get()==original.Get() && c.sign==sign;});
            if(found!=cache.end())biased=found->biased;
            else {
                if(cache.size()>=cacheLimit) {++cacheFull;return token;}
                D3D11_RASTERIZER_DESC desc=DefaultRasterizer();if(original)original->GetDesc(&desc);
                ComPtr<ID3D11Device> device;context->GetDevice(&device);
                const auto wanted=Biased(desc,sign,settings);
                if(!device || FAILED(device->CreateRasterizerState(&wanted,&biased)) || !biased) {++stateFailures;return token;}
                cache.push_back({original,biased,sign});++cachedStates;
            }
        }
        context->RSSetState(biased.Get());
        token.original=original;token.active=true;
        // Refresh the receipt at 1, 2, 4 ... 4096 and then every 4096 draws: bounded unattended
        // runs stop the game without the exit path, so the receipt must not wait for it.
        const auto count=biasedDraws.fetch_add(1)+1;
        if((count&(count-1))==0 ? count<=4096 : (count%4096)==0)Summary();
    } catch(...) {token={};}
    return token;
}
void SponsorDepth::End(ID3D11DeviceContext* context,Token& token) noexcept {
    if(!token.active || !context)return;
    context->RSSetState(token.original.Get());token={};
}
void SponsorDepth::Summary() noexcept {
    if(folder.empty())return;
    try {
        std::lock_guard lock(logMutex);
        std::ofstream out(folder/L"sponsor-depth-bias.json",std::ios::trunc);
        out<<"{\"schema\":1,\"enabled\":"<<(Enabled()?"true":"false")<<",\"refusal\":\""<<refusal
            <<"\",\"units\":"<<settings.units<<",\"slope\":"<<settings.slope<<",\"identities\":"<<identities.size()
            <<",\"candidateTextures\":"<<candidates<<",\"withoutInitialData\":"<<withoutData<<",\"matchedTextures\":"<<matched
            <<",\"taggedViews\":"<<taggedViews<<",\"biasedDraws\":"<<biasedDraws<<",\"skippedNoColourOrDepth\":"<<skippedTarget
            <<",\"skippedDepthTest\":"<<skippedDepthTest<<",\"stateFailures\":"<<stateFailures<<",\"cacheFull\":"<<cacheFull
            <<",\"cachedStates\":"<<cachedStates<<",\"retainedViews\":"<<retainedViewCount
            <<",\"viewCacheOverflow\":"<<(viewCacheFull?"true":"false")
            <<",\"gameFilesModified\":false,\"visualAcceptance\":false}\n";
        ++summaries;
    } catch(...) {}
}
#ifdef GRID2VR_SPONSOR_DEPTH_TEST
void SponsorDepth::ConfigureTest(const std::filesystem::path& run,std::vector<std::string> ids,Settings s) {
    folder=run;identities=std::move(ids);settings=s;enabled=true;
}
std::string SponsorDepth::Mip0Hash(const D3D11_SUBRESOURCE_DATA* data) { return HashMip0(data); }
#endif
}
