#pragma once
#include <cstdint>
#include <span>

namespace grid2vr::chaseobserver {
// Raw export provenance only. A matching graph does not prove that this view
// reached the main renderer, or identify a safe camera-write point.
struct OwnerObservation {
    uintptr_t callerRva{},output{},controller{},record{};
    uintptr_t root{},state{},globalController{},manager{},primary{},alternate{},selected{},cameraTable{};
    uint8_t controller392{},controller393{},alternateEnabled{};
    bool operator==(const OwnerObservation&) const=default;
};

inline bool OwnerNativePointer(uintptr_t value) {
    return value>=0x10000 && value<=UINT32_MAX-0x10000 && value%4==0;
}

// Requires the supported executable and the exact getter/converter/caller
// instruction guards before use. Stack is a bounded copy beginning at the
// getter's native return slot, not the observer helper's return slot.
// Read must be bool(uintptr_t address, void* destination, size_t bytes), with
// safe bounded reads. Native pointers are always four bytes, including tests
// compiled on a wider host. Returns nullptr on success, otherwise a refusal.
template<class Read>
const char* DecodeOwner(std::span<const uintptr_t> stack,uintptr_t base,uintptr_t camera,
                        Read&& read,OwnerObservation& out) {
    out={};
    if(stack.size()<25)return "owner_stack_short";
    if(!OwnerNativePointer(base) || base>UINT32_MAX-0xdef810-4 || !OwnerNativePointer(camera))
        return "owner_address_invalid";
    out.record=camera+0xb0;
    if(stack[0]!=base+0x8047a0)return "owner_getter_caller";
    if(stack[18]!=base+0x1185af && stack[18]!=base+0x21a621)return "owner_export_caller";
    out.callerRva=stack[18]-base;
    unsigned matches=0,returnWord=0;
    for(unsigned i=20;i<=23;++i)if(stack[i]==stack[18]){++matches;returnWord=i;}
    if(matches!=1)return "owner_stack_ambiguous";
    out.output=stack[returnWord+1];
    if(!OwnerNativePointer(out.output) || out.output<0x10000+0x1f0)return "owner_output_invalid";
    out.controller=out.output-0x1f0;

    const auto pointer=[&](uintptr_t address,uintptr_t& value,bool nullable=false) {
        uint32_t native{};
        if(!read(address,&native,sizeof(native)))return false;
        value=native;
        return (nullable && !value) || OwnerNativePointer(value);
    };
    const auto graph=[&](OwnerObservation& value)->const char* {
        if(!pointer(base+0xdef810,value.root) || !pointer(value.root+0x10,value.state) ||
           !pointer(value.state+0x14fc,value.globalController))return "owner_root_unreadable";
        const auto controller=value.globalController;
        if(controller!=value.controller)return "owner_controller_mismatch";
        if(!read(controller+0x392,&value.controller392,1) ||
           !read(controller+0x393,&value.controller393,1))return "owner_flags_unreadable";
        if(value.controller392 || value.controller393)return "owner_controller_busy";
        if(!pointer(controller+0x33c,value.manager) || !pointer(value.manager+0x748,value.primary) ||
           !read(value.manager+0x759,&value.alternateEnabled,1))return "owner_selection_unreadable";
        if(value.alternateEnabled && !pointer(value.primary+0x70,value.alternate,true))
            return "owner_alternate_unreadable";
        value.selected=value.alternate?value.alternate:value.primary;
        if(value.selected!=camera)return "owner_camera_mismatch";
        if(!pointer(camera,value.cameraTable))return "owner_camera_unreadable";
        if(value.cameraTable!=base+0xd09948)return "owner_camera_table";
        return nullptr;
    };
    if(const auto refusal=graph(out))return refusal;
    OwnerObservation again{};
    again.callerRva=out.callerRva;again.output=out.output;
    again.controller=out.controller;again.record=out.record;
    // Two bounded observations reject a visible change; they do not promise an
    // atomic snapshot or rule out changes that return to their original value.
    if(graph(again) || again!=out)return "owner_graph_changed";
    return nullptr;
}
}
