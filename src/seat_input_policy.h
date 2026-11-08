#pragma once
#include "seat_menu.h"
#include <windows.h>
#include <xinput.h>
#include <array>
#include <functional>
#include <mutex>

namespace grid2vr::seat {
// The API filters may run on input threads; the render thread publishes only
// menu eligibility/open state. Keys used by the menu remain blocked until up.
class InputPolicy {
public:
    explicit InputPolicy(std::function<bool(unsigned)> key):key_(std::move(key)){}
    void Set(bool eligible,bool open,bool desktop) {std::lock_guard lock(mutex_);eligible_=eligible;open_=open;desktop_=desktop;}
    void Stop() {std::lock_guard lock(mutex_);eligible_=open_=false;blocked_={};pads_={};}
    bool BlocksKey(unsigned key) {
        if(key>=256)return false;
        std::lock_guard lock(mutex_);
        bool block=blocked_[key];
        if(eligible_ && key==VK_TAB && !key_(VK_CONTROL) && !key_(VK_MENU) && !key_(VK_SHIFT))block=true;
        if(open_ && (key==VK_RETURN || key==VK_ESCAPE || key==VK_SHIFT || key==VK_LSHIFT || key==VK_RSHIFT ||
                     (key>=VK_LEFT && key<=VK_DOWN) || (desktop_ && (key==VK_PRIOR || key==VK_NEXT))))block=true;
        if(block)blocked_[key]=key_(key);
        if(block)++filteredKeys_;
        return block;
    }
    void FilterXbox(unsigned slot,WORD& buttons) {
        if(slot>=pads_.size())return;
        std::lock_guard lock(mutex_);
        WORD mask=pads_[slot];
        if(open_) {
            mask|=XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_DOWN|XINPUT_GAMEPAD_DPAD_LEFT|XINPUT_GAMEPAD_DPAD_RIGHT|
                  XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_X;
            if(desktop_)mask|=XINPUT_GAMEPAD_LEFT_SHOULDER|XINPUT_GAMEPAD_RIGHT_SHOULDER;
        }
        pads_[slot]=static_cast<WORD>(mask&buttons);
        if(buttons&mask)++filteredPads_;
        buttons=static_cast<WORD>(buttons&~mask);
    }
    std::array<uint64_t,2> Filtered() {std::lock_guard lock(mutex_);return {filteredKeys_,filteredPads_};}
private:
    std::mutex mutex_;
    std::function<bool(unsigned)> key_;
    bool eligible_{},open_{},desktop_{};
    std::array<bool,256> blocked_{};
    std::array<WORD,4> pads_{};
    uint64_t filteredKeys_{},filteredPads_{};
};
// Keyboard navigation is identical to the reference mods. Windows/Steam
// combinations keep their own meaning; Shift is the sideways modifier.
inline unsigned KeyboardHeld(const std::function<bool(unsigned)>& key,bool& sideways) {
    sideways=key(VK_SHIFT);
    unsigned held{};
    if(!key(VK_CONTROL) && !key(VK_MENU) && !sideways && key(VK_TAB))held|=Open;
    if(key(VK_RETURN))held|=Save;
    if(key(VK_ESCAPE))held|=Cancel;
    if(key(VK_UP))held|=Up;
    if(key(VK_DOWN))held|=Down;
    if(key(VK_LEFT))held|=sideways?Left:Back;
    if(key(VK_RIGHT))held|=sideways?Right:Forward;
    if(key(VK_PRIOR))held|=Wider;
    if(key(VK_NEXT))held|=Narrower;
    return held;
}
inline unsigned XboxHeld(WORD buttons,bool& sideways) {
    sideways=(buttons&XINPUT_GAMEPAD_X)!=0;
    unsigned held{};
    if(buttons&XINPUT_GAMEPAD_A)held|=Save;
    if(buttons&XINPUT_GAMEPAD_B)held|=Cancel;
    if(buttons&XINPUT_GAMEPAD_DPAD_UP)held|=Up;
    if(buttons&XINPUT_GAMEPAD_DPAD_DOWN)held|=Down;
    if(buttons&XINPUT_GAMEPAD_DPAD_LEFT)held|=sideways?Left:Back;
    if(buttons&XINPUT_GAMEPAD_DPAD_RIGHT)held|=sideways?Right:Forward;
    if(buttons&XINPUT_GAMEPAD_RIGHT_SHOULDER)held|=Wider;
    if(buttons&XINPUT_GAMEPAD_LEFT_SHOULDER)held|=Narrower;
    return held;
}
}
