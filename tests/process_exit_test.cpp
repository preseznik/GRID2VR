#include "process_exit.h"
#include "stop_receipt.h"
#include "chase_observer.h"
#include "chase_observer_guard.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <string_view>
#include <thread>

namespace {
std::filesystem::path folder;
DWORD cleanupOwner{};
uintptr_t observerBase{};
std::wstring childMode;
bool observerCallbackEnabled{};
grid2vr::StopReceipt stopReceipt;
const char* Cleanup() {
    std::ofstream(folder/L"callback.txt") << GetCurrentThreadId();
    if(GetCurrentThreadId()!=cleanupOwner)return "wrong_callback_thread";
    if(observerCallbackEnabled && !std::filesystem::exists(folder/L"observer-callback.txt"))return "observer_callback_order";
    return stopReceipt.GameExit(folder/L"runtime-stopped")?"complete":"stop_receipt_failed";
}
const char* ObserverCleanup() {
    { std::ofstream out(folder/L"observer-callback.txt",std::ios::app);
      out<<GetCurrentThreadId()<<' '<<(GetCurrentThreadId()==cleanupOwner)<<'\n'; }
    if(childMode==L"observer-throw")throw 1;
    if(childMode==L"observer-refusal")return "fixture_refused";
    const auto result=grid2vr::chaseobserver::StopForExit();
    using namespace grid2vr::chaseobserver;
    bool restored=true;
    for(size_t i=0;i<slots.size();++i)
        restored&=*reinterpret_cast<uintptr_t*>(observerBase+SlotRva(i))==observerBase+routines[i];
    MEMORY_BASIC_INFORMATION region{};
    const bool readOnly=VirtualQuery(reinterpret_cast<void*>(observerBase+chaseTableRva),&region,sizeof(region)) && region.Protect==PAGE_READONLY &&
        VirtualQuery(reinterpret_cast<void*>(observerBase+mainTableRva),&region,sizeof(region)) && region.Protect==PAGE_READONLY &&
        VirtualQuery(reinterpret_cast<void*>(observerBase+providerTableRva),&region,sizeof(region)) && region.Protect==PAGE_READONLY;
    std::ofstream(folder/L"observer-restoration.txt")<<"restored="<<restored<<" readonly="<<readOnly<<'\n';
    return result;
}
using Exit=void (WINAPI*)(UINT);
int Child(const wchar_t* mode) {
    cleanupOwner=GetCurrentThreadId();
    childMode=mode;observerCallbackEnabled=childMode.starts_with(L"observer-");
    auto* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,0xe70000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!image)return 2;
    const auto base=reinterpret_cast<uintptr_t>(image);
    constexpr std::array<uint8_t,15> entry{0x55,0x8b,0xec,0x56,0x8b,0xf1,0x80,0xbe,0x94,0,0,0,0,0x75,0x1f};
    memcpy(image+0x607750,entry.data(),entry.size());
    image[0x607778]=0xff;image[0x607779]=0x15;
    const uint32_t operand=static_cast<uint32_t>(base+0xb16200);
    memcpy(image+0x60777a,&operand,4);
    auto** slot=reinterpret_cast<void**>(image+0xb16200);
    const auto original=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"ExitProcess");
    *slot=reinterpret_cast<void*>(original);
    // Refuse changed instructions, relocated operand and a foreign import.
    image[0x607750]^=1;
    if(grid2vr::processexit::Install(base,folder,Cleanup) || *slot!=reinterpret_cast<void*>(original))return 3;
    image[0x607750]^=1;image[0x60777a]^=4;
    if(grid2vr::processexit::Install(base,folder,Cleanup) || *slot!=reinterpret_cast<void*>(original))return 4;
    image[0x60777a]^=4;*slot=reinterpret_cast<void*>(Cleanup);
    if(grid2vr::processexit::Install(base,folder,Cleanup) || *slot!=reinterpret_cast<void*>(Cleanup))return 5;
    *slot=reinterpret_cast<void*>(original);
    DWORD old{};
    if(!VirtualProtect(slot,sizeof(void*),PAGE_READONLY,&old))return 6;
    if(!grid2vr::processexit::Install(base,folder,Cleanup,observerCallbackEnabled?ObserverCleanup:nullptr))return 7;
    MEMORY_BASIC_INFORMATION region{};
    if(!VirtualQuery(slot,&region,sizeof(region)) || region.Protect!=PAGE_READONLY)return 8;
    if(childMode==L"observer-timeout") {
        using namespace grid2vr::chaseobserver;
        observerBase=base;
        const auto put=[&](uintptr_t at,const auto& bytes){memcpy(image+at,std::data(bytes),std::size(bytes));};
        put(routines[0],updateEntry);put(routines[0]+0x2d,updateCall);
        put(routines[1],alternateBody);put(routines[2],copyBody);put(routines[3],obstructionBody);
        put(routines[4],recordPointerBody);put(0x804780,viewConverterCall);
        put(0x118581,selectedViewA);put(0x21a5f3,selectedViewB);
        put(0x2e3930,auxiliaryFrameEntry);put(0x2e394b,auxiliaryFrameAllocation);put(0x2e3c16,auxiliaryViewCall);
        put(routines[5],mainSceneEntry);put(0x3be050,completeSceneEntry);
        put(0x3be087,mainSceneCall);put(0x7a7cb3,completeSceneCall);
        put(routines[6],ProviderPositionGuard(base));put(0x1f78a0,preQueryEntry);
        put(0x61eb91,preQueryStage);put(providerBasisRva,providerBasisBody);
        *reinterpret_cast<uintptr_t*>(image+providerBasisSlotRva)=base+providerBasisRva;
        for(size_t i=0;i<slots.size();++i)*reinterpret_cast<uintptr_t*>(image+SlotRva(i))=base+routines[i];
        {std::ofstream request(folder/L"chase-camera-observer.request",std::ios::binary);request<<"observe-v1\n";}
        if(!VirtualProtect(image+chaseTableRva,0x1000,PAGE_READONLY,&old))return 17;
        if(!VirtualProtect(image+providerTableRva,0x1000,PAGE_READONLY,&old))return 17;
        if(!VirtualProtect(image+mainTableRva,0x1000,PAGE_READONLY,&old))return 25;
        Prepare(base,folder);Install();
        for(size_t i=0;i<slots.size();++i)
            if(*reinterpret_cast<uintptr_t*>(image+SlotRva(i))==base+routines[i])return 18;
    }
    auto exitGame=reinterpret_cast<Exit>(*slot);
    if(childMode==L"owner" || childMode==L"observer-throw" || childMode==L"observer-refusal")exitGame(37);
    else {
        std::thread other([=]{grid2vr::processexit::Service();exitGame(37);});
        if(wcscmp(mode,L"handoff")==0) {
            for(;;) {
                grid2vr::processexit::Service();
                if(stopReceipt.Reported())stopReceipt.Stop(folder/L"runtime-stopped",false);
                Sleep(1);
            }
        }
        other.join();
    }
    return 9; // The original ExitProcess must remain terminal.
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path);return {std::istreambuf_iterator<char>(in),{}};
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc==3) {folder=argv[1];return Child(argv[2]);}
    if(argc!=2)return 1;
    wchar_t executable[32768]{};
    if(!GetModuleFileNameW(nullptr,executable,32768))return 10;
    const auto root=std::filesystem::path(argv[1])/std::to_wstring(GetCurrentProcessId());
    for(const auto* mode:{L"owner",L"handoff",L"timeout",L"observer-timeout",L"observer-throw",L"observer-refusal"}) {
        const auto target=root/mode;
        std::filesystem::create_directories(target);
        std::wstring command=L"\""+std::wstring(executable)+L"\" \""+target.wstring()+L"\" "+mode;
        STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
        if(!CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))return 11;
        const auto wait=WaitForSingleObject(process.hProcess,10000);
        if(wait!=WAIT_OBJECT_0) {TerminateProcess(process.hProcess,99);WaitForSingleObject(process.hProcess,5000);}
        DWORD code{};GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hThread);CloseHandle(process.hProcess);
        if(wait!=WAIT_OBJECT_0 || code!=37)return 12;
        const bool observer=std::wstring_view(mode).starts_with(L"observer-");
        const bool observerTimeout=wcscmp(mode,L"observer-timeout")==0;
        const bool cleaned=wcscmp(mode,L"timeout")!=0 && !observerTimeout;
        if(std::filesystem::exists(target/L"callback.txt")!=cleaned)return 13;
        if(std::filesystem::exists(target/L"runtime-stopped")!=cleaned)return 15;
        if(cleaned && Read(target/L"runtime-stopped")!="{\"failed\":false,\"source\":\"game_exit\"}\n")return 16;
        const auto log=Read(target/L"process-exit.txt");
        if(log.find(cleaned?"exit cleanup=complete":"exit cleanup=owner_unavailable")==std::string::npos ||
           log.find("forwarding=1")==std::string::npos)return 14;
        if(observer) {
            const auto callback=Read(target/L"observer-callback.txt");
            if(std::count(callback.begin(),callback.end(),'\n')!=1)return 19;
            if(!callback.ends_with(observerTimeout?" 0\n":" 1\n"))return 20;
            const auto expected=observerTimeout?"observer_restored":wcscmp(mode,L"observer-throw")==0?"callback_exception":"fixture_refused";
            if(log.find(std::string("exit observer_cleanup=")+expected+'\n')==std::string::npos)return 21;
            if(observerTimeout) {
                if(Read(target/L"observer-restoration.txt")!="restored=1 readonly=1\n")return 22;
                const auto receipt=Read(target/L"chase-camera-observer.json");
                if(receipt.find("\"stopped\":true,\"restored\":true")==std::string::npos)return 23;
            }
        } else if(log.find("observer_cleanup=")!=std::string::npos)return 24;
    }
    puts("Guarded exit preserves code, owner-only cleanup and timeout; observer restores without owner service and failures still exit.");
    return 0;
}
