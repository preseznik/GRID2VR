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
    memory[controller]=0;Check(!ReadInputRoute(image,read).valid,"foreign local controller is refused");memory[controller]=image+0xd0ba08;
    memory[node]=node;Check(!ReadInputRoute(image,read).valid,"cyclic controller tree is refused");memory[node]=head;
    memory[car+0xb0]=1;Check(!ReadInputRoute(image,read).valid,"another player's controller is not selected");memory[car+0xb0]=0;
    unsigned mapReads{};
    Check(!ReadInputRoute(image,[&](uintptr_t at,void* out,size_t bytes){
        if(at==controller+0x5c && ++mapReads==2)memory[at]=1;return read(at,out,bytes);
    }).valid,"map changes during route lookup are refused");
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
    std::cout << "Driving controls protocol, calibration and session-only aggregation passed.\n";
}
