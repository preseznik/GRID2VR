#include "ffb_observer.h"
#include "guarded_hooks.h"
#include "pe_imports.h"
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <Xinput.h>
#include <MinHook.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <type_traits>

namespace grid2vr::ffbobserver {
namespace {
// GRID 2 imports XINPUT1_3 by ordinal: 2 XInputGetState, 3 XInputSetState.
constexpr WORD GetStateOrdinal=2,SetStateOrdinal=3;
// IDirectInputDevice8 / IDirectInputEffect vtable slots.
constexpr size_t AcquireSlot=7,CooperativeSlot=13,CreateEffectSlot=18,SendCommandSlot=22;
constexpr size_t SetParametersSlot=6,StartSlot=7,DownloadSlot=10;

using SetState=DWORD (WINAPI*)(DWORD,XINPUT_VIBRATION*);
using GetState=DWORD (WINAPI*)(DWORD,XINPUT_STATE*);
using Cooperative=HRESULT (STDMETHODCALLTYPE*)(void*,HWND,DWORD);
using Acquire=HRESULT (STDMETHODCALLTYPE*)(void*);
using CreateEffect=HRESULT (STDMETHODCALLTYPE*)(void*,REFGUID,LPCDIEFFECT,LPDIRECTINPUTEFFECT*,LPUNKNOWN);
using SendCommand=HRESULT (STDMETHODCALLTYPE*)(void*,DWORD);
using SetParameters=HRESULT (STDMETHODCALLTYPE*)(void*,LPCDIEFFECT,DWORD);
using Start=HRESULT (STDMETHODCALLTYPE*)(void*,DWORD,DWORD);
using Download=HRESULT (STDMETHODCALLTYPE*)(void*);

uintptr_t base{},imageEnd{};
std::filesystem::path folder;
std::atomic<bool> installed{},stopped{},effectsHooked{};
std::mutex mutex;
ULONGLONG lastWrite{};
const char* reason="not_installed";

SetState originalSet{};GetState originalGet{};
void** setSlot{};void** getSlot{};

struct Result {
    std::atomic<uint32_t> calls{},failures{},lastResult{},lastFailure{};
    void Record(HRESULT hr) {
        ++calls;lastResult=static_cast<uint32_t>(hr);
        if(FAILED(hr)) {++failures;lastFailure=static_cast<uint32_t>(hr);}
    }
};
std::atomic<uint32_t> getCalls{},getConnected{},setCalls{},setNonzero{},setFailures{},setOther{},lastSetResult{},peakLeft{},peakRight{};
std::array<std::atomic<uint32_t>,4> setByIndex{};
Result cooperative,acquire,createEffect,sendCommand,setParameters,start,download;
std::atomic<uint32_t> exclusive{},foreground{},background{},lastFlags{},lastCommand{};
std::atomic<uint32_t> deviceHooks{},effectHooks{};

bool FromGame(void* address) {const auto at=reinterpret_cast<uintptr_t>(address);return at>=base && at<imageEnd;}
void Peak(std::atomic<uint32_t>& peak,uint32_t value) {auto current=peak.load();while(value>current && !peak.compare_exchange_weak(current,value)){}}

DWORD WINAPI ObservedSet(DWORD index,XINPUT_VIBRATION* vibration) {
    const auto result=originalSet(index,vibration);
    if(!stopped) {
        ++setCalls;
        if(index<setByIndex.size())++setByIndex[index];else ++setOther;
        if(result!=ERROR_SUCCESS)++setFailures;
        lastSetResult=result;
        XINPUT_VIBRATION value{};
        if(vibration && ReadMemory(vibration,&value,sizeof(value))) {
            if(value.wLeftMotorSpeed || value.wRightMotorSpeed)++setNonzero;
            Peak(peakLeft,value.wLeftMotorSpeed);Peak(peakRight,value.wRightMotorSpeed);
        }
    }
    return result;
}
DWORD WINAPI ObservedGet(DWORD index,XINPUT_STATE* state) {
    const auto result=originalGet(index,state);
    if(!stopped) {++getCalls;getConnected+=result==ERROR_SUCCESS;}
    return result;
}

// The ANSI and Unicode device interfaces may have distinct implementations.
struct DeviceOriginals {Cooperative cooperative{};Acquire acquire{};CreateEffect createEffect{};SendCommand sendCommand{};};
std::array<DeviceOriginals,2> devices{};
SetParameters originalSetParameters{};Start originalStart{};Download originalDownload{};

HRESULT STDMETHODCALLTYPE ObservedSetParameters(void* self,LPCDIEFFECT effect,DWORD flags) {
    const auto hr=originalSetParameters(self,effect,flags);
    if(!stopped && FromGame(_ReturnAddress()))setParameters.Record(hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE ObservedStart(void* self,DWORD iterations,DWORD flags) {
    const auto hr=originalStart(self,iterations,flags);
    if(!stopped && FromGame(_ReturnAddress()))start.Record(hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE ObservedDownload(void* self) {
    const auto hr=originalDownload(self);
    if(!stopped && FromGame(_ReturnAddress()))download.Record(hr);
    return hr;
}
bool Hook(void* target,void* detour,void** original) {
    if(!target)return false;
    return MH_CreateHook(target,detour,original)==MH_OK && MH_EnableHook(target)==MH_OK;
}
void HookEffect(LPDIRECTINPUTEFFECT effect) {
    if(effectsHooked.exchange(true))return;
    void** table{};
    if(!effect || !ReadMemory(effect,&table,sizeof(table)) || !table)return;
    std::array<void*,DownloadSlot+1> methods{};
    if(!ReadMemory(table,methods.data(),sizeof(methods)))return;
    effectHooks+=Hook(methods[SetParametersSlot],reinterpret_cast<void*>(ObservedSetParameters),reinterpret_cast<void**>(&originalSetParameters));
    effectHooks+=Hook(methods[StartSlot],reinterpret_cast<void*>(ObservedStart),reinterpret_cast<void**>(&originalStart));
    effectHooks+=Hook(methods[DownloadSlot],reinterpret_cast<void*>(ObservedDownload),reinterpret_cast<void**>(&originalDownload));
}

template<size_t V> HRESULT STDMETHODCALLTYPE ObservedCooperative(void* self,HWND window,DWORD flags) {
    const auto hr=devices[V].cooperative(self,window,flags);
    if(!stopped && FromGame(_ReturnAddress())) {
        cooperative.Record(hr);lastFlags=flags;
        exclusive+=(flags&DISCL_EXCLUSIVE)!=0;foreground+=(flags&DISCL_FOREGROUND)!=0;background+=(flags&DISCL_BACKGROUND)!=0;
    }
    return hr;
}
template<size_t V> HRESULT STDMETHODCALLTYPE ObservedAcquire(void* self) {
    const auto hr=devices[V].acquire(self);
    if(!stopped && FromGame(_ReturnAddress()))acquire.Record(hr);
    return hr;
}
template<size_t V> HRESULT STDMETHODCALLTYPE ObservedCreateEffect(void* self,REFGUID kind,LPCDIEFFECT effect,LPDIRECTINPUTEFFECT* output,LPUNKNOWN outer) {
    const auto hr=devices[V].createEffect(self,kind,effect,output,outer);
    if(!stopped && FromGame(_ReturnAddress())) {
        createEffect.Record(hr);
        LPDIRECTINPUTEFFECT created{};
        if(SUCCEEDED(hr) && output && ReadMemory(output,&created,sizeof(created)))HookEffect(created);
    }
    return hr;
}
template<size_t V> HRESULT STDMETHODCALLTYPE ObservedSendCommand(void* self,DWORD command) {
    const auto hr=devices[V].sendCommand(self,command);
    if(!stopped && FromGame(_ReturnAddress())) {sendCommand.Record(hr);lastCommand=command;}
    return hr;
}
template<size_t V> void HookDevice(void** table,std::array<void*,4>& hooked) {
    std::array<void*,SendCommandSlot+1> methods{};
    if(!ReadMemory(table,methods.data(),sizeof(methods)))return;
    const std::array<void*,4> targets{methods[CooperativeSlot],methods[AcquireSlot],methods[CreateEffectSlot],methods[SendCommandSlot]};
    const std::array<void*,4> detours{reinterpret_cast<void*>(ObservedCooperative<V>),reinterpret_cast<void*>(ObservedAcquire<V>),
        reinterpret_cast<void*>(ObservedCreateEffect<V>),reinterpret_cast<void*>(ObservedSendCommand<V>)};
    const std::array<void**,4> originals{reinterpret_cast<void**>(&devices[V].cooperative),reinterpret_cast<void**>(&devices[V].acquire),
        reinterpret_cast<void**>(&devices[V].createEffect),reinterpret_cast<void**>(&devices[V].sendCommand)};
    for(size_t i=0;i<targets.size();++i) {
        // A shared ANSI/Unicode implementation is observed once.
        if(targets[i]==hooked[i])continue;
        if(Hook(targets[i],detours[i],originals[i])) {hooked[i]=targets[i];++deviceHooks;}
    }
}
// A private keyboard device exposes dinput8's shared device implementation.
// It is released immediately; only the game's own calls are counted.
void HookDirectInput() {
    std::array<void*,4> hooked{};
    const auto module=GetModuleHandleW(nullptr);
    const auto hookVariant=[&](REFIID iid,auto variant) {
        constexpr size_t V=decltype(variant)::value;
        void* input{};
        if(FAILED(DirectInput8Create(module,DIRECTINPUT_VERSION,iid,&input,nullptr)) || !input)return;
        if constexpr(V==0) {
            auto* object=static_cast<IDirectInput8A*>(input);LPDIRECTINPUTDEVICE8A device{};
            if(SUCCEEDED(object->CreateDevice(GUID_SysKeyboard,&device,nullptr)) && device) {
                HookDevice<0>(*reinterpret_cast<void***>(device),hooked);device->Release();
            }
            object->Release();
        } else {
            auto* object=static_cast<IDirectInput8W*>(input);LPDIRECTINPUTDEVICE8W device{};
            if(SUCCEEDED(object->CreateDevice(GUID_SysKeyboard,&device,nullptr)) && device) {
                HookDevice<1>(*reinterpret_cast<void***>(device),hooked);device->Release();
            }
            object->Release();
        }
    };
    hookVariant(IID_IDirectInput8A,std::integral_constant<size_t,0>{});
    hookVariant(IID_IDirectInput8W,std::integral_constant<size_t,1>{});
}

void WriteResult(std::ostream& file,const char* name,const Result& r) {
    char text[192]{};
    sprintf_s(text,"\"%s\":{\"calls\":%u,\"failures\":%u,\"lastResult\":\"0x%08x\",\"lastFailure\":\"0x%08x\"}",
        name,r.calls.load(),r.failures.load(),r.lastResult.load(),r.lastFailure.load());
    file<<text;
}
void WriteStatus() {
    std::lock_guard lock(mutex);
    std::ofstream file(folder/L"ffb-observer.json",std::ios::trunc);
    char text[512]{};
    sprintf_s(text,"{\"schema\":1,\"observeOnly\":true,\"installed\":%s,\"stopped\":%s,\"reason\":\"%s\","
        "\"xinput\":{\"setPatched\":%s,\"getPatched\":%s,\"getCalls\":%u,\"getConnected\":%u,\"setCalls\":%u,"
        "\"setByIndex\":[%u,%u,%u,%u],\"setOther\":%u,\"setNonzero\":%u,\"setFailures\":%u,\"lastSetResult\":%u,"
        "\"peakLeft\":%u,\"peakRight\":%u},",
        installed?"true":"false",stopped?"true":"false",reason,setSlot?"true":"false",getSlot?"true":"false",
        getCalls.load(),getConnected.load(),setCalls.load(),setByIndex[0].load(),setByIndex[1].load(),setByIndex[2].load(),
        setByIndex[3].load(),setOther.load(),setNonzero.load(),setFailures.load(),lastSetResult.load(),peakLeft.load(),peakRight.load());
    file<<text;
    sprintf_s(text,"\"directInput\":{\"deviceHooks\":%u,\"effectHooks\":%u,\"exclusiveRequests\":%u,\"foregroundRequests\":%u,"
        "\"backgroundRequests\":%u,\"lastCooperativeFlags\":%u,\"lastCommand\":%u,",
        deviceHooks.load(),effectHooks.load(),exclusive.load(),foreground.load(),background.load(),lastFlags.load(),lastCommand.load());
    file<<text;
    WriteResult(file,"cooperativeLevel",cooperative);file<<',';WriteResult(file,"acquire",acquire);file<<',';
    WriteResult(file,"createEffect",createEffect);file<<',';WriteResult(file,"sendCommand",sendCommand);file<<',';
    WriteResult(file,"effectSetParameters",setParameters);file<<',';WriteResult(file,"effectStart",start);file<<',';
    WriteResult(file,"effectDownload",download);
    file<<"}}\n";
}
}

void Install(uintptr_t gameBase,const std::filesystem::path& output) {
    static bool attempted{};if(attempted)return;attempted=true;
    base=gameBase;folder=output;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(base+static_cast<uint32_t>(dos->e_lfanew));
    imageEnd=base+nt->OptionalHeader.SizeOfImage;
    // Store the original before publishing the detour: calls may arrive at once.
    if(auto* slot=FindImportSlot(base,"XINPUT1_3.dll",SetStateOrdinal,nullptr)) {
        void* current{};
        if(ReadMemory(slot,&current,sizeof(current)) && current) {
            originalSet=reinterpret_cast<SetState>(current);
            if(SwapGuardedPointer(slot,current,reinterpret_cast<void*>(ObservedSet)))setSlot=slot;
        }
    }
    if(auto* slot=FindImportSlot(base,"XINPUT1_3.dll",GetStateOrdinal,nullptr)) {
        void* current{};
        if(ReadMemory(slot,&current,sizeof(current)) && current) {
            originalGet=reinterpret_cast<GetState>(current);
            if(SwapGuardedPointer(slot,current,reinterpret_cast<void*>(ObservedGet)))getSlot=slot;
        }
    }
    HookDirectInput();
    installed=true;
    reason=setSlot && deviceHooks?"observing":setSlot?"observing_xinput_only":deviceHooks?"observing_directinput_only":"no_observers";
    WriteStatus();lastWrite=GetTickCount64();
}
void Tick() {
    if(!installed || stopped)return;
    const auto now=GetTickCount64();
    if(now-lastWrite<2000)return;
    lastWrite=now;WriteStatus();
}
void Stop() {
    if(!installed || stopped.exchange(true))return;
    // Restore the game's import slots; the DirectInput detours become pure
    // pass-through and stay in place so no in-flight call loses its trampoline.
    if(setSlot)SwapGuardedPointer(setSlot,reinterpret_cast<void*>(ObservedSet),reinterpret_cast<void*>(originalSet));
    if(getSlot)SwapGuardedPointer(getSlot,reinterpret_cast<void*>(ObservedGet),reinterpret_cast<void*>(originalGet));
    reason="stopped";WriteStatus();
}
}
