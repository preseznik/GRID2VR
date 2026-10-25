#include "process_exit.h"
#include "stop_receipt.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <thread>

namespace {
std::filesystem::path folder;
DWORD cleanupOwner{};
grid2vr::StopReceipt stopReceipt;
const char* Cleanup() {
    std::ofstream(folder/L"callback.txt") << GetCurrentThreadId();
    if(GetCurrentThreadId()!=cleanupOwner)return "wrong_callback_thread";
    return stopReceipt.GameExit(folder/L"runtime-stopped")?"complete":"stop_receipt_failed";
}
using Exit=void (WINAPI*)(UINT);
int Child(const wchar_t* mode) {
    cleanupOwner=GetCurrentThreadId();
    auto* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,0xb17000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
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
    if(!grid2vr::processexit::Install(base,folder,Cleanup))return 7;
    MEMORY_BASIC_INFORMATION region{};
    if(!VirtualQuery(slot,&region,sizeof(region)) || region.Protect!=PAGE_READONLY)return 8;
    auto exitGame=reinterpret_cast<Exit>(*slot);
    if(wcscmp(mode,L"owner")==0)exitGame(37);
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
    for(const auto* mode:{L"owner",L"handoff",L"timeout"}) {
        const auto target=root/mode;
        std::filesystem::create_directories(target);
        std::wstring command=L"\""+std::wstring(executable)+L"\" \""+target.wstring()+L"\" "+mode;
        STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
        if(!CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))return 11;
        const auto wait=WaitForSingleObject(process.hProcess,10000);
        if(wait!=WAIT_OBJECT_0) {TerminateProcess(process.hProcess,99);WaitForSingleObject(process.hProcess,5000);}
        DWORD code{};GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hThread);CloseHandle(process.hProcess);
        if(wait!=WAIT_OBJECT_0 || code!=37)return 12;
        const bool cleaned=wcscmp(mode,L"timeout")!=0;
        if(std::filesystem::exists(target/L"callback.txt")!=cleaned)return 13;
        if(std::filesystem::exists(target/L"runtime-stopped")!=cleaned)return 15;
        if(cleaned && Read(target/L"runtime-stopped")!="{\"failed\":false,\"source\":\"game_exit\"}\n")return 16;
        const auto log=Read(target/L"process-exit.txt");
        if(log.find(cleaned?"cleanup=complete":"cleanup=owner_unavailable")==std::string::npos ||
           log.find("forwarding=1")==std::string::npos)return 14;
    }
    puts("Guarded game import preserves exit code, owning-thread cleanup, worker handoff and bounded timeout.");
    return 0;
}
