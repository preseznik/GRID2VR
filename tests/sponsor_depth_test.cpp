#include "sponsor_depth.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace grid2vr;
using namespace grid2vr::sponsordepth;
#define CHECK(x) do { if(!(x)) {std::cerr<<"Failed line "<<__LINE__<<": "<<#x<<'\n';return 1;} } while(false)

namespace {
int Model() {
    Settings s;
    CHECK(ParseSettings("",s) && s.units==16 && s.slope==2.f);
    CHECK(ParseSettings("units=8\r\nslope=1.5\n",s) && s.units==8 && s.slope==1.5f);
    CHECK(!ParseSettings("units=0",s) && !ParseSettings("units=257",s) && !ParseSettings("slope=-1",s));
    CHECK(!ParseSettings("slope=17",s) && !ParseSettings("bias=4",s) && !ParseSettings("units=4x",s) && !ParseSettings("units",s));
    D3D11_DEPTH_STENCIL_DESC d=DefaultDepthStencil();
    CHECK(BiasSign(d)==-1);
    d.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;CHECK(BiasSign(d)==-1);
    d.DepthFunc=D3D11_COMPARISON_GREATER;CHECK(BiasSign(d)==1);
    d.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;CHECK(BiasSign(d)==1);
    d.DepthFunc=D3D11_COMPARISON_ALWAYS;CHECK(BiasSign(d)==0);
    d.DepthFunc=D3D11_COMPARISON_EQUAL;CHECK(BiasSign(d)==0);
    d.DepthFunc=D3D11_COMPARISON_LESS;d.DepthEnable=FALSE;CHECK(BiasSign(d)==0);
    auto r=DefaultRasterizer();r.DepthBias=3;r.SlopeScaledDepthBias=.5f;
    const auto b=Biased(r,-1,Settings{});
    CHECK(b.DepthBias==3-16 && b.SlopeScaledDepthBias==.5f-2.f && b.CullMode==D3D11_CULL_BACK && b.DepthClipEnable);
    CHECK(Biased(r,1,Settings{4,1.f}).DepthBias==7);
    D3D11_TEXTURE2D_DESC t{};t.Width=1024;t.Height=256;t.MipLevels=10;t.ArraySize=1;t.Format=DXGI_FORMAT_BC1_UNORM;
    t.SampleDesc.Count=1;t.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    CHECK(Candidate(t));
    t.Format=DXGI_FORMAT_BC3_UNORM;CHECK(!Candidate(t));t.Format=DXGI_FORMAT_BC1_UNORM_SRGB;CHECK(Candidate(t));
    t.Width=512;CHECK(!Candidate(t));t.Width=1024;t.CPUAccessFlags=D3D11_CPU_ACCESS_READ;CHECK(!Candidate(t));
    return 0;
}

struct Gpu {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
};
ComPtr<ID3D11ShaderResourceView> Cloth(Gpu& g,SponsorDepth& depth,unsigned char seed,std::string* hash) {
    // Padded rows prove the identity excludes driver pitch padding.
    constexpr UINT pitch=mip0Pitch+64;
    std::vector<unsigned char> mip0(size_t(pitch)*mip0Rows);
    for(size_t i=0;i<mip0.size();++i)mip0[i]=static_cast<unsigned char>(i*31+seed);
    std::vector<D3D11_SUBRESOURCE_DATA> data(10);std::vector<std::vector<unsigned char>> lower(10);
    data[0]={mip0.data(),pitch,0};
    for(UINT mip=1;mip<10;++mip) {
        const UINT w=std::max(1u,1024u>>mip),h=std::max(1u,256u>>mip),p=std::max(1u,(w+3)/4)*8,rows=std::max(1u,(h+3)/4);
        lower[mip].assign(size_t(p)*rows,seed);data[mip]={lower[mip].data(),p,0};
    }
    if(hash)*hash=SponsorDepth::Mip0Hash(data.data());
    D3D11_TEXTURE2D_DESC d{};d.Width=1024;d.Height=256;d.MipLevels=10;d.ArraySize=1;d.Format=DXGI_FORMAT_BC1_UNORM;
    d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;if(FAILED(g.device->CreateTexture2D(&d,data.data(),&texture)))return {};
    depth.TextureCreated(&d,data.data(),texture.Get());
    ComPtr<ID3D11ShaderResourceView> view;if(FAILED(g.device->CreateShaderResourceView(texture.Get(),nullptr,&view)))return {};
    depth.ViewCreated(texture.Get(),view.Get());
    return view;
}
std::string Read(const std::filesystem::path& p) { std::ifstream f(p);std::stringstream s;s<<f.rdbuf();return s.str(); }

int Gpu(const std::filesystem::path& run) {
    // Real D3D11 WARP, no game, GPU or headset assumptions.
    const auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);CHECK(module);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));CHECK(create);
    struct Gpu g;
    CHECK(SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&g.device,nullptr,&g.context)));
    std::filesystem::remove_all(run);std::filesystem::create_directories(run);

    // Identity: the sponsor hash comes from a first, untracked upload of the same bytes.
    SponsorDepth probe;std::string sponsorHash;CHECK(Cloth(g,probe,7,&sponsorHash) && sponsorHash.size()==64);
    SponsorDepth depth;depth.ConfigureTest(run,{sponsorHash},Settings{});
    const auto sponsor=Cloth(g,depth,7,nullptr),other=Cloth(g,depth,8,nullptr);CHECK(sponsor && other);
    depth.Summary();
    const auto afterTags=Read(run/"sponsor-depth-bias.json");
    CHECK(afterTags.find("\"candidateTextures\":2")!=std::string::npos && afterTags.find("\"matchedTextures\":1")!=std::string::npos &&
        afterTags.find("\"taggedViews\":1")!=std::string::npos);

    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=64;td.MipLevels=td.ArraySize=1;td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> colour,depthTexture;ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11DepthStencilView> dsv;
    CHECK(SUCCEEDED(g.device->CreateTexture2D(&td,nullptr,&colour)) && SUCCEEDED(g.device->CreateRenderTargetView(colour.Get(),nullptr,&rtv)));
    td.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    CHECK(SUCCEEDED(g.device->CreateTexture2D(&td,nullptr,&depthTexture)) && SUCCEEDED(g.device->CreateDepthStencilView(depthTexture.Get(),nullptr,&dsv)));
    auto dsDesc=DefaultDepthStencil();dsDesc.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
    ComPtr<ID3D11DepthStencilState> lessEqual,greater,always;
    CHECK(SUCCEEDED(g.device->CreateDepthStencilState(&dsDesc,&lessEqual)));
    dsDesc.DepthFunc=D3D11_COMPARISON_GREATER;CHECK(SUCCEEDED(g.device->CreateDepthStencilState(&dsDesc,&greater)));
    dsDesc.DepthFunc=D3D11_COMPARISON_ALWAYS;CHECK(SUCCEEDED(g.device->CreateDepthStencilState(&dsDesc,&always)));
    auto rsDesc=DefaultRasterizer();rsDesc.DepthBias=3;rsDesc.CullMode=D3D11_CULL_NONE;
    ComPtr<ID3D11RasterizerState> native;CHECK(SUCCEEDED(g.device->CreateRasterizerState(&rsDesc,&native)));
    auto* c=g.context.Get();
    auto bound=[&]{ComPtr<ID3D11RasterizerState> s;c->RSGetState(&s);return s;};

    c->OMSetRenderTargets(1,rtv.GetAddressOf(),dsv.Get());c->OMSetDepthStencilState(lessEqual.Get(),0);c->RSSetState(native.Get());
    // A non-sponsor view in any slot leaves the draw native.
    c->PSSetShaderResources(0,1,other.GetAddressOf());
    {auto t=depth.Begin(c);CHECK(!t.active && bound().Get()==native.Get());}
    // The sponsor view in a later slot biases toward the viewer, on top of the game's own bias.
    ID3D11ShaderResourceView* slots[3]{other.Get(),nullptr,sponsor.Get()};c->PSSetShaderResources(0,3,slots);
    {
        auto t=depth.Begin(c);CHECK(t.active);
        D3D11_RASTERIZER_DESC got{};bound()->GetDesc(&got);
        CHECK(bound().Get()!=native.Get() && got.DepthBias==3-16 && got.SlopeScaledDepthBias==-2.f && got.CullMode==D3D11_CULL_NONE);
        depth.End(c,t);CHECK(bound().Get()==native.Get() && !t.active);
    }
    {auto t=depth.Begin(c);CHECK(t.active);depth.End(c,t);} // cached: no second state
    // Reversed depth adds; tests that never order by depth stay native.
    c->OMSetDepthStencilState(greater.Get(),0);
    {auto t=depth.Begin(c);CHECK(t.active);D3D11_RASTERIZER_DESC got{};bound()->GetDesc(&got);CHECK(got.DepthBias==3+16);depth.End(c,t);}
    c->OMSetDepthStencilState(always.Get(),0);
    {auto t=depth.Begin(c);CHECK(!t.active && bound().Get()==native.Get());}
    // Default (null) rasterizer state is biased from the D3D11 defaults and restored to null.
    c->OMSetDepthStencilState(lessEqual.Get(),0);c->RSSetState(nullptr);
    {auto t=depth.Begin(c);CHECK(t.active);D3D11_RASTERIZER_DESC got{};bound()->GetDesc(&got);
        CHECK(got.DepthBias==-16 && got.CullMode==D3D11_CULL_BACK);depth.End(c,t);CHECK(!bound());}
    // Depth-only (shadow) and colour-only passes stay native.
    c->RSSetState(native.Get());c->OMSetRenderTargets(0,nullptr,dsv.Get());
    {auto t=depth.Begin(c);CHECK(!t.active);}
    c->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);
    {auto t=depth.Begin(c);CHECK(!t.active);}
    depth.Summary();
    const auto summary=Read(run/"sponsor-depth-bias.json");
    CHECK(summary.find("\"biasedDraws\":4")!=std::string::npos && summary.find("\"cachedStates\":3")!=std::string::npos &&
        summary.find("\"skippedNoColourOrDepth\":2")!=std::string::npos && summary.find("\"skippedDepthTest\":1")!=std::string::npos &&
        summary.find("\"gameFilesModified\":false")!=std::string::npos);

    // Rendered proof: a banner coplanar with its backing loses LESS natively and wins biased.
    const auto compiler=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);CHECK(compiler);
    const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile"));CHECK(compile);
    const char vs[]="float4 main(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);return float4(p*2-1,0.5,1);}";
    const char ps[]="cbuffer C:register(b0){float4 colour;};Texture2D t:register(t0);float4 main():SV_Target{return colour;}";
    ComPtr<ID3DBlob> vb,pb;
    CHECK(SUCCEEDED(compile(vs,sizeof(vs)-1,nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vb,nullptr)));
    CHECK(SUCCEEDED(compile(ps,sizeof(ps)-1,nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&pb,nullptr)));
    ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;
    CHECK(SUCCEEDED(g.device->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,&vertex)));
    CHECK(SUCCEEDED(g.device->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,&pixel)));
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants;CHECK(SUCCEEDED(g.device->CreateBuffer(&bd,nullptr,&constants)));
    dsDesc=DefaultDepthStencil();ComPtr<ID3D11DepthStencilState> less;CHECK(SUCCEEDED(g.device->CreateDepthStencilState(&dsDesc,&less)));
    rsDesc=DefaultRasterizer();rsDesc.CullMode=D3D11_CULL_NONE;ComPtr<ID3D11RasterizerState> plain;CHECK(SUCCEEDED(g.device->CreateRasterizerState(&rsDesc,&plain)));
    D3D11_VIEWPORT vp{0,0,64,64,0,1};
    auto frame=[&](bool biased)->UINT {
        const float clear[]{0,0,0,0};c->ClearRenderTargetView(rtv.Get(),clear);c->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);
        std::array<ID3D11ShaderResourceView*,viewSlots> none{};c->PSSetShaderResources(0,viewSlots,none.data());
        c->OMSetRenderTargets(1,rtv.GetAddressOf(),dsv.Get());c->OMSetDepthStencilState(less.Get(),0);c->RSSetState(plain.Get());
        c->RSSetViewports(1,&vp);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->IASetInputLayout(nullptr);
        c->VSSetShader(vertex.Get(),nullptr,0);c->PSSetShader(pixel.Get(),nullptr,0);c->PSSetConstantBuffers(0,1,constants.GetAddressOf());
        const float red[]{1,0,0,1},green[]{0,1,0,1};
        c->UpdateSubresource(constants.Get(),0,nullptr,red,0,0);c->PSSetShaderResources(0,1,other.GetAddressOf());c->Draw(3,0);
        c->UpdateSubresource(constants.Get(),0,nullptr,green,0,0);
        ID3D11ShaderResourceView* banner=biased?sponsor.Get():other.Get();c->PSSetShaderResources(0,1,&banner);
        auto t=depth.Begin(c);c->Draw(3,0);depth.End(c,t);
        D3D11_TEXTURE2D_DESC sd{};colour->GetDesc(&sd);sd.Usage=D3D11_USAGE_STAGING;sd.BindFlags=0;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;if(FAILED(g.device->CreateTexture2D(&sd,nullptr,&staging)))return 0;
        c->CopyResource(staging.Get(),colour.Get());D3D11_MAPPED_SUBRESOURCE m{};if(FAILED(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)))return 0;
        const UINT pixel=*reinterpret_cast<const UINT*>(static_cast<const unsigned char*>(m.pData)+16*m.RowPitch+16*4);c->Unmap(staging.Get(),0);
        return pixel;
    };
    const UINT nativePixel=frame(false),biasedPixel=frame(true);
    std::cout<<std::hex<<"native=0x"<<nativePixel<<" biased=0x"<<biasedPixel<<std::dec<<'\n';
    CHECK(nativePixel==0xff0000ffu); // native: equal depth fails LESS, backing stays
    CHECK(biasedPixel==0xff00ff00u); // biased: the banner wins
    // Retained view identity survives the caller releasing its reference. The
    // 129th identity falls back to the original tag lookup without growing storage.
    ComPtr<ID3D11Resource> sponsorResource;sponsor->GetResource(&sponsorResource);
    std::vector<ComPtr<ID3D11ShaderResourceView>> extraViews;
    for(unsigned i=0;i<128;++i) {
        ComPtr<ID3D11ShaderResourceView> view;
        CHECK(SUCCEEDED(g.device->CreateShaderResourceView(sponsorResource.Get(),nullptr,&view)));
        depth.ViewCreated(sponsorResource.Get(),view.Get());
        c->PSSetShaderResources(0,1,view.GetAddressOf());
        auto token=depth.Begin(c);CHECK(token.active);depth.End(c,token);
        CHECK(bound().Get()==plain.Get());
        extraViews.push_back(view);
        if(i==126) {
            depth.Summary();const auto atLimit=Read(run/"sponsor-depth-bias.json");
            CHECK(atLimit.find("\"retainedViews\":128")!=std::string::npos);
            CHECK(atLimit.find("\"viewCacheOverflow\":false")!=std::string::npos);
        }
    }
    auto* retained=extraViews.front().Get();extraViews.clear();
    c->PSSetShaderResources(0,1,&retained);
    {auto token=depth.Begin(c);CHECK(token.active);depth.End(c,token);}
    c->PSSetShaderResources(0,1,other.GetAddressOf());
    {auto token=depth.Begin(c);CHECK(!token.active && bound().Get()==plain.Get());}
    depth.Summary();const auto cached=Read(run/"sponsor-depth-bias.json");
    CHECK(cached.find("\"retainedViews\":128")!=std::string::npos);
    CHECK(cached.find("\"viewCacheOverflow\":true")!=std::string::npos);
    std::cout<<"bounded view identity: lifetime, limit and overflow fallback passed\n";

    // Binding tracking on the immediate context: known non-sponsor slots skip the readback,
    // unknown slots, ClearState and context-state swaps stay correct, the audit catches a miss.
    {
        const auto trackRun=run/"tracking";std::filesystem::create_directories(trackRun);
        SponsorDepth tracker;tracker.ConfigureTest(trackRun,{sponsorHash},Settings{});
        const auto banner=Cloth(g,tracker,7,nullptr),plainView=Cloth(g,tracker,8,nullptr);CHECK(banner && plainView);
        auto targets=[&]{c->OMSetRenderTargets(1,rtv.GetAddressOf(),dsv.Get());c->OMSetDepthStencilState(lessEqual.Get(),0);c->RSSetState(native.Get());};
        auto bind=[&](UINT slot,ID3D11ShaderResourceView* v){c->PSSetShaderResources(slot,1,&v);tracker.ResourcesBound(c,slot,1,&v);};
        auto draw=[&]{auto t=tracker.Begin(c);const bool active=t.active;tracker.End(c,t);return active;};
        std::array<ID3D11ShaderResourceView*,viewSlots> none{};
        c->ClearState();targets();
        c->PSSetShaderResources(3,1,banner.GetAddressOf());   // bound before tracking: unknown, still found
        tracker.Track(c);
        CHECK(draw());
        c->PSSetShaderResources(0,viewSlots,none.data());tracker.ResourcesBound(c,0,viewSlots,none.data());
        CHECK(!draw());
        bind(2,banner.Get());CHECK(draw());
        bind(2,plainView.Get());CHECK(!draw());
        bind(5,banner.Get());c->ClearState();tracker.StateCleared(c);targets();CHECK(!draw());
        // Swap out to a fresh state and back: the banner record comes back with the game's state.
        ComPtr<ID3D11Device1> device1;ComPtr<ID3D11DeviceContext1> c1;
        CHECK(SUCCEEDED(g.device.As(&device1)) && SUCCEEDED(g.context.As(&c1)));
        const D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;ComPtr<ID3DDeviceContextState> fresh,gameState,back;
        CHECK(SUCCEEDED(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device1),nullptr,&fresh)));
        bind(1,banner.Get());
        c1->SwapDeviceContextState(fresh.Get(),&gameState);tracker.StateSwapped(c,gameState.Get(),fresh.Get());
        targets();CHECK(!draw());                              // fresh state: slots unknown, full check finds none
        c->PSSetShaderResources(0,viewSlots,none.data());tracker.ResourcesBound(c,0,viewSlots,none.data());CHECK(!draw());
        c1->SwapDeviceContextState(gameState.Get(),&back);tracker.StateSwapped(c,back.Get(),gameState.Get());
        CHECK(draw());                                         // restored: banner still in slot 1
        bind(1,plainView.Get());CHECK(!draw());
        // A binding the tracker never saw: skipped until the audit finds it, then the full check stays on.
        c->PSSetShaderResources(4,1,banner.GetAddressOf());
        unsigned skipped=0;while(!draw() && skipped<auditInterval)++skipped;
        CHECK(skipped<auditInterval);
        CHECK(draw());
        tracker.Summary();const auto report=Read(trackRun/"sponsor-depth-bias.json");
        CHECK(report.find("\"bindingTracking\":\"audit_mismatch\"")!=std::string::npos && report.find("\"auditMismatches\":1")!=std::string::npos);
        CHECK(report.find("\"trackedSkips\":0")==std::string::npos);
        std::cout<<"binding tracking: skips, unknown slots, ClearState, state swaps and audit fallback passed\n";
    }
    std::cout<<"sponsor depth bias: model, tagging, state restore, skips and rendered order passed\n";
    return 0;
}
}


int ProductionCatalog(const std::filesystem::path& run,const char* boardFile,const char* clothFile) {
    std::filesystem::create_directories(run);
    {std::ofstream request(run/"sponsor-depth-bias.request");}
    SponsorDepth depth;depth.Configure(run);CHECK(depth.Enabled());
    const auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);CHECK(module);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));CHECK(create);
    struct Gpu g;
    CHECK(SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&g.device,nullptr,&g.context)));
    for(const auto path:{boardFile,clothFile}) {
        std::ifstream input(path,std::ios::binary);CHECK(input);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),{});CHECK(bytes.size()==mip0Bytes);
        D3D11_TEXTURE2D_DESC d{};d.Width=1024;d.Height=256;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_BC1_UNORM;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{bytes.data(),mip0Pitch,0};
        ComPtr<ID3D11Texture2D> texture;CHECK(SUCCEEDED(g.device->CreateTexture2D(&d,&data,&texture)));
        depth.TextureCreated(&d,&data,texture.Get());
        ComPtr<ID3D11ShaderResourceView> view;CHECK(SUCCEEDED(g.device->CreateShaderResourceView(texture.Get(),nullptr,&view)));
        depth.ViewCreated(texture.Get(),view.Get());depth.Summary();
        const auto report=Read(run/"sponsor-depth-bias.json");
        CHECK(report.find("\"matchedTextures\":1")!=std::string::npos);
        CHECK(report.find("\"taggedViews\":1")!=std::string::npos);
        CHECK(report.find("\"identities\":95")!=std::string::npos);
    }
    std::cout<<"production catalog: real Intel board included; same-package cloth excluded\n";
    return 0;
}

// Not part of CTest: per-draw CPU cost of the check on WARP, full readback versus tracked
// bindings, with 8 non-sponsor views bound and 6 sponsor views tagged (as in a Yas Marina run).
int Bench() {
    const auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);CHECK(module);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));CHECK(create);
    struct Gpu g;
    CHECK(SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&g.device,nullptr,&g.context)));
    const auto run=std::filesystem::temp_directory_path()/"grid2vr-sponsor-bench";std::filesystem::create_directories(run);
    SponsorDepth probe;std::vector<std::string> ids;std::string hash;
    for(unsigned char seed=20;seed<26;++seed) {CHECK(Cloth(g,probe,seed,&hash));ids.push_back(hash);}
    auto measure=[&](bool tracked)->double {
        SponsorDepth depth;depth.ConfigureTest(run,ids,Settings{});
        std::vector<ComPtr<ID3D11ShaderResourceView>> banners,others;
        for(unsigned char seed=20;seed<26;++seed)banners.push_back(Cloth(g,depth,seed,nullptr));
        for(unsigned char seed=40;seed<48;++seed)others.push_back(Cloth(g,depth,seed,nullptr));
        std::array<ID3D11ShaderResourceView*,viewSlots> bound{};for(UINT i=0;i<viewSlots;++i)bound[i]=others[i].Get();
        auto* c=g.context.Get();c->PSSetShaderResources(0,viewSlots,bound.data());
        if(tracked) {depth.Track(c);depth.ResourcesBound(c,0,viewSlots,bound.data());}
        LARGE_INTEGER f{},a{},b{};QueryPerformanceFrequency(&f);double best=1e30;
        constexpr int draws=200000;
        for(int rep=0;rep<7;++rep) {
            QueryPerformanceCounter(&a);
            for(int i=0;i<draws;++i) {auto t=depth.Begin(c);depth.End(c,t);}
            QueryPerformanceCounter(&b);best=std::min(best,double(b.QuadPart-a.QuadPart)*1e9/f.QuadPart/draws);
        }
        return best;
    };
    const double full=measure(false),tracked=measure(true);
    std::cout<<"per draw (best of 7 x 200000, WARP): full readback "<<full<<" ns, tracked "<<tracked<<" ns\n";
    for(const int perFrame:{3500,7000})
        std::cout<<"  at "<<perFrame<<" checked draws/frame: "<<(full-tracked)*perFrame/1e6<<" ms saved per frame\n";
    return 0;
}

int main(int argc,char** argv) {
    if(argc==2 && std::string_view(argv[1])=="--bench")return Bench();
    if(argc!=2 && argc!=4)return 2;
    if(const int r=Model())return r;
    if(const int r=Gpu(argv[1]))return r;
    return argc==4?ProductionCatalog(std::filesystem::path(argv[1])/"catalog",argv[2],argv[3]):0;
}
