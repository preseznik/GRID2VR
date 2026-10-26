#include "focus_cockpit.h"
#include "guarded_hooks.h"
#include <bcrypt.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace grid2vr {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID replacementId{0x7f4610b5,0x0f33,0x420b,{0xa1,0x19,0xa6,0xce,0x3c,0x44,0x61,0x22}};
constexpr GUID observedId{0xe8a54f64,0x3292,0x4194,{0x97,0x51,0xd9,0x3d,0x66,0x05,0x98,0xc5}};
constexpr size_t packBytes=2796344,sourceBytes=174776;
constexpr char capability[]="GRID2VR_COCKPIT_TEXTURES_V1";
struct AtlasIdentity { const char* car;const char* sourceHash;const char* sourceHash8;const char* packHash; };
constexpr AtlasIdentity catalog[]{
#include "cockpit_texture_catalog.inc"
};
UINT Pitch(UINT size) { return std::max(1u,(size+3)/4)*8; }
UINT Rows(UINT size) { return std::max(1u,(size+3)/4); }
bool Bc1(DXGI_FORMAT f) { return f==DXGI_FORMAT_BC1_TYPELESS || f==DXGI_FORMAT_BC1_UNORM || f==DXGI_FORMAT_BC1_UNORM_SRGB; }
bool Candidate(const D3D11_TEXTURE2D_DESC& d) {
    return d.Width==512 && d.Height==512 && (d.MipLevels==8 || d.MipLevels==10) && d.ArraySize==1 && Bc1(d.Format) &&
        d.SampleDesc.Count==1 && d.SampleDesc.Quality==0 && d.BindFlags==D3D11_BIND_SHADER_RESOURCE &&
        (d.Usage==D3D11_USAGE_DEFAULT || d.Usage==D3D11_USAGE_IMMUTABLE) && !d.CPUAccessFlags && !d.MiscFlags;
}
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
bool PlainPath(std::filesystem::path path) {
    if(!path.is_absolute())return false;
    while(!path.empty()) {
        const auto attributes=GetFileAttributesW(path.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
        const auto parent=path.parent_path();if(parent==path)break;path=parent;
    }
    return true;
}
// Copy every uploaded BC1 mip, excluding driver padding, with guarded reads.
// GRID 2 stops at 4x4 (eight mips); the exported DDS also contains 2x2 and 1x1.
std::string SourceHash(const D3D11_SUBRESOURCE_DATA* data,UINT mipCount) {
    std::array<D3D11_SUBRESOURCE_DATA,10> mips{};
    if(!mipCount || mipCount>mips.size() || !ReadMemory(data,mips.data(),sizeof(mips[0])*mipCount))return {};
    std::vector<unsigned char> tight(sourceBytes);size_t offset{};
    for(UINT mip=0;mip<mipCount;++mip) {
        const UINT size=std::max(1u,512u>>mip),pitch=Pitch(size),rows=Rows(size);
        const auto& src=mips[mip];
        if(!src.pSysMem || src.SysMemPitch<pitch || src.SysMemPitch>1024*1024)return {};
        for(UINT row=0;row<rows;++row) {
            const auto address=reinterpret_cast<uintptr_t>(src.pSysMem)+static_cast<uint64_t>(row)*src.SysMemPitch;
            if(address>UINTPTR_MAX || !ReadMemory(reinterpret_cast<const void*>(static_cast<uintptr_t>(address)),tight.data()+offset,pitch))return {};
            offset+=pitch;
        }
    }
    return Hash(tight.data(),offset);
}
}
std::vector<unsigned char> FocusCockpit::Load(const std::filesystem::path& path,const std::string& expectedHash) {
    if(!PlainPath(path) || std::filesystem::file_size(path)!=packBytes)throw std::runtime_error("pack_missing_or_size");
    std::ifstream input(path,std::ios::binary);std::vector<unsigned char> bytes(packBytes);
    if(!input.read(reinterpret_cast<char*>(bytes.data()),bytes.size()) || input.peek()!=std::char_traits<char>::eof() ||
        Hash(bytes.data(),bytes.size())!=expectedHash)throw std::runtime_error("pack_hash");
    std::array<uint32_t,32> h{};memcpy(h.data(),bytes.data(),128);
    if(h[0]!=0x20534444 || h[1]!=124 || h[3]!=2048 || h[4]!=2048 || h[7]!=12 || h[19]!=32 ||
        h[20]!=4 || h[21]!=0x31545844 || h[28]!=0)throw std::runtime_error("pack_layout");
    return {bytes.begin()+128,bytes.end()};
}
void FocusCockpit::Configure(const std::filesystem::path& game,const std::filesystem::path& run) noexcept {
    try {
        const auto request=run/L"cockpit-textures.request";
        if(!std::filesystem::exists(request))return;
        folder=run;
        if(!PlainPath(request) || std::filesystem::file_size(request)>16)throw std::runtime_error("request");
        std::ifstream input(request);std::string text;std::getline(input,text);
        if(input.peek()!=std::char_traits<char>::eof() || (text!="Observe" && text!="Replace"))throw std::runtime_error("request");
        mode=text=="Replace"?Mode::Replace:Mode::Observe;
        const auto pack=game/L"GRID2VR/addons/cockpit-textures/0.2.0-preview.1";
        for(const auto& row:catalog)
            atlases.push_back({row.car,row.sourceHash,row.sourceHash8,row.packHash,pack/(std::string(row.car)+".dds")});
        Event(std::string("{\"event\":\"configured\",\"capability\":\"")+capability+"\",\"mode\":\""+text+"\"}");
    } catch(...) {mode=Mode::Off;atlases.clear();Event("{\"event\":\"refused\",\"reason\":\"request_validation\",\"stockFallback\":true}");}
    Summary();
}
void FocusCockpit::HookFailed() noexcept {
    mode=Mode::Off;Event("{\"event\":\"refused\",\"reason\":\"hook_installation\",\"stockFallback\":true}");Summary();
}
void FocusCockpit::Event(const std::string& text) noexcept {
    if(folder.empty() || events.fetch_add(1)>=256)return;
    try {std::lock_guard lock(logMutex);std::ofstream out(folder/L"cockpit-textures.jsonl",std::ios::app);out<<text<<'\n';}catch(...) {}
}
void FocusCockpit::Summary() noexcept {
    if(folder.empty())return;
    try {
        std::lock_guard lock(logMutex);std::ofstream out(folder/L"cockpit-textures-summary.json",std::ios::trunc);
        out<<"{\"schema\":1,\"mode\":\""<<(mode==Mode::Replace?"Replace":mode==Mode::Observe?"Observe":"Off")
            <<"\",\"candidates\":"<<candidates<<",\"withoutInitialData\":"<<noInitialData<<",\"matches\":"<<matches
            <<",\"textureCalls\":"<<textureCalls<<",\"viewCalls\":"<<viewCalls
            <<",\"replacementViews\":"<<replacements<<",\"fallbacks\":"<<fallbacks
            <<",\"eventLimit\":256,\"gameFilesModified\":false,\"visualAcceptance\":false}\n";
    }catch(...) {}
}
void FocusCockpit::TextureCreated(ID3D11Device* device,const D3D11_TEXTURE2D_DESC* requested,const D3D11_SUBRESOURCE_DATA* data,
    ID3D11Texture2D* texture,TextureFn create,uintptr_t callerRva) noexcept {
    if(!Enabled() || !texture || !requested)return;
    try {
        ++textureCalls;
        D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
        if(mode==Mode::Observe) {
            bool newShape=false;
            {std::lock_guard lock(shapeMutex);
                if(shapes.size()<64)newShape=shapes.insert({d.Width,d.Height,d.MipLevels,d.ArraySize,unsigned(d.Format),
                    unsigned(d.Usage),d.BindFlags,d.CPUAccessFlags,d.MiscFlags,data?1u:0u}).second;
            }
            const bool atlasShape=d.Width==512 && d.Height==512 && d.ArraySize==1 && Bc1(d.Format);
            if(newShape || (atlasShape && atlasObservations.fetch_add(1)<128)) {
                std::ostringstream detail;
                detail<<"{\"event\":\"texture_shape\",\"callerRva\":"<<callerRva<<",\"width\":"<<d.Width<<",\"height\":"<<d.Height
                    <<",\"mips\":"<<d.MipLevels<<",\"array\":"<<d.ArraySize<<",\"format\":"<<unsigned(d.Format)<<",\"usage\":"<<unsigned(d.Usage)
                    <<",\"bind\":"<<d.BindFlags<<",\"cpu\":"<<d.CPUAccessFlags<<",\"misc\":"<<d.MiscFlags
                    <<",\"initialData\":"<<(data?"true":"false")<<",\"initialSha256\":\""<<(atlasShape?SourceHash(data,d.MipLevels):"")<<"\"}";
                Event(detail.str());Summary();
            }
        }
        if(!Candidate(d))return;
        ++candidates;if(!data)++noInitialData;
        const auto hash=SourceHash(data,d.MipLevels);
        const auto found=std::find_if(atlases.begin(),atlases.end(),[&](const Atlas& atlas){return hash==(d.MipLevels==8?atlas.sourceHash8:atlas.sourceHash);});
        const bool matched=found!=atlases.end();
        std::ostringstream event;
        event<<"{\"event\":\"candidate\",\"callerRva\":"<<callerRva<<",\"format\":"<<unsigned(d.Format)
            <<",\"mips\":"<<d.MipLevels<<",\"usage\":"<<unsigned(d.Usage)<<",\"initialData\":"<<(data?"true":"false")
            <<",\"sha256\":\""<<hash<<"\",\"matched\":"<<(matched?"true":"false")
            <<",\"car\":\""<<(matched?found->car:"")<<"\"}";Event(event.str());
        if(!matched)return;
        ++matches;const UINT tag=d.MipLevels;
        if(FAILED(texture->SetPrivateData(observedId,sizeof(tag),&tag))) {++fallbacks;Summary();return;}
        if(mode==Mode::Replace) {
            // Serialize transient reads, and release CPU artwork immediately after
            // upload. Loading the whole pack would waste >200 MiB in this x86 game.
            std::lock_guard lock(packMutex);
            if(refusedPacks.count(found->car)) {++fallbacks;Summary();return;}
            std::vector<unsigned char> pixels;
            try {pixels=Load(found->path,found->packHash);}
            catch(...) {
                refusedPacks.insert(found->car);++fallbacks;
                Event("{\"event\":\"stock_fallback\",\"reason\":\"pack_validation\",\"car\":\""+found->car+"\"}");Summary();return;
            }
            std::array<D3D11_SUBRESOURCE_DATA,12> mips{};size_t offset{};
            for(UINT mip=0;mip<12;++mip) {
                const UINT size=std::max(1u,2048u>>mip),pitch=Pitch(size),bytes=pitch*Rows(size);
                mips[mip]={pixels.data()+offset,pitch,bytes};offset+=bytes;
            }
            d.Width=d.Height=2048;d.MipLevels=12;d.Usage=D3D11_USAGE_IMMUTABLE;
            ComPtr<ID3D11Texture2D> replacement;
            if(!create || FAILED(create(device,&d,mips.data(),&replacement)) || !replacement ||
                FAILED(texture->SetPrivateDataInterface(replacementId,replacement.Get()))) {
                ++fallbacks;Event("{\"event\":\"stock_fallback\",\"reason\":\"replacement_texture_allocation\"}");
            }
        }
        Summary();
    }catch(...) {++fallbacks;Event("{\"event\":\"stock_fallback\",\"reason\":\"texture_exception\"}");}
}
HRESULT FocusCockpit::CreateView(ID3D11Device* device,ID3D11Resource* resource,const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
    ID3D11ShaderResourceView** view,ViewFn create) noexcept {
    // Let the driver validate the original call first, including null-output
    // validation calls. The original view survives every refusal/allocation error.
    const auto result=create(device,resource,desc,view);
    if(!Enabled() || FAILED(result) || !resource || !view || !*view)return result;
    ++viewCalls;
    try {
        UINT tag{},size=sizeof(tag);
        if(FAILED(resource->GetPrivateData(observedId,&size,&tag)) || (tag!=8 && tag!=10))return result;
        D3D11_SHADER_RESOURCE_VIEW_DESC actual{};(*view)->GetDesc(&actual);
        const bool full=actual.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D &&
            actual.Texture2D.MostDetailedMip==0 && actual.Texture2D.MipLevels==tag &&
            (actual.Format==DXGI_FORMAT_BC1_UNORM || actual.Format==DXGI_FORMAT_BC1_UNORM_SRGB);
        bool replaced=false;
        if(mode==Mode::Replace && full) {
            ComPtr<ID3D11Texture2D> replacement;size=sizeof(ID3D11Texture2D*);
            if(SUCCEEDED(resource->GetPrivateData(replacementId,&size,replacement.GetAddressOf())) && replacement) {
                actual.Texture2D.MipLevels=12;ComPtr<ID3D11ShaderResourceView> candidate;
                if(SUCCEEDED(create(device,replacement.Get(),&actual,&candidate)) && candidate) {
                    (*view)->Release();*view=candidate.Detach();++replacements;replaced=true;
                }
            }
        }
        if(mode==Mode::Replace && !replaced)++fallbacks;
        Event(std::string("{\"event\":\"matched_view\",\"fullChain\":")+(full?"true":"false")+
            ",\"replaced\":"+(replaced?"true":"false")+"}");Summary();
    }catch(...) {++fallbacks;}
    return result;
}
#ifdef GRID2VR_FOCUS_COCKPIT_TEST
void FocusCockpit::ConfigureTest(const std::filesystem::path& run,const std::filesystem::path& dds,const std::string& source,const std::string& source8,
    const std::string& packHash,bool replace) {
    folder=run;AddTestAtlas(dds,source,source8,packHash);mode=replace?Mode::Replace:Mode::Observe;
}
void FocusCockpit::AddTestAtlas(const std::filesystem::path& dds,const std::string& source,const std::string& source8,const std::string& packHash) {
    atlases.push_back({"fixture"+std::to_string(atlases.size()),source,source8,packHash,dds});
}
#endif
}
