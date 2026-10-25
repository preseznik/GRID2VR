#include "flicker_watch.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using Microsoft::WRL::ComPtr;
namespace watch=grid2vr::flickerwatch;
namespace {
constexpr uint64_t FlagVs=0x5365b368abb08738ull,FlagPs=0xaa19112331ae0872ull;
constexpr uint64_t SignVs=0xd946f27a1c896b78ull,SignPs=0x938d132c1aa95cbeull,InstanceVs=0x13dd94df551eb6d9ull;
const std::array<UINT,3> flagArgs{3,0,0},signArgs{54,0,0},boardArgs{126,0,0},largeArgs{97,0,0};
std::string Read(const std::filesystem::path& folder) {
    std::ifstream file(folder/L"flicker-watch.jsonl",std::ios::binary);std::ostringstream s;s<<file.rdbuf();return s.str();
}
size_t Count(const std::string& s,const std::string& text) {
    size_t result{},position{};while((position=s.find(text,position))!=std::string::npos) {++result;position+=text.size();}return result;
}
bool WaitForFixture(ID3D11Device* device,ID3D11DeviceContext* context) {
    // Waiting belongs to the fixture only; the observer itself never flushes or spins.
    ComPtr<ID3D11Query> event;D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT,0};
    if(FAILED(device->CreateQuery(&desc,&event)))return false;
    context->End(event.Get());context->Flush();
    for(unsigned n=0;n<1000;++n) {BOOL done{};if(context->GetData(event.Get(),&done,sizeof(done),0)==S_OK && done)return true;Sleep(1);}
    return false;
}
std::string Hex(const uint8_t* bytes,size_t size) {
    constexpr char alphabet[]="0123456789abcdef";std::string value;
    for(size_t i=0;i<size;++i) {value+=alphabet[bytes[i]>>4];value+=alphabet[bytes[i]&15];}return value;
}
bool SampleObject(const std::string& report,uint64_t scene,unsigned pass,unsigned ordinal,unsigned object) {
    const auto prefix="{\"type\":\"sample\",\"scene\":"+std::to_string(scene)+",\"pass\":"+std::to_string(pass)+",\"ordinal\":"+std::to_string(ordinal)+',';
    const auto begin=report.find(prefix);if(begin==std::string::npos)return false;
    const auto row=report.substr(begin,report.find('\n',begin)-begin);
    return row.find("\"object\":"+std::to_string(object)+',')!=std::string::npos;
}
bool CheckModels(ID3D11Device* device,ID3D11DeviceContext* c,const std::filesystem::path& root) {
    const auto folder=root/L"models",sessionA=root/L"model-session-a",sessionB=root/L"model-session-b";
    for(const auto& path:{folder,sessionA,sessionB})std::filesystem::create_directories(path);
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=256;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb,unobserved;
    if(FAILED(device->CreateBuffer(&desc,nullptr,&cb)) || FAILED(device->CreateBuffer(&desc,nullptr,&unobserved)))return false;
    std::array<uint8_t,256> a{};for(unsigned i=0;i<64;++i)a[144+i]=uint8_t(i+1);
    auto b=a;b[144]=0xfe;
    auto* rawCb=cb.Get();c->VSSetConstantBuffers(0,1,&rawCb);
    auto record=[&](unsigned pass,bool flag=false,bool instanced=false) {
        const std::array<UINT,5> instance{54,1,0,0,0};
        if(instanced)watch::Draw(c,pass,6,"IndexedInstanced",instance,InstanceVs,SignPs,{5,6});
        else watch::Draw(c,pass,6,"Indexed",flag?flagArgs:signArgs,flag?FlagVs:SignVs,flag?FlagPs:SignPs,{3,4});
        watch::AfterDraw(c);
    };
    if(!watch::Start(folder))return false;
    watch::Begin(100);
    watch::Upload(cb.Get(),a.data(),UINT(a.size()));record(0); // Object 1: first placement.
    watch::Upload(cb.Get(),b.data(),UINT(b.size()));record(0); // Object 2: same bindings, second placement.
    watch::Upload(cb.Get(),a.data(),UINT(a.size()));record(0); // Reuses object 1.
    watch::Upload(cb.Get(),a.data(),200);record(0);           // Object 3: incomplete model prefix.
    watch::Upload(cb.Get(),a.data(),UINT(a.size()));record(0);
    record(1,true);                                        // Object 4: flag uses offset176, not144.
    record(2,false,true);                                  // Object 5: instances have no known model.
    ComPtr<ID3D11Buffer> vb;UINT stride{},offset{};c->IAGetVertexBuffers(0,1,&vb,&stride,&offset);
    auto* rawVb=vb.Get();c->IASetVertexBuffers(1,1,&rawVb,&stride,&offset);record(2,false,true); // Object 6.
    rawVb=nullptr;UINT zero=0;c->IASetVertexBuffers(1,1,&rawVb,&zero,&zero);
    watch::Complete(c,100,true);
    watch::Begin(101);
    record(0);                                            // Object 7: previous scene is stale.
    watch::Upload(cb.Get(),a.data(),UINT(a.size()));record(0);record(1,true); // Reuse 1 and 4.
    watch::Complete(c,101,true);
    watch::Begin(102);
    watch::Upload(cb.Get(),a.data(),192);record(0);record(1,true); // Generic3 / new flag8: short.
    watch::Complete(c,102,true);
    watch::Begin(103);
    rawCb=unobserved.Get();c->VSSetConstantBuffers(0,1,&rawCb);record(0); // Object 9.
    rawCb=nullptr;c->VSSetConstantBuffers(0,1,&rawCb);record(0);       // Object 10.
    watch::Complete(c,103,true);watch::Stop();
    const auto report=Read(folder);
    if(Count(report,"\"type\":\"object\"")!=10 || Count(report,"\"modelKnown\":true")!=3 ||
       report.find("\"modelHex\":\""+Hex(a.data()+144,64)+'"')==std::string::npos ||
       report.find("\"modelHex\":\""+Hex(b.data()+144,64)+'"')==std::string::npos ||
       report.find("\"modelHex\":\""+Hex(a.data()+176,64)+'"')==std::string::npos ||
       !SampleObject(report,100,0,1,1) || !SampleObject(report,100,0,2,2) || !SampleObject(report,100,0,3,1) ||
       !SampleObject(report,101,0,1,7) || !SampleObject(report,101,0,2,1) || !SampleObject(report,101,1,1,4) ||
       report.find("\"modelStatus\":\"stale\"")==std::string::npos || Count(report,"\"modelStatus\":\"short\"")!=2 ||
       Count(report,"\"modelStatus\":\"unsupported_shader\"")!=2) {puts(report.c_str());return false;}
    rawCb=cb.Get();c->VSSetConstantBuffers(0,1,&rawCb);
    if(!watch::Start(sessionA))return false;
    watch::Begin(1);watch::Upload(cb.Get(),a.data(),UINT(a.size()));record(0);watch::Complete(c,1,true);watch::Stop();
    if(!watch::Start(sessionB))return false;
    watch::Begin(1);record(0);watch::Complete(c,1,true);watch::Stop();
    const auto restarted=Read(sessionB);
    if(restarted.find("\"modelKnown\":false,\"modelHex\":null,\"modelStatus\":\"stale\"")==std::string::npos)return false;
    rawCb=nullptr;c->VSSetConstantBuffers(0,1,&rawCb);return true;
}
}
int main() {
    auto library=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(library,"D3D11CreateDevice"));
    auto compilerLibrary=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compilerLibrary,"D3DCompile"));
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> c;
    if(!create || !compile || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&c)))return 1;
    const char* hlsl="float4 vs(uint id:SV_VertexID):SV_Position {float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[id],0,1);} float4 ps():SV_Target{return float4(1,0,0,1);}";
    ComPtr<ID3DBlob> vsCode,psCode;ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    if(FAILED(compile(hlsl,strlen(hlsl),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vsCode,nullptr)) ||
       FAILED(compile(hlsl,strlen(hlsl),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&psCode,nullptr)) ||
       FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs)) ||
       FAILED(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps)))return 2;
    D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=4;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> target,texture;ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11ShaderResourceView> srv;
    if(FAILED(device->CreateTexture2D(&d,nullptr,&target)) || FAILED(device->CreateTexture2D(&d,nullptr,&texture)) ||
       FAILED(device->CreateRenderTargetView(target.Get(),nullptr,&rtv)) || FAILED(device->CreateShaderResourceView(texture.Get(),nullptr,&srv)))return 3;
    const UINT indices[]{0,1,2};D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(indices);bd.BindFlags=D3D11_BIND_INDEX_BUFFER|D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{indices,0,0};ComPtr<ID3D11Buffer> buffer;
    if(FAILED(device->CreateBuffer(&bd,&initial,&buffer)))return 4;
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.ScissorEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;if(FAILED(device->CreateRasterizerState(&rd,&raster)))return 5;
    auto* rawRtv=rtv.Get();c->OMSetRenderTargets(1,&rawRtv,nullptr);
    auto* rawSrv=srv.Get();c->PSSetShaderResources(0,1,&rawSrv);
    auto* rawBuffer=buffer.Get();UINT stride=4,offset=0;c->IASetVertexBuffers(0,1,&rawBuffer,&stride,&offset);
    c->IASetIndexBuffer(buffer.Get(),DXGI_FORMAT_R32_UINT,0);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vs.Get(),nullptr,0);c->PSSetShader(ps.Get(),nullptr,0);c->RSSetState(raster.Get());
    const D3D11_VIEWPORT viewport{0,0,4,4,0,1};c->RSSetViewports(1,&viewport);
    const D3D11_RECT full{0,0,4,4},empty{0,0,0,0};c->RSSetScissorRects(1,&full);
    const auto root=std::filesystem::temp_directory_path()/(L"grid2vr-watch-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    const auto evidence=root/L"evidence",bounds=root/L"bounds",rejects=root/L"rejects",contexts=root/L"contexts";
    for(const auto& path:{evidence,bounds,rejects,contexts})std::filesystem::create_directories(path);
    if(watch::Active())return 6;
    watch::Begin(99);watch::Draw(c.Get(),0,6,"Indexed",flagArgs,FlagVs,FlagPs,{1,2});watch::AfterDraw(c.Get());watch::Complete(c.Get(),99,true);
    if(std::filesystem::exists(root/L"flicker-watch.jsonl") || !watch::Start(evidence) || watch::Start(evidence))return 7;
    watch::Begin(1);
    watch::Draw(c.Get(),0,6,"Indexed",flagArgs,FlagVs,FlagPs,{1,2});c->DrawIndexed(3,0,0);watch::AfterDraw(c.Get());
    watch::Draw(c.Get(),0,6,"Indexed",boardArgs,SignVs,SignPs,{7,8});c->DrawIndexed(3,0,0);watch::AfterDraw(c.Get());
    c->RSSetScissorRects(1,&empty);
    watch::Draw(c.Get(),1,6,"Indexed",signArgs,SignVs,SignPs,{3,4});c->DrawIndexed(3,0,0);watch::AfterDraw(c.Get());
    c->RSSetScissorRects(1,&full);
    watch::Draw(c.Get(),2,6,"Indexed",largeArgs,SignVs,SignPs,{3,4});watch::AfterDraw(c.Get());
    watch::Draw(c.Get(),2,6,"Indexed",signArgs,SignVs,FlagPs,{3,4});watch::AfterDraw(c.Get());
    watch::Draw(c.Get(),2,6,"Draw",signArgs,SignVs,SignPs,{3,4});watch::AfterDraw(c.Get());
    watch::Draw(c.Get(),2,6,"Indexed",flagArgs,SignVs,SignPs,{3,4});watch::AfterDraw(c.Get());
    watch::Draw(c.Get(),2,5,"Indexed",signArgs,SignVs,SignPs,{3,4});watch::AfterDraw(c.Get());
    const std::array<UINT,5> instanceArgs{54,1,0,0,0};
    watch::Draw(c.Get(),2,6,"IndexedInstanced",instanceArgs,InstanceVs,SignPs,{5,6});c->DrawIndexed(3,0,0);watch::AfterDraw(c.Get());
    if(!WaitForFixture(device.Get(),c.Get()))return 8;
    watch::Complete(c.Get(),1,true);
    if(Read(evidence).find("\"type\":\"sample\"")!=std::string::npos)return 9;
    watch::Begin(2);watch::Complete(c.Get(),2,false);
    if(Read(evidence).find("\"type\":\"sample\"")!=std::string::npos)return 10;
    watch::Begin(3);watch::Complete(c.Get(),3,true);watch::Stop();
    const auto report=Read(evidence);
    if(Count(report,"\"type\":\"object\"")!=4 || Count(report,"\"type\":\"sample\"")!=4 ||
       Count(report,"\"samples\":16")!=3 || Count(report,"\"samples\":0")!=1 ||
       report.find("\"scene\":2,\"accepted\":false,\"candidates\":[0,0,0]")==std::string::npos ||
       report.find("\"candidates\":[2,1,1],\"draws\":[2,1,6]")==std::string::npos) {puts(report.c_str());return 11;}
    ComPtr<ID3D11RenderTargetView> actualRtv;ComPtr<ID3D11ShaderResourceView> actualSrv;ComPtr<ID3D11Buffer> actualVb,actualIb;
    ComPtr<ID3D11VertexShader> actualVs;ComPtr<ID3D11PixelShader> actualPs;ComPtr<ID3D11RasterizerState> actualRaster;
    UINT actualStride{},actualOffset{},actualIndexOffset{};DXGI_FORMAT actualFormat{};D3D11_VIEWPORT actualViewport{};UINT viewportCount=1;
    c->OMGetRenderTargets(1,&actualRtv,nullptr);c->PSGetShaderResources(0,1,&actualSrv);c->IAGetVertexBuffers(0,1,&actualVb,&actualStride,&actualOffset);
    c->IAGetIndexBuffer(&actualIb,&actualFormat,&actualIndexOffset);c->VSGetShader(&actualVs,nullptr,nullptr);c->PSGetShader(&actualPs,nullptr,nullptr);
    c->RSGetState(&actualRaster);c->RSGetViewports(&viewportCount,&actualViewport);
    if(actualRtv!=rtv || actualSrv!=srv || actualVb!=buffer || actualIb!=buffer || actualVs!=vs || actualPs!=ps || actualRaster!=raster ||
       actualStride!=4 || actualOffset || actualIndexOffset || actualFormat!=DXGI_FORMAT_R32_UINT ||
       viewportCount!=1 || memcmp(&actualViewport,&viewport,sizeof(viewport)))return 12;
    if(!watch::Start(bounds))return 13;
    for(unsigned scene=1;scene<=180;++scene) {
        watch::Begin(scene);
        if(scene<=3)for(unsigned pass=0;pass<3;++pass) {
            for(unsigned draw=0;draw<26;++draw) {
                watch::Draw(c.Get(),pass,6,"Indexed",signArgs,SignVs,SignPs,{3,4});c->DrawIndexed(3,0,0);watch::AfterDraw(c.Get());
            }
            for(unsigned draw=0;draw<2;++draw) {
                watch::Draw(c.Get(),pass,6,"Indexed",flagArgs,FlagVs,FlagPs,{1,2});c->DrawIndexed(3,0,0);watch::AfterDraw(c.Get());
            }
        }
        watch::Complete(c.Get(),scene,true);
    }
    if(watch::Active())return 14;
    const auto bounded=Read(bounds);
    if(Count(bounded,"\"type\":\"sample\"")!=216 || Count(bounded,"\"type\":\"scene\"")!=180 ||
       Count(bounded,"\"truncated\":[true,true,true]")!=3 || Count(bounded,"\"type\":\"object\"")!=2 ||
       bounded.find("\"accepted\":180,\"reason\":\"complete\"")==std::string::npos || bounded.size()>16*1024*1024)return 15;
    if(!watch::Start(rejects))return 16;
    for(unsigned scene=1;scene<=240;++scene) {watch::Begin(scene);watch::Complete(c.Get(),scene,false);}
    if(watch::Active() || Read(rejects).find("\"scenes\":240,\"accepted\":0,\"reason\":\"scene_budget\"")==std::string::npos)return 17;
    if(!watch::Start(contexts))return 18;
    ComPtr<ID3D11DeviceContext> deferred;if(FAILED(device->CreateDeferredContext(0,&deferred)))return 19;
    watch::Begin(1);watch::Draw(deferred.Get(),0,6,"Indexed",flagArgs,FlagVs,FlagPs,{1,2});watch::AfterDraw(deferred.Get());watch::Complete(c.Get(),1,true);
    watch::Begin(2);watch::Complete(c.Get(),2,true);watch::Begin(3);watch::Complete(c.Get(),3,true);
    watch::Begin(4);watch::Draw(c.Get(),0,6,"Indexed",flagArgs,FlagVs,FlagPs,{1,2});watch::Complete(c.Get(),4,true);
    watch::Begin(5);watch::Complete(c.Get(),5,true);watch::Begin(6);watch::Complete(c.Get(),6,true);watch::Stop();
    const auto invalid=Read(contexts);
    if(invalid.find("\"status\":\"context\"")==std::string::npos || invalid.find("\"status\":\"missing_after_draw\"")==std::string::npos ||
       invalid.find("\"samples\":")!=std::string::npos)return 20;
    if(!CheckModels(device.Get(),c.Get(),root))return 22;
    // Delete only this fixture's PID-specific temporary directory.
    const auto resolved=std::filesystem::weakly_canonical(root);
    if(resolved.parent_path()!=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()) ||
       resolved.filename()!=L"grid2vr-watch-"+std::to_wstring(GetCurrentProcessId()))return 21;
    std::filesystem::remove_all(resolved);
    puts("Temporal witness selects exact colour draws, distinguishes real zero samples, preserves bindings, delays reads and bounds scenes/queries/logs.");
}
