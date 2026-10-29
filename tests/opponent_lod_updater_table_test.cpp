#include "opponent_lod_updater_table.h"
#include <cstdio>

#if !defined(_M_IX86)
#error The owned virtual-call ABI fixture is MSVC x86.
#endif
namespace table=grid2vr::opponentlod::updatertable;
#define CHECK(value) do {if(!(value)){std::printf("table check line %d: %s\n",__LINE__,#value);return 1;}}while(false)
struct Object {const uintptr_t* vptr{};uint32_t lifecycle{},updated{};double clock=9.25;};
uint32_t __fastcall Lifecycle(Object* self,void*,uint32_t argument) {self->lifecycle=argument;return argument^0x12345678;}
uint32_t __fastcall OldUpdate(Object* self,void*,uint32_t argument) {self->updated=argument;return argument;}
uint32_t __fastcall NewUpdate(Object* self,void*,uint32_t argument) {self->updated=argument;return argument^0xabcdef01;}
double __fastcall Clock(Object* self,void*) {return self->clock;}
int main() {
    static_assert(table::MethodCount==11 && sizeof(table::Block)==48);
    table::Block pinned{};
    for(std::size_t i=0;i<pinned.size();++i)pinned[i]=0x400000+table::NativeRvas[i];
    CHECK(table::MatchesNative(pinned,0x400000));
    for(std::size_t i=0;i<pinned.size();++i) {auto bad=pinned;bad[i]^=4;CHECK(!table::MatchesNative(bad,0x400000));}
    table::Block owned{};owned[0]=0x11223344;
    for(std::size_t i=1;i<owned.size();++i)owned[i]=reinterpret_cast<uintptr_t>(&Lifecycle);
    owned[table::PrefixWords+table::UpdateSlot]=reinterpret_cast<uintptr_t>(&OldUpdate);
    owned[table::PrefixWords+10]=reinterpret_cast<uintptr_t>(&Clock);
    const auto copy=table::PrivateCopy(owned,reinterpret_cast<uintptr_t>(&NewUpdate));
    for(std::size_t i=0;i<owned.size();++i)
        CHECK(copy[i]==(i==table::PrefixWords+table::UpdateSlot?reinterpret_cast<uintptr_t>(&NewUpdate):owned[i]));
    Object object;object.vptr=copy.data()+table::PrefixWords;
    using WordMethod=uint32_t (__thiscall*)(Object*,uint32_t);
    using ClockMethod=double (__thiscall*)(Object*);
    // Reproduce the native scheduler's +14 dispatch, but ONLY to fixture code.
    CHECK(reinterpret_cast<WordMethod>(object.vptr[5])(&object,0xfedcba98)==(0xfedcba98u^0x12345678u));
    CHECK(object.lifecycle==0xfedcba98);
    CHECK(reinterpret_cast<WordMethod>(object.vptr[1])(&object,0x87654321)==(0x87654321u^0xabcdef01u));
    CHECK(object.updated==0x87654321);
    CHECK(reinterpret_cast<ClockMethod>(object.vptr[10])(&object)==9.25);
    std::puts("PASS: full eleven-method table, all-word guards, one changed slot, lifecycle +14 and clock +28 dispatch; owned code only");
    return 0;
}
