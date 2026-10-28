#include "spotlight_correction.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
using namespace grid2vr;
using namespace grid2vr::spotlightprojection;
using Microsoft::WRL::ComPtr;
using Bytes=std::vector<uint8_t>;
using Vec=std::array<double,4>;
using Result=SpotlightCorrection::Result;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);return false;}} while(0)
namespace {
unsigned dispatches{};
void STDMETHODCALLTYPE Native(ID3D11DeviceContext* c,UINT x,UINT y,UINT z){++dispatches;c->Dispatch(x,y,z);}
Matrix Identity(){return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};}
Matrix Product(const Matrix& a,const Matrix& b){Matrix m{};for(int r=0;r<4;++r)for(int c=0;c<4;++c)for(int k=0;k<4;++k)m[r*4+c]+=a[r*4+k]*b[k*4+c];return m;}
Vec Times(Vec v,const Matrix& m){Vec a{};for(int c=0;c<4;++c)for(int k=0;k<4;++k)a[c]+=v[k]*m[k*4+c];return a;}
Matrix Pose(double yaw,double pitch,Vec p){
    auto y=Identity(),x=y;y[0]=y[10]=cos(yaw);y[2]=-sin(yaw);y[8]=sin(yaw);
    x[5]=x[10]=cos(pitch);x[6]=sin(pitch);x[9]=-sin(pitch);
    auto m=Product(y,x);for(int i=0;i<3;++i)m[12+i]=p[i];return m;
}
Matrix Inverse(const Matrix& a){auto m=Identity();for(int r=0;r<3;++r)for(int c=0;c<3;++c)m[r*4+c]=a[c*4+r];for(int c=0;c<3;++c)for(int k=0;k<3;++k)m[12+c]-=a[12+k]*m[k*4+c];return m;}
void Put(uint8_t* dst,const Matrix& m){for(unsigned i=0;i<16;++i){float f=float(m[i]);memcpy(dst+4*i,&f,4);}}
void Put(uint8_t* dst,Vec v){for(unsigned i=0;i<4;++i){float f=float(v[i]);memcpy(dst+4*i,&f,4);}}
float Float(const uint8_t* p){float f{};memcpy(&f,p,4);return f;}
std::array<uint8_t,512> Camera(const Matrix& world){std::array<uint8_t,512> b{};Put(b.data()+0x70,world);Put(b.data()+0x150,Inverse(world));return b;}
Bytes Vertex(const Matrix& model,const Matrix& view){Bytes b(192,0x37);Put(b.data()+128,Product(model,view));return b;}
const Vec light{2,3,-2,1},direction{0,-.6,-.8,0};
const Matrix projector{.16,.02,0,0, .01,.19,0,0, -.01,.03,1,-.03, .3,.5,0,1};
const Matrix shadow{.2,0,0,0, 0,.2,.01,0, .01,0,.04,-.02, .2,.4,.1,1};
Bytes Pixel(Layout layout,const Matrix& world){
    Bytes b(layout.bytes,0x5a);const auto view=Inverse(world);
    auto p=Times(light,view),d=Times(direction,view);p[3]=13;d[3]=-7;
    Put(b.data()+32,p);Put(b.data()+48,d);Put(b.data()+layout.mapping,Product(world,projector));
    if(layout.shadow)Put(b.data()+144,Product(world,shadow));return b;
}
struct Gpu {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    bool Open(){
        const auto dll=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(dll,"D3D11CreateDevice"));
        CHECK(create && SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
        return true;
    }
    bool Buffer(std::span<const uint8_t> bytes,ComPtr<ID3D11Buffer>& output){
        output.Reset();D3D11_BUFFER_DESC desc{};desc.ByteWidth=UINT(bytes.size());desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA initial{bytes.data(),0,0};CHECK(SUCCEEDED(device->CreateBuffer(&desc,&initial,&output)));return true;
    }
    bool Read(ID3D11Buffer* buffer,Bytes& output){
        D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);output.resize(desc.ByteWidth);
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;CHECK(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&staging)));
        context->CopyResource(staging.Get(),buffer);D3D11_MAPPED_SUBRESOURCE mapped{};
        CHECK(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)));
        memcpy(output.data(),mapped.pData,output.size());context->Unmap(staging.Get(),0);return true;
    }
    void Bind(ID3D11Buffer* pixel,ID3D11Buffer* vertex){context->PSSetConstantBuffers(0,1,&pixel);context->VSSetConstantBuffers(0,1,&vertex);}
};
bool Compile(const char* source,const char* entry,const char* target,ComPtr<ID3DBlob>& code){
    ComPtr<ID3DBlob> error;const auto hr=D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,entry,target,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
    if(FAILED(hr) && error)std::fprintf(stderr,"%.*s\n",int(error->GetBufferSize()),static_cast<char*>(error->GetBufferPointer()));CHECK(SUCCEEDED(hr));return true;
}
SpotlightCorrection::DrawKey Key(unsigned variant=3,unsigned ordinal=332){
    SpotlightCorrection::DrawKey k{};k.slot=0;k.ordinal=ordinal;k.kindHash=0x44524157;k.args={36,0,0,0,0};k.argCount=3;
    k.vsHash=VertexShader;k.psHash=PixelShaders[variant];return k;
}
bool EqualConstants(const Bytes& actual,const Bytes& expected,const Layout& layout){
    CHECK(actual.size()==expected.size());
    for(size_t i=0;i<actual.size();++i)if(!MutablePixelByte(layout,i))CHECK(actual[i]==expected[i]);
    for(size_t i=0;i<actual.size();i+=4)if(MutablePixelByte(layout,i)){
        const float a=Float(actual.data()+i),b=Float(expected.data()+i);
        CHECK(std::isfinite(a) && std::abs(a-b)<.00004+std::abs(b)*.000003);
    }
    return true;
}
// Faithful spotlight math on a world-space plane: distance attenuation,
// cone/diffuse terms, projector coordinates and shadow coordinates. The
// independent CPU oracle uses the fixed world light, never the helper's math.
struct ImageFixture {
    static constexpr UINT Width=24,Height=16;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Texture2D> target,staging;ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11RasterizerState> rasterizer;
    bool Open(Gpu& gpu){
        constexpr char source[]=R"(
cbuffer Light:register(b0){float4 light[29];};
cbuffer Eye:register(b1){row_major float4x4 view;uint4 layout;};
float4 VS(uint id:SV_VertexID):SV_Position {return float4(id==1?3:-1,id==2?3:-1,0,1);}
float4 Times(float4 p,uint i){return p.x*light[i]+p.y*light[i+1]+p.z*light[i+2]+p.w*light[i+3];}
float4 PS(float4 screen:SV_Position):SV_Target {
    float4 world=float4((screen.x/24-.5)*8,0,-4-screen.y/16*12,1);
    float4 surfaceView=mul(world,view);float3 normal=mul(float4(0,1,0,0),view).xyz;
    float3 toward=light[2].xyz-surfaceView.xyz;float distance=length(toward);toward/=distance;
    float cone=saturate((-dot(toward,light[3].xyz)-.7)/.3);
    float intensity=cone*cone*saturate(dot(toward,normal))/(1+.02*distance*distance);
    float4 mapped=Times(surfaceView,layout.x);float4 shadow=layout.y?Times(surfaceView,9):mapped;
    return float4(intensity,mapped.x/mapped.w,mapped.y/mapped.w,shadow.z/shadow.w);
})";
        ComPtr<ID3DBlob> code;CHECK(Compile(source,"VS","vs_5_0",code));CHECK(SUCCEEDED(gpu.device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs)));
        code.Reset();CHECK(Compile(source,"PS","ps_5_0",code));CHECK(SUCCEEDED(gpu.device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps)));
        D3D11_TEXTURE2D_DESC td{};td.Width=Width;td.Height=Height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
        td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
        CHECK(SUCCEEDED(gpu.device->CreateTexture2D(&td,nullptr,&target)));CHECK(SUCCEEDED(gpu.device->CreateRenderTargetView(target.Get(),nullptr,&rtv)));
        td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        CHECK(SUCCEEDED(gpu.device->CreateTexture2D(&td,nullptr,&staging)));
        D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
        CHECK(SUCCEEDED(gpu.device->CreateRasterizerState(&raster,&rasterizer)));return true;
    }
    bool Render(Gpu& gpu,const Matrix& eye,Layout layout,std::vector<float>& values){
        std::array<uint8_t,80> params{};Put(params.data(),eye);const uint32_t meta[]{UINT(layout.mapping/16),UINT(layout.shadow),0,0};memcpy(params.data()+64,meta,16);
        ComPtr<ID3D11Buffer> buffer;CHECK(gpu.Buffer(params,buffer));auto* binding=buffer.Get();gpu.context->PSSetConstantBuffers(1,1,&binding);
        gpu.context->SetPredication(nullptr,FALSE);auto* rt=rtv.Get();gpu.context->OMSetRenderTargets(1,&rt,nullptr);
        const D3D11_VIEWPORT viewport{0,0,float(Width),float(Height),0,1};gpu.context->RSSetViewports(1,&viewport);
        gpu.context->IASetInputLayout(nullptr);gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gpu.context->VSSetShader(vs.Get(),nullptr,0);gpu.context->PSSetShader(ps.Get(),nullptr,0);
        gpu.context->GSSetShader(nullptr,nullptr,0);gpu.context->HSSetShader(nullptr,nullptr,0);gpu.context->DSSetShader(nullptr,nullptr,0);
        gpu.context->OMSetBlendState(nullptr,nullptr,~0u);gpu.context->OMSetDepthStencilState(nullptr,0);gpu.context->RSSetState(rasterizer.Get());
        gpu.context->Draw(3,0);gpu.context->OMSetRenderTargets(0,nullptr,nullptr);
        gpu.context->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        CHECK(SUCCEEDED(gpu.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)));
        values.resize(Width*Height*4);for(UINT y=0;y<Height;++y)memcpy(values.data()+y*Width*4,static_cast<uint8_t*>(mapped.pData)+y*mapped.RowPitch,Width*16);
        gpu.context->Unmap(staging.Get(),0);return true;
    }
    bool Oracle(const std::vector<float>& pixels,bool shadowed){
        double maxError{};unsigned lit{};
        for(UINT y=0;y<Height;++y)for(UINT x=0;x<Width;++x){
            const Vec p{((x+.5)/Width-.5)*8,0,-4-(y+.5)/Height*12,1};Vec toward{};double distance2{};
            for(int i=0;i<3;++i){toward[i]=light[i]-p[i];distance2+=toward[i]*toward[i];}
            for(int i=0;i<3;++i)toward[i]/=sqrt(distance2);
            const double cone=std::clamp((-toward[0]*direction[0]-toward[1]*direction[1]-toward[2]*direction[2]-.7)/.3,0.,1.);
            const auto m=Times(p,projector),s=Times(p,shadowed?shadow:projector);
            const double intensity=cone*cone*std::clamp(toward[1],0.,1.)/(1+.02*distance2);
            const double expected[]{intensity,m[0]/m[3],m[1]/m[3],s[2]/s[3]};if(intensity>.01)++lit;
            for(unsigned c=0;c<4;++c){const float actual=pixels[(y*Width+x)*4+c];CHECK(std::isfinite(actual));maxError=std::max(maxError,std::abs(actual-expected[c]));}
        }
        if(maxError>=.00008)std::fprintf(stderr,"world-fixed image max error %.9f\n",maxError);
        CHECK(lit>20 && maxError<.00008);return true;
    }
};
bool LayoutsAndImages(){
    Gpu gpu;CHECK(gpu.Open());ImageFixture image;CHECK(image.Open(gpu));SpotlightCorrection helper;helper.Configure(Native);
    const auto center=Pose(.17,-.04,{1,2,3,1}),model=Pose(.3,.1,{3,0,-7,1});const auto centerVertex=Vertex(model,Inverse(center));
    unsigned compared{};bool staleDiffers=false;
    for(unsigned variant=0;variant<6;++variant){
        const auto key=Key(variant);const auto layout=PixelLayout(key.vsHash,key.psHash);const auto source=Pixel(layout,center);
        ComPtr<ID3D11Buffer> nativePixel,nativeVertex;CHECK(gpu.Buffer(source,nativePixel));CHECK(gpu.Buffer(centerVertex,nativeVertex));
        gpu.Bind(nativePixel.Get(),nativeVertex.Get());CHECK(helper.BeginScene(variant+1));
        CHECK(helper.BeforeDraw(gpu.context.Get(),key,0)==Result::Captured);
        for(unsigned scenario=0;scenario<3;++scenario)for(unsigned pass=1;pass<=2;++pass){
            const auto eyeWorld=Pose(.17+.31*scenario,-.04-.09*scenario,{1+(pass==1?-.032:.032),2+.12*scenario,3-.2*scenario,1});
            const auto eyeView=Inverse(eyeWorld);const auto eyeVertex=Vertex(model,eyeView);Change change{};
            CHECK(MakeChange(Camera(center),Camera(eyeWorld),change));
            gpu.context->UpdateSubresource(nativeVertex.Get(),0,nullptr,eyeVertex.data(),0,0);
            gpu.Bind(nativePixel.Get(),nativeVertex.Get());
            CHECK(helper.BeforeDraw(gpu.context.Get(),key,pass,&change)==Result::Applied);
            CHECK(helper.Pending() && !helper.ResetResources() && !helper.BeginScene(999));
            ComPtr<ID3D11Buffer> corrected;gpu.context->PSGetConstantBuffers(0,1,&corrected);CHECK(corrected!=nativePixel);
            Bytes actual,expected(source.size()),unchanged;CHECK(gpu.Read(corrected.Get(),actual));
            CHECK(CorrectPixelConstants(key.vsHash,key.psHash,source,change,expected));CHECK(EqualConstants(actual,expected,layout));
            CHECK(gpu.Read(nativePixel.Get(),unchanged));CHECK(unchanged==source);
            CHECK(gpu.Read(nativeVertex.Get(),unchanged));CHECK(unchanged==eyeVertex);
            std::vector<float> correctedImage;CHECK(image.Render(gpu,eyeView,layout,correctedImage));CHECK(image.Oracle(correctedImage,layout.shadow));
            CHECK(helper.AfterDraw(gpu.context.Get()) && !helper.Pending());
            ComPtr<ID3D11Buffer> restored;gpu.context->PSGetConstantBuffers(0,1,&restored);CHECK(restored==nativePixel);
            if(scenario==2){std::vector<float> stale;CHECK(image.Render(gpu,eyeView,layout,stale));for(size_t i=0;i<stale.size();++i)if(std::abs(stale[i]-correctedImage[i])>.05)staleDiffers=true;}
            ++compared;
        }
    }
    CHECK(helper.BeginScene(99));CHECK(helper.Stats().corrected>0 && helper.Stats().samples<=32);
    CHECK(compared==36 && staleDiffers && helper.ResetResources());
    std::printf("Six layouts: %u GPU images / %u pixel components match the independent world-space light oracle; stale constants visibly differ.\n",compared,compared*ImageFixture::Width*ImageFixture::Height*4);
    return true;
}
bool GuardsAndLifecycle(){
    Gpu gpu;CHECK(gpu.Open());SpotlightCorrection helper;helper.Configure(Native);
    const auto center=Pose(.1,.03,{1,2,3,1}),eye=Pose(.4,-.07,{1.032,2.2,2.8,1}),model=Pose(.2,.1,{0,1,-5,1});
    const auto key=Key();const auto layout=PixelLayout(key.vsHash,key.psHash);const auto source=Pixel(layout,center);
    const auto cv=Vertex(model,Inverse(center)),ev=Vertex(model,Inverse(eye));Change change{};CHECK(MakeChange(Camera(center),Camera(eye),change));
    ComPtr<ID3D11Buffer> pixel,vertex;CHECK(gpu.Buffer(source,pixel));CHECK(gpu.Buffer(cv,vertex));gpu.Bind(pixel.Get(),vertex.Get());
    CHECK(helper.BeginScene(100));CHECK(helper.BeforeDraw(gpu.context.Get(),key,0)==Result::Captured);
    gpu.context->UpdateSubresource(vertex.Get(),0,nullptr,ev.data(),0,0);
    auto missing=key;missing.ordinal++;CHECK(helper.BeforeDraw(gpu.context.Get(),missing,1,&change)==Result::Skipped);
    missing=key;missing.args[0]++;CHECK(helper.BeforeDraw(gpu.context.Get(),missing,1,&change)==Result::Skipped);
    missing=key;missing.vsHash^=1;CHECK(helper.BeforeDraw(gpu.context.Get(),missing,1,&change)==Result::Skipped);
    missing=key;missing.psHash^=1;CHECK(helper.BeforeDraw(gpu.context.Get(),missing,1,&change)==Result::Skipped);
    Change invalid{};CHECK(helper.BeforeDraw(gpu.context.Get(),key,1,&invalid)==Result::Skipped);
    auto unchanged=[&](const Bytes& input,const Bytes& vs){
        gpu.context->UpdateSubresource(pixel.Get(),0,nullptr,input.data(),0,0);gpu.context->UpdateSubresource(vertex.Get(),0,nullptr,vs.data(),0,0);gpu.Bind(pixel.Get(),vertex.Get());
        CHECK(helper.BeforeDraw(gpu.context.Get(),key,1,&change)==Result::Applied);
        ComPtr<ID3D11Buffer> output;gpu.context->PSGetConstantBuffers(0,1,&output);Bytes got;CHECK(gpu.Read(output.Get(),got));CHECK(got==input);
        CHECK(helper.AfterDraw(gpu.context.Get()));return true;
    };
    Bytes already(source.size());CHECK(CorrectPixelConstants(key.vsHash,key.psHash,source,change,already));CHECK(unchanged(already,ev)); // no double transform
    auto different=source;different[16]^=1;CHECK(unchanged(different,ev));
    CHECK(unchanged(source,cv)); // stale native volume cannot be paired to a fresh eye
    // Matching nonfinite center data must not produce partially transformed output.
    auto bad=source;const float nan=std::numeric_limits<float>::quiet_NaN();memcpy(bad.data()+32,&nan,4);
    gpu.context->UpdateSubresource(pixel.Get(),0,nullptr,bad.data(),0,0);gpu.context->UpdateSubresource(vertex.Get(),0,nullptr,cv.data(),0,0);gpu.Bind(pixel.Get(),vertex.Get());
    CHECK(helper.BeginScene(101));CHECK(helper.BeforeDraw(gpu.context.Get(),key,0)==Result::Captured);CHECK(unchanged(bad,ev));
    CHECK(helper.BeginScene(102));CHECK(helper.BeforeDraw(gpu.context.Get(),key,1,&change)==Result::Skipped);
    // Bounded GPU storage: unknown/missing center entries never reuse an old snapshot.
    gpu.context->UpdateSubresource(pixel.Get(),0,nullptr,source.data(),0,0);gpu.context->UpdateSubresource(vertex.Get(),0,nullptr,cv.data(),0,0);
    for(unsigned i=0;i<128;++i)CHECK(helper.BeforeDraw(gpu.context.Get(),Key(3,i),0)==Result::Captured);
    CHECK(helper.BeforeDraw(gpu.context.Get(),Key(3,128),0)==Result::Skipped && helper.Stats().overflow==1);
    gpu.context->UpdateSubresource(vertex.Get(),0,nullptr,ev.data(),0,0);CHECK(helper.BeforeDraw(gpu.context.Get(),Key(3,128),1,&change)==Result::Skipped);
    Gpu second;CHECK(second.Open());CHECK(helper.BeforeDraw(second.context.Get(),key,0)==Result::Failed);
    ComPtr<ID3D11DeviceContext> deferred;CHECK(SUCCEEDED(gpu.device->CreateDeferredContext(0,&deferred)));
    CHECK(helper.BeforeDraw(deferred.Get(),key,0)==Result::Failed);
    CHECK(helper.ResetResources());CHECK(helper.BeginScene(200));
    CHECK(second.Buffer(source,pixel));CHECK(second.Buffer(cv,vertex));second.Bind(pixel.Get(),vertex.Get());
    CHECK(helper.BeforeDraw(second.context.Get(),key,0)==Result::Captured);
    second.context->UpdateSubresource(vertex.Get(),0,nullptr,ev.data(),0,0);
    CHECK(helper.BeforeDraw(second.context.Get(),key,2,&change)==Result::Applied);
    CHECK(helper.AfterDraw(second.context.Get()) && helper.ResetResources());return true;
}
bool StateRestoration(){
    Gpu gpu;CHECK(gpu.Open());SpotlightCorrection helper;helper.Configure(Native);
    const auto center=Pose(.1,0,{0,0,0,1}),eye=Pose(.4,.1,{.032,.1,.2,1});const auto key=Key();const auto layout=PixelLayout(key.vsHash,key.psHash);
    auto source=Pixel(layout,center),cv=Vertex(Identity(),Inverse(center)),ev=Vertex(Identity(),Inverse(eye));Change change{};CHECK(MakeChange(Camera(center),Camera(eye),change));
    ComPtr<ID3D11Buffer> pixel,vertex;CHECK(gpu.Buffer(source,pixel));CHECK(gpu.Buffer(cv,vertex));gpu.Bind(pixel.Get(),vertex.Get());
    // Non-default graphics/compute bindings, predication, a counter UAV, and
    // dynamic-linkage class instances must survive the isolated compute pass.
    constexpr char csCode[]=R"(
interface Operation{float Value();};
class NativeOperation:Operation{float stored;float Value(){return stored;}};
class OtherOperation:Operation{float stored;float Value(){return stored*2;}};
Operation selected;
RWStructuredBuffer<float> data:register(u0);
[numthreads(1,1,1)] void main(){data[0]=selected.Value();}
)";
    ComPtr<ID3DBlob> code;CHECK(Compile(csCode,"main","cs_5_0",code));ComPtr<ID3D11ClassLinkage> linkage;
    ComPtr<ID3D11ShaderReflection> reflection;CHECK(SUCCEEDED(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),__uuidof(ID3D11ShaderReflection),&reflection)));
    CHECK(reflection->GetNumInterfaceSlots()==1);
    CHECK(SUCCEEDED(gpu.device->CreateClassLinkage(&linkage)));ComPtr<ID3D11ComputeShader> native;
    CHECK(SUCCEEDED(gpu.device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),linkage.Get(),&native)));
    ComPtr<ID3D11ClassInstance> instance;CHECK(SUCCEEDED(linkage->CreateClassInstance("NativeOperation",0,0,0,0,&instance)));
    auto* ci=instance.Get();gpu.context->CSSetShader(native.Get(),&ci,1);
    auto* cb=pixel.Get();gpu.context->CSSetConstantBuffers(0,1,&cb);gpu.context->CSSetConstantBuffers(4,1,&cb);
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=64;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=4;
    ComPtr<ID3D11Buffer> raw;CHECK(SUCCEEDED(gpu.device->CreateBuffer(&bd,nullptr,&raw)));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=16;ud.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_COUNTER;
    ComPtr<ID3D11UnorderedAccessView> uav;CHECK(SUCCEEDED(gpu.device->CreateUnorderedAccessView(raw.Get(),&ud,&uav)));
    auto* uv=uav.Get();UINT counter=7;gpu.context->CSSetUnorderedAccessViews(0,1,&uv,&counter);
    D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION_PREDICATE,0};ComPtr<ID3D11Predicate> predicate;CHECK(SUCCEEDED(gpu.device->CreatePredicate(&qd,&predicate)));
    gpu.context->Begin(predicate.Get());gpu.context->End(predicate.Get());gpu.context->SetPredication(predicate.Get(),TRUE);
    const D3D11_VIEWPORT viewport{3,4,31,29,.2f,.8f};gpu.context->RSSetViewports(1,&viewport);
    auto verify=[&](){
        ComPtr<ID3D11ComputeShader> actual;ID3D11ClassInstance* classes[256]{};UINT n=256;gpu.context->CSGetShader(&actual,classes,&n);
        const bool classOk=n==1 && classes[0]==instance.Get();for(UINT i=0;i<n;++i)if(classes[i])classes[i]->Release();
        if(!classOk || actual!=native)std::fprintf(stderr,"CS restore: classes=%u classOk=%u shaderOk=%u\n",n,classOk,actual==native);
        CHECK(classOk && actual==native);
        ComPtr<ID3D11Buffer> a,b;gpu.context->CSGetConstantBuffers(0,1,&a);gpu.context->CSGetConstantBuffers(4,1,&b);CHECK(a==pixel && b==pixel);
        ComPtr<ID3D11UnorderedAccessView> ua,ub;gpu.context->CSGetUnorderedAccessViews(0,1,&ua);gpu.context->CSGetUnorderedAccessViews(1,1,&ub);CHECK(ua==uav && !ub);
        ComPtr<ID3D11Predicate> p;BOOL value{};gpu.context->GetPredication(&p,&value);CHECK(p==predicate && value);
        D3D11_VIEWPORT v{};n=1;gpu.context->RSGetViewports(&n,&v);CHECK(n==1 && !memcmp(&v,&viewport,sizeof(v)));return true;
    };
    CHECK(verify());
    CHECK(helper.BeginScene(1));CHECK(helper.BeforeDraw(gpu.context.Get(),key,0)==Result::Captured);
    CHECK(verify());
    gpu.context->UpdateSubresource(vertex.Get(),0,nullptr,ev.data(),0,0);
    CHECK(helper.BeforeDraw(gpu.context.Get(),key,1,&change)==Result::Applied);CHECK(verify());
    CHECK(helper.AfterDraw(gpu.context.Get()));CHECK(verify());gpu.context->SetPredication(nullptr,FALSE);
    D3D11_BUFFER_DESC counterDesc{};counterDesc.ByteWidth=16;counterDesc.Usage=D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Buffer> counterCopy;CHECK(SUCCEEDED(gpu.device->CreateBuffer(&counterDesc,nullptr,&counterCopy)));
    gpu.context->CopyStructureCount(counterCopy.Get(),0,uav.Get());Bytes bytes;CHECK(gpu.Read(counterCopy.Get(),bytes));UINT actualCount{};memcpy(&actualCount,bytes.data(),4);CHECK(actualCount==7);
    CHECK(helper.ResetResources());return true;
}
}
int main(){
    if(!LayoutsAndImages() || !GuardsAndLifecycle() || !StateRestoration())return 1;
    std::printf("Spotlight GPU guards, unchanged native resources, same-scene pairing, state/class-instance/predicate/counter restoration, bounded snapshots and second-device reset pass; native dispatch calls=%u.\n",dispatches);
    return 0;
}
