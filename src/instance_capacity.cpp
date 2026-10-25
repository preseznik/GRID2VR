#include "instance_capacity.h"
#include "guarded_hooks.h"
#include "instance_capacity_policy.h"
#include "instance_buffer_registry.h"
#include "headset_visibility_candidate.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <mutex>

namespace grid2vr::instancecapacity {
namespace {
uintptr_t imageBase{};
bool enabled{};
bool expanded{};
std::mutex mutex;
std::ofstream log;
RequestTotals demand;
BufferRegistry buffers;
constexpr unsigned MaxBufferLogEntries=256;
unsigned bufferCount{};
uint64_t bufferObservations{};
bool truncated{};
std::atomic<uint64_t> createCalls{};
template<class T> T Read(uintptr_t address) {
    T value{};ReadMemory(reinterpret_cast<void*>(address),&value,sizeof(value));return value;
}
template<size_t N> void Words(const std::array<uint32_t,N>& words) {
    log << '[';for(size_t i=0;i<N;++i){if(i)log<<',';log<<words[i];}log<<']';
}
}
bool Install(uintptr_t base,const std::filesystem::path& folder) {
    if(!std::filesystem::exists(folder/L"instance-capacity-observe.request"))return true;
    if(enabled)return imageBase==base;
    expanded=std::filesystem::exists(folder/L"instance-capacity-expanded.request");
    const bool headset=HeadsetVisibilityCandidate(folder);
    if((std::filesystem::exists(folder/L"headset-visibility-candidate.request") && !headset) ||
       (!headset && (!std::filesystem::exists(folder/L"tracked-visibility.request") ||
       !std::filesystem::exists(folder/L"flicker-visibility-only.request") ||
       !std::filesystem::exists(folder/L"flicker-unattended.request") ||
       std::filesystem::exists(folder/L"xr-projection.request") || std::filesystem::exists(folder/L"xr-session.request"))))return false;
    // Read-only qualification of the native lazy resource -> wrapper -> COM
    // buffer chain. No native startup instructions or allocations are changed.
    constexpr uint8_t wrapperSlot[]{0x83,0x7f,0x28,0,0x8d,0x77,0x28};
    constexpr uint8_t wrapperStore[]{0xff,0xd2,0x89,0x06,0xc7,0x47,0x2c,0,0,0,0};
    constexpr uint8_t ownerStore[]{0x89,0x48,0x04,0xc7,0};
    constexpr uint8_t gpuStore[]{0x8b,0x55,0x0c,0x5f,0x89,0x73,0x0c,0x5e,0x89,0x53,0x08};
    if(!MatchesInstructions(reinterpret_cast<void*>(base+0x93fd8b),wrapperSlot) ||
       !MatchesInstructions(reinterpret_cast<void*>(base+0x93fda0),wrapperStore) ||
       !MatchesInstructions(reinterpret_cast<void*>(base+0x93b1b9),ownerStore) ||
       Read<uintptr_t>(base+0x93b1be)!=base+0xd49628 ||
       !MatchesInstructions(reinterpret_cast<void*>(base+0x9498d0),gpuStore))return false;
    imageBase=base;
    buffers.Reset();bufferCount=0;bufferObservations=0;truncated=false;createCalls=0;
    log.open(folder/L"instance-capacity.jsonl",std::ios::binary|std::ios::trunc);if(!log)return false;
    enabled=true;
    log << "{\"type\":\"begin\",\"mode\":\"" << (expanded?"native_config":"observe")
        << "\",\"nativeCodeModified\":false,\"maxBuffers\":" << MaxBufferLogEntries
        << ",\"bufferOwnership\":\"live_resource\",\"maxTrackedBuffers\":" << BufferRegistry::MaxLiveBuffers << "}\n";log.flush();
    return true;
}
bool Enabled(){return enabled;}
void Disable(){enabled=false;}
namespace {
BindReadiness InspectBinding(uintptr_t main,bool report) {
    if(!enabled)return expanded?BindReadiness::Refused:BindReadiness::Ready;
    uintptr_t manager{},allocator{};
    const bool managerRead=ReadMemory(reinterpret_cast<void*>(main+0xd4),&manager,sizeof(manager));
    const bool allocatorRead=manager && ReadMemory(reinterpret_cast<void*>(manager+0xf8),&allocator,sizeof(allocator));
    std::array<uint32_t,7> header{};
    const bool headerRead=allocator && ReadMemory(reinterpret_cast<void*>(allocator),header.data(),sizeof(header));
    const bool known=headerRead &&
        (header[0]&255)==1 && header[3]==Stride && header[5]==LaneCount && header[4] && header[4]<=UINT32_MAX-16*LaneCount;
    std::lock_guard lock(mutex);
    uint32_t style{},styleCount{};
    const bool styleRead=manager && ReadMemory(reinterpret_cast<void*>(manager+0xb58),&style,sizeof(style));
    const bool styleCountRead=manager && ReadMemory(reinterpret_cast<void*>(manager+0xb34),&styleCount,sizeof(styleCount));
    bool initializing=!manager || !allocator;
    bool malformed=!managerRead || (manager && (!allocatorRead || !styleRead || !styleCountRead)) || (allocator && !headerRead);
    const auto initializedMatch=[&](uint32_t actual,uint32_t expected) {
        if(!actual && expected)initializing=true;
        else if(actual!=expected)malformed=true;
    };
    if(headerRead) {
        initializedMatch(header[0]&255,1);initializedMatch(header[3],Stride);initializedMatch(header[5],LaneCount);
        if(!header[4])initializing=true;
        else if(header[4]>UINT32_MAX-16*LaneCount)malformed=true;
    }
    std::array<uint32_t,LaneCount> expected{};
    bool candidateHealthy=known && ExpectedCapacities(style,styleCount,expanded,expected);
    if(expanded) {
        initializedMatch(styleCount,10);
        if(!style)initializing=true;
        else if(style!=2 && style!=3 && style!=4)malformed=true;
    }
    if(report) {log << "{\"type\":\"bind\",\"main\":" << main << ",\"manager\":" << manager << ",\"allocator\":" << allocator
        << ",\"known\":" << known << ",\"header\":";Words(header);
    log << ",\"styleIndex\":" << style << ",\"styleCount\":" << styleCount << ",\"lanes\":[";}
    if(known)for(unsigned lane=0;lane<LaneCount;++lane) {
        if(report && lane)log<<',';std::array<uint32_t,4> state{};
        const bool laneKnown=ReadMemory(reinterpret_cast<void*>(header[4]+16*lane),state.data(),sizeof(state));
        std::array<uint32_t,22> resource{};std::array<uint32_t,5> wrapper{};
        const bool resourceKnown=state[0] && ReadMemory(reinterpret_cast<void*>(state[0]),resource.data(),sizeof(resource));
        const bool wrapperKnown=resourceKnown && resource[10] && ReadMemory(reinterpret_cast<void*>(resource[10]),wrapper.data(),sizeof(wrapper));
        const bool bindingKnown=resourceKnown && resource[0]==imageBase+0xd4aec8 && wrapperKnown &&
            wrapper[0]==imageBase+0xd49628 && wrapper[1]==state[0] && wrapper[2] &&
            state[2]<=MaxCapacity && resource[14]==state[2]*Stride && wrapper[3]==state[2]*Stride;
        unsigned matches{};uint32_t actual{};bool healthy{};
        BufferEvidence b{};
        if(bindingKnown && buffers.Find(wrapper[2],b)) {
            matches=b.observations;actual=b.actual;healthy=SUCCEEDED(b.result) && actual==state[2]*Stride && b.requested==actual &&
                b.bind==D3D11_BIND_VERTEX_BUFFER && b.usage==D3D11_USAGE_DYNAMIC && b.cpuAccess==D3D11_CPU_ACCESS_WRITE;
        }
        candidateHealthy=candidateHealthy && laneKnown && state[2]==expected[lane] &&
            (state[2]?(state[0] && state[1] && healthy && matches==1):(!state[0] && !state[1]));
        if(expanded && styleCount==10 && style>=2 && style<=4) {
            malformed=malformed || !laneKnown;
            initializedMatch(state[2],expected[lane]);
            if(!expected[lane])malformed=malformed || state[0] || state[1];
            else {
                initializing=initializing || !state[0] || !state[1];
                malformed=malformed || (state[0] && !resourceKnown);
                if(resourceKnown) {
                    initializedMatch(resource[0],static_cast<uint32_t>(imageBase+0xd4aec8));
                    initializedMatch(resource[14],expected[lane]*Stride);
                    if(!resource[10])initializing=true;
                    else if(!wrapperKnown)malformed=true;
                    else {
                        initializedMatch(wrapper[0],static_cast<uint32_t>(imageBase+0xd49628));
                        initializedMatch(wrapper[1],state[0]);initializedMatch(wrapper[3],expected[lane]*Stride);
                        if(!wrapper[2] || !matches)initializing=true;
                        else if(matches!=1 || !healthy)malformed=true;
                    }
                }
            }
        }
        if(report) {log << "{\"lane\":" << lane << ",\"known\":" << laneKnown << ",\"state\":";Words(state);
        log << ",\"bindingKnown\":" << bindingKnown << ",\"wrapper\":";Words(wrapper);
        log << ",\"gpuMatches\":" << matches << ",\"actualBytes\":" << actual << ",\"gpuVerified\":" << (healthy && matches==1) << '}';}
    }
    if(report) {log << "],\"bufferEvidenceTruncated\":" << truncated << ",\"createCalls\":" << createCalls.load() << ",\"candidateHealthy\":" << candidateHealthy << "}\n";log.flush();}
    // Missing GPU observations cannot qualify a sizing candidate. Observation
    // alone does not alter allocation sizes or require them for stereo playback.
    if(known && (!expanded || candidateHealthy))return BindReadiness::Ready;
    return initializing && !malformed?BindReadiness::Waiting:BindReadiness::Refused;
}
}
BindReadiness ReadyToBind(uintptr_t main){return InspectBinding(main,false);}
bool Bind(uintptr_t main){return InspectBinding(main,true)==BindReadiness::Ready;}
void BufferCreated(const D3D11_BUFFER_DESC* requested,HRESULT result,ID3D11Buffer* buffer,uintptr_t caller) {
    if(!enabled)return;
    ++createCalls;
    if(caller!=imageBase+0x9498d0 || !requested || requested->BindFlags!=D3D11_BIND_VERTEX_BUFFER ||
       requested->Usage!=D3D11_USAGE_DYNAMIC || requested->CPUAccessFlags!=D3D11_CPU_ACCESS_WRITE ||
       !requested->ByteWidth || requested->ByteWidth>MaxCapacity*Stride || requested->ByteWidth%Stride)return;
    D3D11_BUFFER_DESC actual{};if(SUCCEEDED(result) && buffer)buffer->GetDesc(&actual);
    if(SUCCEEDED(result) && buffer)buffers.Observe(buffer,{uintptr_t(buffer),requested->ByteWidth,actual.ByteWidth,
        actual.BindFlags,uint32_t(actual.Usage),actual.CPUAccessFlags,result});
    std::lock_guard lock(mutex);
    ++bufferObservations;
    if(bufferCount==MaxBufferLogEntries){truncated=true;return;}
    ++bufferCount;
    log << "{\"type\":\"gpu_created\",\"buffer\":" << uintptr_t(buffer) << ",\"requestedBytes\":" << requested->ByteWidth
        << ",\"actualBytes\":" << actual.ByteWidth << ",\"hr\":" << result << ",\"usage\":" << actual.Usage
        << ",\"bind\":" << actual.BindFlags << ",\"cpuAccess\":" << actual.CPUAccessFlags << "}\n";log.flush();
}

void Demand(uint64_t frame,int lane,int count) {
    if(!enabled || lane<0 || lane>=16 || count<=0)return;
    std::lock_guard lock(mutex);
    // Bound telemetry cost after the recorded prefix in a long headset session.
    if(!demand.Truncated())demand.Add(frame,lane,count);
}
void Status(std::ostream& output) {
    if(!enabled)return;
    std::lock_guard lock(mutex);
    output << ",\"instancePeakFrameDemand\":[";
    for(unsigned i=0;i<LaneCount;++i){if(i)output<<',';output<<demand.MaxDemand(i);}
    const auto tracked=buffers.Stats();
    output << "],\"instanceDemandTruncated\":" << (demand.Truncated()?"true":"false") << ",\"instanceGpuBuffers\":" << bufferCount
        << ",\"instanceBufferEvidenceTruncated\":" << (truncated?"true":"false")
        << ",\"instanceGpuBufferObservations\":" << bufferObservations
        << ",\"instanceTrackedGpuBuffers\":" << tracked.live << ",\"instancePeakTrackedGpuBuffers\":" << tracked.peak
        << ",\"instanceBufferTrackingFailures\":" << tracked.failures;
}
}
