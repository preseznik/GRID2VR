#include "rear_mirror.h"
#include <d3dcompiler.h>
using Microsoft::WRL::ComPtr;
namespace grid2vr {
namespace {
// t0 game cube, t1 rear image, u0 substitute cube (6 slices), u1 gain stats:
// [0] cube luma sum, [4] rear luma sum (both x1024, sparse), [12] gain (float).
constexpr char fillCode[]=R"(
cbuffer P:register(b0){float4 vp[4];float4 params;}
// The game's PerFrameConstantBuffer (car_basic b1): envMapEncodingParams is
// how the cube is encoded; car_basic decodes y=x*e.y/(x*e.w+1).
cbuffer F:register(b1){float4 colorBufferEncodingParams;float4 e;}
float3 Decode(float3 x){return x*e.y/(x*e.w+1);}
float3 Encode(float3 y){return saturate(y/max(e.y-y*e.w,1e-4));}
TextureCube<float4> gameCube:register(t0);
Texture2D<float4> rear:register(t1);
SamplerState lin:register(s0);
RWTexture2DArray<float4> cube:register(u0);
RWByteAddressBuffer stats:register(u1);
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){
    float n=params.x;if(id.x>=(uint)n||id.y>=(uint)n)return;
    float u=(id.x+.5)/n*2-1,v=(id.y+.5)/n*2-1;float3 d;uint f=id.z;
    if(f==0)d=float3(1,-v,-u);else if(f==1)d=float3(-1,-v,u);else if(f==2)d=float3(u,1,v);
    else if(f==3)d=float3(u,-1,-v);else if(f==4)d=float3(u,-v,1);else d=float3(-u,-v,-1);
    // car_basic looks the cube up at (x,y,-z) of the world reflection vector.
    float3 w=normalize(float3(d.x,d.y,-d.z));
    float4 env=gameCube.SampleLevel(lin,w,0);float4 c=env;
    if(params.z>0){
        // Same projection as the game shader: rows 0-2, clip x/y/w.
        float4 clip=w.x*vp[0]+w.y*vp[1]+w.z*vp[2];
        if(clip.w>1e-4){
            float2 ndc=clip.xy/clip.w;float2 a=abs(ndc);
            if(a.x<1&&a.y<1){
                // Display-encoded rear pixels: linearise, match exposure to the
                // decoded cube, then encode through the game's own curve so
                // highlights compress like the rest of the cube.
                float3 r=pow(saturate(rear.SampleLevel(lin,float2(ndc.x*.5+.5,.5-ndc.y*.5),0).rgb),2.2);
                float g=asfloat(stats.Load(12));if(!(g>.01&&g<100))g=1;
                bool encoded=e.y>0&&params.w>0;
                float3 shown=encoded?Encode(r*g):saturate(r*g);
                float edge=saturate((1-max(a.x,a.y))/params.y);
                c.rgb=lerp(env.rgb,shown,edge);
                if(((id.x|id.y)&7)==0&&edge>=1){
                    float3 k=float3(.25,.5,.25);
                    uint ignored;
                    stats.InterlockedAdd(0,(uint)(min(dot(encoded?Decode(env.rgb):env.rgb,k),64)*256),ignored);
                    stats.InterlockedAdd(4,(uint)(dot(r,k)*256),ignored);
                }
            }
        }
    }
    cube[uint3(id.xy,f)]=c;
})";
// Runs before each fill: moves the gain a tenth of the way to last frame's ratio.
constexpr char gainCode[]=R"(
RWByteAddressBuffer stats:register(u1);
[numthreads(1,1,1)] void main(){
    uint ce=stats.Load(0),re=stats.Load(4);float g=asfloat(stats.Load(12));if(!(g>.05&&g<20))g=1;
    if(!(g>.01&&g<100))g=1;
    if(re>4096&&ce>0)g=lerp(g,clamp((float)ce/(float)re,.02,50),.1);
    stats.Store(12,asuint(g));stats.Store(0,0);stats.Store(4,0);
})";
bool Compile(ID3D11Device* device,const char* code,size_t bytes,ComPtr<ID3D11ComputeShader>& shader) {
    ComPtr<ID3DBlob> blob;
    return SUCCEEDED(D3DCompile(code,bytes,nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,nullptr)) &&
        SUCCEEDED(device->CreateComputeShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&shader));
}
}
RearMirror& Mirror() { static RearMirror mirror;return mirror; }
bool RearMirror::Initialize(ID3D11DeviceContext* context) {
    if (!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    context->GetDevice(&device_);ComPtr<ID3D11Device1> device1;
    if (FAILED(device_.As(&device1)) || FAILED(context->QueryInterface(IID_PPV_ARGS(&context_)))) return false;
    const auto level=device_->GetFeatureLevel();
    if (FAILED(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&state_))) return false;
    if (!Compile(device_.Get(),fillCode,sizeof(fillCode)-1,fill_) || !Compile(device_.Get(),gainCode,sizeof(gainCode)-1,gain_)) return false;
    D3D11_TEXTURE2D_DESC desc{cubeSize,cubeSize,1,6,DXGI_FORMAT_R10G10B10A2_UNORM,{1,0},D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,0,D3D11_RESOURCE_MISC_TEXTURECUBE};
    if (FAILED(device_->CreateTexture2D(&desc,nullptr,&cube_))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=desc.Format;view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURECUBE;view.TextureCube.MipLevels=1;
    D3D11_UNORDERED_ACCESS_VIEW_DESC access{};access.Format=desc.Format;access.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2DARRAY;access.Texture2DArray.ArraySize=6;
    if (FAILED(device_->CreateShaderResourceView(cube_.Get(),&view,&cubeView_)) || FAILED(device_->CreateUnorderedAccessView(cube_.Get(),&access,&cubeAccess_))) return false;
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=16;buffer.Usage=D3D11_USAGE_DEFAULT;buffer.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
    buffer.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    const float one=1.f;uint32_t initial[4]{0,0,0,0};memcpy(&initial[3],&one,4);
    const D3D11_SUBRESOURCE_DATA data{initial,16,0};
    if (FAILED(device_->CreateBuffer(&buffer,&data,&stats_))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC raw{};raw.Format=DXGI_FORMAT_R32_TYPELESS;raw.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
    raw.Buffer.NumElements=4;raw.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(device_->CreateUnorderedAccessView(stats_.Get(),&raw,&statsAccess_))) return false;
    D3D11_BUFFER_DESC constants{};constants.ByteWidth=80;constants.Usage=D3D11_USAGE_DEFAULT;constants.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device_->CreateBuffer(&constants,nullptr,&constants_))) return false;
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sampler,&sampler_))) return false;
    thread_=GetCurrentThreadId();return true;
}
bool RearMirror::CaptureRear(ID3D11DeviceContext* context,uint64_t scene,ID3D11Texture2D* source) {
    if (!context || !source) return false;
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    if (desc.ArraySize!=1 || desc.MipLevels!=1) return false;
    const bool resolve=desc.SampleDesc.Count>1;
    desc.SampleDesc={1,0};desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=0;desc.CPUAccessFlags=0;desc.Usage=D3D11_USAGE_DEFAULT;
    D3D11_TEXTURE2D_DESC have{};if (rear_) rear_->GetDesc(&have);
    if (!rear_ || have.Width!=desc.Width || have.Height!=desc.Height || have.Format!=desc.Format) {
        rear_.Reset();rearView_.Reset();ComPtr<ID3D11Device> device;context->GetDevice(&device);
        if (FAILED(device->CreateTexture2D(&desc,nullptr,&rear_)) || FAILED(device->CreateShaderResourceView(rear_.Get(),nullptr,&rearView_))) {
            rear_.Reset();rearView_.Reset();return false;
        }
    }
    if (resolve) context->ResolveSubresource(rear_.Get(),0,source,0,desc.Format);
    else context->CopyResource(rear_.Get(),source);
    rearScene_=scene;return true;
}
bool RearMirror::Build(ID3D11ShaderResourceView* gameCube,ID3D11Buffer* perFrame,NativeDispatch dispatch) {
    struct {std::array<float,16> vp;float params[4];} constants{viewProjection_,{float(cubeSize),.08f,camera_ && rearView_?1.f:0.f,perFrame?1.f:0.f}};
    ComPtr<ID3DDeviceContextState> previous;context_->SwapDeviceContextState(state_.Get(),&previous);
    context_->UpdateSubresource(constants_.Get(),0,nullptr,&constants,0,0);
    ID3D11UnorderedAccessView* stats[2]{nullptr,statsAccess_.Get()};
    context_->CSSetUnorderedAccessViews(0,2,stats,nullptr);
    context_->CSSetShader(gain_.Get(),nullptr,0);dispatch(context_.Get(),1,1,1);
    ID3D11UnorderedAccessView* targets[2]{cubeAccess_.Get(),statsAccess_.Get()};
    context_->CSSetUnorderedAccessViews(0,2,targets,nullptr);
    ID3D11ShaderResourceView* inputs[2]{gameCube,constants.params[2]>0?rearView_.Get():nullptr};
    context_->CSSetShaderResources(0,2,inputs);
    ID3D11Buffer* buffers[2]{constants_.Get(),perFrame?perFrame:constants_.Get()};context_->CSSetConstantBuffers(0,2,buffers);
    auto* sampler=sampler_.Get();context_->CSSetSamplers(0,1,&sampler);
    context_->CSSetShader(fill_.Get(),nullptr,0);dispatch(context_.Get(),cubeSize/8,cubeSize/8,6);
    ID3D11UnorderedAccessView* none[2]{};context_->CSSetUnorderedAccessViews(0,2,none,nullptr);
    ID3D11ShaderResourceView* noInputs[2]{};context_->CSSetShaderResources(0,2,noInputs);
    ID3D11Buffer* noBuffers[2]{};context_->CSSetConstantBuffers(0,2,noBuffers);
    context_->SwapDeviceContextState(previous.Get(),nullptr);
    if (FAILED(device_->GetDeviceRemovedReason())) return false;
    ++builds_;return true;
}
RearMirror::Result RearMirror::Apply(ID3D11DeviceContext* context,uint64_t scene,UINT cubeSlot,NativeDispatch dispatch) {
    if (failed_ || pending_ || !context || !dispatch || cubeSlot>=D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) return Result::Skipped;
    if (!device_ && !Initialize(context)) {failed_=true;return Result::Failed;}
    if (thread_!=GetCurrentThreadId() || context_.Get()!=context) return Result::Skipped;
    ComPtr<ID3D11ShaderResourceView> bound;context->PSGetShaderResources(cubeSlot,1,&bound);
    if (!bound) return Result::Skipped;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};bound->GetDesc(&desc);
    if (desc.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURECUBE || bound==cubeView_) return Result::Skipped;
    if (builtScene_!=scene) {
        // car_basic's b1 is the game's PerFrameConstantBuffer (env encoding).
        ComPtr<ID3D11Buffer> perFrame;context->PSGetConstantBuffers(1,1,&perFrame);
        D3D11_BUFFER_DESC frameDesc{};if (perFrame) perFrame->GetDesc(&frameDesc);
        if (frameDesc.ByteWidth<32 || !(frameDesc.BindFlags&D3D11_BIND_CONSTANT_BUFFER)) perFrame.Reset();
        if (!Build(bound.Get(),perFrame.Get(),dispatch)) {failed_=true;return Result::Failed;}
        builtScene_=scene;
    }
    auto* view=cubeView_.Get();context->PSSetShaderResources(cubeSlot,1,&view);
    ComPtr<ID3D11ShaderResourceView> actual;context->PSGetShaderResources(cubeSlot,1,&actual);
    original_=bound;slot_=cubeSlot;pending_=true;
    if (actual.Get()!=view) {Restore(context);failed_=true;return Result::Failed;}
    ++applied_;return Result::Applied;
}
bool RearMirror::Restore(ID3D11DeviceContext* context) {
    if (!pending_ || context_.Get()!=context || thread_!=GetCurrentThreadId()) return false;
    ComPtr<ID3D11ShaderResourceView> before;context->PSGetShaderResources(slot_,1,&before);
    auto* view=original_.Get();context->PSSetShaderResources(slot_,1,&view);
    ComPtr<ID3D11ShaderResourceView> after;context->PSGetShaderResources(slot_,1,&after);
    const bool restored=before==cubeView_ && after.Get()==view;
    original_.Reset();pending_=false;
    return restored;
}
namespace {
// car_basic's pixel input signature (all its permutations share it); world
// position is TEXCOORD0. b0 is the draw's own $Globals, b1 the game's
// PerFrameConstantBuffer (colorBufferEncodingParams first).
constexpr char mirrorCode[]=R"(
struct I{float4 p:SV_Position;float4 c0:COLOR0;float4 c1:COLOR1;float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t4:TEXCOORD4;float4 t5:TEXCOORD5;};
cbuffer G:register(b0){float4 g[48];}
cbuffer F:register(b1){float4 colorEnc;}
cbuffer M:register(b8){float4 eye;float4 axisR;float4 axisU;float4 axisF;float4 vp[4];float4 aim[3];float4 params;}
Texture2D rear:register(t30);SamplerState lin:register(s14);
ByteAddressBuffer previous:register(t31);ByteAddressBuffer gain:register(t29);
RWByteAddressBuffer current:register(u7);
float4 main(I i):SV_Target{
    float3 P=i.t0.xyz;
    float3 facet=cross(ddx(P),ddy(P));
    float4 ms=g[(uint)params.y];
    bool shiny=abs(ms.z-.08)<.005&&((abs(ms.x-17.45)<.05&&abs(ms.y-6.48)<.05)||(abs(ms.x-12.45)<.05&&abs(ms.y-1.48)<.05));
    if(!shiny||params.z<.5)discard;
    float3 L=P-eye.xyz;
    float3 loc=float3(dot(L,axisR.xyz),dot(L,axisU.xyz),dot(L,axisF.xyz));
    // Zones relative to the seat (measured on the SL: centre mirror 0.3 m
    // right, 0.04 m up, 0.31 m ahead; left wing 0.47 m left, 0.39 m down,
    // 0.79 m ahead). Only glass seen fairly face-on counts, so
    // chrome trim in the same batch (door handles, strips) keeps its shading.
    // Distance from the seat separates them for either driving side: the
    // centre mirror is within reach (SL: 0.43 m), wing mirrors are further
    // (left 0.94 m, right about 1.45 m); wings by side.
    int slot=-1;
    bool inBox=abs(loc.x)<1.8&&loc.y>-.9&&loc.y<.6&&loc.z>-.5&&loc.z<1.8;
    float reach=length(loc);
    if(inBox&&reach<.68&&loc.z>.05)slot=1;
    else if(inBox&&reach>=.68&&loc.x<0&&loc.y>-.65&&loc.y<.3)slot=0;
    else if(inBox&&reach>=.68&&loc.x>0&&loc.y>-.65&&loc.y<.3)slot=2;
    // Mirror glass faces backwards, towards the driver; trim in the same
    // batch (window lines, door handles) faces up or outwards.
    float3 nf=normalize(facet);if(dot(nf,L)>0)nf=-nf;
    float backwards=-(nf.x*axisF.x+nf.y*axisF.y+nf.z*axisF.z);
    if(slot<0||backwards<.5)discard;
    // Per mirror, kept for the whole cockpit stint (not just what is visible
    // this frame, so turning the head never moves a mirror's aim): a seen
    // flag, then the bounding box in mm relative to the seat, both edges
    // stored as maxima of (10000 + v) and (10000 - v). Its centre is not
    // pulled towards the nearer, denser side of the glass.
    uint base=(uint)slot*32,ignored;
    bool seen=previous.Load(base)!=0,boxed=previous.Load(base+20)!=0;
    int3 high=int3(previous.Load3(base+4))-10000,low=10000-int3(previous.Load3(base+16));
    int3 mm=int3(round(loc*1000));
    current.InterlockedMax(base,1,ignored);
    current.InterlockedMax(base+8,uint(10000+mm.y),ignored);
    // Wing mirrors: the glass is the highest backward-facing Shiny Metal in
    // its zone; mounts and trim below it (SL: down to 0.65 m under the eye)
    // keep the game's shading and never enter the box.
    if(!seen||(slot!=1&&mm.y<high.y-180))discard;
    current.InterlockedMax(base+4,uint(10000+mm.x),ignored);current.InterlockedMax(base+12,uint(10000+mm.z),ignored);
    current.InterlockedMax(base+16,uint(10000-mm.x),ignored);current.InterlockedMax(base+20,uint(10000-mm.y),ignored);current.InterlockedMax(base+24,uint(10000-mm.z),ignored);
    float3 centre=boxed?float3(high+low)*.0005:loc;
    // A flat mirror through this pixel whose normal sends the seat-to-centre
    // ray along the aim: one normal per mirror, so straight lines stay straight.
    float3 dc=normalize(centre),d=normalize(loc),t=normalize(aim[slot].xyz);
    float3 n=normalize(t-dc);
    float3 r=reflect(d,n);
    float c=dot(r,t);float3 side=r-t*c;r=normalize(t*c+side*params.x);
    float3 w=r.x*axisR.xyz+r.y*axisU.xyz+r.z*axisF.xyz;
    float4 clip=w.x*vp[0]+w.y*vp[1]+w.z*vp[2];
    float3 col=0;
    if(clip.w>1e-4){
        // Just outside the rear image: its edge, never black.
        float2 ndc=clamp(clip.xy/clip.w,-.98,.98);
        col=pow(saturate(rear.SampleLevel(lin,float2(ndc.x*.5+.5,.5-ndc.y*.5),0).rgb),2.2);
    }
    if(params.z>1.5){
        // Debug: zone colour; magenta = reflection behind the rear camera,
        // yellow = outside its image.
        // Rear-image position: red = horizontal, green = vertical (0..1),
        // blue = zone; behind the rear camera = white.
        float2 at=clip.w>1e-4?saturate(clip.xy/clip.w*.5+.5):float2(1,1);
        col=float3(at.x,at.y,clip.w>1e-4?slot*.5:1)*.5;
    }
    float k=params.z>1.5?1:asfloat(gain.Load(12));if(!(k>.01&&k<100))k=1;
    col*=k*(params.z>1.5?1:params.w);
    return float4(col*colorEnc.z/(col*colorEnc.x+1),g[(uint)params.y-2].x);
})";
}
bool RearMirror::InitializeOverlay() {
    ComPtr<ID3DBlob> blob,errors;
    if (FAILED(D3DCompile(mirrorCode,sizeof(mirrorCode)-1,nullptr,nullptr,nullptr,"main","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors)) ||
        FAILED(device_->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&mirrorShader_))) return false;
    D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=TRUE;depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;depth.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(device_->CreateDepthStencilState(&depth,&depthEqual_))) return false;
    for (int i=0;i<2;++i) {
        D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=96*4;buffer.Usage=D3D11_USAGE_DEFAULT;
        buffer.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;buffer.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(device_->CreateBuffer(&buffer,nullptr,&feedback_[i]))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC write{};write.Format=DXGI_FORMAT_R32_TYPELESS;write.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
        write.Buffer.NumElements=96;write.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
        D3D11_SHADER_RESOURCE_VIEW_DESC read{};read.Format=DXGI_FORMAT_R32_TYPELESS;read.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;
        read.BufferEx.NumElements=96;read.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (FAILED(device_->CreateUnorderedAccessView(feedback_[i].Get(),&write,&feedbackWrite_[i])) ||
            FAILED(device_->CreateShaderResourceView(feedback_[i].Get(),&read,&feedbackRead_[i]))) return false;
        const UINT zero[4]{};context_->ClearUnorderedAccessViewUint(feedbackWrite_[i].Get(),zero);
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC stats{};stats.Format=DXGI_FORMAT_R32_TYPELESS;stats.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;
    stats.BufferEx.NumElements=4;stats.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
    if (FAILED(device_->CreateShaderResourceView(stats_.Get(),&stats,&statsView_))) return false;
    D3D11_BUFFER_DESC constants{};constants.ByteWidth=192;constants.Usage=D3D11_USAGE_DEFAULT;constants.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device_->CreateBuffer(&constants,nullptr,&overlayConstants_));
}
bool RearMirror::ReadFeedback(std::array<int,96>& values) {
    if (!overlayReady_ || !context_) return false;
    D3D11_BUFFER_DESC desc{96*4,D3D11_USAGE_STAGING,0,D3D11_CPU_ACCESS_READ,0,0};ComPtr<ID3D11Buffer> staging;
    if (FAILED(device_->CreateBuffer(&desc,nullptr,&staging))) return false;
    context_->CopyResource(staging.Get(),feedback_[0].Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return false;
    memcpy(values.data(),mapped.pData,96*4);context_->Unmap(staging.Get(),0);return true;
}
RearMirror::OverlayResult RearMirror::DrawOverlay(ID3D11DeviceContext* context,uint64_t scene,uint64_t hash,Redraw redraw,void* opaque) {
    const auto* shader=FindOverlay(hash);
    if (!shader || overlayFailed_ || !redraw || !context || !device_ || !rearView_ || !camera_ || !seatValid_) return OverlayResult::Skipped;
    if (thread_!=GetCurrentThreadId() || context_.Get()!=context) return OverlayResult::Skipped;
    if (!overlayReady_) {
        if (!InitializeOverlay()) {overlayFailed_=true;return OverlayResult::Failed;}
        overlayReady_=true;
    }
    // The UAV goes in slot 7, after the game's render targets.
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,targets,nullptr);
    // Only single-target draws: the mirror shader writes SV_Target0 alone.
    UINT bound{};for (auto* target:targets) if (target) {++bound;target->Release();}
    if (bound!=1 || targets[7]) return OverlayResult::Skipped;
    if (overlayScene_!=scene) {
        // A new frame reads a snapshot of the mirror boxes kept so far. They
        // start again after two seconds without mirrors (menus, a new car).
        overlayScene_=scene;
        const auto now=GetTickCount64();
        if (now-lastOverlayTick_>2000) {
            const UINT zero[4]{};
            context->ClearUnorderedAccessViewUint(feedbackWrite_[0].Get(),zero);context->ClearUnorderedAccessViewUint(feedbackWrite_[1].Get(),zero);
        }
        lastOverlayTick_=now;
        context->CopyResource(feedback_[1].Get(),feedback_[0].Get());
        // Car-local aims (right, up, forward): wing mirrors about 10 degrees
        // outward, all slightly down, like mirrors adjusted from the seat.
        const float s=.17f,down=-.03f;
        const float values[48]{seat_[0],seat_[1],seat_[2],0, seat_[3],seat_[4],seat_[5],0, seat_[6],seat_[7],seat_[8],0, seat_[9],seat_[10],seat_[11],0,
            viewProjection_[0],viewProjection_[1],viewProjection_[2],viewProjection_[3],viewProjection_[4],viewProjection_[5],viewProjection_[6],viewProjection_[7],
            viewProjection_[8],viewProjection_[9],viewProjection_[10],viewProjection_[11],viewProjection_[12],viewProjection_[13],viewProjection_[14],viewProjection_[15],
            -s,down,-1,0, 0,down,-1,0, s,down,-1,0, fovScale,0,debug_?2.f:1.f,.9f};
        memcpy(overlayData_.data(),values,sizeof(values));overlayRegister_=~0u;
    }
    // params.y: the draw's MetallicSpecularValues register (GhostCarValues is two before).
    const UINT reg=shader->metallicOffset/16;
    if (reg!=overlayRegister_) {
        overlayRegister_=reg;overlayData_[45]=float(reg);
        context->UpdateSubresource(overlayConstants_.Get(),0,nullptr,overlayData_.data(),0,0);
    }
    ComPtr<ID3D11PixelShader> pixel;ID3D11ClassInstance* instances[256]{};UINT instanceCount=256;
    context->PSGetShader(&pixel,instances,&instanceCount);
    ID3D11ShaderResourceView* views[3]{};context->PSGetShaderResources(29,3,views);
    ID3D11Buffer* buffer{};context->PSGetConstantBuffers(8,1,&buffer);
    ID3D11SamplerState* samplerState{};context->PSGetSamplers(14,1,&samplerState);
    ComPtr<ID3D11DepthStencilState> depth;UINT stencilRef{};context->OMGetDepthStencilState(&depth,&stencilRef);
    ID3D11UnorderedAccessView* access{};context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,7,1,&access);

    context->PSSetShader(mirrorShader_.Get(),nullptr,0);
    ID3D11ShaderResourceView* mine[3]{statsView_.Get(),rearView_.Get(),feedbackRead_[1].Get()};
    context->PSSetShaderResources(29,3,mine);
    auto* constants=overlayConstants_.Get();context->PSSetConstantBuffers(8,1,&constants);
    auto* sampler=sampler_.Get();context->PSSetSamplers(14,1,&sampler);
    context->OMSetDepthStencilState(depthEqual_.Get(),stencilRef);
    auto* write=feedbackWrite_[0].Get();
    context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,nullptr,nullptr,7,1,&write,nullptr);
    redraw(context,opaque);

    context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,nullptr,nullptr,7,1,&access,nullptr);
    context->OMSetDepthStencilState(depth.Get(),stencilRef);
    context->PSSetSamplers(14,1,&samplerState);
    context->PSSetConstantBuffers(8,1,&buffer);
    context->PSSetShaderResources(29,3,views);
    context->PSSetShader(pixel.Get(),instances,instanceCount);
    ComPtr<ID3D11PixelShader> restored;context->PSGetShader(&restored,nullptr,nullptr);
    const bool same=restored==pixel;
    for (UINT i=0;i<instanceCount;++i) if (instances[i]) instances[i]->Release();
    for (auto* view:views) if (view) view->Release();
    if (buffer) buffer->Release();
    if (samplerState) samplerState->Release();
    if (access) access->Release();
    if (!same || FAILED(device_->GetDeviceRemovedReason())) {overlayFailed_=true;return OverlayResult::Failed;}
    ++overlays_;return OverlayResult::Drawn;
}
}
