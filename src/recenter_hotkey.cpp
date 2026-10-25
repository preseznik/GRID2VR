#include "recenter_hotkey.h"
#include <atomic>

namespace grid2vr::recenter {
namespace {
HWND gameWindow{};
WNDPROC previous{};
RecenterShortcut binding;
RecenterKeyState keys;
std::atomic<bool> enabled{},pending{};
bool Foreground() {
    DWORD process{}; GetWindowThreadProcessId(GetForegroundWindow(),&process);
    return process==GetCurrentProcessId();
}
LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM key,LPARAM flags) {
    const bool down=message==WM_KEYDOWN || message==WM_SYSKEYDOWN;
    const bool up=message==WM_KEYUP || message==WM_SYSKEYUP;
    if (down || up) {
        const unsigned modifiers=(GetKeyState(VK_CONTROL)<0?1u:0u) | (GetKeyState(VK_MENU)<0?2u:0u) |
            (GetKeyState(VK_SHIFT)<0?4u:0u) | ((GetKeyState(VK_LWIN)<0 || GetKeyState(VK_RWIN)<0)?8u:0u);
        bool requested{};
        if (keys.Key(binding,static_cast<unsigned>(key),modifiers,down,(flags&(1L<<30))!=0,enabled.load() && Foreground(),requested)) {
            if (requested) pending=true;
            return 0;
        }
    }
    if (message==WM_KILLFOCUS) { keys.Reset(); pending=false; }
    const auto result=CallWindowProcW(previous,window,message,key,flags);
    if (message==WM_NCDESTROY) { enabled=false; pending=false; }
    return result;
}
}
bool Attach(HWND window,RecenterShortcut value) {
    if (gameWindow) return gameWindow==window;
    DWORD process{};
    if (!value.Valid() || !window || !GetWindowThreadProcessId(window,&process) || process!=GetCurrentProcessId()) return false;
    binding=value;
    previous=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window,GWLP_WNDPROC));
    if (!previous) return false;
    SetLastError(0);
    const auto old=SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(WindowProc));
    if (!old && GetLastError()) return false;
    if (old) previous=reinterpret_cast<WNDPROC>(old);
    gameWindow=window;
    return true;
}
void Enable(bool value) { enabled=value; if (!value) pending=false; }
bool Consume() { return pending.exchange(false) && Foreground(); }
}
