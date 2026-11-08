#include "seat_input_policy.h"
#include "seat_input_filter.h"
#include <iostream>
#include <stdexcept>
#include <string>
using namespace grid2vr::seat;
#define CHECK(value) do {if(!(value))throw std::runtime_error("Failed line "+std::to_string(__LINE__)+": " #value);} while(false)
int main() {
    try {
        std::array<bool,256> keys{};auto key=[&](unsigned code){return keys[code];};InputPolicy policy(key);
        keys[VK_TAB]=true;CHECK(!policy.BlocksKey(VK_TAB));policy.Set(true,false,true);CHECK(policy.BlocksKey(VK_TAB));
        policy.Set(false,false,true);CHECK(policy.BlocksKey(VK_TAB));keys[VK_TAB]=false;CHECK(policy.BlocksKey(VK_TAB));CHECK(!policy.BlocksKey(VK_TAB));
        policy.Set(true,false,true);keys[VK_TAB]=keys[VK_MENU]=true;CHECK(!policy.BlocksKey(VK_TAB));keys={};
        bool sideways{};keys[VK_LEFT]=true;CHECK(KeyboardHeld(key,sideways)==Back && !sideways);keys[VK_SHIFT]=true;CHECK(KeyboardHeld(key,sideways)==Left && sideways);
        policy.Set(true,true,false);CHECK(policy.BlocksKey(VK_LEFT) && !policy.BlocksKey(VK_PRIOR) && !policy.BlocksKey(VK_F10));
        policy.Set(true,true,true);CHECK(policy.BlocksKey(VK_PRIOR));
        WORD buttons=XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_RIGHT_SHOULDER|XINPUT_GAMEPAD_Y;policy.FilterXbox(0,buttons);CHECK(buttons==XINPUT_GAMEPAD_Y);
        policy.Set(true,false,true);buttons=XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_Y;policy.FilterXbox(0,buttons);CHECK(buttons==XINPUT_GAMEPAD_Y);
        buttons=0;policy.FilterXbox(0,buttons);buttons=XINPUT_GAMEPAD_DPAD_UP;policy.FilterXbox(0,buttons);CHECK(buttons==XINPUT_GAMEPAD_DPAD_UP);
        CHECK(XboxHeld(XINPUT_GAMEPAD_DPAD_LEFT|XINPUT_GAMEPAD_X,sideways)==Left && sideways);
        grid2vr::seatinput::KeyboardFilter keyboard;bool blocked{};auto block=[&](unsigned){return blocked;};
        RAWKEYBOARD event{};event.VKey=VK_UP;event.Message=WM_KEYDOWN;CHECK(!keyboard.Apply(event,block) && keyboard.Held(VK_UP));
        blocked=true;event.Message=WM_KEYDOWN;CHECK(keyboard.Apply(event,block) && event.Message==WM_KEYUP && !keyboard.Held(VK_UP));
        event.VKey=VK_UP;event.Message=WM_KEYDOWN;CHECK(keyboard.Apply(event,block) && event.VKey==0xff);
        blocked=false;event.VKey=VK_UP;event.Message=WM_KEYDOWN;CHECK(!keyboard.Apply(event,block));
        blocked=true;event.Message=WM_KEYUP;CHECK(!keyboard.Apply(event,block) && !keyboard.Held(VK_UP));
        policy.Stop();CHECK(!policy.BlocksKey(VK_LEFT));
        std::cout<<"Seat input policy: hotkey modifiers, held release, keyboard translation, pad masks and unrelated keys passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
