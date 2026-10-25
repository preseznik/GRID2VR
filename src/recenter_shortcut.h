#pragma once
#include <charconv>
#include <string_view>

namespace grid2vr {
struct RecenterShortcut {
    unsigned key{121},modifiers{}; // F10; Ctrl=1, Alt=2, Shift=4, as in DiRT2VR.
    bool Valid() const {
        const bool validKey=(key>=48 && key<=90) || (key>=96 && key<=135) ||
            key==8 || key==9 || key==13 || key==19 || key==32 || (key>=33 && key<=40) || key==45 || key==46;
        return validKey && modifiers<=7 && !((modifiers&2) && (key==9 || key==115)) &&
            !(key==46 && (modifiers&3)==3);
    }
    static bool Parse(std::string_view text,RecenterShortcut& output) {
        if (text.size()>16) return false;
        RecenterShortcut value;
        const char* begin=text.data(); const char* end=begin+text.size();
        const auto first=std::from_chars(begin,end,value.key);
        if (first.ec!=std::errc{} || first.ptr==end || *first.ptr!=' ') return false;
        const auto second=std::from_chars(first.ptr+1,end,value.modifiers);
        if (second.ec!=std::errc{} || second.ptr!=end || !value.Valid()) return false;
        output=value; return true;
    }
};

// Window-thread state. Consume both edges so F10 cannot activate a window menu.
class RecenterKeyState {
    bool held{};
public:
    void Reset() { held=false; }
    bool Key(const RecenterShortcut& binding,unsigned key,unsigned modifiers,bool down,bool repeat,bool enabled,bool& requested) {
        requested=false;
        if (key!=binding.key || (!held && (!enabled || modifiers!=binding.modifiers))) return false;
        if (down) {
            requested=enabled && !repeat && !held;
            held=true;
        } else held=false;
        return true;
    }
};
}
