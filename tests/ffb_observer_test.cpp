#include "ffb_observer.h"
#include "pe_imports.h"
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <MinHook.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#define CHECK(value) do {if(!(value)){std::printf("ffb observer check line %d: %s\n",__LINE__,#value);return 1;}} while(false)
namespace observer=grid2vr::ffbobserver;

namespace {
std::string Read(const std::filesystem::path& path) {std::ifstream f(path);std::stringstream s;s<<f.rdbuf();return s.str();}
bool Has(const std::string& json,const std::string& text) {return json.find(text)!=std::string::npos;}
}

// The test program stands in for the game image: only calls returning into it
// are counted. A private keyboard device and a hidden, never-shown window are
// owned by this process; no game, controller output or user window is touched.
int main(int,char** argv) {
    const std::filesystem::path folder=std::filesystem::path(argv[0]).parent_path()/"ffb-observer-fixture";
    std::filesystem::remove_all(folder);std::filesystem::create_directories(folder);
    CHECK(MH_Initialize()==MH_OK);
    const auto self=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    // Early route: this program imports DINPUT8!DirectInput8Create by name, as the game does.
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(self+reinterpret_cast<const IMAGE_DOS_HEADER*>(self)->e_lfanew);
    void** createImport=grid2vr::FindImportSlot(self,"DINPUT8.dll",0,"DirectInput8Create");
    CHECK(createImport);
    void* realCreate=*createImport;
    CHECK(observer::AttachEarly(self,nt->OptionalHeader.SizeOfImage,static_cast<uint32_t>(reinterpret_cast<uintptr_t>(createImport)-self)));
    CHECK(*createImport!=realCreate && !observer::AttachEarly(self,nt->OptionalHeader.SizeOfImage,0));
    observer::Install(self,folder);
    auto json=Read(folder/"ffb-observer.json");
    // This program has no XINPUT1_3 import; DirectInput device methods are hooked.
    CHECK(Has(json,"\"reason\":\"observing_directinput_only\"") && Has(json,"\"setPatched\":false") && Has(json,"\"observeOnly\":true"));
    CHECK(Has(json,"\"shaping\":{\"configured\":false,\"active\":false,"));
    // Launcher Force Feedback: out-of-range settings are ignored; neutral ones change nothing.
    observer::ConfigureShaping({100,0,50});json=Read(folder/"ffb-observer.json");CHECK(Has(json,"\"configured\":false"));
    observer::ConfigureShaping({});json=Read(folder/"ffb-observer.json");
    CHECK(Has(json,"\"observeOnly\":true") && Has(json,"\"shaping\":{\"configured\":true,\"active\":false,\"strength\":100,\"weight\":100,\"effects\":100,"));
    observer::ConfigureShaping({70,50,20});json=Read(folder/"ffb-observer.json");
    CHECK(Has(json,"\"observeOnly\":false") && Has(json,"\"shaping\":{\"configured\":true,\"active\":true,\"strength\":70,\"weight\":50,\"effects\":20,\"splitHz\":3,\"shaped\":0,"));

    const auto module=GetModuleHandleW(nullptr);
    HWND window=CreateWindowExW(0,L"STATIC",L"ffb-observer-fixture",WS_POPUP,0,0,1,1,nullptr,nullptr,module,nullptr);
    CHECK(window && !IsWindowVisible(window));
    IDirectInput8W* input{};
    CHECK(SUCCEEDED(DirectInput8Create(module,DIRECTINPUT_VERSION,IID_IDirectInput8W,reinterpret_cast<void**>(&input),nullptr)));
    LPDIRECTINPUTDEVICE8W keyboard{};
    CHECK(SUCCEEDED(input->CreateDevice(GUID_SysKeyboard,&keyboard,nullptr)));
    CHECK(SUCCEEDED(keyboard->SetDataFormat(&c_dfDIKeyboard)));
    const HRESULT cooperative=keyboard->SetCooperativeLevel(window,DISCL_NONEXCLUSIVE|DISCL_BACKGROUND);
    const HRESULT acquired=keyboard->Acquire();
    const HRESULT command=keyboard->SendForceFeedbackCommand(DISFFC_RESET);
    DIEFFECT effect{};effect.dwSize=sizeof(effect);LPDIRECTINPUTEFFECT created{};
    const HRESULT creation=keyboard->CreateEffect(GUID_ConstantForce,&effect,&created,nullptr);
    CHECK(SUCCEEDED(cooperative) && SUCCEEDED(acquired) && FAILED(command) && FAILED(creation) && !created);
    keyboard->Unacquire();keyboard->Release();input->Release();DestroyWindow(window);

    observer::Stop();
    json=Read(folder/"ffb-observer.json");
    char expected[96]{};
    CHECK(Has(json,"\"stopped\":true") && Has(json,"\"backgroundRequests\":1") && Has(json,"\"exclusiveRequests\":0"));
    CHECK(Has(json,"\"cooperativeLevel\":{\"calls\":1,\"failures\":0"));
    CHECK(Has(json,"\"acquire\":{\"calls\":1,\"failures\":0"));
    sprintf_s(expected,"\"sendCommand\":{\"calls\":1,\"failures\":1,\"lastResult\":\"0x%08x\"",static_cast<unsigned>(command));
    CHECK(Has(json,expected));
    sprintf_s(expected,"\"createEffect\":{\"calls\":1,\"failures\":1,\"lastResult\":\"0x%08x\"",static_cast<unsigned>(creation));
    CHECK(Has(json,expected) && Has(json,"\"lastCommand\":1"));
    CHECK(Has(json,"\"effectHooks\":0"));
    // The import route was published and the program's own create calls were counted; Stop restores it.
    // The program's own CreateDevice is observed for both interfaces (here the observer's private
    // objects also pass this program's patched import); its keyboard shares the hooked methods.
    CHECK(Has(json,"\"createDeviceHooked\":[true,true]") && Has(json,"{\"guidData1\":\"0x6f1d2b61\",\"shared\":true,\"table\":\"DINPUT8.dll+"));
    CHECK(Has(json,"\"foreign\":{\"cooperativeLevel\":{\"calls\":"));
    CHECK(Has(json,"\"createSlot\":true") && !Has(json,"\"gameCreates\":0,") && *createImport==realCreate);
    // After Stop, calls pass through and are no longer counted.
    CHECK(SUCCEEDED(DirectInput8Create(module,DIRECTINPUT_VERSION,IID_IDirectInput8W,reinterpret_cast<void**>(&input),nullptr)));
    CHECK(SUCCEEDED(input->CreateDevice(GUID_SysKeyboard,&keyboard,nullptr)));
    keyboard->Acquire();keyboard->Release();input->Release();
    observer::Stop();CHECK(Read(folder/"ffb-observer.json")==json);
    std::filesystem::remove_all(folder);
    std::puts("PASS: DirectInput cooperative/acquire/command/effect calls counted with results from the owning image; stop is pass-through; no game or device output");
    return 0;
}
