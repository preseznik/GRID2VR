#include "chase_camera.h"
#include "chase_camera_adapter.h"
#include "chase_camera_guard.h"
#include "chase_camera_input.h"
#include "chase_camera_input_guard.h"
#include "chase_camera_state.h"
#include "chase_camera_region_guard.h"
#include "chase_camera_playback_state.h"
#include "chase_camera_transaction.h"
#include "chase_camera_collision.h"
#include "chase_camera_replay.h"
#include "driving_controls_patch.h"
#include "chase_observer_guard.h"
#include "solo_race.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <vector>

namespace grid2vr::engine { bool testSecondEye{};bool ReplayingEye() {return testSecondEye;} }
using namespace grid2vr;
using namespace grid2vr::chasecamera;
namespace {
constexpr uintptr_t updateResult=0x12345678,alternateResult=0x87654321,stageResult=0x2468abcd;
constexpr uintptr_t lookResult=0x9753abcd;
constexpr uintptr_t copyBackResult=0x1234abcd;
constexpr Pose nativePose{0,0,0,1,0,2,-5,1};
uintptr_t camera{},provider{},entity{},nextEntity{},manager{};
void* expectedTiming{};
unsigned calls[8]{};
bool valid=true,changeEntity{},overlap{},nativeAdjustment{},repeatProvider{},readOnlyPose{},stopInStage{};
bool collisionTest{},collisionWrongResult{},collisionStop{},collisionSecondEye{},collisionNoStack{};
bool collisionDuplicate{};
using TargetCall=uintptr_t(__thiscall*)(void*,void*,uint32_t);
TargetCall collisionAbi{};
uintptr_t collisionCaller=collisionTargetReturnRva;
uint32_t collisionArgument=1;
unsigned targetCalls{};
std::array<float,16> collisionOutput{};
std::array<float,16> nativeTarget{1,0,0,0,0,1,0,0,0,0,1,0,0,1,1.8f,1};
Pose updateEntry{},getterEntry{},afterProvider{},copyEntry{};
LookPacket receivedLook{};
void* receivedLookAddress{};
uintptr_t providerCaller=0x1f78ed;
uint32_t expectedFlag=0xfedc1234;
void Word(uintptr_t at,uint32_t value) {std::memcpy(reinterpret_cast<void*>(at),&value,4);}
void Double(uintptr_t at,double value) {std::memcpy(reinterpret_cast<void*>(at),&value,8);}
Pose GetPose(uintptr_t at) {Pose value{};std::memcpy(value.data(),reinterpret_cast<void*>(at+0xb0),sizeof(value));return value;}
void SetPose(uintptr_t at,const Pose& value) {std::memcpy(reinterpret_cast<void*>(at+0xb0),value.data(),sizeof(value));}
uintptr_t __fastcall NativeUpdate(void* self,void*,void* timing) {
    ++calls[0];valid&=reinterpret_cast<uintptr_t>(self)==camera && timing==expectedTiming;
    updateEntry=GetPose(camera);SetPose(camera,nativePose);return updateResult;
}
uintptr_t __fastcall NativeAlternate(void* self,void*,void* timing,uint32_t flag) {
    ++calls[1];valid&=reinterpret_cast<uintptr_t>(self)==camera && timing==expectedTiming && flag==expectedFlag;
    updateEntry=GetPose(camera);SetPose(camera,nativePose);return alternateResult;
}
uintptr_t __fastcall NativeGetter(void* self,void*) {
    ++calls[3];valid&=reinterpret_cast<uintptr_t>(self)==camera;getterEntry=GetPose(camera);return camera+0xb0;
}
uintptr_t __fastcall NativeProvider(void* self,void*,void* output) {
    ++calls[4];valid&=reinterpret_cast<uintptr_t>(self)==provider;
    uint32_t target{};std::memcpy(&target,reinterpret_cast<void*>(provider+8),4);
    std::memcpy(output,reinterpret_cast<void*>(target+0x330),16);return reinterpret_cast<uintptr_t>(output);
}
uintptr_t __fastcall NativeCopy(void* self,void*,void* output) {
    ++calls[5];valid&=reinterpret_cast<uintptr_t>(self)==camera;copyEntry=GetPose(camera);
    std::memcpy(output,reinterpret_cast<void*>(camera+0xb0),chaseobserver::recordBytes);
    return reinterpret_cast<uintptr_t>(output);
}
uintptr_t __fastcall NativeLook(void* self,void*,void* packet) {
    ++calls[6];valid&=reinterpret_cast<uintptr_t>(self)==camera;receivedLookAddress=packet;
    std::memcpy(receivedLook.data(),packet,sizeof(receivedLook));return lookResult;
}
uintptr_t __fastcall NativeCopyBack(void* self,void*,void* input) {
    ++calls[7];valid&=reinterpret_cast<uintptr_t>(self)==camera;
    std::memcpy(reinterpret_cast<void*>(camera+0xb0),input,chaseobserver::recordBytes);
    return copyBackResult;
}
uintptr_t __fastcall NativeTarget(void* self,void*,void* output,uint32_t adjusted) {
    ++targetCalls;valid&=reinterpret_cast<uintptr_t>(self)==camera && adjusted==collisionArgument;
    // The real helper calls the provider hook recursively before returning.
    alignas(16) std::array<float,4> position{};
    TestProvider(reinterpret_cast<void*>(provider),position.data(),0x18f8d2);
    std::memcpy(output,nativeTarget.data(),sizeof(nativeTarget));
    if(collisionStop)Stop();
    return reinterpret_cast<uintptr_t>(output)+(collisionWrongResult?16:0);
}
uintptr_t __fastcall NativeStage(void* self,void*,void* timing) {
    ++calls[2];valid&=reinterpret_cast<uintptr_t>(self)==camera && timing==expectedTiming;
    if(overlap) {
        // Completes before the provider boundary, returning identical pose bytes.
        std::thread other([&]{valid&=TestCall(0,self,timing)==updateResult;});other.join();
    }
    alignas(16) std::array<uint32_t,8> output{};output.fill(0x76543210);
    DWORD previous{};
    if(readOnlyPose)valid&=VirtualProtect(reinterpret_cast<void*>(camera),0x1000,PAGE_READONLY,&previous)!=0;
    valid&=TestProvider(reinterpret_cast<void*>(provider),output.data(),providerCaller)==reinterpret_cast<uintptr_t>(output.data());
    if(repeatProvider)valid&=TestProvider(reinterpret_cast<void*>(provider),output.data(),providerCaller)==reinterpret_cast<uintptr_t>(output.data());
    if(readOnlyPose)valid&=VirtualProtect(reinterpret_cast<void*>(camera),0x1000,previous,&previous)!=0;
    for(unsigned i=4;i<output.size();++i)valid&=output[i]==0x76543210;
    afterProvider=GetPose(camera);
    if(changeEntity)Word(provider+8,static_cast<uint32_t>(nextEntity));
    if(collisionTest) {
        alignas(16) std::array<float,16> scratch{};
        alignas(16) static std::array<float,16> foreignScratch{};
        auto* outputTarget=collisionNoStack?foreignScratch.data():scratch.data();
        engine::testSecondEye=collisionSecondEye;
        for(unsigned pass=0;pass<(collisionDuplicate?2u:1u);++pass) {
            const auto returned=collisionAbi?collisionAbi(self,outputTarget,collisionArgument):
                TestCollisionTarget(self,outputTarget,collisionArgument,collisionCaller);
            valid&=returned==reinterpret_cast<uintptr_t>(outputTarget)+(collisionWrongResult?16:0);
        }
        engine::testSecondEye=false;
        std::memcpy(collisionOutput.data(),outputTarget,sizeof(collisionOutput));
    }
    if(nativeAdjustment && afterProvider!=nativePose) {
        auto result=afterProvider;result[6]+=.25f;SetPose(camera,result);
    }
    if(stopInStage)Stop();
    return stageResult;
}
struct Fixture {
    static constexpr size_t size=0x100e000;
    uint8_t* memory=static_cast<uint8_t*>(VirtualAlloc(nullptr,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    uintptr_t base=reinterpret_cast<uintptr_t>(memory);
    alignas(16) std::array<float,4> timing{1.f/60,0,0,0};
    Fixture() {
        if(!memory)throw std::runtime_error("VirtualAlloc failed");
        using namespace grid2vr::chaseobserver;
        const auto put=[&](uintptr_t rva,const auto& bytes){std::memcpy(memory+rva,std::data(bytes),std::size(bytes));};
        put(routines[0],chaseobserver::updateEntry);put(routines[0]+0x2d,updateCall);put(routines[1],alternateBody);put(routines[2],copyBody);
        put(routines[3],obstructionBody);put(routines[4],recordPointerBody);put(routines[5],mainSceneEntry);
        put(0x804780,viewConverterCall);put(0x118581,selectedViewA);put(0x21a5f3,selectedViewB);
        put(0x2e3930,auxiliaryFrameEntry);put(0x2e394b,auxiliaryFrameAllocation);put(0x2e3c16,auxiliaryViewCall);
        put(0x3be050,completeSceneEntry);put(0x3be087,mainSceneCall);put(0x7a7cb3,completeSceneCall);
        put(routines[6],ProviderPositionGuard(base));put(0x1f78a0,preQueryEntry);put(0x61eb91,preQueryStage);put(providerBasisRva,providerBasisBody);
        put(0x18f8e0,pivotSelect);put(0x18f9ab,pivotXY);put(0x18f9d8,pivotZAddPosition);put(0x1f84c6,obstructionPivotCall);
        put(0x1fa885,nativePoseCommit);put(0x3caf10,postQueryZeroStub);put(0x59a164,managerPriorPose);put(0x5a687f,nativeBaselineCopy);
        for(const auto& guard:collisionGuards)std::memcpy(memory+guard.rva,guard.bytes.data(),guard.size);
        for(const auto& guard:LookGuards(base))std::memcpy(memory+guard.rva,guard.bytes.data(),guard.size);
        for(const auto& guard:PauseGuards(base))std::memcpy(memory+guard.rva,guard.bytes.data(),guard.size);
        for(const auto& guard:RegionGuards())std::memcpy(memory+guard.rva,guard.bytes.data(),guard.size);
        for(const auto& guard:ReplayStateGuards(base))std::memcpy(memory+guard.rva,guard.bytes.data(),guard.size);
        for(const auto& guard:PlaybackClockGuards(base))std::memcpy(memory+guard.rva,guard.bytes.data(),guard.size);
        for(const auto& guard:ReplayOwnerInputGuards(base))std::memcpy(memory+guard.rva,guard.bytes.data(),guard.size);
        for(const auto& guard:ReplayDirectorThreeGuards(base))std::memcpy(memory+guard.rva,guard.bytes.data(),guard.size);
        for(const auto& guard:ReplayShuttleGuards(base))std::memcpy(memory+guard.rva,guard.bytes.data(),guard.size);
        for(const auto& guard:ReplayMenuScopeGuards(base))std::memcpy(memory+guard.rva,guard.bytes.data(),guard.size);
        const auto relocated=[&](uintptr_t rva,const auto& bytes,const auto& offsets) {
            put(rva,bytes);
            for(const auto offset:offsets) {
                uint32_t absolute{};std::memcpy(&absolute,memory+rva+offset,4);
                Word(base+rva+offset,absolute-0x400000u+static_cast<uint32_t>(base));
            }
        };
        using namespace transaction_detail;
        relocated(0x70ec10,selectedTransaction,selectedTransactionRelocations);
        relocated(0x5afbe0,recordPostprocess,recordPostprocessRelocations);
        relocated(0x410c60,recordCopyOut,recordCopyOutRelocations);
        relocated(0x1ea430,recordCopyBack,recordCopyBackRelocations);
        Word(base+0xd09998,static_cast<uint32_t>(base+0x4b82f0));
        Word(base+0xd09948+9*4,static_cast<uint32_t>(base+0x5afbe0));
        Word(base+transactionCopyBackSlotRva,static_cast<uint32_t>(base+transactionCopyBackRva));
        Word(base+lookSlotRva,static_cast<uint32_t>(base+lookRoutineRva));
        for(size_t i=0;i<slots.size();++i)Word(base+SlotRva(i),static_cast<uint32_t>(base+routines[i]));
        Word(base+providerBasisSlotRva,static_cast<uint32_t>(base+providerBasisRva));
        const auto ptr=[&](uintptr_t at,uintptr_t value){Word(base+at,static_cast<uint32_t>(base+value));};
        constexpr uintptr_t parent=0x10000,car=0x11000,head=0x12000,node=0x12100,group=0x13000,local=0x14000,ai=0x15000;
        constexpr uintptr_t raceManager=parent+0x190;
        Word(base+0xe5917c,2);ptr(0xd96808,parent);ptr(0xdaa424,parent);ptr(parent,0xd1266c);
        ptr(parent+0xa8,0x16000);ptr(parent+0xac,0x16004);ptr(0x16000,car);ptr(car,0xcdfeb4);
        ptr(raceManager,0xd0bc48);Word(base+raceManager+0x18,1);memory[raceManager+0x24]=1;
        Word(base+raceManager+0x2c,4);Word(base+raceManager+0x30,2);ptr(raceManager+0x14,head);
        ptr(head,node);ptr(head+4,node);ptr(head+8,node);ptr(node,head);ptr(node+4,head);ptr(node+8,head);
        ptr(node+12,car);ptr(node+16,group);ptr(group,0xd0bb30);ptr(group+8,0x16100);ptr(group+12,0x16108);
        ptr(0x16100,local);ptr(0x16104,ai);ptr(local,0xd0ba08);memory[local+0x40]=1;ptr(ai,0xd039ac);
        camera=base+0x30000;provider=base+car+0x12c;entity=base+0x40000;nextEntity=base+0x44000;manager=base+0x75000;
        ptr(0xdef810,0x70000);ptr(0x70010,0x71000);ptr(0x71000+0x14fc,0x73000);ptr(0x73000+0x33c,0x75000);ptr(0x75000+0x748,0x30000);
        ptr(0xd98014,0x78000);ptr(0x78000,0xccaf10);
        // Ordinary driving used UI mode0 in all four retained focus samples
        // from the failed active-controls run20261027-160156-372.
        ptr(0x71000+0x148c,0x7a000);ptr(0x7a000,0xd10640);Word(base+0x7a000+0x1d8,0);
        Word(base+0x7a000+0x7640,0); // No native playback override.
        Word(base+0x7a000+0x7604,0); // No buffered-playback application.
        Word(camera,static_cast<uint32_t>(base+chaseTableRva));Word(camera+0xc,1);Word(camera+0x60,static_cast<uint32_t>(provider));
        Word(provider,static_cast<uint32_t>(base+providerTableRva));Word(provider+8,static_cast<uint32_t>(entity));Word(provider+0xc,static_cast<uint32_t>(base+car));
        std::memcpy(reinterpret_cast<void*>(camera+0x19),"chase_close",12);Word(camera+0x98,0x7238294);
        const std::array<float,12> basis{1,0,0,0,0,1,0,0,0,0,1,0};const std::array<float,4> position{0,0,0,1};
        for(auto target:{entity,nextEntity}) {
            std::memcpy(reinterpret_cast<void*>(target+0x300),basis.data(),sizeof(basis));
            std::memcpy(reinterpret_cast<void*>(target+0x330),position.data(),sizeof(position));
        }
        SetPose(camera,nativePose);Word(camera+0xb0+0x20,0x3fa00000);Word(camera+0xb0+0x1a0,0x87654320);
        expectedTiming=timing.data();
        const auto reader=[](uintptr_t address,void* out,size_t bytes){return ReadMemory(reinterpret_cast<void*>(address),out,bytes);};
        if(!GuardsMatch(base) || !AdapterGuardsMatch(base) || !LookGuardsMatch(base) || !PauseGuardsMatch(base) ||
           !RegionGuardsMatch(base) || !ReplayStateGuardsMatch(base,reader) || NativeDrivingReplayRefusal(base,reader) ||
           !TransactionGuardsMatch(base,reader) || !NormalLocalRaceReady(base))throw std::runtime_error("Synthetic native fixture did not qualify");
    }
    ~Fixture(){VirtualFree(memory,0,MEM_RELEASE);}
    void Originals() {TestOriginals(reinterpret_cast<void*>(NativeUpdate),reinterpret_cast<void*>(NativeAlternate),reinterpret_cast<void*>(NativeStage),reinterpret_cast<void*>(NativeGetter),reinterpret_cast<void*>(NativeProvider),reinterpret_cast<void*>(NativeCopy),reinterpret_cast<void*>(NativeLook),reinterpret_cast<void*>(NativeCopyBack));TestTargetOriginal(reinterpret_cast<void*>(NativeTarget));}
    bool HooksRestored() const {
        const auto call=drivingcontrols::RelativeCall(static_cast<uint32_t>(base+collisionTargetCallRva),static_cast<uint32_t>(base+collisionTargetRoutineRva));
        if(std::memcmp(memory+collisionTargetCallRva,call.data(),call.size()))return false;
        for(const auto route:{0u,1u,3u,4u,6u,2u})
            if(*reinterpret_cast<uint32_t*>(base+chaseobserver::SlotRva(route))!=base+chaseobserver::routines[route])return false;
        return *reinterpret_cast<uint32_t*>(base+lookSlotRva)==base+lookRoutineRva &&
            *reinterpret_cast<uint32_t*>(base+transactionCopyBackSlotRva)==base+transactionCopyBackRva;
    }
    void Frame(uint64_t n,ChaseCameraInput input={},bool focused=true,float dt=1.f/60) {
        TestInput(n,input,focused,dt);
        valid&=TestCall(0,reinterpret_cast<void*>(camera),timing.data())==updateResult;
        valid&=TestCall(2,reinterpret_cast<void*>(camera),timing.data())==stageResult;
    }
    void Replay(uint32_t mode=1) {
        const auto ptr=[&](uintptr_t at,uintptr_t value){Word(base+at,static_cast<uint32_t>(base+value));};
        constexpr uintptr_t ui=0x7a000,track=0x86000,director=0x88000,player=0x8a000;
        ptr(0x71000+0x1504,track);ptr(0x71000+0x150c,director);
        ptr(ui+0x260,0xd10574);ptr(ui+0x75ec,player);ptr(ui+0x75f0,track);ptr(ui+0x7638,0x10000);
        ptr(track,0xd0a374);ptr(track+0xa8,0x10000);ptr(track+0xac,0x73000);
        ptr(director,0xce3714);ptr(director+0x10,ui);ptr(director+0x18,track);ptr(director+0x1c,0x73000);
        ptr(player+0xa8,0x10000);ptr(0x11000+0x148,0x75000);
        Word(base+ui+0x1d8,mode);Word(base+ui+0x214,std::bit_cast<uint32_t>(4.f));
        Word(base+ui+0x218,std::bit_cast<uint32_t>(1.f));memory[ui+0x238]=1;
        Double(base+ui+0x48,10);Double(base+ui+0x228,4);Double(base+ui+0x230,1);Double(base+ui+0x248,1./60);
    }
    void ReplayAiTarget() {
        const auto ptr=[&](uintptr_t at,uintptr_t value){Word(base+at,static_cast<uint32_t>(base+value));};
        ptr(0x10000+0xac,0x16008);ptr(0x16004,0x17000);ptr(0x17000,0xcdfeb4);Word(base+0x17000+0xb0,1);
        Word(base+0x10190+0x18,2);ptr(0x12008,0x12200);ptr(0x12108,0x12200);
        ptr(0x12200,0x12000);ptr(0x12204,0x12100);ptr(0x12208,0x12000);ptr(0x1220c,0x17000);ptr(0x12210,0x19000);
        ptr(0x19000,0xd0bb30);ptr(0x19008,0x16200);ptr(0x1900c,0x16204);ptr(0x16200,0x18000);ptr(0x18000,0xd039ac);
        provider=base+0x17000+0x12c;entity=nextEntity;
        Word(provider,static_cast<uint32_t>(base+chaseobserver::providerTableRva));Word(provider+8,static_cast<uint32_t>(entity));
        Word(provider+0xc,static_cast<uint32_t>(base+0x17000));Word(base+0x17000+0x148,static_cast<uint32_t>(manager));
        Word(camera+0x60,static_cast<uint32_t>(provider));
    }
    void MenuReplay() {
        const auto ptr=[&](uintptr_t at,uintptr_t value){Word(base+at,static_cast<uint32_t>(base+value));};
        Word(base+0x88030,3);ptr(0xdef804,0x71000);ptr(0x71000,0xce96b8);ptr(0x71000+0x1428,0x8b000);
        ptr(0x8b000,0xccaf98);ptr(0x8b010,0x8c000);ptr(0x8c004,0x8d000);Word(base+0xed14e8,69);
        ptr(0x8d000+69*4,0x8e000);ptr(0x8b038,0x8e000);ptr(0x8e000,0xccaf88);
        ptr(0x8e008,0x90000);ptr(0x8e00c,0x90000+120);
        std::memcpy(memory+0x90004,"earlier_flag",13);
        std::memcpy(memory+0x90000+40+4,"replay_from_menu_flag",21);
        std::memcpy(memory+0x90000+80+4,"zzz_flag",9);
        ptr(0x90000+40,0xed05bc);ptr(0x90000+40+36,0x95000);ptr(0x95000,0xcc5ba4);memory[0x95004]=1;
    }
    void ReplayFrame(uint64_t n,ChaseCameraInput input={},bool focused=true,bool poll=true,uintptr_t caller=transactionBeginCallerRva) {
        if(poll)TestInput(n,input,focused,1.f/60);
        alignas(16) std::array<uint8_t,0x320> stack{};
        const auto slot=reinterpret_cast<uintptr_t>(stack.data())+8;
        auto* const step=reinterpret_cast<void*>(slot+transactionTimingOffset);
        auto* const record=reinterpret_cast<void*>(slot+transactionRecordOffset);
        std::memcpy(step,timing.data(),sizeof(timing));expectedTiming=step;
        valid&=TestCall(1,reinterpret_cast<void*>(camera),step,expectedFlag)==alternateResult;
        valid&=TestTransaction(2,reinterpret_cast<void*>(camera),step,caller,slot)==stageResult;
        valid&=TestTransaction(5,reinterpret_cast<void*>(camera),record,transactionCopyCallerRva,slot)==reinterpret_cast<uintptr_t>(record);
        valid&=TestTransaction(7,reinterpret_cast<void*>(camera),record,transactionCommitCallerRva,slot)==copyBackResult;
        expectedTiming=timing.data();
    }
};
ChaseCameraInput Neutral() {ChaseCameraInput value;value.mouseAvailable=true;return value;}
ChaseCameraInput Motion() {auto value=Neutral();value.rightMouse=true;value.mouseX=100;value.wheel=-120;return value;}
std::string Receipt(const std::filesystem::path& folder) {std::ifstream in(folder/L"chase-camera.json");return {std::istreambuf_iterator<char>(in),{}};}
bool Has(const std::string& json,const char* text) {return json.find(text)!=std::string::npos;}
void Require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
}
int main(int argc,char** argv) {
    try {
        if(argc!=3)return 1;
        const std::string mode=argv[2];const auto folder=std::filesystem::path(argv[1])/mode;std::filesystem::create_directories(folder);
        if(mode=="recorded-captured") {
            // Exact bounded read-only slices from the failed pause-menu run.
            // Replay the reads as inert data; never access these process addresses.
            std::ifstream file(GRID2VR_REPLAY_CAPTURE_PATH);
            uintptr_t capturedBase{},capturedCamera{},capturedProvider{},capturedEntity{},address{};
            Require(bool(file>>std::hex>>capturedBase>>capturedCamera>>capturedProvider>>capturedEntity),"Capture header missing");
            std::map<uintptr_t,uint8_t> memory;std::string bytes;unsigned slices{};
            while(file>>std::hex>>address>>bytes) {
                Require(++slices<=256 && !bytes.empty() && bytes.size()%2==0 && bytes.size()<=128,"Capture slice exceeds bound");
                for(size_t i=0;i<bytes.size();i+=2)memory[address+i/2]=static_cast<uint8_t>(std::stoul(bytes.substr(i,2),nullptr,16));
            }
            const auto read=[&](uintptr_t at,void* out,size_t count) {
                for(size_t i=0;i<count;++i) {const auto found=memory.find(at+i);if(found==memory.end())return false;static_cast<uint8_t*>(out)[i]=found->second;}return true;
            };
            ReplayPlayback sample{};
            const auto refusal=CaptureRecordedReplay(capturedBase,capturedCamera,capturedProvider,capturedEntity,read,sample);
            if(refusal)throw std::runtime_error(refusal);
            Require(sample.direction==3 && sample.mode==2 && sample.clock.rate==0 && sample.menuScope[9],"Captured pause-menu state classified incorrectly");
            memory[sample.menuScope[9]+4]=0;
            Require(CaptureRecordedReplay(capturedBase,capturedCamera,capturedProvider,capturedEntity,read,sample),"Captured Flashback entry mutation accepted");
            return 0;
        }
        constexpr std::string_view modes[]{"absent","invalid","disabled","observer-conflict","guard-refusal","foreign-before",
            "forwarding","foreign-after","wrong-owner","second-eye","wrong-provider-caller","overlap-aba","changed-entity",
            "focus","stop","exit","getter-restore","copy-restore","deduplicate","apply","input-clock","pause","readonly-pose","duplicate-provider",
            "selection-roundtrip","look-type1","look-type2","look-other","look-focus","look-owner","look-malformed","look-flag",
            "look-second-eye","native-look-settling","rear-alternate","rear-no-alternate","controller",
            "pause-mode1","pause-invalid","pause-owner","look-paused","look-camera-gate","look-record-gate","look-delta-consumer","stop-inflight",
            "region-guard-refusal","region-history","no-device","look-no-device","embedded-replay2","embedded-replay1","replay-owner",
            "driving-stopped7","driving-recording2","replay-unknown","replay-override","replay-transport1",
            "local-race","local-provider","local-car","look-replay","replay-input-only","vehicle-pivot","mouse-always","mouse-inverted",
            "collision-target","collision-neutral","collision-caller","collision-argument","collision-result","collision-owner",
            "collision-stop","collision-second-eye","collision-stack","collision-invalid","collision-guard","collision-foreign","collision-abi","collision-duplicate",
            "transaction-roundtrip","transaction-copy-caller","transaction-copy-stack","transaction-back-stack","transaction-back-owner",
            "transaction-back-eye","transaction-stop",
            "recorded-play","recorded-pause","recorded-pause-menu","recorded-menu-play","recorded-menu-shuttle","recorded-menu-scope","recorded-flashback","recorded-rewind","recorded-fast","recorded-seek","recorded-jump","recorded-target",
            "recorded-menu","recorded-focus","recorded-duplicate","recorded-eye","recorded-caller","recorded-exit",
            "recorded-collision","recorded-look","recorded-guards","recorded-snapshot","recorded-ai-target","recorded-offline"};
        Require(std::find(std::begin(modes),std::end(modes),mode)!=std::end(modes),"Unknown test case");
        for(const auto name:{L"chase-camera.request",L"chase-camera-observer.request",L"chase-camera.json"})
            std::filesystem::remove(folder/name);
        Fixture fixture;
        if(mode=="driving-stopped7")Word(fixture.base+0x7a000+0x1d8,7);
        if(mode=="driving-recording2")Word(fixture.base+0x7a000+0x7604,2);
        if(mode!="absent") {
            std::ofstream file(folder/L"chase-camera.request",std::ios::binary);
            file<<(mode=="invalid"?"chase-v1 1 0 1 181 100 100 0\n":mode=="disabled"?"chase-v1 0 0 1 180 100 100 0\n":
                (mode=="recorded-pause-menu" || mode=="recorded-menu-play" || mode=="recorded-menu-shuttle")?"chase-v1 1 0 2 180 100 100 1\n":
                mode=="mouse-always"?"chase-v1 1 1 2 1000 100 100 0\n":mode=="mouse-inverted"?"chase-v1 1 1 1 1000 100 100 1\n":"chase-v1 1 1 1 1000 100 100 0\n");
        }
        if(mode=="observer-conflict") {std::ofstream file(folder/L"chase-camera-observer.request");file<<"observe-v1\n";}
        if(mode=="guard-refusal")fixture.memory[0x18f9ab]^=1;
        if(mode=="region-guard-refusal")fixture.memory[RegionGuards()[0].rva]^=1;
        if(mode=="collision-guard")fixture.memory[collisionGuards[0].rva]^=1;
        if(mode=="foreign-before")Word(fixture.base+chaseobserver::SlotRva(0),0x12345678);
        Prepare(fixture.base,folder);Install();
        if(mode=="absent" || mode=="invalid" || mode=="disabled" || mode=="guard-refusal" || mode=="region-guard-refusal" || mode=="collision-guard" || mode=="observer-conflict" || mode=="foreign-before") {
            const auto json=Receipt(folder);
            if(mode=="absent")Require(json.empty(),"Absent request created a receipt");
            else {
                Require(Has(json,"\"installed\":false"),"Refused request installed hooks");
                const auto expected=mode=="invalid"?"invalid_request":mode=="disabled"?"disabled":mode=="observer-conflict"?"observer_conflict":"instruction_or_vtable_guard";
                Require(Has(json,expected),"Wrong installation refusal");
            }
            if(mode=="foreign-before")Require(*reinterpret_cast<uint32_t*>(fixture.base+chaseobserver::SlotRva(0))==0x12345678,"Foreign hook overwritten");
            return 0;
        }
        Require(Has(Receipt(folder),"\"installed\":true"),"Hooks not installed");fixture.Originals();
        const auto originalRecord=std::vector<uint8_t>(reinterpret_cast<uint8_t*>(camera+0xb0),reinterpret_cast<uint8_t*>(camera+0xb0+chaseobserver::recordBytes));
        if(mode.starts_with("recorded-")) {
            fixture.Replay();const auto ui=fixture.base+0x7a000;
            if(mode=="recorded-ai-target") {fixture.ReplayAiTarget();Require(NormalLocalRaceReady(fixture.base),"AI replay fixture lost offline provenance");}
            const auto reader=[](uintptr_t at,void* out,size_t bytes){return ReadMemory(reinterpret_cast<void*>(at),out,bytes);};
            ReplayPlayback sample{};
            Require(!CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Replay snapshot refused");
            Require(sample.clock.rate==1 && sample.transport==0,"Ordinary UI transport0 was not accepted");
            if(mode=="recorded-guards") {
                for(const auto& guard:PlaybackClockGuards(fixture.base)) {
                    fixture.memory[guard.rva]^=1;Require(!RecordedReplayGuardsMatch(fixture.base),"Clock mutation accepted");fixture.memory[guard.rva]^=1;
                }
                for(const auto& guard:ReplayOwnerInputGuards(fixture.base)) {
                    fixture.memory[guard.rva]^=1;Require(!RecordedReplayGuardsMatch(fixture.base),"Owner mutation accepted");fixture.memory[guard.rva]^=1;
                }
                for(const auto& guard:ReplayDirectorThreeGuards(fixture.base)) {
                    fixture.memory[guard.rva]^=1;Require(!RecordedReplayGuardsMatch(fixture.base),"Pause-menu director mutation accepted");fixture.memory[guard.rva]^=1;
                }
                for(const auto& guard:ReplayShuttleGuards(fixture.base)) {
                    fixture.memory[guard.rva]^=1;Require(!RecordedReplayGuardsMatch(fixture.base),"Shuttle mutation accepted");fixture.memory[guard.rva]^=1;
                }
                for(const auto& guard:ReplayMenuScopeGuards(fixture.base)) {
                    fixture.memory[guard.rva]^=1;Require(!RecordedReplayGuardsMatch(fixture.base),"Menu scope mutation accepted");fixture.memory[guard.rva]^=1;
                }
            } else if(mode=="recorded-menu-scope") {
                fixture.MenuReplay();
                Require(!CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Pause-menu scope refused");
                for(const auto offset:{0xdef804,0x71000,0x71000+0x1428,0x8b000,0x8b010,0x8c004,0x8b038,0x8d000+69*4,0x8e000,0x8e008,0x8e00c,0x90000+40,0x95000}) {
                    uint32_t original{};reader(fixture.base+offset,&original,4);Word(fixture.base+offset,0);
                    Require(CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Malformed pause-menu scope accepted");Word(fixture.base+offset,original);
                }
                Word(fixture.base+0xed14e8,4097);Require(CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Unbounded resource index accepted");Word(fixture.base+0xed14e8,69);
                Word(fixture.base+0x8e00c,static_cast<uint32_t>(fixture.base+0x90000+65537*40));
                Require(CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Unbounded native map accepted");Word(fixture.base+0x8e00c,static_cast<uint32_t>(fixture.base+0x90000+120));
                fixture.memory[0x95004]=2;Require(CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Malformed bool accepted");fixture.memory[0x95004]=1;
                unsigned reads{};
                const auto changing=[&](uintptr_t at,void* out,size_t bytes) {const auto ok=reader(at,out,bytes);if(at==fixture.base+0x95004 && ++reads==2)*static_cast<uint8_t*>(out)=0;return ok;};
                Require(CaptureRecordedReplay(fixture.base,camera,provider,entity,changing,sample),"Changing menu entry accepted");
                std::memcpy(fixture.memory+0x90000+80,fixture.memory+0x90000+40,40);
                Require(CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Duplicate menu flag accepted");
            } else if(mode=="recorded-flashback") {
                fixture.MenuReplay();fixture.memory[0x95004]=0;
                const auto refusal=CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample);
                Require(refusal && std::strcmp(refusal,"replay_flashback_excluded")==0,"Flashback entry accepted");
                fixture.ReplayFrame(1,Neutral());fixture.ReplayFrame(2,Motion());Require(afterProvider==nativePose,"Flashback changed camera");
                fixture.memory[0x95004]=1;Word(fixture.base+0x88030,11);
                Require(CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Menu-to-crashback director accepted with stale menu flag");
                Word(fixture.base+0x88030,3);fixture.memory[0x7a000+0x168]=1;
                Require(CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Special rewind accepted from menu");
                fixture.memory[0x7a000+0x168]=0;
                fixture.ReplayFrame(3,Neutral());fixture.ReplayFrame(4,Motion());Require(afterProvider!=nativePose,"Menu replay did not rearm after excluded transition");
            } else if(mode=="recorded-snapshot") {
                for(uint32_t nativeMode=1;nativeMode<=6;++nativeMode) {
                    Word(ui+0x1d8,nativeMode);Require(!CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Supported playback mode refused");
                    const double expected[]{0,1,0,-4,4,.1,-.1};Require(std::abs(sample.clock.rate-expected[nativeMode])<1e-8,"Native replay rate mismatch");
                }
                Word(ui+0x1d8,1);Word(ui+0x218,std::bit_cast<uint32_t>(-2.f));
                Require(!CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample) && sample.clock.rate==-2,"Signed play rate lost");
                fixture.memory[0x7a000+0x210]=1;
                Require(!CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample) && sample.clock.rate==0,"Rewind boundary ignored");
                fixture.memory[0x7a000+0x210]=0;Word(ui+0x218,std::bit_cast<uint32_t>(1.f));
                Word(ui+0x28,1);fixture.memory[0x7a000+0x181]=1;
                Require(!CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample) && sample.clock.rate==0,"Playback endpoint ignored");
                Word(ui+0x28,0);fixture.memory[0x7a000+0x181]=0;
                Word(ui+0x1d8,11);Word(ui+0x220,std::bit_cast<uint32_t>(.5f));Word(ui+0x218,std::bit_cast<uint32_t>(-4.f));Double(ui+0x230,2);
                Require(!CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample) && sample.clock.rate==-4,"Native shuttle product mismatch");
                fixture.memory[0x7a000+0x210]=1;
                Require(!CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample) && sample.clock.rate==0,"Shuttle start boundary ignored");
                fixture.memory[0x7a000+0x210]=0;Word(ui+0x218,std::bit_cast<uint32_t>(4.f));Word(ui+0x28,1);fixture.memory[0x7a000+0x181]=1;
                Require(!CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample) && sample.clock.rate==0,"Shuttle end boundary ignored");
                Word(ui+0x28,0);fixture.memory[0x7a000+0x181]=0;
                Word(ui+0x220,std::bit_cast<uint32_t>(std::numeric_limits<float>::infinity()));
                Require(CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Nonfinite shuttle scale accepted");
                Word(ui+0x220,std::bit_cast<uint32_t>(1.f));Word(ui+0x218,std::bit_cast<uint32_t>(1.f));Double(ui+0x230,1);
                for(uint32_t nativeMode:{0u,7u,8u,9u,10u,12u,13u,UINT32_MAX}) {
                    Word(ui+0x1d8,nativeMode);Require(CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Unsupported UI mode accepted");
                }
                Word(ui+0x1d8,1);
                for(uint32_t director:{1u,2u,4u,5u,6u,7u,8u,9u,11u,UINT32_MAX}) {
                    Word(fixture.base+0x88030,director);Require(CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Unsupported director accepted");
                }
                Word(fixture.base+0x88030,10);
                Word(ui+0x7604,1);Require(!CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Buffered replay transport refused");
                Word(ui+0x7604,2);Require(CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Recording transport accepted as replay");Word(ui+0x7604,0);
                for(const auto offset:{0x73000+0x391,0x73000+0x392,0x73000+0x3a2,0x73000+0x3a3,0x75000+0x2bc,0x7a000+0x168}) {
                    fixture.memory[offset]=1;Require(CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Native suppression ignored");fixture.memory[offset]=0;
                }
                for(const auto offset:{0x86000+0xa8,0x86000+0xac,0x88000+0x10,0x88000+0x18,0x88000+0x1c,0x8a000+0xa8,0x7a000+0x75f0,0x7a000+0x7638,0x11000+0x148}) {
                    uint32_t original{};reader(fixture.base+offset,&original,4);Word(fixture.base+offset,original+4);
                    Require(CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Broken replay graph accepted");Word(fixture.base+offset,original);
                }
                unsigned reads{};
                const auto changing=[&](uintptr_t at,void* out,size_t bytes) {
                    const auto ok=reader(at,out,bytes);if(at==ui+0x48 && ++reads==2){double changed=99;std::memcpy(out,&changed,8);}return ok;
                };
                Require(CaptureRecordedReplay(fixture.base,camera,provider,entity,changing,sample),"Changing replay snapshot accepted");
                Double(ui+0x48,std::numeric_limits<double>::infinity());Require(CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample),"Nonfinite replay clock accepted");
            } else {
                const bool pauseMenu=mode=="recorded-pause-menu" || mode=="recorded-menu-play" || mode=="recorded-menu-shuttle";
                if(pauseMenu) {
                    // Captured run20261027-182403-018: pause menu Replay, not flashback.
                    fixture.MenuReplay();Word(ui+0x1d8,2);Word(ui+0x28,1);
                    Double(ui+0x48,25.78123076851615);Double(ui+0x248,1./120);
                    fixture.memory[0x78000+0x2f6]=0;fixture.timing[0]=1.f/120;
                    std::memcpy(reinterpret_cast<void*>(camera+0x19),"chase_far",10);
                    if(mode=="recorded-menu-play") {Word(ui+0x1d8,1);fixture.memory[0x78000+0x2f6]=0;fixture.timing[0]=1.f/60;}
                }
                if(mode=="recorded-pause") {Word(ui+0x1d8,2);fixture.memory[0x78000+0x2f6]=1;fixture.timing[0]=0;Double(ui+0x248,0);}
                if(mode=="recorded-rewind") {Word(ui+0x1d8,3);fixture.timing[0]=-1.f/60;}
                if(mode=="recorded-fast")Word(ui+0x1d8,4);
                fixture.ReplayFrame(1,Neutral());fixture.ReplayFrame(2,Neutral());
                const auto recordingBefore=std::vector<uint8_t>(fixture.memory+0x8a000,fixture.memory+0x8a100);
                collisionTest=mode=="recorded-collision" || pauseMenu;
                if(mode=="recorded-menu-shuttle") {
                    Word(ui+0x1d8,11);Word(ui+0x220,std::bit_cast<uint32_t>(1.f));Word(ui+0x218,std::bit_cast<uint32_t>(-4.f));
                    Double(ui+0x48,25.78123076851615-4./60);fixture.timing[0]=-4.f/60;
                    Require(!CaptureRecordedReplay(fixture.base,camera,provider,entity,reader,sample) && sample.clock.rate==-4,"Paused replay shuttle was refused or lost its signed rate");
                }
                if(mode=="recorded-fast" || mode=="recorded-rewind") {
                    Double(ui+0x48,10+(mode=="recorded-fast"?4.:-4.)/60);
                    Word(entity+0x330,std::bit_cast<uint32_t>(200.f/3.6f*4.f/60));
                }
                auto input=Motion();
                if(pauseMenu) {input.rightMouse=false;input.mouseY=50;}
                fixture.ReplayFrame(3,input);const auto displayed=afterProvider;
                Require(displayed!=nativePose && GetPose(camera)==displayed,"Replay controls did not survive native copy transaction");
                if(mode=="recorded-duplicate") {fixture.ReplayFrame(3,{},true,false);Require(afterProvider==displayed,"Repeated replay evaluation consumed input twice");}
                if(mode=="recorded-fast" || mode=="recorded-rewind") {
                    Double(ui+0x48,10+(mode=="recorded-fast"?8.:-8.)/60);
                    Word(entity+0x330,std::bit_cast<uint32_t>(200.f/3.6f*8.f/60));
                    fixture.ReplayFrame(4,Motion());Require(afterProvider!=nativePose && afterProvider!=displayed,"Fast playback reset camera orbit or failed to advance input");
                }
                if(mode=="recorded-pause" || mode=="recorded-pause-menu") {fixture.ReplayFrame(4,input);Require(afterProvider!=displayed,"Stationary replay stopped camera input");}
                if(mode=="recorded-menu-shuttle") {
                    Word(ui+0x1d8,2);fixture.timing[0]=0; // Native release returns shuttle to pause.
                    fixture.ReplayFrame(4,input);Require(afterProvider!=displayed && afterProvider!=nativePose,"Shuttle release stopped paused replay controls");
                }
                if(pauseMenu)Require(targetCalls>=1 && collisionOutput!=nativeTarget,"Pause-menu replay bypassed collision target correction");
                if(mode=="recorded-collision")Require(targetCalls==1 && collisionOutput!=nativeTarget,"Replay collision target did not orbit");
                if(mode=="recorded-look") {
                    LookPacket packet{static_cast<uint32_t>(fixture.base+0xce095c),1,std::bit_cast<uint32_t>(.5f),std::bit_cast<uint32_t>(-.5f),0};
                    const auto before=packet;TestCall(6,reinterpret_cast<void*>(camera),packet.data());
                    Require(packet==before && receivedLook[2]==0 && receivedLook[3]==0 && receivedLookAddress!=packet.data(),"Replay look packet arbitration failed");
                }
                if(mode=="recorded-seek" || mode=="recorded-jump" || mode=="recorded-target" || mode=="recorded-menu" || mode=="recorded-focus" || mode=="recorded-eye" || mode=="recorded-caller" || mode=="recorded-exit" || mode=="recorded-offline") {
                    if(mode=="recorded-seek")Double(ui+0x250,10); // A completed seek missed between polls.
                    if(mode=="recorded-jump")Double(ui+0x48,60);
                    if(mode=="recorded-target") {Word(provider+8,static_cast<uint32_t>(nextEntity));entity=nextEntity;}
                    if(mode=="recorded-menu")Word(fixture.base+0x88030,2);
                    if(mode=="recorded-eye")engine::testSecondEye=true;
                    if(mode=="recorded-exit")Word(ui+0x1d8,0);
                    if(mode=="recorded-offline")fixture.memory[0x14040]=0;
                    fixture.ReplayFrame(4,Motion(),mode!="recorded-focus",true,transactionBeginCallerRva+(mode=="recorded-caller"?1:0));
                    Require(afterProvider==nativePose,"Unsafe replay transition applied held input");
                    engine::testSecondEye=false;Word(fixture.base+0x88030,0);
                    fixture.memory[0x14040]=1;
                    fixture.ReplayFrame(5,Neutral());fixture.ReplayFrame(6,Motion());Require(afterProvider!=nativePose,"Replay controls failed to rearm");
                }
                Require(!std::memcmp(recordingBefore.data(),fixture.memory+0x8a000,recordingBefore.size()),"Recorded playback data changed");
            }
            Stop();Require(!Has(Receipt(folder),"\"failed\":true"),"Replay transaction failed closed unexpectedly");
        } else if(mode=="collision-foreign") {
            fixture.memory[collisionTargetCallRva+1]^=1;
            const auto foreign=fixture.memory[collisionTargetCallRva+1];
            Require(std::strcmp(StopForExit(),"pointer_restore_failed")==0,"Foreign target call not refused");
            Require(fixture.memory[collisionTargetCallRva+1]==foreign,"Foreign target call overwritten");return 0;
        } else if(mode.starts_with("collision-")) {
            fixture.Frame(1,Neutral());fixture.Frame(2,Neutral());
            collisionTest=true;nativeAdjustment=true;
            collisionDuplicate=mode=="collision-duplicate";
            if(mode=="collision-abi") {
                // Synthetic executable caller only: push choice/output, execute
                // the real installed CALL and wrapper, then return to this test.
                // No instruction from a game executable is executed.
                const std::array<uint8_t,8> args{0xff,0x74,0x24,0x08,0xff,0x74,0x24,0x08};
                const std::array<uint8_t,3> ret{0xc2,0x08,0x00};
                std::memcpy(fixture.memory+collisionTargetCallRva-args.size(),args.data(),args.size());
                std::memcpy(fixture.memory+collisionTargetReturnRva,ret.data(),ret.size());
                DWORD old{};
                Require(VirtualProtect(fixture.memory+(collisionTargetCallRva&~0xfffu),0x1000,PAGE_EXECUTE_READWRITE,&old)!=0,
                    "Synthetic caller protection failed");
                Require(FlushInstructionCache(GetCurrentProcess(),fixture.memory+collisionTargetCallRva-args.size(),16)!=0,
                    "Synthetic caller flush failed");
                collisionAbi=reinterpret_cast<TargetCall>(fixture.base+collisionTargetCallRva-args.size());
            }
            collisionCaller=mode=="collision-caller"?0x36706e:collisionTargetReturnRva;
            collisionArgument=mode=="collision-argument"?0:1;
            collisionWrongResult=mode=="collision-result";
            collisionStop=mode=="collision-stop";collisionSecondEye=mode=="collision-second-eye";
            collisionNoStack=mode=="collision-stack";changeEntity=mode=="collision-owner";
            if(mode=="collision-invalid")nativeTarget[12]=std::bit_cast<float>(0x7fc00000u);
            auto input=Motion();input.wheel=0;
            fixture.Frame(3,mode=="collision-neutral"?Neutral():input);
            Require(targetCalls==(collisionDuplicate?2u:1u),"Collision helper was not forwarded exactly once per call");
            Require(!std::memcmp(collisionOutput.data(),nativeTarget.data(),48) && collisionOutput[15]==nativeTarget[15],"Collision basis/w changed");
            if(mode=="collision-target" || mode=="collision-abi" || mode=="collision-duplicate") {
                Require(std::abs(collisionOutput[12]+1.8f*std::sin(.3f))<.0001f &&
                    std::abs(collisionOutput[13]-1)<.0001f && std::abs(collisionOutput[14]-1.8f*std::cos(.3f))<.0001f,
                    "Collision response kept the old forward target");
                auto displaced=afterProvider;displaced[6]+=.25f;
                Require(GetPose(camera)==displaced,"Target correction erased native collision displacement");
            } else Require(!std::memcmp(collisionOutput.data(),nativeTarget.data(),sizeof(nativeTarget)),"Unsupported target output changed");
            changeEntity=false;if(mode=="collision-owner")Word(provider+8,static_cast<uint32_t>(entity));
        } else if(mode.starts_with("look-")) {
            if(mode=="look-replay") {fixture.Frame(1,Neutral());fixture.Frame(2,Neutral());}
            TestInput(mode=="look-replay"?3:1,mode=="look-no-device"?ChaseCameraInput{}:Neutral(),mode!="look-focus");
            if(mode=="look-owner")Word(manager+0x748,static_cast<uint32_t>(camera+0x4000));
            if(mode=="look-second-eye")engine::testSecondEye=true;
            if(mode=="look-paused")fixture.memory[0x78000+0x2f6]=1;
            if(mode=="look-replay")Word(fixture.base+0x7a000+0x1d8,2);
            if(mode=="look-camera-gate")Word(camera+0xc,0);
            if(mode=="look-record-gate")Word(camera+0xb0+0x1a4,2);
            if(mode=="look-delta-consumer") {Word(manager+0x744,static_cast<uint32_t>(fixture.base+0x79000));fixture.memory[0x79000+0x129]=1;}
            LookPacket packet{static_cast<uint32_t>(fixture.base+0xce095c),mode=="look-type2"?2u:1u,
                std::bit_cast<uint32_t>(.75f),std::bit_cast<uint32_t>(-.5f),0xaabbcc01};
            if(mode=="look-other")packet[1]=3;
            if(mode=="look-malformed")packet[0]+=4;
            if(mode=="look-flag")packet[4]=0xaabbcc02;
            const auto before=packet;
            Require(TestCall(6,reinterpret_cast<void*>(camera),packet.data())==lookResult,"Look native return changed");
            Require(calls[6]==1 && valid,"Look native callback not forwarded exactly once");
            Require(packet==before,"Shared look packet was mutated");
            if(mode=="look-type1" || mode=="look-type2" || mode=="look-delta-consumer") {
                auto expected=before;expected[2]=expected[3]=0;
                Require(receivedLook==expected && receivedLookAddress!=packet.data(),"Eligible look did not receive private neutral axes with metadata preserved");
            } else Require(receivedLook==before && receivedLookAddress==packet.data(),"Unsupported look input was changed");
            if(mode=="look-replay") {
                Word(fixture.base+0x7a000+0x1d8,0);fixture.Frame(4,Motion());
                Require(afterProvider==nativePose,"Look-only replay interval retained armed input");
                fixture.Frame(5,Neutral());fixture.Frame(6,Motion());
                Require(afterProvider!=nativePose,"Look-only replay interval did not recover after neutral");
            }
            if(mode=="look-record-gate")Word(camera+0xb0+0x1a4,0);
            if(mode=="look-delta-consumer") {
                fixture.Frame(2,Neutral());fixture.Frame(3,Neutral());fixture.Frame(4,Motion());
                Require(afterProvider!=nativePose,"Native camera-region consumer disabled chase controls");
            }
        } else if(mode=="replay-input-only") {
            fixture.Frame(1,Neutral());fixture.Frame(2,Neutral());
            Word(fixture.base+0x7a000+0x1d8,2);TestInput(3,Motion(),true);
            Word(fixture.base+0x7a000+0x1d8,0);fixture.Frame(4,Motion());
            Require(afterProvider==nativePose,"Replay interval without camera callbacks retained armed input");
            fixture.Frame(5,Neutral());fixture.Frame(6,Motion());
            Require(afterProvider!=nativePose,"Input-only replay interval did not recover after neutral");
        } else if(mode.starts_with("transaction-")) {
            fixture.Frame(1,Neutral());fixture.Frame(2,Neutral());
            TestInput(3,Motion(),true);
            Require(TestCall(0,reinterpret_cast<void*>(camera),fixture.timing.data())==updateResult,"Transaction update result changed");
            alignas(16) std::array<uint8_t,0x320> stack{};
            const auto slot=reinterpret_cast<uintptr_t>(stack.data())+8;
            auto* const timing=reinterpret_cast<void*>(slot+transactionTimingOffset);
            auto* const buffer=reinterpret_cast<void*>(slot+transactionRecordOffset);
            std::memcpy(timing,fixture.timing.data(),sizeof(fixture.timing));expectedTiming=timing;nativeAdjustment=true;
            Require(TestTransaction(2,reinterpret_cast<void*>(camera),timing,transactionBeginCallerRva,slot)==stageResult,"Transaction stage result changed");
            auto displayed=GetPose(camera);Require(displayed!=nativePose,"Transaction stage did not apply controls");
            const auto copyCaller=mode=="transaction-copy-caller"?transactionCopyCallerRva+1:transactionCopyCallerRva;
            const auto copySlot=mode=="transaction-copy-stack"?slot+16:slot;
            Require(TestTransaction(5,reinterpret_cast<void*>(camera),buffer,copyCaller,copySlot)==reinterpret_cast<uintptr_t>(buffer),"Transaction copy result changed");
            const bool copyRefused=mode=="transaction-copy-caller" || mode=="transaction-copy-stack";
            Require(copyEntry==(copyRefused?nativePose:displayed),"Transaction copy preserved/restored the wrong pose");
            Pose copied{};std::memcpy(copied.data(),buffer,sizeof(copied));copied[4]+=.125f;
            std::memcpy(buffer,copied.data(),sizeof(copied)); // Native slot9 postprocess.
            if(mode=="transaction-stop" || copyRefused) {
                Stop();Require(!fixture.HooksRestored(),"Stop unhooked before outstanding transaction commit");
            }
            if(mode=="transaction-back-owner")Word(provider+8,static_cast<uint32_t>(nextEntity));
            if(mode=="transaction-back-eye")engine::testSecondEye=true;
            const auto commitSlot=mode=="transaction-back-stack"?slot+16:slot;
            Require(TestTransaction(7,reinterpret_cast<void*>(camera),buffer,transactionCommitCallerRva,commitSlot)==copyBackResult,"Transaction copy-back result changed");
            Require(GetPose(camera)==copied,"Native postprocess result was overwritten");
            if(mode=="transaction-back-stack") {
                Stop();Require(!fixture.HooksRestored(),"Wrong-stack commit drained outstanding native frame");
                Require(TestTransaction(7,reinterpret_cast<void*>(camera),buffer,transactionCommitCallerRva,slot)==copyBackResult,"Actual commit not forwarded");
            }
            engine::testSecondEye=false;
            Require(calls[5]==1 && calls[7]==(mode=="transaction-back-stack"?2u:1u),"Transaction originals were not forwarded once per invocation");
            if(mode=="transaction-roundtrip") {
                expectedTiming=fixture.timing.data();TestCall(0,reinterpret_cast<void*>(camera),fixture.timing.data());
                Require(updateEntry==nativePose,"Native update consumed postprocessed camera overlay");
            }
            Stop();Require(fixture.HooksRestored(),"Completed transaction did not permit hook cleanup");
            if(mode=="transaction-stop")Require(GetPose(camera)==nativePose,"Deferred stop did not restore transaction baseline");
            if(mode!="transaction-roundtrip" && mode!="transaction-stop")Require(Has(Receipt(folder),"\"failed\":true"),"Invalid transaction did not fail closed");
        } else if(mode=="forwarding") {
            TestInput(1,Neutral(),false);
            Require(TestCall(0,reinterpret_cast<void*>(camera),fixture.timing.data())==updateResult,"Update result changed");
            Require(TestCall(1,reinterpret_cast<void*>(camera),fixture.timing.data(),expectedFlag)==alternateResult,"Alternate result changed");
            Require(TestCall(2,reinterpret_cast<void*>(camera),fixture.timing.data())==stageResult,"Stage result changed");
            Require(TestGetter(reinterpret_cast<void*>(camera),0x8047a0)==camera+0xb0,"Getter result changed");
            std::array<uint8_t,chaseobserver::recordBytes> copy{};
            Require(TestCall(5,reinterpret_cast<void*>(camera),copy.data())==reinterpret_cast<uintptr_t>(copy.data()),"Copy result changed");
            Require(valid && calls[0]==1 && calls[1]==1 && calls[2]==1 && calls[3]==1 && calls[4]==1 && calls[5]==1,"Native callback forwarding changed");
        } else if(mode=="vehicle-pivot" || mode=="mouse-always" || mode=="mouse-inverted") {
            // Real requests through the guarded adapter, with a forward native
            // camera target matching the retained close/far observation.
            Word(camera+0x2454,std::bit_cast<uint32_t>(1.0774f));
            Word(camera+0x2458,std::bit_cast<uint32_t>(1.8122f));
            std::array<uint8_t,48> targetBefore{};
            std::memcpy(targetBefore.data(),reinterpret_cast<void*>(camera+0x2430),targetBefore.size());
            fixture.Frame(1,Neutral());fixture.Frame(2,Neutral());
            auto input=Neutral();input.rightMouse=mode!="mouse-always";
            if(mode=="vehicle-pivot")input.mouseX=100;
            else input.mouseY=100;
            fixture.Frame(3,input);
            Require(afterProvider!=nativePose,"Requested mouse mode did not move the camera");
            const auto closeEnough=[](float a,float b){return std::abs(a-b)<.0001f;};
            if(mode=="vehicle-pivot") {
                // 100 horizontal counts = -0.3 radians around vehicle origin.
                Require(closeEnough(afterProvider[4],5*std::sin(.3f)) && closeEnough(afterProvider[5],2) &&
                    closeEnough(afterProvider[6],-5*std::cos(.3f)),"Orbit still uses the forward look target");
            } else {
                const float pitch=mode=="mouse-inverted"?.3f:-.3f;
                Require(closeEnough(afterProvider[4],0) && closeEnough(afterProvider[5],2*std::cos(pitch)+5*std::sin(pitch)) &&
                    closeEnough(afterProvider[6],2*std::sin(pitch)-5*std::cos(pitch)),"Saved inversion did not reach native camera pose");
            }
            Require(!std::memcmp(targetBefore.data(),reinterpret_cast<void*>(camera+0x2430),targetBefore.size()),"Native obstruction target changed");
            TestCall(0,reinterpret_cast<void*>(camera),fixture.timing.data());
            Require(updateEntry==nativePose,"Pivot change leaked into next native update");
        } else if(mode=="foreign-after") {
            Word(fixture.base+chaseobserver::SlotRva(0),0x12345678);
            Require(std::strcmp(StopForExit(),"pointer_restore_failed")==0,"Foreign replacement was not refused");
            Require(*reinterpret_cast<uint32_t*>(fixture.base+chaseobserver::SlotRva(0))==0x12345678,"Foreign installed pointer overwritten");return 0;
        } else {
            auto neutral=Neutral();if(mode=="controller")neutral.stickAvailable=true;
            fixture.Frame(1,neutral);fixture.Frame(2,neutral);
            if(mode=="wrong-owner")Word(manager+0x748,static_cast<uint32_t>(camera+0x4000));
            if(mode=="second-eye")engine::testSecondEye=true;
            if(mode=="wrong-provider-caller")providerCaller=0x1f78ec;
            if(mode=="overlap-aba")overlap=true;
            if(mode=="changed-entity")changeEntity=true;
            if(mode=="pause")fixture.timing[0]=0;
            if(mode=="pause-mode1")fixture.memory[0x78000+0x2f6]=1;
            if(mode=="pause-invalid")fixture.memory[0x78000+0x2f6]=2;
            if(mode=="pause-owner")Word(fixture.base+0x78000,static_cast<uint32_t>(fixture.base+0xccaf14));
            if(mode=="embedded-replay2")Word(fixture.base+0x7a000+0x1d8,2);
            if(mode=="embedded-replay1")Word(fixture.base+0x7a000+0x1d8,1);
            if(mode=="replay-unknown")Word(fixture.base+0x7a000+0x1d8,UINT32_MAX);
            if(mode=="replay-override")Word(fixture.base+0x7a000+0x7640,1);
            if(mode=="replay-transport1")Word(fixture.base+0x7a000+0x7604,1);
            if(mode=="replay-owner")Word(fixture.base+0x7a000,static_cast<uint32_t>(fixture.base+0xd10644));
            if(mode=="local-race")Word(fixture.base+0xe5917c,3);
            if(mode=="local-provider") {
                std::memcpy(reinterpret_cast<void*>(fixture.base+0x72000),reinterpret_cast<void*>(provider),16);
                provider=fixture.base+0x72000;Word(camera+0x60,static_cast<uint32_t>(provider));
            }
            if(mode=="local-car")Word(provider+0xc,static_cast<uint32_t>(fixture.base+0x11400));
            if(mode=="readonly-pose")readOnlyPose=true;
            if(mode=="duplicate-provider")repeatProvider=true;
            if(mode=="stop-inflight")stopInStage=true;
            if(mode=="native-look-settling")Word(camera+0xb0+0x174,std::bit_cast<uint32_t>(.02f));
            if(mode=="rear-alternate" || mode=="rear-no-alternate") {
                *reinterpret_cast<uint8_t*>(manager+0x759)=1;
                if(mode=="rear-alternate")Word(camera+0x70,static_cast<uint32_t>(camera+0x4000));
            }
            nativeAdjustment=true;
            auto motion=Motion();if(mode=="input-clock")motion.mouseX=0;
            if(mode=="no-device")motion={};
            if(mode=="region-history")motion.wheel=0;
            if(mode=="controller") {motion=neutral;motion.stickX=.6f;motion.stickY=.4f;}
            fixture.Frame(3,motion,mode!="focus",mode=="input-clock"?1.f/30:1.f/60);
            if(mode=="wrong-owner" || mode=="second-eye" || mode=="wrong-provider-caller" || mode=="overlap-aba" ||
               mode=="focus" || mode=="pause" || mode=="readonly-pose" || mode=="native-look-settling" || mode=="rear-alternate" ||
               mode=="pause-mode1" || mode=="pause-invalid" || mode=="pause-owner" || mode=="no-device" ||
               mode=="embedded-replay2" || mode=="embedded-replay1" || mode=="replay-owner" ||
               mode=="replay-unknown" || mode=="replay-override" || mode=="replay-transport1" ||
               mode=="local-race" || mode=="local-provider" || mode=="local-car") {
                Require(afterProvider==nativePose && GetPose(camera)==nativePose,"Unsupported state changed the native pose");
                if(mode=="embedded-replay1" || mode=="embedded-replay2" || mode=="replay-unknown" || mode=="replay-override" || mode=="replay-transport1" ||
                   mode=="replay-owner" || mode=="local-race" || mode=="local-provider" || mode=="local-car") {
                    LookPacket packet{static_cast<uint32_t>(fixture.base+0xce095c),1,
                        std::bit_cast<uint32_t>(.75f),std::bit_cast<uint32_t>(-.5f),0xaabbcc01};
                    const auto before=packet;
                    Require(TestCall(6,reinterpret_cast<void*>(camera),packet.data())==lookResult,"Refused target changed native look return");
                    Require(packet==before && receivedLook==before && receivedLookAddress==packet.data(),"Refused target suppressed native look input");
                }
                if(mode=="embedded-replay2") {
                    Word(fixture.base+0x7a000+0x1d8,0);fixture.Frame(4,Motion());
                    Require(afterProvider==nativePose,"Replay exit resumed held input without neutral");
                    fixture.Frame(5,Neutral());fixture.Frame(6,Motion());
                    Require(afterProvider!=nativePose,"Replay exit did not rearm after neutral");
                }
                if(mode=="native-look-settling") {
                    Word(camera+0xb0+0x174,0);fixture.Frame(4,Motion());
                    Require(afterProvider==nativePose,"Native settling resumed held camera input without neutral");
                    fixture.Frame(5,Neutral());fixture.Frame(6,Motion());Require(afterProvider!=nativePose,"Native look settling did not recover after neutral");
                }
                if(mode=="focus" || mode=="pause" || mode=="pause-mode1" || mode=="no-device") {
                    fixture.timing[0]=1.f/60;
                    fixture.memory[0x78000+0x2f6]=0;
                    fixture.Frame(4,Motion());Require(afterProvider==nativePose,"Held focus-recovery input moved the camera");
                    fixture.Frame(5,Neutral());fixture.Frame(6,Motion());Require(afterProvider!=nativePose,"Neutral recovery did not rearm camera input");
                }
            } else {
                Require(afterProvider!=nativePose,"Eligible mouse input did not modify pose");
                if(mode=="input-clock") {
                    const float expected=1.1f+(1.f-1.1f)*std::exp(-12.f/30);
                    Require(std::abs(afterProvider[6]/nativePose[6]-expected)<.00001f,"Runtime used native evaluation time instead of elapsed input time");
                }
                if(mode=="duplicate-provider")Require(calls[4]==4,"Repeated provider call was not forwarded once per native invocation");
                auto displayed=afterProvider;displayed[6]+=.25f;Require(GetPose(camera)==displayed,"Observer replaced native obstruction result");
                if(mode=="stop-inflight") {
                    Require(!fixture.HooksRestored(),"Inflight stop unhooked native camera before completion");
                    Present(4,5);Require(fixture.HooksRestored() && GetPose(camera)==nativePose,"Deferred stop did not restore owned pose and hook pointers");
                } else if(mode=="controller") {
                    auto rear=neutral;rear.rearHeld=true;fixture.Frame(4,rear);
                    Require(std::abs(afterProvider[4])<.0001f && std::abs(afterProvider[6]+nativePose[6])<.0001f,"Controller R3 did not produce rear view");
                    fixture.Frame(5,neutral);Require(afterProvider==nativePose,"R3 release retained rear angle with Keep angle enabled");
                } else if(mode=="changed-entity") {
                    TestCall(0,reinterpret_cast<void*>(camera),fixture.timing.data());
                    Require(updateEntry==displayed,"Old baseline was restored into a changed entity");
                } else if(mode=="stop") {
                    Stop();Require(GetPose(camera)==nativePose,"Runtime stop left a pose overlay feeding native updates");
                    Require(fixture.HooksRestored(),"Stop did not restore native hook addresses");return 0;
                } else if(mode=="exit") {
                    StopForExit();Require(GetPose(camera)==displayed,"Exit callback wrote a pose from the foreign exit thread");
                    Require(fixture.HooksRestored(),"Exit did not restore native hook addresses");return 0;
                } else if(mode=="getter-restore") {
                    TestGetter(reinterpret_cast<void*>(camera),0x8047a0);Require(getterEntry==displayed,"Render getter restored the overlay");
                    TestGetter(reinterpret_cast<void*>(camera),0x59a16d);Require(getterEntry==displayed,"Manager getter erased previous displayed pose");
                    TestCall(0,reinterpret_cast<void*>(camera),fixture.timing.data());Require(updateEntry==nativePose,"Update did not restore baseline after manager getter");
                } else if(mode=="region-history") {
                    Word(manager+0x744,static_cast<uint32_t>(fixture.base+0x79000));fixture.memory[0x79000+0x129]=1;
                    TestGetter(reinterpret_cast<void*>(camera),0x59a16d);const auto priorDisplayed=getterEntry;
                    fixture.Frame(4,Neutral());const auto currentDisplayed=GetPose(camera);
                    Require(updateEntry==nativePose,"Region-history update consumed modified baseline");
                    Require(currentDisplayed==priorDisplayed,"Unchanged held orbit created a new camera-region displacement");
                } else if(mode=="copy-restore") {
                    std::array<uint8_t,chaseobserver::recordBytes> copy{};
                    Require(TestCall(5,reinterpret_cast<void*>(camera),copy.data())==reinterpret_cast<uintptr_t>(copy.data()),"Copy result changed");
                    Require(copyEntry==nativePose && !std::memcmp(copy.data(),nativePose.data(),sizeof(nativePose)),"Record copy captured the modified pose");
                } else if(mode=="selection-roundtrip") {
                    Word(manager+0x748,static_cast<uint32_t>(camera+0x4000));fixture.Frame(4,Motion());
                    Require(afterProvider==nativePose,"Unselected chase camera was modified");
                    Word(manager+0x748,static_cast<uint32_t>(camera));fixture.Frame(5,Motion());
                    Require(afterProvider==nativePose,"Held input rearmed after returning from another view");
                    fixture.Frame(6,Neutral());fixture.Frame(7,Neutral());
                    Require(afterProvider==nativePose,"Orbit or zoom survived a camera selection roundtrip");
                    fixture.Frame(8,Motion());Require(afterProvider!=nativePose,"Camera did not rearm after selection change");
                } else if(mode=="deduplicate") {
                    const auto first=afterProvider;
                    TestCall(2,reinterpret_cast<void*>(camera),fixture.timing.data());
                    Require(afterProvider==first,"Repeated evaluation applied mouse input twice");
                } else {
                    TestCall(0,reinterpret_cast<void*>(camera),fixture.timing.data());Require(updateEntry==nativePose,"Native update consumed its prior modified pose");
                }
            }
        }
        Require(valid,"Native arguments or output sentinel changed");
        Require(!std::memcmp(reinterpret_cast<void*>(camera+0xb0+sizeof(Pose)),originalRecord.data()+sizeof(Pose),originalRecord.size()-sizeof(Pose)),"Non-pose record words changed");
        Stop();Require(fixture.HooksRestored(),"Native hook addresses were not restored");
        const char* expectedRefusal=mode=="local-race"?"local_race_not_ready":
            mode=="local-provider"?"local_provider_mismatch":mode=="local-car"?"local_provider_car_mismatch":
            mode=="replay-owner"?"driving_replay_type_mismatch":
            mode=="replay-override"?"driving_replay_override_active_or_unknown":
            mode=="replay-transport1"?"driving_replay_transport_active_or_unknown":
            mode=="embedded-replay1" || mode=="embedded-replay2" || mode=="replay-unknown" || mode=="look-replay" || mode=="replay-input-only"?
            "driving_replay_active_or_unknown":nullptr;
        if(expectedRefusal)Require(Has(Receipt(folder),expectedRefusal),"Ownership refusal did not identify its failed predicate");
        std::cout<<"Guarded chase runtime fixture passed: "<<mode<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 2;}
}
