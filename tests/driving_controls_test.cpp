#include "driving_controls_model.h"
#include "driving_controls_route.h"
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#if defined(_WIN32) && !defined(_WIN64)
#include "driving_controls_patch.h"
#include "driving_controls_instance.h"
#endif
using namespace grid2vr::drivingcontrols;
#if defined(_WIN32) && !defined(_WIN64)
bool ValidInputFixture(uintptr_t object) {
    const auto* words=reinterpret_cast<const uint32_t*>(object);return words[1]==0x47524944;
}
void PublishInputFixture(uintptr_t object) noexcept {
    grid2vr::RegisterLocalControllerShadow(0x400000,object,*reinterpret_cast<const uint32_t*>(object));
}
#endif
void Check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
int main() {
    std::map<uintptr_t,uint32_t> memory;
    constexpr uintptr_t image=0x400000,parent=0x100000,head=0x110000,node=0x120000,car=0x130000,
        group=0x140000,children=0x150000,controller=0x160000,input=0x170000;
    memory[image+0xd96808]=parent;memory[parent]=image+0xd1266c;memory[parent+0x1a4]=head;memory[head+4]=node;
    memory[node]=head;memory[node+4]=head;memory[node+8]=head;memory[node+12]=car;memory[node+16]=group;
    memory[car+0xb0]=0;memory[group+8]=children;memory[group+12]=children+8;memory[children]=controller;
    memory[controller]=image+0xd0ba08;memory[controller+0x10]=input;memory[controller+0x5c]=0;
    const auto read=[&](uintptr_t at,void* output,size_t bytes) {
        if(bytes%4)return false;
        for(size_t i=0;i<bytes;i+=4) {const auto found=memory.find(at+i);if(found==memory.end())return false;
            std::memcpy(static_cast<uint8_t*>(output)+i,&found->second,4);}
        return true;
    };
    const auto route=ReadInputRoute(image,read);
    Check(route.valid && route.input==input && route.map==0 && route.controller==controller,"actual local controller map is resolved");
    Check(route.car==car && !route.physics && !SamePhysicsRoute(route,route),"legacy input route cannot imply a physics owner");
    memory[car+0x30]=0x190000;
    const auto physicsRoute=ReadInputRoute(image,read);
    Check(physicsRoute.valid && physicsRoute.physics==0x190000 && SamePhysicsRoute(physicsRoute,physicsRoute),"local physics is rooted through the exact native car");
    auto changedPhysics=physicsRoute;changedPhysics.physics+=4;
    Check(!SamePhysicsRoute(physicsRoute,changedPhysics),"replacement physics cannot reuse a shift permit");
    changedPhysics=physicsRoute;changedPhysics.car+=4;
    Check(!SamePhysicsRoute(physicsRoute,changedPhysics),"replacement car cannot reuse a shift permit");
    unsigned physicsReads{};
    const auto unstablePhysics=ReadInputRoute(image,[&](uintptr_t at,void* out,size_t bytes){
        if(at==car+0x30 && ++physicsReads==2)memory[at]+=4;return read(at,out,bytes);
    });
    Check(unstablePhysics.valid && !unstablePhysics.physics,"changing physics link preserves input but refuses shift ownership");
    memory.erase(car+0x30);
    Check(SameInputRoute(route,route),"current samples match their exact consumed route");
    auto changedRoute=route;changedRoute.map=1;Check(!SameInputRoute(route,changedRoute),"map switch cannot reuse sampled steering");
    changedRoute=route;changedRoute.controller+=4;Check(!SameInputRoute(route,changedRoute),"replacement controller cannot reuse sampled steering");
    changedRoute=route;changedRoute.input+=4;Check(!SameInputRoute(route,changedRoute),"replacement input service cannot reuse sampled steering");
    changedRoute=route;changedRoute.valid=false;Check(!SameInputRoute(route,changedRoute) && !SameInputRoute(changedRoute,route),"unproven route cannot reuse sampled steering");
    constexpr uintptr_t ownedTable=0x180000;
    memory[controller]=ownedTable;
    Check(!ReadInputRoute(image,read).valid,"unregistered private controller table is foreign");
    grid2vr::RegisterLocalControllerShadow(image,controller,ownedTable);
    Check(ReadInputRoute(image,read).valid,"exact owned driver table preserves the local input route");
    Check(!grid2vr::IsLocalControllerTable(image,controller+4,ownedTable) && !grid2vr::IsLocalControllerTable(image+4,controller,ownedTable),
        "registered table cannot admit another object or game image");
    grid2vr::RegisterLocalControllerShadow(0,0,0);
    Check(!ReadInputRoute(image,read).valid,"revoked ownership fails closed");memory[controller]=image+0xd0ba08;
    memory[controller]=0;Check(!ReadInputRoute(image,read).valid,"foreign local controller is refused");memory[controller]=image+0xd0ba08;
    memory[node]=node;Check(!ReadInputRoute(image,read).valid,"cyclic controller tree is refused");memory[node]=head;
    memory[car+0xb0]=1;Check(!ReadInputRoute(image,read).valid,"another player's controller is not selected");memory[car+0xb0]=0;
    unsigned mapReads{};
    Check(!ReadInputRoute(image,[&](uintptr_t at,void* out,size_t bytes){
        if(at==controller+0x5c && ++mapReads==2)memory[at]=1;return read(at,out,bytes);
    }).valid,"map changes during route lookup are refused");
    // A full Normal-launch grid: fifteen AI cars before the local car in the tree.
    {
        std::map<uintptr_t,uint32_t> grid=memory;
        const auto gridRead=[&](uintptr_t at,void* output,size_t bytes) {
            for(size_t i=0;i<bytes;i+=4) {const auto found=grid.find(at+i);if(found==grid.end())return false;
                std::memcpy(static_cast<uint8_t*>(output)+i,&found->second,4);}
            return true;
        };
        constexpr uintptr_t chain=0x200000,cars=0x300000;
        const auto build=[&](unsigned count) {
            grid[head+4]=chain;
            for(unsigned i=0;i<count;++i) {
                const auto at=chain+i*0x40;const bool local=i+1==count;
                grid[at]=head;grid[at+4]=i?at-0x40:head;grid[at+8]=local?head:at+0x40;
                grid[at+12]=local?car:cars+i*0x100;grid[at+16]=group;
                if(!local)grid[cars+i*0x100+0xb0]=1;
            }
        };
        build(16);RouteProbe probe;
        const auto full=ReadInputRoute(image,gridRead,&probe);
        Check(full.valid && full.controller==controller && !std::strcmp(probe.step,"none") && probe.nodes==16 && probe.roles==16,"local car after fifteen AI cars is found");
        build(40);
        Check(!ReadInputRoute(image,gridRead,&probe).valid && !std::strcmp(probe.step,"node_limit") && probe.nodes==32,"an oversized tree fails closed at the bound");
        grid[parent]=image+0xd12670;
        Check(!ReadInputRoute(image,gridRead,&probe).valid && !std::strcmp(probe.step,"parent_table") && probe.parentTable==0xd12670,"probe names a foreign parent table");
    }
    {
        RouteProbe probe;memory[car+0xb0]=1;
        Check(!ReadInputRoute(image,read,&probe).valid && !std::strcmp(probe.step,"no_local_car") && probe.roles==1 && probe.roleValues[0]==1,"probe reports only AI roles");
        memory[car+0xb0]=0;memory[controller]=0;
        Check(!ReadInputRoute(image,read,&probe).valid && !std::strcmp(probe.step,"controller_identity") && probe.controllerBytes==8,"probe reports a foreign controller");
        memory[controller]=image+0xd0ba08;
    }
    Check(RelativeCall(0x400cd5,0x791ba0)==std::array<uint8_t,5>{0xe8,0xc6,0x0e,0x39,0x00},"native update call displacement");
    Check(RelativeCall(0x183bd8,0x28e7e0)==std::array<uint8_t,5>{0xe8,0x03,0xac,0x10,0x00},"native key-repeat pressed call");
    Check(RelativeCall(0xfffffffc,0x100)==std::array<uint8_t,5>{0xe8,0xff,0x00,0x00,0x00},"Win32 call wrapping");
    Check(InCallInstruction(0x100,0x100) && InCallInstruction(0x104,0x100) && !InCallInstruction(0xff,0x100) && !InCallInstruction(0x105,0x100),"suspended peer instruction boundaries");
#if defined(_WIN32) && !defined(_WIN64)
    auto* code=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    Check(code!=nullptr,"allocate owned call-site fixture");
    const std::array<uint8_t,6> nativeFunction{0xb8,1,0,0,0,0xc3},replacementFunction{0xb8,7,0,0,0,0xc3};
    std::memcpy(code+64,nativeFunction.data(),6);std::memcpy(code+80,replacementFunction.data(),6);
    for(const auto offset:{0,16}) {
        const auto call=RelativeCall(reinterpret_cast<uint32_t>(code+offset),reinterpret_cast<uint32_t>(code+64));
        std::memcpy(code+offset,call.data(),5);code[offset+5]=0xc3;
    }
    DWORD old{};Check(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old)!=FALSE,"protect owned fixture");
    FlushInstructionCache(GetCurrentProcess(),code,4096);
    const auto first=reinterpret_cast<unsigned (__cdecl*)()>(code),second=reinterpret_cast<unsigned (__cdecl*)()>(code+16);
    Check(first()==1 && second()==1,"unmodified fixture calls native functions");
    const std::array<CallPatch,2> mismatch{{{code,code+64,code+80},{code+16,code+65,code+80}}};
    Check(PatchCalls(mismatch)!=nullptr && first()==1 && second()==1,"whole transaction refuses a foreign call before any mutation");
    const std::array<CallPatch,2> calls{{{code,code+64,code+80},{code+16,code+64,code+80}}};
    Check(PatchCalls(calls)==nullptr && first()==7 && second()==7,"call-site adapters execute with native CALL stack behavior");
    MEMORY_BASIC_INFORMATION info{};VirtualQuery(code,&info,sizeof(info));
    Check(info.Protect==PAGE_EXECUTE_READ && std::memcmp(code+64,nativeFunction.data(),6)==0,"page protection and native entrypoint retained");
    const std::array<CallPatch,2> reverse{{{code,code+80,code+64},{code+16,code+80,code+64}}};
    Check(PatchCalls(reverse)==nullptr && first()==1 && second()==1,"bounded reverse transaction restores original calls");
    VirtualFree(code,0,MEM_RELEASE);
#endif
#if defined(_WIN32) && !defined(_WIN64)
    auto* instance=static_cast<uint32_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    Check(instance!=nullptr,"allocate private Input2 attachment fixture");
    const InstanceBusyRange driverRanges[]{{0xce6a0,0xce74b},{0x6012a0,0x601347},{0x333c40,0x333c62},
        {0x1aab50,0x1ab2ad},{0x5a1d90,0x5a1db1},{0x63740,0x63807}};
    for(const auto& range:driverRanges)Check(InstanceInstructionBusy(range.begin,driverRanges) && InstanceInstructionBusy(range.end-1,driverRanges) &&
        !InstanceInstructionBusy(range.end,driverRanges),"driver construction/destruction/update reject attachment at exact instruction boundaries");
    std::array<uint32_t,14> originalTable{},shadowTable{};
    for(size_t i=0;i<originalTable.size();++i)originalTable[i]=static_cast<uint32_t>(0x1000+i*4);
    shadowTable=originalTable;shadowTable[2]=0x7000;
    const auto tableBefore=originalTable;
    instance[0]=reinterpret_cast<uint32_t>(originalTable.data()+1);instance[1]=0x47524944;
    Check(AttachInputInstance(instance,originalTable.data()+1,shadowTable.data()+1,0,ValidInputFixture)==nullptr,
        "private instance vptr transaction succeeds");
    Check(instance[0]==reinterpret_cast<uint32_t>(shadowTable.data()+1) && originalTable==tableBefore && instance[1]==0x47524944,
        "native table and instance data retained");
    Check(AttachInputInstance(instance,originalTable.data()+1,shadowTable.data()+1,0,ValidInputFixture)!=nullptr,
        "foreign vptr refuses transaction");
    Check(AttachInputInstance(instance,shadowTable.data()+1,originalTable.data()+1,0,ValidInputFixture)==nullptr,
        "owned heap pointer restoration succeeds");
    instance[1]=0;
    Check(AttachInputInstance(instance,originalTable.data()+1,shadowTable.data()+1,0,ValidInputFixture)!=nullptr &&
        instance[0]==reinterpret_cast<uint32_t>(originalTable.data()+1),"changed instance fails final lifetime validation without write");
    grid2vr::RegisterLocalControllerShadow(image,controller,ownedTable);
    Check(AttachInputInstance(instance,originalTable.data()+1,shadowTable.data()+1,0,ValidInputFixture,{},PublishInputFixture)!=nullptr &&
        grid2vr::IsLocalControllerTable(image,controller,ownedTable),"failed B attachment preserves A ownership when the route returns to A");
    instance[1]=0x47524944;
    Check(AttachInputInstance(instance,shadowTable.data()+1,originalTable.data()+1,0,ValidInputFixture,{},PublishInputFixture)!=nullptr &&
        grid2vr::IsLocalControllerTable(image,controller,ownedTable),"failed B compare-exchange preserves A ownership");
    Check(AttachInputInstance(instance,originalTable.data()+1,shadowTable.data()+1,0,ValidInputFixture,{},PublishInputFixture)==nullptr &&
        grid2vr::IsLocalControllerTable(image,reinterpret_cast<uintptr_t>(instance),instance[0]) &&
        !grid2vr::IsLocalControllerTable(image,controller,ownedTable),"successful B attachment publishes its post-swap identity");
    grid2vr::RegisterLocalControllerShadow(0,0,0);
    VirtualFree(instance,0,MEM_RELEASE);
#endif
    Check(CallsiteDiagnosticRequest("GRID2VR_DRIVING_CONTROLS_CALLSITE_DIAGNOSTIC_1\n"),"explicit call-site diagnostic token");
    Check(!CallsiteDiagnosticRequest("") &&
          !CallsiteDiagnosticRequest("GRID2VR_DRIVING_CONTROLS_1\n") &&
          !CallsiteDiagnosticRequest("GRID2VR_DRIVING_CONTROLS_PASSTHROUGH_1\n") &&
          !CallsiteDiagnosticRequest("GRID2VR_DRIVING_CONTROLS_CALLSITE_DIAGNOSTIC_1") &&
          !CallsiteDiagnosticRequest("GRID2VR_DRIVING_CONTROLS_CALLSITE_DIAGNOSTIC_1\r\n") &&
          !CallsiteDiagnosticRequest("GRID2VR_DRIVING_CONTROLS_CALLSITE_DIAGNOSTIC_1\nextra") &&
          !CallsiteDiagnosticRequest(std::string(129,'x')),"normal, pass-through and malformed requests cannot enable native code changes");
    Check(PassthroughRequest("GRID2VR_DRIVING_CONTROLS_PASSTHROUGH_1\n"),"explicit diagnostic pass-through token");
    Check(!PassthroughRequest("") && !PassthroughRequest("GRID2VR_DRIVING_CONTROLS_PASSTHROUGH_1") &&
          !PassthroughRequest("GRID2VR_DRIVING_CONTROLS_PASSTHROUGH_1\r\n") &&
          !PassthroughRequest("GRID2VR_DRIVING_CONTROLS_PASSTHROUGH_1\nextra") &&
          !PassthroughRequest(std::string(129,'x')),"empty, malformed and oversized pass-through requests are rejected");
    const std::string header="GRID2VR_DRIVING_CONTROLS_1\n";
    const std::string row="keyboard\tAccelerate\tvk_code_0x57\tuniDirPos\t0\t1\n";
    std::vector<Binding> parsed;
    Check(Parse(header+row+"xinput_pad\tAccelerate\txinput_right_trigger\tuniDirPos\t0.1\t0.9\n",parsed) && parsed.size()==2,"keyboard and controller parse");
    Check(!Parse(header+row+row,parsed),"duplicate action family");
    Check(!Parse(header+"xinput_pad\tBrake\txinput_left_trigger\tuniDirNeg\t0\t1\n",parsed),"trigger direction");
    Check(!Parse(header+"keyboard\tBrake\tvk_code_0x01\tuniDirPos\t0\t1\n",parsed),"invalid key");
    Check(!Parse(header+"keyboard\tBrake\txinput_button_a\tuniDirPos\t0\t1\n",parsed),"wrong backend axis");
    Check(!Parse(header+"keyboard\tBrake\tvk_code_0x53\tuniDirPos\tNaN\t1\n",parsed),"nonfinite deadzone");
    Check(!Parse(header+"keyboard\tBrake\tvk_code_0x53\tuniDirPos\t0.8\t0.8\n",parsed),"empty calibration interval");
    Check(!Parse(header+"keyboard\tSteer Left\tvk_code_0x41\tuniDirPos\t0\t1\nkeyboard\tSteer Right\tvk_code_0x41\tuniDirPos\t0\t1\n",parsed),"identical steering");
    Check(Parse(header+"{00000001-0002-0003-0004-000000000005}\tSteer Left\tdi_x_axis\tbiDirLower\t0.2\t1\n{00000001-0002-0003-0004-000000000005}\tSteer Right\tdi_x_axis\tbiDirUpper\t0.2\t1\n",parsed),"native wheel family");
    Check(parsed[0].calibration==3 && parsed[1].calibration==2,"native calibration enum order");
    Check(!Parse(header,parsed) && !Parse(header+row.substr(0,row.size()-1),parsed),"empty or truncated protocol");
    Check(!Parse(std::string(131073,'x'),parsed),"bounded input");
    const std::string v2="GRID2VR_DRIVING_CONTROLS_2\n";
    const std::string product="{C262046D-0000-0000-0000-504944564944}";
    const std::string firstId="{00000001-0002-0003-0004-000000000005}",secondId="{00000002-0002-0003-0004-000000000005}";
    const auto wheelRow=product+"\tAccelerate\tdi_y_axis\tuniDirNeg\t0.02\t1\t";
    Check(Parse(v2+wheelRow+firstId+"\n"+wheelRow+secondId+"\nxinput_pad\tAccelerate\txinput_right_trigger\tuniDirPos\t0\t1\txinput:0\n"+row.substr(0,row.size()-1)+"\tkeyboard\n",parsed) && parsed.size()==4,
        "two physical wheels, Xbox and keyboard survive native protocol together");
    Check(parsed[0].deviceId==firstId && parsed[1].deviceId==secondId,"instance GUIDs survive family grouping");
    Check(!Parse(v2+wheelRow+firstId+"\n"+wheelRow+firstId+"\n",parsed),"same physical action duplicated");
    Check(!Parse(v2+wheelRow+"xinput:0\n",parsed),"DirectInput cannot impersonate XInput slot");
    Check(!Parse(v2+"xinput_pad\tAccelerate\txinput_right_trigger\tuniDirPos\t0\t1\txinput:4\n",parsed),"XInput slot bound");
    Check(!Parse(v2+row,parsed),"v2 refuses ambiguous physical identity");
    DrivingSettings settings;settings.transmission=Transmission::HPatternClutch;
    Check(Parse(v2+wheelRow+firstId+"\n",parsed,&settings) && settings.transmission==Transmission::Game,
        "legacy profiles preserve the game's transmission setting");
    const std::string v3="GRID2VR_DRIVING_CONTROLS_3\n";
    for(size_t i=0;i<std::size(TransmissionNames);++i) {
        Check(Parse(v3+"settings\t"+std::string(TransmissionNames[i])+"\t0\n"+wheelRow+firstId+"\n",parsed,&settings) &&
            settings.transmission==static_cast<Transmission>(i) && settings.wheelSettings && settings.steeringLinearity==0 && parsed.size()==1 && parsed[0].deviceId==firstId,
            "every transmission option preserves physical binding identity");
    }
    for(const auto invalid:{"", "settings\tunknown\n", "settings\tHPattern\n", "settings\thpattern\textra\n", "settings\thpattern_clutch", "settings\t\n"}) {
        settings.transmission=Transmission::Sequential;const auto before=parsed.size();
        Check(!Parse(v3+invalid+wheelRow+firstId+"\n",parsed,&settings) && settings.transmission==Transmission::Sequential && parsed.size()==before,
            "malformed v3 options fail without changing caller settings or bindings");
    }
    Check(!Parse(v3+"settings\tgame\t0\nsettings\tgame\t0\n"+wheelRow+firstId+"\n",parsed,&settings),"duplicate option row refused");
    Check(Parse(v3+"settings\tsequential\t0\n",parsed,&settings) && parsed.empty() && settings.transmission==Transmission::Sequential,
        "transmission-only request keeps native bindings");
    Check(!Parse(v3+"settings\tgame\t0\n",parsed,&settings),"empty no-op request refused");
    for(int value=-10;value<=10;++value)Check(Parse(v3+"settings\tgame\t"+std::to_string(value)+"\n"+wheelRow+firstId+"\n",parsed,&settings) &&
        settings.steeringLinearity==value,"full steering response range parsed");
    for(const auto invalid:{"-11","11","-0","+1","01","1.0"," 1","nan","1\textra",""})
        Check(!Parse(v3+"settings\tgame\t"+invalid+"\n"+wheelRow+firstId+"\n",parsed,&settings),"noncanonical or unbounded response refused");
    const std::string v4="GRID2VR_DRIVING_CONTROLS_4\n";
    for(int threshold=10;threshold<=100;++threshold) {
        Check(Parse(v4+"settings\thpattern_clutch\t-3\t"+std::to_string(threshold)+"\n"+wheelRow+firstId+"\n",parsed,&settings) &&
            settings.clutchShiftSettings && settings.clutchShiftThreshold==threshold && settings.transmission==Transmission::HPatternClutch &&
            settings.steeringLinearity==-3 && settings.wheelSettings && parsed.size()==1 && parsed[0].deviceId==firstId &&
            parsed[0].dead==.02f && parsed[0].saturation==1,"independent shift threshold preserves pedal calibration and binding identity");
    }
    for(const auto invalid:{"9","101","-50","+50","050","50.0"," 50","50 ","nan","50\textra",""}) {
        const auto before=settings;
        Check(!Parse(v4+"settings\thpattern_clutch\t0\t"+invalid+"\n"+wheelRow+firstId+"\n",parsed,&settings) &&
            settings.clutchShiftThreshold==before.clutchShiftThreshold && settings.clutchShiftSettings==before.clutchShiftSettings && parsed.size()==1,
            "invalid shift threshold fails transactionally");
    }
    Check(!Parse(v4+"settings\thpattern_clutch\t0\n",parsed,&settings),"v4 requires explicit threshold");
    Check(Parse(v4+"settings\thpattern_clutch\t0\t50\n",parsed,&settings) && parsed.empty() && settings.clutchShiftThreshold==50,
        "threshold and transmission work with native bindings");
    for(const auto& legacy:{header+row,v2+wheelRow+firstId+"\n",v3+"settings\thpattern_clutch\t0\n"}) {
        Check(Parse(legacy,parsed,&settings) && settings.clutchShiftThreshold==100 && !settings.clutchShiftSettings,
            "legacy requests never silently relax the native clutch gate");
    }
    Check(!Parse("GRID2VR_DRIVING_CONTROLS_5\nsettings\thpattern_clutch\t0\t50\n",parsed,&settings),"unknown protocol version refused");
    Check(Parse(v2+product+"\tBrake\tdi_y_axis\tuniDirNeg\t0.0123456789012345678901234567\t1\t"+firstId+"\n",parsed),"launcher decimal precision survives native parsing");
    std::array<Axis,3> native{};
    native[0].device.family=10;native[0].state={1,1,1,0};
    native[1].device.family=20;native[1].state={.4f,1,0,0};
    native[2].device.family=30;native[2].state={.2f,0,0,1};
    const auto before=native;
    const std::array<Override,1> replace{{{100,10,11}}};
    std::array<Axis,1> shadow{};shadow[0].device.family=10;shadow[0].state={.1f,0,0,0};
    auto state=Aggregate(native,shadow,replace,100);
    Check(state.value==.4f && state.held && !state.pressed && state.released,"replace only selected family, preserve other devices");
    Check(std::memcmp(before.data(),native.data(),sizeof(native))==0,"no writes to saved native axes");
    state=Aggregate(native,{},replace,200);
    Check(state.value==1 && state.pressed,"untouched action retains native values");
    state=Aggregate({},shadow,replace,100);
    Check(state.value==.1f,"override can add an unbound action");
    const std::array<Override,3> all{{{100,10,11},{100,20,21},{100,30,31}}};
    state=Aggregate(native,{},all,100);
    Check(state.value==0 && !state.held && !state.pressed && !state.released,"disconnected override does not fall back to old binding");
    shadow[0].state={.75f,1,1,0};state=Aggregate({},shadow,all,100);
    Check(state.value==.75f && state.pressed,"native edge output preserved");
    shadow[0].state={0,0,0,1};state=Aggregate({},shadow,all,100);
    Check(state.value==0 && state.released,"native release output preserved");
    Check(Replaces(replace,100,11) && !Replaces(replace,100,12),"GUID case identity aliases bounded");
    constexpr auto relocated=LockPrologue(0xbb0000+0xb16238);
    Check(relocated[5]==0x38 && relocated[6]==0x62 && relocated[7]==0x6c && relocated[8]==1,"ASLR lock import operand");
    Check(relocated!=LockPrologue(0x400000+0xb16238),"lock guard must follow actual module base");
    std::array<Axis,4> live{};
    live[0].device={1,10,0};live[1].device={1,10,0};live[2].device={1,10,1};live[3].device={2,20,0};
    for(size_t i=0;i<live.size();++i) {live[i].registry=123;live[i].input=500+static_cast<uint32_t>(i);live[i].calibration[0]=7;live[i].state={1,1,1,0};}
    const auto original=live;
    std::array<Axis,2> custom{};custom[0].device=live[0].device;custom[0].state={.25f,1,0,0};
    custom[1].device=live[2].device;custom[1].state={.5f,1,1,0};
    Check(OverlayFamily(live,custom,{100,10,11})==2,"one carrier per exact native device index");
    Check(live[0].state.value==.25f && live[1].state.value==0 && live[2].state.value==.5f && live[3].state.value==1,
        "separate controllers keep distinct states and untouched keyboard family");
    for(size_t i=0;i<live.size();++i)Check(SameBinding(live[i],original[i]) &&
        std::memcmp(&live[i],&original[i],offsetof(Axis,state))==0,"serialized binding/calibration bytes remain exact");
    auto rebind=live[0];++rebind.input;
    Check(!RestoreOverlay(rebind,original[0],live[0]) && rebind.input==original[0].input+1,"in-game rebinding never receives a stale restore");
    auto changedState=live[0];changedState.state.value=.9f;
    Check(!RestoreOverlay(changedState,original[0],live[0]) && changedState.state.value==.9f,"native state changes are not overwritten");
    const auto applied=live;
    for(size_t i=0;i<live.size();++i)Check(RestoreOverlay(live[i],original[i],applied[i]),"restore before native polling");
    Check(std::memcmp(live.data(),original.data(),sizeof(live))==0,"native held/pressed/released history restored exactly");
    auto missing=original;
    Check(OverlayFamily(missing,custom,{100,99,99})==0 && std::memcmp(missing.data(),original.data(),sizeof(missing))==0,
        "no borrowing a different family's carrier for an unrepresented binding");
    Check(OverlayFamily({},custom,{100,10,11})==0,"empty action stays absent instead of generating phantom menu input");
    std::array<Axis,2> keyboard{};keyboard[0].device={2,20,0};keyboard[1].device={2,20,0};
    keyboard[0].input=50;keyboard[1].input=51;keyboard[0].calibration[0]=7;
    keyboard[0].state={.1f,1,1,0};keyboard[1].state={.2f,0,0,0};const auto keyboardBefore=keyboard;
    custom[0].state={.375f,1,0,1};custom[1].state={.25f,0,0,0};
    Check(OverlayMissingFamily(keyboard,custom,{100,10,11},20),"missing Xbox family can contribute to the proven consumed map");
    Check(keyboard[0].state.value==.375f && keyboard[0].state.held && keyboard[0].state.pressed && keyboard[0].state.released,
        "native keyboard press coexists with continuous Xbox value and release");
    Check(SameState(keyboard[1].state,keyboardBefore[1].state),"only one aggregate carrier receives Xbox edges");
    for(size_t i=0;i<keyboard.size();++i)Check(SameBinding(keyboard[i],keyboardBefore[i]) &&
        std::memcmp(&keyboard[i],&keyboardBefore[i],offsetof(Axis,state))==0,"aggregate fallback preserves every serialized byte");
    const auto keyboardApplied=keyboard;
    auto foreignCalibration=keyboard[0];++foreignCalibration.calibration[6];
    Check(!RestoreOverlay(foreignCalibration,keyboardBefore[0],keyboardApplied[0]),"fallback restore refuses changed native calibration");
    for(size_t i=0;i<keyboard.size();++i)Check(RestoreOverlay(keyboard[i],keyboardBefore[i],keyboardApplied[i]),"aggregate fallback restores native state history");
    Check(std::memcmp(keyboard.data(),keyboardBefore.data(),sizeof(keyboard))==0,"aggregate state restoration is exact");
    Check(!OverlayMissingFamily(keyboard,{}, {100,10,11},20) && !OverlayMissingFamily({},custom,{100,10,11},20),
        "disconnected or absent native action cannot become a phantom input");
    Check(!OverlayMissingFamily(live,custom,{100,10,11},20),"existing family is handled only by exact-device overlay");
    auto wheelMap=keyboard;wheelMap[0].device.family=30;
    Check(!OverlayMissingFamily(wheelMap,custom,{100,10,11},20) &&
        SameState(wheelMap[0].state,keyboard[0].state) && SameState(wheelMap[1].state,keyboard[1].state),
        "mixed or wheel-selected carrier map is outside the Xbox keyboard fallback");
    Check(!HasCarrier(live,Device{1,10,2}) && HasCarrier(live,Device{1,10,1}),"unrepresented additional controller is detectable");
    auto disconnected=original;OverlayFamily(disconnected,{}, {100,10,11});
    Check(disconnected[0].state.value==0 && disconnected[1].state.value==0 && disconnected[2].state.value==0 && disconnected[3].state.value==1,
        "disconnected configured family cannot revive old native bindings");
    auto mixedMap=keyboardBefore;mixedMap[1].device.family=40;mixedMap[1].state={.7f,1,0,0};
    const auto mixedBefore=mixedMap;
    std::array<Axis,2> concurrent{};concurrent[0].device={0,10,0};concurrent[0].state={.4f,0,0,1};
    concurrent[1].device={0,40,1};concurrent[1].state={.9f,1,1,0};
    const std::array<Override,2> combinedOverrides{{{100,10,11},{100,40,41}}};
    Check(OverlayAction(mixedMap,concurrent,combinedOverrides,100),"any existing action can carry simultaneous devices");
    Check(mixedMap[0].state.value==.9f && mixedMap[0].state.pressed && mixedMap[0].state.released && mixedMap[1].state.value==0,
        "wheel, Xbox and native keyboard max/OR merge exactly once");
    for(size_t i=0;i<mixedMap.size();++i)Check(SameBinding(mixedMap[i],mixedBefore[i]),"merged action never rewrites identity/calibration/FFB device");
    mixedMap=mixedBefore;OverlayAction(mixedMap,{},combinedOverrides,100);
    Check(mixedMap[0].state.value==.1f && mixedMap[0].state.pressed && mixedMap[1].state.value==0,"disconnect preserves native keyboard, clears configured family");
    Check(!OverlayAction({},concurrent,combinedOverrides,100),"absent action requires separately guarded producer path");
    Axis menuPedal{};menuPedal.input=73;menuPedal.device.family=10;menuPedal.state={1,1,1,0};const auto menuBefore=menuPedal;
    Check(SuppressPedalAlias(menuPedal,73,{100,10,11},false) && menuPedal.state.value==0 && SameBinding(menuPedal,menuBefore),"legacy pedal menu alias suppressed without binding edits");
    for(unsigned kind=0;kind<3;++kind) {menuPedal=menuBefore;
        Check(!SuppressPedalAlias(menuPedal,kind==0?74:73,{100,kind==1?20u:10u,11},kind==2) && SameState(menuPedal.state,menuBefore.state),
            "other axis, other device family and explicit menu assignment stay active");}
    std::cout << "Driving controls v1/v2 identity, calibration and session-only aggregation passed.\n";
}
