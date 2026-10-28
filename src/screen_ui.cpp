#include "screen_ui.h"
#include "resolution_limits.h"
#include <algorithm>
#include <fstream>
#include <d3dcompiler.h>
using Microsoft::WRL::ComPtr;
namespace grid2vr {
namespace {
constexpr GUID uiUpload{0x4c35d81a,0xb967,0x4d83,{0x98,0x12,0x5f,0x31,0x0d,0x9c,0xf8,0x0e}};
constexpr GUID uiReadbackAttempt{0xc56eb08a,0x986a,0x4a70,{0xb7,0xb8,0xb1,0x9a,0x9b,0x26,0x74,0x43}};
bool SupportedBuffer(const D3D11_BUFFER_DESC& desc) {
    return desc.ByteWidth==320 && desc.Usage==D3D11_USAGE_DYNAMIC && desc.CPUAccessFlags==D3D11_CPU_ACCESS_WRITE &&
        desc.BindFlags==D3D11_BIND_CONSTANT_BUFFER && !desc.MiscFlags && !desc.StructureByteStride;
}
struct ProjectionConstants {
    ScreenUiPlane plane;
    std::array<float,4> bounds{-1,1,-1,1}; // original native left/right/bottom/top
};
static_assert(sizeof(ProjectionConstants)==64);
std::string GeometrySource(unsigned mask) {
    // Exact native signatures use float4 TEXCOORDs; missing TEXCOORD3 in
    // signatures 23/55 is intentional. Retain the OSD/radial TEXCOORD5 too.
    std::string members="float4 p:SV_Position;";
    for (unsigned i=0;i<6;++i) if (mask&(1u<<i)) members+="float4 t"+std::to_string(i)+":TEXCOORD"+std::to_string(i)+";";
    std::string source="cbuffer Projection:register(b0){float4 row0;float4 row1;float4 row2;float4 bounds;}struct A{"+members+"};struct B{"+members+"float4 clip:SV_ClipDistance0;};";
    source+="[maxvertexcount(3)] void main(triangle A v[3],inout TriangleStream<B> stream){"
        // These native UI cameras put the whole canvas in front of the eye.
        // Refuse a triangle crossing native W=0 instead of dividing by zero.
        "if(v[0].p.w<=0.000001||v[1].p.w<=0.000001||v[2].p.w<=0.000001)return;"
        "[unroll]for(uint i=0;i<3;++i){B o;float4 p=v[i].p;float3 h=p.xyw;"
        "o.p=float4(dot(row0.xyz,h),dot(row1.xyz,h),0,dot(row2.xyz,h));"
        "o.p.z=p.z*o.p.w/p.w;"
        // Explicitly retain native clip bounds after changing projection.
        // Hardware clipping against the new XY/W alone admits old offscreen
        // matte quads into the smaller HUD canvas (persistent black bars).
        "o.clip=float4(p.x-bounds.x*p.w,bounds.y*p.w-p.x,p.y-bounds.z*p.w,bounds.w*p.w-p.y);";
    for (unsigned i=0;i<6;++i) if (mask&(1u<<i)) source+="o.t"+std::to_string(i)+"=v[i].t"+std::to_string(i)+";";
    return source+"stream.Append(o);}stream.RestartStrip();}";
}
}
bool FlatScreenUi::Configure(const std::filesystem::path& folder) {
    if (pending_ || width_) return false;
    std::error_code error;
    for (const auto* name:{L"headset-resolution.request",L"scene-size.request"}) {
        const auto bytes=std::filesystem::file_size(folder/name,error);
        if (error || !bytes || bytes>64) return false;
    }
    resolution::Plan request;
    std::ifstream headset(folder/L"headset-resolution.request"),scene(folder/L"scene-size.request");
    unsigned width{},height{};char extra{};
    if (!resolution::Read(headset,request) || !(scene>>width>>height) || (scene>>extra) ||
        width!=request.SceneWidth() || height!=request.SceneHeight()) return false;
    width_=width;height_=height;return true;
}
bool FlatScreenUi::Apply(ID3D11DeviceContext* context,uint64_t shader) {
    if (!Enabled() || pending_ || !context || !ScreenUiWitnessShader(shader) ||
        context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        (owner_ && (owner_!=context || thread_!=GetCurrentThreadId()))) return false;
    std::array<ID3D11RenderTargetView*,8> rawTargets{};context->OMGetRenderTargets(UINT(rawTargets.size()),rawTargets.data(),nullptr);
    std::array<ComPtr<ID3D11RenderTargetView>,8> targets;
    for (unsigned i=0;i<targets.size();++i) targets[i].Attach(rawTargets[i]);
    if (std::any_of(targets.begin()+1,targets.end(),[](const auto& target){return bool(target);})) return false;
    const auto& target=targets[0];
    ComPtr<ID3D11Resource> targetResource;if (target) target->GetResource(&targetResource);
    ComPtr<ID3D11Texture2D> texture;D3D11_TEXTURE2D_DESC td{};
    if (!targetResource || FAILED(targetResource.As(&texture))) return false;
    texture->GetDesc(&td);
    UINT count=1;D3D11_VIEWPORT viewport{};context->RSGetViewports(&count,&viewport);
    if (td.Width!=width_ || td.Height!=height_ || td.Format!=DXGI_FORMAT_R10G10B10A2_UNORM ||
        td.ArraySize!=1 || td.MipLevels!=1 || td.SampleDesc.Count!=1 || count!=1 || viewport.TopLeftX || viewport.TopLeftY ||
        viewport.Width!=float(width_) || viewport.Height!=float(height_)) return false;
    ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
    context->GSGetShader(&gs,nullptr,nullptr);context->HSGetShader(&hs,nullptr,nullptr);context->DSGetShader(&ds,nullptr,nullptr);
    if (gs || hs || ds) return false;
    ComPtr<ID3D11Buffer> source;context->VSGetConstantBuffers(3,1,&source);
    D3D11_BUFFER_DESC bd{};if (source) source->GetDesc(&bd);
    if (!source || !SupportedBuffer(bd)) return false;
    std::array<uint8_t,320> native{},corrected{};UINT bytes=UINT(native.size());
    ComPtr<ID3D11Device> device;context->GetDevice(&device);
    if (FAILED(source->GetPrivateData(uiUpload,&bytes,native.data())) || bytes!=native.size()) {
        // Cameras made before the first Present need one bounded seed. Native
        // dynamic uploads replace this private copy before every later draw.
        if (seedAttempts_>=32) return false;
        ++seedAttempts_;
        auto sd=bd;sd.Usage=D3D11_USAGE_STAGING;sd.BindFlags=0;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;if (FAILED(device->CreateBuffer(&sd,nullptr,&staging))) return false;
        ComPtr<ID3D11Predicate> predicate;BOOL value{};context->GetPredication(&predicate,&value);
        context->SetPredication(nullptr,FALSE);context->CopyResource(staging.Get(),source.Get());context->SetPredication(predicate.Get(),value);
        D3D11_MAPPED_SUBRESOURCE mapped{};if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return false;
        memcpy(native.data(),mapped.pData,native.size());context->Unmap(staging.Get(),0);
        ScreenUi::Upload(source.Get(),native.data(),UINT(native.size()));
    }
    if (!FlatScreenUiCamera(native,width_,height_,corrected) || native==corrected) return false;
    if (!replacement_) {
        if (FAILED(device->CreateBuffer(&bd,nullptr,&replacement_))) return false;
        owner_=context;thread_=GetCurrentThreadId();
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(replacement_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) return false;
    memcpy(mapped.pData,corrected.data(),corrected.size());context->Unmap(replacement_.Get(),0);
    changedScissors_=false;
    ComPtr<ID3D11RasterizerState> raster;D3D11_RASTERIZER_DESC rd{};context->RSGetState(&raster);if(raster)raster->GetDesc(&rd);
    if (rd.ScissorEnable) {
        scissorCount_=UINT(scissors_.size());context->RSGetScissorRects(&scissorCount_,scissors_.data());
        if (!scissorCount_) return false;
        for (UINT i=0;i<scissorCount_;++i) {
            const auto& r=scissors_[i];
            if (r.left<0 || r.top<0 || r.right>LONG(width_) || r.bottom>LONG(height_)) return false;
        }
        auto transformed=scissors_;
        float oldX{},oldY{},newX{},newY{};memcpy(&oldX,native.data(),4);memcpy(&oldY,native.data()+20,4);
        memcpy(&newX,corrected.data(),4);memcpy(&newY,corrected.data()+20,4);
        for (UINT i=0;i<scissorCount_;++i) {
            auto& r=transformed[i];
            // Full-axis clipping is the viewport, not a list/control clip.
            // Widen it so formerly offscreen menu geometry can re-enter.
            if (r.left>0) r.left=LONG(std::floor((r.left-width_*.5)*newX/oldX+width_*.5));
            if (r.right<LONG(width_)) r.right=LONG(std::ceil((r.right-width_*.5)*newX/oldX+width_*.5));
            if (r.top>0) r.top=LONG(std::floor((r.top-height_*.5)*newY/oldY+height_*.5));
            if (r.bottom<LONG(height_)) r.bottom=LONG(std::ceil((r.bottom-height_*.5)*newY/oldY+height_*.5));
            if (scissors_[i].right<=scissors_[i].left) r.right=r.left;
            if (scissors_[i].bottom<=scissors_[i].top) r.bottom=r.top;
        }
        context->RSSetScissorRects(scissorCount_,transformed.data());changedScissors_=true;
    }
    originalVs_=source;context->PSGetConstantBuffers(3,1,&originalPs_);changedPs_=originalPs_==source;
    auto* binding=replacement_.Get();context->VSSetConstantBuffers(3,1,&binding);
    if (changedPs_) context->PSSetConstantBuffers(3,1,&binding);
    pending_=true;return true;
}
bool FlatScreenUi::Restore(ID3D11DeviceContext* context) {
    if (!pending_ || context!=owner_ || thread_!=GetCurrentThreadId()) return false;
    auto* binding=originalVs_.Get();context->VSSetConstantBuffers(3,1,&binding);
    if (changedPs_) {binding=originalPs_.Get();context->PSSetConstantBuffers(3,1,&binding);}
    if (changedScissors_) context->RSSetScissorRects(scissorCount_,scissors_.data());
    ComPtr<ID3D11Buffer> vs,ps;context->VSGetConstantBuffers(3,1,&vs);context->PSGetConstantBuffers(3,1,&ps);
    bool restored=vs==originalVs_ && ps==originalPs_;
    if (changedScissors_) {
        auto actual=scissors_;UINT count=UINT(actual.size());context->RSGetScissorRects(&count,actual.data());
        restored=restored && count==scissorCount_ && !memcmp(actual.data(),scissors_.data(),count*sizeof(D3D11_RECT));
    }
    originalVs_.Reset();originalPs_.Reset();pending_=changedPs_=changedScissors_=false;return restored;
}
bool FlatScreenUi::Reset() {
    if (pending_ || (owner_ && thread_!=GetCurrentThreadId())) return false;
    replacement_.Reset();originalVs_.Reset();originalPs_.Reset();owner_=nullptr;thread_=0;
    width_=height_=seedAttempts_=0;return true;
}
void ScreenUi::Diagnose(const std::filesystem::path& folder,bool late) {
    output_=folder;options_={};invalidOptions_=false;late_=late;
    std::ifstream request(folder/L"vr-hud.request");
    const bool supplied=std::filesystem::exists(folder/L"vr-hud.request");
    if (supplied) invalidOptions_=!ReadScreenUiOptions(request,options_);
    std::ofstream receipt(folder/L"screen-ui-options.json");
    receipt << "{\"schema\":1,\"projection\":\"post_vs_native_clip\",\"requestPresent\":" << (supplied?"true":"false")
        << ",\"requestValid\":" << (invalidOptions_?"false":"true") << ",\"distanceMetres\":" << options_.distanceMetres
        << ",\"followView\":" << (options_.followView?"true":"false") << ",\"hiddenMask\":" << options_.hiddenMask
        << ",\"maskApplied\":false,\"scope\":\"supported_ui_including_menus_and_results\"}\n";
}
void ScreenUi::Upload(ID3D11Resource* resource,const void* data,UINT bytes) {
    if (!resource || !data || bytes!=320) return;
    // Replace on every native upload, including when a resource is reused for
    // a different camera. Only ProjectScreenUi decides whether it is supported.
    resource->SetPrivateData(uiUpload,bytes,data);
}
void ScreenUi::Observe(ID3D11DeviceContext* context,uint64_t shader,uint64_t frame,uintptr_t nativeTarget) {
    if (!context || output_.empty() || !ScreenUiWitnessShader(shader) ||
        context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        (owner_ && (owner_!=context || thread_!=GetCurrentThreadId()))) return;
    ComPtr<ID3D11Buffer> buffer;context->VSGetConstantBuffers(3,1,&buffer);
    Record(context,buffer.Get(),shader,frame,nativeTarget);
}
void ScreenUi::Record(ID3D11DeviceContext* context,ID3D11Buffer* buffer,uint64_t shader,uint64_t frame,uintptr_t nativeTarget) {
    if (!buffer || samples_.size()>=64 || (late_ && !nativeTarget)) return;
    D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);
    if (desc.ByteWidth!=320 && desc.ByteWidth!=368) return;
    UINT viewCount=1;D3D11_VIEWPORT viewport{};context->RSGetViewports(&viewCount,&viewport);
    const bool validViewport=viewCount==1 && viewport.TopLeftX==0 && viewport.TopLeftY==0 &&
        std::isfinite(viewport.Width) && std::isfinite(viewport.Height) && viewport.Width>=1 && viewport.Height>=1 &&
        viewport.Width<=8192 && viewport.Height<=8192 && std::floor(viewport.Width)==viewport.Width && std::floor(viewport.Height)==viewport.Height;
    const unsigned width=validViewport?unsigned(viewport.Width):0,height=validViewport?unsigned(viewport.Height):0;
    std::array<uint8_t,368> data{};UINT bytes=desc.ByteWidth;
    bool uploaded=SUCCEEDED(buffer->GetPrivateData(uiUpload,&bytes,data.data())) && bytes==desc.ByteWidth;
    if (!uploaded) {
        // A UI camera can be created before native hooks are armed and never
        // uploaded again. Seed only an exact supported camera, once per native
        // resource. Later tracked Map/Unmap uploads replace that private copy.
        // Repeated early non-font draws cannot exhaust the late-font witness.
        UINT attempted{},attemptBytes=sizeof(attempted);
        if (SUCCEEDED(buffer->GetPrivateData(uiReadbackAttempt,&attemptBytes,&attempted)) || sampleAttempts_>=32) return;
        ++sampleAttempts_;attempted=1;buffer->SetPrivateData(uiReadbackAttempt,sizeof(attempted),&attempted);
        auto sd=desc;sd.Usage=D3D11_USAGE_STAGING;sd.BindFlags=sd.MiscFlags=sd.StructureByteStride=0;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Device> device;context->GetDevice(&device);ComPtr<ID3D11Buffer> staging;
        if (FAILED(device->CreateBuffer(&sd,nullptr,&staging))) return;
        ComPtr<ID3D11Predicate> predicate;BOOL value{};context->GetPredication(&predicate,&value);
        context->SetPredication(nullptr,FALSE);context->CopyResource(staging.Get(),buffer);context->SetPredication(predicate.Get(),value);
        D3D11_MAPPED_SUBRESOURCE mapped{};if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped))) return;
        memcpy(data.data(),mapped.pData,desc.ByteWidth);context->Unmap(staging.Get(),0);bytes=desc.ByteWidth;
        std::array<float,2> scale{};
        if (SupportedBuffer(desc) && ScreenUiCanvasIdentity(std::span(data).first(bytes),width,height,scale)) Upload(buffer,data.data(),bytes);
    }
    std::array<float,2> scale{};
    const auto identity=ScreenUiCanvasIdentity(std::span(data).first(bytes),width,height,scale);
    if (output_.empty() || (!nativeTarget && samples_.contains({shader,identity,false}))) return;
    ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11DepthStencilView> depth;context->OMGetRenderTargets(1,&target,&depth);
    ComPtr<ID3D11Resource> resource;if (target) target->GetResource(&resource);
    const bool targetMatches=nativeTarget && reinterpret_cast<uintptr_t>(resource.Get())==nativeTarget;
    if (!samples_.insert({shader,identity,targetMatches}).second || output_.empty()) return;
    const auto hash=ScreenUiHash(std::span(data).first(bytes));
    char name[24]{};sprintf_s(name,"%016llx",hash);
    constexpr char digits[]="0123456789abcdef";std::string hex;hex.reserve(bytes*2);
    for (UINT i=0;i<bytes;++i) { hex+=digits[data[i]>>4];hex+=digits[data[i]&15]; }
    std::ofstream log(output_/L"screen-ui-cameras.jsonl",std::ios::app);
    ComPtr<ID3D11Texture2D> texture;D3D11_TEXTURE2D_DESC td{};if (resource && SUCCEEDED(resource.As(&texture))) texture->GetDesc(&td);
    log << "{\"fnv64\":\"" << name << "\",\"dataHex\":\"" << hex << "\",\"bytes\":" << bytes << ",\"usage\":" << desc.Usage << ",\"cpuAccess\":" << desc.CPUAccessFlags
        << ",\"bindFlags\":" << desc.BindFlags << ",\"uploaded\":" << (uploaded?"true":"false")
        << ",\"supportedCamera\":" << (identity?"true":"false") << ",\"cameraIdentity\":" << identity
        << ",\"vertexShader\":" << shader << ",\"projectionShader\":" << (ScreenUiVertexShader(shader)?"true":"false")
        << ",\"phase\":\"" << (late_?"after_scene":"scene_replay") << "\",\"nativeFrame\":" << frame
        << ",\"viewport\":[" << viewport.TopLeftX << ',' << viewport.TopLeftY << ',' << viewport.Width << ',' << viewport.Height << ']'
        << ",\"viewportCount\":" << viewCount << ",\"target\":[" << td.Width << ',' << td.Height << ',' << td.Format << ',' << td.SampleDesc.Count << ']'
        << ",\"matchesCompletedNativeTarget\":" << (targetMatches?"true":"false")
        << ",\"depthTarget\":" << (depth?"true":"false") << "}\n";
}
ScreenUi::Result ScreenUi::Apply(ID3D11DeviceContext* context,uint64_t vertexShader,const SceneViews& views,unsigned eye) {
    if (!context || Pending() || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        (owner_ && (owner_!=context || thread_!=GetCurrentThreadId()))) return Result::Failed;
    if (invalidOptions_ || !ScreenUiVertexShader(vertexShader)) return Result::Skipped;
    ProjectionConstants constants;
    if (!ScreenUiPlaneProjection(views,eye,options_,constants.plane)) return Result::Skipped;
    D3D11_PRIMITIVE_TOPOLOGY topology{};context->IAGetPrimitiveTopology(&topology);
    if (topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) return Result::Skipped;
    ComPtr<ID3D11Buffer> source;context->VSGetConstantBuffers(3,1,&source);
    Record(context,source.Get(),vertexShader);
    D3D11_BUFFER_DESC desc{};if (source) source->GetDesc(&desc);
    if (!source || !SupportedBuffer(desc)) return Result::Skipped;
    std::array<uint8_t,320> native{};UINT bytes=UINT(native.size());
    if (FAILED(source->GetPrivateData(uiUpload,&bytes,native.data())) || bytes!=native.size()) return Result::Skipped;
    // Only verified vertex/pixel programs with no later native stages.
    ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
    context->GSGetShader(&gs,nullptr,nullptr);context->HSGetShader(&hs,nullptr,nullptr);context->DSGetShader(&ds,nullptr,nullptr);
    if (gs || hs || ds) return Result::Skipped;
    UINT count=1;D3D11_VIEWPORT vp{};context->RSGetViewports(&count,&vp);
    if (count!=1 || !std::isfinite(vp.Width) || !std::isfinite(vp.Height) || vp.Width<=0 || vp.Height<=0 ||
        vp.Width>8192 || vp.Height>8192 || vp.TopLeftX!=0 || vp.TopLeftY!=0 ||
        std::floor(vp.Width)!=vp.Width || std::floor(vp.Height)!=vp.Height) return Result::Skipped;
    std::array<float,2> canvas{};
    if (!ScreenUiCanvasIdentity(native,unsigned(vp.Width),unsigned(vp.Height),canvas)) return Result::Skipped;
    for (auto& row:constants.plane.rows) {row[0]*=canvas[0];row[1]*=canvas[1];}
    // The full logical 16:9 canvas may extend outside portrait native clip XY.
    // Recover that extent before hardware clipping without changing the native
    // shader's UVs. Partial control scissors retain their original coordinates.
    constants.bounds={-1/canvas[0],1/canvas[0],-1/canvas[1],1/canvas[1]};
    ComPtr<ID3D11RasterizerState> raster;context->RSGetState(&raster);D3D11_RASTERIZER_DESC rd{};
    if (raster) raster->GetDesc(&rd);
    changedScissors_=false;
    if (rd.ScissorEnable) {
        scissorCount_=UINT(scissors_.size());context->RSGetScissorRects(&scissorCount_,scissors_.data());
        // The verified UI has one viewport; native scissor[0] is the only one
        // that applies. Carry it in native clip coordinates, including empty
        // rectangles, so a rotated fixed HUD keeps exactly the same clipping.
        if (!scissorCount_) return Result::Skipped;
        const auto& r=scissors_[0];
        if (r.left>0) constants.bounds[0]=std::max(constants.bounds[0],float(2.*r.left/vp.Width-1));
        if (r.right<vp.Width) constants.bounds[1]=std::min(constants.bounds[1],float(2.*r.right/vp.Width-1));
        if (r.bottom<vp.Height) constants.bounds[2]=std::max(constants.bounds[2],float(1-2.*r.bottom/vp.Height));
        if (r.top>0) constants.bounds[3]=std::min(constants.bounds[3],float(1-2.*r.top/vp.Height));
        if (r.right<=r.left) constants.bounds[1]=constants.bounds[0];
        if (r.bottom<=r.top) constants.bounds[3]=constants.bounds[2];
    }
    ComPtr<ID3D11Device> device;context->GetDevice(&device);
    if (!replacement_) {
        D3D11_BUFFER_DESC cd{};cd.ByteWidth=sizeof(constants);cd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        cd.Usage=D3D11_USAGE_DYNAMIC;cd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&cd,nullptr,&replacement_))) return Result::Failed;
        owner_=context;thread_=GetCurrentThreadId();
    }
    const unsigned mask=ScreenUiVaryingMask(vertexShader),index=((mask&8)?1:0)+((mask&32)?2:0);
    if (!shaders_[index]) {
        const auto code=GeometrySource(mask);ComPtr<ID3DBlob> blob,error;
        if (FAILED(D3DCompile(code.data(),code.size(),"grid2vr-screen-ui",nullptr,nullptr,"main","gs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error)) ||
            FAILED(device->CreateGeometryShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&shaders_[index]))) return Result::Failed;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(replacement_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) return Result::Failed;
    memcpy(mapped.pData,&constants,sizeof(constants));context->Unmap(replacement_.Get(),0);
    context->GSGetConstantBuffers(0,1,&original_);
    if (rd.ScissorEnable) {
        // GS handles the original scissor before projection. Leave rasterizer,
        // MSAA and depth/stencil state native, and widen only its rectangle.
        const D3D11_RECT full{0,0,LONG(std::ceil(vp.Width)),LONG(std::ceil(vp.Height))};
        context->RSSetScissorRects(1,&full);changedScissors_=true;
    }
    pending_=true;auto* binding=replacement_.Get();context->GSSetConstantBuffers(0,1,&binding);
    context->GSSetShader(shaders_[index].Get(),nullptr,0);
    ComPtr<ID3D11Buffer> actual;context->GSGetConstantBuffers(0,1,&actual);
    ComPtr<ID3D11GeometryShader> actualShader;context->GSGetShader(&actualShader,nullptr,nullptr);
    if (actual.Get()!=binding || actualShader!=shaders_[index]) { Restore(context);return Result::Failed; }
    return Result::Applied;
}
bool ScreenUi::Restore(ID3D11DeviceContext* context) {
    if (!Pending() || owner_!=context || thread_!=GetCurrentThreadId()) return false;
    context->GSSetShader(nullptr,nullptr,0);
    auto* binding=original_.Get();context->GSSetConstantBuffers(0,1,&binding);
    if (changedScissors_) context->RSSetScissorRects(scissorCount_,scissors_.data());
    ComPtr<ID3D11Buffer> actual;context->GSGetConstantBuffers(0,1,&actual);
    ComPtr<ID3D11GeometryShader> actualShader;context->GSGetShader(&actualShader,nullptr,nullptr);
    bool restored=actual==original_ && !actualShader;
    if (changedScissors_) {
        auto check=scissors_;UINT count=UINT(check.size());context->RSGetScissorRects(&count,check.data());
        restored&=count==scissorCount_ && !memcmp(check.data(),scissors_.data(),count*sizeof(D3D11_RECT));
    }
    original_.Reset();pending_=changedScissors_=false;return restored;
}

void ScreenUiNativeWitness::Diagnose(const std::filesystem::path& folder) {
    output_=folder;ui_.Diagnose(folder,true);
}
bool ScreenUiNativeWitness::Begin(ID3D11DeviceContext* context,uint64_t frame,uintptr_t nativeTarget) {
    if (!context || !frame || !nativeTarget || frame_ || frame<=lastFrame_ || output_.empty() ||
        context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        (context_ && (context_!=context || thread_!=GetCurrentThreadId()))) return false;
    context_=context;thread_=GetCurrentThreadId();nativeTarget_=nativeTarget;frame_=lastFrame_=frame;++frames_;return true;
}
void ScreenUiNativeWitness::Observe(ID3D11DeviceContext* context,uint64_t frame,uint64_t shader) {
    if (!frame_ || frame!=frame_ || context!=context_ || thread_!=GetCurrentThreadId()) return;
    const auto index=ScreenUiWitnessIndex(shader);if (index>=counts_.size()) return;
    if (!counts_[index]++) changed_=true;
    ui_.Observe(context,shader,frame,nativeTarget_);
}
void ScreenUiNativeWitness::End(bool force) {
    if (!context_ || thread_!=GetCurrentThreadId()) return;
    if (!output_.empty() && (force || frames_==1 || frames_>=published_+120 || changed_)) {
        std::ofstream out(output_/L"screen-ui-native.json",std::ios::trunc);
        out << "{\"schema\":1,\"scope\":\"normal_after_scene_observation_only\",\"lastFrame\":" << lastFrame_
            << ",\"completedSceneFrames\":" << frames_ << ",\"draws\":[";
        for (size_t i=0;i<counts_.size();++i) {
            const auto shader=i<screenUiShaders.size()?screenUiShaders[i]:screenUiObservedShaders[i-screenUiShaders.size()];
            char name[24]{};sprintf_s(name,"%016llx",shader);if (i) out << ',';
            out << "{\"vertexShader\":\"" << name << "\",\"count\":" << counts_[i]
                << ",\"projectionShader\":" << (i<screenUiShaders.size()?"true":"false") << '}';
        }
        out << "],\"limits\":\"Native counts only. Numeric target identity is sampled in the same frame; no texture or XR frame is retained. No draw is projected or duplicated by this observer.\"}\n";
        published_=frames_;changed_=false;
    }
    frame_=0;nativeTarget_=0;
}

void LateScreenUi::Diagnose(const std::filesystem::path& folder) {
    output_=folder;ui_.Diagnose(folder,true);
}
bool LateScreenUi::Begin(ID3D11DeviceContext* context,uint64_t frame,const SceneViews& views,
    ID3D11Texture2D* native,ID3D11Texture2D* left,ID3D11Texture2D* right) {
    if (frame_ || !frame || frame<=lastFrame_ || !context || !native || !left || !right || left==right ||
        native==left || native==right || !ValidSceneViews(views) ||
        context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        (context_ && (context_!=context || thread_!=GetCurrentThreadId()))) return false;
    ComPtr<ID3D11Device> device;context->GetDevice(&device);
    D3D11_TEXTURE2D_DESC source{};native->GetDesc(&source);
    if (!source.Width || !source.Height || source.Width>8192 || source.Height>8192 || source.ArraySize!=1 ||
        source.MipLevels!=1 || source.SampleDesc.Count!=1 || source.SampleDesc.Quality ||
        !(source.BindFlags&D3D11_BIND_RENDER_TARGET)) return false;
    const std::array<ID3D11Texture2D*,2> input{left,right};
    for (unsigned eye=0;eye<2;++eye) {
        D3D11_TEXTURE2D_DESC d{};input[eye]->GetDesc(&d);ComPtr<ID3D11Device> owner;input[eye]->GetDevice(&owner);
        if (owner!=device || d.Width!=source.Width || d.Height!=source.Height || d.Format!=source.Format ||
            d.ArraySize!=1 || d.MipLevels!=1 || d.SampleDesc.Count!=1 || d.SampleDesc.Quality ||
            !(d.BindFlags&D3D11_BIND_RENDER_TARGET)) return false;
        if (eyes_[eye].Get()!=input[eye]) {
            targets_[eye].Reset();
            if (FAILED(device->CreateRenderTargetView(input[eye],nullptr,&targets_[eye]))) return false;
            eyes_[eye]=input[eye];
        }
    }
    ComPtr<ID3D11Device> nativeDevice;native->GetDevice(&nativeDevice);if (nativeDevice!=device) return false;
    context_=context;thread_=GetCurrentThreadId();frame_=lastFrame_=frame;views_=views;native_=native;++prepared_;return true;
}
ScreenUi::Result LateScreenUi::Draw(ID3D11DeviceContext* context,uint64_t frame,uint64_t shader,NativeDraw draw,void* args) {
    using Result=ScreenUi::Result;
    if (!frame_ || frame!=frame_ || context!=context_ || thread_!=GetCurrentThreadId() || !draw) return Result::Skipped;
    ui_.Observe(context,shader,frame,reinterpret_cast<uintptr_t>(native_.Get()));
    if (!ScreenUiVertexShader(shader)) return Result::Skipped;
    const auto skip=[&](unsigned reason) {++skipped_[reason];reasons_|=1u<<reason;return Result::Skipped;};
    // A same-sized offscreen target is not the native final scene. Require the
    // exact resource and a single output; never duplicate postprocessing.
    std::array<ID3D11RenderTargetView*,8> raw{};ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(UINT(raw.size()),raw.data(),&depth);
    std::array<ComPtr<ID3D11RenderTargetView>,8> original;
    for (unsigned i=0;i<raw.size();++i) original[i].Attach(raw[i]);
    ComPtr<ID3D11Resource> resource;if (original[0]) original[0]->GetResource(&resource);
    if (resource.Get()!=native_.Get() || std::any_of(original.begin()+1,original.end(),[](const auto& value){return bool(value);})) return skip(0);
    D3D11_TEXTURE2D_DESC source{};native_->GetDesc(&source);
    UINT count=1;D3D11_VIEWPORT viewport{};context->RSGetViewports(&count,&viewport);
    if (count!=1 || viewport.TopLeftX || viewport.TopLeftY || viewport.Width!=float(source.Width) || viewport.Height!=float(source.Height)) return skip(0);
    ComPtr<ID3D11DepthStencilState> ds;context->OMGetDepthStencilState(&ds,nullptr);
    D3D11_DEPTH_STENCIL_DESC dd{};if (ds) ds->GetDesc(&dd);
    // NULL is D3D's depth-enabled default. With no DSV there is no depth or
    // stencil test; otherwise require both explicitly disabled.
    if (depth && (!ds || dd.DepthEnable || dd.StencilEnable)) return skip(1);
    std::array<ID3D11UnorderedAccessView*,8> uavs{};
    context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,UINT(uavs.size()),uavs.data());
    bool sideEffects=false;for (auto* value:uavs) if (value) {sideEffects=true;value->Release();}
    std::array<ID3D11Buffer*,4> so{};context->SOGetTargets(UINT(so.size()),so.data());
    for (auto* value:so) if (value) {sideEffects=true;value->Release();}
    if (sideEffects) return skip(2);
    ComPtr<ID3D11Predicate> predicate;BOOL predicateValue{};context->GetPredication(&predicate,&predicateValue);
    if (predicate) return skip(3);
    UINT sampleMask{};context->OMGetBlendState(nullptr,nullptr,&sampleMask);if (sampleMask!=~0u) return skip(4);
    for (unsigned eye=0;eye<2;++eye) {
        const auto projected=ui_.Apply(context,shader,views_,eye);
        if (projected!=Result::Applied) {
            if (!eye && projected==Result::Skipped) return skip(5);
            ++failed_;return Result::Failed;
        }
        auto* target=targets_[eye].Get();context->OMSetRenderTargets(1,&target,nullptr);
        draw(context,args);
        // Restore the exact native target/depth bindings before restoring the
        // projected geometry shader and native scissor. Native sample counts,
        // viewport, blend state, fonts, buffers and game draws are untouched.
        context->OMSetRenderTargets(UINT(raw.size()),raw.data(),depth.Get());
        if (!ui_.Restore(context)) {++failed_;return Result::Failed;}
    }
    ++pairedDraws_;return Result::Applied;
}
void LateScreenUi::End(bool forceReceipt) {
    if (context_ && !OnThread()) return;
    if (!output_.empty() && (forceReceipt || prepared_==1 || prepared_>=published_+120 || reasons_!=publishedReasons_)) {
        std::ofstream out(output_/L"screen-ui-late.json",std::ios::trunc);
        out << "{\"schema\":1,\"scope\":\"normal_launch_known_ui_after_scene\",\"lastFrame\":" << lastFrame_
            << ",\"preparedFrames\":" << prepared_ << ",\"pairedDraws\":" << pairedDraws_ << ",\"failed\":" << failed_ << ",\"skipped\":[";
        for (unsigned i=0;i<skipped_.size();++i) {if (i) out << ',';out << skipped_[i];}
        out << "],\"skipOrder\":[\"target\",\"depth_stencil\",\"uav_stream_output\",\"predication\",\"sample_mask\",\"camera_state\"],"
            "\"limits\":\"Known shader and camera coverage only; headset visibility requires acceptance.\"}\n";
        published_=prepared_;publishedReasons_=reasons_;
    }
    frame_=0;native_.Reset();
}
void LateScreenUi::Reset() { if (context_ && !OnThread()) return;End(true);targets_={};eyes_={}; }
}
