#include "motion_blur.h"
#include <d3dcompiler.h>
using Microsoft::WRL::ComPtr;
namespace grid2vr {
bool MotionBlurControl::Initialize(ID3D11DeviceContext* context) {
    if (!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    context->GetDevice(&device_);ComPtr<ID3D11Device1> device1;
    if (FAILED(device_.As(&device1)) || FAILED(context->QueryInterface(IID_PPV_ARGS(&context_)))) return false;
    const auto level=device_->GetFeatureLevel();
    if (FAILED(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&state_))) return false;
    // Known final shaders multiply their tap displacement by cb0[0].xy.
    // Modify an owned GPU copy only, retaining the other 1288 bytes exactly.
    constexpr char code[]="RWByteAddressBuffer data:register(u0);[numthreads(1,1,1)] void main(){data.Store2(0,uint2(0,0));}";
    ComPtr<ID3DBlob> bytes;
    if (FAILED(D3DCompile(code,sizeof(code)-1,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&bytes,nullptr)) ||
        FAILED(device_->CreateComputeShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&shader_))) return false;
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=1296;desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(device_->CreateBuffer(&desc,nullptr,&raw_))) return false;
    desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;desc.MiscFlags=0;
    if (FAILED(device_->CreateBuffer(&desc,nullptr,&filtered_))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_TYPELESS;view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements=1296/4;view.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(device_->CreateUnorderedAccessView(raw_.Get(),&view,&uav_))) return false;
    thread_=GetCurrentThreadId();return true;
}
bool MotionBlurControl::Apply(ID3D11DeviceContext* context,uint64_t hash,NativeDispatch dispatch) {
    if (!KnownShader(hash) || !dispatch || original_ || !context) return false;
    if (!device_ && !Initialize(context)) return false;
    if (!thread_ || thread_!=GetCurrentThreadId() || context_.Get()!=context) return false;
    ComPtr<ID3D11Buffer> source;context->PSGetConstantBuffers(0,1,&source);
    if (!source || source==filtered_) return false;
    D3D11_BUFFER_DESC desc{};source->GetDesc(&desc);
    if (desc.ByteWidth!=1296 || desc.BindFlags!=D3D11_BIND_CONSTANT_BUFFER || desc.MiscFlags || desc.StructureByteStride) return false;
    ComPtr<ID3DDeviceContextState> previous;context_->SwapDeviceContextState(state_.Get(),&previous);
    context_->CopyResource(raw_.Get(),source.Get());
    auto* uav=uav_.Get();context_->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    context_->CSSetShader(shader_.Get(),nullptr,0);dispatch(context_.Get(),1,1,1);
    uav=nullptr;context_->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    context_->CopyResource(filtered_.Get(),raw_.Get());
    context_->SwapDeviceContextState(previous.Get(),nullptr);
    if (FAILED(device_->GetDeviceRemovedReason())) return false;
    original_=source;auto* replacement=filtered_.Get();context->PSSetConstantBuffers(0,1,&replacement);
    ComPtr<ID3D11Buffer> actual;context->PSGetConstantBuffers(0,1,&actual);
    if (actual!=filtered_) {Restore(context);return false;}
    return true;
}
bool MotionBlurControl::Restore(ID3D11DeviceContext* context) {
    if (!original_ || context_.Get()!=context || thread_!=GetCurrentThreadId()) return false;
    ComPtr<ID3D11Buffer> before;context->PSGetConstantBuffers(0,1,&before);
    auto* binding=original_.Get();context->PSSetConstantBuffers(0,1,&binding);
    ComPtr<ID3D11Buffer> after;context->PSGetConstantBuffers(0,1,&after);
    const bool restored=before==filtered_ && after==original_;original_.Reset();return restored;
}
}
