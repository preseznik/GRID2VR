#include "focus_cockpit.h"
#include "texture_digest.h"
#include <wrl/client.h>
#include <bcrypt.h>
#include <array>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace grid2vr;
using Microsoft::WRL::ComPtr;
#define CHECK(x) do { if(!(x)) {std::cerr<<"Failed line "<<__LINE__<<": "<<#x<<'\n';return 1;} } while(false)
namespace {
bool failTexture{},failView{};
HRESULT STDMETHODCALLTYPE Texture(ID3D11Device* d,const D3D11_TEXTURE2D_DESC* desc,const D3D11_SUBRESOURCE_DATA* data,ID3D11Texture2D** texture) {
    if(failTexture)return E_OUTOFMEMORY;return d->CreateTexture2D(desc,data,texture);
}
HRESULT STDMETHODCALLTYPE View(ID3D11Device* d,ID3D11Resource* resource,const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,ID3D11ShaderResourceView** view) {
    ComPtr<ID3D11Texture2D> texture;D3D11_TEXTURE2D_DESC td{};
    if(resource && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture))))texture->GetDesc(&td);
    if(failView && td.Width==2048)return E_OUTOFMEMORY;
    return d->CreateShaderResourceView(resource,desc,view);
}
std::string Sha(const std::vector<unsigned char>& bytes) {
    BCRYPT_ALG_HANDLE alg{};BCRYPT_HASH_HANDLE hash{};unsigned char result[32]{};
    BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0);
    BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0);
    BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0);
    BCryptFinishHash(hash,result,32,0);BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(alg,0);
    std::string out;for(auto b:result){out+="0123456789abcdef"[b>>4];out+="0123456789abcdef"[b&15];}return out;
}
std::vector<unsigned char> Read(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
void Write(const std::filesystem::path& path,const std::vector<unsigned char>& data) {
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(data.data()),data.size());
}
UINT Width(ID3D11ShaderResourceView* view) {
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;
    resource.As(&texture);D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);return d.Width;
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2 && argc!=4 && argc!=5)return 2;
    const bool expectStock=argc==5 && std::wstring(argv[4])==L"--expect-stock";
    if(argc==5 && !expectStock)return 2;
    const auto folder=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(folder);
    // Synthetic data in normal CTest; the optional local invocation additionally
    // exercises the extracted stock atlas and real generated pack, never shipped.
    std::vector<unsigned char> source(174776),dds(2796344);
    for(size_t i=0;i<source.size();++i)source[i]=static_cast<unsigned char>(i*37+11);
    for(size_t i=128;i<dds.size();++i)dds[i]=static_cast<unsigned char>(i*19+7);
    std::array<uint32_t,32> h{};h[0]=0x20534444;h[1]=124;h[2]=0xa1007;h[3]=h[4]=2048;
    h[5]=2097152;h[7]=12;h[19]=32;h[20]=4;h[21]=0x31545844;h[27]=0x401008;
    memcpy(dds.data(),h.data(),128);
    if(argc>=4) {auto stock=Read(argv[2]);CHECK(stock.size()==174904);source.assign(stock.begin()+128,stock.end());dds=Read(argv[3]);}
    const auto pack=folder/L"fixture.dds";Write(pack,dds);
    const auto sourceSha=Sha(source),sourceSha8=Sha(std::vector<unsigned char>(source.begin(),source.end()-16)),packSha=Sha(dds);
    // Real D3D11 WARP, no game, GPU or headset assumptions.
    const auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);CHECK(module);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));CHECK(create);
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    CHECK(SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
    D3D11_TEXTURE2D_DESC td{512,512,10,1,DXGI_FORMAT_BC1_TYPELESS,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};
    // Deliberately padded rows. Padding must not participate in identity.
    std::array<std::vector<unsigned char>,10> padded;std::array<D3D11_SUBRESOURCE_DATA,10> mips{};size_t offset{};
    for(UINT mip=0;mip<10;++mip) {
        const UINT dim=std::max(1u,512u>>mip),rows=std::max(1u,(dim+3)/4),pitch=rows*8;
        padded[mip].resize((pitch+16)*rows,0xcd);
        for(UINT row=0;row<rows;++row) {memcpy(padded[mip].data()+row*(pitch+16),source.data()+offset,pitch);offset+=pitch;}
        mips[mip]={padded[mip].data(),pitch+16,static_cast<UINT>(padded[mip].size())};
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};vd.Format=DXGI_FORMAT_BC1_UNORM;vd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;vd.Texture2D={0,10};
    FocusCockpit loader;loader.ConfigureTest(folder,pack,sourceSha,sourceSha8,packSha,true);
    ComPtr<ID3D11Texture2D> original;CHECK(SUCCEEDED(Texture(device.Get(),&td,mips.data(),&original)));
    uint64_t budget=1024*1024;const auto before=DigestTexture(context.Get(),original.Get(),budget);
    loader.TextureCreated(device.Get(),&td,mips.data(),original.Get(),Texture,123);
    ComPtr<ID3D11ShaderResourceView> view;
    CHECK(SUCCEEDED(loader.CreateView(device.Get(),original.Get(),&vd,&view,View)));CHECK(Width(view.Get())==2048);
    D3D11_SHADER_RESOURCE_VIEW_DESC actual{};view->GetDesc(&actual);CHECK(actual.Texture2D.MipLevels==12 && actual.Format==vd.Format);
    ComPtr<ID3D11Resource> uploaded;view->GetResource(&uploaded);budget=4*1024*1024;
    const auto replacementDigest=DigestTexture(context.Get(),uploaded.Get(),budget);CHECK(std::string(replacementDigest.status)=="ok" && replacementDigest.subresources.size()==12);
    size_t candidateOffset=128;
    for(const auto& mip:replacementDigest.subresources) {
        const size_t bytes=static_cast<size_t>(mip.rowBytes)*mip.rows;
        CHECK(mip.sha256==Sha(std::vector<unsigned char>(dds.begin()+candidateOffset,dds.begin()+candidateOffset+bytes)));
        candidateOffset+=bytes;
    }
    CHECK(candidateOffset==dds.size());uploaded.Reset();
    D3D11_TEXTURE2D_DESC originalDesc{};original->GetDesc(&originalDesc);CHECK(originalDesc.Width==512 && originalDesc.MipLevels==10);
    budget=1024*1024;const auto after=DigestTexture(context.Get(),original.Get(),budget);
    std::ostringstream a,b;WriteTextureDigest(a,before);WriteTextureDigest(b,after);CHECK(a.str()==b.str());
    // SRGB views remain SRGB. Unsupported mip subsets stay on the stock resource.
    vd.Format=DXGI_FORMAT_BC1_UNORM_SRGB;
    CHECK(SUCCEEDED(loader.CreateView(device.Get(),original.Get(),&vd,view.ReleaseAndGetAddressOf(),View)));
    view->GetDesc(&actual);CHECK(Width(view.Get())==2048 && actual.Format==DXGI_FORMAT_BC1_UNORM_SRGB);
    vd.Texture2D={1,9};CHECK(SUCCEEDED(loader.CreateView(device.Get(),original.Get(),&vd,view.ReleaseAndGetAddressOf(),View)));CHECK(Width(view.Get())==512);
    vd.Texture2D={0,8};CHECK(SUCCEEDED(loader.CreateView(device.Get(),original.Get(),&vd,view.ReleaseAndGetAddressOf(),View)));CHECK(Width(view.Get())==512);
    vd.Texture2D={0,10};failView=true;
    CHECK(SUCCEEDED(loader.CreateView(device.Get(),original.Get(),&vd,view.ReleaseAndGetAddressOf(),View)));CHECK(Width(view.Get())==512);failView=false;
    CHECK(loader.CreateView(device.Get(),original.Get(),&vd,nullptr,View)==S_FALSE);
    vd.Texture2D={11,1};CHECK(FAILED(loader.CreateView(device.Get(),original.Get(),&vd,view.ReleaseAndGetAddressOf(),View)));vd.Texture2D={0,10};
    CHECK(SUCCEEDED(loader.CreateView(device.Get(),original.Get(),&vd,view.ReleaseAndGetAddressOf(),View)));
    original.Reset();CHECK(Width(view.Get())==2048); // View retains its own resource after the game texture dies.
    for(int scenario=0;scenario<6;++scenario) {
        FocusCockpit candidate;
        if(scenario!=0)candidate.ConfigureTest(folder,pack,sourceSha,sourceSha8,packSha,scenario!=1);
        CHECK(SUCCEEDED(Texture(device.Get(),&td,mips.data(),original.ReleaseAndGetAddressOf())));
        if(scenario==2)padded[9][0]^=1; // One byte in the smallest mip must refuse.
        if(scenario==3)failTexture=true;
        if(scenario==5)candidate.HookFailed();
        candidate.TextureCreated(device.Get(),&td,scenario==4?nullptr:mips.data(),original.Get(),Texture,0);
        if(scenario==2)padded[9][0]^=1;
        failTexture=false;
        CHECK(SUCCEEDED(candidate.CreateView(device.Get(),original.Get(),&vd,view.ReleaseAndGetAddressOf(),View)));
        CHECK(Width(view.Get())==512);
    }
    // Null view description on a typed resource and UINT_MAX full-mip requests.
    td.Format=DXGI_FORMAT_BC1_UNORM;
    CHECK(SUCCEEDED(Texture(device.Get(),&td,mips.data(),original.ReleaseAndGetAddressOf())));
    loader.TextureCreated(device.Get(),&td,mips.data(),original.Get(),Texture,0);
    CHECK(SUCCEEDED(loader.CreateView(device.Get(),original.Get(),nullptr,view.ReleaseAndGetAddressOf(),View)));CHECK(Width(view.Get())==2048);
    vd.Format=DXGI_FORMAT_BC1_UNORM;vd.Texture2D={0,UINT_MAX};
    CHECK(SUCCEEDED(loader.CreateView(device.Get(),original.Get(),&vd,view.ReleaseAndGetAddressOf(),View)));CHECK(Width(view.Get())==2048);
    // Regression: the real game uploads eight immutable mips, stopping at 4x4.
    // Match all eight, allow only a full view, and keep the source texture intact.
    td.MipLevels=8;td.Usage=D3D11_USAGE_IMMUTABLE;
    for(auto format:{DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_BC1_UNORM_SRGB,DXGI_FORMAT_BC1_TYPELESS}) {
        td.Format=format;vd.Format=format==DXGI_FORMAT_BC1_TYPELESS?DXGI_FORMAT_BC1_UNORM:format;
        CHECK(SUCCEEDED(Texture(device.Get(),&td,mips.data(),original.ReleaseAndGetAddressOf())));
        budget=1024*1024;const auto before8=DigestTexture(context.Get(),original.Get(),budget);
        loader.TextureCreated(device.Get(),&td,mips.data(),original.Get(),Texture,9740013);
        vd.Texture2D={0,8};
        CHECK(SUCCEEDED(loader.CreateView(device.Get(),original.Get(),&vd,view.ReleaseAndGetAddressOf(),View)));CHECK(Width(view.Get())==2048);
        view->GetDesc(&actual);CHECK(actual.Texture2D.MipLevels==12 && actual.Format==vd.Format);
        vd.Texture2D={0,UINT_MAX};
        CHECK(SUCCEEDED(loader.CreateView(device.Get(),original.Get(),&vd,view.ReleaseAndGetAddressOf(),View)));CHECK(Width(view.Get())==2048);
        if(format!=DXGI_FORMAT_BC1_TYPELESS) {
            CHECK(SUCCEEDED(loader.CreateView(device.Get(),original.Get(),nullptr,view.ReleaseAndGetAddressOf(),View)));CHECK(Width(view.Get())==2048);
        }
        vd.Texture2D={0,7};
        CHECK(SUCCEEDED(loader.CreateView(device.Get(),original.Get(),&vd,view.ReleaseAndGetAddressOf(),View)));CHECK(Width(view.Get())==512);
        vd.Texture2D={1,7};
        CHECK(SUCCEEDED(loader.CreateView(device.Get(),original.Get(),&vd,view.ReleaseAndGetAddressOf(),View)));CHECK(Width(view.Get())==512);
        budget=1024*1024;const auto after8=DigestTexture(context.Get(),original.Get(),budget);
        std::ostringstream c,d;WriteTextureDigest(c,before8);WriteTextureDigest(d,after8);CHECK(c.str()==d.str());
    }
    td.Format=DXGI_FORMAT_BC1_UNORM;vd.Format=DXGI_FORMAT_BC1_UNORM;vd.Texture2D={0,UINT_MAX};
    for(int scenario=0;scenario<2;++scenario) {
        td.MipLevels=scenario==0?8:9; // Unknown chain length must fail closed.
        if(scenario==0)padded[7][0]^=1;
        CHECK(SUCCEEDED(Texture(device.Get(),&td,mips.data(),original.ReleaseAndGetAddressOf())));
        loader.TextureCreated(device.Get(),&td,mips.data(),original.Get(),Texture,0);
        if(scenario==0)padded[7][0]^=1;
        CHECK(SUCCEEDED(loader.CreateView(device.Get(),original.Get(),&vd,view.ReleaseAndGetAddressOf(),View)));CHECK(Width(view.Get())==512);
    }
    td.MipLevels=8;
    // A second independent atlas must receive its own artwork. The first
    // resource/view stays alive and must not be redirected to the new car.
    const auto secondPack=folder/L"second.dds";
    auto secondSource=source,secondDds=dds;secondSource[0]^=1;secondDds[128]^=1;Write(secondPack,secondDds);
    loader.AddTestAtlas(secondPack,Sha(secondSource),Sha(std::vector<unsigned char>(secondSource.begin(),secondSource.end()-16)),Sha(secondDds));
    padded[0][0]^=1;
    CHECK(SUCCEEDED(Texture(device.Get(),&td,mips.data(),original.ReleaseAndGetAddressOf())));
    loader.TextureCreated(device.Get(),&td,mips.data(),original.Get(),Texture,0);
    padded[0][0]^=1;
    CHECK(SUCCEEDED(loader.CreateView(device.Get(),original.Get(),nullptr,view.ReleaseAndGetAddressOf(),View)));CHECK(Width(view.Get())==2048);
    view->GetResource(uploaded.ReleaseAndGetAddressOf());budget=4*1024*1024;
    const auto secondDigest=DigestTexture(context.Get(),uploaded.Get(),budget);
    CHECK(secondDigest.subresources[0].sha256==Sha(std::vector<unsigned char>(secondDds.begin()+128,secondDds.begin()+128+2097152)));
    uploaded.Reset();
    // Lazy pack validation must notice corruption after configuration.
    FocusCockpit changed;changed.ConfigureTest(folder,secondPack,sourceSha,sourceSha8,Sha(secondDds),true);
    secondDds.back()^=1;Write(secondPack,secondDds);
    CHECK(SUCCEEDED(Texture(device.Get(),&td,mips.data(),original.ReleaseAndGetAddressOf())));
    changed.TextureCreated(device.Get(),&td,mips.data(),original.Get(),Texture,0);
    CHECK(SUCCEEDED(changed.CreateView(device.Get(),original.Get(),nullptr,view.ReleaseAndGetAddressOf(),View)));CHECK(Width(view.Get())==512);
    std::filesystem::remove(secondPack);
    // Production configuration: default off, bounded malformed request, missing
    // pack and tampering all fail closed, independent of the synthetic hash seam.
    const auto run=folder/L"production",game=folder/L"game";std::filesystem::create_directories(run);
    FocusCockpit off;off.Configure(game,run);CHECK(!off.Enabled());
    {std::ofstream out(run/L"cockpit-textures.request");out<<"Observe";}
    FocusCockpit observe;observe.Configure(game,run);CHECK(observe.Enabled());
    {std::ofstream out(run/L"cockpit-textures.request");out<<"Replace";}
    FocusCockpit absent;absent.Configure(game,run);CHECK(absent.Enabled());
    CHECK(SUCCEEDED(Texture(device.Get(),&td,mips.data(),original.ReleaseAndGetAddressOf())));
    absent.TextureCreated(device.Get(),&td,mips.data(),original.Get(),Texture,0);
    CHECK(SUCCEEDED(absent.CreateView(device.Get(),original.Get(),nullptr,view.ReleaseAndGetAddressOf(),View)));CHECK(Width(view.Get())==512);
    const auto installed=game/L"GRID2VR/addons/cockpit-textures/0.2.0-preview.2"/(argc>=4?std::filesystem::path(argv[3]).filename():L"foc.dds");
    std::filesystem::create_directories(installed.parent_path());Write(installed,dds);
    if(argc>=4) {
        FocusCockpit real;real.Configure(game,run);CHECK(real.Enabled());
        CHECK(SUCCEEDED(Texture(device.Get(),&td,mips.data(),original.ReleaseAndGetAddressOf())));
        real.TextureCreated(device.Get(),&td,mips.data(),original.Get(),Texture,0);
        CHECK(SUCCEEDED(real.CreateView(device.Get(),original.Get(),nullptr,view.ReleaseAndGetAddressOf(),View)));CHECK(Width(view.Get())==(expectStock?512:2048));
    }
    dds.back()^=1;Write(installed,dds);
    FocusCockpit tampered;tampered.Configure(game,run);CHECK(tampered.Enabled());
    CHECK(SUCCEEDED(Texture(device.Get(),&td,mips.data(),original.ReleaseAndGetAddressOf())));
    tampered.TextureCreated(device.Get(),&td,mips.data(),original.Get(),Texture,0);
    CHECK(SUCCEEDED(tampered.CreateView(device.Get(),original.Get(),nullptr,view.ReleaseAndGetAddressOf(),View)));CHECK(Width(view.Get())==512);
    {std::ofstream out(run/L"cockpit-textures.request");out<<"Observe\nReplace";}
    FocusCockpit bad;bad.Configure(game,run);CHECK(!bad.Enabled());
    std::filesystem::remove(run/L"cockpit-textures.request");std::filesystem::remove(pack);std::filesystem::remove(installed);
    std::cout<<"Cockpit texture D3D11 resource, matching, format, fallback and ownership checks passed"<<(argc>=4?" with local artwork":" with synthetic fixtures")<<(expectStock?" (stock exception verified)":"")<<'\n';
    return 0;
}
