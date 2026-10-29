// Launcher-only, non-exclusive capture. The game continues to read its own inputs.
// Only the explicit feedback test briefly takes a wheel exclusively.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>
#include "axes.h"

struct Sample {
    unsigned connected{}, axes{};
    float values[8]{};
    unsigned char buttons[128]{};
    unsigned pov[4]{};
};
// A launcher feedback test owns a second, exclusive handle only while it plays.
struct Feedback {
    IDirectInputDevice8W* device{};
    IDirectInputEffect* effect{};
    ~Feedback() {
        if(effect) { effect->Stop(); effect->Unload(); effect->Release(); }
        if(device) { device->SendForceFeedbackCommand(DISFFC_STOPALL); device->Unacquire(); device->Release(); }
    }
};
struct Device {
    IDirectInputDevice8W* input{};
    GUID instance{};
    std::wstring id, name, product;
    unsigned axes{};
    AxisLayout layout;
    int xbox{-1};
    std::unique_ptr<Feedback> feedback;
    ~Device() { feedback.reset(); if(input) { input->Unacquire(); input->Release(); } }
};
struct Capture {
    IDirectInput8W* input{};
    HWND window{};
    std::vector<std::unique_ptr<Device>> devices;
    ~Capture() { devices.clear(); if(input) input->Release(); }
};
BOOL CALLBACK Enumerate(const DIDEVICEINSTANCEW* info, void* user) {
    auto& capture=*static_cast<Capture*>(user);
    auto device=std::make_unique<Device>();
    if(FAILED(capture.input->CreateDevice(info->guidInstance,&device->input,nullptr))) return DIENUM_CONTINUE;
    DIPROPGUIDANDPATH path{};
    path.diph={sizeof(path),sizeof(path.diph),0,DIPH_DEVICE};
    if(SUCCEEDED(device->input->GetProperty(DIPROP_GUIDANDPATH,&path.diph)) &&
       (wcsstr(path.wszPath,L"IG_") || wcsstr(path.wszPath,L"ig_"))) return DIENUM_CONTINUE;
    if(FAILED(device->input->SetDataFormat(&c_dfDIJoystick2)) ||
       FAILED(device->input->SetCooperativeLevel(capture.window,DISCL_NONEXCLUSIVE|DISCL_BACKGROUND))) return DIENUM_CONTINUE;
    wchar_t guid[40]{};
    StringFromGUID2(info->guidInstance,guid,40);
    device->id=guid; device->name=info->tszProductName; device->instance=info->guidInstance;
    StringFromGUID2(info->guidProduct,guid,40); device->product=guid;
    device->layout=ReadAxes(device->input); device->axes=device->layout.mask;
    device->input->Acquire();
    capture.devices.push_back(std::move(device));
    return DIENUM_CONTINUE;
}
extern "C" __declspec(dllexport) void* __cdecl CaptureOpen(HWND window) {
    try {
        // Embedded monitors supply a child HWND; DirectInput requires our top-level window.
        window=GetAncestor(window,GA_ROOT);
        DWORD owner{};
        if(!window || !GetWindowThreadProcessId(window,&owner) || owner!=GetCurrentProcessId()) return nullptr;
        auto capture=std::make_unique<Capture>(); capture->window=window;
        if(FAILED(DirectInput8Create(GetModuleHandleW(nullptr),DIRECTINPUT_VERSION,IID_IDirectInput8W,
                                    reinterpret_cast<void**>(&capture->input),nullptr))) return nullptr;
        capture->input->EnumDevices(DI8DEVCLASS_GAMECTRL,Enumerate,capture.get(),DIEDFL_ATTACHEDONLY);
        for(int slot=0;slot<4;++slot) {
            XINPUT_STATE state{};
            if(XInputGetState(slot,&state)!=ERROR_SUCCESS) continue;
            auto device=std::make_unique<Device>(); device->xbox=slot; device->axes=0x3f;
            device->id=L"xinput:"+std::to_wstring(slot); device->name=L"Xbox controller "+std::to_wstring(slot+1); device->product=L"xinput_pad";
            capture->devices.push_back(std::move(device));
        }
        return capture.release();
    } catch(...) { return nullptr; }
}
extern "C" __declspec(dllexport) void __cdecl CaptureClose(void* capture) { delete static_cast<Capture*>(capture); }
extern "C" __declspec(dllexport) unsigned __cdecl CaptureCount(void* capture) {
    return capture ? static_cast<unsigned>(static_cast<Capture*>(capture)->devices.size()) : 0;
}
extern "C" __declspec(dllexport) int __cdecl CaptureName(void* capture,unsigned index,wchar_t* id,unsigned idCount,wchar_t* name,unsigned nameCount) {
    if(!capture || index>=CaptureCount(capture) || !id || !name) return 0;
    const auto& device=*static_cast<Capture*>(capture)->devices[index];
    return wcscpy_s(id,idCount,device.id.c_str())==0 && wcscpy_s(name,nameCount,device.name.c_str())==0;
}
extern "C" __declspec(dllexport) int __cdecl CaptureProduct(void* capture,unsigned index,wchar_t* product,unsigned count) {
    if(!capture || index>=CaptureCount(capture) || !product) return 0;
    return wcscpy_s(product,count,static_cast<Capture*>(capture)->devices[index]->product.c_str())==0;
}
// Plays a short test effect: both Xbox motors at half strength, or a 10 Hz sine
// shake on a wheel's first force-feedback axis. milliseconds==0 stops it. The
// wheel is acquired exclusively only for the test; the game must not be running.
enum FeedbackResult { FeedbackStarted=1, FeedbackInvalid=0, FeedbackUnsupported=-1, FeedbackBusy=-2, FeedbackFailed=-3 };
BOOL CALLBACK FirstActuator(const DIDEVICEOBJECTINSTANCEW* object, void* user) {
    *static_cast<DWORD*>(user)=object->dwOfs; return DIENUM_STOP;
}
extern "C" __declspec(dllexport) int __cdecl CaptureFeedback(void* capture,unsigned index,unsigned milliseconds) {
    if(!capture || index>=CaptureCount(capture) || milliseconds>5000) return FeedbackInvalid;
    auto& owner=*static_cast<Capture*>(capture);
    auto& device=*owner.devices[index];
    if(device.xbox>=0) {
        XINPUT_VIBRATION motors{};
        if(milliseconds) motors.wLeftMotorSpeed=motors.wRightMotorSpeed=32768;
        return XInputSetState(device.xbox,&motors)==ERROR_SUCCESS ? FeedbackStarted : FeedbackFailed;
    }
    device.feedback.reset();
    if(!milliseconds) return FeedbackStarted;
    DIDEVCAPS caps{sizeof(caps)};
    if(FAILED(device.input->GetCapabilities(&caps)) || !(caps.dwFlags&DIDC_FORCEFEEDBACK)) return FeedbackUnsupported;
    auto feedback=std::make_unique<Feedback>();
    if(FAILED(owner.input->CreateDevice(device.instance,&feedback->device,nullptr)) ||
       FAILED(feedback->device->SetDataFormat(&c_dfDIJoystick2)) ||
       FAILED(feedback->device->SetCooperativeLevel(owner.window,DISCL_EXCLUSIVE|DISCL_FOREGROUND))) return FeedbackFailed;
    if(FAILED(feedback->device->Acquire())) return FeedbackBusy;
    DWORD axis=DIJOFS_X;
    feedback->device->EnumObjects(FirstActuator,&axis,DIDFT_AXIS|DIDFT_FFACTUATOR);
    LONG direction=0;
    DIPERIODIC shake{5000,0,0,100000};
    DIEFFECT effect{sizeof(effect)};
    effect.dwFlags=DIEFF_CARTESIAN|DIEFF_OBJECTOFFSETS;
    effect.dwDuration=milliseconds*1000;effect.dwGain=DI_FFNOMINALMAX;effect.dwTriggerButton=DIEB_NOTRIGGER;
    effect.cAxes=1;effect.rgdwAxes=&axis;effect.rglDirection=&direction;
    effect.cbTypeSpecificParams=sizeof(shake);effect.lpvTypeSpecificParams=&shake;
    if(FAILED(feedback->device->CreateEffect(GUID_Sine,&effect,&feedback->effect,nullptr)) ||
       FAILED(feedback->effect->Start(1,0))) return FeedbackFailed;
    device.feedback=std::move(feedback);
    return FeedbackStarted;
}
extern "C" __declspec(dllexport) int __cdecl CaptureRead(void* capture,unsigned index,Sample* result) {
    if(!result) return 0;
    *result={}; std::fill(std::begin(result->pov),std::end(result->pov),0xffffffffu);
    if(!capture || index>=CaptureCount(capture)) return 0;
    auto& device=*static_cast<Capture*>(capture)->devices[index];
    result->axes=device.axes;
    if(device.xbox>=0) {
        XINPUT_STATE state{};
        if(XInputGetState(device.xbox,&state)!=ERROR_SUCCESS) return 0;
        auto& pad=state.Gamepad;
        result->values[0]=std::clamp(pad.sThumbLX/32767.f,-1.f,1.f);
        result->values[1]=std::clamp(pad.sThumbLY/32767.f,-1.f,1.f);
        result->values[2]=std::clamp(pad.sThumbRX/32767.f,-1.f,1.f);
        result->values[3]=std::clamp(pad.sThumbRY/32767.f,-1.f,1.f);
        result->values[4]=pad.bLeftTrigger/255.f;
        result->values[5]=pad.bRightTrigger/255.f;
        for(unsigned i=0;i<16;++i) result->buttons[i]=(pad.wButtons&(1u<<i))?1:0;
    } else {
        auto status=device.input->Poll();
        if(FAILED(status)) { device.input->Acquire(); return 0; }
        DIJOYSTATE2 state{};
        if(FAILED(device.input->GetDeviceState(sizeof(state),&state))) return 0;
        const LONG axes[]={state.lX,state.lY,state.lZ,state.lRx,state.lRy,state.lRz,state.rglSlider[0],state.rglSlider[1]};
        for(unsigned i=0;i<8;++i) if(device.axes&(1u<<i)) result->values[i]=NormalizeAxis(axes[i],device.layout.ranges[i]);
        for(unsigned i=0;i<128;++i) result->buttons[i]=(state.rgbButtons[i]&0x80)?1:0;
        std::copy(std::begin(state.rgdwPOV),std::end(state.rgdwPOV),result->pov);
    }
    result->connected=1; return 1;
}
