#define DIRECTINPUT_VERSION 0x0800
#include "driving_controls_input.h"
#include "driving_controls_input_model.h"
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include "../tools/driving-input/axes.h"

namespace grid2vr::drivingcontrols {
namespace {
bool ReadGuid(std::string_view text,GUID& guid) {
    if(!Guid(text))return false;
    const auto part=[&](size_t start,size_t size){unsigned value{};std::from_chars(text.data()+start,text.data()+start+size,value,16);return value;};
    guid.Data1=part(1,8);guid.Data2=static_cast<WORD>(part(10,4));guid.Data3=static_cast<WORD>(part(15,4));
    guid.Data4[0]=static_cast<BYTE>(part(20,2));guid.Data4[1]=static_cast<BYTE>(part(22,2));
    for(unsigned i=2;i<8;++i)guid.Data4[i]=static_cast<BYTE>(part(25+(i-2)*2,2));
    return true;
}
struct Snapshot {
    bool connected{};
    unsigned axes{};
    std::array<float,8> values{};
    std::array<uint8_t,256> buttons{};
    std::array<uint32_t,4> hats{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
};
using XInputRead=DWORD(WINAPI*)(DWORD,XINPUT_STATE*);
struct DeviceReader {
    enum class Kind { Keyboard,Xbox,Direct } kind{};
    std::string id,family;
    GUID instance{},product{};
    unsigned slot{};
    IDirectInputDevice8W* direct{};
    HWND window{};
    AxisLayout layout;
    ULONGLONG retryAt{};
    const char* status="waiting_for_device";
    int32_t error{};
    Snapshot sample;
    std::array<bool,256> keys{};
    ~DeviceReader(){Close();}
    void Close(){if(direct){direct->Unacquire();direct->Release();direct=nullptr;}window=nullptr;}
    bool Open(IDirectInput8W* root,HWND owner,ULONGLONG now) {
        if(direct && window!=owner){Close();retryAt=0;}
        if(direct)return true;
        if(!root){status="directinput_unavailable";return false;}
        if(now<retryAt)return false;
        retryAt=now+1000;
        auto result=root->CreateDevice(instance,&direct,nullptr);
        if(FAILED(result)){status="device_open_failed";error=result;return false;}
        DIDEVICEINSTANCEW identity{};identity.dwSize=sizeof(identity);
        result=direct->GetDeviceInfo(&identity);
        if(FAILED(result)){status="device_identity_failed";error=result;Close();return false;}
        if(!IsEqualGUID(identity.guidInstance,instance)){status="device_instance_mismatch";error=0;Close();return false;}
        if(!IsEqualGUID(identity.guidProduct,product)){status="device_family_mismatch";error=0;Close();return false;}
        result=direct->SetDataFormat(&c_dfDIJoystick2);
        if(FAILED(result)){status="device_format_failed";error=result;Close();return false;}
        result=direct->SetCooperativeLevel(owner,DISCL_NONEXCLUSIVE|DISCL_FOREGROUND);
        if(FAILED(result)){status="device_cooperative_failed";error=result;Close();return false;}
        layout=ReadAxes(direct);window=owner;
        result=direct->Acquire();
        if(FAILED(result)){status="device_acquire_failed";error=result;Close();return false;}
        retryAt=0;return true;
    }
    void Read(IDirectInput8W* root,XInputRead xbox,HWND owner,ULONGLONG now) {
        sample={};
        if(kind==Kind::Keyboard) {
            sample.connected=true;status="connected";error=0;
            for(unsigned key=0;key<keys.size();++key)if(keys[key])sample.buttons[key]=(GetAsyncKeyState(key)&0x8000)!=0;
            return;
        }
        if(kind==Kind::Xbox) {
            if(!xbox){status="xinput_unavailable";return;}
            if(now<retryAt)return;
            XINPUT_STATE state{};
            const auto result=xbox(slot,&state);
            if(result!=ERROR_SUCCESS){status="xinput_disconnected";error=static_cast<int32_t>(result);retryAt=now+1000;return;}
            retryAt=0;const auto& pad=state.Gamepad;
            sample.values={std::clamp(pad.sThumbLX/32767.f,-1.f,1.f),std::clamp(pad.sThumbLY/32767.f,-1.f,1.f),
                std::clamp(pad.sThumbRX/32767.f,-1.f,1.f),std::clamp(pad.sThumbRY/32767.f,-1.f,1.f),pad.bLeftTrigger/255.f,pad.bRightTrigger/255.f};
            for(unsigned i=0;i<16;++i)sample.buttons[i]=(pad.wButtons&(1u<<i))!=0;
            sample.axes=0x3f;sample.connected=true;status="connected";error=0;return;
        }
        if(!Open(root,owner,now))return;
        DIJOYSTATE2 state{};
        auto result=direct->Poll();
        if(FAILED(result)) {
            result=direct->Acquire();
            if(FAILED(result)){status="device_acquire_failed";error=result;Close();retryAt=now+1000;return;}
            result=direct->Poll();
        }
        if(SUCCEEDED(result))result=direct->GetDeviceState(sizeof(state),&state);
        if(FAILED(result)){status="device_poll_failed";error=result;Close();retryAt=now+1000;return;}
        const LONG axes[]{state.lX,state.lY,state.lZ,state.lRx,state.lRy,state.lRz,state.rglSlider[0],state.rglSlider[1]};
        sample.axes=layout.mask;
        for(unsigned i=0;i<8;++i)if(sample.axes&(1u<<i))sample.values[i]=NormalizeAxis(axes[i],layout.ranges[i]);
        for(unsigned i=0;i<128;++i)sample.buttons[i]=(state.rgbButtons[i]&0x80)!=0;
        std::copy(std::begin(state.rgdwPOV),std::end(state.rgdwPOV),sample.hats.begin());sample.connected=true;status="connected";error=0;
    }
};
}

struct BindingInput::Impl {
    struct Entry {Binding binding;InputControl control;size_t device{};BindingInputState history;};
    std::vector<std::optional<Entry>> entries;
    std::vector<std::unique_ptr<DeviceReader>> devices;
    std::vector<BindingSample> samples;
    IDirectInput8W* direct{};
    HMODULE xinput{};
    XInputRead readXbox{};
    ULONGLONG directRetry{},xboxRetry{};
    HWND window{};
    bool needsDirect{},needsXbox{};
    int32_t directError{};
    ~Impl(){devices.clear();if(direct)direct->Release();if(xinput)FreeLibrary(xinput);}
    void Neutral() {
        for(size_t i=0;i<entries.size();++i) {
            samples[i].connected=false;samples[i].state={};samples[i].error=0;samples[i].raw=samples[i].calibrated=0;
            if(samples[i].supported)samples[i].status="unfocused";
            if(entries[i])entries[i]->history.Update(false,0);
        }
    }
};

BindingInput::BindingInput():impl_(std::make_unique<Impl>()){}
// Abrupt process teardown can reach static destruction under the loader lock.
// Normal Stop calls Release explicitly; otherwise retain OS-owned resources
// until process termination rather than releasing COM/DLLs from DllMain.
BindingInput::~BindingInput(){(void)impl_.release();}
void BindingInput::Release(){impl_.reset();}
void BindingInput::Configure(const std::vector<Binding>& bindings) {
    auto next=std::make_unique<Impl>();next->entries.resize(bindings.size());next->samples.resize(bindings.size());
    for(size_t i=0;i<bindings.size();++i) {
        const auto& binding=bindings[i];
        // Empty identity belongs to protocol v1; its native-family path is
        // retained by the caller. A v2 device never silently falls back to it.
        if(binding.deviceId.empty())continue;
        next->samples[i].supported=true;next->samples[i].status="unsupported_input";
        const auto control=ParseInputControl(binding);if(!control)continue;
        size_t index{};
        for(;index<next->devices.size();++index)if(EqualId(next->devices[index]->id,binding.deviceId) && EqualId(next->devices[index]->family,binding.device))break;
        if(index==next->devices.size()) {
            if(index>=16)continue;
            auto device=std::make_unique<DeviceReader>();device->id=binding.deviceId;device->family=binding.device;
            if(binding.device=="keyboard" && binding.deviceId=="keyboard")device->kind=DeviceReader::Kind::Keyboard;
            else if(binding.device=="xinput_pad" && binding.deviceId.size()==8 && binding.deviceId.starts_with("xinput:") && binding.deviceId[7]>='0' && binding.deviceId[7]<='3') {
                device->kind=DeviceReader::Kind::Xbox;device->slot=binding.deviceId[7]-'0';next->needsXbox=true;
            }
            else if(ReadGuid(binding.deviceId,device->instance) && ReadGuid(binding.device,device->product)) {
                device->kind=DeviceReader::Kind::Direct;next->needsDirect=true;
            }
            else continue;
            next->devices.push_back(std::move(device));
        }
        if(control->kind==InputKind::Key)next->devices[index]->keys[control->index]=true;
        next->entries[i]=Impl::Entry{binding,*control,index,{}};next->samples[i].status="waiting_for_focus";
    }
    impl_=std::move(next);
}
void BindingInput::Poll() {
    if(!impl_)return;
    auto& data=*impl_;
    const auto foreground=GetForegroundWindow();DWORD process{};
    if(!foreground || !GetWindowThreadProcessId(foreground,&process) || process!=GetCurrentProcessId() || IsIconic(foreground)) {
        data.Neutral();data.window=nullptr;return;
    }
    const auto window=GetAncestor(foreground,GA_ROOT);
    if(!window){data.Neutral();data.window=nullptr;return;}
    if(data.window!=window){data.Neutral();data.window=window;}
    const auto now=GetTickCount64();
    if(data.needsDirect && !data.direct && now>=data.directRetry) {
        data.directRetry=now+1000;
        data.directError=DirectInput8Create(GetModuleHandleW(nullptr),DIRECTINPUT_VERSION,IID_IDirectInput8W,reinterpret_cast<void**>(&data.direct),nullptr);
    }
    if(data.needsXbox && !data.readXbox && now>=data.xboxRetry) {
        data.xboxRetry=now+1000;
        for(const auto name:{L"xinput1_4.dll",L"xinput9_1_0.dll"}) {
            const auto library=LoadLibraryExW(name,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!library)continue;
            const auto read=reinterpret_cast<XInputRead>(GetProcAddress(library,"XInputGetState"));
            if(read){data.xinput=library;data.readXbox=read;break;}FreeLibrary(library);
        }
    }
    for(auto& device:data.devices)device->Read(data.direct,data.readXbox,window,now);
    // Recheck after device calls: a focus change during polling must not publish
    // even one stale throttle/gear sample to the native update.
    if(GetForegroundWindow()!=foreground){data.Neutral();data.window=nullptr;return;}
    for(size_t i=0;i<data.entries.size();++i)if(data.entries[i]) {
        auto& entry=*data.entries[i];const auto& device=*data.devices[entry.device];const auto& sample=device.sample;
        const auto& control=entry.control;float raw{};bool available=sample.connected;
        switch(control.kind) {
        case InputKind::SignedAxis:case InputKind::PositiveAxis:
            available=available && (sample.axes&(1u<<control.index));raw=sample.values[control.index];break;
        case InputKind::Button:case InputKind::Key:raw=sample.buttons[control.index]?1.f:0.f;break;
        case InputKind::Hat:raw=HatValue(sample.hats[control.index],control.direction);break;
        }
        available=available && std::isfinite(raw);
        auto& result=data.samples[i];result.connected=available;
        if(device.kind==DeviceReader::Kind::Direct && control.kind==InputKind::SignedAxis)
            result.driverFiltering=!device.direct?"unavailable":(device.layout.nonNeutralFilters&(1u<<control.index))?"non_neutral":
                (device.layout.neutralFilters&(1u<<control.index))?"neutral_verified":"unverified";
        result.raw=available?raw:0;result.calibrated=available?CalibrateInput(raw,control.kind,entry.binding):0;
        result.state=entry.history.Update(available,result.calibrated);
        result.error=!data.direct && device.kind==DeviceReader::Kind::Direct?data.directError:device.error;
        result.status=!available?(sample.connected?"input_unavailable":device.status):entry.history.armed?"active":"waiting_for_neutral";
    }
}
const std::vector<BindingSample>& BindingInput::Samples() const{
    static const std::vector<BindingSample> empty;
    return impl_?impl_->samples:empty;
}
}
