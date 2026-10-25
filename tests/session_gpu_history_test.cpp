#include "engine_trace.h"
#include "replay_trace.h"
#include "texture_digest.h"
#include <d3dcompiler.h>
#include <fstream>
#include <thread>
using Microsoft::WRL::ComPtr;
namespace grid2vr::engine {
SceneStamp testScene{1,false,0};
SceneStamp CurrentScene() { return testScene; }
bool CurrentCockpitCamera() { return false; }
uint64_t NativeCameraFrame() { return testScene.id; }
const SceneViews* CurrentReplayViews() { return nullptr; }
StageStamp CurrentStage() { return {}; }
bool RepeatingEyes() { return true; }
bool DualEyes() { return true; }
bool ObservingEyes() { return true; }
unsigned EyePairCount() { return 8; } // Also exercise a bounded non-runtime probe.
}
std::string Digest(ID3D11DeviceContext* context,ID3D11Texture2D* texture) {
    uint64_t budget=4096;
    auto digest=grid2vr::DigestTexture(context,texture,budget);
    return digest.subresources.size()==1?digest.subresources[0].sha256:std::string{};
}
int main(int argc,char** argv) {
    if (argc!=2 && argc!=3) return 1;
    const std::string scenario=argc==3?argv[2]:"";
    const auto root=scenario.empty()?std::filesystem::path(argv[1]):std::filesystem::path(argv[1])/scenario;
    std::filesystem::create_directories(root);
    for (const auto* name:{L"runtime-session.request",L"compact-stereo.request",L"stereo-eyes.request",L"restore-compute-state.request",L"disable-motion-blur.request",L"isolate-shadow-sampler.request"})
        std::ofstream(root/name) << "fixture\n";
    // Ordinary headset sessions must enable UI correction without the former
    // diagnostic-only marker. These files do not create an OpenXR session.
    std::filesystem::remove(root/L"screen-ui-stereo.request");
    for (const auto* name:{L"xr-session.request",L"xr-projection.request"}) std::ofstream(root/name) << "fixture\n";
    std::filesystem::remove(root/L"shadow-default-replay.json");
    std::filesystem::remove(root/L"shadow-absent-replay.json");
    std::filesystem::remove(root/L"normal-launch.request");
    std::filesystem::remove(root/L"normalize-shadow-bindings.request");
    if (!scenario.empty()) std::ofstream(root/L"normalize-shadow-bindings.request") << "fixture\n";
    if (scenario.starts_with("normal-")) std::ofstream(root/L"normal-launch.request") << "fixture\n";
    // The marker alone must not enable the omission outside a runtime session.
    if (scenario=="normal-offline-zero") std::filesystem::remove(root/L"runtime-session.request");
    auto module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    if (!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))) return 2;
    const char code[]="RWTexture2D<float2> output:register(u0);[numthreads(1,1,1)]void main(uint3 id:SV_DispatchThreadID){output[id.xy]=0;}";
    ComPtr<ID3DBlob> bytes;ComPtr<ID3D11ComputeShader> shader;
    if (FAILED(D3DCompile(code,sizeof(code),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&bytes,nullptr)) ||
        FAILED(device->CreateComputeShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&shader))) return 3;
    // This fixture tests resource/history ownership, not the game's shader code.
    grid2vr::replaytrace::ShaderIdentity(shader.Get(),0x3edaf831f6f23893ull);
    context->CSSetShader(shader.Get(),nullptr,0);
    // Exercise the production shadow sampler capture/replay/restore path with
    // a valid quad pipeline. Shader tags stand in for the native identities;
    // this checks state ownership, not the game's shadow pixels.
    const char vsSource[]="float4 main(float4 p:POSITION):SV_Position{return p;}";
    const char psSource[]="float4 main():SV_Target{return 1;}";
    ComPtr<ID3DBlob> vsCode,psCode;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11InputLayout> layout;
    const D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    if (FAILED(D3DCompile(vsSource,sizeof(vsSource),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vsCode,nullptr)) ||
        FAILED(D3DCompile(psSource,sizeof(psSource),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&psCode,nullptr)) ||
        FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs)) ||
        FAILED(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps)) ||
        FAILED(device->CreateInputLayout(&element,1,vsCode->GetBufferPointer(),vsCode->GetBufferSize(),&layout))) return 12;
    grid2vr::replaytrace::ShaderIdentity(vs.Get(),0x4b2b0f44adfbf5bbull);
    grid2vr::replaytrace::ShaderIdentity(ps.Get(),0x158f4162b8000a3full);
    context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
    D3D11_TEXTURE2D_DESC color{};color.Width=8;color.Height=4;color.ArraySize=color.MipLevels=color.SampleDesc.Count=1;
    color.Format=DXGI_FORMAT_R8G8B8A8_UNORM;color.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> colorTexture;ComPtr<ID3D11RenderTargetView> target;
    D3D11_BUFFER_DESC vertexDesc{};vertexDesc.ByteWidth=64;vertexDesc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    ComPtr<ID3D11Buffer> vertices;
    D3D11_SAMPLER_DESC samplerDesc{};samplerDesc.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU=samplerDesc.AddressV=samplerDesc.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MaxAnisotropy=1;samplerDesc.ComparisonFunc=D3D11_COMPARISON_ALWAYS;samplerDesc.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> explicitSampler;
    if (FAILED(device->CreateTexture2D(&color,nullptr,&colorTexture)) || FAILED(device->CreateRenderTargetView(colorTexture.Get(),nullptr,&target)) ||
        FAILED(device->CreateBuffer(&vertexDesc,nullptr,&vertices)) || FAILED(device->CreateSamplerState(&samplerDesc,&explicitSampler))) return 13;
    auto* rtv=target.Get();context->OMSetRenderTargets(1,&rtv,nullptr);
    auto* vb=vertices.Get();const UINT stride=16,offset=0;context->IASetVertexBuffers(0,1,&vb,&stride,&offset);
    context->IASetInputLayout(layout.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    const D3D11_VIEWPORT viewport{0,0,8,4,0,1};context->RSSetViewports(1,&viewport);
    grid2vr::replaytrace::Configure(root,true);
    if (grid2vr::replaytrace::ScreenUiEnabled()!=(scenario!="normal-offline-zero")) return 22;
    if (!grid2vr::replaytrace::ResetHistory()) return 4;
    bool wrongThreadReset=true;
    std::thread worker([&]{wrongThreadReset=grid2vr::replaytrace::ResetHistory();});worker.join();
    if (wrongThreadReset) return 5;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=8;desc.Height=4;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R8G8_UNORM;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    if (!scenario.empty()) {
        struct Case { const char* name;std::array<unsigned,3> draws;int failurePass; };
        constexpr Case cases[]{
            {"normal-transitions",{0,0,0},-1},
            {"strict-zero",{0,0,0},0}, {"normal-offline-zero",{0,0,0},0},
            {"normal-extra-left",{0,1,0},1}, {"normal-extra-right",{0,0,1},2},
            {"normal-missing-left",{1,0,1},1}, {"normal-missing-right",{1,1,0},2},
            {"normal-duplicate-center",{2,1,1},0}, {"normal-duplicate-eye",{1,2,1},1},
            {"normal-pending-center",{1,1,1},0}, {"normal-pending-eye",{1,1,1},1},
            {"normal-wrong-target",{1,1,1},1},
            {"normal-no-ao",{0,0,0},0}, {"normal-short-compute",{0,0,0},1}
        };
        const Case* selected{};
        for (const auto& item:cases) if (scenario==item.name) selected=&item;
        if (!selected) return 30;
        ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11UnorderedAccessView> uav;
        if (FAILED(device->CreateTexture2D(&desc,nullptr,&texture)) ||
            FAILED(device->CreateUnorderedAccessView(texture.Get(),nullptr,&uav))) return 31;
        auto* binding=uav.Get();const UINT preserve=UINT(-1);context->CSSetUnorderedAccessViews(0,1,&binding,&preserve);
        std::array<uint16_t,32> pixels;pixels.fill(0x5a5a);
        context->UpdateSubresource(texture.Get(),0,nullptr,pixels.data(),16,0);
        const auto original=Digest(context.Get(),texture.Get());if (original.empty()) return 32;
        // Reuse the same history across 1 -> 0 -> 1 -> 0 -> 1. Both explicit
        // and null/default samplers must survive entry into an absent pass.
        const bool transitions=selected->failurePass<0;
        for (unsigned frame=0;frame<(transitions?5u:1u);++frame) {
            auto& scene=grid2vr::engine::testScene;scene={2000+frame,false,0};
            const auto counts=transitions && frame%2==0?std::array<unsigned,3>{1,1,1}:selected->draws;
            auto* centerSampler=frame<2?explicitSampler.Get():nullptr;
            grid2vr::replaytrace::SceneBoundary(context.Get(),"scene-entry");
            for (unsigned pass=0;pass<3;++pass) {
                scene.pass=pass;scene.replay=pass!=0;
                if (pass) grid2vr::replaytrace::SceneBoundary(context.Get(),"replay-entry");
                unsigned dispatches=scenario=="normal-no-ao"?0:4;
                if (scenario=="normal-short-compute" && pass==1) dispatches=3;
                for (unsigned i=0;i<dispatches;++i) {
                    const UINT args[]{1,1,1};auto mark=grid2vr::replaytrace::Work(context.Get(),"Compute",args);
                    grid2vr::replaytrace::BeforeCompute(context.Get(),mark);
                }
                auto* before=pass?(centerSampler?nullptr:explicitSampler.Get()):centerSampler;
                context->PSSetSamplers(6,1,&before);
                if (scenario=="normal-wrong-target" && pass==1) context->OMSetRenderTargets(0,nullptr,nullptr);
                for (unsigned draw=0;draw<counts[pass];++draw) {
                    const UINT args[]{4,0};grid2vr::replaytrace::Work(context.Get(),"Draw",args);
                    if (!(scenario=="normal-pending-eye" && pass==1) &&
                        !(scenario=="normal-pending-center" && pass==0)) grid2vr::replaytrace::AfterDraw(context.Get());
                }
                if (transitions) {
                    ComPtr<ID3D11SamplerState> actual;context->PSGetSamplers(6,1,&actual);
                    if (actual.Get()!=before) return 33; // Zero path must not touch s6.
                }
                if (pass) {
                    pixels.fill(uint16_t(pass*900));context->UpdateSubresource(texture.Get(),0,nullptr,pixels.data(),16,0);
                    grid2vr::replaytrace::SceneBoundary(context.Get(),"replay-complete");
                    grid2vr::replaytrace::SceneBoundary(context.Get(),"replay-restored");
                } else grid2vr::replaytrace::SceneBoundary(context.Get(),"scene-complete");
                if (int(pass)==selected->failurePass) {
                    // Failure must remain latched, including after restoration.
                    return !grid2vr::replaytrace::PairHealthy() && !grid2vr::replaytrace::ResetHistory()?0:34;
                }
                if (!grid2vr::replaytrace::PairHealthy() || (pass && Digest(context.Get(),texture.Get())!=original)) return 35;
            }
        }
        std::ifstream proof(root/L"shadow-absent-replay.json");
        const std::string receipt((std::istreambuf_iterator<char>(proof)),std::istreambuf_iterator<char>());
        if (receipt!="{\"scene\":2001,\"nativeFrame\":2001,\"pairs\":1,\"normalRuntime\":true,\"draws\":[0,0,0],\"pending\":false,\"pairGpuHealthy\":true}\n") return 36;
        return grid2vr::replaytrace::ResetHistory()?0:37;
    }
    for (unsigned epoch=1;epoch<=3;++epoch) {
        ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11UnorderedAccessView> uav;
        if (FAILED(device->CreateTexture2D(&desc,nullptr,&texture)) || FAILED(device->CreateUnorderedAccessView(texture.Get(),nullptr,&uav))) return 6;
        auto* binding=uav.Get();const UINT preserve=UINT(-1);context->CSSetUnorderedAccessViews(0,1,&binding,&preserve);
        std::array<uint16_t,32> pixels;pixels.fill(uint16_t(epoch*100));context->UpdateSubresource(texture.Get(),0,nullptr,pixels.data(),16,0);
        const auto original=Digest(context.Get(),texture.Get());if (original.empty()) return 7;
        auto& scene=grid2vr::engine::testScene;scene={epoch*100ull,false,0};
        // Event/garage loading can draw a center scene without acquiring eye
        // views. Cancel that scope, preserving native pixels/bindings, then
        // prove that fresh stereo history can be built on the next scene.
        scene.id-=1;
        grid2vr::replaytrace::SceneBoundary(context.Get(),"scene-entry");
        if (grid2vr::replaytrace::ResetHistory() ||
            !grid2vr::replaytrace::CancelBeforeReplay() || !grid2vr::replaytrace::ResetHistory()) return 23;
        ComPtr<ID3D11UnorderedAccessView> retained;
        context->CSGetUnorderedAccessViews(0,1,&retained);
        if (retained!=uav || Digest(context.Get(),texture.Get())!=original) return 24;
        ++scene.id;
        auto* centerSampler=epoch%2?nullptr:explicitSampler.Get();
        grid2vr::replaytrace::SceneBoundary(context.Get(),"scene-entry");
        if (grid2vr::replaytrace::ResetHistory()) return 8; // Must preserve pending restoration.
        auto work=[&] {
            for (unsigned i=0;i<4;++i) {
                const UINT args[]{1,1,1};auto mark=grid2vr::replaytrace::Work(context.Get(),"Compute",args);
                grid2vr::replaytrace::BeforeCompute(context.Get(),mark);
            }
            auto* before=scene.replay?(centerSampler?nullptr:explicitSampler.Get()):centerSampler;
            context->PSSetSamplers(6,1,&before);
            const UINT args[]{4,0};grid2vr::replaytrace::Work(context.Get(),"Draw",args);
            ComPtr<ID3D11SamplerState> actual;context->PSGetSamplers(6,1,&actual);
            if (actual.Get()!=centerSampler) return false;
            grid2vr::replaytrace::AfterDraw(context.Get());
            actual.Reset();context->PSGetSamplers(6,1,&actual);
            return actual.Get()==before && grid2vr::replaytrace::PairHealthy();
        };
        // This native center/eye sequence omits blur, as observed after the
        // SteamVR overlay. AO and compute ownership must still be restored.
        if (!work()) return 14;
        grid2vr::replaytrace::SceneBoundary(context.Get(),"scene-complete");
        for (unsigned eye=1;eye<=2;++eye) {
            scene.replay=true;scene.pass=eye;
            grid2vr::replaytrace::SceneBoundary(context.Get(),"replay-entry");if (!work()) return 15;
            pixels.fill(uint16_t(eye*900));context->UpdateSubresource(texture.Get(),0,nullptr,pixels.data(),16,0);
            grid2vr::replaytrace::SceneBoundary(context.Get(),"replay-complete");
            grid2vr::replaytrace::SceneBoundary(context.Get(),"replay-restored");
            if (!grid2vr::replaytrace::PairHealthy() || Digest(context.Get(),texture.Get())!=original) return 9;
        }
        if (!grid2vr::replaytrace::ResetHistory()) return 10;
        // A new native AO resource is legal only after the explicit reset.
    }
    std::ifstream proof(root/L"shadow-default-replay.json");
    const std::string receipt((std::istreambuf_iterator<char>(proof)),std::istreambuf_iterator<char>());
    if (receipt!="{\"scene\":100,\"eyes\":2,\"centerSampler\":null,\"samplerRestored\":true,\"pairGpuHealthy\":true}\n") return 16;
    auto& scene=grid2vr::engine::testScene;scene={999,false,0};
    grid2vr::replaytrace::SceneBoundary(context.Get(),"scene-entry");
    grid2vr::replaytrace::SceneBoundary(context.Get(),"scene-complete"); // No valid producer.
    if (grid2vr::replaytrace::PairHealthy() || grid2vr::replaytrace::ResetHistory()) return 11;
    // A proved menu may cancel a center-only scope without race passes; no
    // eye draw or binding mutation was begun. Later stereo starts fresh.
    if (!grid2vr::replaytrace::CancelBeforeReplay() || !grid2vr::replaytrace::ResetHistory() ||
        grid2vr::replaytrace::CancelBeforeReplay()) return 17;
    scene={1000,false,0};grid2vr::replaytrace::SceneBoundary(context.Get(),"scene-entry");
    scene.replay=true;scene.pass=1;
    grid2vr::replaytrace::SceneBoundary(context.Get(),"replay-entry");
    if (grid2vr::replaytrace::CancelBeforeReplay()) return 18;
    scene.replay=false;scene.pass=0;
    if (grid2vr::replaytrace::CancelBeforeReplay() || grid2vr::replaytrace::ResetHistory()) return 19;
    return 0;
}
