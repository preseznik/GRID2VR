#include "opponent_lod_runtime.h"
#include "opponent_lod_native.h"
#include "game_executable.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>
#include <string>
#include <thread>
#include <vector>
#include <intrin.h>
#if defined(GRID2VR_OPPONENT_LOD_SINGLE_ATTEMPT_DIAGNOSTIC) || defined(GRID2VR_OPPONENT_LOD_CONTROLLED_ACTIVATION)
#include "opponent_lod_bridge.h"
#endif

#if !defined(_M_IX86) || !defined(GRID2VR_OPPONENT_LOD_RUNTIME_TEST)
#error This fixture is isolated MSVC x86; ZERO target instructions may execute.
#endif
namespace rt=grid2vr::opponentlod::runtime;
namespace native=grid2vr::opponentlod::native;
#define CHECK(x) do {if(!(x)){std::printf("line %d: %s (reason=%s)\n",__LINE__,#x,rt::Report().reason);return false;}} while(false)
namespace {
struct HostPunctuation:std::numpunct<char> {
    char do_thousands_sep() const override {return '_';}
    std::string do_grouping() const override {return "\3";}
    std::string do_truename() const override {return "ja";}
    std::string do_falsename() const override {return "nein";}
};
uintptr_t Addr(const void* p) {return reinterpret_cast<uintptr_t>(p);}
template<class T> void Put(uintptr_t at,const T& value) {std::memcpy(reinterpret_cast<void*>(at),&value,sizeof(value));}
template<class T=uintptr_t> T Get(uintptr_t at) {T value{};std::memcpy(&value,reinterpret_cast<void*>(at),sizeof(value));return value;}
void I32(uintptr_t at,int32_t value) {Put(at,value);}
void XYZ(uintptr_t at,float x,float y=0,float z=0) {const std::array<float,3> p{x,y,z};Put(at,p);}
struct Allocation {
    uintptr_t at{};std::size_t size{};
    explicit Allocation(std::size_t bytes):size(bytes) {
        at=Addr(VirtualAlloc(nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        if(!at)throw std::bad_alloc();
    }
    ~Allocation() {if(at)VirtualFree(reinterpret_cast<void*>(at),0,MEM_RELEASE);}
    Allocation(const Allocation&)=delete;
};
struct Image {
    Allocation allocation{0x100e000};
    uintptr_t base=allocation.at;
    std::vector<uint8_t> raw;
    std::vector<IMAGE_SECTION_HEADER> sections;
    std::vector<uint8_t> protectedBytes;
    bool Init(const char* path,const char* corrupt) {
        std::ifstream in(path,std::ios::binary);
        raw.assign(std::istreambuf_iterator<char>(in),{});
        CHECK(raw.size()==15703608 && grid2vr::gameexe::Digest(raw.data(),static_cast<ULONG>(raw.size()))==native::ImageSha256);
        CHECK(base!=native::PreferredImageBase);
        const auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
        const auto pe=reinterpret_cast<const IMAGE_NT_HEADERS32*>(raw.data()+dos->e_lfanew);
        const auto header=base+static_cast<uint32_t>(dos->e_lfanew);
        CHECK(pe->OptionalHeader.SizeOfImage==allocation.size);
        std::memcpy(reinterpret_cast<void*>(base),raw.data(),pe->OptionalHeader.SizeOfHeaders);
        const auto first=IMAGE_FIRST_SECTION(pe);
        sections.assign(first,first+pe->FileHeader.NumberOfSections);
        for(const auto& s:sections) {
            CHECK(s.PointerToRawData<=raw.size() && s.SizeOfRawData<=raw.size()-s.PointerToRawData);
            CHECK(s.VirtualAddress<allocation.size && s.SizeOfRawData<=allocation.size-s.VirtualAddress);
            std::memcpy(reinterpret_cast<void*>(base+s.VirtualAddress),raw.data()+s.PointerToRawData,s.SizeOfRawData);
        }
        const auto reloc=pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        std::vector<uint32_t> relocations;
        uintptr_t at=base+reloc.VirtualAddress,end=at+reloc.Size;
        while(at<end) {
            const auto page=Get<uint32_t>(at),size=Get<uint32_t>(at+4);
            CHECK(size>=8 && size<=end-at);
            for(uint32_t j=8;j<size;j+=2) {
                const auto entry=Get<uint16_t>(at+j);
                if(!entry)continue;
                CHECK(entry>>12==3);
                const auto target=base+page+(entry&0xfff);
                relocations.push_back(page+(entry&0xfff));
                Put(target,Get<uint32_t>(target)+static_cast<uint32_t>(base-native::PreferredImageBase));
            }
            at+=size;
        }
        const auto managerOriginal=std::find_if(sections.begin(),sections.end(),[](const auto& s){return s.VirtualAddress==0x1000;});
        CHECK(managerOriginal!=sections.end());
        const auto rawCtor=raw.data()+managerOriginal->PointerToRawData+0xf0b60-managerOriginal->VirtualAddress;
        std::vector<uint8_t> normalized(reinterpret_cast<uint8_t*>(base+0xf0b60),reinterpret_cast<uint8_t*>(base+0xf202c));
        for(const auto r:relocations)if(r>=0xf0b60 && r<0xf202c) {
            const auto pos=r-0xf0b60;uint32_t value{};std::memcpy(&value,normalized.data()+pos,4);
            value-=static_cast<uint32_t>(base-native::PreferredImageBase);std::memcpy(normalized.data()+pos,&value,4);
        }
        CHECK(std::memcmp(normalized.data(),rawCtor,normalized.size())==0);
        Put(base+0xd967fc,uintptr_t(0));Put(base+0xdef810,uintptr_t(0));
        if(!std::strcmp(corrupt,"body"))Put(base+0x1699c1,uint8_t(0x7f));
        if(!std::strcmp(corrupt,"aux"))Put(base+0x17d555,uint8_t(0x7f));
        if(!std::strcmp(corrupt,"table"))Put(base+0xcdcb68+8,uintptr_t(0));
        if(!std::strncmp(corrupt,"table-slot-",11)) {
            const auto slot=std::strtoul(corrupt+11,nullptr,10);CHECK(slot<11);
            Put(base+0xcdcb68+slot*4,uintptr_t(0));
        }
        if(!std::strcmp(corrupt,"shared-enter-corrupt"))Put(base+0x392680,uint8_t(0x90));
        if(!std::strcmp(corrupt,"shared-leave-corrupt"))Put(base+0x648910,uint8_t(0x90));
        if(!std::strcmp(corrupt,"header-rebased"))Put(header+offsetof(IMAGE_NT_HEADERS32,OptionalHeader.ImageBase),static_cast<uint32_t>(base));
        if(!std::strcmp(corrupt,"header-foreign-base"))Put(header+offsetof(IMAGE_NT_HEADERS32,OptionalHeader.ImageBase),static_cast<uint32_t>(base+0x10000));
        if(!std::strcmp(corrupt,"header-dos"))Put(base+offsetof(IMAGE_DOS_HEADER,e_magic),WORD(0));
        if(!std::strcmp(corrupt,"header-offset"))Put(base+offsetof(IMAGE_DOS_HEADER,e_lfanew),LONG(0x2000));
        if(!std::strcmp(corrupt,"header-signature"))Put(header+offsetof(IMAGE_NT_HEADERS32,Signature),DWORD(0));
        if(!std::strcmp(corrupt,"header-machine"))Put(header+offsetof(IMAGE_NT_HEADERS32,FileHeader.Machine),WORD(IMAGE_FILE_MACHINE_AMD64));
        if(!std::strcmp(corrupt,"header-magic"))Put(header+offsetof(IMAGE_NT_HEADERS32,OptionalHeader.Magic),WORD(IMAGE_NT_OPTIONAL_HDR64_MAGIC));
        if(!std::strcmp(corrupt,"header-image-size"))Put(header+offsetof(IMAGE_NT_HEADERS32,OptionalHeader.SizeOfImage),DWORD(0x100d000));
        if(!std::strcmp(corrupt,"header-directory-count"))Put(header+offsetof(IMAGE_NT_HEADERS32,OptionalHeader.NumberOfRvaAndSizes),DWORD(5));
        if(!std::strcmp(corrupt,"header-reloc-shape"))Put(header+offsetof(IMAGE_NT_HEADERS32,OptionalHeader.DataDirectory)+IMAGE_DIRECTORY_ENTRY_BASERELOC*sizeof(IMAGE_DATA_DIRECTORY),DWORD(0xf48000));
        DWORD old{};
        CHECK(VirtualProtect(reinterpret_cast<void*>(base),allocation.size,PAGE_READONLY,&old));
        // Only mutable native DATA remains RW. Code and shared vtables remain
        // read-only AND NONEXECUTABLE for the entire test process.
        CHECK(VirtualProtect(reinterpret_cast<void*>(base+0xd90000),0x100e000-0xd90000,PAGE_READWRITE,&old));
        CHECK(VirtualProtect(reinterpret_cast<void*>(base+0xf47000),0x96000,PAGE_NOACCESS,&old));
        protectedBytes.assign(reinterpret_cast<uint8_t*>(base),reinterpret_cast<uint8_t*>(base+0xd90000));
        return true;
    }
    bool Unchanged() const {
        CHECK(std::memcmp(protectedBytes.data(),reinterpret_cast<void*>(base),protectedBytes.size())==0);
        MEMORY_BASIC_INFORMATION m{};
        CHECK(VirtualQuery(reinterpret_cast<void*>(base+0x169050),&m,sizeof(m)) && m.Protect==PAGE_READONLY);
        CHECK(VirtualQuery(reinterpret_cast<void*>(base+0xcdcb68),&m,sizeof(m)) && m.Protect==PAGE_READONLY);
        return true;
    }
};
struct World {
    Allocation manager{0x14450},updater{0x5c},parent{0x300},cars{12*0x200},meshes{12*0x100},
        list{0xb0},items{12*0x40},pointers{12*4},alternates{12*0x340},view{0x60};
    uintptr_t base{};
    uintptr_t Record(unsigned i) const {return manager.at+0x1110+i*0x1810;}
    uintptr_t Car(unsigned i) const {return cars.at+i*0x200;}
    uintptr_t Alternate(unsigned i) const {return alternates.at+i*0x340;}
    void Setup(uintptr_t image) {
        base=image;
        Put(manager.at,image+0xd1222c);Put(manager.at+0x1104,manager.at+0x1110);
        I32(manager.at+0x13260,8);I32(manager.at+0x134a0,8);
        I32(manager.at+0x1346c,2);I32(manager.at+0x13350,-1);I32(manager.at+0x134d4,7);
        Put(manager.at+0x13488,image+0x2e3930);
        Put(updater.at,image+0xcdcb68);Put(updater.at+0xc,parent.at);Put(parent.at+0xcc,updater.at);
        Put(list.at+0xa8,pointers.at);Put(list.at+0xac,pointers.at+8*4);
        XYZ(view.at+0x40,0);
        for(unsigned i=0;i<8;++i) {
            Put(Record(i)+0xdc,Car(i));Put(Record(i)+0xc6,uint8_t(i==0));
            I32(Record(i)+0xfc,3);I32(Record(i)+0x100,0x12345678);
            I32(Car(i)+0x124,0);I32(Car(i)+0x120,4);I32(Car(i)+0x128,3);
            Put(Car(i)+0xe0,meshes.at+i*0x100);
            XYZ(Car(i)+0x60,10);XYZ(Alternate(i)+0x330,10);
            Put(pointers.at+i*4,items.at+i*0x40);Put(items.at+i*0x40+0x30,Alternate(i));
        }
        XYZ(Car(1)+0x60,0,0,10);XYZ(Alternate(1)+0x330,0,0,10); // front
        XYZ(Car(2)+0x60,10,0,0);XYZ(Alternate(2)+0x330,10,0,0); // side
        XYZ(Car(3)+0x60,0,0,-10);XYZ(Alternate(3)+0x330,0,0,-10); // behind
        XYZ(Car(4)+0x60,100);XYZ(Alternate(4)+0x330,100); // far
        Put(Record(5)+0xc6,uint8_t(1)); // second local always excluded
        I32(Record(6)+0xfc,5); // hidden worst+1 sentinel
    }
    void Publish() {Put(base+0xd967fc,manager.at);Put(base+0xdef810,updater.at);}
    void Selections(int32_t value=3) {for(unsigned i=0;i<8;++i)I32(Record(i)+0xfc,i==6?5:value);}
};
World* active{};
unsigned transactionCalls{},transactionWrites{},rejectTransactions{};
bool transactionBroken{},changeBeforeValidate{},sawDisabledPublication{};
bool noticeAfterCommit{},forcePublicationAfterActiveRefusal{};
unsigned activeValidatorRefusals{};
unsigned wantedChanges=2;
const char* MockTransaction(std::span<const rt::PointerChange> changes,std::span<const rt::BusyRange> busy,
    bool (*validate)(void*) noexcept,void* ctx,void (*committed)(void*) noexcept) noexcept {
    ++transactionCalls;
    if(changes.size()!=wantedChanges || !validate || !committed ||
        busy.size()!=((rt::Report().singleAttemptDiagnostic || rt::Report().controlledActivation)?32u:34u))transactionBroken=true;
    const auto report=rt::Report();
    if(report.ready)transactionBroken=true;
    sawDisabledPublication=true;
    const auto covers=[&](uintptr_t begin,uintptr_t end) {
        return std::any_of(busy.begin(),busy.end(),[&](const auto& range){return range.begin<=begin && range.end>=end;});
    };
    for(const auto& guard:native::Guards())if(!covers(active->base+guard.rva,active->base+guard.rva+guard.size))transactionBroken=true;
    for(const auto shared:{uintptr_t(0x392680),uintptr_t(0x648910)}) {
        if(rt::Report().singleAttemptDiagnostic || rt::Report().controlledActivation) {
            for(const auto& range:busy)if(range.begin<active->base+shared+0xa && active->base+shared<range.end)transactionBroken=true;
        } else if(!covers(active->base+shared,active->base+shared+0xa))transactionBroken=true;
    }
    // Each audited shared-lock call's return remains owned, including both
    // dispatchers and the reset/update paths parked inside a Windows API.
    for(const auto rva:{0x1b3794u,0x1b37fcu,0x489184u,0x4891d6u,0x3bc20u,0x3bc70u,
                       0x215f3au,0x215f57u,0x215f7du,0x216333u})
        if(!covers(active->base+rva,active->base+rva+1))transactionBroken=true;
    if(!covers(report.cache,report.cache+native::AllocationBytes) ||
       !covers(active->base+0xf0b60,active->base+0xf202c) ||
        !covers(active->base+0x6d3490,active->base+0x6d4d76))transactionBroken=true;
    // No owning-mod interval: unrelated input/XR stacks must not block attach.
    const auto module=Addr(GetModuleHandleW(nullptr));
    const auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto pe=reinterpret_cast<const IMAGE_NT_HEADERS32*>(module+dos->e_lfanew);
    if(covers(module,module+pe->OptionalHeader.SizeOfImage) || covers(Addr(&MockTransaction),Addr(&MockTransaction)+1))transactionBroken=true;
    for(const auto& change:changes) {
        if(change.slot!=reinterpret_cast<void**>(active->manager.at+0x13488) &&
           change.slot!=reinterpret_cast<void**>(active->updater.at))transactionBroken=true;
        if(*change.slot!=change.expected)transactionBroken=true;
    }
    if(rejectTransactions) {
        --rejectTransactions;
        return rt::Report().singleAttemptDiagnostic?"BusyStack":"fixture_peer_busy_full_stack";
    }
    if(changeBeforeValidate) {
        changeBeforeValidate=false;
        const auto saved=Get(active->parent.at+0xcc);Put(active->parent.at+0xcc,uintptr_t(0));
        const bool accepted=validate(ctx);Put(active->parent.at+0xcc,saved);
        if(accepted)transactionBroken=true;
        return "fixture_revalidation_refused";
    }
    const bool validated=validate(ctx);
    if(!validated) {
        if(report.activeWrappers)++activeValidatorRefusals;
        if(!forcePublicationAfterActiveRefusal)return "fixture_validator_refused";
        // Adversarial test ONLY: intentionally violate the helper's refusal to
        // test the independent TLS owner defense after already-eligible entry.
        if(!report.activeWrappers)transactionBroken=true;
        forcePublicationAfterActiveRefusal=false;
    }
    if(rt::Report().singleAttemptDiagnostic)return "diagnostic_probe_safe_no_writes";
    for(const auto& change:changes) {
        if(InterlockedCompareExchangePointer(change.slot,change.replacement,change.expected)!=change.expected) {
            transactionBroken=true;return "fixture_unexpected_compare_failure";
        }
        ++transactionWrites;
        if(rt::Report().ready)transactionBroken=true; // no publication between writes
    }
    committed(ctx);
    return noticeAfterCommit?"fixture_cleanup_notice_\"resume\"\\close\n":nullptr;
}
unsigned nativeCalls{},applyCalls{},copiedCalls{},floorCount=1,maxActiveWrappers{};
uintptr_t returnAddress{};
uint32_t viewCount=1,selectorFlag=1,lastArgument{};
bool boundaryCopied{},boundaryUpdater{},invokeFloor=true,recurse{};
bool checkFp{},fpGood=true;
bool stopInBoundary{},eyeInBoundary{},ownerInBoundary{};
World* replacement{};
uint32_t Boundary(bool copied,bool updater,void* self,uint32_t argument) noexcept {
    ++nativeCalls;copiedCalls+=copied?1:0;boundaryCopied=copied;boundaryUpdater=updater;lastArgument=argument;
    if(updater && Addr(self)!=active->updater.at)transactionBroken=true;
    const auto activeCount=rt::Report().activeWrappers;
    if(!activeCount)transactionBroken=true;
    maxActiveWrappers=std::max(maxActiveWrappers,activeCount);
    if(stopInBoundary) {stopInBoundary=false;rt::Stop();}
    if(eyeInBoundary) {eyeInBoundary=false;rt::SetEyeReplay(true);}
    if(ownerInBoundary) {
        ownerInBoundary=false;active=replacement;active->Publish();
        forcePublicationAfterActiveRefusal=true;rt::Tick(true);
    }
    if(recurse) {recurse=false;rt::testing::Callback(active->base+0x1b37d5);}
    alignas(16) unsigned char saved[512],beforeFp[512],afterFp[512];
    if(checkFp) {
        const uint32_t rounding=0x5f80;
        _fxsave(saved);
        __asm {fninit}
        __asm {fld1}
        __asm {fldpi}
        __asm {ldmxcsr rounding}
        _fxsave(beforeFp);
    }
    if(invokeFloor && copied)for(unsigned i=0;i<floorCount;++i)
        rt::testing::Floor(returnAddress,reinterpret_cast<void*>(active->manager.at),reinterpret_cast<void*>(active->view.at),
            viewCount,reinterpret_cast<void*>(active->list.at),selectorFlag);
    if(checkFp) {
        _fxsave(afterFp);_fxrstor(saved);
        if(std::memcmp(beforeFp,afterFp,32))fpGood=false;
        for(unsigned i=0;i<8;++i)if(std::memcmp(beforeFp+32+i*16,afterFp+32+i*16,10))fpGood=false;
        if(std::memcmp(beforeFp+160,afterFp+160,128))fpGood=false;
    }
    ++applyCalls; // fixture native selection boundary's one apply continuation
    return 0xfedcba98;
}
std::vector<uint8_t> Bytes(uintptr_t at,std::size_t count) {
    const auto p=reinterpret_cast<const uint8_t*>(at);return {p,p+count};
}
bool OnlySelections(const std::vector<uint8_t>& before,const World& w,std::span<const unsigned> changed) {
    const auto after=Bytes(w.manager.at,w.manager.size);
    for(std::size_t i=0;i<before.size();++i)if(before[i]!=after[i]) {
        bool allowed=false;
        for(const auto index:changed)if(i>=0x1110+index*0x1810+0xfc && i<0x1110+index*0x1810+0x100)allowed=true;
        CHECK(allowed);
    }
    return true;
}
void Advance(unsigned count=30,bool ready=true) {for(unsigned i=0;i<count;++i)rt::Tick(ready);}
bool CheckNoFloor(World& w);
std::string Text(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};
}
bool StatusCases(World& first,const std::filesystem::path& folder) {
    const auto status=folder/L"opponent-lod-runtime.json",events=folder/L"opponent-lod-runtime.jsonl";
    // Host loss must revoke the REAL runtime's cached permission immediately,
    // before any controller Tick/Present can refresh it.
    rt::RevokeWorld();
    CHECK(!rt::Report().worldReady && !rt::Report().ready && CheckNoFloor(first));
    rt::Tick(true);CHECK(rt::Report().ready);
    // A floor must become observable without Stop. Neither selectors nor ordinary
    // sub-period ticks write receipts. World waiting refreshes within 120 ticks.
    auto prior=Text(status);first.Selections();rt::testing::Callback(first.base+0x1b37d5);
    CHECK(Text(status)==prior);
    rt::Tick(false);CHECK(Text(status)==prior);
    Advance(120,false);
    auto current=Text(status);
    CHECK(current.find("\"worldReady\":false")!=std::string::npos && current.find("\"floors\":"+std::to_string(rt::Report().floors))!=std::string::npos);
    CHECK(current.find("\"activeWrappers\":0")!=std::string::npos && current.size()<8192);
    CHECK(current.find("\"policyResults\":{")!=std::string::npos && current.find("\"planned\":")!=std::string::npos &&
        current.find("\"attachmentAttempts\":")!=std::string::npos && current.find("\"restoreAttempts\":")!=std::string::npos);
    CHECK(current.find("\"floorRefusals\":{\"permission\":")!=std::string::npos &&
        current.find("\"freshStructure\":"+std::to_string(rt::Report().floorRefusals[12]))!=std::string::npos &&
        current.find("\"motionRefreshes\":"+std::to_string(rt::Report().motionRefreshes))!=std::string::npos &&
        current.find("\"retainedPasses\":")!=std::string::npos &&
        current.find("\"originalByReason\":{\"eyeReplay\":")!=std::string::npos &&
        current.find("\"transientGates\":{\"notReady\":")!=std::string::npos);
    CHECK(current.find("\"imageBase\":"+std::to_string(first.base))!=std::string::npos &&
        current.find("\"peImageBase\":"+std::to_string(native::PreferredImageBase))!=std::string::npos);
    rt::Tick(true);
    // Recreate bounded owned fixtures, retaining every prior heap until exit.
    // More than 64 attach events exercises the overwrite past the append cap.
    std::vector<World*> worlds;
    struct Cleanup {std::vector<World*>& worlds;~Cleanup(){for(auto* w:worlds)delete w;}} cleanup{worlds};
    for(unsigned i=0;i<70;++i) {
        auto* w=new World;worlds.push_back(w);w->Setup(first.base);active=w;w->Publish();rt::Tick(true);
        CHECK(rt::Report().ready && rt::Report().manager==w->manager.at);
    }
    const auto log=Text(events);
    CHECK(std::count(log.begin(),log.end(),'\n')==64 && log.size()<64*8192);
    // Missing objects and inactive world paths must also refresh periodically.
    Put(first.base+0xd967fc,uintptr_t(0));Advance(120);
    current=Text(status);CHECK(current.find("waiting_for_live_owner_manager_updater")!=std::string::npos);
    active->Publish();Advance(120,false);
    current=Text(status);CHECK(current.find("waiting_for_active_vr_world")!=std::string::npos);
    rt::SetEyeReplay(true);rt::Stop();
    current=Text(status);
    CHECK(current.find("\"stopped\":true")!=std::string::npos && current.find("\"eyeReplay\":true")!=std::string::npos &&
        current.find("\"restorations\":1")!=std::string::npos && current.find("\"manager\":0")!=std::string::npos);
    CHECK(Text(events)==log); // final receipt cannot be hidden by the 64-event cap
    rt::SetEyeReplay(false);active=&first;CHECK(!transactionBroken);return true;
}
bool EyeCases(World& w,const std::filesystem::path& folder) {
    const auto report=rt::Report();const auto n=nativeCalls,a=applyCalls,tx=transactionCalls;
    const auto status=Text(folder/L"opponent-lod-runtime.json");
    rt::SetEyeReplay(true);CHECK(rt::Report().eyeReplay);
    CHECK(CheckNoFloor(w));
    rt::testing::Update(reinterpret_cast<void*>(w.updater.at),0x12345678);
    CHECK(!boundaryCopied && Get<int32_t>(w.Record(2)+0xfc)==3);
    auto after=rt::Report();
    CHECK(after.privateRoutes==report.privateRoutes && after.floors==report.floors && after.batches==report.batches &&
        after.selectionPassesByRoute==report.selectionPassesByRoute && nativeCalls==n+2 && applyCalls==a+2 && transactionCalls==tx);
    CHECK(after.originalByReason[0]==report.originalByReason[0]+2 && after.originalRoutes==report.originalRoutes+2);
    CHECK(after.originalByReason[0]+after.originalByReason[1]+after.originalByReason[2]+after.originalByReason[3]==after.originalRoutes);
    rt::SetEyeReplay(false);w.Selections();rt::testing::Callback(w.base+0x1b37d5);
    CHECK(boundaryCopied && Get<int32_t>(w.Record(2)+0xfc)==0 && !rt::Report().eyeReplay);
    // Suppression that begins after eligibility must still refuse the floor.
    const auto floors=rt::Report().floors;eyeInBoundary=true;CHECK(CheckNoFloor(w));
    CHECK(boundaryCopied && rt::Report().eyeReplay && rt::Report().floors==floors);
    rt::SetEyeReplay(false);w.Selections();rt::testing::Callback(w.base+0x4891af);
    CHECK(Get<int32_t>(w.Record(3)+0xfc)==0 && Text(folder/L"opponent-lod-runtime.json")==status);
    CHECK(rt::Report().activeWrappers==0);return true;
}
bool CheckNoFloor(World& w) {
    w.Selections();const auto manager=Bytes(w.manager.at,w.manager.size),cars=Bytes(w.cars.at,w.cars.size);
    const auto n=nativeCalls,a=applyCalls;
    CHECK(rt::testing::Callback(w.base+0x4891af)==0xfedcba98);
    CHECK(nativeCalls==n+1 && applyCalls==a+1);
    CHECK(Bytes(w.manager.at,w.manager.size)==manager && Bytes(w.cars.at,w.cars.size)==cars);
    return true;
}
bool FloorCases(World& w,Image& image) {
    returnAddress=rt::Report().adapter+0x12;
    w.Selections();
    auto before=Bytes(w.manager.at,w.manager.size),carBefore=Bytes(w.cars.at,w.cars.size),meshBefore=Bytes(w.meshes.at,w.meshes.size);
    auto n=nativeCalls,a=applyCalls;
    CHECK(rt::testing::Callback(w.base+0x1b37d5)==0xfedcba98);
    CHECK(boundaryCopied && !boundaryUpdater && nativeCalls==n+1 && applyCalls==a+1);
    const std::array<unsigned,3> expected{1,2,3};CHECK(OnlySelections(before,w,expected));
    for(unsigned i=0;i<8;++i)CHECK(Get<int32_t>(w.Record(i)+0xfc)==(i>=1 && i<=3?0:i==6?5:3));
    CHECK(Bytes(w.cars.at,w.cars.size)==carBefore && Bytes(w.meshes.at,w.meshes.size)==meshBefore);
    // Only updater this pointer and full 32-bit arg bits determine the ABI path.
    w.Selections();n=nativeCalls;a=applyCalls;
    CHECK(rt::testing::Update(reinterpret_cast<void*>(w.updater.at),0xf185a37e)==0xfedcba98);
    CHECK(boundaryCopied && boundaryUpdater && lastArgument==0xf185a37e && nativeCalls==n+1 && applyCalls==a+1);
    CHECK(Get<int32_t>(w.Record(3)+0xfc)==0);
    // No TLS wrapper permission, even with an otherwise correct adapter return.
    w.Selections();before=Bytes(w.manager.at,w.manager.size);
    rt::testing::Floor(returnAddress,reinterpret_cast<void*>(w.manager.at),reinterpret_cast<void*>(w.view.at),1,
        reinterpret_cast<void*>(w.list.at),1);
    CHECK(Bytes(w.manager.at,w.manager.size)==before);
    CHECK(std::strcmp(rt::Report().floorReason,"no_current_owned_wrapper_permission")==0);
    ++returnAddress;CHECK(CheckNoFloor(w));--returnAddress;
    viewCount=2;CHECK(CheckNoFloor(w));CHECK(!std::strcmp(rt::Report().floorReason,"snapshot_view_count"));viewCount=1;
    for(int profile=3;profile<=6;++profile) {
        I32(w.manager.at+0x1346c,profile);CHECK(CheckNoFloor(w));CHECK(!std::strcmp(rt::Report().floorReason,"snapshot_profile"));
    }
    for(int profile=0;profile<=2;++profile) {
        I32(w.manager.at+0x1346c,profile);w.Selections();rt::testing::Callback(w.base+0x4891af);
        CHECK(Get<int32_t>(w.Record(2)+0xfc)==0);
    }
    I32(w.manager.at+0x13350,0);CHECK(CheckNoFloor(w));CHECK(!std::strcmp(rt::Report().floorReason,"snapshot_forced_selection"));I32(w.manager.at+0x13350,-1);
    I32(w.manager.at+0x134a0,7);CHECK(CheckNoFloor(w));CHECK(!std::strcmp(rt::Report().floorReason,"snapshot_list_count"));I32(w.manager.at+0x134a0,8);
    Put(w.manager.at+0x1104,w.manager.at+0x1120);CHECK(CheckNoFloor(w));Put(w.manager.at+0x1104,w.manager.at+0x1110);
    I32(w.manager.at+0x13260,13);CHECK(CheckNoFloor(w));I32(w.manager.at+0x13260,8);
    XYZ(w.view.at+0x40,std::numeric_limits<float>::infinity());CHECK(CheckNoFloor(w));XYZ(w.view.at+0x40,0);
    XYZ(w.Alternate(2)+0x330,std::numeric_limits<float>::quiet_NaN());CHECK(CheckNoFloor(w));XYZ(w.Alternate(2)+0x330,10);
    // Alternate position is authoritative when the LOW BYTE is true.
    XYZ(w.Car(1)+0x60,10);XYZ(w.Alternate(1)+0x330,100);
    w.Selections();rt::testing::Callback(w.base+0x4891af);CHECK(Get<int32_t>(w.Record(1)+0xfc)==3);
    selectorFlag=0x100;w.Selections();rt::testing::Callback(w.base+0x4891af);CHECK(Get<int32_t>(w.Record(1)+0xfc)==0);
    selectorFlag=0x12345601;w.Selections();rt::testing::Callback(w.base+0x4891af);CHECK(Get<int32_t>(w.Record(1)+0xfc)==3);
    // Native fallback uses normal position when item+30 is null, even flag=true.
    Put(w.items.at+0x40+0x30,uintptr_t(0));w.Selections();rt::testing::Callback(w.base+0x4891af);
    CHECK(Get<int32_t>(w.Record(1)+0xfc)==0);Put(w.items.at+0x40+0x30,w.Alternate(1));XYZ(w.Alternate(1)+0x330,10);
    selectorFlag=1;
    // No 40-unit carry across unproven lifetimes/batches, even reused addresses.
    w.Selections();rt::testing::Callback(w.base+0x4891af);XYZ(w.Alternate(1)+0x330,35);
    w.Selections();rt::testing::Callback(w.base+0x4891af);CHECK(Get<int32_t>(w.Record(1)+0xfc)==3);XYZ(w.Alternate(1)+0x330,10);
    // best, bounds, sentinel, null asset and local/current-target exclusions.
    I32(w.Car(1)+0x124,1);w.Selections();rt::testing::Callback(w.base+0x4891af);
    CHECK(Get<int32_t>(w.Record(1)+0xfc)==3); // best slot 1 is not loaded
    Put(w.Car(1)+0xe4,w.meshes.at);w.Selections();rt::testing::Callback(w.base+0x4891af);
    CHECK(Get<int32_t>(w.Record(1)+0xfc)==1);I32(w.Car(1)+0x124,0);
    I32(w.Car(1)+0x120,5);w.Selections();rt::testing::Callback(w.base+0x4891af);CHECK(Get<int32_t>(w.Record(1)+0xfc)==3);I32(w.Car(1)+0x120,4);
    I32(w.manager.at+0x134d4,2);w.Selections();rt::testing::Callback(w.base+0x4891af);CHECK(Get<int32_t>(w.Record(2)+0xfc)==3);
    I32(w.manager.at+0x134d4,-1);w.Selections();rt::testing::Callback(w.base+0x4891af);CHECK(Get<int32_t>(w.Record(2)+0xfc)==0);
    I32(w.manager.at+0x134d4,99);w.Selections();rt::testing::Callback(w.base+0x4891af);CHECK(Get<int32_t>(w.Record(0)+0xfc)==3);
    I32(w.manager.at+0x134d4,7);
    // Wrong callback caller falls through ORIGINAL exactly once.
    w.Selections();n=nativeCalls;a=applyCalls;before=Bytes(w.manager.at,w.manager.size);
    rt::testing::Callback(w.base+0x1b37d6);CHECK(!boundaryCopied && nativeCalls==n+1 && applyCalls==a+1);
    CHECK(Bytes(w.manager.at,w.manager.size)==before);
    // Inactive VR route is dormant, even while heap attachment remains owned.
    rt::Tick(false);CHECK(CheckNoFloor(w));rt::Tick(true);
    CHECK(image.Unchanged());return true;
}
bool FloatingPoint(World& w) {
    checkFp=true;w.Selections();rt::testing::Callback(w.base+0x1b37d5);
    CHECK(fpGood && Get<int32_t>(w.Record(1)+0xfc)==0);
    ++returnAddress;rt::testing::Callback(w.base+0x4891af);--returnAddress;
    CHECK(fpGood);checkFp=false;
    // The wrapper's real helper preserves FPU/MXCSR with a nonempty x87 stack.
    alignas(16) unsigned char saved[512],before[512],after[512];
    const uint32_t rounding=0x5f80;
    _fxsave(saved);
    __asm {fninit}
    __asm {fld1}
    __asm {fldpi}
    __asm {ldmxcsr rounding}
    _fxsave(before);
    rt::testing::Floor(returnAddress+1,reinterpret_cast<void*>(w.manager.at),reinterpret_cast<void*>(w.view.at),1,
        reinterpret_cast<void*>(w.list.at),1);
    _fxsave(after);_fxrstor(saved);
    CHECK(std::memcmp(before,after,32)==0);
    for(unsigned i=0;i<8;++i)CHECK(std::memcmp(before+32+i*16,after+32+i*16,10)==0);
    CHECK(std::memcmp(before+160,after+160,128)==0);
    return true;
}
// Another game thread moves cars/camera or changes structure between the
// floor's two captures. Motion alone must still floor from the newer positions;
// structure, selection or gate changes must refuse with their own counter.
bool DriftCases(World& w) {
    using Hook=void (*)() noexcept;
    const auto refusalAt=[](std::size_t i){return rt::Report().floorRefusals[i];};
    constexpr std::size_t FreshGate=10,FreshSelection=11,FreshStructure=12;
    const auto run=[&](Hook hook) {
        w.Selections();rt::testing::BetweenCaptures(hook);
        rt::testing::Callback(w.base+0x4891af);rt::testing::BetweenCaptures(nullptr);
    };
    const auto cars=Bytes(w.cars.at,w.cars.size),alternates=Bytes(w.alternates.at,w.alternates.size),view=Bytes(w.view.at,w.view.size);
    const auto restore=[&] {
        std::memcpy(reinterpret_cast<void*>(w.cars.at),cars.data(),cars.size());
        std::memcpy(reinterpret_cast<void*>(w.alternates.at),alternates.data(),alternates.size());
        std::memcpy(reinterpret_cast<void*>(w.view.at),view.data(),view.size());
    };
    auto start=rt::Report();
    run(+[]() noexcept {XYZ(active->Car(3)+0x60,0,0,-12);XYZ(active->Alternate(3)+0x330,0,0,-12);XYZ(active->view.at+0x40,0.25f);});
    CHECK(!std::strcmp(rt::Report().floorReason,"selected_floor_only"));
    CHECK(rt::Report().motionRefreshes==start.motionRefreshes+1 && rt::Report().floors==start.floors+3);
    for(unsigned i=0;i<8;++i)CHECK(Get<int32_t>(w.Record(i)+0xfc)==(i>=1 && i<=3?0:i==6?5:3));
    CHECK(rt::Report().floorRefusals==start.floorRefusals);
    restore();
    // The re-evaluated plan follows the newer position: a car that left the
    // radius between captures keeps its native selection.
    run(+[]() noexcept {XYZ(active->Car(2)+0x60,100);XYZ(active->Alternate(2)+0x330,100);});
    CHECK(Get<int32_t>(w.Record(1)+0xfc)==0 && Get<int32_t>(w.Record(2)+0xfc)==3 && Get<int32_t>(w.Record(3)+0xfc)==0);
    run(+[]() noexcept {for(unsigned i=1;i<4;++i) {XYZ(active->Car(i)+0x60,100);XYZ(active->Alternate(i)+0x330,100);}});
    CHECK(!std::strcmp(rt::Report().floorReason,"native_selection_retained"));
    for(unsigned i=1;i<4;++i)CHECK(Get<int32_t>(w.Record(i)+0xfc)==3);
    CHECK(rt::Report().motionRefreshes==start.motionRefreshes+3);
    restore();
    const auto refused=[&](Hook hook,std::size_t index,const char* reason) {
        const auto before=rt::Report();w.Selections();
        const auto manager=Bytes(w.manager.at,w.manager.size);
        rt::testing::BetweenCaptures(hook);rt::testing::Callback(w.base+0x4891af);rt::testing::BetweenCaptures(nullptr);
        const auto after=rt::Report();
        auto expected=Bytes(w.manager.at,w.manager.size);
        bool selectionOnlyByHook=true;
        // Only the hook's own edits (record 1 selection, car count) may differ.
        for(std::size_t i=0;i<manager.size();++i)if(manager[i]!=expected[i] && !(i>=0x1110+0x1810+0xfc && i<0x1110+0x1810+0x100) &&
            !(i>=0x13260 && i<0x13264))selectionOnlyByHook=false;
        return selectionOnlyByHook && after.floors==before.floors && after.refusals==before.refusals+1 &&
            refusalAt(index)==before.floorRefusals[index]+1 && !std::strcmp(after.floorReason,reason) &&
            Get<int32_t>(w.Record(2)+0xfc)==3 && Get<int32_t>(w.Record(3)+0xfc)==3;
    };
    CHECK(refused(+[]() noexcept {Put(active->Car(2)+0xe0,active->meshes.at+0x100);},FreshStructure,"fresh_snapshot_structure_changed"));
    Put(w.Car(2)+0xe0,w.meshes.at+2*0x100);
    CHECK(refused(+[]() noexcept {I32(active->Record(1)+0xfc,2);},FreshSelection,"fresh_native_selection_changed"));
    CHECK(Get<int32_t>(w.Record(1)+0xfc)==2);
    // An owner-graph change (here the car count) between captures refuses.
    CHECK(refused(+[]() noexcept {I32(active->manager.at+0x13260,7);},FreshGate,"fresh_owner_changed_or_stopped"));
    I32(w.manager.at+0x13260,8);
    // The host's per-Present revoke and the render thread's eye pair are
    // transient for this thread: the floor proceeds and the flips are counted.
    start=rt::Report();
    run(+[]() noexcept {
        std::thread other([] {rt::RevokeWorld();rt::SetEyeReplay(true);});other.join();
    });
    auto after=rt::Report();
    CHECK(after.floors==start.floors+3 && after.refusals==start.refusals && !std::strcmp(after.floorReason,"selected_floor_only"));
    CHECK(after.transientGates[0]==start.transientGates[0]+1 && after.transientGates[1]==start.transientGates[1]+1 &&
        after.transientGates[2]==start.transientGates[2]+1);
    // Eye replay on another thread does not block new passes on this thread.
    CHECK(after.eyeReplay);rt::Tick(true);CHECK(rt::Report().ready);
    start=rt::Report();w.Selections();rt::testing::Callback(w.base+0x4891af);
    CHECK(rt::Report().floors==start.floors+3 && rt::Report().originalRoutes==start.originalRoutes);
    rt::SetEyeReplay(false);
    start=rt::Report();w.Selections();rt::testing::Callback(w.base+0x4891af);
    CHECK(rt::Report().floors==start.floors+3 && rt::Report().motionRefreshes==start.motionRefreshes);
    return true;
}
bool FullUpdaterTable(const World& world) {
    const auto original=world.base+0xcdcb68,owned=Get(world.updater.at);
    bool valid=Get(owned-4)==Get(original-4) && Get(owned+4)!=Get(original+4);
    // The scheduler's lifecycle call uses +0x14 (slot 5), and its clock query
    // uses +0x28 (slot 10). Every one of the eleven methods must survive.
    for(unsigned slot=0;slot<11;++slot) {
        if(slot==1)continue;
        const auto expected=Get(original+slot*4),actual=Get(owned+slot*4);
        if(expected!=actual) {
            std::printf("updater vtable slot %u expected=%08x actual=%08x\n",slot,
                static_cast<unsigned>(expected),static_cast<unsigned>(actual));valid=false;
        }
    }
    CHECK(valid);return true;
}
bool Integration(Image& image,const std::filesystem::path& folder,const char* mode) {
    CHECK(rt::Prepare(image.base,folder,MockTransaction));
    CHECK(rt::Report().imageBase==image.base && rt::Report().peImageBase==
        (!std::strcmp(mode,"header-rebased")?image.base:native::PreferredImageBase));
    CHECK(rt::Report().prepared && !rt::Report().ready && rt::Report().cache);
    MEMORY_BASIC_INFORMATION cache{};
    CHECK(VirtualQuery(reinterpret_cast<void*>(rt::Report().cache),&cache,sizeof(cache)) && cache.Protect==PAGE_EXECUTE_READ);
    // Only StageCopies data is in RX; execution boundary is always substituted.
    World first;first.Setup(image.base);active=&first;rt::testing::Boundary(Boundary);
    rt::Tick(true);CHECK(transactionCalls==0 && !rt::Report().ready); // objects not published yet
    first.Publish();rt::Tick(false);CHECK(transactionCalls==0);
    rejectTransactions=1;rt::Tick(true);CHECK(transactionCalls==1 && transactionWrites==0);
    CHECK(std::strstr(rt::Report().reason,"fixture_peer_busy")!=nullptr);
    Advance(29);CHECK(transactionCalls==1);
    changeBeforeValidate=true;Advance(1);CHECK(transactionCalls==2 && transactionWrites==0);
    noticeAfterCommit=!std::strcmp(mode,"commit-notice");
    Advance();CHECK(transactionWrites==2 && transactionCalls==3 && !transactionBroken);
    CHECK(sawDisabledPublication);
    const auto originalTable=image.base+0xcdcb68;
    const auto ownedTable=Get(first.updater.at),ownedCallback=Get(first.manager.at+0x13488);
    CHECK(ownedTable>=rt::Report().cache+native::StagingBytes && ownedTable<rt::Report().cache+8192);
    CHECK(Get(ownedTable-4)==Get(originalTable-4) && Get(ownedTable)==Get(originalTable));
    CHECK(Get(ownedTable+8)==Get(originalTable+8) && Get(ownedTable+4)!=Get(originalTable+4));
    CHECK(FullUpdaterTable(first));
    if(!std::strcmp(mode,"commit-notice")) {
        const auto published=rt::Report();
        CHECK(published.attachments==1 && published.manager==first.manager.at && published.updaterObject==first.updater.at &&
            published.cache && published.cleanupNotice && published.stopped && !published.ready && !published.worldReady);
        CHECK(std::strstr(published.reason,"fixture_cleanup_notice") && Get(first.updater.at)==ownedTable);
        const auto json=Text(folder/L"opponent-lod-runtime.json");
        CHECK(json.find("\\\"resume\\\"\\\\close\\u000a")!=std::string::npos);
        noticeAfterCommit=false;rejectTransactions=1;rt::Stop();
        CHECK(rt::Report().manager==first.manager.at && Get(first.updater.at)==ownedTable && rt::Report().restorations==0);
        CHECK(CheckNoFloor(first));Advance(30,true);
        CHECK(rt::Report().restorations==1 && rt::Report().attachments==1 && rt::Report().manager==0 &&
            Get(first.updater.at)==originalTable && Get(first.manager.at+0x13488)==image.base+0x2e3930 &&
            rt::Report().cleanupNotice && rt::Report().stopped && !rt::Report().worldReady && rt::Report().cache==published.cache);
        const auto calls=transactionCalls;Advance(240);rt::Stop();CHECK(transactionCalls==calls && !rt::Report().ready);
        return image.Unchanged();
    }
    CHECK(rt::Report().ready);
    CHECK(FloorCases(first,image));CHECK(FloatingPoint(first));CHECK(DriftCases(first));
    const auto passes=rt::Report();
    CHECK(passes.privateByRoute[0]>0 && passes.privateByRoute[1]>0 && passes.privateByRoute[2]==1);
    CHECK(passes.selectionPassesByRoute[0]>0 && passes.selectionPassesByRoute[1]>0 && passes.selectionPassesByRoute[2]==1);
    CHECK(passes.privateByRoute[0]+passes.privateByRoute[1]+passes.privateByRoute[2]==passes.privateRoutes);
    if(!std::strcmp(mode,"eye-replay")) {CHECK(EyeCases(first,folder));rt::Stop();return image.Unchanged();}
    if(!std::strcmp(mode,"status")) {CHECK(StatusCases(first,folder));return image.Unchanged();}
    if(!std::strcmp(mode,"active-wrapper")) {
        const auto nestedCalls=nativeCalls,nestedCopies=copiedCalls;
        recurse=true;first.Selections();rt::testing::Callback(first.base+0x1b37d5);
        CHECK(nativeCalls==nestedCalls+2 && copiedCalls==nestedCopies+1 && maxActiveWrappers==2 &&
            rt::Report().activeWrappers==0 && Get<int32_t>(first.Record(3)+0xfc)==0);
        const auto calls=nativeCalls;stopInBoundary=true;first.Selections();
        rt::testing::Callback(first.base+0x1b37d5);
        CHECK(activeValidatorRefusals==1 && rt::Report().activeWrappers==0 && rt::Report().manager==first.manager.at &&
            rt::Report().restorations==0 && nativeCalls==calls+1 && Get(first.updater.at)==ownedTable);
        Advance(30,false);CHECK(rt::Report().restorations==1 && rt::Report().manager==0 && !transactionBroken);
        return image.Unchanged();
    }
    if(!std::strcmp(mode,"owner-tls")) {
        World second;second.Setup(image.base);replacement=&second;ownerInBoundary=true;
        // Old updater is already eligible, then the mock deliberately publishes
        // a new manager/updater despite active-wrapper refusal. Its TLS may not
        // borrow new ownership, even though the new graph passes every live guard.
        const auto calls=nativeCalls;rt::testing::Update(reinterpret_cast<void*>(first.updater.at),0xa5a55a5a);
        CHECK(activeValidatorRefusals==1 && rt::Report().attachments==2 && rt::Report().ready &&
            std::strcmp(rt::Report().floorReason,"wrapper_entry_owner_graph_changed")==0 &&
            Get<int32_t>(second.Record(3)+0xfc)==3 && nativeCalls==calls+1 && rt::Report().activeWrappers==0);
        second.Selections();rt::testing::Callback(image.base+0x1b37d5);
        CHECK(Get<int32_t>(second.Record(3)+0xfc)==0);
        rt::Stop();CHECK(rt::Report().restorations==1 && !transactionBroken);return image.Unchanged();
    }
    if(!std::strcmp(mode,"no-owned-stop")) {
        // "No owned fields" cannot be inferred through an invalid, foreign or
        // replaced graph. Retain the old ownership ledger in all false cases.
        const uintptr_t foreign=0x12345678;const auto tx=transactionCalls;
        Put(first.manager.at+0x13488,foreign);Put(first.updater.at,foreign);rt::Stop();
        CHECK(rt::Report().manager==first.manager.at && transactionCalls==tx);
        Put(first.updater.at,originalTable);Put(first.parent.at+0xcc,uintptr_t(0));rt::Stop();
        CHECK(rt::Report().manager==first.manager.at && transactionCalls==tx);
        Put(first.parent.at+0xcc,first.updater.at);
        World other;other.Setup(image.base);active=&other;other.Publish();rt::Stop();
        CHECK(rt::Report().manager==first.manager.at && transactionCalls==tx && Get(other.manager.at+0x13488)==image.base+0x2e3930);
        active=&first;first.Publish();rt::Stop();
        CHECK(transactionCalls==tx && rt::Report().manager==0 && rt::Report().restorations==0);
        const auto reason=std::string(rt::Report().reason);
        Put(image.base+0xd967fc,uintptr_t(0));Advance(240,false);rt::Stop();
        CHECK(transactionCalls==tx && reason==rt::Report().reason && !transactionBroken);return image.Unchanged();
    }
    // Foreign replacement is never overwritten. Original invocation remains once.
    const uintptr_t foreign=0x12345678;Put(first.manager.at+0x13488,foreign);
    rt::Tick(true);CHECK(!rt::Report().ready && Get(first.manager.at+0x13488)==foreign);
    CHECK(CheckNoFloor(first));Put(first.manager.at+0x13488,ownedCallback);rt::Tick(true);CHECK(rt::Report().ready);
    Put(first.parent.at+0xcc,uintptr_t(0));rt::Tick(true);CHECK(!rt::Report().ready);CHECK(CheckNoFloor(first));
    Put(first.parent.at+0xcc,first.updater.at);rt::Tick(true);CHECK(rt::Report().ready);
    if(!std::strcmp(mode,"bounded")) {
        World second;second.Setup(image.base);active=&second;second.Publish();rejectTransactions=1000;
        Advance(2000);const auto count=transactionCalls;Advance(2000);
        CHECK(transactionCalls==count && !rt::Report().ready && std::strstr(rt::Report().reason,"exhausted"));
        rt::Stop();CHECK(image.Unchanged());return true;
    }
    // New owner graph is expected/native only. Old still-owned pointers are not
    // restored through stale globals, and old thunks cannot obtain floor permission.
    World second;second.Setup(image.base);active=&second;second.Publish();rt::Tick(true);
    CHECK(rt::Report().ready && rt::Report().attachments==2 && transactionWrites==4);
    CHECK(Get(first.manager.at+0x13488)==ownedCallback && Get(first.updater.at)==ownedTable);
    const auto originalCount=nativeCalls;
    second.Selections();rt::testing::Callback(image.base+0x1b37d5,reinterpret_cast<void*>(first.manager.at));
    CHECK(!boundaryCopied && nativeCalls==originalCount+1 && Get<int32_t>(second.Record(3)+0xfc)==3);
    second.Selections();returnAddress=rt::Report().adapter+0x12;rt::testing::Callback(image.base+0x1b37d5);
    CHECK(Get<int32_t>(second.Record(3)+0xfc)==0);
    if(!std::strcmp(mode,"foreign-stop")) {
        Put(second.manager.at+0x13488,foreign);wantedChanges=1;
        rt::Stop();CHECK(Get(second.manager.at+0x13488)==foreign && Get(second.updater.at)==originalTable);
        CHECK(rt::Report().restorations==1 && !rt::Report().ready);return image.Unchanged();
    }
    if(!std::strcmp(mode,"freed-stop")) {
        Put(image.base+0xd967fc,uintptr_t(0));rt::Stop();
        CHECK(Get(second.updater.at)!=originalTable && rt::Report().restorations==0);return image.Unchanged();
    }
    if(!std::strcmp(mode,"restore-notice")) {
        const auto allocation=rt::Report().cache;noticeAfterCommit=true;rt::Stop();
        CHECK(rt::Report().restorations==1 && rt::Report().manager==0 && rt::Report().cleanupNotice &&
            rt::Report().stopped && !rt::Report().ready && rt::Report().cache==allocation && Get(second.updater.at)==originalTable);
        const auto calls=transactionCalls;const auto reason=std::string(rt::Report().reason);
        Put(image.base+0xd967fc,uintptr_t(0));Advance(240,true);rt::Stop();
        CHECK(transactionCalls==calls && reason==rt::Report().reason && !transactionBroken);return image.Unchanged();
    }
    rejectTransactions=1;rt::Stop();CHECK(!rt::Report().ready && rt::Report().stopped && rt::Report().restorations==0);
    CHECK(CheckNoFloor(second));Advance(30,false);
    CHECK(rt::Report().restorations==1 && transactionWrites==6 && Get(second.updater.at)==originalTable &&
        Get(second.manager.at+0x13488)==image.base+0x2e3930);
    const auto writes=transactionWrites,calls=transactionCalls;const auto reason=std::string(rt::Report().reason);
    Put(image.base+0xd967fc,uintptr_t(0));Advance(240,false);rt::Stop();
    CHECK(transactionWrites==writes && transactionCalls==calls && !rt::Report().ready && reason==rt::Report().reason);
    CHECK(CheckNoFloor(second));CHECK(!transactionBroken && image.Unchanged());return true;
}
#if defined(GRID2VR_OPPONENT_LOD_SINGLE_ATTEMPT_DIAGNOSTIC) || defined(GRID2VR_OPPONENT_LOD_CONTROLLED_ACTIVATION)
struct ScopeMarkers {volatile LONG ready{},stop{};uintptr_t shared{},owned{};};
DWORD WINAPI ScopeWorker(void* pointer) {
    auto& state=*static_cast<ScopeMarkers*>(pointer);
    volatile uintptr_t markers[2]{state.shared,state.owned};
    InterlockedExchange(&state.ready,1);
    while(!state.stop)Sleep(1);
    return static_cast<DWORD>(markers[0]^markers[1]);
}
struct ScopeThread {
    ScopeMarkers state;HANDLE handle{};
    bool Start(uintptr_t shared,uintptr_t owned=0) {
        state.shared=shared;state.owned=owned;
        handle=CreateThread(nullptr,0,ScopeWorker,&state,0,nullptr);
        if(handle)for(unsigned i=0;i<5000 && !state.ready;++i)Sleep(1);
        return handle && state.ready;
    }
    void Stop() {
        if(handle){InterlockedExchange(&state.stop,1);WaitForSingleObject(handle,10000);CloseHandle(handle);handle=nullptr;}
    }
    ~ScopeThread(){Stop();}
};
const char* RealTransaction(std::span<const rt::PointerChange> changes,std::span<const rt::BusyRange> ranges,
    bool (*validate)(void*) noexcept,void* context,void (*committed)(void*) noexcept) noexcept {
    ++transactionCalls;
    return grid2vr::opponentlod::bridge::Transaction(changes,ranges,validate,context,committed);
}
#ifdef GRID2VR_OPPONENT_LOD_SINGLE_ATTEMPT_DIAGNOSTIC
bool ProbeScopeCases(Image& image,const std::filesystem::path& folder,const char* mode) {
    namespace bridge=grid2vr::opponentlod::bridge;
    const bool target=!std::strcmp(mode,"probe-scope-target");
    bridge::Configure(folder);CHECK(rt::Prepare(image.base,folder,RealTransaction));
    World first;first.Setup(image.base);first.Publish();
    struct Worker {
        ScopeMarkers state;HANDLE handle{};
        ~Worker(){if(handle){InterlockedExchange(&state.stop,1);WaitForSingleObject(handle,10000);CloseHandle(handle);}}
    } worker;
    worker.state.shared=image.base+0x648919;
    worker.state.owned=target?image.base+0x1b37fc:0;
    worker.handle=CreateThread(nullptr,0,ScopeWorker,&worker.state,0,nullptr);CHECK(worker.handle);
    for(unsigned i=0;i<5000 && !worker.state.ready;++i)Sleep(1);
    CHECK(worker.state.ready);rt::Tick(true);
    const auto result=bridge::LastTransaction();
    CHECK(transactionCalls==1 && rt::DiagnosticComplete() && !result.committed && !result.writtenMask);
    CHECK(!result.resumeFailures && !result.closeFailures && rt::Report().integrityBodyCount==33 && rt::Report().busyRangeCount==32);
    if(target) {
        CHECK(result.failure==grid2vr::quiescence::Failure::BusyStack && !result.probePassed);
        CHECK(result.threadId==GetThreadId(worker.handle) && result.busyValue==image.base+0x1b37fc);
        CHECK(result.busyRangeBegin==image.base+0x1b3770 && result.busyRangeEnd==image.base+0x1b3802);
    } else {
        CHECK(result.failure==grid2vr::quiescence::Failure::None && result.probePassed);
        CHECK(!std::strcmp(rt::Report().reason,"diagnostic_probe_safe_no_writes"));
    }
    CHECK(Get(first.manager.at+0x13488)==image.base+0x2e3930 && Get(first.updater.at)==image.base+0xcdcb68);
    Advance(3000);rt::Stop();Advance(180,false);
    CHECK(transactionCalls==1 && rt::Report().attachments==0 && rt::Report().floors==0 && image.Unchanged());
    return true;
}
#endif
#endif
bool ProbeCases(Image& image,const std::filesystem::path& folder,const char* mode) {
#ifdef GRID2VR_OPPONENT_LOD_SINGLE_ATTEMPT_DIAGNOSTIC
    if(!std::strncmp(mode,"probe-scope-",12))return ProbeScopeCases(image,folder,mode);
#endif
    CHECK(rt::Prepare(image.base,folder,MockTransaction));
    CHECK(rt::Report().singleAttemptDiagnostic && !rt::DiagnosticComplete());
    World first;first.Setup(image.base);active=&first;rt::testing::Boundary(Boundary);
    Advance(180);CHECK(!transactionCalls); // Missing owner graph cannot consume a pause attempt.
    first.Publish();Advance(180,false);CHECK(!transactionCalls && !rt::DiagnosticComplete());
    if(!std::strcmp(mode,"probe-foreign")) {
        Put(first.manager.at+0x13488,uintptr_t(0x12345678));Advance(180);
        CHECK(!transactionCalls && !rt::DiagnosticComplete() && Get(first.manager.at+0x13488)==0x12345678);
        Put(first.manager.at+0x13488,image.base+0x2e3930);
    }
    if(!std::strcmp(mode,"probe-invalid")) {
        Put(first.parent.at+0xcc,uintptr_t(0));Advance(180);
        CHECK(!transactionCalls && !rt::DiagnosticComplete());Put(first.parent.at+0xcc,first.updater.at);
    }
    rejectTransactions=!std::strcmp(mode,"probe-refused")?1u:0u;
    Advance(30);CHECK(transactionCalls==1 && transactionWrites==0 && rt::DiagnosticComplete());
    CHECK(!rt::Report().ready && !rt::Report().worldReady && rt::Report().attachments==0 && rt::Report().attempts==1);
    const auto reason=std::string(rt::Report().reason);
    CHECK(reason==(!std::strcmp(mode,"probe-refused")?"BusyStack":"diagnostic_probe_safe_no_writes"));
    CHECK(rt::Report().refusals==(!std::strcmp(mode,"probe-refused")?1u:0u));
    CHECK(Get(first.manager.at+0x13488)==image.base+0x2e3930 && Get(first.updater.at)==image.base+0xcdcb68);
    CHECK(CheckNoFloor(first));Advance(3000);rt::RevokeWorld();Advance(180,false);Advance(180);
    World second;second.Setup(image.base);active=&second;second.Publish();Advance(3000);
    CHECK(rt::Prepare(image.base,folder,MockTransaction));Advance(180);
    CHECK(transactionCalls==1 && !transactionWrites && rt::Report().reason==reason && !transactionBroken);
    CHECK(CheckNoFloor(second));rt::Stop();Advance(180,false);
    CHECK(transactionCalls==1 && !transactionWrites && rt::Report().stopped && !rt::Report().floors);
    const auto status=Text(folder/L"opponent-lod-runtime.json");
    CHECK(status.find("\"singleAttemptDiagnostic\":true")!=std::string::npos &&
        status.find("\"diagnosticComplete\":true")!=std::string::npos &&
        status.find("\"attempts\":1")!=std::string::npos && rt::Report().reason==reason);
    return image.Unchanged();
}
#ifdef GRID2VR_OPPONENT_LOD_CONTROLLED_ACTIVATION
bool ActivationCases(Image& image,const std::filesystem::path& folder,const char* mode) {
    namespace bridge=grid2vr::opponentlod::bridge;
    const bool refused=!std::strcmp(mode,"activation-refused");
    const bool notice=!std::strcmp(mode,"activation-commit-notice");
    bridge::Configure(folder);CHECK(rt::Prepare(image.base,folder,notice?MockTransaction:RealTransaction));
    CHECK(rt::Report().controlledActivation && !rt::Report().singleAttemptDiagnostic && rt::WorldChecksNeeded());
    CHECK(rt::Report().integrityBodyCount==33 && rt::Report().busyRangeCount==32);
    World first;first.Setup(image.base);active=&first;rt::testing::Boundary(Boundary);
    first.Publish();Advance(120,false);CHECK(!transactionCalls && !rt::Report().attachmentAttempted);
    if(!std::strcmp(mode,"activation-foreign-before")) {
        Put(first.manager.at+0x13488,uintptr_t(0x12345678));Advance(120);
        CHECK(!transactionCalls && !rt::Report().attachmentAttempted);
        CHECK(Get(first.manager.at+0x13488)==0x12345678);Put(first.manager.at+0x13488,image.base+0x2e3930);
    }
    ScopeThread shared;CHECK(shared.Start(image.base+0x648919,refused?image.base+0x1b37fc:0));
    noticeAfterCommit=notice;Advance(30);
    CHECK(transactionCalls==1 && rt::Report().attachmentAttempts==1 && rt::Report().attachmentAttempted);
    CHECK(!rt::DiagnosticComplete());
    if(refused) {
        CHECK(bridge::LastTransaction().failure==grid2vr::quiescence::Failure::BusyStack);
        CHECK(!bridge::LastTransaction().committed && !bridge::LastTransaction().writtenMask);
        CHECK(rt::Report().controlledDormant && !rt::WorldChecksNeeded() && !rt::Report().ready && !rt::Report().worldReady);
        shared.Stop();Advance(3000);rt::RevokeWorld();Advance(180);
        World second;second.Setup(image.base);active=&second;second.Publish();Advance(3000);
        CHECK(rt::Prepare(image.base,folder,RealTransaction));rt::Stop();Advance(180,false);
        CHECK(transactionCalls==1 && rt::Report().attachments==0 && rt::Report().restoreAttempts==0 && !rt::Report().floors);
        CHECK(Get(first.manager.at+0x13488)==image.base+0x2e3930 && Get(first.updater.at)==image.base+0xcdcb68);
        return image.Unchanged();
    }
    CHECK(rt::Report().attachments==1 && rt::Report().manager==first.manager.at);
    CHECK(FullUpdaterTable(first));
    CHECK(Get(first.manager.at+0x13488)!=image.base+0x2e3930 && Get(first.updater.at)!=image.base+0xcdcb68);
    if(notice) {
        CHECK(rt::Report().controlledDormant && rt::Report().cleanupNotice && rt::Report().stopped && !rt::Report().ready);
        CHECK(CheckNoFloor(first));noticeAfterCommit=false;rt::Stop();
        CHECK(rt::Report().restorations==1 && rt::Report().attachmentAttempts==1 && rt::Report().restoreAttempts==1);
        CHECK(Get(first.manager.at+0x13488)==image.base+0x2e3930 && Get(first.updater.at)==image.base+0xcdcb68);
        CHECK(!transactionBroken);return image.Unchanged();
    }
    CHECK(bridge::LastTransaction().committed && bridge::LastTransaction().writtenMask==3 && !bridge::LastTransaction().probePassed);
    CHECK(rt::Report().ready && rt::WorldChecksNeeded() && !rt::Report().controlledDormant);
    CHECK(FloorCases(first,image));CHECK(FloatingPoint(first));CHECK(DriftCases(first));CHECK(EyeCases(first,folder));
    CHECK(rt::Report().floors>0 && rt::Report().policyInspected>0 && rt::Report().policyNearby>0 && rt::Report().policyResults[0]>0);
    CHECK(rt::Report().policyResults[2]>0 && rt::Report().policyResults[3]>0);
    if(!std::strcmp(mode,"activation-owner-loss")) {
        World second;second.Setup(image.base);active=&second;second.Publish();rt::Tick(true);
        CHECK(rt::Report().controlledDormant && !rt::Report().ready && !rt::WorldChecksNeeded());
        CHECK(CheckNoFloor(second));Advance(3000);rt::Stop();
        CHECK(transactionCalls==1 && rt::Report().attachmentAttempts==1 && !rt::Report().restoreAttempts);
        CHECK(Get(second.manager.at+0x13488)==image.base+0x2e3930 && Get(second.updater.at)==image.base+0xcdcb68);
        return image.Unchanged();
    }
    const bool foreign=!std::strcmp(mode,"activation-foreign-after");
    if(foreign) {
        Put(first.manager.at+0x13488,uintptr_t(0x12345678));rt::Tick(true);
        CHECK(rt::Report().controlledDormant && !rt::WorldChecksNeeded() && CheckNoFloor(first));
    }
    if(!std::strcmp(mode,"activation-restore-retry")) {
        shared.Stop();ScopeThread blocking;CHECK(blocking.Start(image.base+0x648919,image.base+0x1b37fc));
        rt::Stop();CHECK(rt::Report().stopped && !rt::Report().ready && rt::Report().restorations==0);
        CHECK(bridge::LastTransaction().failure==grid2vr::quiescence::Failure::BusyStack && CheckNoFloor(first));
        blocking.Stop();Advance(30,false);
        CHECK(rt::Report().restorations==1 && rt::Report().restoreAttempts==2);
    } else {
        rt::Stop();CHECK(rt::Report().restorations==1 && rt::Report().restoreAttempts==1);
    }
    CHECK(Get(first.manager.at+0x13488)==(foreign?uintptr_t(0x12345678):image.base+0x2e3930));
    CHECK(Get(first.updater.at)==image.base+0xcdcb68 && rt::Report().manager==0 && !rt::WorldChecksNeeded());
    const auto calls=transactionCalls;Advance(3000);rt::Stop();
    CHECK(transactionCalls==calls && rt::Report().attachmentAttempts==1 && !transactionBroken);
    const auto status=Text(folder/L"opponent-lod-runtime.json");
    CHECK(status.find("\"controlledActivation\":true")!=std::string::npos &&
        status.find("\"attachmentAttempts\":1")!=std::string::npos &&
        status.find("\"policyResults\":{")!=std::string::npos);
    return image.Unchanged();
}
#endif
}
int main(int argc,char** argv) {
    const char* path="E:\\SteamLibrary\\steamapps\\common\\grid 2\\grid2_avx.exe";
    const char* mode=argc>1?argv[1]:"normal";
    if(argc>2)path=argv[2];
    try {
        const auto folder=std::filesystem::current_path()/(L"fixture-requests-"+std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(folder);
        // Desktop check does not latch attempted preparation.
        if(!rt::Prepare(0,folder,nullptr) || rt::Report().requested || rt::Report().prepared)return 1;
        if(!std::strcmp(mode,"status")) {
            std::locale::global(std::locale(std::locale::classic(),new HostPunctuation));
            std::ofstream stale(folder/L"opponent-lod-runtime.jsonl");stale<<"previous process event\n";
        }
        for(const auto* name:{L"runtime-session.request",L"xr-session.request",L"xr-projection.request"}) {
            std::ofstream request(folder/name);request<<"fixture-owned request\n";
        }
        Image image;bool ok=image.Init(path,mode);
        const bool badHeader=!std::strncmp(mode,"header-",7) && std::strcmp(mode,"header-rebased");
        const bool sharedCorrupt=!std::strcmp(mode,"shared-enter-corrupt") || !std::strcmp(mode,"shared-leave-corrupt");
        const bool tableTailCorrupt=!std::strncmp(mode,"table-slot-",11);
        if(ok && (!std::strcmp(mode,"body") || !std::strcmp(mode,"aux") || !std::strcmp(mode,"table") || badHeader || sharedCorrupt || tableTailCorrupt)) {
            ok=!rt::Prepare(image.base,folder,MockTransaction) && !rt::Report().prepared && transactionWrites==0 && image.Unchanged();
            if(ok && tableTailCorrupt)ok=!std::strcmp(rt::Report().reason,"native_updater_table_guard") && !rt::Report().cache;
            if(ok && sharedCorrupt)ok=!std::strcmp(rt::Report().reason,!std::strcmp(mode,"shared-enter-corrupt")?
                "auxiliary_body_hash_00392680":"auxiliary_body_hash_00648910");
            if(ok && badHeader) {
                const std::pair<const char*,const char*> reasons[]{
                    {"header-foreign-base","pe_image_base_guard"},{"header-dos","pe_dos_magic_guard"},
                    {"header-offset","pe_nt_offset_guard"},{"header-signature","pe_nt_signature_guard"},
                    {"header-machine","pe_machine_guard"},{"header-magic","pe_optional_magic_guard"},
                    {"header-image-size","pe_image_size_guard"},{"header-directory-count","pe_directory_count_guard"},
                    {"header-reloc-shape","pe_relocation_directory_shape"}};
                bool matched{};
                for(const auto& [name,reason]:reasons)if(!std::strcmp(mode,name))matched=!std::strcmp(rt::Report().reason,reason);
                ok=matched && rt::Report().cache==0 && rt::Report().attachments==0 && rt::Report().floors==0;
            }
        } else if(ok) {
#ifdef GRID2VR_OPPONENT_LOD_CONTROLLED_ACTIVATION
            if(!std::strncmp(mode,"activation-",11))ok=ActivationCases(image,folder,mode);
            else
#endif
            ok=!std::strncmp(mode,"probe-",6)?ProbeCases(image,folder,mode):Integration(image,folder,mode);
        }
        for(const auto* name:{L"runtime-session.request",L"xr-session.request",L"xr-projection.request"})std::filesystem::remove(folder/name);
        std::filesystem::remove(folder/L"opponent-lod-runtime.jsonl");
        std::filesystem::remove(folder/L"opponent-lod-runtime.json");
        std::filesystem::remove(folder/L"opponent-lod-transaction.json");
        std::filesystem::remove(folder/L"opponent-lod-transaction.jsonl");
        std::filesystem::remove(folder);
        std::printf("runtime fixture mode=%s: %s; native=%u apply=%u private=%u transactions=%u heapWrites=%u floors=%u; ZERO game instructions executed\n",
            mode,ok?"PASS":"FAIL",nativeCalls,applyCalls,copiedCalls,transactionCalls,transactionWrites,rt::Report().floors);
        return ok?0:1;
    } catch(const std::exception& e) {std::printf("fixture exception: %s\n",e.what());return 1;}
}
