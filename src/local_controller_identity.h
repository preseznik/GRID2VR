#pragma once
#include <atomic>
#include <cstdint>

namespace grid2vr {
// Only our immutable private table, on the exact verified local object, may
// substitute for the native identity. Other tables/objects remain foreign.
inline std::atomic<uintptr_t> localControllerImage{},localControllerObject{},localControllerShadow{};
static_assert(std::atomic<uintptr_t>::is_always_lock_free);
inline void RegisterLocalControllerShadow(uintptr_t image,uintptr_t object,uintptr_t table) noexcept {
    localControllerObject.store(0);
    localControllerImage.store(image);localControllerShadow.store(table);
    localControllerObject.store(object);
}
inline bool IsLocalControllerTable(uintptr_t image,uintptr_t object,uintptr_t table) {
    if(table==image+0xd0ba08)return true;
    return object && table && localControllerObject.load()==object && localControllerImage.load()==image &&
        localControllerShadow.load()==table && localControllerObject.load()==object;
}
template<class Reader> bool ReadLocalControllerIdentity(uintptr_t image,uintptr_t object,Reader read) {
    uint32_t table{};return read(object,&table,sizeof(table)) && IsLocalControllerTable(image,object,table);
}
}
