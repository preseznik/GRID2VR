#pragma once
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <windows.h>
#include <dinput.h>
#include <cstdint>
#include <functional>

// Menu input policy and system-API filters adapted from DiRT2VR/DiRT3VR.
// Only reads made by the admitted GRID 2 module and its owned window are
// filtered. No game addresses from the reference projects are reused.
namespace grid2vr::seatinput {

// DirectInput devices are named by their instance GUID; a blocked button reads released and
// a blocked hat centred.
struct Policy {
    std::function<bool(unsigned key)> blocksKey;                                  // virtual-key code
    std::function<void(unsigned slot,WORD& buttons)> filterXbox;                  // clears blocked pad buttons
    std::function<bool(const GUID& device,unsigned button,bool pressed)> blocksButton;
    std::function<bool(const GUID& device,unsigned hat,bool centred)> blocksHat;
};

// One DirectInput object's value as the game reads it (a button byte, or a hat's DWORD),
// changed in place when the policy blocks it. Returns whether it was changed.
inline bool FilterObject(const Policy& policy,const GUID& device,DWORD type,void* value,DWORD size) {
    const unsigned instance=DIDFT_GETINSTANCE(type);
    if((type&DIDFT_PSHBUTTON || type&DIDFT_TGLBUTTON) && size>=1 && policy.blocksButton) {
        auto& state=*static_cast<BYTE*>(value);
        if(!policy.blocksButton(device,instance,(state&0x80)!=0)) return false;
        state=0;
        return true;
    }
    if(type&DIDFT_POV && size>=4 && policy.blocksHat) {
        auto& state=*static_cast<DWORD*>(value);
        if(!policy.blocksHat(device,instance,LOWORD(state)==0xffff)) return false;
        state=0xffffffff;
        return true;
    }
    return false;
}

// The WM_INPUT handler takes VKey and Message from RAWKEYBOARD: it ignores VKey >= 0xff, and
// WM_KEYDOWN / WM_SYSKEYDOWN press a key, WM_KEYUP / WM_SYSKEYUP release it (Flags only give
// the E0/E1 prefix). A blocked key the game never saw go down is hidden (VKey 0xff); one it
// holds is released once instead, so nothing sticks. Releases of keys the game holds always
// pass.
class KeyboardFilter {
public:
    // Returns whether the event was changed.
    bool Apply(RAWKEYBOARD& key,const std::function<bool(unsigned)>& blocks) {
        const unsigned vkey=key.VKey;
        if(vkey>=0xff) return false;
        const bool down=key.Message==WM_KEYDOWN || key.Message==WM_SYSKEYDOWN;
        const bool up=key.Message==WM_KEYUP || key.Message==WM_SYSKEYUP;
        if(!down && !up) return false;
        if(up) {
            if(held_[vkey]) { held_[vkey]=false; return false; }
            if(!blocks || !blocks(vkey)) return false;
            Hide(key); return true;
        }
        if(!blocks || !blocks(vkey)) { held_[vkey]=true; return false; }
        if(held_[vkey]) {
            held_[vkey]=false;
            key.Message=key.Message==WM_SYSKEYDOWN ? WM_SYSKEYUP : WM_KEYUP;
            key.Flags|=RI_KEY_BREAK;
            return true;
        }
        Hide(key); return true;
    }
    bool Held(unsigned vkey) const { return vkey<0xff && held_[vkey]; }
private:
    static void Hide(RAWKEYBOARD& key) { key.VKey=0xff; key.MakeCode=0; }
    bool held_[0xff]{};
};

// Installs the hooks once (MinHook); later calls only replace the policy. Returns whether
// every input path is filtered.
bool Install(Policy policy,void (*log)(const char*,...));
// Whether Install succeeded.
bool Ready();
// Which input paths are filtered after Install: the menu can open with the keyboard's and ignore
// a controller type whose filter is missing (#bd820b32).
struct Filters { bool keyboard{},directInput{},xinput{}; };
Filters Installed();
// Window messages are an independent keyboard path. The menu stays closed
// unless its actual game-owned swapchain window is attached successfully.
bool AttachWindow(HWND window);
void Stop();

}
