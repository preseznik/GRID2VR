#include "recenter_shortcut.h"
#include <cstdio>
using namespace grid2vr;
int main() {
    int failures{};
    const auto check=[&](bool condition) { if (!condition) ++failures; };
    RecenterShortcut binding;
    check(binding.Valid() && binding.key==121 && !binding.modifiers);
    for (const auto text:{"", "121", "121 0 extra", "121 8", "27 0", "115 2", "9 2", "46 3", "99999999999999999999 0", "-1 0"}) {
        check(!RecenterShortcut::Parse(text,binding));
        check(binding.key==121 && !binding.modifiers);
    }
    check(RecenterShortcut::Parse("82 5",binding) && binding.key==82 && binding.modifiers==5);
    RecenterKeyState state;
    bool request{};
    check(!state.Key(binding,82,0,true,false,true,request) && !request); // Exact chord only.
    check(state.Key(binding,82,5,true,false,true,request) && request);
    check(state.Key(binding,82,5,true,true,true,request) && !request); // No autorepeat.
    check(state.Key(binding,82,0,false,false,true,request) && !request); // Release after modifiers lift.
    check(!state.Key(binding,82,5,true,false,false,request) && !request); // Loading/stopped/unfocused.
    check(state.Key(binding,82,5,true,true,true,request) && !request); // Held on return cannot trigger.
    state.Reset();
    check(state.Key(binding,82,5,true,false,true,request) && request);
    check(state.Key(binding,82,0,false,false,false,request) && !request); // Stop still consumes held release.
    binding={};
    check(state.Key(binding,121,0,true,false,true,request) && request);
    check(state.Key(binding,121,0,false,false,true,request) && !request); // F10 key-up consumed too.
    check(!state.Key(binding,121,8,true,false,true,request) && !request); // Windows chords are not bindings.
    if (failures) std::printf("%d recenter shortcut checks failed\n",failures);
    return failures?1:0;
}
