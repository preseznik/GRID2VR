#include "process_exit.h"
#include "stop_receipt.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

namespace {
using Microsoft::WRL::ComPtr;
std::filesystem::path folder;
std::wstring mode;
HWND window{};
WNDPROC subclass{};
DWORD owner{};
std::atomic<bool> initialized{},ready{},service{},stop{},cleaned{},invalid{};
std::atomic<unsigned> calls{},forwarded{},heartbeats{},polls{};
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
grid2vr::StopReceipt receipt;
using Exit=void (WINAPI*)(UINT);
Exit exitGame{};

const char* Cleanup() {
    if(GetCurrentThreadId()!=owner)invalid=true;
    ++calls;
    PostMessageW(window,WM_APP+1,0,0);
    if(mode==L"running")Sleep(5200);
    context.Reset();device.Reset();
    if(mode==L"failure")return "fixture_cleanup_failed";
    cleaned=true;
    std::ofstream(folder/L"callback.txt",std::ios::app)<<GetCurrentThreadId()<<'\n';
    return receipt.GameExit(folder/L"runtime-stopped")?"complete":"receipt_failed";
}

LRESULT CALLBACK NativeWindow(HWND target,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message==WM_APP+1) {++heartbeats;return 0;}
    if(message==WM_SYSCOMMAND && (wparam&0xfff0)==SC_CLOSE) {
        if(mode!=L"system" || wparam!=(SC_CLOSE|7) || lparam!=0x456)invalid=true;
        return SendMessageW(target,WM_CLOSE,0,0);
    }
    if(message==WM_CLOSE) {
        if(!cleaned && mode!=L"timeout" && mode!=L"failure")invalid=true;
        if(mode!=L"system" && (wparam!=0x123 || lparam!=0x456))invalid=true;
        ++forwarded;
        std::ofstream(folder/L"forwarded.txt")<<calls<<' '<<cleaned<<' '<<GetCurrentThreadId()<<'\n';
        DestroyWindow(target);
        if(mode==L"exit")exitGame(37);
        PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(target,message,wparam,lparam);
}
LRESULT CALLBACK LaterSubclass(HWND target,UINT message,WPARAM wparam,LPARAM lparam) {
    const auto result=CallWindowProcW(subclass,target,message,wparam,lparam);
    if(message==WM_NCDESTROY && reinterpret_cast<WNDPROC>(GetWindowLongPtrW(target,GWLP_WNDPROC))!=LaterSubclass)
        invalid=true;
    return result;
}

bool Initialize(uintptr_t base) {
    owner=GetCurrentThreadId();
    wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);
    auto module=LoadLibraryExW((std::filesystem::path(system)/L"d3d11.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(module?GetProcAddress(module,"D3D11CreateDevice"):nullptr);
    D3D_FEATURE_LEVEL level{};
    if(!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context)))return false;
    if(!grid2vr::processexit::Install(base,folder,Cleanup))return false;
    // Control: retain the old ExitProcess-only boundary. Native close then
    // reaches teardown before the owner ever receives a cleanup request.
    if(mode==L"late-only")return true;
    if(grid2vr::processexit::AttachWindow(nullptr) || grid2vr::processexit::AttachWindow(GetDesktopWindow()))return false;
    if(!grid2vr::processexit::AttachWindow(window) || !grid2vr::processexit::AttachWindow(window))return false;
    exitGame=reinterpret_cast<Exit>(*reinterpret_cast<void**>(base+0xb16200));return true;
}

int Child() {
    auto* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,0xb18000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!image)return 1;
    const auto base=reinterpret_cast<uintptr_t>(image);
    constexpr std::array<uint8_t,15> entry{0x55,0x8b,0xec,0x56,0x8b,0xf1,0x80,0xbe,0x94,0,0,0,0,0x75,0x1f};
    memcpy(image+0x607750,entry.data(),entry.size());
    image[0x607778]=0xff;image[0x607779]=0x15;
    const uint32_t operand=static_cast<uint32_t>(base+0xb16200);memcpy(image+0x60777a,&operand,4);
    *reinterpret_cast<void**>(image+0xb16200)=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"ExitProcess");
    WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=NativeWindow;cls.lpszClassName=L"GRID2VR-close-fixture";
    if(!RegisterClassW(&cls))return 2;
    window=CreateWindowExW(0,cls.lpszClassName,L"Never shown",WS_OVERLAPPEDWINDOW,0,0,64,64,nullptr,nullptr,cls.hInstance,nullptr);
    if(!window || grid2vr::processexit::AttachWindow(window))return 3; // Guarded install is required first.
    const bool sameThread=mode==L"same-thread";
    std::thread renderer;
    if(sameThread)ready=Initialize(base);
    else {
        renderer=std::thread([&] {
            ready=Initialize(base);initialized=true;
            while(ready && !stop) {
                if(service) {grid2vr::processexit::Service();++polls;}
                Sleep(1);
            }
            context.Reset();device.Reset();
        });
        while(!initialized)Sleep(1);
    }
    if(!ready) {stop=true;if(renderer.joinable())renderer.join();return 4;}
    if(!sameThread && grid2vr::processexit::AttachWindow(window))invalid=true; // Foreign owner cannot attach.
    if(mode==L"subclass") {
        subclass=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(LaterSubclass)));
    }
    const auto began=GetTickCount64();
    if(mode==L"system")SendMessageW(window,WM_SYSCOMMAND,SC_CLOSE|7,0x456);
    else SendMessageW(window,WM_CLOSE,0x123,0x456);
    if(GetTickCount64()-began>500 || calls || forwarded)invalid=true; // UI must not block waiting for Present.
    SendMessageW(window,WM_CLOSE,0x123,0x456); // Duplicate close must retain the original request.
    SendMessageW(window,WM_APP+1,0,0);
    if(heartbeats!=1 || forwarded)invalid=true;
    if(mode==L"destroy") {
        DestroyWindow(window);service=true;
        Sleep(80);stop=true;renderer.join();
        return calls || forwarded || std::filesystem::exists(folder/L"runtime-stopped")?5:0;
    }
    service=mode!=L"timeout";
    MSG message{};
    while(GetMessageW(&message,nullptr,0,0)>0) {
        TranslateMessage(&message);DispatchMessageW(&message);
        if(sameThread)grid2vr::processexit::Service();
    }
    if(mode==L"timeout") {
        // Once native teardown is released, a late Present cannot start cleanup.
        service=true;const auto deadline=GetTickCount64()+1000;
        while(!polls && GetTickCount64()<deadline)Sleep(1);
        if(!polls)invalid=true;
    }
    stop=true;if(renderer.joinable())renderer.join();
    const bool timeout=mode==L"timeout",failure=mode==L"failure";
    if(forwarded!=1 || calls!=(timeout?0u:1u) || invalid)return 6;
    if(std::filesystem::exists(folder/L"runtime-stopped")!=( !timeout && !failure))return 7;
    if(!timeout && heartbeats<2)return 8;
    return 0;
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path);return {std::istreambuf_iterator<char>(in),{}};
}
}

int wmain(int argc,wchar_t** argv) {
    if(argc==3) {folder=argv[1];mode=argv[2];return Child();}
    if(argc!=2)return 20;
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
    const auto root=std::filesystem::path(argv[1])/std::to_wstring(GetCurrentProcessId());
    for(const auto* name:{L"late-only",L"close",L"system",L"same-thread",L"subclass",L"running",L"timeout",L"failure",L"destroy",L"exit"}) {
        const auto target=root/name;std::filesystem::create_directories(target);
        std::wstring command=L"\""+std::wstring(executable)+L"\" \""+target.wstring()+L"\" "+name;
        STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
        if(!CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))return 21;
        const auto wait=WaitForSingleObject(process.hProcess,15000);
        if(wait!=WAIT_OBJECT_0) {TerminateProcess(process.hProcess,99);WaitForSingleObject(process.hProcess,5000);}
        DWORD code{};GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hThread);CloseHandle(process.hProcess);
        const bool terminal=wcscmp(name,L"exit")==0;
        if(wcscmp(name,L"late-only")==0) {
            if(wait!=WAIT_OBJECT_0 || code!=6 || !Read(target/L"forwarded.txt").starts_with("0 0 ") ||
               std::filesystem::exists(target/L"runtime-stopped"))return 29;
            continue;
        }
        if(wait!=WAIT_OBJECT_0 || code!=(terminal?37u:0u)) {printf("%ls failed: %lu\n",name,code);return 22;}
        const auto log=Read(target/L"window-close.txt");
        if(log.find("close requested=1 deferred=1")==std::string::npos)return 23;
        if(wcscmp(name,L"running")==0 && log.find("still_running forwarding=0")==std::string::npos)return 24;
        if(wcscmp(name,L"timeout")==0 && log.find("owner_unavailable forwarding=1")==std::string::npos)return 25;
        if(wcscmp(name,L"failure")==0 && log.find("fixture_cleanup_failed")==std::string::npos)return 26;
        if(terminal) {
            if(Read(target/L"process-exit.txt").find("exit cleanup=complete forwarding=1")==std::string::npos)return 27;
            const auto callback=Read(target/L"callback.txt");
            if(std::count(callback.begin(),callback.end(),'\n')!=1)return 28;
        }
    }
    puts("Close deferral preserves UI pumping, exact native messages, render-owner WARP cleanup, timeout/refusal, subclass ownership and single exit cleanup.");
    return 0;
}
