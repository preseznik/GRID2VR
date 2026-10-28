#include "frontend_video_resize.h"
#include "driving_controls_instance.h"
#include "scene_resolution.h"
#include <atomic>
#include <fstream>
#include <mutex>

namespace grid2vr::frontendvideo {
namespace {
using Callback=int (__thiscall*)(void*,void*);
constexpr uintptr_t handlerRva=0x3ff4a0,eventTableRva=0xcdca04,eventKeyRva=0xbf8914;
uintptr_t base{};
std::atomic<uintptr_t> record{},receiver{};
std::atomic<DWORD> ownerThread{};
std::atomic<bool> installed{},stopped{};
std::atomic<unsigned> callbacks{},repairs{},restores{},refused{},recordWrites{};
std::atomic<DWORD> lastCallbackThread{};
std::filesystem::path output;
std::mutex configuration,reportMutex;
std::atomic<const char*> status{"not_requested"};
thread_local unsigned depth{};
void Report();
#ifdef GRID2VR_FRONTEND_VIDEO_TEST
Callback testCallback{};
#endif
template<class T> bool Read(uintptr_t at,T& value) {
    return at>=0x10000 && ReadMemory(reinterpret_cast<void*>(at),&value,sizeof(value));
}
bool Heap(uintptr_t at,size_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    return at>=0x10000 && bytes && at<=UINTPTR_MAX-bytes &&
        VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info)) && info.State==MEM_COMMIT &&
        info.Type==MEM_PRIVATE && info.Protect==PAGE_READWRITE &&
        at+bytes<=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
}
bool EventGuards(uintptr_t image) {
    constexpr uint8_t handler[]{0x55,0x8b,0xec,0x8b,0x89,0xac,0x2c,0,0,0x85,0xc9,0x74,0x19,
        0x8b,0x45,0x08,0x8b,0x50,0x14,0x52,0x0f,0xb6,0x50,0x10,0x52,0x8b,0x50,0x0c,
        0x8b,0x40,0x08,0x52,0x50,0xe8,0x4a,0x2b,0xd3,0xff,0x33,0xc0,0x5d,0xc2,0x04,0};
    constexpr uint8_t dispatch[]{0x8b,0x40,0x0c,0x8b,0x10,0x8b,0x48,0x04,0x56,0xff,0xd2,0x89,0x45,0xfc};
    constexpr uint8_t allocation[]{0x6a,0x04,0x6a,0x08,0xff,0xd2,0x85,0xc0,0x74,0x34,
        0x8b,0x4d,0x0c,0x8b,0x55,0x08,0x89,0x08,0x50,0x8d,0x4d,0xe8,0x89,0x50,0x04};
    uint8_t registration[]{0x8b,0x8e,0xa4,0x13,0,0,0x68,0,0,0,0,0x56,0xe8,0xce,0x19,0xd0,0xff};
    uint8_t getter[]{0xb8,0,0,0,0,0xc3};
    uint8_t destruction[]{0xc7,0x06,0,0,0,0,0xc7,0x46,0x3c,0,0,0,0,0xc6,0x46,0x48,0};
    const uint32_t native=static_cast<uint32_t>(image+handlerRva),key=static_cast<uint32_t>(image+eventKeyRva);
    const uint32_t registryTable=static_cast<uint32_t>(image+0xcffb68),eventReceiverTable=static_cast<uint32_t>(image+0xc5c134);
    memcpy(registration+7,&native,4);memcpy(getter+1,&key,4);
    memcpy(destruction+2,&registryTable,4);memcpy(destruction+9,&eventReceiverTable,4);
    uintptr_t typeGetter{};
    return InstructionsMatch(image) && MatchesInstructions(reinterpret_cast<void*>(image+handlerRva),handler) &&
        MatchesInstructions(reinterpret_cast<void*>(image+0x206aa6),dispatch) &&
        MatchesInstructions(reinterpret_cast<void*>(image+0x15a50c),allocation) &&
        MatchesInstructions(reinterpret_cast<void*>(image+0x458b01),registration) &&
        MatchesInstructions(reinterpret_cast<void*>(image+0x5ee740),getter) &&
        MatchesInstructions(reinterpret_cast<void*>(image+0x1e6d75),destruction) &&
        Read(image+eventTableRva+4,typeGetter) && typeGetter==image+0x5ee740;
}
// Follow the registry's circular list, not an unbounded memory search. The
// callback must belong to this exact receiver and this one native event key.
bool Registered(uintptr_t candidate,uintptr_t expected) {
    std::array<uintptr_t,2> callback{};uintptr_t registry{};
    if(candidate%4 || !Heap(candidate,8) || !Read(candidate,callback) || callback[0]!=expected ||
       !Heap(callback[1],0x2cb0) || !Read(callback[1]+0x13a4,registry) || !Heap(registry,0x1b4))return false;
    uint8_t active{};std::array<uintptr_t,2> head{};uintptr_t vptr{};
    if(!Read(registry,vptr) || vptr!=base+0xcffb68 || !Read(registry+0x48,active) || active!=1 || !Read(registry+0x180,head))return false;
    const auto sentinel=registry+0x180;auto node=head[1],previous=sentinel;unsigned found{};
    for(unsigned i=0;i<512;++i) {
        if(node==sentinel)return previous==head[0] && found==1;
        std::array<uintptr_t,4> item{};
        if(!Heap(node,sizeof(item)) || !Read(node,item) || item[0]!=previous || !item[1])return false;
        if(item[2]==base+eventKeyRva) {
            if(item[3]!=candidate)return false;
            ++found;
        }
        previous=node;node=item[1];
    }
    return false;
}
struct CameraRoute {uintptr_t manager{},camera{},target{};};
bool Route(uintptr_t self,CameraRoute& route) {
    uintptr_t target{};
    return Heap(self,0x2cb0) && Read(self+0x2cac,route.manager) && Heap(route.manager,0xfb8) &&
        Read(route.manager+0xfb4,route.camera) && Heap(route.camera,0x420) &&
        Read(route.manager+0x70,route.target) && Heap(route.target,0x90) &&
        Read(route.camera+0x94,target) && target==route.target;
}
bool Same(const CameraRoute& a,const CameraRoute& b) {
    return a.manager==b.manager && a.camera==b.camera && a.target==b.target;
}
struct CameraRepair {
    uintptr_t self{};CameraRoute route{};std::array<unsigned,2> actual{},canonical{};bool armed{};
    void Begin(uintptr_t object) {
        self=object;
        if(!Route(self,route) || !Read(route.camera+0x408,actual))return;
        canonical=actual;
        if(!ResetArguments(base,base+resetReturnRva,reinterpret_cast<void*>(route.camera),canonical[0],canonical[1],true) || canonical==actual)return;
        CameraRoute again{};std::array<unsigned,2> values{};
        if(!Route(self,again) || !Same(route,again) || !Read(route.camera+0x408,values) || values!=actual)return;
        memcpy(reinterpret_cast<void*>(route.camera+0x408),canonical.data(),sizeof(canonical));armed=true;++repairs;
    }
    ~CameraRepair() {
        if(!armed)return;
        CameraRoute now{};std::array<unsigned,2> values{},targetSize{};
        // A successful native resize can itself produce the canonical size.
        // Restore only a skipped reset: target identity AND old target size
        // must remain unchanged. Native resource or camera edits always win.
        if(Route(self,now) && Same(route,now) && Read(route.camera+0x408,values) && values==canonical &&
           Read(route.target+0x3c,targetSize) && targetSize==actual) {
            memcpy(reinterpret_cast<void*>(route.camera+0x408),actual.data(),sizeof(actual));++restores;
        }
    }
};
int __fastcall ResizeEvent(void* self,void*,void* event) {
    ++callbacks;
    lastCallbackThread=GetCurrentThreadId();
    struct Depth {Depth(){++depth;}~Depth(){--depth;}} nesting;
    auto original=reinterpret_cast<Callback>(base+handlerRva);
#ifdef GRID2VR_FRONTEND_VIDEO_TEST
    if(testCallback)original=testCallback;
#endif
    int result{};
    {
        CameraRepair repair;
        uintptr_t eventType{};
        if(installed.load() && !stopped.load() && depth==1 && GetCurrentThreadId()==ownerThread &&
           reinterpret_cast<uintptr_t>(self)==receiver && Registered(record,reinterpret_cast<uintptr_t>(ResizeEvent)) &&
           Read(reinterpret_cast<uintptr_t>(event),eventType) && eventType==base+eventTableRva &&
           sceneresolution::OwnsNativeResize(base))repair.Begin(receiver);
        else ++refused;
        result=original(self,event);
    }
    Report();
    return result;
}
void Report() {
    if(output.empty())return;
    try {
        std::lock_guard lock(reportMutex);
        std::ofstream file(output/L"frontend-video-resize.json",std::ios::trunc);
        file<<"{\"schema\":1,\"strategy\":\"private_event_callback\",\"status\":\""<<status.load()
            <<"\",\"installed\":"<<(installed.load()?"true":"false")<<",\"imageWrites\":0,\"recordWrites\":"<<recordWrites.load()
            <<",\"callbacks\":"<<callbacks.load()<<",\"repairs\":"<<repairs.load()<<",\"restores\":"<<restores.load()
            <<",\"refused\":"<<refused.load()<<",\"ownerThread\":"<<ownerThread.load()<<",\"lastCallbackThread\":"<<lastCallbackThread.load()<<"}\n";
    }catch(...){}
}
bool ValidateAttach(uintptr_t at) {return at==record && Registered(at,base+handlerRva);}
bool ValidateRestore(uintptr_t at) {return at==record && Registered(at,reinterpret_cast<uintptr_t>(ResizeEvent));}
void Attached(uintptr_t) noexcept {installed=true;++recordWrites;}
void Restored(uintptr_t) noexcept {installed=false;++recordWrites;}
constexpr drivingcontrols::InstanceBusyRange busy[]{{0x15a4e0,0x15a558},{0x77f710,0x77f783},{0x67fbb0,0x67fc10},{0x206a40,0x206b60},{0x100e70,0x101000}};
uintptr_t FindRecord() {
    // Only writable private allocations are read. Bounded chunks avoid
    // dereferencing volatile heap pages, and a complete scan proves uniqueness.
    std::array<uint32_t,16384> buffer{};uintptr_t found{};size_t scanned{};
    SYSTEM_INFO system{};GetSystemInfo(&system);
    const uint64_t limit=uint64_t(reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress))+1;
    const auto started=GetTickCount64();uint64_t cursor=0x10000;
    while(cursor<limit) {
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(cursor)),&info,sizeof(info)))return 0;
        const auto end=(std::min)(limit,uint64_t(reinterpret_cast<uintptr_t>(info.BaseAddress))+info.RegionSize);
        if(end<=cursor)return 0;
        if(info.State==MEM_COMMIT && info.Type==MEM_PRIVATE && info.Protect==PAGE_READWRITE) {
            for(auto at=cursor;at<end;) {
                const auto count=(std::min)(size_t(end-at),sizeof(buffer));
                scanned+=count;if(scanned>size_t(1536)*1024*1024 || GetTickCount64()-started>1500)return 0;
                const auto address=static_cast<uintptr_t>(at);
                if(ReadMemory(reinterpret_cast<void*>(address),buffer.data(),count)) {
                    for(size_t i=0;i<count/4;++i)if(buffer[i]==base+handlerRva && Registered(address+i*4,base+handlerRva)) {
                        if(found && found!=address+i*4)return 0;found=address+i*4;
                    }
                }
                at+=count;
            }
        }
        cursor=end;
    }
    return found;
}
}
bool Install(uintptr_t gameBase,const std::filesystem::path& folder) {
    std::lock_guard lock(configuration);
    if(installed) {
        if(base!=gameBase || stopped)return false;
        if(Registered(record,reinterpret_cast<uintptr_t>(ResizeEvent)))return true;
        // Native replacement/removal owns the old allocation. Never write it
        // after membership is lost; discovery may attach its new record.
        installed=false;
    }
    if(stopped || (base && base!=gameBase))return false;
    if(!base){base=gameBase;output=folder;}
    if(!EventGuards(base)){status="instruction_guard";Report();return false;}
    record=FindRecord();
    uintptr_t boundReceiver{};
    if(!record || !Read(record+4,boundReceiver)){status="callback_not_unique_or_not_ready";Report();return false;}
    receiver=boundReceiver;
    ownerThread=GetCurrentThreadId();
    const auto failure=drivingcontrols::AttachInputInstance(reinterpret_cast<void*>(record.load()),reinterpret_cast<void*>(base+handlerRva),
        reinterpret_cast<void*>(ResizeEvent),base,ValidateAttach,busy,Attached,true);
    status=failure?failure:"attached";Report();return !failure;
}
void Stop() {
    std::lock_guard lock(configuration);stopped=true;
    if(installed) {
        const auto failure=drivingcontrols::AttachInputInstance(reinterpret_cast<void*>(record.load()),reinterpret_cast<void*>(ResizeEvent),
            reinterpret_cast<void*>(base+handlerRva),base,ValidateRestore,busy,Restored,true);
        status=failure?"stop_record_changed_or_busy":"restored";
    }
    Report();
}
#ifdef GRID2VR_FRONTEND_VIDEO_TEST
void SetTestCallback(TestCallback callback){testCallback=callback;}
bool TestEventGuards(uintptr_t image){return EventGuards(image);}
#endif
}
