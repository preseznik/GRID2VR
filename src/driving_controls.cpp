#include "driving_controls.h"
#include "driving_controls_model.h"
#include "driving_controls_instance.h"
#include "driving_controls_route.h"
#include "guarded_hooks.h"
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>

namespace grid2vr::drivingcontrols {
namespace {
using UpdateFn=unsigned (__thiscall*)(void*);
using ServiceUpdateFn=void (__thiscall*)(void*,const double*);
ServiceUpdateFn originalServiceUpdate{};
uintptr_t base{},owner{};void** shadowTable{};
std::filesystem::path folder;
std::atomic<bool> installed{},stopped{},failed{};
bool requested{};
std::mutex mutex;
std::vector<Binding> bindings;
std::vector<Override> identities;
std::vector<uint32_t> inputs;
uint32_t keyboardFamily{};
uint64_t updates{},refused{},stateWrites{},restored{},staleRestores{},scanBytes{},scanMilliseconds{},scanPasses{},attachments{};
const char* reason="not_requested";
constexpr uintptr_t inputTable=0xd0bc84;
constexpr uintptr_t methods[]{0x333530,0x3b5dd0,0x4a920,0x4d950,0x4d950,0x99bc30,0x58ddf0,0x15bd40,0x623c20,0x1090f0,0x693370,0xc7130,0x158410};
struct DeviceVector { void* allocator{};Device* begin{};Device* end{};unsigned capacity{};int limit=-1; };
struct Action { uint32_t allocator{},registry{},id{},vectorAllocator{};Axis* begin{};Axis* end{};unsigned capacity{};int limit=-1; };
static_assert(sizeof(Action)==0x20 && offsetof(Action,begin)==0x10);
struct Shadow { size_t binding{};Axis axis{}; };
struct MapState { uintptr_t map{},registry{},head{};uint64_t stamp{};std::vector<Shadow> shadows;bool ready{}; };
std::array<MapState,64> maps{};
struct DeviceCache { uintptr_t registry{};ULONGLONG refreshed{};std::vector<Device> devices; };
DeviceCache devices;
struct Coverage { unsigned carrierMaps{},aggregateMaps{},missingActionMaps{},missingCarrierMaps{},connectedDevices{},missingDeviceCarriers{};bool routed{},aggregateRouted{};float customValue{};bool customEdges{}; };
std::vector<Coverage> coverage;
struct Activity { uint64_t samples{},routedSamples{};float peak{}; };
std::vector<Activity> activity;
InputRoute route;
struct StatePatch { uintptr_t map{},registry{},head{},node{},address{};uint32_t action{};Axis native{},applied{}; };
std::vector<StatePatch> previous;
struct LiveMap { uintptr_t address{};uint32_t id{}; };
template<class T> T Function(uintptr_t rva) { return reinterpret_cast<T>(base+rva); }
// This reader is also used during the brief suspended-peer attachment interval.
// It deliberately has no profiling, logging, allocation or user-mode locks.
template<class T> bool Read(uintptr_t address,T& result) {
    SIZE_T copied{};return address>=0x10000 && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&result,sizeof(result),&copied) && copied==sizeof(result);
}
bool Heap(uintptr_t address,size_t bytes) {
    MEMORY_BASIC_INFORMATION region{};
    return address && bytes && address<=UINTPTR_MAX-bytes && VirtualQuery(reinterpret_cast<void*>(address),&region,sizeof(region)) &&
        region.Type==MEM_PRIVATE && region.State==MEM_COMMIT && region.Protect==PAGE_READWRITE &&
        address+bytes<=reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize;
}
bool Instance(uintptr_t object,void* table) {
    uintptr_t actual{},save{},xml{},selfA{},selfB{},selfC{},helper{},head{},lock{};uint32_t count{};uint8_t nil{};
    const auto library=object+0x84;
    return Heap(object,0x2f0) && Read(object,actual) && actual==reinterpret_cast<uintptr_t>(table) &&
        Read(object+0xc,save) && save==base+0xc3f364 && Read(object+0x78,xml) && xml==base+0xc56b50 &&
        Read(object+0x21c,selfA) && selfA==object && Read(object+0x224,selfB) && selfB==object &&
        Read(object+0x240,selfC) && selfC==object && Read(object+0x278,helper) && helper==library &&
        Read(library+0xb8,lock) && Heap(lock,sizeof(CRITICAL_SECTION)) &&
        Read(library+0x30,head) && Heap(head,0x78) && Read(head+0x75,nil) && nil==1 &&
        Read(library+0x34,count) && count<=64;
}
bool OriginalInstance(uintptr_t object) { uint32_t count{};return Instance(object,reinterpret_cast<void*>(base+inputTable)) && Read(object+0xb8,count) && count>0; }
void Status() {
    if(folder.empty())return;
    size_t active{},carriers{},routed{};for(const auto& map:maps)if(map.ready)active+=map.shadows.size();
    for(const auto& item:coverage) {carriers+=item.carrierMaps>0;routed+=item.routed;}
    std::ofstream file(folder/L"driving-controls-status.json",std::ios::trunc);
    file << "{\"installed\":" << (installed?"true":"false") << ",\"stopped\":" << (stopped?"true":"false")
         << ",\"failed\":" << (failed?"true":"false") << ",\"reason\":\"" << reason << "\",\"bindings\":" << bindings.size()
         << ",\"carrierBindings\":" << carriers << ",\"routedBindings\":" << routed
         << ",\"routingReady\":" << (installed && !failed && routed==bindings.size()?"true":"false")
         << ",\"gameplayAcceptance\":\"unverified\",\"consumedMapVerified\":" << (route.valid?"true":"false")
         << ",\"consumedMap\":" << (route.valid?std::to_string(route.map):"null")
         << ",\"mapUpdates\":" << updates << ",\"queries\":0,\"overriddenQueries\":0,\"nativeQueries\":0"
         << ",\"refused\":" << refused << ",\"devices\":" << devices.devices.size() << ",\"activeOverrideAxes\":" << active
         << ",\"gameBindingWrites\":0,\"gameCodeWrites\":0,\"sessionOnly\":true,\"hookStrategy\":\"input_instance_state\",\"callSites\":0"
         << ",\"diagnosticMode\":\"state_overlay\",\"stateWrites\":" << stateWrites << ",\"restoredStates\":" << restored
         << ",\"staleRestoresSkipped\":" << staleRestores << ",\"attachments\":" << attachments
         << ",\"discovery\":{\"passes\":" << scanPasses << ",\"bytes\":" << scanBytes << ",\"milliseconds\":" << scanMilliseconds << "},\"bindingStatus\":[";
    for(size_t i=0;i<bindings.size();++i) { if(i)file<<',';const auto& c=coverage[i];const auto& a=activity[i];
        const char* status=!c.connectedDevices?"pending_device":c.routed?"routed_state":!route.valid?"pending_gameplay_route":"missing_consumed_map_carrier";
        file << "{\"action\":\"" << bindings[i].action << "\",\"device\":\"" << bindings[i].device << "\",\"status\":\"" << status
             << "\",\"nativeCarrierMaps\":" << c.carrierMaps << ",\"aggregateCarrierMaps\":" << c.aggregateMaps
             << ",\"fallbackMap\":" << (c.aggregateRouted?std::to_string(route.map):"null")
             << ",\"nativeDeviceAttributionRetained\":" << (c.aggregateRouted?"true":"false")
             << ",\"missingActionMaps\":" << c.missingActionMaps << ",\"missingCarrierMaps\":" << c.missingCarrierMaps
             << ",\"connectedDevices\":" << c.connectedDevices << ",\"missingDeviceCarriers\":" << c.missingDeviceCarriers
             << ",\"customValue\":" << c.customValue << ",\"observedCustomSamples\":" << a.samples
             << ",\"routedCustomSamples\":" << a.routedSamples << ",\"peakCustomValue\":" << a.peak << '}';
    }file << "]}";
}
void Refuse(const char* why) { failed=true;reason=why;++refused; }
struct LibraryLock {
    uintptr_t lock;
    explicit LibraryLock(uintptr_t object):lock(object+0x84+0xb8){Function<void (__thiscall*)(void*)>(0x392680)(reinterpret_cast<void*>(lock));}
    ~LibraryLock(){Function<void (__thiscall*)(void*)>(0x648910)(reinterpret_cast<void*>(lock));}
};
uint32_t Intern(const std::string& value) {
    uint32_t id{};Function<void* (__thiscall*)(void*,const char*)>(0x643fa0)(&id,value.c_str());return id;
}
bool Compile() {
    if(!identities.empty())return true;
    keyboardFamily=Intern("keyboard");if(!keyboardFamily)return false;
    for(const auto& binding:bindings) {
        auto upper=binding.device;
        if(Guid(upper))for(auto& c:upper)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        identities.push_back({Intern(binding.action),Intern(binding.device),Intern(upper)});
        inputs.push_back(Intern(binding.input));
        if(!identities.back().action || !identities.back().family || !inputs.back())return false;
    }return true;
}
// Native red-black maps use unsigned interned IDs and the same four-word node
// prefix. Read only; neither the saved map nor any native vector is replaced.
bool Find(uintptr_t map,uint32_t key,uintptr_t& node) {
    uintptr_t head{},current{};node=0;
    if(!Read(map+4,head) || !head || !Read(head+4,current))return false;
    for(unsigned depth=0;depth<64;++depth) {
        if(current==head)return true;
        std::array<uint32_t,4> words{};
        if(!current || !Read(current,words))return false;
        if(words[3]==key) { node=current;return true; }
        current=words[key<words[3]?0:2];
    }return false;
}
bool Enumerate(uintptr_t registry) {
    const auto now=GetTickCount64();
    if(devices.registry==registry && now-devices.refreshed<500)return true;
    void* allocator{};if(!Read(registry,allocator) || !allocator)return false;
    DeviceVector native{allocator};
    const auto result=Function<unsigned (__thiscall*)(void*,DeviceVector*)>(0x1a0be0)(reinterpret_cast<void*>(registry),&native);
    const auto begin=reinterpret_cast<uintptr_t>(native.begin),end=reinterpret_cast<uintptr_t>(native.end);
    const bool shape=end>=begin && (end-begin)%sizeof(Device)==0 && (end-begin)/sizeof(Device)<=32 && native.capacity<=128;
    std::vector<Device> snapshot;
    bool valid=result==0 && shape;
    if(valid) {
        snapshot.resize((end-begin)/sizeof(Device));
        valid=snapshot.empty() || ReadMemory(native.begin,snapshot.data(),snapshot.size()*sizeof(Device));
        for(const auto& device:snapshot)valid=valid && device.backend && device.family && device.index!=UINT32_MAX;
    }
    // This is the exact release path used by the native enumerator itself.
    Function<unsigned (__thiscall*)(void*,unsigned,int)>(0x39e3a0)(&native,0,-1);
    if(!valid)return false;
    devices={registry,now,std::move(snapshot)};return true;
}
void Advance(void* object) {
    const auto map=reinterpret_cast<uintptr_t>(object);uintptr_t registry{},head{};
    if(!Read(map+4,registry) || !registry || !Read(map+0xc,head) || !head || !Compile() || !Enumerate(registry)) { Refuse("input_map_or_devices");return; }
    auto slot=std::find_if(maps.begin(),maps.end(),[&](const auto& s){return s.map==map;});
    if(slot==maps.end())slot=std::min_element(maps.begin(),maps.end(),[](const auto& a,const auto& b){return a.stamp<b.stamp;});
    if(slot->map!=map || slot->registry!=registry || slot->head!=head)*slot={map,registry,head};
    slot->ready=false;slot->stamp=updates;
    std::vector<Shadow> next;
    for(size_t index=0;index<bindings.size();++index)for(const auto& device:devices.devices) {
        const auto& identity=identities[index];
        if(device.family!=identity.family && device.family!=identity.familyUpper)continue;
        Shadow shadow{index};
        const auto prior=std::find_if(slot->shadows.begin(),slot->shadows.end(),[&](const auto& s){return s.binding==index && s.axis.device==device;});
        if(prior!=slot->shadows.end())shadow=*prior;
        else {
            shadow.axis.registry=static_cast<uint32_t>(registry);shadow.axis.input=inputs[index];shadow.axis.device=device;
            struct Calibration { uint32_t type;float dead,saturation,threshold; } calibration{bindings[index].calibration,bindings[index].dead,bindings[index].saturation,.5f};
            Function<void* (__thiscall*)(void*,const void*)>(0x310c0)(shadow.axis.calibration.data(),&calibration);
        }
        Action action{};action.registry=static_cast<uint32_t>(registry);action.id=identity.action;
        action.begin=&shadow.axis;action.end=&shadow.axis+1;action.capacity=1;
        // Poll and calibrate through GRID 2's existing device backend. Only this
        // private one-axis action receives the native value/edge-state writes.
        if(Function<UpdateFn>(0x1eb6d0)(&action)!=0 || !std::isfinite(shadow.axis.state.value) || shadow.axis.state.value<0 || shadow.axis.state.value>1.001f) { Refuse("native_axis_update");return; }
        next.push_back(shadow);
    }
    slot->shadows=std::move(next);slot->ready=true;
}

bool LibraryMaps(uintptr_t object,std::vector<LiveMap>& output) {
    uintptr_t head{},root{};uint32_t count{};
    if(!Read(object+0x84+0x30,head) || !Read(object+0x84+0x34,count) || count>64 || !Read(head+4,root))return false;
    std::array<uintptr_t,64> pending{},seen{};size_t queued{},visited{};
    if(root!=head)pending[queued++]=root;
    while(queued) {
        const auto node=pending[--queued];
        if(visited>=count || std::find(seen.begin(),seen.begin()+visited,node)!=seen.begin()+visited)return false;
        seen[visited++]=node;
        std::array<uint32_t,4> words{};uint8_t nil{};
        if(!Read(node,words) || !Read(node+0x75,nil) || nil)return false;
        output.push_back({node+0x10,words[3]});
        for(auto child:{words[0],words[2]})if(child!=head) {if(queued==pending.size())return false;pending[queued++]=child;}
    }return visited==count;
}
void RestoreStates(const std::vector<LiveMap>& live) {
    for(const auto& patch:previous) {
        uintptr_t registry{},head{},node{};std::array<uint32_t,2> range{};Axis current{};
        const bool valid=std::any_of(live.begin(),live.end(),[&](const auto& m){return m.address==patch.map;}) &&
            Read(patch.map+4,registry) && registry==patch.registry && Read(patch.map+0xc,head) && head==patch.head &&
            Find(patch.map+8,patch.action,node) && node==patch.node && Read(node+0x20,range) &&
            patch.address>=range[0] && patch.address+sizeof(Axis)<=range[1] && (patch.address-range[0])%sizeof(Axis)==0 &&
            Heap(patch.address,sizeof(Axis)) && Read(patch.address,current) && RestoreOverlay(current,patch.native,patch.applied);
        if(valid) {std::memcpy(reinterpret_cast<void*>(patch.address+offsetof(Axis,state)),&current.state,sizeof(State));++restored;}
        else ++staleRestores;
    }
    previous.clear();
}
bool PrepareStates(const std::vector<LiveMap>& live) {
    coverage.assign(bindings.size(),{});
    route=ReadInputRoute(base,[](uintptr_t address,void* output,size_t bytes){
        return ReadMemory(reinterpret_cast<void*>(address),output,bytes);});
    route.valid=route.valid && route.input==owner && std::any_of(live.begin(),live.end(),[](const auto& item){return item.id==route.map;});
    std::vector<StatePatch> next;
    for(const auto& item:live) {
        Advance(reinterpret_cast<void*>(item.address));if(failed)return false;
        const auto found=std::find_if(maps.begin(),maps.end(),[&](const auto& m){return m.map==item.address && m.ready;});
        if(found==maps.end())return false;
        std::vector<uint32_t> processed;
        for(size_t index=0;index<bindings.size();++index) {
            const auto id=identities[index].action;
            if(std::find(processed.begin(),processed.end(),id)!=processed.end())continue;
            processed.push_back(id);
            uintptr_t node{};if(!Find(item.address+8,id,node))return false;
            std::array<uint32_t,2> range{};std::vector<Axis> native,applied;
            if(node) {
                if(!Read(node+0x20,range) || range[1]<range[0] || (range[1]-range[0])%sizeof(Axis) || (range[1]-range[0])/sizeof(Axis)>256)return false;
                native.resize((range[1]-range[0])/sizeof(Axis));
                if(!native.empty() && (!Heap(range[0],native.size()*sizeof(Axis)) || !ReadMemory(reinterpret_cast<void*>(range[0]),native.data(),native.size()*sizeof(Axis))))return false;
            }
            applied=native;
            // Replace exact device families first; then add the missing Xbox
            // family to the proven gameplay map without erasing keyboard input.
            for(size_t binding=0;binding<bindings.size();++binding)if(identities[binding].action==id) {
                auto& report=coverage[binding];std::vector<Axis> shadows;
                for(const auto& s:found->shadows)if(s.binding==binding)shadows.push_back(s.axis);
                report.connectedDevices=std::max(report.connectedDevices,static_cast<unsigned>(shadows.size()));
                for(const auto& shadow:shadows) {report.customValue=std::max(report.customValue,shadow.state.value);
                    report.customEdges|=shadow.state.held || shadow.state.pressed || shadow.state.released;}
                if(!node || native.empty()){++report.missingActionMaps;continue;}
                const auto carriers=OverlayFamily(applied,shadows,identities[binding]);
                if(carriers)++report.carrierMaps;else ++report.missingCarrierMaps;
                if(route.valid && item.id==route.map && carriers && !shadows.empty() &&
                    std::all_of(shadows.begin(),shadows.end(),[&](const auto& shadow){return HasCarrier(native,shadow.device);}))report.routed=true;
                if(carriers)for(const auto& shadow:shadows)if(!HasCarrier(native,shadow.device))++report.missingDeviceCarriers;
            }
            if(route.valid && item.id==route.map)for(size_t binding=0;binding<bindings.size();++binding)
                if(identities[binding].action==id && bindings[binding].device=="xinput_pad") {
                    std::vector<Axis> shadows;for(const auto& s:found->shadows)if(s.binding==binding)shadows.push_back(s.axis);
                    if(OverlayMissingFamily(applied,shadows,identities[binding],keyboardFamily)) {
                        auto& report=coverage[binding];++report.aggregateMaps;report.routed=true;report.aggregateRouted=true;
                    }
                }
            for(size_t axis=0;axis<native.size();++axis)if(!SameState(native[axis].state,applied[axis].state)) {
                if(next.size()>=4096)return false;
                next.push_back({item.address,found->registry,found->head,node,range[0]+axis*sizeof(Axis),id,native[axis],applied[axis]});
            }
        }
    }
    // All allocations and guards precede any write. The native library lock
    // keeps action maps, vectors, save operations and queries stable here.
    previous=std::move(next);
    for(const auto& patch:previous) {
        std::memcpy(reinterpret_cast<void*>(patch.address+offsetof(Axis,state)),&patch.applied.state,sizeof(State));++stateWrites;
    }
    for(size_t i=0;i<coverage.size();++i) {
        const auto& c=coverage[i];auto& a=activity[i];a.peak=std::max(a.peak,c.customValue);
        if(c.customValue>0 || c.customEdges) {++a.samples;if(c.routed)++a.routedSamples;}
    }
    reason="state_overrides_active";return true;
}
void __fastcall ServiceUpdate(void* self,void*,const double* delta) {
    std::lock_guard guard(mutex);
    const auto object=reinterpret_cast<uintptr_t>(self);
    if(object!=owner || !Instance(object,shadowTable)) {originalServiceUpdate(self,delta);return;}
    std::vector<LiveMap> live;
    try {LibraryLock lock(object);if(!LibraryMaps(object,live))Refuse("input_map_tree");else RestoreStates(live);}
    catch(...) {Refuse("input_restore_exception");}
    // Do not hold the library lock while native device polling acquires its
    // backend locks. This preserves the game's existing lock order. Always
    // forward exactly once, including any observer allocation failure.
    originalServiceUpdate(self,delta);
    if(stopped || failed){if(failed)Status();return;}
    ++updates;live.clear();
    try {LibraryLock lock(object);if(!LibraryMaps(object,live) || !PrepareStates(live))Refuse("input_state_shape");}
    catch(...) {Refuse("input_state_exception");}
    if(updates==1 || updates%600==0 || failed)Status();
}
bool Guards() {
    struct Guard { uintptr_t rva;std::array<uint8_t,16> bytes; };
    constexpr Guard guards[]{
        {0x791ba0,{0x53,0x56,0x57,0x8b,0x79,0x0c,0x8b,0x37,0x33,0xdb,0x3b,0xf7,0x74,0x5d,0x8b,0xff}},
        {0x3f1f60,{0x55,0x8b,0xec,0x51,0xc5,0xf8,0x57,0xc0,0x56,0x57,0x8b,0x7d,0x08,0x8b,0xf1,0x57}},
        {0x5e5870,{0x55,0x8b,0xec,0x53,0x56,0x57,0x8b,0x7d,0x08,0x8b,0xf1,0x57,0x8d,0x45,0x08,0x50}},
        {0x28e7e0,{0x55,0x8b,0xec,0x53,0x56,0x57,0x8b,0x7d,0x08,0x8b,0xf1,0x57,0x8d,0x45,0x08,0x50}},
        {0x7a60c0,{0x55,0x8b,0xec,0x53,0x56,0x57,0x8b,0x7d,0x08,0x8b,0xf1,0x57,0x8d,0x45,0x08,0x50}},
        {0x643fa0,{0x55,0x8b,0xec,0x8b,0x45,0x08,0x56,0x8b,0xf1,0xc7,0x06,0x00,0x00,0x00,0x00,0x85}},
        {0x310c0,{0x55,0x8b,0xec,0x8b,0x45,0x08,0x3b,0xc8,0x74,0x16,0x8b,0x10,0x89,0x11,0xd9,0x40}},
        {0x1eb6d0,{0x55,0x8b,0xec,0x83,0xec,0x08,0x56,0x8b,0x71,0x10,0x8b,0x49,0x14,0x89,0x4d,0xf8}},
        {0x1a0be0,{0x55,0x8b,0xec,0x83,0xec,0x18,0x53,0x56,0x8b,0x75,0x08,0x8b,0x46,0x04,0x8b,0xd9}},
        {0x1aab50,{0x55,0x8b,0xec,0x83,0xec,0x34,0x56,0x8b,0xf1,0x80,0x7e,0x40,0x00,0x0f,0x84,0x43}},
        {0x39e3a0,{0x55,0x8b,0xec,0x83,0xec,0x0c,0x53,0x8b,0x5d,0x08,0x56,0x8b,0xf1,0x8b,0x46,0x0c}},
        {0x1eb731,{0x8b,0x52,0x1c,0x8d,0x4e,0x04,0x51,0x57,0x8b,0xc8,0xff,0xd2,0xd9,0x55,0xfc,0xd9}},
        {0x1eb763,{0xd9,0x56,0x38,0xd9,0x46,0x20,0x8a,0x56,0x3c,0xd9,0xc9,0xdf,0xf1,0xdd,0xd8,0x76}},
        {0x3f1fb5,{0xc5,0xfa,0x10,0x40,0x38,0xc5,0xf8,0x2f,0xc1,0x76,0x04,0xc5,0xf2,0x10,0xc8,0x83}},
        {0x5e58c4,{0x8a,0x41,0x3c,0x83,0xc1,0x40,0x3b,0xca,0x75,0xf2,0x5f,0x5e,0x5b,0x5d,0xc2,0x04}},
        {0x28e834,{0x8a,0x41,0x3d,0x83,0xc1,0x40,0x3b,0xca,0x75,0xf2,0x5f,0x5e,0x5b,0x5d,0xc2,0x04}},
        {0x7a6114,{0x8a,0x41,0x3e,0x83,0xc1,0x40,0x3b,0xca,0x75,0xf2,0x5f,0x5e,0x5b,0x5d,0xc2,0x04}}
    };
    for(const auto& guard:guards)if(!MatchesInstructions(reinterpret_cast<void*>(base+guard.rva),guard.bytes))return false;
    if(!MatchesInstructions(reinterpret_cast<void*>(base+0x392680),LockPrologue(static_cast<uint32_t>(base+0xb16238))) ||
       !MatchesInstructions(reinterpret_cast<void*>(base+0x648910),LockPrologue(static_cast<uint32_t>(base+0xb1623c))))return false;

    constexpr uint8_t service[]{0x55,0x8b,0xec,0x8b,0x45,0x08,0xc5,0xfb,0x10,0x00,0xc5,0xf0,0x57,0xc9,0xc5,0xf9};
    constexpr uint8_t calibration[]{0x8b,0x4d,0x10,0x83,0xc0,0x14,0x3b,0xc8,0x74,0x16,0x8b,0x10,0x89,0x11};
    if(!MatchesInstructions(reinterpret_cast<void*>(base+0x3b5dd0),service) ||
       !MatchesInstructions(reinterpret_cast<void*>(base+0x2c2912),calibration))return false;
    for(size_t i=0;i<std::size(methods);++i) {uintptr_t pointer{};if(!Read(base+inputTable+4*i,pointer) || pointer!=base+methods[i])return false;}
    return true;
}
uintptr_t Discover() {
    // Discovery is sliced across Present calls (<=8 MiB per slice) and runs
    // only while unattached. A complete sweep must identify exactly one Input2.
    static uintptr_t cursor{},candidate{};static unsigned matches{};
    static uint64_t bytes{};static ULONGLONG retryAfter{};
    if(GetTickCount64()<retryAfter)return 0;
    std::array<uint32_t,4096> words{};uint64_t slice{};const auto started=GetTickCount64();
    bool complete{};
    while(slice<8*1024*1024 && GetTickCount64()-started<4) {
        MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQuery(reinterpret_cast<void*>(cursor),&region,sizeof(region))) {complete=true;break;}
        const auto start=reinterpret_cast<uintptr_t>(region.BaseAddress);
        const uint64_t end=uint64_t(start)+region.RegionSize;
        if(region.State!=MEM_COMMIT || region.Type!=MEM_PRIVATE || region.Protect!=PAGE_READWRITE) {
            if(end>UINTPTR_MAX || end<=cursor){complete=true;break;}cursor=static_cast<uintptr_t>(end);continue;
        }
        const auto amount=std::min<size_t>(sizeof(words),static_cast<size_t>(end-cursor));
        SIZE_T copied{};
        if(ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(cursor),words.data(),amount,&copied) && copied==amount)
            for(size_t i=0;i<amount/4;++i)if(words[i]==base+inputTable) {
                const auto object=cursor+i*4;
                if(OriginalInstance(object) && object!=candidate){candidate=object;++matches;}
            }
        slice+=amount;bytes+=amount;scanBytes+=amount;
        if(bytes>2ull*1024*1024*1024){Refuse("input_discovery_bound");matches=0;complete=true;break;}
        if(uint64_t(cursor)+amount>UINTPTR_MAX){complete=true;break;}
        cursor+=static_cast<uintptr_t>(amount);
    }
    scanMilliseconds+=GetTickCount64()-started;
    if(!complete)return 0;
    ++scanPasses;const auto result=matches==1?candidate:0;
    cursor=0;candidate=0;matches=0;bytes=0;retryAfter=GetTickCount64()+2000;
    return result;
}
}
void Install(uintptr_t gameBase,const std::filesystem::path& output) {
    static bool attempted{};if(attempted)return;attempted=true;
    std::ifstream request(output/L"driving-controls.request",std::ios::binary);if(!request)return;
    base=gameBase;folder=output;
    if(std::filesystem::file_size(output/L"driving-controls.request")>131072){Refuse("request_size");Status();return;}
    const std::string text((std::istreambuf_iterator<char>(request)),{});
    if(!std::filesystem::exists(output/L"driving-controls.xml") || !Parse(text,bindings)){Refuse("invalid_request");Status();return;}
    coverage.resize(bindings.size());activity.resize(bindings.size());
    if(!Guards()){Refuse("input_instruction_guard");Status();return;}
    originalServiceUpdate=Function<ServiceUpdateFn>(0x3b5dd0);
    // Retain this private table for process lifetime. Native destruction restores
    // the original/base tables itself; no native allocator ever owns this table.
    auto** allocation=static_cast<void**>(VirtualAlloc(nullptr,14*sizeof(void*),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!allocation || !ReadMemory(reinterpret_cast<void*>(base+inputTable-4),allocation,14*sizeof(void*))) {
        if(allocation)VirtualFree(allocation,0,MEM_RELEASE);Refuse("input_shadow_allocation");Status();return;}
    allocation[2]=reinterpret_cast<void*>(ServiceUpdate);DWORD old{};
    if(!VirtualProtect(allocation,14*sizeof(void*),PAGE_READONLY,&old)){VirtualFree(allocation,0,MEM_RELEASE);Refuse("input_shadow_protection");Status();return;}
    shadowTable=allocation+1;requested=true;reason="waiting_for_input_instance";Status();
}
void Tick() {
    if(!requested || stopped || failed)return;
    std::lock_guard guard(mutex);
    if(owner && Instance(owner,shadowTable))return;
    if(owner) {owner=0;installed=false;previous.clear();maps={};devices={};coverage.assign(bindings.size(),{});reason="waiting_for_input_replacement";Status();}
    const auto object=Discover();if(!object)return;
    const auto failure=AttachInputInstance(reinterpret_cast<void*>(object),reinterpret_cast<void*>(base+inputTable),shadowTable,base,OriginalInstance);
    if(failure){reason=failure;Status();return;}
    owner=object;installed=true;++attachments;reason="waiting_for_input_update";Status();
}
void Stop() {stopped=true;std::lock_guard guard(mutex);Status();}
}
