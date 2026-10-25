#include "screen_ui.h"
#include <fstream>
#include <cstdio>
#include <thread>
#include <d3dcompiler.h>
using namespace grid2vr;
using Microsoft::WRL::ComPtr;
#define CHECK(x) do { if (!(x)) { printf("Failed line %d: %s\n",__LINE__,#x);return 1; } } while(0)
std::array<uint8_t,320> Read(const char* path) {
    std::array<uint8_t,320> data{};std::ifstream f(path,std::ios::binary);f.read(reinterpret_cast<char*>(data.data()),data.size());
    if (!f || f.peek()!=EOF) data={};return data;
}
std::array<double,4> Clip(const std::array<uint8_t,320>& data,std::array<double,3> point) {
    std::array<float,16> m{};memcpy(m.data(),data.data()+64,64);std::array<double,4> out{};
    for (unsigned c=0;c<4;++c) out[c]=point[0]*m[c]+point[1]*m[4+c]+point[2]*m[8+c]+m[12+c];return out;
}
int main(int argc,char** argv) {
    CHECK(argc==3);const auto perspective=Read(argv[1]),ortho=Read(argv[2]);
    CHECK(ScreenUiCamera(perspective) && ScreenUiCamera(ortho));
    SceneViews views;views.poses[0].position={-.032f,0,0};views.poses[1].position={.032f,0,0};
    views.frusta={EyeFrustum{-1.37638187f,.839099586f,-1.428148f,.965689f},EyeFrustum{-.839099586f,1.37638187f,-1.428148f,.965689f}};
    for (const auto& native:{perspective,ortho}) for (unsigned eye=0;eye<2;++eye) {
        std::array<uint8_t,320> output{};CHECK(ProjectScreenUi(native,views,eye,output));
        for (size_t byte=0;byte<native.size();++byte) {
            const bool xy=byte>=64 && byte<128 && (byte-64)%16<8;
            CHECK(xy || output[byte]==native[byte]);
        }
        // Reconstruct eye rays at a shared two-unit UI plane, at center and
        // nonzero screen points. This catches swapped eyes and FOV-offset signs.
        for (const auto point:{std::array<double,3>{0,0,0},{1.2,-.7,0},{-3,2,0}}) {
            const auto oldClip=Clip(native,point),newClip=Clip(output,point);const auto& f=views.frusta[eye];
            const double rayX=(newClip[0]/newClip[3]*(f.right-f.left)+f.right+f.left)/2;
            const double rayY=(newClip[1]/newClip[3]*(f.up-f.down)+f.up+f.down)/2;
            CHECK(std::abs(views.poses[eye].position[0]+2*rayX-2*oldClip[0]/oldClip[3]/1.4894007444381714)<1e-6);
            CHECK(std::abs(2*rayY-2*oldClip[1]/oldClip[3]/2.6478235721588135)<1e-6);
        }
    }
    std::array<ScreenUiTransform,2> baseline{},transformed{};CHECK(ScreenUiTransforms(views,baseline));
    auto turned=views;const float a=.5f;
    for (unsigned eye=0;eye<2;++eye) {
        turned.poses[eye].orientation={0,std::sin(a/2),0,std::cos(a/2)};
        const float x=views.poses[eye].position[0];turned.poses[eye].position={.5f+x*std::cos(a),.2f,-x*std::sin(a)};
    }
    CHECK(ScreenUiTransforms(turned,transformed));
    for (unsigned eye=0;eye<2;++eye) CHECK(std::abs(baseline[eye].bx-transformed[eye].bx)<1e-6 && std::abs(baseline[eye].by-transformed[eye].by)<1e-6);
    auto invalid=views;invalid.poses[1].orientation={0,.1f,0,std::sqrt(.99f)};
    CHECK(!ScreenUiTransforms(invalid,transformed));invalid=views;invalid.frusta[0].left=0;CHECK(!ScreenUiTransforms(invalid,transformed));
    auto foreign=perspective;foreign[64]^=1;std::array<uint8_t,320> unchanged{};unchanged.fill(0xA5);auto out=unchanged;
    CHECK(!ProjectScreenUi(foreign,views,0,out) && out==unchanged);
    CHECK(!ProjectScreenUi(perspective,views,2,out) && out==unchanged);
    auto padded=perspective;for (size_t i=308;i<320;++i) padded[i]=uint8_t(i);
    CHECK(ProjectScreenUi(padded,views,0,out));
    CHECK(!memcmp(padded.data()+308,out.data()+308,12));
    CHECK(!ScreenUiVertexShader(0x4dd4ed841ce0d62eull) && !ScreenUiVertexShader(0x0f53b20492bb7c46ull)); // Clip-derived art UVs require separate handling.
    CHECK(!ScreenUiCamera(std::span(perspective).first(128)) && !ScreenUiVertexShader(0xac1fbbd477a609bdull));
    ScreenUiOptions options;CHECK(options.distanceMetres==2 && options.followView);
    for (const auto input:{"1 0 1","1.5 1 31","20 0 0"}) {
        std::istringstream request(input);CHECK(ReadScreenUiOptions(request,options));
    }
    for (const auto input:{"0.5 0 1","20.5 0 1","1.1 0 1","1 2 1","1 0 32","1 0 -1","1 0 1 extra","1 0"}) {
        const auto previous=options;std::istringstream request(input);CHECK(!ReadScreenUiOptions(request,options));
        CHECK(options.distanceMetres==previous.distanceMetres && options.followView==previous.followView && options.hiddenMask==previous.hiddenMask);
    }
    // Both eyes must intersect one physical plane. Fixed-to-car remains in
    // the native camera frame while turning/translating the head; follow mode
    // moves with the midpoint. Distance changes parallax, not angular size.
    for (bool follow:{false,true}) for (float distance:{1.f,2.f,20.f}) for (unsigned eye=0;eye<2;++eye) {
        options={distance,follow,1};ScreenUiPlane plane;CHECK(ScreenUiPlaneProjection(turned,eye,options,plane));
        for (const auto point:{std::array<double,3>{0,0,1},{.6,-.4,1},{-1,1,1}}) {
            std::array<double,3> projected{};for (unsigned r=0;r<3;++r) for (unsigned c=0;c<3;++c) projected[r]+=plane.rows[r][c]*point[c];
            const auto& f=turned.frusta[eye];PoseRotation axes{};CHECK(PoseAxes(turned.poses[eye],axes));
            // Invert the tangent projection directly to recover eye-local XYZ.
            const std::array<double,3> local{(projected[0]*(f.right-f.left)+(f.right+f.left)*projected[2])/2,
                (projected[1]*(f.up-f.down)+(f.up+f.down)*projected[2])/2,-projected[2]};
            std::array<double,3> world{};for (unsigned k=0;k<3;++k) {world[k]=turned.poses[eye].position[k];for (unsigned r=0;r<3;++r) world[k]+=local[r]*axes[r][k];}
            const std::array<double,3> original{distance*point[0]/1.4894007444381714,distance*point[1]/2.6478235721588135,-distance};
            std::array<double,3> expected=original;
            if (follow) {expected={.5,.2,0};for (unsigned k=0;k<3;++k) for (unsigned r=0;r<3;++r) expected[k]+=original[r]*axes[r][k];}
            for (unsigned k=0;k<3;++k) CHECK(std::abs(world[k]-expected[k])<4e-6);
        }
    }
    ScreenUiPlane canted;invalid=views;invalid.poses[1].orientation={0,.1f,0,std::sqrt(.99f)};
    CHECK(ScreenUiPlaneProjection(invalid,1,ScreenUiOptions{},canted));

    const auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    CHECK(create && SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=320;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;desc.Usage=D3D11_USAGE_DYNAMIC;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA data{perspective.data(),0,0};ComPtr<ID3D11Buffer> buffer;
    CHECK(SUCCEEDED(device->CreateBuffer(&desc,&data,&buffer)));auto* binding=buffer.Get();
    context->VSSetConstantBuffers(3,1,&binding);context->PSSetConstantBuffers(3,1,&binding);
    context->GSSetConstantBuffers(0,1,&binding);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D11_VIEWPORT viewport{0,0,1920,1080,0,1};context->RSSetViewports(1,&viewport);
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.ScissorEnable=TRUE;rd.MultisampleEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;CHECK(SUCCEEDED(device->CreateRasterizerState(&rd,&raster)));context->RSSetState(raster.Get());
    const D3D11_RECT scissor{300,100,1600,900};context->RSSetScissorRects(1,&scissor);
    D3D11_TEXTURE2D_DESC targetDesc{};targetDesc.Width=64;targetDesc.Height=64;targetDesc.MipLevels=targetDesc.ArraySize=1;
    targetDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;targetDesc.SampleDesc.Count=4;targetDesc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;ComPtr<ID3D11RenderTargetView> targetView;
    CHECK(SUCCEEDED(device->CreateTexture2D(&targetDesc,nullptr,&target)) && SUCCEEDED(device->CreateRenderTargetView(target.Get(),nullptr,&targetView)));
    auto* rtv=targetView.Get();context->OMSetRenderTargets(1,&rtv,nullptr);context->OMSetBlendState(nullptr,nullptr,0x5);
    const auto unchangedMultisampling=[&] {
        ComPtr<ID3D11RenderTargetView> actual;context->OMGetRenderTargets(1,&actual,nullptr);
        ComPtr<ID3D11RasterizerState> actualRaster;context->RSGetState(&actualRaster);
        ComPtr<ID3D11BlendState> blend;UINT mask{};context->OMGetBlendState(&blend,nullptr,&mask);
        D3D11_TEXTURE2D_DESC actualDesc{};target->GetDesc(&actualDesc);
        return actual==targetView && actualRaster==raster && !blend && mask==0x5 && actualDesc.SampleDesc.Count==4;
    };
    const auto witnessFolder=std::filesystem::temp_directory_path()/ ("grid2vr-screen-ui-"+std::to_string(GetCurrentProcessId()));
    CHECK(std::filesystem::create_directory(witnessFolder));
    ScreenUi ui;ui.Diagnose(witnessFolder);
    CHECK(ui.Apply(context.Get(),0xac1fbbd477a609bdull,views,0)==ScreenUi::Result::Skipped && !ui.Pending());
    // A native camera initialized before capture has no Upload metadata. One
    // bounded readback may seed an exact supported camera, then tracked updates
    // replace it. Repeated non-font draws must not consume late-font witnesses.
    for (unsigned i=0;i<80;++i) {
        CHECK(ui.Apply(context.Get(),0xe057d253ecd4296eull,views,0)==ScreenUi::Result::Applied && ui.Pending());
        CHECK(ui.Restore(context.Get()) && unchangedMultisampling());
    }
    const auto readback=[&](ID3D11Buffer* source) {
        D3D11_BUFFER_DESC sd{};sd.ByteWidth=320;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;std::array<uint8_t,320> result{};
        if (FAILED(device->CreateBuffer(&sd,nullptr,&staging))) return result;
        context->CopyResource(staging.Get(),source);D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return result;
        memcpy(result.data(),mapped.pData,result.size());context->Unmap(staging.Get(),0);return result;
    };
    for (const auto shader:screenUiShaders) for (unsigned eye=0;eye<2;++eye) {
        CHECK(ui.Apply(context.Get(),shader,views,eye)==ScreenUi::Result::Applied && ui.Pending());
        ComPtr<ID3D11Buffer> actual;context->VSGetConstantBuffers(3,1,&actual);CHECK(actual==buffer && readback(actual.Get())==perspective);
        actual.Reset();context->GSGetConstantBuffers(0,1,&actual);CHECK(actual && actual!=buffer);
        ComPtr<ID3D11GeometryShader> projected;context->GSGetShader(&projected,nullptr,nullptr);CHECK(projected);
        actual.Reset();context->PSGetConstantBuffers(3,1,&actual);CHECK(actual==buffer);
        D3D11_RECT changed{};UINT count=1;context->RSGetScissorRects(&count,&changed);CHECK(count==1 && memcmp(&changed,&scissor,sizeof(scissor)));
        CHECK(ui.Apply(context.Get(),0x6980d3e81754047dull,views,eye)==ScreenUi::Result::Failed);
        bool foreignRestore=true;std::thread other([&]{foreignRestore=ui.Restore(context.Get());});other.join();CHECK(!foreignRestore && ui.Pending());
        CHECK(ui.Restore(context.Get()) && !ui.Pending());
        CHECK(unchangedMultisampling());
        projected.Reset();context->GSGetShader(&projected,nullptr,nullptr);CHECK(!projected);
        actual.Reset();context->GSGetConstantBuffers(0,1,&actual);CHECK(actual==buffer);
        actual.Reset();context->VSGetConstantBuffers(3,1,&actual);CHECK(actual==buffer && readback(buffer.Get())==perspective);
        count=1;context->RSGetScissorRects(&count,&changed);CHECK(count==1 && !memcmp(&changed,&scissor,sizeof(scissor)));
    }
    {
        std::ifstream witnesses(witnessFolder/"screen-ui-cameras.jsonl");std::string row;unsigned lines{};bool textSeen=false;
        while (std::getline(witnesses,row)) {++lines;textSeen|=row.find("\"vertexShader\":"+std::to_string(0x6980d3e81754047dull))!=std::string::npos;}
        CHECK(lines==screenUiShaders.size() && textSeen);
    }
    // The exact overlay/font witness does not qualify that shader for stereo,
    // alter GPU state, or append one row per frame. Unknown programs stay out.
    constexpr uint64_t unqualifiedShader=0x7daadac4d591f788ull;
    CHECK(ScreenUiWitnessShader(unqualifiedShader) && !ScreenUiVertexShader(unqualifiedShader));
    for (unsigned i=0;i<100;++i) {
        ui.Observe(context.Get(),unqualifiedShader,42+i);
        ui.Observe(context.Get(),0xac1fbbd477a609bdull,42+i);
        CHECK(ui.Apply(context.Get(),unqualifiedShader,views,0)==ScreenUi::Result::Skipped && !ui.Pending());
        CHECK(unchangedMultisampling() && readback(buffer.Get())==perspective);
    }
    {
        std::ifstream witnesses(witnessFolder/"screen-ui-cameras.jsonl");std::string row;unsigned lines{},observed{};
        while (std::getline(witnesses,row)) {
            ++lines;if (row.find("\"vertexShader\":"+std::to_string(unqualifiedShader))==std::string::npos) continue;
            ++observed;CHECK(row.find("\"projectionShader\":false")!=std::string::npos && row.find("\"nativeFrame\":42")!=std::string::npos);
            CHECK(row.find("\"supportedCamera\":true")!=std::string::npos && row.find("\"phase\":\"scene_replay\"")!=std::string::npos);
            CHECK(row.find("\"viewport\":[0,0,1920,1080]")!=std::string::npos && row.find("\"target\":[64,64,28,4]")!=std::string::npos);
        }
        CHECK(lines==screenUiShaders.size()+1 && observed==1);
    }
    // Native coverage is independent of late-UI rendering. It holds no target
    // reference and cannot keep an XR frame open or execute a draw callback.
    ScreenUiNativeWitness nativeWitness;nativeWitness.Diagnose(witnessFolder);
    const auto targetReferences=target->AddRef();target->Release();
    CHECK(nativeWitness.Begin(context.Get(),100,reinterpret_cast<uintptr_t>(target.Get())));
    CHECK(target->AddRef()==targetReferences);target->Release();
    CHECK(!nativeWitness.Begin(context.Get(),101,reinterpret_cast<uintptr_t>(target.Get())));
    nativeWitness.Observe(context.Get(),99,unqualifiedShader);
    nativeWitness.Observe(context.Get(),100,0xac1fbbd477a609bdull);
    std::thread foreignWitness([&] {
        nativeWitness.Observe(context.Get(),100,unqualifiedShader);nativeWitness.End(true);
    });foreignWitness.join();CHECK(nativeWitness.Frame()==100);
    for (unsigned i=0;i<100;++i) nativeWitness.Observe(context.Get(),100,unqualifiedShader);
    nativeWitness.Observe(context.Get(),100,screenUiShaders[0]);
    nativeWitness.End(true);CHECK(!nativeWitness.Frame());
    nativeWitness.Observe(context.Get(),100,unqualifiedShader);
    CHECK(!nativeWitness.Begin(context.Get(),100,reinterpret_cast<uintptr_t>(target.Get())));
    CHECK(unchangedMultisampling() && readback(buffer.Get())==perspective);
    {
        std::ifstream report(witnessFolder/"screen-ui-native.json");std::string text((std::istreambuf_iterator<char>(report)),{});
        CHECK(text.find("\"scope\":\"normal_after_scene_observation_only\"")!=std::string::npos);
        CHECK(text.find("\"completedSceneFrames\":1")!=std::string::npos && text.find("\"lastFrame\":100")!=std::string::npos);
        CHECK(text.find("\"vertexShader\":\"7daadac4d591f788\",\"count\":100,\"projectionShader\":false")!=std::string::npos);
        CHECK(text.find("\"vertexShader\":\"e057d253ecd4296e\",\"count\":1,\"projectionShader\":true")!=std::string::npos);
        CHECK(text.size()<4096);
    }
    ComPtr<ID3D11DeviceContext> deferred;CHECK(SUCCEEDED(device->CreateDeferredContext(0,&deferred)));
    CHECK(ui.Apply(deferred.Get(),screenUiShaders[0],views,0)==ScreenUi::Result::Failed && !ui.Pending());
    bool foreignApply=false;std::thread other([&]{foreignApply=ui.Apply(context.Get(),screenUiShaders[0],views,0)==ScreenUi::Result::Failed;});other.join();
    CHECK(foreignApply && unchangedMultisampling());
    auto unsupportedViewport=viewport;unsupportedViewport.TopLeftX=1;context->RSSetViewports(1,&unsupportedViewport);
    CHECK(ui.Apply(context.Get(),screenUiShaders[0],views,0)==ScreenUi::Result::Skipped && !ui.Pending());
    ComPtr<ID3D11Buffer> unchangedBinding;context->VSGetConstantBuffers(3,1,&unchangedBinding);CHECK(unchangedBinding==buffer);
    D3D11_RECT unchangedScissor{};UINT unchangedCount=1;context->RSGetScissorRects(&unchangedCount,&unchangedScissor);
    CHECK(unchangedCount==1 && !memcmp(&unchangedScissor,&scissor,sizeof(scissor)) && unchangedMultisampling());
    context->RSSetViewports(1,&viewport);
    ScreenUi::Upload(buffer.Get(),foreign.data(),320);
    CHECK(ui.Apply(context.Get(),0xe057d253ecd4296eull,views,0)==ScreenUi::Result::Skipped && !ui.Pending());
    ScreenUi::Upload(buffer.Get(),perspective.data(),320);
    CHECK(ui.Apply(context.Get(),screenUiShaders[0],views,0)==ScreenUi::Result::Applied && ui.Restore(context.Get()));
    // Actually rasterize both exact varying signatures on WARP/4x MSAA. The
    // oversized native canvas deliberately has geometry outside its old clip
    // volume: it must not become visible when the HUD projection shrinks it.
    targetDesc.Width=targetDesc.Height=128;target.Reset();targetView.Reset();
    CHECK(SUCCEEDED(device->CreateTexture2D(&targetDesc,nullptr,&target)) && SUCCEEDED(device->CreateRenderTargetView(target.Get(),nullptr,&targetView)));
    rtv=targetView.Get();context->OMSetRenderTargets(1,&rtv,nullptr);context->OMSetBlendState(nullptr,nullptr,~0u);
    const D3D11_VIEWPORT gpuViewport{0,0,128,128,0,1};context->RSSetViewports(1,&gpuViewport);
    auto resolvedDesc=targetDesc;resolvedDesc.SampleDesc.Count=1;resolvedDesc.BindFlags=0;
    ComPtr<ID3D11Texture2D> resolved,staging;
    CHECK(SUCCEEDED(device->CreateTexture2D(&resolvedDesc,nullptr,&resolved)));
    resolvedDesc.Usage=D3D11_USAGE_STAGING;resolvedDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(device->CreateTexture2D(&resolvedDesc,nullptr,&staging)));
    const auto pixels=[&] {
        context->ResolveSubresource(resolved.Get(),0,target.Get(),0,targetDesc.Format);context->CopyResource(staging.Get(),resolved.Get());
        std::array<uint8_t,128*128*4> result{};D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) {
            for (unsigned y=0;y<128;++y) memcpy(result.data()+y*128*4,static_cast<uint8_t*>(mapped.pData)+y*mapped.RowPitch,128*4);
            context->Unmap(staging.Get(),0);
        }
        return result;
    };
    const auto compile=[&](const std::string& source,const char* target) {
        ComPtr<ID3DBlob> blob,errors;
        if (FAILED(D3DCompile(source.data(),source.size(),nullptr,nullptr,nullptr,"main",target,D3DCOMPILE_ENABLE_STRICTNESS,0,&blob,&errors))) {
            if (errors) printf("%s\n",static_cast<const char*>(errors->GetBufferPointer()));return ComPtr<ID3DBlob>{};
        }
        return blob;
    };
    {std::ofstream request(witnessFolder/"vr-hud.request");request<<"1 0 1";}
    ScreenUi gpuUi;gpuUi.Diagnose(witnessFolder);CHECK(!gpuUi.Options().followView && gpuUi.Options().distanceMetres==1);
    const float clear[4]{};
    for (unsigned mask:{23u,31u}) {
        std::string fields="float4 p:SV_Position;float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;";
        if (mask==31) fields+="float4 t3:TEXCOORD3;";fields+="float4 t4:TEXCOORD4;";
        const std::string definition="struct V{"+fields+"};";
        const auto vsCode=compile(definition+"V main(uint id:SV_VertexID){float2 p[6]={float2(-2,-2),float2(-2,2),float2(2,-2),float2(2,-2),float2(-2,2),float2(2,2)};V o;o.p=float4(p[id],.4,1);o.t0=float4(p[id]*.5+.5,.17,.18);o.t1=float4(.01,.02,.03,.04);o.t2=float4(.05,.06,.07,.08);o.t4=float4(.09,.10,.11,.12);"+
            (mask==31?"o.t3=float4(.13,.14,.15,.16);":"")+"return o;}","vs_5_0");
        const auto psCode=compile(definition+"float4 main(V v):SV_Target{float4 w=float4(.1,.2,.3,.4);return float4(v.t0.xy,dot(v.t1,w)+dot(v.t2,w)+dot(v.t4,w)+v.t0.z*.1+v.t0.w*.2"+
            (mask==31?"+dot(v.t3,w)":"")+",1);}","ps_5_0");
        CHECK(vsCode && psCode);ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;
        CHECK(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vertex)) &&
            SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&pixel)));
        context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);
        for (bool clipped:{false,true}) for (unsigned eye=0;eye<2;++eye) {
            const D3D11_RECT nativeRect{0,0,clipped?64:128,128};context->RSSetScissorRects(1,&nativeRect);
            context->ClearRenderTargetView(targetView.Get(),clear);
            const auto shader=mask==31?screenUiShaders[2]:screenUiShaders[0];
            CHECK(gpuUi.Apply(context.Get(),shader,turned,eye)==ScreenUi::Result::Applied);
            context->Draw(6,0);CHECK(gpuUi.Restore(context.Get()));
            const auto image=pixels();ScreenUiPlane plane;CHECK(ScreenUiPlaneProjection(turned,eye,gpuUi.Options(),plane));
            unsigned inside{},outside{};
            for (unsigned y=0;y<128;++y) for (unsigned x=0;x<128;++x) {
                const double nx=2.*(x+.5)/128-1,ny=1-2.*(y+.5)/128;
                // Solve the inverse plane homography for native NDC XY.
                const auto& a=plane.rows;
                const double a00=a[0][0]-nx*a[2][0],a01=a[0][1]-nx*a[2][1],b0=nx*a[2][2]-a[0][2];
                const double a10=a[1][0]-ny*a[2][0],a11=a[1][1]-ny*a[2][1],b1=ny*a[2][2]-a[1][2];
                const double det=a00*a11-a01*a10;if (std::abs(det)<1e-8) continue;
                const double u=(b0*a11-a01*b1)/det,v=(a00*b1-b0*a10)/det;
                const auto* rgba=image.data()+4*(y*128+x);const double right=clipped?0:1;
                if (u>-.85 && u<right-.15 && std::abs(v)<.85) {
                    ++inside;CHECK(rgba[3]==255 && std::abs(rgba[0]/255.-(u+1)/2)<.012 && std::abs(rgba[1]/255.-(v+1)/2)<.012);
                    CHECK(std::abs(rgba[2]/255.-(mask==31?.413:.263))<.005);
                } else if (u<-1.2 || u>right+.2 || std::abs(v)>1.2) {++outside;CHECK(rgba[3]==0);}
            }
            CHECK(inside>50 && outside>1000);
            ComPtr<ID3D11Buffer> actual;context->VSGetConstantBuffers(3,1,&actual);CHECK(actual==buffer && readback(buffer.Get())==perspective);
            actual.Reset();context->GSGetConstantBuffers(0,1,&actual);CHECK(actual==buffer);
            ComPtr<ID3D11RasterizerState> actualRaster;context->RSGetState(&actualRaster);CHECK(actualRaster==raster);
            D3D11_RECT restoredRect{};UINT n=1;context->RSGetScissorRects(&n,&restoredRect);CHECK(n==1 && !memcmp(&restoredRect,&nativeRect,sizeof(nativeRect)));
        }
        // Garage/front-end overlays arrive after both scene images have been
        // captured. Duplicate only that late draw, preserving each eye's scene
        // and the desktop target. Compare against independent direct draws,
        // with translucent blending, native clipping and both UI signatures.
        auto lateDesc=targetDesc;lateDesc.SampleDesc={1,0};
        ComPtr<ID3D11Texture2D> native,foreignTarget,lateEyes[2],reference;
        ComPtr<ID3D11RenderTargetView> nativeView,foreignView,lateViews[2],referenceView;
        CHECK(SUCCEEDED(device->CreateTexture2D(&lateDesc,nullptr,&native)) && SUCCEEDED(device->CreateRenderTargetView(native.Get(),nullptr,&nativeView)));
        CHECK(SUCCEEDED(device->CreateTexture2D(&lateDesc,nullptr,&foreignTarget)) && SUCCEEDED(device->CreateRenderTargetView(foreignTarget.Get(),nullptr,&foreignView)));
        CHECK(SUCCEEDED(device->CreateTexture2D(&lateDesc,nullptr,&reference)) && SUCCEEDED(device->CreateRenderTargetView(reference.Get(),nullptr,&referenceView)));
        for (unsigned eye=0;eye<2;++eye) CHECK(SUCCEEDED(device->CreateTexture2D(&lateDesc,nullptr,&lateEyes[eye])) &&
            SUCCEEDED(device->CreateRenderTargetView(lateEyes[eye].Get(),nullptr,&lateViews[eye])));
        const auto singlePixels=[&](ID3D11Texture2D* source) {
            context->CopyResource(staging.Get(),source);std::array<uint8_t,128*128*4> result{};D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) {
                for (unsigned y=0;y<128;++y) memcpy(result.data()+y*128*4,static_cast<uint8_t*>(mapped.pData)+y*mapped.RowPitch,128*4);
                context->Unmap(staging.Get(),0);
            }
            return result;
        };
        D3D11_BLEND_DESC blendDesc{};auto& blendTarget=blendDesc.RenderTarget[0];
        blendTarget.BlendEnable=TRUE;blendTarget.SrcBlend=D3D11_BLEND_BLEND_FACTOR;blendTarget.DestBlend=D3D11_BLEND_INV_BLEND_FACTOR;
        blendTarget.BlendOp=blendTarget.BlendOpAlpha=D3D11_BLEND_OP_ADD;blendTarget.SrcBlendAlpha=D3D11_BLEND_ONE;blendTarget.DestBlendAlpha=D3D11_BLEND_ZERO;
        blendTarget.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;ComPtr<ID3D11BlendState> translucent;
        CHECK(SUCCEEDED(device->CreateBlendState(&blendDesc,&translucent)));
        const float factor[]{.5f,.5f,.5f,.5f};context->OMSetBlendState(translucent.Get(),factor,~0u);
        const D3D11_RECT lateScissor{0,0,96,128};context->RSSetScissorRects(1,&lateScissor);
        auto* nativeRtv=nativeView.Get();context->OMSetRenderTargets(1,&nativeRtv,nullptr);
        const float desktopColor[]{.1f,.2f,.3f,1};context->ClearRenderTargetView(nativeView.Get(),desktopColor);
        const auto desktopBefore=singlePixels(native.Get());
        const float eyeColors[2][4]{{.1f,.4f,.6f,1},{.7f,.3f,.2f,1}};
        for (unsigned eye=0;eye<2;++eye) context->ClearRenderTargetView(lateViews[eye].Get(),eyeColors[eye]);
        LateScreenUi late;late.Diagnose(witnessFolder);
        CHECK(!late.Begin(context.Get(),10,turned,target.Get(),lateEyes[0].Get(),lateEyes[1].Get())); // Native 4x target remains untouched.
        CHECK(late.Begin(context.Get(),10,turned,native.Get(),lateEyes[0].Get(),lateEyes[1].Get()));
        CHECK(!late.Begin(context.Get(),11,turned,native.Get(),lateEyes[0].Get(),lateEyes[1].Get()));
        unsigned calls{};const auto draw=[](ID3D11DeviceContext* c,void* count) {++*static_cast<unsigned*>(count);c->Draw(6,0);};
        const auto shader=mask==31?screenUiShaders[2]:screenUiShaders[0];
        bool foreignLate=false;std::thread lateOther([&] {
            foreignLate=late.Draw(context.Get(),10,shader,draw,&calls)==ScreenUi::Result::Skipped;
            late.End(true);late.Reset();
        });lateOther.join();CHECK(foreignLate && late.Frame()==10 && !calls);
        CHECK(late.Draw(context.Get(),9,shader,draw,&calls)==ScreenUi::Result::Skipped && !calls);
        CHECK(late.Draw(context.Get(),10,0xac1fbbd477a609bdull,draw,&calls)==ScreenUi::Result::Skipped && !calls);
        CHECK(late.Draw(context.Get(),10,unqualifiedShader,draw,&calls)==ScreenUi::Result::Skipped && !calls);
        CHECK(singlePixels(native.Get())==desktopBefore);
        {
            std::ifstream witnesses(witnessFolder/"screen-ui-cameras.jsonl");std::string row;bool observed=false;
            while (std::getline(witnesses,row)) if (row.find("\"vertexShader\":"+std::to_string(unqualifiedShader))!=std::string::npos &&
                row.find("\"phase\":\"after_scene\"")!=std::string::npos && row.find("\"matchesCompletedNativeTarget\":true")!=std::string::npos) observed=true;
            CHECK(observed);
        }
        auto* otherRtv=foreignView.Get();context->OMSetRenderTargets(1,&otherRtv,nullptr);
        CHECK(late.Draw(context.Get(),10,shader,draw,&calls)==ScreenUi::Result::Skipped && !calls);
        context->OMSetRenderTargets(1,&nativeRtv,nullptr);
        CHECK(late.Draw(context.Get(),10,shader,draw,&calls)==ScreenUi::Result::Applied && calls==2);
        CHECK(singlePixels(native.Get())==desktopBefore);
        ComPtr<ID3D11RenderTargetView> restoredTarget;context->OMGetRenderTargets(1,&restoredTarget,nullptr);CHECK(restoredTarget==nativeView);
        ComPtr<ID3D11BlendState> restoredBlend;float restoredFactors[4]{};UINT restoredMask{};
        context->OMGetBlendState(&restoredBlend,restoredFactors,&restoredMask);
        CHECK(restoredBlend==translucent && restoredMask==~0u && !memcmp(restoredFactors,factor,sizeof(factor)));
        for (unsigned eye=0;eye<2;++eye) {
            const auto actual=singlePixels(lateEyes[eye].Get());
            auto* referenceRtv=referenceView.Get();context->OMSetRenderTargets(1,&referenceRtv,nullptr);
            context->ClearRenderTargetView(referenceView.Get(),eyeColors[eye]);
            CHECK(gpuUi.Apply(context.Get(),shader,turned,eye)==ScreenUi::Result::Applied);
            context->Draw(6,0);CHECK(gpuUi.Restore(context.Get()));
            CHECK(actual==singlePixels(reference.Get()));
            unsigned changed{};for (unsigned p=0;p<128*128;++p)
                changed+=std::abs(actual[p*4]/255.f-eyeColors[eye][0])>.02f ||
                    std::abs(actual[p*4+1]/255.f-eyeColors[eye][1])>.02f;
            CHECK(changed>50 && changed<128*128/2); // UI was drawn, surrounding stereo scene survives.
        }
        CHECK(singlePixels(lateEyes[0].Get())!=singlePixels(lateEyes[1].Get()));
        context->OMSetRenderTargets(1,&nativeRtv,nullptr);late.End(true);
        CHECK(late.Frame()==0 && late.Draw(context.Get(),10,shader,draw,&calls)==ScreenUi::Result::Skipped && calls==2);
        CHECK(!late.Begin(context.Get(),10,turned,native.Get(),lateEyes[0].Get(),lateEyes[1].Get()));
        CHECK(late.Begin(context.Get(),11,turned,native.Get(),lateEyes[0].Get(),lateEyes[1].Get()));late.Reset();CHECK(!late.Frame());
        context->OMSetBlendState(nullptr,nullptr,~0u);
        rtv=targetView.Get();context->OMSetRenderTargets(1,&rtv,nullptr);
    }
    // The witnessed garage programs emit native clip XY/W in TEXCOORD0.xy.
    // Their PS samples the backdrop there, while text uses T0.w/T1.w for its
    // atlas. Rasterize those exact varying layouts and sampling coordinates:
    // changing clip position must not change either texture lookup domain.
    std::array<uint8_t,32*32*4> backdropBytes{},atlasBytes{};
    for (unsigned y=0;y<32;++y) for (unsigned x=0;x<32;++x) {
        const auto at=4*(y*32+x);
        backdropBytes[at]=uint8_t(x*8+4);backdropBytes[at+1]=uint8_t(y*8+4);backdropBytes[at+2]=64;backdropBytes[at+3]=255;
        atlasBytes[at]=uint8_t(x*8+4);atlasBytes[at+3]=255;
    }
    D3D11_TEXTURE2D_DESC sampleDesc{};sampleDesc.Width=sampleDesc.Height=32;sampleDesc.MipLevels=sampleDesc.ArraySize=1;
    sampleDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sampleDesc.SampleDesc.Count=1;sampleDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> backdrop,atlas;ComPtr<ID3D11ShaderResourceView> backdropView,atlasView;
    D3D11_SUBRESOURCE_DATA backdropData{backdropBytes.data(),32*4,0},atlasData{atlasBytes.data(),32*4,0};
    CHECK(SUCCEEDED(device->CreateTexture2D(&sampleDesc,&backdropData,&backdrop)) &&
        SUCCEEDED(device->CreateTexture2D(&sampleDesc,&atlasData,&atlas)) &&
        SUCCEEDED(device->CreateShaderResourceView(backdrop.Get(),nullptr,&backdropView)) &&
        SUCCEEDED(device->CreateShaderResourceView(atlas.Get(),nullptr,&atlasView)));
    D3D11_SAMPLER_DESC sampleState{};sampleState.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampleState.AddressU=sampleState.AddressV=sampleState.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampleState.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;CHECK(SUCCEEDED(device->CreateSamplerState(&sampleState,&sampler)));
    ID3D11SamplerState* sampleBindings[]{sampler.Get(),sampler.Get()};context->PSSetSamplers(3,2,sampleBindings);
    D3D11_MAPPED_SUBRESOURCE cameraWrite{};CHECK(SUCCEEDED(context->Map(buffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&cameraWrite)));
    memcpy(cameraWrite.pData,ortho.data(),ortho.size());context->Unmap(buffer.Get(),0);ScreenUi::Upload(buffer.Get(),ortho.data(),320);
    for (bool text:{false,true}) {
        const uint64_t shader=text?0x7115b259dffd0f6aull:0x0ef1c3aff7d5fd94ull;
        CHECK(ScreenUiVertexShader(shader) && ScreenUiVaryingMask(shader)==(text?31:23));
        const std::string fields="float4 p:SV_Position;float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;"+
            std::string(text?"float4 t3:TEXCOORD3;":"")+"float4 t4:TEXCOORD4;";
        const std::string shape="struct V{"+fields+"};";
        const auto vs=compile(shape+"V main(uint id:SV_VertexID){float2 p[6]={float2(-2,-2),float2(-2,2),float2(2,-2),float2(2,-2),float2(-2,2),float2(2,2)};V o;"
            "o.p=float4(p[id],.4,1);o.t0=float4(p[id],.4,.5+.1*p[id].x);o.t1=float4(0,0,1,.5+.1*p[id].y);o.t2=1;o.t4=0;"+
            std::string(text?"o.t3=0;":"")+"return o;}","vs_5_0");
        const auto ps=compile(shape+"Texture2D back:register("+std::string(text?"t1":"t0")+");SamplerState sb:register(s3);"+
            std::string(text?"Texture2D font:register(t0);SamplerState sf:register(s4);":"")+
            "float4 main(V v):SV_Target{float2 uv=float2((v.t0.x+1)*.5,(1-v.t0.y)*.5);float3 c=back.Sample(sb,uv).rgb;"+
            std::string(text?"c.b=font.Sample(sf,float2(v.t0.w,v.t1.w)).r;":"")+"return float4(c,1);}","ps_5_0");
        CHECK(vs && ps);ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;
        CHECK(SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex)) &&
            SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel)));
        context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);
        ID3D11ShaderResourceView* resources[]{text?atlasView.Get():backdropView.Get(),backdropView.Get()};context->PSSetShaderResources(0,2,resources);
        ScreenUi::Upload(buffer.Get(),foreign.data(),320);
        CHECK(gpuUi.Apply(context.Get(),shader,turned,0)==ScreenUi::Result::Skipped && !gpuUi.Pending());
        ScreenUi::Upload(buffer.Get(),ortho.data(),320);
        CHECK(gpuUi.Apply(context.Get(),0x7daadac4d591f788ull,turned,0)==ScreenUi::Result::Skipped); // Extra varying shape stays excluded.
        for (bool bottomStrip:{false,true}) for (unsigned eye=0;eye<2;++eye) {
            const D3D11_RECT nativeRect{0,bottomStrip?96:0,96,128};context->RSSetScissorRects(1,&nativeRect);
            context->ClearRenderTargetView(targetView.Get(),clear);
            CHECK(gpuUi.Apply(context.Get(),shader,turned,eye)==ScreenUi::Result::Applied);
            context->Draw(6,0);CHECK(gpuUi.Restore(context.Get()));
            const auto image=pixels();ScreenUiPlane plane;CHECK(ScreenUiPlaneProjection(turned,eye,gpuUi.Options(),plane));
            unsigned inside{},outside{};
            for (unsigned y=0;y<128;++y) for (unsigned x=0;x<128;++x) {
                const double nx=2.*(x+.5)/128-1,ny=1-2.*(y+.5)/128;const auto& a=plane.rows;
                const double aa=a[0][0]-nx*a[2][0],ab=a[0][1]-nx*a[2][1],ac=nx*a[2][2]-a[0][2];
                const double ba=a[1][0]-ny*a[2][0],bb=a[1][1]-ny*a[2][1],bc=ny*a[2][2]-a[1][2];
                const double det=aa*bb-ab*ba;if (std::abs(det)<1e-8) continue;
                const double u=(ac*bb-ab*bc)/det,v=(aa*bc-ac*ba)/det,top=bottomStrip?-.5:1;
                const auto* rgba=image.data()+4*(y*128+x);
                if (u>-.85 && u<.35 && v>-.85 && v<top-.15) {
                    ++inside;CHECK(rgba[3]==255 && std::abs(rgba[0]/255.-(u+1)/2)<.017 && std::abs(rgba[1]/255.-(1-v)/2)<.017);
                    CHECK(std::abs(rgba[2]/255.-(text?.5+.1*u:64./255))<.017);
                } else if (u<-1.2 || u>.7 || v<-1.2 || v>top+.2) {++outside;CHECK(!rgba[3]);}
            }
            CHECK(inside>8 && outside>1000 && readback(buffer.Get())==ortho);
            D3D11_RECT restored{};UINT count=1;context->RSGetScissorRects(&count,&restored);CHECK(count==1 && !memcmp(&restored,&nativeRect,sizeof(restored)));
            ID3D11ShaderResourceView* bound[2]{};context->PSGetShaderResources(0,2,bound);
            CHECK(bound[0]==resources[0] && bound[1]==resources[1]);for (auto* value:bound) if (value) value->Release();
        }
    }
    CHECK(std::filesystem::remove(witnessFolder/"vr-hud.request"));
    CHECK(SUCCEEDED(device->GetDeviceRemovedReason()));
    CHECK(std::filesystem::remove(witnessFolder/"screen-ui-cameras.jsonl"));
    CHECK(std::filesystem::remove(witnessFolder/"screen-ui-late.json"));
    CHECK(std::filesystem::remove(witnessFolder/"screen-ui-native.json"));
    CHECK(std::filesystem::remove(witnessFolder/"screen-ui-options.json") && std::filesystem::remove(witnessFolder));
    puts("Guarded UI: binocular plane rays, nine shader signatures, early-camera cache, late-font witnesses, unknown-camera/context refusal, native bytes/bindings/scissors and 4x MSAA state preservation passed.");
}
