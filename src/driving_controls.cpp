#include "driving_controls.h"
#include "driving_controls_model.h"
#include "driving_controls_instance.h"
#include "driving_controls_route.h"
#include "driving_controls_input.h"
#include "driving_controls_producer.h"
#include "driving_controls_steering.h"
#include "driving_controls_shift.h"
#include "device_claim_model.h"
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
ServiceUpdateFn originalDriverUpdate{};
BindingInput physicalInput;
bool driverHook{};
void** driverShadow{};uintptr_t driverObject{};
const char* driverReason="not_requested";
uint64_t driverEntered{},driverCalls{},driverApplied{},driverAttachments{},pedalMenuStates{};
enum class OutputGate { Ready,Stopped,Failed,NotInstalled,Stale,Unfocused,Route,Inactive,MapDisabled,InputDisabled };
constexpr const char* outputGateNames[]{"ready","stopped","failed","not_installed","stale_sample","unfocused","route_changed","inactive_controller","map_disabled","input_disabled"};
std::array<uint64_t,std::size(outputGateNames)> outputGates{};
const char* outputGate="waiting_for_driver";
void CountOutputGate(OutputGate gate) {const auto index=static_cast<size_t>(gate);outputGate=outputGateNames[index];++outputGates[index];}
struct ClutchOutput {
    uint64_t samples{},fullSamples{},endStopSamples{};
    float custom{},native{},output{},customPeak{},nativePeak{},outputPeak{};
    uint32_t transmission{},selection{},forced{};int32_t gear{};
} clutchOutput;
ULONGLONG sampledAt{};
uintptr_t base{},owner{};void** shadowTable{};
std::filesystem::path folder;
std::atomic<bool> installed{},stopped{},failed{};
bool requested{};
std::mutex mutex;
std::vector<Binding> bindings;
DrivingSettings settings;
bool wheelConfigured{},identitiesCompiled{};
std::array<uint32_t,2> steeringActions{};
std::array<float,2> nativeOtherSteering{};
WheelCurveOverride curveOverride;
uint64_t transmissionUpdates{},wheelUpdates{},curveRestores{};
const char* steeringSource="native";
std::vector<Override> identities;
std::vector<uint32_t> inputs;
uint32_t keyboardFamily{};
std::array<uint32_t,7> menuActions{};
uint64_t updates{},refused{},stateWrites{},restored{},staleRestores{},scanBytes{},scanMilliseconds{},scanPasses{},attachments{};
const char* reason="not_requested";
// Diagnostic: why the local driving route was last refused, and how often per step.
RouteProbe routeProbe;
std::vector<std::pair<std::string,uint64_t>> routeRefusals;
void CountRouteRefusal(const char* step) {
    for(auto& entry:routeRefusals)if(entry.first==step){++entry.second;return;}
    if(routeRefusals.size()<32)routeRefusals.emplace_back(step,1);
}
constexpr uintptr_t inputTable=0xd0bc84;
constexpr uintptr_t methods[]{0x333530,0x3b5dd0,0x4a920,0x4d950,0x4d950,0x99bc30,0x58ddf0,0x15bd40,0x623c20,0x1090f0,0x693370,0xc7130,0x158410};
constexpr uintptr_t driverTable=0xd0ba08;
constexpr uintptr_t driverMethods[]{0x333c40,0x9845a0,0x9845a0,0x1aab50,0x37b100,0x405800,0xc04d0,0x4a920,0x4911f0,0x6187b0,0x5d2500,0x6f4bb0,0x163d40};
constexpr InstanceBusyRange driverBusy[]{{0xce6a0,0xce74b},{0x6012a0,0x601347},{0x333c40,0x333c62},
    {0x1aab50,0x1ab2ad},{0x5a1d90,0x5a1db1},{0x63740,0x63807}};
InputRoute driverCandidate;uintptr_t driverExpected{};
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
// Direct-launch device claim (device_claim_model.h). Runs in Input2 Update;
// claim-only sessions attach the same private Input2 table without bindings.
struct Claim {
    bool requested{},pending{},only{},called{};ClaimRequest request;std::vector<ClaimCandidate> candidates;
    const char* reason="not_requested";const char* matched="";
    uint64_t updates{};ULONGLONG routeSince{};DWORD thread{};
    uint32_t map{},saveMap{},activeMap{},result{};Device device{};std::vector<uint32_t> before,after,mapsSeen;
} claim;
std::array<uint32_t,2> claimActions{};
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
    const auto shiftStatus=shift::Snapshot();
    size_t active{},carriers{},routed{};for(const auto& map:maps)if(map.ready)active+=map.shadows.size();
    for(const auto& item:coverage) {carriers+=item.carrierMaps>0;routed+=item.routed;}
    std::ofstream file(folder/L"driving-controls-status.json",std::ios::trunc);
    file << "{\"installed\":" << (installed?"true":"false") << ",\"stopped\":" << (stopped?"true":"false")
         << ",\"failed\":" << (failed?"true":"false") << ",\"reason\":\"" << reason << "\",\"bindings\":" << bindings.size()
         << ",\"carrierBindings\":" << carriers << ",\"routedBindings\":" << routed
         << ",\"routingReady\":" << (installed && !failed && !stopped && routed==bindings.size()?"true":"false")
         << ",\"gameplayAcceptance\":\"unverified\",\"consumedMapVerified\":" << (route.valid?"true":"false")
         << ",\"consumedMap\":" << (route.valid?std::to_string(route.map):"null")
         << ",\"mapUpdates\":" << updates << ",\"queries\":0,\"overriddenQueries\":0,\"nativeQueries\":0"
         << ",\"refused\":" << refused << ",\"devices\":" << devices.devices.size() << ",\"activeOverrideAxes\":" << active
         << ",\"gameBindingWrites\":0,\"gameCodeWrites\":0,\"gameImageTableWrites\":0,\"sessionOnly\":true,\"hookStrategy\":\"private_input_and_driver_instances\",\"callSites\":0"
         << ",\"driverHook\":" << (driverHook?"true":"false") << ",\"driverUpdates\":" << driverCalls << ",\"driverOutputWrites\":" << driverApplied
         << ",\"driverEntered\":" << driverEntered << ",\"driverAttachments\":" << driverAttachments << ",\"driverReason\":\"" << driverReason << '"'
         << ",\"driverOutputGate\":\"" << outputGate << "\",\"driverOutputGates\":{";
    for(size_t i=0;i<outputGates.size();++i){if(i)file<<',';file<<'"'<<outputGateNames[i]<<"\":"<<outputGates[i];}
    file << "},\"clutchOutput\":{\"samples\":" << clutchOutput.samples << ",\"custom\":" << clutchOutput.custom
         << ",\"native\":" << clutchOutput.native << ",\"output\":" << clutchOutput.output
         << ",\"customPeak\":" << clutchOutput.customPeak << ",\"nativePeak\":" << clutchOutput.nativePeak << ",\"outputPeak\":" << clutchOutput.outputPeak
         << ",\"fullSamples\":" << clutchOutput.fullSamples << ",\"endStopSamples\":" << clutchOutput.endStopSamples
         << ",\"transmission\":" << clutchOutput.transmission << ",\"selection\":" << clutchOutput.selection
         << ",\"forced\":" << clutchOutput.forced << ",\"gear\":" << clutchOutput.gear << '}'
         << ",\"transmission\":\"" << TransmissionNames[static_cast<size_t>(settings.transmission)] << "\",\"transmissionUpdates\":" << transmissionUpdates
         << ",\"clutchShift\":{\"threshold\":" << shiftStatus.threshold << ",\"enabled\":" << (shiftStatus.enabled?"true":"false")
         << ",\"ready\":" << (shiftStatus.ready?"true":"false") << ",\"reason\":\"" << shiftStatus.reason << "\",\"attached\":" << shiftStatus.attached
         << ",\"callbacks\":" << shiftStatus.callbacks << ",\"localJobs\":" << shiftStatus.localJobs << ",\"clonedJobs\":" << shiftStatus.clonedJobs
         << ",\"refused\":" << shiftStatus.refused << ",\"gameDataInstanceWrites\":" << shiftStatus.gameDataInstanceWrites << ",\"restores\":" << shiftStatus.restores << '}'
         << ",\"steeringLinearity\":" << settings.steeringLinearity << ",\"steeringSource\":\"" << steeringSource << "\",\"wheelUpdates\":" << wheelUpdates << ",\"curveRestores\":" << curveRestores
         << ",\"curveRestorePending\":" << (curveOverride.object?"true":"false")
         << ",\"suppressedPedalMenuStates\":" << pedalMenuStates
         << ",\"diagnosticMode\":\"state_overlay\",\"stateWrites\":" << stateWrites << ",\"restoredStates\":" << restored
         << ",\"staleRestoresSkipped\":" << staleRestores << ",\"attachments\":" << attachments
         << ",\"routeProbe\":{\"step\":\"" << (route.valid?"valid":routeProbe.step) << "\",\"parentTableRva\":" << routeProbe.parentTable
         << ",\"nodes\":" << routeProbe.nodes << ",\"roles\":[";
    for(uint32_t i=0;i<std::min<uint32_t>(routeProbe.roles,8);++i){if(i)file<<',';file<<routeProbe.roleValues[i];}
    file << "],\"controllerBytes\":" << routeProbe.controllerBytes << ",\"controllerTableRva\":" << routeProbe.controllerTable << ",\"refusals\":{";
    for(size_t i=0;i<routeRefusals.size();++i){if(i)file<<',';file<<'"'<<routeRefusals[i].first<<"\":"<<routeRefusals[i].second;}
    file << "}}"
         << ",\"discovery\":{\"passes\":" << scanPasses << ",\"bytes\":" << scanBytes << ",\"milliseconds\":" << scanMilliseconds << "},\"bindingStatus\":[";
    for(size_t i=0;i<bindings.size();++i) { if(i)file<<',';const auto& c=coverage[i];const auto& a=activity[i];
        const bool physicalConnected=physicalInput.Samples().size()>i && physicalInput.Samples()[i].connected;
        const char* status=!c.connectedDevices && !physicalConnected?"pending_device":c.routed?"routed_state":!route.valid?"pending_gameplay_route":"missing_consumed_map_carrier";
        file << "{\"action\":\"" << bindings[i].action << "\",\"device\":\"" << bindings[i].device << "\",\"status\":\"" << status
             << "\",\"deviceId\":\"" << bindings[i].deviceId
             << "\",\"sampling\":\"" << (physicalInput.Samples().size()>i && physicalInput.Samples()[i].supported?physicalInput.Samples()[i].status:bindings[i].deviceId.empty()?"legacy_native_family":"released")
             << "\",\"sampleError\":" << (physicalInput.Samples().size()>i?physicalInput.Samples()[i].error:0)
             << ",\"driverFiltering\":\"" << (physicalInput.Samples().size()>i?physicalInput.Samples()[i].driverFiltering:"unavailable") << '"'
             << ",\"rawInput\":" << (physicalInput.Samples().size()>i?physicalInput.Samples()[i].raw:0)
             << ",\"calibratedInput\":" << (physicalInput.Samples().size()>i?physicalInput.Samples()[i].calibrated:0)
             << ",\"physicalConnected\":" << (physicalConnected?"true":"false")
             << ",\"nativeCarrierMaps\":" << c.carrierMaps << ",\"aggregateCarrierMaps\":" << c.aggregateMaps
             << ",\"fallbackMap\":" << (c.aggregateRouted?std::to_string(route.map):"null")
             << ",\"nativeDeviceAttributionRetained\":true"
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
    if(identitiesCompiled)return true;
    keyboardFamily=Intern("keyboard");if(!keyboardFamily)return false;
    steeringActions={Intern("Steer Left"),Intern("Steer Right")};if(!steeringActions[0] || !steeringActions[1])return false;
    size_t menu{};for(auto name:{"Menu Up","Menu Down","Menu Left","Menu Right","Menu Select","Menu Back","Menu Start Button"}) {
        menuActions[menu]=Intern(name);if(!menuActions[menu++])return false;
    }
    for(const auto& binding:bindings) {
        auto upper=binding.device;
        if(Guid(upper))for(auto& c:upper)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        identities.push_back({Intern(binding.action),Intern(binding.device),Intern(upper)});
        inputs.push_back(Intern(binding.input));
        if(!identities.back().action || !identities.back().family || !inputs.back())return false;
    }identitiesCompiled=true;return true;
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
    for(size_t index=0;index<bindings.size();++index) {
      const auto& sample=physicalInput.Samples()[index];
      if(sample.supported) {
        if(sample.connected) {
            Shadow shadow{index};shadow.axis.registry=static_cast<uint32_t>(registry);shadow.axis.input=inputs[index];
            shadow.axis.device={0,identities[index].family,static_cast<uint32_t>(index)};shadow.axis.state=sample.state;
            next.push_back(shadow);
        }
        continue;
      }
      for(const auto& device:devices.devices) {
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
    nativeOtherSteering={};
    route=ReadInputRoute(base,[](uintptr_t address,void* output,size_t bytes){
        return ReadMemory(reinterpret_cast<void*>(address),output,bytes);},&routeProbe);
    if(route.valid && route.input!=owner)routeProbe.step="foreign_input";
    else if(route.valid && std::none_of(live.begin(),live.end(),[](const auto& item){return item.id==route.map;}))routeProbe.step="map_not_live";
    route.valid=route.valid && route.input==owner && std::any_of(live.begin(),live.end(),[](const auto& item){return item.id==route.map;});
    if(!route.valid)CountRouteRefusal(routeProbe.step);
    std::vector<StatePatch> next;
    for(const auto& item:live) {
        Advance(reinterpret_cast<void*>(item.address));if(failed)return false;
        const auto found=std::find_if(maps.begin(),maps.end(),[&](const auto& m){return m.map==item.address && m.ready;});
        if(found==maps.end())return false;
        std::vector<uint32_t> actions;
        for(const auto& identity:identities)if(std::find(actions.begin(),actions.end(),identity.action)==actions.end())actions.push_back(identity.action);
        if(wheelConfigured)for(auto id:steeringActions)if(std::find(actions.begin(),actions.end(),id)==actions.end())actions.push_back(id);
        // A legacy wheel preset may use a pedal for menu navigation. Suppress
        // only that pedal/axis alias, unless the user explicitly assigned it
        // as a menu binding. Buttons, hats and other families stay native.
        for(const auto id:menuActions) {
            if(std::find(actions.begin(),actions.end(),id)==actions.end())actions.push_back(id);
        }
        for(const auto id:actions) {
            const bool menuAction=std::find(menuActions.begin(),menuActions.end(),id)!=menuActions.end();
            bool replayAction{};
            for(size_t b=0;b<bindings.size();++b)if(identities[b].action==id && bindings[b].action.starts_with("Replay "))replayAction=true;
            // Driving values belong only to the proven local player's map.
            // Menu/replay actions retain native query eligibility in their own
            // contexts, where a local driving controller may not exist.
            if(!menuAction && !replayAction && (!route.valid || item.id!=route.map))continue;
            uintptr_t node{};if(!Find(item.address+8,id,node))return false;
            std::array<uint32_t,2> range{};std::vector<Axis> native,applied;
            if(node) {
                if(!Read(node+0x20,range) || range[1]<range[0] || (range[1]-range[0])%sizeof(Axis) || (range[1]-range[0])/sizeof(Axis)>256)return false;
                native.resize((range[1]-range[0])/sizeof(Axis));
                if(!native.empty() && (!Heap(range[0],native.size()*sizeof(Axis)) || !ReadMemory(reinterpret_cast<void*>(range[0]),native.data(),native.size()*sizeof(Axis))))return false;
            }
            applied=native;
            if(route.valid && item.id==route.map)for(size_t i=0;i<steeringActions.size();++i)
                if(steeringActions[i]==id) {
                    auto& value=nativeOtherSteering[i];
                    for(const auto& axis:native)if(!Replaces(identities,id,axis.device.family) && std::isfinite(axis.state.value))
                        value=std::max(value,std::clamp(axis.state.value,0.f,1.f));
                    break;
                }
            for(size_t b=0;b<bindings.size();++b)if(PedalAction(bindings[b].action) && DirectAxis(bindings[b].input)) {
                if(std::find(menuActions.begin(),menuActions.end(),id)==menuActions.end())continue;
                for(auto& axis:applied)if(axis.input==inputs[b] && (axis.device.family==identities[b].family || axis.device.family==identities[b].familyUpper)) {
                    const bool explicitMenu=std::any_of(identities.begin(),identities.end(),[&](const auto& entry){
                        return entry.action==id && (entry.family==axis.device.family || entry.familyUpper==axis.device.family);});
                    if(SuppressPedalAlias(axis,inputs[b],identities[b],explicitMenu))++pedalMenuStates;
                }
            }
            std::vector<Axis> combined;
            for(size_t binding=0;binding<bindings.size();++binding)if(identities[binding].action==id) {
                auto& report=coverage[binding];std::vector<Axis> shadows;
                for(const auto& s:found->shadows)if(s.binding==binding)shadows.push_back(s.axis);
                report.connectedDevices=std::max(report.connectedDevices,static_cast<unsigned>(shadows.size()));
                for(const auto& shadow:shadows) {report.customValue=std::max(report.customValue,shadow.state.value);
                    report.customEdges|=shadow.state.held || shadow.state.pressed || shadow.state.released;}
                if(!node || native.empty()){++report.missingActionMaps;continue;}
                const auto carriers=std::count_if(native.begin(),native.end(),[&](const auto& axis){return axis.device.family==identities[binding].family || axis.device.family==identities[binding].familyUpper;});
                if(carriers)++report.carrierMaps;else ++report.missingCarrierMaps;
                combined.insert(combined.end(),shadows.begin(),shadows.end());
                if(!shadows.empty()) {
                    ++report.aggregateMaps;
                    if((route.valid && item.id==route.map) || bindings[binding].action.starts_with("Menu ")) {
                        report.routed=true;report.aggregateRouted=true;
                    }
                }
            }
            if(std::any_of(identities.begin(),identities.end(),[&](const auto& entry){return entry.action==id;}))
                OverlayAction(applied,combined,identities,id);
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
        auto& c=coverage[i];
        uint32_t gearMode{};
        if(driverHook && route.valid && c.connectedDevices &&
           (bindings[i].action=="Clutch" || (HPatternIndex(bindings[i].action)>=0 &&
            ((settings.transmission==Transmission::HPattern || settings.transmission==Transmission::HPatternClutch) ||
             (settings.transmission==Transmission::Game && Read(route.controller+0x44,gearMode) && gearMode==2)))))c.routed=true;
        auto& a=activity[i];a.peak=std::max(a.peak,c.customValue);
        if(c.customValue>0 || c.customEdges) {++a.samples;if(c.routed)++a.routedSamples;}
    }
    reason=std::all_of(coverage.begin(),coverage.end(),[](const auto& c){return c.routed;})?"input_routes_ready":"input_routes_pending";return true;
}
void WriteClaim() {
    if(folder.empty() || !claim.requested)return;
    std::ofstream file(folder/L"device-claim.json",std::ios::trunc);
    const auto list=[&](const std::vector<uint32_t>& values){file<<'[';for(size_t i=0;i<values.size();++i){if(i)file<<',';file<<values[i];}file<<']';};
    file << "{\"schema\":1,\"mode\":\"" << (claim.request.automatic?"auto":"device") << "\",\"families\":[";
    for(size_t i=0;i<claim.request.families.size();++i){if(i)file<<',';file<<'"'<<claim.request.families[i]<<'"';}
    file << "],\"pending\":" << (claim.pending?"true":"false") << ",\"reason\":\"" << claim.reason << "\",\"claimOnly\":" << (claim.only?"true":"false")
         << ",\"inputThread\":" << claim.thread << ",\"updates\":" << claim.updates
         << ",\"consumedMap\":" << claim.map << ",\"saveMap\":" << claim.saveMap << ",\"activeMap\":" << claim.activeMap
         << ",\"keyboardFamily\":" << keyboardFamily << ",\"consumedMapsSeen\":";list(claim.mapsSeen);
    file << ",\"familiesBefore\":";list(claim.before);
    file << ",\"familiesAfter\":";list(claim.after);
    file << ",\"nativeCall\":" << (claim.called?"true":"false") << ",\"nativeResult\":" << claim.result
         << ",\"device\":{\"family\":" << claim.device.family << ",\"index\":" << claim.device.index << ",\"matched\":\"" << claim.matched << "\"}"
         << ",\"bindTarget\":\"Input2 5c1db0 (ScreenPressStart 7dcec4)\",\"gameCodeWrites\":0}\n";
}
void FinishClaim(const char* why) { claim.pending=false;claim.reason=why;WriteClaim(); }
bool ClaimCompile() {
    if(claimActions[0])return true;
    if(!Compile())return false;
    claimActions={Intern("Accelerate"),Intern("Brake")};
    for(const auto& family:claim.request.families) {
        auto upper=family;for(auto& c:upper)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        claim.candidates.push_back({Intern(family),Intern(upper)});
        if(!claim.candidates.back().family || !claim.candidates.back().familyUpper)return false;
    }
    return claimActions[0] && claimActions[1];
}
// Caller holds the library lock. Collects the device families bound to the
// driving actions of one live map; no native vector or binding is changed.
bool MapFamilies(uintptr_t object,uint32_t id,std::vector<uint32_t>& families,uintptr_t& registry) {
    std::vector<LiveMap> live;families.clear();
    if(!LibraryMaps(object,live))return false;
    const auto found=std::find_if(live.begin(),live.end(),[&](const auto& m){return m.id==id;});
    if(found==live.end() || !Read(found->address+4,registry) || !registry)return false;
    for(const auto action:{claimActions[0],claimActions[1],steeringActions[0],steeringActions[1]}) {
        uintptr_t node{};std::array<uint32_t,2> range{};
        if(!Find(found->address+8,action,node))return false;
        if(!node)continue;
        if(!Read(node+0x20,range) || range[1]<range[0] || (range[1]-range[0])%sizeof(Axis) || (range[1]-range[0])/sizeof(Axis)>256)return false;
        std::vector<Axis> axes((range[1]-range[0])/sizeof(Axis));
        if(!axes.empty() && (!Heap(range[0],axes.size()*sizeof(Axis)) || !ReadMemory(reinterpret_cast<void*>(range[0]),axes.data(),axes.size()*sizeof(Axis))))return false;
        for(const auto& axis:axes)if(!CarriesFamily(families,axis.device.family))families.push_back(axis.device.family);
    }
    return true;
}
void ClaimDevice(uintptr_t object) {
    if(!claim.pending || stopped)return;
    if(!claim.thread)claim.thread=GetCurrentThreadId();
    if(++claim.updates%15)return;
    const auto current=ReadInputRoute(base,[](uintptr_t at,void* output,size_t bytes){return ReadMemory(reinterpret_cast<void*>(at),output,bytes);});
    if(!current.valid || current.input!=object) {
        if(claim.reason!=std::string_view("waiting_for_local_controller")){claim.reason="waiting_for_local_controller";WriteClaim();}
        return;
    }
    if(!Read(object+0x24c,claim.saveMap) || !Read(object+0x250,claim.activeMap)){FinishClaim("input_maps_unreadable");return;}
    claim.map=current.map;
    if(std::find(claim.mapsSeen.begin(),claim.mapsSeen.end(),current.map)==claim.mapsSeen.end() && claim.mapsSeen.size()<8) {
        claim.mapsSeen.push_back(current.map);WriteClaim();
    }
    // Press Start binds the profile map, which the native save also uses. A
    // direct race first routes the controller through the combined any-device
    // map (FFFFFFFF) and moves it to the profile map later: wait, never bind
    // the combined map.
    if(claim.saveMap!=current.map) {
        if(claim.reason!=std::string_view("waiting_for_profile_map")){claim.reason="waiting_for_profile_map";WriteClaim();}
        return;
    }
    const auto now=GetTickCount64();if(!claim.routeSince)claim.routeSince=now;
    if(!ClaimCompile()){FinishClaim("claim_identity");return;}
    // Decide under the library lock; receipts are written after releasing it.
    const char* refusal{};std::optional<Device> selected;
    {
        LibraryLock lock(object);uintptr_t registry{};
        if(!MapFamilies(object,current.map,claim.before,registry))refusal="consumed_map_shape";
        else if(claim.before.empty())refusal="consumed_map_without_driving_actions";
        else if(!KeyboardOnly(claim.before,keyboardFamily))refusal="map_already_has_device";
        else if(!Enumerate(registry))refusal="device_enumeration";
        else selected=SelectClaimDevice(devices.devices,claim.candidates,keyboardFamily);
    }
    if(refusal){FinishClaim(refusal);return;}
    if(!selected) {
        if(now-claim.routeSince>30000){FinishClaim("device_not_connected");return;}
        if(claim.reason!=std::string_view("waiting_for_device")){claim.reason="waiting_for_device";WriteClaim();}
        return;
    }
    const auto device=*selected;
    claim.device=device;
    for(size_t i=0;i<claim.candidates.size();++i)
        if(device.family==claim.candidates[i].family || device.family==claim.candidates[i].familyUpper){claim.matched=claim.request.families[i].c_str();break;}
    // The receipt names the attempt before the call: a fault stays attributable.
    claim.reason="native_bind_called";claim.called=true;WriteClaim();
    // Exactly ScreenPressStart's bind (VA 7dcec4): Input2 this, profile map,
    // native device record. Outside the library lock; it locks natively.
    claim.result=Function<uint32_t (__thiscall*)(void*,uint32_t,const Device*)>(0x1c1db0)(reinterpret_cast<void*>(object),current.map,&device);
    bool shaped{};
    {LibraryLock lock(object);uintptr_t registry{};shaped=MapFamilies(object,current.map,claim.after,registry);}
    FinishClaim(!shaped?"claimed_map_shape":CarriesFamily(claim.after,device.family)?"claimed":"bind_left_map_keyboard_only");
}
void __fastcall ServiceUpdate(void* self,void*,const double* delta) {
    std::lock_guard guard(mutex);
    const auto object=reinterpret_cast<uintptr_t>(self);
    if(object!=owner || !Instance(object,shadowTable)) {originalServiceUpdate(self,delta);return;}
    if(claim.only) {originalServiceUpdate(self,delta);ClaimDevice(object);return;}
    std::vector<LiveMap> live;
    try {LibraryLock lock(object);if(!LibraryMaps(object,live))Refuse("input_map_tree");else RestoreStates(live);}
    catch(...) {Refuse("input_restore_exception");}
    // Do not hold the library lock while native device polling acquires its
    // backend locks. This preserves the game's existing lock order. Always
    // forward exactly once, including any observer allocation failure.
    originalServiceUpdate(self,delta);
    // Previous overlay states are already restored; the next preparation
    // rereads every live map, including any vectors the native bind replaced.
    ClaimDevice(object);
    if(stopped || failed){if(failed)Status();return;}
    ++updates;live.clear();
    try {physicalInput.Poll();sampledAt=GetTickCount64();
        LibraryLock lock(object);if(!LibraryMaps(object,live) || !PrepareStates(live))Refuse("input_state_shape");}
    catch(...) {Refuse("input_state_exception");}
    if(updates==1 || updates%600==0 || failed)Status();
}
void RestoreCurve(uintptr_t object) noexcept {
    if(!curveOverride.object || curveOverride.object!=object)return;
    const auto current=ReadInputRoute(base,[](uintptr_t at,void* data,size_t bytes){SIZE_T copied{};
        return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),data,bytes,&copied) && copied==bytes;});
    float value{},restore{};
    const bool valid=current.valid && current.controller==object && current.input==owner && Heap(object,0x6c) && Read(object+0x2c,value);
    if(RestoreWheelCurve(curveOverride,object,valid,value,restore)) {
        std::memcpy(reinterpret_cast<void*>(object+0x2c),&restore,4);++curveRestores;
    }
    if(valid)steeringSource="native";
}
void __fastcall DriverUpdate(void* self,void*,const double* delta) {
    // Always preserve the native update, including every unrelated controller,
    // disabled input, pause, replay and any failed guard. No simulation replay.
    const auto object=reinterpret_cast<uintptr_t>(self);
    std::array<uint8_t,0x6c> nativeSettings{},appliedSettings{};bool transmissionApplied{};
    {
        std::lock_guard guard(mutex);++driverEntered;RestoreCurve(object);
        shift::Permit({},false,0);
        const auto current=ReadInputRoute(base,[](uintptr_t at,void* data,size_t bytes){return ReadMemory(reinterpret_cast<void*>(at),data,bytes);});
        if(!stopped && !failed && installed && current.valid && current.controller==object && current.input==owner &&
           Instance(owner,shadowTable) && Heap(object,0x6c) && Read(object,nativeSettings)) {
            appliedSettings=nativeSettings;
            if(ApplyTransmission(appliedSettings,settings.transmission)) {
                std::memcpy(reinterpret_cast<void*>(object+4),appliedSettings.data()+4,4);
                std::memcpy(reinterpret_cast<void*>(object+0x44),appliedSettings.data()+0x44,4);
                transmissionApplied=true;++transmissionUpdates;
            }
        }
    }
    originalDriverUpdate(self,delta);
    std::lock_guard guard(mutex);++driverCalls;
    std::array<uint8_t,0x6c> before{};const bool read=Heap(object,0x6c) && Read(object,before);
    // Native update copied the requested mode into its volatile output and
    // queried the corresponding gear actions. Restore configuration immediately;
    // only output state carries the session choice into the physics consumer.
    if(transmissionApplied && read && ReadLocalControllerIdentity(base,object,[](uintptr_t at,void* data,size_t bytes){return ReadMemory(reinterpret_cast<void*>(at),data,bytes);}) &&
       !std::memcmp(before.data()+4,appliedSettings.data()+4,4) && !std::memcmp(before.data()+0x44,appliedSettings.data()+0x44,4)) {
        std::memcpy(reinterpret_cast<void*>(object+4),nativeSettings.data()+4,4);
        std::memcpy(reinterpret_cast<void*>(object+0x44),nativeSettings.data()+0x44,4);
    }
    if(stopped){CountOutputGate(OutputGate::Stopped);return;}
    if(failed){CountOutputGate(OutputGate::Failed);return;}
    if(!installed){CountOutputGate(OutputGate::NotInstalled);return;}
    if(!sampledAt || GetTickCount64()-sampledAt>250){CountOutputGate(OutputGate::Stale);return;}
    DWORD foreground{};GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
    if(foreground!=GetCurrentProcessId()){CountOutputGate(OutputGate::Unfocused);return;}
    const auto current=ReadInputRoute(base,[](uintptr_t at,void* data,size_t bytes){return ReadMemory(reinterpret_cast<void*>(at),data,bytes);});
    if(!current.valid || current.controller!=object || current.input!=owner || !SameInputRoute(route,current) || !Instance(owner,shadowTable) || !Heap(object,0x6c)){CountOutputGate(OutputGate::Route);return;}
    if(!read || before[0x40]!=1){CountOutputGate(OutputGate::Inactive);return;}
    // Use the same native eligibility gate as RVA 1aab72. Focus is an
    // additional gate for our physical-device sampling, never an override.
    if(!Function<bool (__thiscall*)(void*,unsigned)>(0x32a260)(reinterpret_cast<void*>(owner),current.map)){CountOutputGate(OutputGate::MapDisabled);return;}
    uint32_t blocked{};uint8_t enabled{};
    if(!Read(owner+0x2b0,blocked) || !Read(owner+0x248,enabled) || !NativeInputEnabled(blocked,enabled)){CountOutputGate(OutputGate::InputDisabled);return;}
    CountOutputGate(OutputGate::Ready);
    SupplementalInput custom;
    WheelSteeringInput steering;steering.other=nativeOtherSteering;
    for(size_t i=0;i<steeringActions.size();++i)
        if(!Function<bool (__thiscall*)(void*,unsigned,const uint32_t*)>(0x3bf8c0)(reinterpret_cast<void*>(owner),current.map,&steeringActions[i]))steering.other[i]=0;
    for(size_t i=0;i<bindings.size();++i) {
        auto& c=coverage[i];if(!c.connectedDevices)continue;
        const auto& action=bindings[i].action;
        const auto gear=HPatternIndex(action);
        const bool steer=action=="Steer Left" || action=="Steer Right";
        if(action!="Clutch" && gear<0 && !steer)continue;
        // The native query's per-action allow mask defaults to allowed for an
        // absent mask entry; it does not require a saved H-pattern binding.
        if(!Function<bool (__thiscall*)(void*,unsigned,const uint32_t*)>(0x3bf8c0)(reinterpret_cast<void*>(owner),current.map,&identities[i].action))continue;
        if(steer) {
            const bool wheel=Guid(bindings[i].device) && DirectAxis(bindings[i].input);
            auto& value=(wheel?steering.wheel:steering.other)[action=="Steer Right"?1:0];
            value=std::max(value,c.customValue);steering.connected|=wheel;
        }
        else if(action=="Clutch") {custom.clutchConnected=true;custom.clutch=std::max(custom.clutch,c.customValue);}
        else custom.gears[gear]=custom.gears[gear] || c.customValue>=.5f;
    }
    auto after=before;
    if(ApplySupplementalInput(after,custom)) {
        // Limit writes to the two proven output fields; retain the native
        // state machine, device type, controller/map pointers and FFB path.
        std::memcpy(reinterpret_cast<void*>(object+0x24),after.data()+0x24,4);
        std::memcpy(reinterpret_cast<void*>(object+0x30),after.data()+0x30,4);++driverApplied;
    }
    uint32_t outputTransmission{},forcedInput{};
    std::memcpy(&outputTransmission,after.data()+0x34,4);std::memcpy(&forcedInput,after.data()+0x3c,4);
    shift::Permit(current,settings.clutchShiftSettings && settings.transmission==Transmission::HPatternClutch &&
        outputTransmission==1 && forcedInput!=1,sampledAt);
    if(custom.clutchConnected) {
        float native{},output{};std::memcpy(&native,before.data()+0x30,4);std::memcpy(&output,after.data()+0x30,4);
        if(std::isfinite(native) && std::isfinite(output) && std::isfinite(custom.clutch)) {
            auto& c=clutchOutput;++c.samples;c.custom=custom.clutch;c.native=native;c.output=output;
            c.customPeak=std::max(c.customPeak,c.custom);c.nativePeak=std::max(c.nativePeak,native);c.outputPeak=std::max(c.outputPeak,output);
            c.fullSamples+=output==1.f;
            std::memcpy(&c.transmission,after.data()+0x34,4);std::memcpy(&c.selection,after.data()+0x44,4);
            std::memcpy(&c.forced,after.data()+0x3c,4);std::memcpy(&c.gear,after.data()+0x24,4);
            c.endStopSamples+=custom.clutch<1.f && native<1.f && output==1.f;
        }
    }
    // A route may have been temporarily unavailable before the native call.
    // Recover its owned curve now, never replacing an unresolved older record.
    RestoreCurve(object);
    float nativeCurve{};
    if(settings.wheelSettings && !curveOverride.object && Read(object+0x2c,nativeCurve) && std::isfinite(nativeCurve) && nativeCurve>=-1 && nativeCurve<=1 &&
       ApplyWheelSteering(after,steering,settings.steeringLinearity)) {
        float curve{};std::memcpy(&curve,after.data()+0x2c,4);curveOverride={object,nativeCurve,curve};
        std::memcpy(reinterpret_cast<void*>(object+0x28),after.data()+0x28,8);
        *reinterpret_cast<uint8_t*>(object+0x38)=after[0x38];++wheelUpdates;steeringSource="wheel";
    }
}
bool Guards() {
    struct OptionGuard { uintptr_t rva;std::initializer_list<uint8_t> bytes; };
    const OptionGuard options[]{
        {0x175ce5,{0xc7,0x43,0x04,0x04,0x00,0x00,0x00,0x89,0x4b,0x44}},
        {0x175cf8,{0x89,0x4b,0x04,0xc7,0x43,0x44,0x02,0x00,0x00,0x00}},
        {0x175d0a,{0xb8,0x02,0x00,0x00,0x00,0x5e,0x89,0x43,0x04,0x89,0x43,0x44}},
        {0x175d1f,{0x89,0x4b,0x44,0xc7,0x43,0x04,0x02,0x00,0x00,0x00}},
        {0x892a4a,{0xc5,0xf8,0x2e,0x05}}, {0x892a52,{0x9f,0xf6,0xc4,0x44,0x7b,0x0e}},
        {0xd76b6c,{0x00,0x00,0x80,0x3f}},
        {0x1aabbf,{0x8b,0x4e,0x04,0x8b,0x56,0x5c}}, {0x1aabcb,{0x89,0x4e,0x34}},
        {0x1aae84,{0xc5,0xfa,0x11,0x46,0x28}}, {0x9845a0,{0x8d,0x41,0x14,0xc3}},
        {0x2f08b0,{0xc5,0xfa,0x10,0x45,0x0c,0xc5,0xfa,0x11,0x40,0x18}},
        {0x1aac65,{0x80,0x4e,0x38,0x20}}, {0x1aacad,{0x80,0x66,0x38,0xdf}},
        {0x520e1a,{0xf6,0x46,0x24,0x20,0x75,0x3f}}, {0x520e43,{0xe8,0xd2,0x5a,0x5a,0x00}},
        {0x520f75,{0x8a,0x4e,0x24,0xc0,0xe9,0x05,0x80,0xe1,0x01,0x88,0x8f,0x18,0x13,0x00,0x00}},
        {0x88f23c,{0x80,0xbe,0x18,0x13,0x00,0x00,0x00,0x0f,0x84,0x9f,0x01,0x00,0x00}},
        {0x88f3e8,{0xd9,0x86,0x0c,0x13,0x00,0x00,0x5e,0xd8,0x4b,0x08}}
    };
    for(const auto& guard:options)if(!MatchesInstructions(reinterpret_cast<void*>(base+guard.rva),std::span(guard.bytes.begin(),guard.bytes.size())))return false;
    uint32_t clutchCompare{};
    if(!Read(base+0x892a4e,clutchCompare) || clutchCompare!=base+0xd76b6c)return false;
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
    // Proven clutch store, H-pattern mode/default/gear store and native
    // eligibility call. No executable instructions are patched.
    constexpr uint8_t clutchStore[]{0xc5,0xfa,0x11,0x46,0x30,0x83,0xf8,0x01};
    constexpr uint8_t gearMode[]{0x83,0xf8,0x02,0x0f,0x85,0x70,0x01,0x00,0x00};
    constexpr uint8_t gearStore[]{0x89,0x5e,0x24,0x8b,0x46,0x5c,0x8b,0x4e,0x10};
    constexpr uint8_t neutral[]{0x8d,0x58,0x07,0xe8,0x37,0xda,0x3a,0x00};
    constexpr uint8_t valueGate[]{0x8b,0x8e,0xb0,0x02,0x00,0x00,0x8a,0x86,0x48,0x02,0x00,0x00,0xc5,0xfa,0x11,0x45,0xfc,0xf7,0xc1,0xff,0x00,0x00,0x00,0x7f,0x2f,0x84,0xc0,0x74,0x2b};
    constexpr uint8_t heldGate[]{0x8b,0x96,0xb0,0x02,0x00,0x00,0x8a,0x8e,0x48,0x02,0x00,0x00,0x32,0xc0,0xf7,0xc2,0xff,0x00,0x00,0x00,0x7f,0x31,0x84,0xc9,0x74,0x2d};
    constexpr uint8_t actionMask[]{0x55,0x8b,0xec,0x83,0x7d,0x08,0x00,0x53,0x56,0x8b,0xf1,0xb3,0x01,0x74,0x26};
    if(!MatchesInstructions(reinterpret_cast<void*>(base+0x1aaf28),clutchStore) ||
       !MatchesInstructions(reinterpret_cast<void*>(base+0x1ab0dc),gearMode) ||
       !MatchesInstructions(reinterpret_cast<void*>(base+0x1ab252),gearStore) ||
       !MatchesInstructions(reinterpret_cast<void*>(base+0x1ab0f1),neutral) ||
       !MatchesInstructions(reinterpret_cast<void*>(base+0x1aab72),RelativeCall(0x1aab72,0x32a260)) ||
       !MatchesInstructions(reinterpret_cast<void*>(base+0x141eab),valueGate) ||
       !MatchesInstructions(reinterpret_cast<void*>(base+0x558b36),heldGate) ||
       !MatchesInstructions(reinterpret_cast<void*>(base+0x3bf8c0),actionMask) ||
       !MatchesInstructions(reinterpret_cast<void*>(base+0x141ed4),RelativeCall(0x141ed4,0x3bf8c0)) ||
       !MatchesInstructions(reinterpret_cast<void*>(base+0x558b5c),RelativeCall(0x558b5c,0x3bf8c0)))return false;
    for(size_t i=0;i<std::size(driverMethods);++i) {uintptr_t pointer{};if(!Read(base+driverTable+4*i,pointer) || pointer!=base+driverMethods[i])return false;}
    for(size_t i=0;i<std::size(methods);++i) {uintptr_t pointer{};if(!Read(base+inputTable+4*i,pointer) || pointer!=base+methods[i])return false;}
    return true;
}
// Input2 bind 5c1db0(map, const Device*): prologue, preset lookup, library
// find, non-keyboard default merge, calibration propagation and two-argument
// return. ScreenPressStart passes the device record and the profile map, with
// its device map from the PC stub 443250 (always 0). No bytes are patched.
bool ClaimGuards() {
    constexpr uint8_t prologue[]{0x55,0x8b,0xec,0x83,0xec,0x2c,0x53,0x56,0x57,0x8b,0xf9,0x8b,0x4d,0x0c,0x8d,0x45,0xfc,0x50,0x51,0x8b,0xcf,0xbb,0x58,0x00,0x00,0x00};
    constexpr uint8_t epilogue[]{0x5f,0x5e,0x8b,0xc3,0x5b,0x8b,0xe5,0x5d,0xc2,0x08,0x00};
    constexpr uint8_t pressStart[]{0x51,0x52,0x8b,0xcf,0x89,0x43,0x10};
    constexpr uint8_t mapStub[]{0x33,0xc0,0xc2,0x04,0x00};
    const auto at=[](uintptr_t rva,std::span<const uint8_t> bytes){return MatchesInstructions(reinterpret_cast<void*>(base+rva),bytes);};
    return at(0x1c1db0,prologue) && at(0x1c1ed3,epilogue) && at(0x1c1dd1,RelativeCall(0x1c1dd1,0x499220)) &&
        at(0x1c1dea,RelativeCall(0x1c1dea,0x55e820)) && at(0x1c1e42,RelativeCall(0x1c1e42,0x2ead40)) &&
        at(0x1c1ece,RelativeCall(0x1c1ece,0x1a1cf0)) && at(0x3dcebd,pressStart) && at(0x3dcec4,RelativeCall(0x3dcec4,0x1c1db0)) &&
        at(0x43250,mapStub) && at(0x3dce77,RelativeCall(0x3dce77,0x43250));
}
void LoadClaim(const std::filesystem::path& output) {
    claim.requested=true;
    std::ifstream file(output/L"device-claim.request",std::ios::binary);
    std::error_code error;const auto size=std::filesystem::file_size(output/L"device-claim.request",error);
    const std::string text=file && !error && size<=1024?std::string((std::istreambuf_iterator<char>(file)),{}):std::string();
    if(!ParseClaimRequest(text,claim.request)){claim.reason="invalid_request";WriteClaim();return;}
    if(!ClaimGuards()){claim.reason="claim_instruction_guard";WriteClaim();return;}
    claim.pending=true;claim.reason="waiting_for_input_instance";WriteClaim();
}
InputRoute DriverRoute() {
    // Raw kernel reads only: also used while peers are paused for attachment.
    return ReadInputRoute(base,[](uintptr_t at,void* output,size_t bytes){SIZE_T copied{};
        return at>=0x10000 && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),output,bytes,&copied) && copied==bytes;});
}
bool DriverInstance(uintptr_t object) {
    const auto current=DriverRoute();uintptr_t table{};
    return current.valid && object==driverCandidate.controller && current.controller==object && current.input==owner &&
        current.input==driverCandidate.input && current.map==driverCandidate.map && Heap(object,0x6c) &&
        Read(object,table) && table==driverExpected;
}
void DriverAttached(uintptr_t object) noexcept {
    RegisterLocalControllerShadow(base,object,reinterpret_cast<uintptr_t>(driverShadow));
}
void AttachDriver() {
    if(!driverShadow)return;
    if(curveOverride.object) {
        uintptr_t oldTable{};
        // The verified native destructor replaces the private vptr before free.
        // A different rooted controller alone is not proof that the old one died.
        if(!Heap(curveOverride.object,0x6c) || (Read(curveOverride.object,oldTable) && oldTable!=reinterpret_cast<uintptr_t>(driverShadow)))curveOverride={};
    }
    driverCandidate=DriverRoute();
    if(!driverCandidate.valid || driverCandidate.input!=owner) {driverHook=false;driverReason="waiting_for_local_controller";return;}
    uintptr_t table{};uint8_t active{};
    if(!Read(driverCandidate.controller,table) || !Read(driverCandidate.controller+0x40,active) || active!=1) {driverHook=false;driverReason="waiting_for_active_controller";return;}
    if(driverCandidate.controller==driverObject && table==reinterpret_cast<uintptr_t>(driverShadow)) {driverHook=true;driverReason="private_instance_attached";return;}
    if(table!=base+driverTable){driverReason="foreign_controller_table";return;}
    driverExpected=base+driverTable;
    const auto failure=AttachInputInstance(reinterpret_cast<void*>(driverCandidate.controller),reinterpret_cast<void*>(driverExpected),
        driverShadow,base,DriverInstance,driverBusy,DriverAttached);
    if(failure) {driverHook=false;driverReason=failure;return;}
    driverObject=driverCandidate.controller;driverHook=true;++driverAttachments;driverReason="private_instance_attached";Status();
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
// Retain this private table for process lifetime. Native destruction restores
// the original/base tables itself; no native allocator ever owns this table.
const char* PrepareInputShadow(void**& allocation) {
    originalServiceUpdate=Function<ServiceUpdateFn>(0x3b5dd0);
    allocation=static_cast<void**>(VirtualAlloc(nullptr,14*sizeof(void*),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!allocation || !ReadMemory(reinterpret_cast<void*>(base+inputTable-4),allocation,14*sizeof(void*))) {
        if(allocation)VirtualFree(allocation,0,MEM_RELEASE);allocation=nullptr;return "input_shadow_allocation";}
    allocation[2]=reinterpret_cast<void*>(ServiceUpdate);DWORD old{};
    if(!VirtualProtect(allocation,14*sizeof(void*),PAGE_READONLY,&old)){VirtualFree(allocation,0,MEM_RELEASE);allocation=nullptr;return "input_shadow_protection";}
    return nullptr;
}
// A refused bindings install cannot attach Input2, so a pending claim ends too.
void RefuseInstall(const char* why) { Refuse(why);Status();if(claim.pending)FinishClaim("driving_controls_refused"); }
void Install(uintptr_t gameBase,const std::filesystem::path& output) {
    static bool attempted{};if(attempted)return;attempted=true;
    const bool claimRequested=std::filesystem::is_regular_file(output/L"device-claim.request");
    std::ifstream request(output/L"driving-controls.request",std::ios::binary);if(!request && !claimRequested)return;
    base=gameBase;folder=output;
    if(claimRequested)LoadClaim(output);
    if(!request) {
        // Claim only: no bindings, transmission, wheel or menu overlay; the
        // private Input2 Update forwards natively and performs the claim.
        claim.only=true;if(!claim.pending)return;
        if(!Guards()){FinishClaim("input_instruction_guard");return;}
        void** allocation{};
        if(const auto failure=PrepareInputShadow(allocation)){FinishClaim(failure);return;}
        shadowTable=allocation+1;requested=true;reason="waiting_for_input_instance";Status();return;
    }
    if(std::filesystem::file_size(output/L"driving-controls.request")>524288){RefuseInstall("request_size");return;}
    const std::string text((std::istreambuf_iterator<char>(request)),{});
    if(!std::filesystem::exists(output/L"driving-controls.xml") || !Parse(text,bindings,&settings)){RefuseInstall("invalid_request");return;}
    wheelConfigured=settings.wheelSettings && std::any_of(bindings.begin(),bindings.end(),[](const auto& binding){
        return Guid(binding.device) && DirectAxis(binding.input) && (binding.action=="Steer Left" || binding.action=="Steer Right");});
    coverage.resize(bindings.size());activity.resize(bindings.size());
    if(!Guards()){RefuseInstall("input_instruction_guard");return;}
    shift::Configure(base,settings.clutchShiftThreshold,settings.clutchShiftSettings && settings.transmission==Transmission::HPatternClutch);
    try {physicalInput.Configure(bindings);}catch(...) {RefuseInstall("input_reader_configuration");return;}
    originalDriverUpdate=Function<ServiceUpdateFn>(0x1aab50);
    void** allocation{};DWORD old{};
    if(const auto failure=PrepareInputShadow(allocation)){RefuseInstall(failure);return;}
    if(settings.transmission!=Transmission::Game || wheelConfigured || std::any_of(bindings.begin(),bindings.end(),[](const auto& binding){return binding.action=="Clutch" ||
        HPatternIndex(binding.action)>=0;})) {
        // The game hashes image bytes during startup and later. Never patch
        // its shared table, even before the first driving update. Prepare an
        // immutable private copy; attach only to a proven live local object.
        // The preceding native word is preserved, not assumed to be RTTI.
        auto** driverAllocation=static_cast<void**>(VirtualAlloc(nullptr,14*sizeof(void*),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        if(!driverAllocation || !ReadMemory(reinterpret_cast<void*>(base+driverTable-4),driverAllocation,14*sizeof(void*))) {
            if(driverAllocation)VirtualFree(driverAllocation,0,MEM_RELEASE);VirtualFree(allocation,0,MEM_RELEASE);RefuseInstall("driver_shadow_allocation");return;
        }
        driverAllocation[4]=reinterpret_cast<void*>(DriverUpdate);
        if(!VirtualProtect(driverAllocation,14*sizeof(void*),PAGE_READONLY,&old)) {
            VirtualFree(driverAllocation,0,MEM_RELEASE);VirtualFree(allocation,0,MEM_RELEASE);RefuseInstall("driver_shadow_protection");return;
        }
        driverShadow=driverAllocation+1;driverReason="waiting_for_local_controller";
    }
    shadowTable=allocation+1;requested=true;reason="waiting_for_input_instance";Status();
}
void Tick() {
    if(!requested || stopped || failed)return;
    std::lock_guard guard(mutex);
    shift::Tick();
    if(owner && Instance(owner,shadowTable)){AttachDriver();return;}
    if(owner) {owner=0;installed=false;previous.clear();maps={};devices={};coverage.assign(bindings.size(),{});reason="waiting_for_input_replacement";Status();}
    const auto object=Discover();if(!object)return;
    const auto failure=AttachInputInstance(reinterpret_cast<void*>(object),reinterpret_cast<void*>(base+inputTable),shadowTable,base,OriginalInstance);
    if(failure){reason=failure;Status();return;}
    owner=object;installed=true;++attachments;reason="waiting_for_input_update";AttachDriver();Status();
}
void Stop() {
    stopped=true;shift::Stop();std::lock_guard guard(mutex);
    if(driverObject) {
        driverCandidate=DriverRoute();driverExpected=reinterpret_cast<uintptr_t>(driverShadow);
        if(driverCandidate.valid && driverCandidate.controller==driverObject && DriverInstance(driverObject)) {
            const auto failure=AttachInputInstance(reinterpret_cast<void*>(driverObject),driverShadow,reinterpret_cast<void*>(base+driverTable),base,DriverInstance,driverBusy,RestoreCurve);
            driverReason=failure?failure:"private_instance_restored";
            if(!failure){driverHook=false;RegisterLocalControllerShadow(0,0,0);}
        } else {driverHook=false;driverReason="controller_no_longer_current";}
    }
    // Tables stay allocated for process lifetime. A stale object can safely
    // forward to its native destructor even when shutdown cannot prove ownership.
    Status();WriteClaim();physicalInput.Release();
}
#if defined(GRID2VR_DRIVING_CONTROLS_TEST)
namespace testing {
void DriverUpdate(void* object,void* nativeUpdate) {
    const auto saved=originalDriverUpdate;
    originalDriverUpdate=reinterpret_cast<ServiceUpdateFn>(nativeUpdate);
    const double delta=1.0/60.0;
    grid2vr::drivingcontrols::DriverUpdate(object,nullptr,&delta);
    originalDriverUpdate=saved;
}
}
#endif
}
