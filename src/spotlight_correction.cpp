#include "spotlight_correction.h"
#include <d3dcompiler.h>
#include <cstring>

namespace grid2vr {
namespace {
using Microsoft::WRL::ComPtr;
constexpr char shaderCode[]=R"(
cbuffer EyePixel:register(b0){uint4 eyePixel[PIXEL_ROWS];};
cbuffer CenterPixel:register(b1){uint4 centerPixel[PIXEL_ROWS];};
cbuffer EyeVertex:register(b2){float4 eyeVertex[12];};
cbuffer CenterVertex:register(b3){float4 centerVertex[12];};
cbuffer Parameters:register(b4){float4 T[4];float4 Q[4];uint4 layout;};
RWByteAddressBuffer output:register(u0);
RWByteAddressBuffer status:register(u1);
float4 RowTimes(float4 v,float4 a,float4 b,float4 c,float4 d){return v.x*a+v.y*b+v.z*c+v.w*d;}
[numthreads(1,1,1)] void main(){
    uint reason=0;
    for(uint i=0;i<layout.x;++i)if(any(eyePixel[i]!=centerPixel[i]))reason=1;
    for(uint r=0;r<4;++r){
        float4 expected=RowTimes(centerVertex[8+r],T[0],T[1],T[2],T[3]);
        float4 actual=eyeVertex[8+r];
        if(!all(isfinite(expected)) || !all(isfinite(actual)) ||
           any(abs(expected-actual)>.001+abs(expected)*.000002))if(reason==0)reason=2;
    }
    float3 position=RowTimes(float4(asfloat(centerPixel[2].xyz),1),T[0],T[1],T[2],T[3]).xyz;
    float3 direction=RowTimes(float4(asfloat(centerPixel[3].xyz),0),T[0],T[1],T[2],T[3]).xyz;
    float length2=dot(direction,direction);
    if(!all(isfinite(position)) || !all(isfinite(direction)) || !isfinite(length2) || length2<1e-12)
        if(reason==0)reason=3;
    direction*=rsqrt(length2);
    // Validate every transformed matrix before changing any output bytes.
    for(uint m=0;m<(layout.z?2:1);++m){
        uint start=m?9:layout.y;
        for(uint r=0;r<4;++r){
            float4 row=RowTimes(Q[r],asfloat(centerPixel[start]),asfloat(centerPixel[start+1]),
                asfloat(centerPixel[start+2]),asfloat(centerPixel[start+3]));
            if(!all(isfinite(row)))if(reason==0)reason=3;
        }
    }
    for(uint i=0;i<layout.x;++i)output.Store4(i*16,eyePixel[i]);
    if(reason==0){
        output.Store3(32,asuint(position));output.Store3(48,asuint(direction));
        for(uint m=0;m<(layout.z?2:1);++m){
            uint start=m?9:layout.y;
            for(uint r=0;r<4;++r)output.Store4((start+r)*16,asuint(RowTimes(Q[r],
                asfloat(centerPixel[start]),asfloat(centerPixel[start+1]),
                asfloat(centerPixel[start+2]),asfloat(centerPixel[start+3]))));
        }
    }
    status.Store4(0,uint4(reason,0,0,0));
})";
bool ExactBuffer(ID3D11Buffer* buffer,UINT bytes) {
    if(!buffer)return false;
    D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);
    return desc.ByteWidth==bytes && desc.BindFlags==D3D11_BIND_CONSTANT_BUFFER &&
        !desc.MiscFlags && !desc.StructureByteStride;
}
bool RawBuffer(ID3D11Device* device,UINT bytes,ComPtr<ID3D11Buffer>& buffer,
               ComPtr<ID3D11UnorderedAccessView>& view) {
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=bytes;desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if(FAILED(device->CreateBuffer(&desc,nullptr,&buffer)))return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC vd{};vd.Format=DXGI_FORMAT_R32_TYPELESS;
    vd.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;vd.Buffer.NumElements=bytes/4;
    vd.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
    return SUCCEEDED(device->CreateUnorderedAccessView(buffer.Get(),&vd,&view));
}
}
bool SpotlightCorrection::Buffer(Ptr<ID3D11Buffer>& buffer,UINT bytes) {
    if(ExactBuffer(buffer.Get(),bytes))return true;
    buffer.Reset();D3D11_BUFFER_DESC desc{};desc.ByteWidth=bytes;
    desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device_->CreateBuffer(&desc,nullptr,&buffer));
}
bool SpotlightCorrection::Initialize(ID3D11DeviceContext* context) {
    if(!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    context->GetDevice(&device_);ComPtr<ID3D11Device1> device1;
    if(FAILED(device_.As(&device1)) || FAILED(context->QueryInterface(IID_PPV_ARGS(&context_))))return false;
    const auto level=device_->GetFeatureLevel();
    if(FAILED(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&state_)))return false;
    for(unsigned i=0;i<output_.size();++i) {
        ComPtr<ID3DBlob> code;
        const D3D_SHADER_MACRO defines[]{{"PIXEL_ROWS",i?"29":"27"},{nullptr,nullptr}};
        if(FAILED(D3DCompile(shaderCode,sizeof(shaderCode)-1,nullptr,defines,nullptr,"main","cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS,0,&code,nullptr)) ||
            FAILED(device_->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shaders_[i])))return false;
        auto& out=output_[i];const UINT bytes=i?464:432;
        if(!RawBuffer(device_.Get(),bytes,out.raw,out.view) || !Buffer(out.constant,bytes))return false;
    }
    if(!Buffer(parameters_,144) || !RawBuffer(device_.Get(),16,status_,statusView_))return false;
    owner_=context;thread_=GetCurrentThreadId();return true;
}
bool SpotlightCorrection::BeginScene(uint64_t scene) {
    if(Pending() || (owner_ && thread_!=GetCurrentThreadId()))return false;
    PollSamples();
    if(scene_!=scene){scene_=scene;count_=0;sampleMask_=0;if(scene)++sceneCount_;}
    return true;
}
void SpotlightCorrection::PollSamples() {
    if(!context_)return;
    for(auto& sample:samples_)if(sample.pending) {
        BOOL ready=FALSE;
        if(context_->GetData(sample.ready.Get(),&ready,sizeof(ready),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK || !ready)continue;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(context_->Map(sample.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped)!=S_OK)continue;
        uint32_t reason{};memcpy(&reason,mapped.pData,sizeof(reason));context_->Unmap(sample.staging.Get(),0);
        ++statistics_.samples;
        if(reason==0)++statistics_.corrected;
        else if(reason==1)++statistics_.pixelMismatch;
        else if(reason==2)++statistics_.vertexMismatch;
        else ++statistics_.invalidConstants;
        sample.pending=false;
    }
}
void SpotlightCorrection::SampleGuard(unsigned pass) {
    // At most two samples in each selected scene, 32 total. No draw waits for
    // GPU completion; full staging slots simply defer the next opportunity.
    if(samplesIssued_>=MaxSamples || (sampleMask_&(1u<<pass)) ||
        (samplesIssued_>=16 && sceneCount_%120))return;
    for(auto& sample:samples_)if(!sample.pending) {
        if(!sample.staging) {
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=16;desc.Usage=D3D11_USAGE_STAGING;
            desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};
            if(FAILED(device_->CreateBuffer(&desc,nullptr,&sample.staging)) ||
                FAILED(device_->CreateQuery(&query,&sample.ready))){sample.staging.Reset();sample.ready.Reset();return;}
        }
        context_->CopyResource(sample.staging.Get(),status_.Get());context_->End(sample.ready.Get());
        sample.pending=true;sampleMask_|=1u<<pass;++samplesIssued_;return;
    }
}
SpotlightCorrection::Result SpotlightCorrection::BeforeDraw(ID3D11DeviceContext* context,const DrawKey& key,
    unsigned pass,const spotlightprojection::Change* change) {
    const auto layout=spotlightprojection::PixelLayout(key.vsHash,key.psHash);
    if(!layout.bytes || !scene_ || pass>2 || !dispatch_ || key.argCount>key.args.size())return Result::Skipped;
    if(Pending() || !context)return Result::Failed;
    if(!owner_ && !Initialize(context)){ResetResources();return Result::Failed;}
    if(owner_!=context || thread_!=GetCurrentThreadId())return Result::Failed;
    ComPtr<ID3D11Buffer> pixel,vertex;UINT pixelFirst{},pixelCount{},vertexFirst{},vertexCount{};
    context_->PSGetConstantBuffers1(0,1,&pixel,&pixelFirst,&pixelCount);
    context_->VSGetConstantBuffers1(0,1,&vertex,&vertexFirst,&vertexCount);
    if(!ExactBuffer(pixel.Get(),UINT(layout.bytes)) || !ExactBuffer(vertex.Get(),192) ||
        pixelFirst || vertexFirst || pixelCount<layout.bytes/16 || vertexCount<12)return Result::Skipped;
    if(pass==0) {
        for(unsigned i=0;i<count_;++i)if(snapshots_[i].key==key)return Result::Skipped;
        if(count_==MaxSnapshots){++statistics_.overflow;return Result::Skipped;}
        auto& snapshot=snapshots_[count_];
        if(!Buffer(snapshot.pixel,UINT(layout.bytes)) || !Buffer(snapshot.vertex,192))return Result::Failed;
        ComPtr<ID3DDeviceContextState> previous;context_->SwapDeviceContextState(state_.Get(),&previous);
        context_->SetPredication(nullptr,FALSE);
        context_->CopyResource(snapshot.pixel.Get(),pixel.Get());context_->CopyResource(snapshot.vertex.Get(),vertex.Get());
        context_->SwapDeviceContextState(previous.Get(),nullptr);
        if(FAILED(device_->GetDeviceRemovedReason()))return Result::Failed;
        snapshot.key=key;++count_;++statistics_.captures;return Result::Captured;
    }
    if(!change || !change->valid || !spotlightprojection::detail::Affine(change->centerToEye) ||
        !spotlightprojection::detail::Affine(change->eyeToCenter))return Result::Skipped;
    Snapshot* snapshot=nullptr;
    for(unsigned i=0;i<count_;++i)if(snapshots_[i].key==key){snapshot=&snapshots_[i];break;}
    if(!snapshot){++statistics_.missingCenter;return Result::Skipped;}
    struct Parameters {float t[16],q[16];uint32_t layout[4];} parameters{};
    for(unsigned i=0;i<16;++i){
        parameters.t[i]=float(change->centerToEye[i]);parameters.q[i]=float(change->eyeToCenter[i]);
        if(!std::isfinite(parameters.t[i]) || !std::isfinite(parameters.q[i]))return Result::Skipped;
    }
    parameters.layout[0]=UINT(layout.bytes/16);parameters.layout[1]=UINT(layout.mapping/16);parameters.layout[2]=layout.shadow;
    activeOutput_=layout.bytes==464;auto& out=output_[activeOutput_];
    ComPtr<ID3DDeviceContextState> previous;context_->SwapDeviceContextState(state_.Get(),&previous);
    context_->SetPredication(nullptr,FALSE);
    context_->UpdateSubresource(parameters_.Get(),0,nullptr,&parameters,0,0);
    ID3D11Buffer* inputs[]{pixel.Get(),snapshot->pixel.Get(),vertex.Get(),snapshot->vertex.Get(),parameters_.Get()};
    context_->CSSetConstantBuffers(0,5,inputs);
    ID3D11UnorderedAccessView* views[]{out.view.Get(),statusView_.Get()};
    context_->CSSetUnorderedAccessViews(0,2,views,nullptr);context_->CSSetShader(shaders_[activeOutput_].Get(),nullptr,0);
    dispatch_(owner_,1,1,1);
    views[0]=views[1]=nullptr;context_->CSSetUnorderedAccessViews(0,2,views,nullptr);
    context_->CopyResource(out.constant.Get(),out.raw.Get());SampleGuard(pass);
    // Retain no native resource references in the private context-state object.
    ID3D11Buffer* empty[5]{};context_->CSSetConstantBuffers(0,5,empty);
    context_->SwapDeviceContextState(previous.Get(),nullptr);
    if(FAILED(device_->GetDeviceRemovedReason()))return Result::Failed;
    original_=pixel;originalFirst_=pixelFirst;originalCount_=pixelCount;
    auto* replacement=out.constant.Get();context->PSSetConstantBuffers(0,1,&replacement);
    ComPtr<ID3D11Buffer> actual;context->PSGetConstantBuffers(0,1,&actual);
    if(actual!=out.constant){AfterDraw(context);return Result::Failed;}
    ++statistics_.submissions;return Result::Applied;
}
bool SpotlightCorrection::AfterDraw(ID3D11DeviceContext* context) {
    if(!Pending())return true;
    if(context!=owner_ || thread_!=GetCurrentThreadId())return false;
    ComPtr<ID3D11Buffer> before,after;context->PSGetConstantBuffers(0,1,&before);
    auto* buffer=original_.Get();context_->PSSetConstantBuffers1(0,1,&buffer,&originalFirst_,&originalCount_);
    UINT first{},count{};context_->PSGetConstantBuffers1(0,1,&after,&first,&count);
    const bool restored=before==output_[activeOutput_].constant && after==original_ &&
        first==originalFirst_ && count==originalCount_;
    original_.Reset();return restored;
}
bool SpotlightCorrection::ResetResources() {
    if(Pending())return false;
    if(owner_ && thread_!=GetCurrentThreadId())return false;
    PollSamples();
    for(auto& snapshot:snapshots_)snapshot={};
    for(auto& output:output_)output={};
    for(auto& sample:samples_)sample={};
    parameters_.Reset();status_.Reset();statusView_.Reset();for(auto& shader:shaders_)shader.Reset();state_.Reset();context_.Reset();device_.Reset();
    owner_=nullptr;thread_=0;scene_=0;count_=0;sampleMask_=0;return true;
}
}
