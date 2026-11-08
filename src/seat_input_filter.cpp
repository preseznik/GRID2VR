#define DIRECTINPUT_VERSION 0x0800
#include "seat_input_filter.h"
#include <dinput.h>
#include <xinput.h>
#include <MinHook.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace grid2vr::seatinput {
namespace {
std::atomic<std::shared_ptr<const Policy>> policy;
void (*logLine)(const char*,...){};
bool ready{};

// Only the game's own reads are filtered; GRID2VR's and the runtime's pass unchanged.
bool GameCaller(void* caller) {
    static const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    static const auto size=reinterpret_cast<IMAGE_NT_HEADERS*>(base+reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew)->OptionalHeader.SizeOfImage;
    const auto address=reinterpret_cast<uintptr_t>(caller);
    return address>=base && address<base+size;
}
// MinHook's status, for the install log.
MH_STATUS Hook(void* target,void* replacement,void** original) {
    if(!target) return MH_ERROR_FUNCTION_NOT_FOUND;
    const auto created=MH_CreateHook(target,replacement,original);
    if(created!=MH_OK) return created;
    const auto enabled=MH_EnableHook(target);
    if(enabled!=MH_OK) MH_RemoveHook(target);
    return enabled;
}

// Keyboard: Raw Input.
using RawDataFn=UINT (WINAPI*)(HRAWINPUT,UINT,LPVOID,PUINT,UINT);
RawDataFn rawData{};
std::mutex keyboardMutex;
KeyboardFilter keyboard;
using AsyncFn=SHORT (WINAPI*)(int);
using KeyboardStateFn=BOOL (WINAPI*)(PBYTE);
AsyncFn asyncKey{},keyState{};
KeyboardStateFn keyboardState{};
bool Blocked(unsigned key) {
    const auto current=policy.load();return current && current->blocksKey && current->blocksKey(key);
}
SHORT WINAPI AsyncKey(int key) {
    const auto result=asyncKey(key);
    return GameCaller(_ReturnAddress()) && key>=0 && key<256 && Blocked(unsigned(key))?0:result;
}
SHORT WINAPI KeyState(int key) {
    const auto result=keyState(key);
    return GameCaller(_ReturnAddress()) && key>=0 && key<256 && Blocked(unsigned(key))?0:result;
}
BOOL WINAPI KeyboardState(PBYTE output) {
    const auto result=keyboardState(output);
    if(result && output && GameCaller(_ReturnAddress()))for(unsigned key=0;key<256;++key)if(Blocked(key))output[key]=0;
    return result;
}
std::mutex windowMutex;
HWND gameWindow{};
WNDPROC previousWindow{};
KeyboardFilter windowKeyboard;
LRESULT CALLBACK Window(HWND window,UINT message,WPARAM key,LPARAM details) {
    WNDPROC previous{};
    {
        std::lock_guard lock(windowMutex);previous=previousWindow;
        if(window==gameWindow && (message==WM_CHAR || message==WM_SYSCHAR) && (key==VK_TAB || key==VK_RETURN || key==VK_ESCAPE) && Blocked(static_cast<unsigned>(key)))return 0;
        if(window==gameWindow && key<0xff && (message==WM_KEYDOWN || message==WM_KEYUP || message==WM_SYSKEYDOWN || message==WM_SYSKEYUP)) {
            const auto current=policy.load();
            if(current && current->blocksKey) {
                RAWKEYBOARD event{};event.VKey=static_cast<USHORT>(key);event.Message=message;
                if(windowKeyboard.Apply(event,current->blocksKey)) {
                    if(event.VKey==0xff)return 0;
                    message=event.Message;
                    if(message==WM_KEYUP || message==WM_SYSKEYUP)details|=static_cast<LPARAM>(0xc0000000u);
                }
            }
        }
    }
    return previous?CallWindowProcW(previous,window,message,key,details):DefWindowProcW(window,message,key,details);
}
UINT WINAPI RawData(HRAWINPUT input,UINT command,LPVOID data,PUINT size,UINT headerSize) {
    const auto result=rawData(input,command,data,size,headerSize);
    if(result==UINT(-1) || command!=RID_INPUT || !data || headerSize!=sizeof(RAWINPUTHEADER) || result<sizeof(RAWINPUTHEADER)+sizeof(RAWKEYBOARD) ||
       !GameCaller(_ReturnAddress()))
        return result;
    auto* raw=static_cast<RAWINPUT*>(data);
    if(raw->header.dwType!=RIM_TYPEKEYBOARD) return result;
    const auto current=policy.load();
    if(!current || !current->blocksKey) return result;
    std::lock_guard lock(keyboardMutex);
    keyboard.Apply(raw->data.keyboard,current->blocksKey);
    return result;
}

// DirectInput devices: every object of a read is offered to the policy.
struct Device { GUID id{}; bool keyboard{},unicode{}; std::vector<DIOBJECTDATAFORMAT> objects; };
std::mutex deviceMutex;
std::map<void*,Device> devices;
using CreateFn=HRESULT (STDMETHODCALLTYPE*)(IDirectInput8W*,REFGUID,IDirectInputDevice8W**,IUnknown*);
using StateFn=HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8W*,DWORD,void*);
using DataFn=HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8W*,DWORD,DIDEVICEOBJECTDATA*,DWORD*,DWORD);
using FormatFn=HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8W*,const DIDATAFORMAT*);
CreateFn create{},createA{};
StateFn state{};
DataFn data{};
FormatFn format{};
bool deviceHooks{};

// A device the game opened before the hooks is first seen when read: learn its identity
// and objects then. Its interface may be ANSI or Unicode, and each refuses the other's
// structure size, so both are tried. Caller holds deviceMutex.
template<class Info,class Interface> bool DeviceInfo(IDirectInputDevice8W* self,GUID& id,bool& isKeyboard) {
    Info info{sizeof(info)};
    if(FAILED(reinterpret_cast<Interface*>(self)->GetDeviceInfo(&info))) return false;
    id=info.guidInstance;
    isKeyboard=GET_DIDEVICE_TYPE(info.dwDevType)==DI8DEVTYPE_KEYBOARD;
    return true;
}
template<class Info,class Interface> bool ObjectType(IDirectInputDevice8W* self,DWORD offset,DWORD& type) {
    Info info{sizeof(info)};
    if(FAILED(reinterpret_cast<Interface*>(self)->GetObjectInfo(&info,offset,DIPH_BYOFFSET)) || info.dwOfs!=offset) return false;
    type=info.dwType;
    return true;
}
Device* Known(IDirectInputDevice8W* self,DWORD length) {
    if(auto found=devices.find(self);found!=devices.end()) return &found->second;
    Device device;
    if(!DeviceInfo<DIDEVICEINSTANCEW,IDirectInputDevice8W>(self,device.id,device.keyboard) &&
       !DeviceInfo<DIDEVICEINSTANCEA,IDirectInputDevice8A>(self,device.id,device.keyboard))
        return nullptr;
    for(DWORD offset=0;offset<std::min<DWORD>(length,1024);) {
        DWORD type{};
        const bool found=ObjectType<DIDEVICEOBJECTINSTANCEW,IDirectInputDevice8W>(self,offset,type) ||
                         ObjectType<DIDEVICEOBJECTINSTANCEA,IDirectInputDevice8A>(self,offset,type);
        if(found) device.objects.push_back({nullptr,offset,type,0});
        offset+=found && !(type&DIDFT_BUTTON) ? 4u : 1u;
    }
    return &(devices[self]=std::move(device));
}

void FilterDeviceObject(const Policy& current,const Device& device,const DIOBJECTDATAFORMAT& object,void* value,DWORD size) {
    if(device.keyboard && (object.dwType&DIDFT_BUTTON) && size>=1 && current.blocksKey) {
        const auto scan=DIDFT_GETINSTANCE(object.dwType);
        const auto code=(scan&0x80)?0xe000u|(scan&0x7f):scan;
        const auto key=MapVirtualKeyW(code,MAPVK_VSC_TO_VK_EX);
        if(key && current.blocksKey(key))*static_cast<BYTE*>(value)=0;
    } else if(!device.keyboard)FilterObject(current,device.id,object.dwType,value,size);
}
HRESULT STDMETHODCALLTYPE State(IDirectInputDevice8W* self,DWORD length,void* output) {
    const auto hr=state(self,length,output);
    if(FAILED(hr) || !output || !GameCaller(_ReturnAddress())) return hr;
    const auto current=policy.load();
    if(!current) return hr;
    std::lock_guard lock(deviceMutex);
    const auto* device=Known(self,length);
    if(!device) return hr;
    for(const auto& object:device->objects) {
        const DWORD size=(object.dwType&DIDFT_BUTTON) ? 1 : 4;
        if(object.dwOfs<=length && size<=length-object.dwOfs)
            FilterDeviceObject(*current,*device,object,static_cast<BYTE*>(output)+object.dwOfs,size);
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Data(IDirectInputDevice8W* self,DWORD objectSize,DIDEVICEOBJECTDATA* output,DWORD* count,DWORD flags) {
    const auto hr=data(self,objectSize,output,count,flags);
    if(FAILED(hr) || !output || !count || objectSize<16 || !GameCaller(_ReturnAddress())) return hr;
    const auto current=policy.load();
    if(!current) return hr;
    std::lock_guard lock(deviceMutex);
    const auto* device=Known(self,sizeof(DIJOYSTATE2));
    if(!device) return hr;
    for(DWORD i=0;i<*count;++i) {
        auto* row=reinterpret_cast<DIDEVICEOBJECTDATA*>(reinterpret_cast<BYTE*>(output)+i*objectSize);
        for(const auto& object:device->objects)
            if(object.dwOfs==row->dwOfs) { FilterDeviceObject(*current,*device,object,&row->dwData,(object.dwType&DIDFT_BUTTON) ? 1 : 4); break; }
    }
    return hr;
}
// The object list comes from the game's data format, with each object's real type.
HRESULT STDMETHODCALLTYPE Format(IDirectInputDevice8W* self,const DIDATAFORMAT* value) {
    const auto hr=format(self,value);
    if(FAILED(hr) || !value || value->dwObjSize!=sizeof(DIOBJECTDATAFORMAT) || value->dwNumObjs>512) return hr;
    std::vector<DIOBJECTDATAFORMAT> objects(value->rgodf,value->rgodf+value->dwNumObjs);
    bool unicode{};
    { std::lock_guard lock(deviceMutex); unicode=devices[self].unicode; }
    for(auto& object:objects) {
        if(unicode) {
            DIDEVICEOBJECTINSTANCEW info{sizeof(info)};
            if(SUCCEEDED(self->GetObjectInfo(&info,object.dwOfs,DIPH_BYOFFSET))) object.dwType=info.dwType;
        } else {
            DIDEVICEOBJECTINSTANCEA info{sizeof(info)};
            if(SUCCEEDED(reinterpret_cast<IDirectInputDevice8A*>(self)->GetObjectInfo(&info,object.dwOfs,DIPH_BYOFFSET))) object.dwType=info.dwType;
        }
    }
    std::lock_guard lock(deviceMutex);
    devices[self].objects=std::move(objects);
    return hr;
}
template<bool Unicode> HRESULT STDMETHODCALLTYPE CreateDevice(IDirectInput8W* self,REFGUID id,IDirectInputDevice8W** out,IUnknown* outer) {
    const auto hr=(Unicode ? create : createA)(self,id,out,outer);
    if(FAILED(hr) || !out || !*out) return hr;
    GUID instance=id;
    bool isKeyboard=id==GUID_SysKeyboard;
    if constexpr(Unicode) {
        DIDEVICEINSTANCEW info{sizeof(info)};
        if(SUCCEEDED((*out)->GetDeviceInfo(&info))) { instance=info.guidInstance; isKeyboard=GET_DIDEVICE_TYPE(info.dwDevType)==DI8DEVTYPE_KEYBOARD; }
    } else {
        DIDEVICEINSTANCEA info{sizeof(info)};
        if(SUCCEEDED(reinterpret_cast<IDirectInputDevice8A*>(*out)->GetDeviceInfo(&info))) { instance=info.guidInstance; isKeyboard=GET_DIDEVICE_TYPE(info.dwDevType)==DI8DEVTYPE_KEYBOARD; }
    }
    std::lock_guard lock(deviceMutex);
    devices[*out]={instance,isKeyboard,Unicode,{}};
    // ANSI and Unicode devices share one implementation, so one set of hooks covers both.
    if(!deviceHooks) {
        auto** table=*reinterpret_cast<void***>(*out);
        deviceHooks=Hook(table[9],reinterpret_cast<void*>(State),reinterpret_cast<void**>(&state))==MH_OK &&
                    Hook(table[10],reinterpret_cast<void*>(Data),reinterpret_cast<void**>(&data))==MH_OK &&
                    Hook(table[11],reinterpret_cast<void*>(Format),reinterpret_cast<void**>(&format))==MH_OK;
    }
    return hr;
}

// Xbox pads: XInput.
using XboxFn=DWORD (WINAPI*)(DWORD,XINPUT_STATE*);
XboxFn xbox{};
DWORD WINAPI Xbox(DWORD slot,XINPUT_STATE* output) {
    const auto result=xbox(slot,output);
    if(result!=ERROR_SUCCESS || !output || !GameCaller(_ReturnAddress())) return result;
    const auto current=policy.load();
    if(current && current->filterXbox) current->filterXbox(slot,output->Gamepad.wButtons);
    return result;
}

Filters installed;
bool InstallHooks() {
    const auto initialized=MH_Initialize();
    if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED) return false;
    auto* user=GetModuleHandleW(L"user32.dll");
    const auto keys=user ? Hook(reinterpret_cast<void*>(GetProcAddress(user,"GetRawInputData")),reinterpret_cast<void*>(RawData),reinterpret_cast<void**>(&rawData)) : MH_ERROR_MODULE_NOT_FOUND;
    const auto async=user ? Hook(reinterpret_cast<void*>(GetProcAddress(user,"GetAsyncKeyState")),reinterpret_cast<void*>(AsyncKey),reinterpret_cast<void**>(&asyncKey)) : MH_ERROR_MODULE_NOT_FOUND;
    const auto key=user ? Hook(reinterpret_cast<void*>(GetProcAddress(user,"GetKeyState")),reinterpret_cast<void*>(KeyState),reinterpret_cast<void**>(&keyState)) : MH_ERROR_MODULE_NOT_FOUND;
    const auto keyboardKeys=user ? Hook(reinterpret_cast<void*>(GetProcAddress(user,"GetKeyboardState")),reinterpret_cast<void*>(KeyboardState),reinterpret_cast<void**>(&keyboardState)) : MH_ERROR_MODULE_NOT_FOUND;

    // DirectInput (#bd820b32): the device reads, shared by ANSI and Unicode devices, filter every
    // device, including those the game opened earlier (Known learns them when first read). Their
    // addresses come from a throwaway keyboard made before any hook, so they don't depend on the
    // factory hooks, which only register new devices early and may fail without harm (in the game
    // they failed where this standalone install succeeds, so each status is logged).
    MH_STATUS reads[3]{MH_UNKNOWN,MH_UNKNOWN,MH_UNKNOWN},factoryW=MH_UNKNOWN,factoryA=MH_UNKNOWN;
    HRESULT probed=E_FAIL;
    if(auto* module=LoadLibraryExW(L"dinput8.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        using Factory=HRESULT (WINAPI*)(HINSTANCE,DWORD,REFIID,void**,IUnknown*);
        const auto factory=reinterpret_cast<Factory>(GetProcAddress(module,"DirectInput8Create"));
        IDirectInput8W* wide{};
        if(factory && SUCCEEDED(probed=factory(GetModuleHandleW(nullptr),DIRECTINPUT_VERSION,IID_IDirectInput8W,reinterpret_cast<void**>(&wide),nullptr))) {
            IDirectInputDevice8W* probe{};
            if(SUCCEEDED(probed=wide->CreateDevice(GUID_SysKeyboard,&probe,nullptr)) && probe) {
                auto** table=*reinterpret_cast<void***>(probe);
                std::lock_guard lock(deviceMutex);
                if(!deviceHooks) {
                    reads[0]=Hook(table[9],reinterpret_cast<void*>(State),reinterpret_cast<void**>(&state));
                    reads[1]=Hook(table[10],reinterpret_cast<void*>(Data),reinterpret_cast<void**>(&data));
                    reads[2]=Hook(table[11],reinterpret_cast<void*>(Format),reinterpret_cast<void**>(&format));
                    deviceHooks=reads[0]==MH_OK && reads[1]==MH_OK && reads[2]==MH_OK;
                }
                probe->Release();
            }
            factoryW=Hook((*reinterpret_cast<void***>(wide))[3],reinterpret_cast<void*>(CreateDevice<true>),reinterpret_cast<void**>(&create));
            wide->Release();
            IDirectInput8A* ansi{};
            if(SUCCEEDED(factory(GetModuleHandleW(nullptr),DIRECTINPUT_VERSION,IID_IDirectInput8A,reinterpret_cast<void**>(&ansi),nullptr))) {
                factoryA=Hook((*reinterpret_cast<void***>(ansi))[3],reinterpret_cast<void*>(CreateDevice<false>),reinterpret_cast<void**>(&createA));
                ansi->Release();
            }
        }
    }

    // GRID 2 links xinput1_3.dll (by ordinal), the same export as XInputGetState.
    MH_STATUS pads=MH_ERROR_MODULE_NOT_FOUND;
    if(auto* library=LoadLibraryExW(L"xinput1_3.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32))
        pads=Hook(reinterpret_cast<void*>(GetProcAddress(library,"XInputGetState")),reinterpret_cast<void*>(Xbox),reinterpret_cast<void**>(&xbox));
    installed={keys==MH_OK && async==MH_OK && key==MH_OK && keyboardKeys==MH_OK && deviceHooks,deviceHooks,pads==MH_OK};
    if(logLine) logLine("Seat input filters: keyboard=%d directinput=%d xinput=%d (raw=%s async=%s key=%s keyboard=%s probe=0x%08lx reads=%s/%s/%s factory=%s/%s xinput=%s)",
        int(installed.keyboard),int(installed.directInput),int(installed.xinput),MH_StatusToString(keys),
        MH_StatusToString(async),MH_StatusToString(key),MH_StatusToString(keyboardKeys),static_cast<unsigned long>(probed),
        MH_StatusToString(reads[0]),MH_StatusToString(reads[1]),MH_StatusToString(reads[2]),MH_StatusToString(factoryW),MH_StatusToString(factoryA),MH_StatusToString(pads));
    return installed.keyboard && installed.directInput && installed.xinput;
}
}

bool Install(Policy next,void (*log)(const char*,...)) {
    static std::once_flag once;
    logLine=log;
    policy.store(std::make_shared<const Policy>(std::move(next)));
    std::call_once(once,[] { ready=InstallHooks(); });
    return ready;
}
bool Ready() { return ready; }
Filters Installed() { return installed; }

bool AttachWindow(HWND window) {
    DWORD process{};
    if(!window || !IsWindow(window) || !GetWindowThreadProcessId(window,&process) || process!=GetCurrentProcessId() || !installed.keyboard)return false;
    std::lock_guard lock(windowMutex);
    if(gameWindow==window)return reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window,GWLP_WNDPROC))==Window;
    if(gameWindow)return false;
    if(!GetWindowLongPtrW(window,GWLP_WNDPROC))return false;
    SetLastError(0);
    const auto previous=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(Window)));
    if(!previous)return false;
    previousWindow=previous;gameWindow=window;return true;
}
void Stop() {
    policy.store(std::make_shared<const Policy>());
    std::lock_guard lock(windowMutex);
    if(gameWindow && IsWindow(gameWindow) && reinterpret_cast<WNDPROC>(GetWindowLongPtrW(gameWindow,GWLP_WNDPROC))==Window)
        SetWindowLongPtrW(gameWindow,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(previousWindow));
    gameWindow=nullptr;
}

}
