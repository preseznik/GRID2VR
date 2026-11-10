#include "ffb_observer.h"
#include "guarded_hooks.h"
#include "pe_imports.h"
#include "force_feedback_shaping.h"
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <Xinput.h>
#include <MinHook.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
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
using DirectInputCreate=HRESULT (WINAPI*)(HINSTANCE,DWORD,REFIID,LPVOID*,LPUNKNOWN);

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
DirectInputCreate originalCreate{};void** createSlot{};
std::atomic<uint32_t> gameCreates{};std::atomic<bool> directInputHooked{};

// Effect kinds and peak parameters, to map launcher Strength / Weight /
// Vibration onto what the game actually sends.
const GUID* const kinds[]{&GUID_ConstantForce,&GUID_RampForce,&GUID_Square,&GUID_Sine,&GUID_Triangle,&GUID_SawtoothUp,
    &GUID_SawtoothDown,&GUID_Spring,&GUID_Damper,&GUID_Inertia,&GUID_Friction,&GUID_CustomForce};
constexpr const char* kindNames[]{"constant","ramp","square","sine","triangle","sawtoothUp","sawtoothDown","spring","damper",
    "inertia","friction","custom","other"};
constexpr size_t kindCount=std::size(kindNames),otherKind=kindCount-1;
struct KindStats {std::atomic<uint32_t> created{},updates{},peakMagnitude{},peakGain{};};
std::array<KindStats,kindCount> kindStats{};
std::array<std::atomic<void*>,32> effectPointers{};
std::array<std::atomic<uint8_t>,32> effectKinds{};

bool FromGame(void* address) {const auto at=reinterpret_cast<uintptr_t>(address);return at>=base && at<imageEnd;}
void Peak(std::atomic<uint32_t>& peak,uint32_t value) {auto current=peak.load();while(value>current && !peak.compare_exchange_weak(current,value)){}}

size_t KindOf(const GUID& kind) {
    for(size_t i=0;i<std::size(kinds);++i) if(IsEqualGUID(kind,*kinds[i])) return i;
    return otherKind;
}
void Remember(void* effect,size_t kind) {
    for(size_t i=0;i<effectPointers.size();++i) {
        void* expected{};
        if(effectPointers[i].load()==effect || effectPointers[i].compare_exchange_strong(expected,effect)) {effectKinds[i]=static_cast<uint8_t>(kind);return;}
    }
}
size_t Recall(void* effect) {
    for(size_t i=0;i<effectPointers.size();++i) if(effectPointers[i].load()==effect) return effectKinds[i];
    return otherKind;
}
uint32_t Magnitude(LONG value) {return static_cast<uint32_t>(std::labs(value));}
// Reads the game's DIEFFECT (DX5 or DX6 size) and its type-specific block.
void Measure(size_t kind,LPCDIEFFECT effect,DWORD flags) {
    DIEFFECT value{};
    if(!effect || !ReadMemory(effect,&value.dwSize,sizeof(value.dwSize)) || value.dwSize<sizeof(DIEFFECT_DX5)) return;
    if(!ReadMemory(effect,&value,std::min<size_t>(sizeof(value),value.dwSize))) return;
    auto& stats=kindStats[kind];
    if(flags&DIEP_GAIN) Peak(stats.peakGain,value.dwGain);
    if(!(flags&DIEP_TYPESPECIFICPARAMS) || !value.lpvTypeSpecificParams) return;
    uint32_t magnitude{};
    if(kind==0) {DICONSTANTFORCE c{};if(value.cbTypeSpecificParams>=sizeof(c) && ReadMemory(value.lpvTypeSpecificParams,&c,sizeof(c))) magnitude=Magnitude(c.lMagnitude);}
    else if(kind==1) {DIRAMPFORCE r{};if(value.cbTypeSpecificParams>=sizeof(r) && ReadMemory(value.lpvTypeSpecificParams,&r,sizeof(r))) magnitude=std::max(Magnitude(r.lStart),Magnitude(r.lEnd));}
    else if(kind<=6) {DIPERIODIC p{};if(value.cbTypeSpecificParams>=sizeof(p) && ReadMemory(value.lpvTypeSpecificParams,&p,sizeof(p))) magnitude=p.dwMagnitude;}
    else if(kind<=10) {DICONDITION c{};if(value.cbTypeSpecificParams>=sizeof(c) && ReadMemory(value.lpvTypeSpecificParams,&c,sizeof(c))) magnitude=std::max(Magnitude(c.lPositiveCoefficient),Magnitude(c.lNegativeCoefficient));}
    Peak(stats.peakMagnitude,magnitude);
}

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
std::array<DeviceOriginals,4> devices{};
std::array<void*,16> hookedTargets{};std::atomic<size_t> hookedCount{};
bool AlreadyHooked(void* target) {for(size_t i=0;i<hookedCount;++i) if(hookedTargets[i]==target) return true;return false;}

// Calls that did not return into the game image: count and last caller.
struct Foreign {std::atomic<uint32_t> calls{};std::atomic<uintptr_t> last{};void Record(void* at){++calls;last=reinterpret_cast<uintptr_t>(at);}};
Foreign foreignCooperative,foreignAcquire,foreignCreateEffect,foreignSetParameters;
// The game's own DirectInput objects and devices, as created.
using CreateDevice=HRESULT (STDMETHODCALLTYPE*)(void*,REFGUID,void**,LPUNKNOWN);
constexpr size_t CreateDeviceSlot=3;
std::array<CreateDevice,2> originalCreateDevice{};std::array<void*,2> createDeviceTarget{};
struct GameDevice {uint32_t guid{};uintptr_t table{},cooperative{},acquire{},createEffect{};bool shared{};};
std::array<GameDevice,8> gameDevices{};std::atomic<uint32_t> gameDeviceCount{};
SetParameters originalSetParameters{};Start originalStart{};Download originalDownload{};

// Launcher Force Feedback: the game's constant-force magnitude is shaped on a
// private copy of its DIEFFECT; the game's own structures are never written.
std::mutex shapeMutex;
forcefeedback::Shaper shaper;
forcefeedback::Settings shapeSettings;
std::atomic<bool> shapeConfigured{},shapeActive{};
void* shapedEffect{};LARGE_INTEGER shapeLast{},shapeFrequency{};
std::atomic<uint32_t> shapedCalls{},shapeClipped{},shapePeakIn{},shapePeakOut{},shapeSkipped{};
bool Shape(void* self,LPCDIEFFECT effect,DIEFFECT& copy,DICONSTANTFORCE& force) {
    DWORD size{};
    if(!effect || !ReadMemory(effect,&size,sizeof(size)) || size<sizeof(DIEFFECT_DX5) || size>sizeof(DIEFFECT) || !ReadMemory(effect,&copy,size) ||
       copy.cbTypeSpecificParams!=sizeof(DICONSTANTFORCE) || !copy.lpvTypeSpecificParams ||
       !ReadMemory(copy.lpvTypeSpecificParams,&force,sizeof(force))) {++shapeSkipped;return false;}
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);
    long shaped{};
    {
        std::lock_guard lock(shapeMutex);
        double seconds=0;
        if(self==shapedEffect && shapeFrequency.QuadPart)seconds=static_cast<double>(now.QuadPart-shapeLast.QuadPart)/static_cast<double>(shapeFrequency.QuadPart);
        else shaper.Reset();
        shapedEffect=self;shapeLast=now;
        shaped=shaper.Apply(force.lMagnitude,seconds);
    }
    Peak(shapePeakIn,Magnitude(force.lMagnitude));Peak(shapePeakOut,Magnitude(shaped));
    if(std::labs(shaped)==forcefeedback::nominalMax)++shapeClipped;
    force.lMagnitude=shaped;copy.lpvTypeSpecificParams=&force;++shapedCalls;
    return true;
}

HRESULT STDMETHODCALLTYPE ObservedSetParameters(void* self,LPCDIEFFECT effect,DWORD flags) {
    DIEFFECT copy{};DICONSTANTFORCE force{};LPCDIEFFECT sent=effect;
    if(shapeActive && !stopped && FromGame(_ReturnAddress()) && (flags&DIEP_TYPESPECIFICPARAMS) && Recall(self)==0 && Shape(self,effect,copy,force))sent=&copy;
    const auto hr=originalSetParameters(self,sent,flags);
    if(!stopped && !FromGame(_ReturnAddress()))foreignSetParameters.Record(_ReturnAddress());
    if(!stopped && FromGame(_ReturnAddress())) {
        setParameters.Record(hr);
        const auto kind=Recall(self);++kindStats[kind].updates;Measure(kind,effect,flags);
    }
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
    if(!stopped && !FromGame(_ReturnAddress()))foreignCooperative.Record(_ReturnAddress());
    if(!stopped && FromGame(_ReturnAddress())) {
        cooperative.Record(hr);lastFlags=flags;
        exclusive+=(flags&DISCL_EXCLUSIVE)!=0;foreground+=(flags&DISCL_FOREGROUND)!=0;background+=(flags&DISCL_BACKGROUND)!=0;
    }
    return hr;
}
template<size_t V> HRESULT STDMETHODCALLTYPE ObservedAcquire(void* self) {
    const auto hr=devices[V].acquire(self);
    if(!stopped && !FromGame(_ReturnAddress()))foreignAcquire.Record(_ReturnAddress());
    if(!stopped && FromGame(_ReturnAddress()))acquire.Record(hr);
    return hr;
}
template<size_t V> HRESULT STDMETHODCALLTYPE ObservedCreateEffect(void* self,REFGUID kind,LPCDIEFFECT effect,LPDIRECTINPUTEFFECT* output,LPUNKNOWN outer) {
    const auto hr=devices[V].createEffect(self,kind,effect,output,outer);
    if(!stopped && !FromGame(_ReturnAddress()))foreignCreateEffect.Record(_ReturnAddress());
    if(!stopped && FromGame(_ReturnAddress())) {
        createEffect.Record(hr);
        LPDIRECTINPUTEFFECT created{};
        if(SUCCEEDED(hr) && output && ReadMemory(output,&created,sizeof(created)) && created) {
            GUID type{};const auto k=ReadMemory(&kind,&type,sizeof(type))?KindOf(type):otherKind;
            ++kindStats[k].created;Remember(created,k);Measure(k,effect,DIEP_ALLPARAMS);
            HookEffect(created);
        }
    }
    return hr;
}
template<size_t V> HRESULT STDMETHODCALLTYPE ObservedSendCommand(void* self,DWORD command) {
    const auto hr=devices[V].sendCommand(self,command);
    if(!stopped && FromGame(_ReturnAddress())) {sendCommand.Record(hr);lastCommand=command;}
    return hr;
}
template<size_t V> void HookDevice(void** table) {
    std::array<void*,SendCommandSlot+1> methods{};
    if(!ReadMemory(table,methods.data(),sizeof(methods)))return;
    const std::array<void*,4> targets{methods[CooperativeSlot],methods[AcquireSlot],methods[CreateEffectSlot],methods[SendCommandSlot]};
    const std::array<void*,4> detours{reinterpret_cast<void*>(ObservedCooperative<V>),reinterpret_cast<void*>(ObservedAcquire<V>),
        reinterpret_cast<void*>(ObservedCreateEffect<V>),reinterpret_cast<void*>(ObservedSendCommand<V>)};
    const std::array<void**,4> originals{reinterpret_cast<void**>(&devices[V].cooperative),reinterpret_cast<void**>(&devices[V].acquire),
        reinterpret_cast<void**>(&devices[V].createEffect),reinterpret_cast<void**>(&devices[V].sendCommand)};
    for(size_t i=0;i<targets.size();++i) {
        // A shared implementation (ANSI/Unicode, keyboard/wheel) is observed once.
        if(!targets[i] || AlreadyHooked(targets[i]) || hookedCount>=hookedTargets.size())continue;
        if(Hook(targets[i],detours[i],originals[i])) {hookedTargets[hookedCount++]=targets[i];++deviceHooks;}
    }
}
// A private keyboard device exposes dinput8's shared device implementation.
// It is released immediately; only the game's own calls are counted.
void HookDirectInput() {
    const auto module=GetModuleHandleW(nullptr);
    const auto hookVariant=[&](REFIID iid,auto variant) {
        constexpr size_t V=decltype(variant)::value;
        void* input{};
        if(FAILED(DirectInput8Create(module,DIRECTINPUT_VERSION,iid,&input,nullptr)) || !input)return;
        if constexpr(V==0) {
            auto* object=static_cast<IDirectInput8A*>(input);LPDIRECTINPUTDEVICE8A device{};
            if(SUCCEEDED(object->CreateDevice(GUID_SysKeyboard,&device,nullptr)) && device) {
                HookDevice<0>(*reinterpret_cast<void***>(device));device->Release();
            }
            object->Release();
        } else {
            auto* object=static_cast<IDirectInput8W*>(input);LPDIRECTINPUTDEVICE8W device{};
            if(SUCCEEDED(object->CreateDevice(GUID_SysKeyboard,&device,nullptr)) && device) {
                HookDevice<1>(*reinterpret_cast<void***>(device));device->Release();
            }
            object->Release();
        }
    };
    hookVariant(IID_IDirectInput8A,std::integral_constant<size_t,0>{});
    hookVariant(IID_IDirectInput8W,std::integral_constant<size_t,1>{});
}
void HookDirectInputOnce() {
    if(directInputHooked.exchange(true))return;
    const auto status=MH_Initialize();
    if(status!=MH_OK && status!=MH_ERROR_ALREADY_INITIALIZED) {directInputHooked=false;return;}
    HookDirectInput();
}
// The game's own CreateDevice: record each device's method table and, if its
// methods differ from the private keyboard's, observe them too (V+2).
template<size_t V> HRESULT STDMETHODCALLTYPE ObservedCreateDevice(void* self,REFGUID guid,void** output,LPUNKNOWN outer) {
    const auto hr=originalCreateDevice[V](self,guid,output,outer);
    if(!stopped && FromGame(_ReturnAddress()) && SUCCEEDED(hr) && output) {
        void* device{};void** table{};std::array<void*,SendCommandSlot+1> methods{};GUID id{};
        if(ReadMemory(output,&device,sizeof(device)) && device && ReadMemory(device,&table,sizeof(table)) && table &&
           ReadMemory(table,methods.data(),sizeof(methods))) {
            const bool shared=AlreadyHooked(methods[CooperativeSlot]) && AlreadyHooked(methods[AcquireSlot]) && AlreadyHooked(methods[CreateEffectSlot]);
            const auto index=gameDeviceCount++;
            if(index<gameDevices.size()) {
                auto& d=gameDevices[index];d.guid=ReadMemory(&guid,&id,sizeof(id))?id.Data1:0;d.table=reinterpret_cast<uintptr_t>(table);
                d.cooperative=reinterpret_cast<uintptr_t>(methods[CooperativeSlot]);d.acquire=reinterpret_cast<uintptr_t>(methods[AcquireSlot]);
                d.createEffect=reinterpret_cast<uintptr_t>(methods[CreateEffectSlot]);d.shared=shared;
            }
            if(!shared)HookDevice<V+2>(table);
        }
    }
    return hr;
}
void HookGameObject(REFIID iid,void* object) {
    const size_t v=IsEqualIID(iid,IID_IDirectInput8W)?1:0;
    void** table{};void* target{};
    if(!object || !ReadMemory(object,&table,sizeof(table)) || !table || !ReadMemory(table+CreateDeviceSlot,&target,sizeof(target)) || !target)return;
    if(createDeviceTarget[0]==target || createDeviceTarget[1]==target)return;
    const auto detour=v?reinterpret_cast<void*>(ObservedCreateDevice<1>):reinterpret_cast<void*>(ObservedCreateDevice<0>);
    if(Hook(target,detour,reinterpret_cast<void**>(&originalCreateDevice[v])))createDeviceTarget[v]=target;
}
// Pass-through: the game's own DirectInput objects are returned unchanged.
// Our device hooks use private DirectInput objects created through this
// module's import, not the game's slot.
HRESULT WINAPI ObservedCreate(HINSTANCE instance,DWORD version,REFIID iid,LPVOID* output,LPUNKNOWN outer) {
    const auto hr=originalCreate(instance,version,iid,output,outer);
    if(!stopped && FromGame(_ReturnAddress())) {
        ++gameCreates;
        void* object{};GUID id{};
        if(SUCCEEDED(hr)) {
            HookDirectInputOnce();
            if(output && ReadMemory(output,&object,sizeof(object)) && ReadMemory(&iid,&id,sizeof(id)))HookGameObject(id,object);
        }
    }
    return hr;
}
void SetImage(uintptr_t image) {
    base=image;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(base+static_cast<uint32_t>(dos->e_lfanew));
    imageEnd=base+nt->OptionalHeader.SizeOfImage;
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
    sprintf_s(text,"{\"schema\":1,\"observeOnly\":%s,\"installed\":%s,\"stopped\":%s,\"reason\":\"%s\","
        "\"xinput\":{\"setPatched\":%s,\"getPatched\":%s,\"getCalls\":%u,\"getConnected\":%u,\"setCalls\":%u,"
        "\"setByIndex\":[%u,%u,%u,%u],\"setOther\":%u,\"setNonzero\":%u,\"setFailures\":%u,\"lastSetResult\":%u,"
        "\"peakLeft\":%u,\"peakRight\":%u},",
        shapeActive?"false":"true",installed?"true":"false",stopped?"true":"false",reason,setSlot?"true":"false",getSlot?"true":"false",
        getCalls.load(),getConnected.load(),setCalls.load(),setByIndex[0].load(),setByIndex[1].load(),setByIndex[2].load(),
        setByIndex[3].load(),setOther.load(),setNonzero.load(),setFailures.load(),lastSetResult.load(),peakLeft.load(),peakRight.load());
    file<<text;
    sprintf_s(text,"\"directInput\":{\"deviceHooks\":%u,\"effectHooks\":%u,\"exclusiveRequests\":%u,\"foregroundRequests\":%u,"
        "\"backgroundRequests\":%u,\"lastCooperativeFlags\":%u,\"lastCommand\":%u,\"createSlot\":%s,\"gameCreates\":%u,",
        deviceHooks.load(),effectHooks.load(),exclusive.load(),foreground.load(),background.load(),lastFlags.load(),lastCommand.load(),
        createSlot?"true":"false",gameCreates.load());
    file<<text<<"\"kinds\":{";
    for(size_t i=0,written=0;i<kindCount;++i) {
        const auto& k=kindStats[i];
        if(!k.created && !k.updates)continue;
        if(written++)file<<',';
        sprintf_s(text,"\"%s\":{\"created\":%u,\"updates\":%u,\"peakMagnitude\":%u,\"peakGain\":%u}",
            kindNames[i],k.created.load(),k.updates.load(),k.peakMagnitude.load(),k.peakGain.load());
        file<<text;
    }
    file<<"},";
    const auto module=[](uintptr_t at) {
        std::string name="none";HMODULE m{};wchar_t path[MAX_PATH]{};
        if(at && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(at),&m) &&
           GetModuleFileNameW(m,path,MAX_PATH)) {
            const wchar_t* leaf=wcsrchr(path,L'\\');leaf=leaf?leaf+1:path;
            name.clear();for(;*leaf;++leaf)name+=static_cast<char>(*leaf<128?*leaf:'?');
            char offset[24]{};sprintf_s(offset,"+0x%x",static_cast<unsigned>(at-reinterpret_cast<uintptr_t>(m)));name+=offset;
        } else if(at) {char raw[24]{};sprintf_s(raw,"0x%08x",static_cast<unsigned>(at));name=raw;}
        return name;
    };
    const auto foreign=[&](const char* name,const Foreign& f) {
        file<<"\""<<name<<"\":{\"calls\":"<<f.calls.load()<<",\"last\":\""<<module(f.last.load())<<"\"}";
    };
    file<<"\"foreign\":{";foreign("cooperativeLevel",foreignCooperative);file<<',';foreign("acquire",foreignAcquire);file<<',';
    foreign("createEffect",foreignCreateEffect);file<<',';foreign("setParameters",foreignSetParameters);
    file<<"},\"createDeviceHooked\":["<<(createDeviceTarget[0]?"true":"false")<<','<<(createDeviceTarget[1]?"true":"false")<<"],\"gameDevices\":[";
    for(uint32_t i=0;i<std::min<uint32_t>(gameDeviceCount,static_cast<uint32_t>(gameDevices.size()));++i) {
        const auto& d=gameDevices[i];if(i)file<<',';
        sprintf_s(text,"{\"guidData1\":\"0x%08x\",\"shared\":%s,",d.guid,d.shared?"true":"false");file<<text;
        file<<"\"table\":\""<<module(d.table)<<"\",\"cooperative\":\""<<module(d.cooperative)<<"\",\"acquire\":\""<<module(d.acquire)
            <<"\",\"createEffect\":\""<<module(d.createEffect)<<"\"}";
    }
    file<<"],\"gameDeviceCount\":"<<gameDeviceCount.load()<<',';
    {
        std::lock_guard shapeLock(shapeMutex);
        sprintf_s(text,"\"shaping\":{\"configured\":%s,\"active\":%s,\"strength\":%d,\"weight\":%d,\"effects\":%d,\"splitHz\":%g,"
            "\"shaped\":%u,\"skipped\":%u,\"clipped\":%u,\"peakIn\":%u,\"peakOut\":%u},",
            shapeConfigured?"true":"false",shapeActive?"true":"false",shapeSettings.strength,shapeSettings.weight,shapeSettings.effects,forcefeedback::splitHz,
            shapedCalls.load(),shapeSkipped.load(),shapeClipped.load(),shapePeakIn.load(),shapePeakOut.load());
    }
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
    folder=output;
    if(base!=gameBase)SetImage(gameBase);
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
    HookDirectInputOnce();
    installed=true;
    reason=setSlot && deviceHooks?"observing":setSlot?"observing_xinput_only":deviceHooks?"observing_directinput_only":"no_observers";
    WriteStatus();lastWrite=GetTickCount64();
}
void ConfigureShaping(const forcefeedback::Settings& value) {
    if(!forcefeedback::ValidSettings(value))return;
    {
        std::lock_guard lock(shapeMutex);
        shapeSettings=value;shaper=forcefeedback::Shaper(value);shapedEffect=nullptr;QueryPerformanceFrequency(&shapeFrequency);
    }
    shapeConfigured=true;shapeActive=!(value==forcefeedback::Settings{});
    if(installed)WriteStatus();
}
bool AttachEarly(uintptr_t image,uint32_t imageSize,uint32_t slotRva) {
    static bool attempted{};if(attempted || !image)return false;attempted=true;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(image+static_cast<uint32_t>(dos->e_lfanew));
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE || nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage!=imageSize)return false;
    auto* slot=FindImportSlot(image,"DINPUT8.dll",0,"DirectInput8Create");
    if(!slot || reinterpret_cast<uintptr_t>(slot)!=image+slotRva)return false;
    SetImage(image);
    void* current{};
    if(!ReadMemory(slot,&current,sizeof(current)) || !current)return false;
    originalCreate=reinterpret_cast<DirectInputCreate>(current);
    if(!SwapGuardedPointer(slot,current,reinterpret_cast<void*>(ObservedCreate)))return false;
    createSlot=slot;return true;
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
    if(createSlot)SwapGuardedPointer(createSlot,reinterpret_cast<void*>(ObservedCreate),reinterpret_cast<void*>(originalCreate));
    reason="stopped";WriteStatus();
}
}
