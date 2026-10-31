#include "replay_trace.h"
#include "engine_trace.h"
#include "guarded_hooks.h"
#include "model_snapshot.h"
#include "texture_digest.h"
#include "shadow_pass.h"
#include "motion_blur.h"
#include "impact_interference.h"
#include "cockpit_exposure.h"
#include "draw_witness.h"
#include "flicker_capture.h"
#include "bounded_log.h"
#include "screen_ui.h"
#include "spotlight_witness.h"
#include "spotlight_correction.h"
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <fstream>
#include <set>
#include <map>

namespace grid2vr::replaytrace {
namespace {
using Microsoft::WRL::ComPtr;
bool enabled{};
bool compact{};
bool runtimeSession{},sceneOpen{},eyeReplayStarted{};
bool disableMotionBlur{};
bool screenUiEnabled{};
bool screenUiNormalDefault{};
ScreenUi screenUi;
SpotlightCorrection spotlightCorrection;
std::filesystem::path output;
bool spotlightEnabled{};
uint64_t spotlightCaptured{},spotlightScene{},spotlightProofPair{};
std::array<uint64_t,2> spotlightSubmitted{},spotlightRestored{},spotlightSkipped{};
void WriteSpotlightProof() {
    if (!spotlightEnabled) return;
    const auto& stats=spotlightCorrection.Stats();
    std::ofstream proof(output/L"spotlight-correction.json");
    proof << "{\"scene\":" << spotlightScene << ",\"captured\":" << spotlightCaptured
        << ",\"submitted\":[" << spotlightSubmitted[0] << ',' << spotlightSubmitted[1]
        << "],\"restored\":[" << spotlightRestored[0] << ',' << spotlightRestored[1]
        << "],\"skipped\":[" << spotlightSkipped[0] << ',' << spotlightSkipped[1]
        << "],\"sampledGpuGuard\":{\"samples\":" << stats.samples << ",\"corrected\":" << stats.corrected
        << ",\"pixelMismatch\":" << stats.pixelMismatch << ",\"vertexMismatch\":" << stats.vertexMismatch
        << ",\"invalidConstants\":" << stats.invalidConstants << "},\"missingCenter\":" << stats.missingCenter
        << ",\"overflow\":" << stats.overflow
        << ",\"gpuGuardRequired\":true,\"nativeLightHook\":false,\"headsetAcceptance\":false}\n";
}
std::array<uint64_t,2> uiApplied{},uiRestored{},uiSkipped{},uiText{};
struct UiShaderCounts {
    std::array<uint64_t,2> applied{},restored{},skipped{};
    uint64_t firstScene{},firstNativeFrame{},firstAppliedScene{},firstAppliedNativeFrame{};
};
std::array<UiShaderCounts,screenUiShaders.size()> uiShaderCounts{};
size_t uiPendingShader=screenUiShaders.size();
uint64_t uiScene{},uiNativeFrame{},uiPairs{},uiProofPairs{};
bool uiProofChanged{};
NativeDispatch nativeDispatch{};
MotionBlurControl motionBlur;
std::array<unsigned,3> blurDraws{};
bool limitCockpitExposure{};
CockpitExposure cockpitExposure;
std::array<unsigned,3> exposureDraws{};
bool removeImpactInterference{};
ImpactInterference impactInterference;
std::array<unsigned,3> impactDraws{};
bool impactInterferenceLogged{};
CenterDrawWitness centerDraws;
bool centerFailureDumped{},defaultShadowProofSaved{};
bool isolateModels{};
bool restoreComputeState{};
bool sortVpl{};
bool isolateShadowSampler{};
bool allowAbsentShadow{};
bool normalizeShadowBindings{};
ComPtr<ID3D11SamplerState> restoreShadowSampler;
ID3D11DeviceContext* restoreShadowContext{};
unsigned shadowOrdinal{};
ShadowPassIdentity shadowIdentity;
std::array<unsigned,3> shadowDraws{};
struct UnusedShadowBindings {
    std::array<ComPtr<ID3D11ShaderResourceView>,123> resources;
    std::array<ComPtr<ID3D11SamplerState>,9> samplers;
    void Read(ID3D11DeviceContext* c) {
        ID3D11ShaderResourceView* views[123]{}; c->PSGetShaderResources(5,123,views);
        ID3D11SamplerState* states[9]{}; c->PSGetSamplers(7,9,states);
        for (size_t i=0;i<resources.size();++i) resources[i].Attach(views[i]);
        for (size_t i=0;i<samplers.size();++i) samplers[i].Attach(states[i]);
    }
    bool Apply(ID3D11DeviceContext* c) const {
        ID3D11ShaderResourceView* views[123]{};
        ID3D11SamplerState* states[9]{};
        for (size_t i=0;i<resources.size();++i) views[i]=resources[i].Get();
        for (size_t i=0;i<samplers.size();++i) states[i]=samplers[i].Get();
        c->PSSetShaderResources(5,123,views); c->PSSetSamplers(7,9,states);
        UnusedShadowBindings actual; actual.Read(c);
        for (size_t i=0;i<resources.size();++i) if (actual.resources[i]!=resources[i]) return false;
        for (size_t i=0;i<samplers.size();++i) if (actual.samplers[i]!=samplers[i]) return false;
        return true;
    }
};
UnusedShadowBindings restoreUnusedShadow;
ID3D11DeviceContext* restoreUnusedContext{};
unsigned unusedOrdinal{};
bool ShadowPassComplete(unsigned pass) {
    if (pass>=shadowDraws.size() || restoreShadowContext || restoreUnusedContext) return false;
    const auto center=shadowDraws[0];
    // Native runtime frames, including low graphics presets, may omit this
    // exact pass. Both eyes follow the center schedule; never reuse a preceding
    // scene's capture or confuse an absent draw with D3D's null sampler.
    return (center==1 || (allowAbsentShadow && center==0)) && shadowDraws[pass]==center &&
        (shadowOrdinal!=0)==(center==1) && bool(shadowIdentity.target)==(center==1);
}
uint64_t absentShadowPairs{},lastAbsentShadowScene{};
VplOrder vplOrder;
std::map<unsigned,ModelDrawSnapshot> modelSnapshots;
ComPtr<ID3D11Buffer> restoreModelBuffer;
ID3D11DeviceContext* restoreModelContext{};
unsigned restoreModelOrdinal{};
uint64_t firstScene{};
uint64_t activeScene{},workScene{};
uint64_t pairNumber{};
unsigned workOrdinal[3]{},slot6Ordinal[3]{},preColorOrdinal[3]{},computeOrdinal[3]{};
constexpr uint64_t lightCullShader=0x6987ff5aba64e635ull;
struct ComputeDispatch {uint64_t shader{};std::array<UINT,3> groups{};unsigned slot{};bool operator==(const ComputeDispatch&) const=default;};
std::array<std::array<ComputeDispatch,8>,3> computeSchedule{};
std::array<bool,3> lightCullSorted{};
std::array<ComputeDispatch,8> lastComputeSchedule{};
unsigned lastComputeCount=UINT_MAX,computeScheduleReceipts{};
bool NoAoSchedule(unsigned pass) {
    return runtimeSession && pass<3 && (computeOrdinal[pass]==0 ||
        (computeOrdinal[pass]==1 && computeSchedule[pass][0].shader==lightCullShader && lightCullSorted[pass]));
}
void RecordComputeSchedule(unsigned pass,bool healthy) {
    if (!runtimeSession || pass>=3 || (healthy && (computeScheduleReceipts>=32 ||
        (lastComputeCount==computeOrdinal[pass] && lastComputeSchedule==computeSchedule[pass])))) return;
    // Reserve an overwritten failure receipt even after the transition budget.
    std::ofstream proof(output/(healthy?L"compute-schedules.jsonl":L"compute-failure.json"),
        healthy && computeScheduleReceipts++?std::ios::app:std::ios::trunc);
    proof << "{\"scene\":" << engine::CurrentScene().id << ",\"pass\":" << pass << ",\"healthy\":" << (healthy?"true":"false")
        << ",\"count\":" << computeOrdinal[pass] << ",\"dispatches\":[";
    for (unsigned i=0;i<std::min(computeOrdinal[pass],8u);++i) {
        const auto& dispatch=computeSchedule[pass][i];
        if(i)proof<<',';
        proof<<"{\"shader\":\""<<std::hex<<dispatch.shader<<std::dec<<"\",\"slot\":"<<dispatch.slot
            <<",\"groups\":["<<dispatch.groups[0]<<','<<dispatch.groups[1]<<','<<dispatch.groups[2]<<"]}";
    }
    proof << "]}\n";lastComputeCount=computeOrdinal[pass];lastComputeSchedule=computeSchedule[pass];
}
bool pairHealthy=true,aoPrepared{};
DWORD ownerThread{};
BoundedLog log;
std::set<uint64_t> saved;
std::set<std::pair<unsigned,uint64_t>> sources;
struct ComputeInputs {
    ComPtr<ID3D11Buffer> counter;
    std::array<ComPtr<ID3D11SamplerState>,16> samplers;
    void Read(ID3D11DeviceContext* c) {
        counter.Reset(); c->CSGetConstantBuffers(12,1,&counter);
        ID3D11SamplerState* pointers[16]{}; c->CSGetSamplers(0,16,pointers);
        for (size_t i=0;i<samplers.size();++i) samplers[i].Attach(pointers[i]);
    }
    bool Apply(ID3D11DeviceContext* c) const {
        auto* buffer=counter.Get(); c->CSSetConstantBuffers(12,1,&buffer);
        ID3D11SamplerState* pointers[16]{};
        for (size_t i=0;i<samplers.size();++i) pointers[i]=samplers[i].Get();
        c->CSSetSamplers(0,16,pointers);
        ComputeInputs actual; actual.Read(c);
        bool match=actual.counter.Get()==counter.Get();
        for (size_t i=0;i<samplers.size();++i) match=match && actual.samplers[i].Get()==samplers[i].Get();
        return match;
    }
};
ComputeInputs incomingCompute,completedCompute;
bool computeBeforeSaved{},computeAfterSaved{};
ComPtr<ID3D11Texture2D> aoResource,aoIncoming,aoCompleted;
struct AoEyeHistory {
    ComPtr<ID3D11Texture2D> texture;
    bool saved{};
    uint64_t digestPair{};
    std::string digest;
};
std::array<AoEyeHistory,2> aoEyes;
AoEyeHistory& CurrentAoEye() { return aoEyes[engine::CurrentScene().pass==2?1:0]; }
uint64_t aoAuditBudget=128ull*1024*1024;
bool AuditAo() { return !runtimeSession && engine::RepeatingEyes() && (pairNumber<=2 || pairNumber+1>=engine::EyePairCount()); }
std::string AoDigest(ID3D11DeviceContext* context,ID3D11Texture2D* texture) {
    const auto digest=DigestTexture(context,texture,aoAuditBudget);
    return !strcmp(digest.status,"ok") && digest.subresources.size()==1 ? digest.subresources[0].sha256:std::string{};
}
bool aoIncomingSaved{},aoCompletedSaved{};
void ClearAoHistory() {
    aoEyes={};aoResource.Reset();aoIncoming.Reset();aoCompleted.Reset();
    aoIncomingSaved=aoCompletedSaved=aoPrepared=false;
}
bool CopyScratch(ID3D11DeviceContext* c,ID3D11Texture2D* destination,ID3D11Texture2D* source) {
    ID3D11UnorderedAccessView* bound[8]{}; c->CSGetUnorderedAccessViews(0,8,bound);
    ID3D11UnorderedAccessView* empty[8]{};
    const UINT preserve[8]{UINT(-1),UINT(-1),UINT(-1),UINT(-1),UINT(-1),UINT(-1),UINT(-1),UINT(-1)};
    c->CSSetUnorderedAccessViews(0,8,empty,preserve);
    c->CopyResource(destination,source);
    c->CSSetUnorderedAccessViews(0,8,bound,preserve);
    for (auto* view:bound) if (view) view->Release();
    ComPtr<ID3D11Device> device; c->GetDevice(&device);
    return SUCCEEDED(device->GetDeviceRemovedReason());
}
size_t savedBytes{};
// Follows the resource's COM lifetime, rather than caching a recycled pointer.
constexpr GUID uploadId{0x27a4fbc8,0xe860,0x4df8,{0xac,0x41,0x1c,0x2e,0x1c,0xae,0xc1,0x89}};
constexpr GUID shaderId{0x725d739f,0x9be8,0x4ba0,{0x95,0x91,0xd3,0x8d,0x50,0x01,0x46,0x08}};
struct UploadInfo { uint64_t hash{}; UINT bytes{}; bool saved{}; };
uint64_t Hash(const void* data,size_t size) {
    uint64_t hash=14695981039346656037ull;
    for (size_t i=0;i<size;++i) { hash^=static_cast<const uint8_t*>(data)[i]; hash*=1099511628211ull; }
    return hash;
}
std::string Hex(uint64_t value) { char text[24]{}; sprintf_s(text,"%016llx",value); return text; }
void WriteUiProof(const char* phase) {
    if (!screenUiEnabled) return;
    std::ofstream proof(output/L"screen-ui-stereo.json");
    proof << "{\"scene\":" << uiScene << ",\"nativeFrame\":" << uiNativeFrame << ",\"phase\":\"" << phase
        << "\",\"normalVrDefault\":" << (screenUiNormalDefault?"true":"false")
        << ",\"scope\":\"verified_shaders_and_recorded_native_camera_variants_only\",\"applied\":[" << uiApplied[0] << ',' << uiApplied[1]
        << "],\"restored\":[" << uiRestored[0] << ',' << uiRestored[1] << "],\"skipped\":[" << uiSkipped[0] << ',' << uiSkipped[1]
        << "],\"distanceMaskDraws\":[" << uiText[0] << ',' << uiText[1] << "],\"pending\":" << (screenUi.Pending()?"true":"false")
        << ",\"shaders\":[";
    for (size_t i=0;i<uiShaderCounts.size();++i) {
        if (i) proof << ',';
        const auto& count=uiShaderCounts[i];
        proof << "{\"vertexShader\":\"" << Hex(screenUiShaders[i]) << "\",\"firstScene\":" << count.firstScene
            << ",\"firstNativeFrame\":" << count.firstNativeFrame << ",\"firstAppliedScene\":" << count.firstAppliedScene
            << ",\"firstAppliedNativeFrame\":" << count.firstAppliedNativeFrame
            << ",\"applied\":[" << count.applied[0] << ',' << count.applied[1]
            << "],\"restored\":[" << count.restored[0] << ',' << count.restored[1]
            << "],\"skipped\":[" << count.skipped[0] << ',' << count.skipped[1] << "]}";
    }
    proof << "],\"resultsScreenIdentified\":false,\"limits\":\"Shader and camera witnesses do not identify text content or establish headset readability.\"}\n";
    uiProofPairs=uiPairs;uiProofChanged=false;
}
void DumpCenterDraws(const CenterDrawWitness::Frame& frame,const char* reason) {
    if (!frame.scene) return;
    log << "center_draw_witness scene=" << frame.scene << " reason=" << reason
        << " count=" << frame.count << " truncated=" << frame.truncated << '\n';
    for (unsigned i=0;i<frame.count;++i) {
        const auto& draw=frame.draws[i];
        log << "center_draw scene=" << frame.scene << " ordinal=" << draw.ordinal << " slot=" << draw.slot
            << " kind=" << draw.kind << " shader=" << Hex(draw.shader) << " cb0_bytes=" << draw.cb0Bytes << '\n';
    }
    log.flush();
}
std::array<uint32_t,2> DrawableSource(std::array<uint32_t,12>* nativeList=nullptr) {
    // Read-only provenance: follow valid frame links inside this thread's stack.
    // Omitted frame pointers may leave gaps; never guess a missing frame.
    CONTEXT registers{}; RtlCaptureContext(&registers);
    ULONG_PTR low{},high{}; GetCurrentThreadStackLimits(&low,&high);
    auto frame=static_cast<uintptr_t>(registers.Ebp);
    const auto game=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    constexpr uint8_t prologue[]{0x55,0x8b,0xec,0x8b,0x45,0x0c,0x83,0xec,0x10,0x53,0x56,0x57,0x8b,0xf1};
    constexpr uint8_t call[]{0xe8,0x57,0xbc,0x08,0x00};
    constexpr uint8_t filteredCall[]{0xe8,0x31,0xbc,0x08,0x00};
    if (!MatchesInstructions(reinterpret_cast<void*>(game+0x5e51e0),prologue) ||
        !MatchesInstructions(reinterpret_cast<void*>(game+0x559584),call) ||
        !MatchesInstructions(reinterpret_cast<void*>(game+0x5595aa),filteredCall)) return {};
    for (unsigned i=0;i<32 && frame>=low && frame<=high-16 && !(frame&3);++i) {
        std::array<uint32_t,4> words{};
        if (!ReadMemory(reinterpret_cast<void*>(frame),words.data(),sizeof(words))) break;
        if (words[1]==game+0x559589 || words[1]==game+0x5595af) {
            // Optional capture-only witness of the actual caller and list head.
            // Validate each cdecl frame's return call before interpreting args.
            const auto calls=[game](uint32_t address,uint32_t target) {
                std::array<uint8_t,5> bytes{};int32_t displacement{};
                if(address<game+5 || address>=game+0x1000000 ||
                   !ReadMemory(reinterpret_cast<void*>(address-5),bytes.data(),bytes.size()) || bytes[0]!=0xe8)return false;
                memcpy(&displacement,bytes.data()+1,sizeof(displacement));
                return address+displacement==game+target;
            };
            std::array<uint32_t,11> walker{};
            if(nativeList && words[0]>frame && words[0]<=high-sizeof(walker) && !(words[0]&3) &&
               ReadMemory(reinterpret_cast<void*>(words[0]),walker.data(),sizeof(walker)) && calls(walker[1],0x559510)) {
                std::array<uint32_t,12> result{};
                result[0]=walker[1]-static_cast<uint32_t>(game);
                std::copy(walker.begin()+2,walker.end(),result.begin()+2);
                if(result[0]==0x7a4118 && walker[0]>words[0] && walker[0]<=high-8 && !(walker[0]&3)) {
                    std::array<uint32_t,2> outer{};
                    if(ReadMemory(reinterpret_cast<void*>(walker[0]),outer.data(),sizeof(outer)) && calls(outer[1],0x7a40f0))
                        result[1]=outer[1]-static_cast<uint32_t>(game);
                }
                if(ReadMemory(reinterpret_cast<void*>(game+0xecce18),&result[11],sizeof(result[11]))) *nativeList=result;
            }
            return {words[2],words[3]};
        }
        if (words[0]<=frame) break;
        frame=words[0];
    }
    return {};
}
void Source(uint64_t hash) {
    const auto scene=engine::CurrentScene();
    if (!scene.id || sources.contains({scene.pass,hash}) || sources.size()>=256) return;
    const auto pointers=DrawableSource();
    const std::array<uint32_t,4> words{0,0,pointers[0],pointers[1]};
    if (words[2]) {
            sources.insert({scene.pass,hash});
            const auto prefix="source-pass"+std::to_string(scene.pass)+"-"+Hex(hash);
            log << "source pass=" << scene.pass << " upload=" << Hex(hash) << " drawable=" << Hex(words[2]) << " properties=" << Hex(words[3]);
            for (unsigned arg=2;arg<4;++arg) {
                std::array<uint8_t,256> bytes{};
                const bool valid=ReadMemory(reinterpret_cast<void*>(words[arg]),bytes.data(),bytes.size());
                const auto tag=arg==2?"drawable":"properties";
                log << ' ' << tag << "_saved=" << valid;
                if (valid) {
                    std::ofstream file(output/(prefix+"-"+tag+".bin"),std::ios::binary);
                    file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
                    if (arg==2) {
                        uint32_t transform{}; memcpy(&transform,bytes.data()+0x2c,4);
                        std::array<uint8_t,512> values{};
                        const bool read=ReadMemory(reinterpret_cast<void*>(transform),values.data(),values.size());
                        log << " transform=" << Hex(transform) << " transform_saved=" << read;
                        if (read) {
                            std::ofstream snapshot(output/(prefix+"-transform.bin"),std::ios::binary);
                            snapshot.write(reinterpret_cast<const char*>(values.data()),values.size());
                        }
                    }
                }
            }
            log << '\n'; return;
    }
}
uint64_t ShaderHash(ID3D11DeviceChild* shader) {
    uint64_t hash{}; UINT bytes=sizeof(hash);
    return shader && SUCCEEDED(shader->GetPrivateData(shaderId,&bytes,&hash)) ? hash:0;
}
void IsolateShadowSampler(ID3D11DeviceContext* context,const char* kind,std::span<const UINT> args,unsigned ordinal) {
    if (!isolateShadowSampler || kind[0]=='C' || engine::CurrentStage().slot!=0) return;
    ComPtr<ID3D11PixelShader> ps; context->PSGetShader(&ps,nullptr,nullptr);
    if (ShaderHash(ps.Get())!=0x158f4162b8000a3full) return;
    auto refuse=[&](const char* reason) { pairHealthy=false; log << "shadow_sampler ordinal=" << ordinal << " refused=" << reason << " scene=" << engine::CurrentScene().id << " pass=" << engine::CurrentScene().pass << '\n'; };
    ComPtr<ID3D11VertexShader> vs; context->VSGetShader(&vs,nullptr,nullptr);
    if (strcmp(kind,"Draw") || args.size()!=2 || args[0]!=4 || args[1]!=0 ||
        ShaderHash(vs.Get())!=0x4b2b0f44adfbf5bbull || restoreShadowContext) { refuse("draw_identity"); return; }
    const auto pass=engine::CurrentScene().pass;
    ShadowPassIdentity identity;
    if (pass>=shadowDraws.size() || ++shadowDraws[pass]!=1 || !identity.Read(context)) { refuse("pass_identity"); return; }
    ComPtr<ID3D11SamplerState> current; context->PSGetSamplers(6,1,&current);
    if (!engine::CurrentScene().replay) {
        if (shadowOrdinal) { refuse("duplicate_capture"); return; }
        shadowOrdinal=ordinal;shadowIdentity=std::move(identity);
        log << "shadow_sampler ordinal=" << ordinal << " captured=1 identity=1 default_sampler=" << !shadowIdentity.sampler6
            << " scene=" << engine::CurrentScene().id << '\n'; return;
    }
    if (!shadowOrdinal || !shadowIdentity.Matches(identity)) { refuse("pass_identity_mismatch"); return; }
    restoreShadowSampler=current; restoreShadowContext=context;
    auto* binding=shadowIdentity.sampler6.Get();
    const bool applied=shadowIdentity.ApplySampler(context);pairHealthy&=applied;
    log << "shadow_sampler ordinal=" << ordinal << " applied=" << applied << " default_sampler=" << !binding
        << " changed=" << (current.Get()!=binding) << " identity=1 source_ordinal=" << shadowOrdinal
        << " scene=" << engine::CurrentScene().id << " pass=" << engine::CurrentScene().pass << '\n';
}
void NormalizeShadowBindings(ID3D11DeviceContext* context,const char* kind,std::span<const UINT> args,unsigned ordinal) {
    if (!normalizeShadowBindings || engine::CurrentStage().slot!=0 || strcmp(kind,"Draw") ||
        args.size()!=2 || args[0]!=4 || args[1]!=0 || restoreUnusedContext) return;
    ComPtr<ID3D11PixelShader> ps; ComPtr<ID3D11VertexShader> vs;
    context->PSGetShader(&ps,nullptr,nullptr); context->VSGetShader(&vs,nullptr,nullptr);
    if (ShaderHash(ps.Get())!=0x158f4162b8000a3full || ShaderHash(vs.Get())!=0x4b2b0f44adfbf5bbull) return;
    // This exact shader reads t0-4 and s1-6. Temporarily remove only its unused
    // texture/sampler tail to test whether stale bindings affect the driver.
    restoreUnusedShadow.Read(context); restoreUnusedContext=context; unusedOrdinal=ordinal;
    const UnusedShadowBindings empty;
    const bool applied=empty.Apply(context); pairHealthy&=applied;
    log << "shadow_unused pass=" << engine::CurrentScene().pass << " ordinal=" << ordinal
        << " applied=" << applied << " scene=" << engine::CurrentScene().id << " pass=" << engine::CurrentScene().pass << '\n';
}
void IsolateModel(ID3D11DeviceContext* context,const char* kind,std::span<const UINT> args,unsigned ordinal) {
    if (!isolateModels || kind[0]=='C' || engine::CurrentStage().slot!=6 || args.size()>5 || restoreModelBuffer) return;
    ComPtr<ID3D11VertexShader> vs; context->VSGetShader(&vs,nullptr,nullptr);
    if (ShaderHash(vs.Get())!=CrowdModelShader) return;
    auto refuse=[&](const char* reason) { log << "model_isolation ordinal=" << ordinal << " refused=" << reason << '\n'; };
    ComPtr<ID3D11Buffer> globals; context->VSGetConstantBuffers(0,1,&globals);
    UploadInfo info{}; UINT bytes=sizeof(info);
    if (!globals || FAILED(globals->GetPrivateData(uploadId,&bytes,&info)) || info.bytes!=128 || !info.saved) { refuse("upload"); return; }
    ModelDrawSnapshot current{};
    std::ifstream file(output/("upload-"+Hex(info.hash)+".bin"),std::ios::binary);
    file.read(reinterpret_cast<char*>(current.constants.data()),current.constants.size());
    if (!file || Hash(current.constants.data(),current.constants.size())!=info.hash) { refuse("upload_file"); return; }
    auto& key=current.identity;
    key.shader=ShaderHash(vs.Get()); key.kind=Hash(kind,strlen(kind));
    ComPtr<ID3D11PixelShader> ps; context->PSGetShader(&ps,nullptr,nullptr); key.pixelShader=ShaderHash(ps.Get());
    const auto source=DrawableSource(); key.drawable=source[0];
    std::array<uint8_t,64> native{};
    const auto game=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    constexpr uint8_t semantic[]{0x8b,0x4b,0x10,0xc5,0xf8,0x28,0x41,0x70,0x8b,0x43,0x14,0xc5,0xf8,0x29,0x00};
    if (!key.drawable || !MatchesInstructions(reinterpret_cast<void*>(game+0x9345c2),semantic) ||
        !ReadMemory(reinterpret_cast<void*>(key.drawable+0x2c),&key.transform,4) ||
        !key.transform || key.transform>UINT32_MAX-0xb0 ||
        !ReadMemory(reinterpret_cast<void*>(key.transform+0x70),native.data(),native.size()) ||
        memcmp(native.data(),current.constants.data(),native.size())) { refuse("native_model"); return; }
    ComPtr<ID3D11Buffer> vertex,index; DXGI_FORMAT format{};
    context->IAGetVertexBuffers(0,1,&vertex,&key.stride,&key.vertexOffset);
    context->IAGetIndexBuffer(&index,&format,&key.indexOffset);
    key.vertex=reinterpret_cast<uint32_t>(vertex.Get()); key.index=reinterpret_cast<uint32_t>(index.Get()); key.indexFormat=format;
    key.argCount=static_cast<uint32_t>(args.size()); std::copy(args.begin(),args.end(),key.args.begin());
    if (!engine::CurrentScene().replay) {
        if (modelSnapshots.size()<128) { modelSnapshots[ordinal]=current; log << "model_isolation ordinal=" << ordinal << " captured=1\n"; }
        else refuse("bound");
        return;
    }
    const auto found=modelSnapshots.find(ordinal);
    std::array<uint8_t,128> data{};
    if (found==modelSnapshots.end() || !PrepareModelSnapshot(found->second,current,native,data)) { refuse("draw_identity"); return; }
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    D3D11_BUFFER_DESC desc{}; desc.ByteWidth=128; desc.Usage=D3D11_USAGE_IMMUTABLE; desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{data.data(),0,0}; ComPtr<ID3D11Buffer> replacement;
    if (FAILED(device->CreateBuffer(&desc,&initial,&replacement))) { refuse("allocation"); return; }
    Upload(replacement.Get(),data.data(),128);
    restoreModelBuffer=globals; restoreModelOrdinal=ordinal; restoreModelContext=context;
    auto* binding=replacement.Get(); context->VSSetConstantBuffers(0,1,&binding);
    log << "model_isolation ordinal=" << ordinal << " applied=1 changed=" << (data!=current.constants) << '\n';
}
void Buffers(ID3D11DeviceContext* context,const char* stage) {
    ID3D11Buffer* buffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]{};
    if (stage[0]=='v') context->VSGetConstantBuffers(0,14,buffers);
    else if (stage[0]=='p') context->PSGetConstantBuffers(0,14,buffers);
    else context->CSGetConstantBuffers(0,14,buffers);
    for (unsigned slot=0;slot<14;++slot) {
        ComPtr<ID3D11Buffer> buffer; buffer.Attach(buffers[slot]);
        if (!buffer) continue;
        UploadInfo info{}; UINT size=sizeof(info);
        const bool known=SUCCEEDED(buffer->GetPrivateData(uploadId,&size,&info));
        log << ' ' << stage << slot << '=' << buffer.Get() << ':' << (known?Hex(info.hash):"unknown") << ':' << (known && info.saved);
    }
}
void Resources(ID3D11DeviceContext* context,const char* stage) {
    // Include the structured lighting resources above slot 16.
    ID3D11ShaderResourceView* views[32]{};
    if (stage[0]=='v') context->VSGetShaderResources(0,32,views);
    else if (stage[0]=='p') context->PSGetShaderResources(0,32,views);
    else context->CSGetShaderResources(0,32,views);
    for (unsigned slot=0;slot<32;++slot) {
        ComPtr<ID3D11ShaderResourceView> view; view.Attach(views[slot]);
        if (!view) continue;
        ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
        log << ' ' << stage << "srv" << slot << '=' << view.Get() << ':' << resource.Get();
        const float lod=context->GetResourceMinLOD(resource.Get()); uint32_t lodBits{}; memcpy(&lodBits,&lod,4);
        log << ' ' << stage << "minlod" << slot << '=' << lodBits;
    }
}
}
void Configure(const std::filesystem::path& folder,bool requested,NativeDispatch dispatch) {
    output=folder; enabled=requested;
    runtimeSession=std::filesystem::exists(folder/L"runtime-session.request");
    screenUiNormalDefault=runtimeSession && std::filesystem::exists(folder/L"xr-session.request") &&
        std::filesystem::exists(folder/L"xr-projection.request") && std::filesystem::exists(folder/L"stereo-eyes.request");
    screenUiEnabled=requested && (screenUiNormalDefault || std::filesystem::exists(folder/L"screen-ui-stereo.request"));
    if (screenUiEnabled) screenUi.Diagnose(folder);
    if (requested && runtimeSession) flicker::Configure(folder); // Configure on the render owner, not device creation.
    spotlightwitness::Configure(folder,requested && runtimeSession);
    spotlightEnabled=requested && screenUiNormalDefault && dispatch;
    spotlightCorrection.Configure(dispatch);
    disableMotionBlur=requested && std::filesystem::exists(folder/L"disable-motion-blur.request");nativeDispatch=dispatch;
    limitCockpitExposure=requested && runtimeSession && std::filesystem::exists(folder/L"cockpit-exposure.request");
    removeImpactInterference=requested && runtimeSession && dispatch && std::filesystem::exists(folder/L"impact-interference.request");
    compact=requested && std::filesystem::exists(folder/L"compact-stereo.request") && std::filesystem::exists(folder/L"stereo-eyes.request");
    isolateModels=requested && std::filesystem::exists(folder/L"isolate-models.request");
    restoreComputeState=requested && std::filesystem::exists(folder/L"restore-compute-state.request");
    sortVpl=requested && std::filesystem::exists(folder/L"sort-vpl.request");
    isolateShadowSampler=requested && std::filesystem::exists(folder/L"isolate-shadow-sampler.request");
    allowAbsentShadow=runtimeSession;
    normalizeShadowBindings=requested && std::filesystem::exists(folder/L"normalize-shadow-bindings.request");
    if (enabled) {
        ownerThread=GetCurrentThreadId(); log.open(output/L"replay-work.txt",runtimeSession?sessionLogSegmentBytes:0,
            !runtimeSession || std::filesystem::exists(output/L"diagnostic-logging.request"));
        log << "capture_mode compact=" << compact << '\n';log.flush();
    }
}
// Optional: an inconsistent pass schedule falls back to the native effect
// without stopping VR. An unrestored binding remains a pair failure.
void CheckImpactInterference(unsigned pass) {
    if (!removeImpactInterference) return;
    pairHealthy&=!impactInterference.Pending();
    if (pass<impactDraws.size() && impactInterference.PassComplete(impactDraws[0],impactDraws[pass])) return;
    removeImpactInterference=false;
    log << "impact_interference disabled=1 native_fallback=1 reason=schedule pass=" << pass
        << " center_draws=" << impactDraws[0] << '\n';log.FlushNow();
}
void SceneBoundary(ID3D11DeviceContext* context,const char* phase) {
    if (!restoreComputeState || !Active()) return;
    if (screenUiEnabled) {
        pairHealthy&=!screenUi.Pending();
        const auto scene=engine::CurrentScene();
        if (!strcmp(phase,"replay-restored") && scene.pass==2) {
            ++uiPairs;uiScene=scene.id;uiNativeFrame=engine::NativeCameraFrame();
            if (uiPairs==1 || uiPairs-uiProofPairs>=120 || uiProofChanged) WriteUiProof("pair");
            if (uiPairs==1 || uiPairs-spotlightProofPair>=120) {
                WriteSpotlightProof();spotlightProofPair=uiPairs;
            }
        }
    }
    auto& eye=CurrentAoEye();
    if (!strcmp(phase,"scene-entry")) {
        if (sceneOpen) { pairHealthy=false;return; }
        sceneOpen=true;eyeReplayStarted=false;
        if (engine::RepeatingEyes()) {
            pairHealthy=true; aoPrepared=false; computeAfterSaved=false;
            aoIncomingSaved=false; aoCompletedSaved=false; shadowOrdinal=0;
            shadowIdentity={};shadowDraws={};
            blurDraws={};
            exposureDraws={};
            impactDraws={};
            for (auto& count:computeOrdinal) count=0;
            computeSchedule={};lightCullSorted={};
            if (compact) centerDraws.Begin(engine::CurrentScene().id);
        }
        incomingCompute.Read(context); computeBeforeSaved=true;
    }
    if (!strcmp(phase,"scene-complete")) {
        if (disableMotionBlur) pairHealthy&=motionBlur.PassComplete(blurDraws[0],blurDraws[0]);
        if (limitCockpitExposure) pairHealthy&=cockpitExposure.PassComplete(exposureDraws[0],exposureDraws[0]);
        CheckImpactInterference(0);
        if (isolateShadowSampler) pairHealthy&=ShadowPassComplete(0);
        completedCompute.Read(context); computeAfterSaved=true;
        if (aoIncomingSaved && aoCompleted) aoCompletedSaved=CopyScratch(context,aoCompleted.Get(),aoResource.Get());
        // AO and tiled lighting are independent native options. A frame may
        // omit AO while still running the exact guarded light culler once.
        // Unknown shaders and partial AO sequences remain failures.
        const bool absent=NoAoSchedule(0) && !aoIncomingSaved && !aoCompletedSaved;
        if (absent && pairHealthy) ClearAoHistory();
        if (!absent && (!aoCompletedSaved || (engine::RepeatingEyes() && computeOrdinal[0]!=4))) pairHealthy=false;
        RecordComputeSchedule(0,pairHealthy);
        if (compact && engine::RepeatingEyes()) {
            if (!pairHealthy && !centerFailureDumped) {
                DumpCenterDraws(centerDraws.Previous(),"before_failure");
                DumpCenterDraws(centerDraws.Current(),"failure");
                centerFailureDumped=true;
            } else if (FirstPair()) DumpCenterDraws(centerDraws.Current(),"first");
        }
        if (!pairHealthy) {
            log << "center_gpu refused=1 scene=" << engine::CurrentScene().id
                << " blur_required=" << disableMotionBlur << " blur_draws=" << blurDraws[0]
                << " blur_pending=" << motionBlur.Pending() << " shadow_required=" << isolateShadowSampler
                << " shadow_draws=" << shadowDraws[0] << " ao_incoming=" << aoIncomingSaved
                << " ao_completed=" << aoCompletedSaved << " compute_draws=" << computeOrdinal[0] << '\n';log.FlushNow();
        }
    }
    if (!strcmp(phase,"replay-entry")) eyeReplayStarted=true;
    if (!strcmp(phase,"replay-entry") && computeBeforeSaved && computeAfterSaved) {
        aoPrepared=false;
        const bool applied=incomingCompute.Apply(context); pairHealthy&=applied;
        log << "compute_state incoming_counter=" << incomingCompute.counter.Get() << " completed_counter=" << completedCompute.counter.Get()
            << " incoming_sampler3=" << incomingCompute.samplers[3].Get() << " completed_sampler3=" << completedCompute.samplers[3].Get()
            << " prepared=" << applied << " scene=" << engine::CurrentScene().id << " pass=" << engine::CurrentScene().pass << '\n'; log.flush();
    }
    if (!strcmp(phase,"replay-complete") && engine::RepeatingEyes()) {
        if (disableMotionBlur) pairHealthy&=motionBlur.PassComplete(blurDraws[0],blurDraws[engine::CurrentScene().pass]);
        if (limitCockpitExposure) pairHealthy&=cockpitExposure.PassComplete(exposureDraws[0],exposureDraws[engine::CurrentScene().pass]);
        CheckImpactInterference(engine::CurrentScene().pass);
        if (isolateShadowSampler) pairHealthy&=ShadowPassComplete(engine::CurrentScene().pass);
        if (aoIncomingSaved) {
            eye.saved=aoPrepared && eye.texture && CopyScratch(context,eye.texture.Get(),aoResource.Get());
            pairHealthy&=eye.saved;
        } else pairHealthy&=!aoPrepared && NoAoSchedule(0) && NoAoSchedule(engine::CurrentScene().pass);
        log << "ao_eye scene=" << engine::CurrentScene().id << " pass=" << engine::CurrentScene().pass << " retained=" << eye.saved << " resource=" << eye.texture.Get() << '\n';
        if (AuditAo() && eye.saved) {
            const auto source=AoDigest(context,aoResource.Get()),actual=AoDigest(context,eye.texture.Get());
            pairHealthy&=!source.empty() && source==actual;
            log << "ao_verify scene=" << engine::CurrentScene().id << " pass=" << engine::CurrentScene().pass << " phase=retain source=" << source << " actual=" << actual << '\n';
            eye.digest=actual; eye.digestPair=pairNumber;
        }
    }
    if (!strcmp(phase,"replay-restored") && computeAfterSaved) {
        const bool restored=completedCompute.Apply(context); pairHealthy&=restored;
        log << "compute_state restored=" << restored << " scene=" << engine::CurrentScene().id << " pass=" << engine::CurrentScene().pass << '\n'; log.flush();
        if (aoCompletedSaved) {
            const bool copied=CopyScratch(context,aoResource.Get(),aoCompleted.Get()); pairHealthy&=copied;
            log << "ao_history completed_restored=" << copied << " scene=" << engine::CurrentScene().id << " pass=" << engine::CurrentScene().pass << '\n'; log.flush();
            if (AuditAo()) {
                const auto source=AoDigest(context,aoCompleted.Get()),actual=AoDigest(context,aoResource.Get());
                pairHealthy&=!source.empty() && source==actual;
                log << "ao_verify scene=" << engine::CurrentScene().id << " pass=" << engine::CurrentScene().pass << " phase=restore source=" << source << " actual=" << actual << '\n';
            }
        }
        pairHealthy&=(aoCompletedSaved && aoPrepared) ||
            (!aoIncomingSaved && !aoCompletedSaved && !aoPrepared && NoAoSchedule(0));
        if (engine::RepeatingEyes()) {
            const auto pass=engine::CurrentScene().pass;
            pairHealthy&=pass>0 && pass<3 && (computeOrdinal[0]==4 || NoAoSchedule(0)) &&
                computeOrdinal[pass]==computeOrdinal[0];
            if (computeOrdinal[0]==1) pairHealthy&=computeSchedule[pass]==computeSchedule[0];
            if (!pairHealthy) RecordComputeSchedule(pass,false);
            log << "pair_gpu scene=" << engine::CurrentScene().id << " pass=" << pass << " healthy=" << pairHealthy
                << " dispatch0=" << computeOrdinal[0] << " dispatch1=" << computeOrdinal[pass]
                << " shadow0=" << shadowDraws[0] << " shadow_eye=" << shadowDraws[pass] << '\n'; log.flush();
            if (pairHealthy && engine::DualEyes() && pass==2 && isolateShadowSampler && allowAbsentShadow &&
                shadowDraws==std::array<unsigned,3>{0,0,0} && ShadowPassComplete(2) &&
                lastAbsentShadowScene!=engine::CurrentScene().id) {
                lastAbsentShadowScene=engine::CurrentScene().id;++absentShadowPairs;
                // One overwritten receipt; GPU success is not engine/headset acceptance.
                if (absentShadowPairs==1 || absentShadowPairs%120==0) {
                    std::ofstream proof(output/L"shadow-absent-replay.json");
                    proof << "{\"scene\":" << lastAbsentShadowScene << ",\"nativeFrame\":" << engine::NativeCameraFrame()
                        << ",\"pairs\":" << absentShadowPairs
                        << ",\"runtimeSession\":true,\"draws\":[0,0,0],\"pending\":false,\"pairGpuHealthy\":true}\n";
                }
            }
            if (!defaultShadowProofSaved && pairHealthy && engine::DualEyes() && pass==2 &&
                isolateShadowSampler && shadowOrdinal && !shadowIdentity.sampler6 && !restoreShadowContext &&
                shadowDraws==std::array<unsigned,3>{1,1,1}) {
                // Keep one small state witness when bounded logs rotate. Final
                // engine acceptance and headset appearance remain separate checks.
                std::ofstream proof(output/L"shadow-default-replay.json");
                proof << "{\"scene\":" << engine::CurrentScene().id
                    << ",\"eyes\":2,\"centerSampler\":null,\"samplerRestored\":true,\"pairGpuHealthy\":true}\n";
                defaultShadowProofSaved=proof.good();
            }
        }
        if (!engine::DualEyes() || engine::CurrentScene().pass==2) sceneOpen=false;
    }
}
bool CancelBeforeReplay() {
    const auto scene=engine::CurrentScene();
    if (!runtimeSession || !enabled || GetCurrentThreadId()!=ownerThread || !sceneOpen || eyeReplayStarted ||
        !scene.id || scene.replay || scene.pass || motionBlur.Pending() || cockpitExposure.Pending() || impactInterference.Pending() ||
        screenUi.Pending() || spotlightCorrection.Pending() ||
        restoreUnusedContext || restoreShadowContext || restoreModelBuffer || restoreModelContext) return false;
    // No eye state was applied. Missing race-only passes in a native menu do
    // not poison GPU history; any unfinished binding still refuses above.
    sceneOpen=false;pairHealthy=true;
    if (uiPairs!=uiProofPairs || uiProofChanged) WriteUiProof("center_cancel");
    log << "session center_cancelled_before_replay=1 scene=" << scene.id << '\n';log.FlushNow();return true;
}
bool ResetHistory() {
    if (!enabled || GetCurrentThreadId()!=ownerThread || sceneOpen || !pairHealthy || motionBlur.Pending() || cockpitExposure.Pending() ||
        impactInterference.Pending() || screenUi.Pending() || spotlightCorrection.Pending() ||
        restoreUnusedContext || restoreShadowContext || restoreModelBuffer || restoreModelContext) return false;
    // The verified normal Quit path suspends and resets on the render owner.
    // Flush cumulative counts here rather than leaving the last periodic sample.
    WriteUiProof("history_reset");
    WriteSpotlightProof();
    if (!spotlightCorrection.ResetResources()) return false;
    // These are owned copies and retained COM references, never native contents.
    ClearAoHistory();
    incomingCompute={};completedCompute={};shadowOrdinal=0;shadowIdentity={};
    aoIncomingSaved=aoCompletedSaved=aoPrepared=computeBeforeSaved=computeAfterSaved=false;
    firstScene=activeScene=workScene=pairNumber=0;
    shadowDraws={};blurDraws={};exposureDraws={};impactDraws={};centerDraws={};
    log << "session gpu_history_reset=1\n";log.FlushNow();return true;
}
void BeforeCompute(ID3D11DeviceContext* context,const std::string& mark) {
    if (!restoreComputeState || !Active() || !mark.ends_with("-compute1")) return;
    ComPtr<ID3D11ComputeShader> shader; context->CSGetShader(&shader,nullptr,nullptr);
    const auto hash=ShaderHash(shader.Get());
    if (runtimeSession && sortVpl && hash==lightCullShader) return; // Its UAV/layout/sort must validate after dispatch.
    if (hash!=0x3edaf831f6f23893ull && hash!=0x9f9f7aeb0ae077eaull && hash!=0xbaa371e5a04c02f1ull && hash!=0xc02984a5d0518e28ull) {
        pairHealthy=false; log << "ao_history refused=shader hash=" << Hex(hash) << '\n'; return;
    }
    ComPtr<ID3D11UnorderedAccessView> view; context->CSGetUnorderedAccessViews(0,1,&view);
    ComPtr<ID3D11Resource> resource; if (view) view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture))) { pairHealthy=false; log << "ao_history refused=resource\n"; return; }
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    if (desc.Format!=DXGI_FORMAT_R8G8_UNORM || desc.ArraySize!=1 || desc.MipLevels!=1 || desc.SampleDesc.Count!=1 ||
        !desc.Width || !desc.Height || desc.Width>8192 || desc.Height>8192) { pairHealthy=false; log << "ao_history refused=format\n"; return; }
    if (!engine::CurrentScene().replay) {
        if (aoResource && (!engine::RepeatingEyes() || aoIncomingSaved || (!runtimeSession && aoResource!=texture))) {
            pairHealthy=false; log << "ao_history refused=duplicate_or_changed_resource\n"; return;
        }
        // A graphics-quality change may recreate this native target between
        // completed scenes. Start fresh owned history at its first guarded AO
        // producer; identity changes during either eye still refuse below.
        if (runtimeSession && aoResource && aoResource!=texture) {
            if (!sceneOpen || eyeReplayStarted || !pairHealthy) {pairHealthy=false;log << "ao_history refused=recreation_scope\n";return;}
            ClearAoHistory();log << "ao_history recreated_at_center=1 scene=" << engine::CurrentScene().id << '\n';
        }
        if (!aoResource) {
            ComPtr<ID3D11Device> device; context->GetDevice(&device);
            desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=0; desc.CPUAccessFlags=0; desc.MiscFlags=0;
            if (FAILED(device->CreateTexture2D(&desc,nullptr,&aoIncoming)) || FAILED(device->CreateTexture2D(&desc,nullptr,&aoCompleted))) {
                pairHealthy=false; log << "ao_history refused=allocation\n"; return;
            }
            if (engine::RepeatingEyes()) for (unsigned i=0;i<(engine::DualEyes()?2u:1u);++i)
                if (FAILED(device->CreateTexture2D(&desc,nullptr,&aoEyes[i].texture))) {
                    pairHealthy=false; log << "ao_history refused=eye_allocation\n"; return;
                }
            aoResource=texture;
        }
        aoIncomingSaved=CopyScratch(context,aoIncoming.Get(),texture.Get());
        pairHealthy&=aoIncomingSaved;
        log << "ao_history captured=" << aoIncomingSaved << " resource=" << texture.Get() << " scene=" << engine::CurrentScene().id << " pass=" << engine::CurrentScene().pass << '\n';
    } else {
        if (!aoIncomingSaved || !aoCompletedSaved || texture.Get()!=aoResource.Get()) { pairHealthy=false; log << "ao_history refused=identity\n"; return; }
        auto& eye=CurrentAoEye();
        const bool ownHistory=engine::RepeatingEyes() && eye.saved;
        const auto sourceHash=AuditAo()?AoDigest(context,ownHistory?eye.texture.Get():aoIncoming.Get()):std::string{};
        aoPrepared=CopyScratch(context,texture.Get(),ownHistory?eye.texture.Get():aoIncoming.Get()); pairHealthy&=aoPrepared;
        if (AuditAo()) {
            const auto actual=AoDigest(context,texture.Get());
            const bool priorSampled=ownHistory && eye.digestPair+1==pairNumber;
            const bool priorMatch=!priorSampled || (!eye.digest.empty() && sourceHash==eye.digest);
            pairHealthy&=!sourceHash.empty() && actual==sourceHash && priorMatch;
            log << "ao_verify scene=" << engine::CurrentScene().id << " pass=" << engine::CurrentScene().pass << " phase=seed source=" << sourceHash << " actual=" << actual
                << " prior_sampled=" << priorSampled << " prior_matched=" << priorMatch << '\n';
        }
        log << "ao_history prepared=" << aoPrepared << " scene=" << engine::CurrentScene().id << " pass=" << engine::CurrentScene().pass << '\n';
        if (engine::RepeatingEyes()) log << "ao_eye scene=" << engine::CurrentScene().id << " pass=" << engine::CurrentScene().pass << " previous_eye=" << ownHistory << " prepared=" << aoPrepared
            << " history_resource=" << (ownHistory?eye.texture.Get():aoIncoming.Get()) << '\n';
    }
    log.flush();
}
ID3D11Texture2D* ComputeScratchResource() { return restoreComputeState && Active() ? aoResource.Get():nullptr; }
bool VplOrderRequested(const std::string& mark) {
    const auto pass=engine::CurrentScene().pass;
    return sortVpl && Active() && (mark.ends_with("-compute4") ||
        (runtimeSession && pass<3 && mark.ends_with("-compute1") && computeSchedule[pass][0].shader==lightCullShader));
}
void AfterCompute(ID3D11DeviceContext* context,const std::string& mark,UINT x,UINT y,UINT z,NativeDispatch dispatch) {
    if (!VplOrderRequested(mark)) return;
    ComPtr<ID3D11ComputeShader> shader; context->CSGetShader(&shader,nullptr,nullptr);
    if (ShaderHash(shader.Get())!=lightCullShader) { pairHealthy=false; log << "vpl_order refused=shader\n"; log.flush(); return; }
    ComPtr<ID3D11UnorderedAccessView> view; context->CSGetUnorderedAccessViews(1,1,&view);
    bool restored{}; const auto hr=vplOrder.Apply(context,view.Get(),x,y,z,dispatch,restored);
    pairHealthy&=SUCCEEDED(hr) && restored;
    lightCullSorted[engine::CurrentScene().pass]=SUCCEEDED(hr) && restored;
    log << "vpl_order pass=" << engine::CurrentScene().pass << " applied=" << SUCCEEDED(hr)
        << " restored=" << restored << " hr=" << Hex(static_cast<uint32_t>(hr)) << " scene=" << engine::CurrentScene().id << " pass=" << engine::CurrentScene().pass;
    if (FAILED(hr)) {
        ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> texture;D3D11_TEXTURE2D_DESC desc{};
        if (view) view->GetResource(&resource);
        if (resource && SUCCEEDED(resource.As(&texture))) texture->GetDesc(&desc);
        log << " groups=" << x << 'x' << y << 'x' << z << " texture=" << desc.Width << 'x' << desc.Height;
    }
    log << '\n';log.flush();
}
void ShaderIdentity(ID3D11DeviceChild* shader,uint64_t hash) { shader->SetPrivateData(shaderId,sizeof(hash),&hash); }
uint64_t ShaderFingerprint(ID3D11DeviceChild* shader) { return ShaderHash(shader); }
bool ShadowControlBindings(ID3D11DeviceContext* context,bool native) {
    if (restoreUnusedContext!=context || GetCurrentThreadId()!=ownerThread) return false;
    const UnusedShadowBindings empty;
    return (native ? restoreUnusedShadow:empty).Apply(context);
}
void AfterDraw(ID3D11DeviceContext* context) {
    if (spotlightCorrection.Pending() && GetCurrentThreadId()==ownerThread) {
        const bool restored=spotlightCorrection.AfterDraw(context);pairHealthy&=restored;
        const auto pass=engine::CurrentScene().pass;
        if (restored && pass>0 && pass<3) ++spotlightRestored[pass-1];
        if (!restored) {log << "spotlight restored=0\n";log.FlushNow();}
    }
    if (cockpitExposure.Pending() && GetCurrentThreadId()==ownerThread) {
        const bool restored=cockpitExposure.Restore(context);pairHealthy&=restored;
        if(!restored) {log<<"cockpit_exposure restored=0\n";log.FlushNow();}
    }
    if (impactInterference.Pending() && GetCurrentThreadId()==ownerThread) {
        const bool restored=impactInterference.Restore(context);pairHealthy&=restored;
        if(!restored) {log<<"impact_interference restored=0\n";log.FlushNow();}
    }
    if (screenUi.Pending() && GetCurrentThreadId()==ownerThread) {
        const bool restored=screenUi.Restore(context);pairHealthy&=restored;
        const auto pass=engine::CurrentScene().pass;
        if (restored && pass>0 && pass<3) {
            ++uiRestored[pass-1];
            if (uiPendingShader<uiShaderCounts.size()) ++uiShaderCounts[uiPendingShader].restored[pass-1];
        }
        uiPendingShader=screenUiShaders.size();
        if (!restored) { log << "screen_ui restored=0\n";log.FlushNow(); }
    }
    if (motionBlur.Pending() && GetCurrentThreadId()==ownerThread) {
        const bool restored=motionBlur.Restore(context);pairHealthy&=restored;
        log << "motion_blur scene=" << engine::CurrentScene().id << " pass=" << engine::CurrentScene().pass << " restored=" << restored << '\n';log.flush();
    }
    if (restoreUnusedContext==context && GetCurrentThreadId()==ownerThread) {
        const bool restored=restoreUnusedShadow.Apply(context); pairHealthy&=restored;
        log << "shadow_unused pass=" << engine::CurrentScene().pass << " ordinal=" << unusedOrdinal
            << " restored=" << restored << " scene=" << engine::CurrentScene().id << " pass=" << engine::CurrentScene().pass << '\n'; log.flush();
        restoreUnusedShadow={}; restoreUnusedContext=nullptr;
    }
    if (restoreShadowContext==context && GetCurrentThreadId()==ownerThread) {
        auto* binding=restoreShadowSampler.Get(); context->PSSetSamplers(6,1,&binding);
        ComPtr<ID3D11SamplerState> actual; context->PSGetSamplers(6,1,&actual);
        pairHealthy&=actual.Get()==binding;
        log << "shadow_sampler ordinal=" << shadowOrdinal << " restored=" << (actual.Get()==binding) << " scene=" << engine::CurrentScene().id << " pass=" << engine::CurrentScene().pass << '\n'; log.flush();
        restoreShadowSampler.Reset(); restoreShadowContext=nullptr;
    }
    if (!restoreModelBuffer || GetCurrentThreadId()!=ownerThread || context!=restoreModelContext) return;
    auto* binding=restoreModelBuffer.Get(); context->VSSetConstantBuffers(0,1,&binding);
    ComPtr<ID3D11Buffer> actual; context->VSGetConstantBuffers(0,1,&actual);
    log << "model_isolation ordinal=" << restoreModelOrdinal << " restored=" << (actual.Get()==binding) << '\n'; log.flush();
    restoreModelBuffer.Reset(); restoreModelContext=nullptr;
}
bool Active() {
    if (!enabled || GetCurrentThreadId()!=ownerThread) return false;
    const auto scene=engine::CurrentScene();
    if (!scene.id) return false;
    if (!firstScene) firstScene=scene.id;
    if (!engine::RepeatingEyes()) return scene.id==firstScene;
    if (!engine::ObservingEyes()) return false;
    if (scene.id==activeScene) return true;
    if (!runtimeSession && pairNumber>=engine::EyePairCount()) return false;
    activeScene=scene.id; ++pairNumber; return true;
}
bool Compact() { return compact; }
bool ScreenUiEnabled() { return enabled && screenUiEnabled; }
bool FirstPair() { return !runtimeSession && engine::RepeatingEyes() && Active() && pairNumber==1; }
bool Detailed() { return !runtimeSession && !compact && (!engine::RepeatingEyes() || FirstPair()); }
bool PairHealthy() { return pairHealthy; }
bool LastPair() { return !runtimeSession && engine::RepeatingEyes() && Active() && pairNumber==engine::EyePairCount(); }
void Upload(ID3D11Resource* resource,const void* data,UINT bytes) {
    // UI buffers initialized before Configure may be seeded by a bounded
    // readback. Replace that private copy on every tracked native upload,
    // including uploads made by another native context thread.
    if (ScreenUiEnabled()) ScreenUi::Upload(resource,data,bytes);
    spotlightwitness::Upload(resource,data,bytes);
    if (!enabled || GetCurrentThreadId()!=ownerThread || bytes>65536 || !Detailed()) return;
    UploadInfo info{Hash(data,bytes),bytes,false};
    const bool existed=saved.contains(info.hash);
    if (existed) info.saved=true;
    else if (saved.size()<4096 && savedBytes+bytes<=64*1024*1024) {
        std::ofstream file(output/("upload-"+Hex(info.hash)+".bin"),std::ios::binary);
        file.write(static_cast<const char*>(data),bytes); file.close();
        info.saved=file.good();
        if (info.saved) { saved.insert(info.hash); savedBytes+=bytes; }
    }
    resource->SetPrivateData(uploadId,sizeof(info),&info);
    if (!existed && Active()) {
        void* entries[16]{};
        const auto count=CaptureStackBackTrace(1,16,entries,nullptr);
        const auto game=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        log << "upload=" << Hex(info.hash) << " bytes=" << bytes << " callers=";
        for (unsigned i=0;i<count;++i) {
            MEMORY_BASIC_INFORMATION region{};
            if (VirtualQuery(entries[i],&region,sizeof(region)) && reinterpret_cast<uintptr_t>(region.AllocationBase)==game)
                log << Hex(reinterpret_cast<uintptr_t>(entries[i])-game) << ',';
        }
        log << '\n'; log.flush();
        if (bytes==128) { Source(info.hash); log.flush(); }
    }
}
std::string Work(ID3D11DeviceContext* context,const char* kind,std::span<const UINT> args) {
    if (!Active()) return {};
    const auto scene=engine::CurrentScene();
    const auto stage=engine::CurrentStage();
    if (workScene!=scene.id) {
        workScene=scene.id;
        if (spotlightEnabled) pairHealthy&=spotlightCorrection.BeginScene(scene.id);
        for (unsigned p=0;p<3;++p) workOrdinal[p]=slot6Ordinal[p]=preColorOrdinal[p]=computeOrdinal[p]=0;
    }
    const unsigned pass=scene.pass;
    const auto index=++workOrdinal[pass];
    if (index>4096) {
        if (compact && pass==0) centerDraws.Truncate();
        if (index==4097) { log << "work capture_truncated=draw_budget scene=" << scene.id << " pass=" << pass << '\n';log.flush(); }
        // The diagnostic history is bounded, not the game's draw list. Keep
        // every runtime correction/guard active for larger grids and scenes.
        if (!runtimeSession) {pairHealthy=false;return {};}
    }
    IsolateShadowSampler(context,kind,args,index);
    NormalizeShadowBindings(context,kind,args,index);
    IsolateModel(context,kind,args,index);
    if ((spotlightwitness::Enabled() || spotlightEnabled) && kind[0]!='C') {
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
        context->VSGetShader(&vs,nullptr,nullptr);context->PSGetShader(&ps,nullptr,nullptr);
        const auto vsHash=ShaderHash(vs.Get()),psHash=ShaderHash(ps.Get());
        spotlightwitness::Draw(context,scene.id,pass,stage.slot,index,kind,args,vsHash,psHash);
        if (spotlightEnabled && spotlightprojection::PixelLayout(vsHash,psHash).bytes && args.size()<=5) {
            SpotlightCorrection::DrawKey key{};
            key.slot=stage.slot;key.ordinal=index;key.kindHash=Hash(kind,strlen(kind));
            key.argCount=static_cast<UINT>(args.size());std::copy(args.begin(),args.end(),key.args.begin());
            key.vsHash=vsHash;key.psHash=psHash;
            spotlightprojection::Change change{};
            std::array<uint8_t,512> center{},eye{};
            const bool posed=scene.replay && engine::CurrentLightCameras(center,eye) &&
                spotlightprojection::MakeChange(center,eye,change);
            const auto result=spotlightCorrection.BeforeDraw(context,key,pass,posed?&change:nullptr);
            spotlightScene=scene.id;
            if (result==SpotlightCorrection::Result::Captured) ++spotlightCaptured;
            if (pass>0 && pass<3) {
                if (result==SpotlightCorrection::Result::Applied) ++spotlightSubmitted[pass-1];
                if (result==SpotlightCorrection::Result::Skipped) ++spotlightSkipped[pass-1];
            }
            if (result==SpotlightCorrection::Result::Failed) {
                pairHealthy=false;log << "spotlight apply_failed=1 pass=" << pass << '\n';log.FlushNow();
            }
        }
    }
    // Replays place the UI for their own eye; a native pass rendering the left
    // eye places it for eye 0, exactly as the left replay would.
    const auto* nativeEyeViews=scene.replay?nullptr:engine::NativeEyeViews();
    const unsigned uiEye=scene.replay?pass-1:0;
    if (screenUiEnabled && ((scene.replay && pass>0 && pass<3) || nativeEyeViews) && stage.slot==0 && kind[0]!='C') {
        ComPtr<ID3D11VertexShader> vs;context->VSGetShader(&vs,nullptr,nullptr);
        const auto shader=ShaderHash(vs.Get());
        if (uiEye==0 && !ScreenUiVertexShader(shader)) screenUi.Observe(context,shader,engine::NativeCameraFrame());
        const auto shaderIndex=ScreenUiShaderIndex(shader);
        if (shaderIndex<uiShaderCounts.size()) {
            auto& counts=uiShaderCounts[shaderIndex];
            if (!counts.firstScene) {
                counts.firstScene=scene.id;counts.firstNativeFrame=engine::NativeCameraFrame();uiProofChanged=true;
            }
            const auto* views=scene.replay?engine::CurrentReplayViews():nativeEyeViews;
            const auto result=views ? screenUi.Apply(context,shader,*views,uiEye):ScreenUi::Result::Skipped;
            if (result==ScreenUi::Result::Applied) {
                uiPendingShader=shaderIndex;++counts.applied[uiEye];
                if (!counts.firstAppliedScene) {
                    counts.firstAppliedScene=scene.id;counts.firstAppliedNativeFrame=engine::NativeCameraFrame();uiProofChanged=true;
                }
                ++uiApplied[uiEye];if (shader==0x6980d3e81754047dull) ++uiText[uiEye];
            } else if (result==ScreenUi::Result::Skipped) { ++uiSkipped[uiEye];++counts.skipped[uiEye]; }
            else { pairHealthy=false;log << "screen_ui apply_failed=1 pass=" << pass << '\n';log.FlushNow(); }
        }
    }
    if (flicker::Observing(scene.id) && kind[0]!='C') {
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
        context->VSGetShader(&vs,nullptr,nullptr);context->PSGetShader(&ps,nullptr,nullptr);
        std::array<uint32_t,12> nativeList{};
        const auto source=DrawableSource(flicker::Capturing(scene.id)?&nativeList:nullptr);
        flicker::Draw(context,pass,stage.slot,kind,args,ShaderHash(vs.Get()),ShaderHash(ps.Get()),source,nativeList);
    }
    // Keep two bounded native draw histories in memory. Dump the first scene
    // and, once only, a failed center plus its predecessor. This catches late
    // shader changes without writing every frame or accepting unknown shaders.
    if (compact && engine::RepeatingEyes() && pass==0 && kind[0]!='C') {
        ComPtr<ID3D11PixelShader> shader;context->PSGetShader(&shader,nullptr,nullptr);
        ComPtr<ID3D11Buffer> constants;context->PSGetConstantBuffers(0,1,&constants);
        D3D11_BUFFER_DESC desc{};if (constants) constants->GetDesc(&desc);
        centerDraws.Add({ShaderHash(shader.Get()),index,stage.slot,desc.ByteWidth,kind});
    }
    if (limitCockpitExposure && kind[0]!='C' && engine::CurrentCockpitCamera()) {
        ComPtr<ID3D11PixelShader> shader;context->PSGetShader(&shader,nullptr,nullptr);
        const auto hash=ShaderHash(shader.Get());
        if(hash==CockpitExposure::shaderHash) {
            const bool applied=exposureDraws[pass]==0 && cockpitExposure.Apply(context,hash);
            if(applied) ++exposureDraws[pass];
            if(!applied) {
                unsigned sizes[2]{};for(unsigned i=0;i<2;++i){ComPtr<ID3D11Buffer> b;context->PSGetConstantBuffers(i,1,&b);D3D11_BUFFER_DESC d{};if(b)b->GetDesc(&d);sizes[i]=d.ByteWidth;}
                // Apply refuses before changing any binding. This optional
                // meter correction may fall back to native without stopping VR.
                limitCockpitExposure=false;
                log<<"cockpit_exposure disabled=1 native_fallback=1 cb0="<<sizes[0]<<" cb1="<<sizes[1]<<'\n';log.FlushNow();
            }
            log<<"cockpit_exposure scene="<<scene.id<<" pass="<<pass<<" applied="<<applied<<'\n';
        }
    }
    if (removeImpactInterference && kind[0]!='C') {
        ComPtr<ID3D11PixelShader> shader;context->PSGetShader(&shader,nullptr,nullptr);
        if (ShaderHash(shader.Get())==ImpactInterference::shaderHash) {
            // Apply refuses before changing any binding; this optional removal
            // falls back to the native effect without stopping VR.
            const bool applied=impactDraws[pass]==0 && impactInterference.Apply(context,nativeDispatch);
            if (applied) ++impactDraws[pass];
            else {
                removeImpactInterference=false;
                log << "impact_interference disabled=1 native_fallback=1 scene=" << scene.id << " pass=" << pass << '\n';log.FlushNow();
            }
            if (applied && !impactInterferenceLogged) {
                impactInterferenceLogged=true;
                log << "impact_interference applied=1 first_scene=" << scene.id << " pass=" << pass << '\n';log.FlushNow();
            }
        }
    }
    if (disableMotionBlur && kind[0]!='C') {
        ComPtr<ID3D11PixelShader> shader;context->PSGetShader(&shader,nullptr,nullptr);
        const auto hash=ShaderHash(shader.Get());
        if (MotionBlurControl::KnownShader(hash)) {
            const bool applied=++blurDraws[pass]==1 && motionBlur.Apply(context,hash,nativeDispatch);pairHealthy&=applied;
            log << "motion_blur scene=" << scene.id << " pass=" << pass << " shader=" << Hex(hash) << " applied=" << applied << '\n';log.flush();
        }
    }
    if (kind[0]=='C' && computeOrdinal[pass]<8) {
        ComPtr<ID3D11ComputeShader> shader;context->CSGetShader(&shader,nullptr,nullptr);
        auto& dispatch=computeSchedule[pass][computeOrdinal[pass]];
        dispatch.shader=ShaderHash(shader.Get());dispatch.slot=stage.slot;
        if(args.size()==3)std::copy(args.begin(),args.end(),dispatch.groups.begin());
    }
    if (!Detailed() || index>4096) {
        if (kind[0]=='C') return "work-"+std::to_string(scene.id)+"-pass"+std::to_string(pass)+"-compute"+std::to_string(++computeOrdinal[pass]);
        if (compact && FirstPair() && stage.slot==6 && ++slot6Ordinal[pass]<=32)
            return "work-"+std::to_string(scene.id)+"-pass"+std::to_string(pass)+"-slot6-draw"+std::to_string(slot6Ordinal[pass]);
        return {};
    }
    log << "scene=" << scene.id << " pass=" << pass << " ordinal=" << index << " slot=" << stage.slot << " kind=" << kind << " args=";
    for (auto arg:args) log << arg << ',';
    ComPtr<ID3D11VertexShader> vs; context->VSGetShader(&vs,nullptr,nullptr);
    ComPtr<ID3D11PixelShader> ps; context->PSGetShader(&ps,nullptr,nullptr);
    ComPtr<ID3D11ComputeShader> cs; context->CSGetShader(&cs,nullptr,nullptr);
    log << " vs=" << vs.Get() << " ps=" << ps.Get() << " cs=" << cs.Get();
    ComPtr<ID3D11RenderTargetView> target; ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(1,&target,&depth);
    log << " rtv=" << target.Get() << " dsv=" << depth.Get();
    ComPtr<ID3D11DepthStencilState> depthState; UINT stencil{};
    ComPtr<ID3D11BlendState> blend; FLOAT factors[4]{}; UINT mask{};
    ComPtr<ID3D11RasterizerState> raster;
    context->OMGetDepthStencilState(&depthState,&stencil); context->OMGetBlendState(&blend,factors,&mask); context->RSGetState(&raster);
    std::array<uint32_t,4> factorBits{}; memcpy(factorBits.data(),factors,sizeof(factors));
    log << " depthstate=" << depthState.Get() << ':' << stencil << " raster=" << raster.Get() << " blend=" << blend.Get() << ':' << mask;
    for (auto bits:factorBits) log << ':' << bits;
    ComPtr<ID3D11Buffer> vertex,indexBuffer; UINT stride{},offset{},indexOffset{}; DXGI_FORMAT format{};
    context->IAGetVertexBuffers(0,1,&vertex,&stride,&offset); context->IAGetIndexBuffer(&indexBuffer,&format,&indexOffset);
    log << " vertex=" << vertex.Get() << ':' << stride << ':' << offset << " index=" << indexBuffer.Get() << ':' << format << ':' << indexOffset;
    if (kind[0]=='C') {
        Buffers(context,"c"); Resources(context,"c");
        ID3D11UnorderedAccessView* outputs[8]{}; context->CSGetUnorderedAccessViews(0,8,outputs);
        for (unsigned slot=0;slot<8;++slot) {
            ComPtr<ID3D11UnorderedAccessView> view; view.Attach(outputs[slot]);
            if (!view) continue;
            ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
            log << " uav" << slot << '=' << view.Get() << ':' << resource.Get();
        }
        ID3D11SamplerState* samplers[16]{}; context->CSGetSamplers(0,16,samplers);
        for (unsigned slot=0;slot<16;++slot) { ComPtr<ID3D11SamplerState> sampler; sampler.Attach(samplers[slot]); if (sampler) log << " sampler" << slot << '=' << sampler.Get(); }
    }
    else {
        Buffers(context,"v"); Buffers(context,"p"); Resources(context,"v"); Resources(context,"p");
        ID3D11SamplerState* samplers[16]{}; context->PSGetSamplers(0,16,samplers);
        for (unsigned slot=0;slot<16;++slot) { ComPtr<ID3D11SamplerState> sampler; sampler.Attach(samplers[slot]); if (sampler) log << " psampler" << slot << '=' << sampler.Get(); }
        ComPtr<ID3D11GeometryShader> gs; ComPtr<ID3D11HullShader> hs; ComPtr<ID3D11DomainShader> ds;
        context->GSGetShader(&gs,nullptr,nullptr); context->HSGetShader(&hs,nullptr,nullptr); context->DSGetShader(&ds,nullptr,nullptr);
        ComPtr<ID3D11InputLayout> layout; D3D11_PRIMITIVE_TOPOLOGY topology{};
        context->IAGetInputLayout(&layout); context->IAGetPrimitiveTopology(&topology);
        log << " gs=" << gs.Get() << " hs=" << hs.Get() << " ds=" << ds.Get() << " layout=" << layout.Get() << " topology=" << topology;
        ComPtr<ID3D11Predicate> predicate; BOOL predicateValue{}; context->GetPredication(&predicate,&predicateValue);
        log << " predicate=" << predicate.Get() << ':' << predicateValue;
        ID3D11Buffer* streams[4]{}; UINT strides[4]{},offsets[4]{};
        context->IAGetVertexBuffers(0,4,streams,strides,offsets);
        for (unsigned i=1;i<4;++i) if (streams[i]) log << " vertex" << i << '=' << streams[i] << ':' << strides[i] << ':' << offsets[i];
        for (auto* stream:streams) if (stream) stream->Release();
        D3D11_VIEWPORT viewport[16]{}; UINT count=16; context->RSGetViewports(&count,viewport);
        log << " viewport=" << count << ':';
        for (UINT i=0;i<count;++i) { std::array<uint32_t,6> bits{}; memcpy(bits.data(),&viewport[i],sizeof(viewport[i])); for (auto value:bits) log << value << ','; }
        D3D11_RECT scissors[16]{}; count=16; context->RSGetScissorRects(&count,scissors);
        log << " scissor=" << count << ':';
        for (UINT i=0;i<count;++i) log << scissors[i].left << ',' << scissors[i].top << ',' << scissors[i].right << ',' << scissors[i].bottom << ';';
    }
    log << '\n'; log.flush();
    if (stage.slot==0 && kind[0]!='C' && !slot6Ordinal[pass]) {
        // State objects are immutable; save each producer sampler descriptor once.
        static std::set<ID3D11SamplerState*> described;
        ID3D11SamplerState* samplers[16]{}; context->PSGetSamplers(0,16,samplers);
        for (auto* pointer:samplers) {
            ComPtr<ID3D11SamplerState> sampler; sampler.Attach(pointer);
            if (!sampler || !described.insert(pointer).second) continue;
            D3D11_SAMPLER_DESC desc{}; sampler->GetDesc(&desc);
            std::array<uint32_t,sizeof(desc)/4> words{}; memcpy(words.data(),&desc,sizeof(desc));
            log << "psampler_descriptor object=" << pointer << " words=";
            for (auto value:words) log << value << ',';
            log << '\n';
        }
    }
    if (kind[0]=='C') {
        const auto n=++computeOrdinal[pass];
        return n<=8 ? "work-"+std::to_string(scene.id)+"-pass"+std::to_string(pass)+"-compute"+std::to_string(n):"";
    }
    // Provenance at the draw also covers native upload-cache hits.
    if (kind[0]!='C') {
        ComPtr<ID3D11Buffer> globals; context->VSGetConstantBuffers(0,1,&globals);
        if (globals) {
            UploadInfo info{}; UINT bytes=sizeof(info);
            if (SUCCEEDED(globals->GetPrivateData(uploadId,&bytes,&info)) && info.bytes==128) Source(info.hash);
        }
    }
    if (stage.slot==6 && kind[0]!='C') {
        const auto n=++slot6Ordinal[pass];
        if (n<=32 || n==40 || n==48 || n==56 || n==64 || n==128 || n==200)
            return "work-"+std::to_string(scene.id)+"-pass"+std::to_string(pass)+"-slot6-draw"+std::to_string(n);
    }
    if (stage.slot==0 && kind[0]!='C' && !slot6Ordinal[pass]) {
        const auto n=++preColorOrdinal[pass];
        const auto pixel=ShaderHash(ps.Get());
        const bool producer=pixel==0xe8bc75f0f617aa9eull || pixel==0x158f4162b8000a3full;
        if (n<=8 || (n<=32 && producer))
            return "work-"+std::to_string(scene.id)+"-pass"+std::to_string(pass)+"-slot0-draw"+std::to_string(n);
    }
    return {};
}
}
