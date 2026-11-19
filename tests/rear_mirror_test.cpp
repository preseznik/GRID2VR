// Rear-view mirror cube (ProJict 18da44dc), on WARP:
// - every texel direction d of the substitute cube carries the game cube at
//   w=(d.x,d.y,-d.z), the direction car_basic actually reflects (un-flipped);
// - where the rear camera sees w, the rear image is shown instead;
// - the substitute is bound only in place of a cube view and restored exactly;
// - it is built once per scene id and refuses non-cube inputs without change.
#include "rear_mirror.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <d3dcompiler.h>
using namespace grid2vr;
using Microsoft::WRL::ComPtr;
#define CHECK(value) do {if(!(value)){std::printf("rear mirror failure line %d: %s\n",__LINE__,#value);std::exit(1);}}while(false)
void STDMETHODCALLTYPE DispatchNative(ID3D11DeviceContext* c,UINT x,UINT y,UINT z) {c->Dispatch(x,y,z);}
namespace {
ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
// R10G10B10A2 with each face a distinct, exactly representable colour.
constexpr std::array<std::array<uint32_t,3>,6> faceColours{{{1000,0,0},{0,1000,0},{0,0,1000},{500,500,0},{0,500,500},{500,0,500}}};
uint32_t Pack(uint32_t r,uint32_t g,uint32_t b) { return r|(g<<10)|(b<<20)|(3u<<30); }
ComPtr<ID3D11ShaderResourceView> GameCube() {
    constexpr UINT size=16;std::array<std::vector<uint32_t>,6> faces;std::array<D3D11_SUBRESOURCE_DATA,6> data{};
    for (UINT f=0;f<6;++f) {
        faces[f].assign(size*size,Pack(faceColours[f][0],faceColours[f][1],faceColours[f][2]));
        data[f]={faces[f].data(),size*4,0};
    }
    D3D11_TEXTURE2D_DESC desc{size,size,1,6,DXGI_FORMAT_R10G10B10A2_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,D3D11_RESOURCE_MISC_TEXTURECUBE};
    ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> view;
    CHECK(SUCCEEDED(device->CreateTexture2D(&desc,data.data(),&texture)));
    CHECK(SUCCEEDED(device->CreateShaderResourceView(texture.Get(),nullptr,&view)));
    return view;
}
ComPtr<ID3D11Texture2D> RearImage(uint32_t packed) {
    constexpr UINT width=64,height=32;std::vector<uint32_t> pixels(width*height,packed);
    D3D11_TEXTURE2D_DESC desc{width,height,1,1,DXGI_FORMAT_R10G10B10A2_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET};
    const D3D11_SUBRESOURCE_DATA data{pixels.data(),width*4,0};ComPtr<ID3D11Texture2D> texture;
    CHECK(SUCCEEDED(device->CreateTexture2D(&desc,&data,&texture)));
    return texture;
}
// Reads the substitute cube's centre texel of each face.
std::array<std::array<uint32_t,3>,6> Centres(ID3D11ShaderResourceView* view) {
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);ComPtr<ID3D11Texture2D> cube;CHECK(SUCCEEDED(resource.As(&cube)));
    D3D11_TEXTURE2D_DESC desc{};cube->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    ComPtr<ID3D11Texture2D> staging;CHECK(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)));
    context->CopyResource(staging.Get(),cube.Get());
    std::array<std::array<uint32_t,3>,6> result{};
    for (UINT f=0;f<6;++f) {
        D3D11_MAPPED_SUBRESOURCE mapped{};CHECK(SUCCEEDED(context->Map(staging.Get(),f,D3D11_MAP_READ,0,&mapped)));
        const auto* row=reinterpret_cast<const uint32_t*>(static_cast<const char*>(mapped.pData)+mapped.RowPitch*(desc.Height/2));
        const uint32_t v=row[desc.Width/2];result[f]={v&1023,(v>>10)&1023,(v>>20)&1023};
        context->Unmap(staging.Get(),f);
    }
    return result;
}
bool Near(const std::array<uint32_t,3>& a,const std::array<uint32_t,3>& b) {
    for (int i=0;i<3;++i) if (std::abs(int(a[i])-int(b[i]))>4) return false;
    return true;
}
ComPtr<ID3D11ShaderResourceView> Bound(UINT slot) { ComPtr<ID3D11ShaderResourceView> view;context->PSGetShaderResources(slot,1,&view);return view; }
}
int main() {
    const auto dll=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(dll,"D3D11CreateDevice"));
    if (!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))) return 1;
    auto& mirror=Mirror();
    const auto game=GameCube();
    constexpr UINT slot=4;

    // A plain texture in the slot is never replaced.
    {
        auto flat=RearImage(Pack(1,2,3));ComPtr<ID3D11ShaderResourceView> view;
        CHECK(SUCCEEDED(device->CreateShaderResourceView(flat.Get(),nullptr,&view)));
        auto* raw=view.Get();context->PSSetShaderResources(slot,1,&raw);
    mirror.SetSubstituteCube(true); // cube substitution is opt-in; the cube tests cover it
        CHECK(mirror.Apply(context.Get(),1,slot,DispatchNative)==RearMirror::Result::Skipped && Bound(slot)==view && !mirror.Pending());
    }
    // No rear image yet: the substitute is the game cube, un-flipped. The face
    // for direction +Z holds the game's -Z face and vice versa; X/Y keep theirs.
    auto* raw=game.Get();context->PSSetShaderResources(slot,1,&raw);
    CHECK(mirror.Apply(context.Get(),1,slot,DispatchNative)==RearMirror::Result::Applied && mirror.Pending());
    const auto substitute=Bound(slot);CHECK(substitute && substitute!=game);
    CHECK(mirror.Restore(context.Get()) && Bound(slot)==game && !mirror.Pending());
    auto centres=Centres(substitute.Get());
    CHECK(Near(centres[0],faceColours[0]) && Near(centres[1],faceColours[1]));
    CHECK(Near(centres[2],faceColours[2]) && Near(centres[3],faceColours[3]));
    CHECK(Near(centres[4],faceColours[5]) && Near(centres[5],faceColours[4]));
    CHECK(mirror.Builds()==1);
    // Same scene: no rebuild.
    CHECK(mirror.Apply(context.Get(),1,slot,DispatchNative)==RearMirror::Result::Applied && mirror.Builds()==1);
    CHECK(mirror.Restore(context.Get()) && Bound(slot)==game);

    // Rear camera looking down world -Z (clip x=w.x, y=w.y, w=-w.z). The glass
    // reflecting along -Z samples the substitute at +Z: that face shows the rear.
    const uint32_t rearColour=Pack(200,600,800);
    // The game's per-frame constants at PS b1: envMapEncodingParams (offset 16)
    // decodes x/(1-x), so the rear's linearised colour is encoded as y/(1+y).
    {
        const float frame[8]{0,0,0,0, 0,1,0,-1};
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=32;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA data{frame,0,0};ComPtr<ID3D11Buffer> buffer;
        CHECK(SUCCEEDED(device->CreateBuffer(&desc,&data,&buffer)));
        auto* rawBuffer=buffer.Get();context->PSSetConstantBuffers(1,1,&rawBuffer);
    }
    std::array<uint32_t,3> expected{};
    for (int i=0;i<3;++i) {
        const double linear=std::pow(std::array<double,3>{200,600,800}[i]/1023.,2.2);
        expected[i]=uint32_t(std::lround(linear/(1+linear)*1023));
    }
    CHECK(mirror.CaptureRear(context.Get(),2,RearImage(rearColour).Get()) && mirror.RearCaptured(2));
    mirror.SetRearCamera({1,0,0,0, 0,1,0,0, 0,0,0,-1, 0,0,0,1});
    CHECK(mirror.Apply(context.Get(),2,slot,DispatchNative)==RearMirror::Result::Applied && mirror.Builds()==2);
    CHECK(mirror.Restore(context.Get()) && Bound(slot)==game);
    centres=Centres(substitute.Get());
    CHECK(Near(centres[4],expected));
    // Directions the rear camera does not see keep the un-flipped game cube.
    CHECK(Near(centres[5],faceColours[4]) && Near(centres[0],faceColours[0]) && Near(centres[2],faceColours[2]));

    // A substitute already bound is not substituted again.
    raw=substitute.Get();context->PSSetShaderResources(slot,1,&raw);
    CHECK(mirror.Apply(context.Get(),3,slot,DispatchNative)==RearMirror::Result::Skipped && !mirror.Pending());
    raw=game.Get();context->PSSetShaderResources(slot,1,&raw);

    // Mirror overlay: a surface where the centre mirror sits (seat at the origin,
    // right +X, up +Y, forward +Z), drawn first by a stand-in "game" shader,
    // then again by the overlay. Shiny Metal constants: the rear image shows;
    // any other material: the game's pixels stay; game bindings are restored.
    constexpr char vertexCode[]=
        "struct O{float4 p:SV_Position;float4 c0:COLOR0;float4 c1:COLOR1;float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t4:TEXCOORD4;float4 t5:TEXCOORD5;};"
        "O main(uint i:SV_VertexID){O o=(O)0;float2 uv=float2((i<<1)&2,i&2);o.p=float4(uv*float2(2,-2)+float2(-1,1),.5,1);"
        "o.t0=float4(.3+o.p.x*.1,.04+o.p.y*.03,.31,1);return o;}"; // centre mirror (SL)
    constexpr char wingCode[]=
        "struct O{float4 p:SV_Position;float4 c0:COLOR0;float4 c1:COLOR1;float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t4:TEXCOORD4;float4 t5:TEXCOORD5;};"
        "O main(uint i:SV_VertexID){O o=(O)0;float2 uv=float2((i<<1)&2,i&2);o.p=float4(uv*float2(2,-2)+float2(-1,1),.5,1);"
        "o.t0=float4(-.5+o.p.x*.1,-.35+o.p.y*.06,.75,1);return o;}"; // left wing mirror
    constexpr char gameCode[]="float4 main(float4 p:SV_Position):SV_Target{return float4(.25,.25,.25,1);}";
    ComPtr<ID3DBlob> vsBlob,wingBlob,gameBlob;ComPtr<ID3D11VertexShader> vertex,wing;ComPtr<ID3D11PixelShader> gamePixel;
    CHECK(SUCCEEDED(D3DCompile(vertexCode,sizeof(vertexCode)-1,nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vsBlob,nullptr)));
    CHECK(SUCCEEDED(D3DCompile(gameCode,sizeof(gameCode)-1,nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&gameBlob,nullptr)));
    CHECK(SUCCEEDED(device->CreateVertexShader(vsBlob->GetBufferPointer(),vsBlob->GetBufferSize(),nullptr,&vertex)));
    CHECK(SUCCEEDED(D3DCompile(wingCode,sizeof(wingCode)-1,nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&wingBlob,nullptr)));
    CHECK(SUCCEEDED(device->CreateVertexShader(wingBlob->GetBufferPointer(),wingBlob->GetBufferSize(),nullptr,&wing)));
    CHECK(SUCCEEDED(device->CreatePixelShader(gameBlob->GetBufferPointer(),gameBlob->GetBufferSize(),nullptr,&gamePixel)));
    constexpr UINT size=32;
    D3D11_TEXTURE2D_DESC colourDesc{size,size,1,1,DXGI_FORMAT_R32G32B32A32_FLOAT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> colour,depthTexture;ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11DepthStencilView> depthView;
    CHECK(SUCCEEDED(device->CreateTexture2D(&colourDesc,nullptr,&colour)) && SUCCEEDED(device->CreateRenderTargetView(colour.Get(),nullptr,&target)));
    D3D11_TEXTURE2D_DESC depthDesc{size,size,1,1,DXGI_FORMAT_D32_FLOAT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_DEPTH_STENCIL};
    CHECK(SUCCEEDED(device->CreateTexture2D(&depthDesc,nullptr,&depthTexture)) && SUCCEEDED(device->CreateDepthStencilView(depthTexture.Get(),nullptr,&depthView)));
    auto constantBuffer=[&](const std::vector<float>& values) {
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=UINT(values.size()*4);desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA data{values.data(),0,0};ComPtr<ID3D11Buffer> buffer;CHECK(SUCCEEDED(device->CreateBuffer(&desc,&data,&buffer)));return buffer;
    };
    std::vector<float> globals(48*4,0.f);globals[22*4]=1; // GhostCarValues.x
    auto shiny=globals;shiny[24*4]=17.45f;shiny[24*4+1]=6.48f;shiny[24*4+2]=.08f;
    auto paint=globals;paint[24*4]=3.2f;paint[24*4+1]=3.13f;paint[24*4+2]=.02f;
    const auto frameConstants=constantBuffer({0,0,1,0, 0,1,0,0}); // colorBufferEncodingParams: identity
    D3D11_VIEWPORT viewport{0,0,float(size),float(size),0,1};
    const uint32_t rearPixel=Pack(512,256,1023);
    CHECK(mirror.CaptureRear(context.Get(),9,RearImage(rearPixel).Get()));
    mirror.SetRearCamera({1,0,0,0, 0,1,0,0, 0,0,0,-1, 0,0,0,1});
    mirror.SetSeat({0,0,0, 1,0,0, 0,1,0, 0,0,1});
    auto render=[&](const std::vector<float>& material,uint64_t scene,float eyeX=0,bool wingSurface=false) {
        const float clear[4]{0,0,0,0};context->ClearRenderTargetView(target.Get(),clear);context->ClearDepthStencilView(depthView.Get(),D3D11_CLEAR_DEPTH,1,0);
        auto* rt=target.Get();context->OMSetRenderTargets(1,&rt,depthView.Get());context->RSSetViewports(1,&viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->IASetInputLayout(nullptr);
        context->VSSetShader(wingSurface?wing.Get():vertex.Get(),nullptr,0);context->PSSetShader(gamePixel.Get(),nullptr,0);
        const auto materialBuffer=constantBuffer(material);
        ID3D11Buffer* buffers[2]{materialBuffer.Get(),frameConstants.Get()};context->PSSetConstantBuffers(0,2,buffers);
        std::vector<float> camera(16*4,0.f);camera[14*4]=eyeX; // CameraParams.eyePositionWS
        const auto cameraBuffer=constantBuffer(camera);auto* rawCamera=cameraBuffer.Get();context->PSSetConstantBuffers(3,1,&rawCamera);
        context->Draw(3,0);
        const auto result=mirror.DrawOverlay(context.Get(),scene,0x5036b5f4a70e7144ull,[](ID3D11DeviceContext* c,void*){c->Draw(3,0);},nullptr);
        CHECK(result==RearMirror::OverlayResult::Drawn);
        ComPtr<ID3D11PixelShader> after;context->PSGetShader(&after,nullptr,nullptr);CHECK(after==gamePixel);
        ComPtr<ID3D11Buffer> boundMaterial;context->PSGetConstantBuffers(0,1,&boundMaterial);CHECK(boundMaterial==materialBuffer);
        ID3D11ShaderResourceView* extra[3]{};context->PSGetShaderResources(29,3,extra);
        for (auto* view:extra) {CHECK(!view);}
        ID3D11UnorderedAccessView* access{};context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,7,1,&access);CHECK(!access);
        D3D11_TEXTURE2D_DESC desc=colourDesc;desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;CHECK(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)));
        context->CopyResource(staging.Get(),colour.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};CHECK(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)));
        std::array<float,4> centre{};memcpy(centre.data(),static_cast<const char*>(mapped.pData)+mapped.RowPitch*(size/2)+(size/2)*16,16);
        context->Unmap(staging.Get(),0);return centre;
    };
    (void)render(shiny,10); // First frame: the mirror is only marked as seen.
    const auto mirrored=render(shiny,11);
    const auto mirrorValue=[](double v){return float(.9*std::pow(v/1023.,2.2));}; // solid rear image: independent of FOV
    CHECK(std::abs(mirrored[0]-mirrorValue(512))<.02f && std::abs(mirrored[1]-mirrorValue(256))<.02f && std::abs(mirrored[2]-mirrorValue(1023))<.02f && std::abs(mirrored[3]-1)<.01f);
    const auto untouched=render(paint,12);
    CHECK(std::abs(untouched[0]-.25f)<.001f && std::abs(untouched[2]-.25f)<.001f);
    // Wing mirrors keep the game's own look (Shiny Metal there is untouched).
    (void)render(shiny,20,0,true);const auto wingGlass=render(shiny,21,0,true);
    CHECK(std::abs(wingGlass[0]-.25f)<.001f && std::abs(wingGlass[2]-.25f)<.001f);
    // A fixed picture: the game's eye position (CameraParams b3) never moves
    // the sampled spot (debug output = rear-image stripes).
    mirror.SetDebug(true);
    const auto atSeat=render(shiny,13),moved=render(shiny,14,.1f),distant=render(shiny,15,5.f);
    mirror.SetDebug(false);
    CHECK(std::abs(moved[0]-atSeat[0])<.001f && std::abs(moved[1]-atSeat[1])<.001f && std::abs(distant[0]-atSeat[0])<.001f && std::abs(distant[1]-atSeat[1])<.001f);
    std::puts("PASS: rear mirror cube un-flips the game cube, shows the rear camera, binds and restores exactly; mirror overlay paints only Shiny Metal");
}
