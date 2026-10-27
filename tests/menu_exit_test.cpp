#include "menu_exit.h"
#include "menu_exit_guard.h"
#include "process_exit.h"
#include "stop_receipt.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <algorithm>
#include <fstream>
#include <string>
#include <thread>

namespace {
using namespace grid2vr;
using namespace grid2vr::menuexit;
using Enter=void (__thiscall*)(void*,void*);
using Evaluate=const char** (__thiscall*)(void*,const char**);
using Leave=bool (__thiscall*)(void*,float);
std::filesystem::path folder;
DWORD owner{};
std::wstring mode;
Microsoft::WRL::ComPtr<ID3D11Device> device;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
StopReceipt stopped;
unsigned forwarded{};
void* forwardedArgument{};
const char* Cleanup() {
    {std::ofstream(folder/L"callback.txt",std::ios::app)<<GetCurrentThreadId()<<'\n';}
    if(GetCurrentThreadId()!=owner) return "wrong_thread";
    if(mode==L"slow") Sleep(5200);
    context->ClearState();context->Flush();context.Reset();device.Reset();
    if(mode==L"failure") return "fixture_failed";
    return stopped.GameExit(folder/L"runtime-stopped")?"complete":"receipt_failed";
}
void __fastcall Forward(void*,void*,void* argument) {++forwarded;forwardedArgument=argument;}
void Jump(uint8_t* at,void* target) {
    at[0]=0xe9;const auto offset=static_cast<int32_t>(reinterpret_cast<uint8_t*>(target)-at-5);
    memcpy(at+1,&offset,4);FlushInstructionCache(GetCurrentProcess(),at,5);
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path);return {std::istreambuf_iterator<char>(in),{}};
}
int Child(const wchar_t* selected) {
    mode=selected;owner=GetCurrentThreadId();
    auto* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,0xe70000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    if(!image)return 2;
    const auto base=reinterpret_cast<uintptr_t>(image);
    const auto put=[&](uintptr_t rva,const auto& bytes,const auto& relocations) {
        const auto expected=guard::Relocate(base,bytes,relocations);memcpy(image+rva,expected.data(),expected.size());
    };
    put(guard::endCurrentRva,guard::endCurrent,guard::endCurrentRelocations);
    put(guard::retireEventRva,guard::retireEvent,guard::retireEventRelocations);
    put(guard::baseEnterRva,guard::baseEnter,guard::baseEnterRelocations);
    put(guard::evaluateRva,guard::evaluate,guard::evaluateRelocations);
    put(guard::leaveRva,guard::leave,guard::leaveRelocations);
    put(guard::finishRva,guard::finish,guard::finishRelocations);
    const std::array<uintptr_t,2> entries{guard::endCurrentRva,guard::retireEventRva};
    for(unsigned k=0;k<2;++k) {
        auto** table=reinterpret_cast<void**>(image+guard::tables[k]);
        table[3]=image+entries[k];table[4]=image+guard::leaveRva;table[7]=image+guard::evaluateRva;
    }
    const auto unchanged=[&] {
        for(unsigned k=0;k<2;++k) {
            auto** t=reinterpret_cast<void**>(image+guard::tables[k]);
            if(t[3]!=image+entries[k] || t[4]!=image+guard::leaveRva || t[7]!=image+guard::evaluateRva)return false;
        }
        return true;
    };
    // Refuse changes throughout every guarded body, including relocated operands.
    for(const auto& [rva,size]:std::array<std::pair<uintptr_t,size_t>,6>{{
        {guard::endCurrentRva,guard::endCurrent.size()},{guard::retireEventRva,guard::retireEvent.size()},
        {guard::baseEnterRva,guard::baseEnter.size()},{guard::evaluateRva,guard::evaluate.size()},
        {guard::leaveRva,guard::leave.size()},{guard::finishRva,guard::finish.size()}}}) {
        for(size_t i=0;i<size;++i) {
            image[rva+i]^=1;
            if(Install(base,folder) || !unchanged())return 3;
            image[rva+i]^=1;
        }
    }
    for(const auto tableRva:guard::tables) for(unsigned slot:{3u,4u,7u}) {
        auto** at=reinterpret_cast<void**>(image+tableRva)+slot;const auto saved=*at;*at=image;
        if(Install(base,folder) || *at!=image)return 4;
        *at=saved;if(!unchanged())return 5;
    }
    auto** exitSlot=reinterpret_cast<void**>(image+0xb16200);
    *exitSlot=reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"ExitProcess"));
    if(!processexit::Install(base,folder,Cleanup))return 6;
    if(mode!=L"old-order") {
        DWORD previous{};
        if(!VirtualProtect(image+(guard::tables[0]&~uintptr_t(4095)),4096,PAGE_READONLY,&previous) || !Install(base,folder))return 7;
        MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQuery(image+guard::tables[0],&region,sizeof(region)) || region.Protect!=PAGE_READONLY)return 8;
    }
    // Execute the real guarded native base/entry/finish bodies in this child.
    // Stub only unrelated external dependencies: native string(null) and the
    // pre-ExitProcess library cleanup. No game or window is created.
    const uint8_t emptyString[]{0x8b,0xc1,0xc7,0x01,0,0,0,0,0xc2,4,0};
    memcpy(image+0x220cb0,emptyString,sizeof(emptyString));
    image[0x88b950]=0xc3;
    *reinterpret_cast<const char**>(image+0xdaf5fc)="next";
    auto* flags=image+guard::finishObjectRva;
    *reinterpret_cast<uint32_t*>(flags+0x84)=2;flags[0x88]=1;flags[0x94]=0;
    alignas(16) std::array<uint8_t,64> state{};
    const unsigned kind=mode==L"retire"?1:0;
    auto** table=reinterpret_cast<void**>(image+guard::tables[kind]);
    memcpy(state.data(),&table,sizeof(table));
    const auto begin=reinterpret_cast<Enter>(table[3]);
    const auto evaluate=reinterpret_cast<Evaluate>(table[7]);
    const auto leave=reinterpret_cast<Leave>(table[4]);
    FlushInstructionCache(GetCurrentProcess(),image,0xe70000);
    if(mode==L"ordinary" || mode==L"canceled") {
        // After guard validation, replace this fixture's original entry with an
        // argument recorder to observe ordinary forwarding without game globals.
        Jump(image+entries[kind],reinterpret_cast<void*>(Forward));
        if(mode==L"canceled") {
            begin(state.data(),image);const char* transition="sentinel";
            if(evaluate(state.data(),&transition)!=&transition || transition)return 20;
            if(!leave(state.data(),0.0f))return 21;
            processexit::Service();
            if(std::filesystem::exists(folder/L"callback.txt"))return 25;
            evaluate(state.data(),&transition);
            if(!transition || forwarded)return 22;
        }
        for(unsigned condition=0;condition<3;++condition) {
            *reinterpret_cast<uint32_t*>(flags+0x84)=condition==0?1:2;
            flags[0x88]=condition==1?0:1;flags[0x94]=condition==2?1:0;
            begin(state.data(),image+condition);
            if(forwarded!=condition+1 || forwardedArgument!=image+condition)return 23;
        }
        if(std::filesystem::exists(folder/L"callback.txt"))return 24;
        return 0;
    }
    D3D_FEATURE_LEVEL level{};
    if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context)))return 9;
    std::atomic<bool> yielded{};
    std::thread flow([&] {
        begin(state.data(),reinterpret_cast<void*>(0xdeadbeef));
        const char* transition="sentinel";
        if(evaluate(state.data(),&transition)!=&transition || transition) ExitProcess(10);
        yielded=true;
    });
    // The old control blocks the owner here until ExitProcess kills the child.
    // The corrected state returns an empty transition, letting Present run.
    flow.join();
    if(!yielded || std::filesystem::exists(folder/L"callback.txt"))return 11;
    std::ofstream(folder/L"flow-yielded.txt")<<"no transition; render owner can service\n";
    std::thread nextTick([&] {
        for(;;) {
            const char* transition="sentinel";
            if(evaluate(state.data(),&transition)!=&transition || transition) ExitProcess(12);
            Sleep(1);
        }
    });
    if(mode!=L"timeout") processexit::Service();
    nextTick.join();
    return 13; // Original native ExitProcess must remain terminal.
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc==3) {folder=argv[1];return Child(argv[2]);}
    if(argc!=2)return 1;
    wchar_t executable[32768]{};if(!GetModuleFileNameW(nullptr,executable,32768))return 30;
    const auto root=std::filesystem::path(argv[1])/std::to_wstring(GetCurrentProcessId());
    for(const auto* selected:{L"old-order",L"end-current",L"retire",L"slow",L"failure",L"timeout",L"ordinary",L"canceled"}) {
        const auto target=root/selected;std::filesystem::create_directories(target);
        std::wstring command=L"\""+std::wstring(executable)+L"\" \""+target.wstring()+L"\" "+selected;
        STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
        if(!CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))return 31;
        const auto wait=WaitForSingleObject(process.hProcess,15000);
        if(wait!=WAIT_OBJECT_0) {TerminateProcess(process.hProcess,99);WaitForSingleObject(process.hProcess,5000);}
        DWORD code{};GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hThread);CloseHandle(process.hProcess);
        if(wait!=WAIT_OBJECT_0 || code!=0) {wprintf(L"menu exit %s failed: code=%lu\n",selected,code);return 32;}
        const std::wstring_view name(selected);
        const bool ordinary=name==L"ordinary" || name==L"canceled";
        const bool cleaned=!ordinary && name!=L"old-order" && name!=L"timeout";
        const auto callback=Read(target/L"callback.txt");
        if(std::count(callback.begin(),callback.end(),'\n')!=(cleaned?1:0))return 33;
        if(std::filesystem::exists(target/L"runtime-stopped")!=(cleaned && name!=L"failure"))return 34;
        if(!ordinary) {
            const auto log=Read(target/L"process-exit.txt");
            const auto expected=name==L"failure"?"fixture_failed":cleaned?"complete":"owner_unavailable";
            if(log.find(std::string("exit cleanup=")+expected+" forwarding=1")==std::string::npos)return 35;
            if(std::filesystem::exists(target/L"flow-yielded.txt")!=(name!=L"old-order"))return 36;
        }
    }
    puts("Menu exit: native terminal bodies, ordinary forwarding, owner handoff, slow cleanup, timeout, cancellation and guards passed.");
    return 0;
}
