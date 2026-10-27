#pragma once
#include "chase_observer_owner.h"
#include <array>

namespace grid2vr::chaseobserver {
struct Destination { uintptr_t output{};bool auxiliary{};const char* reason="destination_stack_unreadable"; };
constexpr const char* destinationReasons[]{"destination_stack_short","destination_address_invalid",
    "destination_getter_caller","destination_parent","destination_stack_ambiguous","destination_output_invalid",
    "destination_stack_unreadable","destination_stack_changed","destination_auxiliary_frame",
    "destination_identity_unreadable","destination_camera_table","destination_overlap",
    "destination_output_unreadable","destination_identity_changed"};

// Requires the guarded converter and caller windows. The native return-slot
// address accompanies its 25-word copy; uintptr_t values in the copy still
// represent four-byte x86 words. Normal destinations remain writes even when
// owner qualification is busy or fails. Only the exact auxiliary caller/frame
// can be exempted; every refusal must conservatively invalidate unknown writes.
template<class Read>
Destination DecodeDestination(std::span<const uintptr_t> stack,uintptr_t returnSlot,uintptr_t base,
                              uintptr_t camera,Read&& read) {
    if(stack.size()<25)return {0,false,"destination_stack_short"};
    if(!OwnerNativePointer(base) || base>UINT32_MAX-0xdff9b0-0x90 || !OwnerNativePointer(camera) ||
       !OwnerNativePointer(returnSlot) || returnSlot%16!=12)return {0,false,"destination_address_invalid"};
    if(stack[0]!=base+0x8047a0)return {0,false,"destination_getter_caller"};
    const auto parent=stack[18];
    const bool auxiliary=parent==base+0x2e3c40;
    if(!auxiliary && parent!=base+0x1185af && parent!=base+0x21a621)return {0,false,"destination_parent"};
    unsigned matches=0,index=0;
    for(unsigned i=20;i<=23;++i)if(stack[i]==parent){++matches;index=i;}
    if(matches!=1)return {0,false,"destination_stack_ambiguous"};
    const auto output=stack[index+1];
    if(!OwnerNativePointer(output) || output%16)return {output,false,"destination_output_invalid"};
    const auto sameStack=[&]()->const char* {
        std::array<uint32_t,25> again{};
        if(!read(returnSlot,again.data(),sizeof(again)))return "destination_stack_unreadable";
        for(size_t i=0;i<again.size();++i)if(again[i]!=stack[i])return "destination_stack_changed";
        return nullptr;
    };
    if(const auto refusal=sameStack())return {output,false,refusal};
    if(!auxiliary) {
        // Do not require owner flags/selection: the guarded native call still
        // writes this exact destination, even if another observer rejects it.
        if(const auto refusal=sameStack())return {output,false,refusal};
        return {output,false,nullptr};
    }
    // At the guarded auxiliary call, the caller's aligned EBP is saved in
    // word17. Its 0x158-byte locals plus saved ESI/EDI, pushed output and return
    // place the converter return at word23 and its output at EBP-0x150.
    const auto callerFrame=returnSlot+0x1c4;
    if(index!=23 || stack[17]!=callerFrame || output!=callerFrame-0x150 ||
       output<returnSlot+25*4 || output+0x90>callerFrame)
        return {output,false,"destination_auxiliary_frame"};
    struct Identity {
        uintptr_t cameraTable{},root{},state{},controller{};
        bool operator==(const Identity&) const=default;
    };
    const auto identity=[&](Identity& value) {
        const auto pointer=[&](uintptr_t at,uintptr_t& destination) {
            uint32_t native{};if(!read(at,&native,4))return false;
            destination=native;return OwnerNativePointer(destination);
        };
        return pointer(camera,value.cameraTable) && pointer(base+0xdef810,value.root) &&
            pointer(value.root+0x10,value.state) && pointer(value.state+0x14fc,value.controller);
    };
    Identity before{};
    if(!identity(before))return {output,false,"destination_identity_unreadable"};
    if(before.cameraTable!=base+0xd09948)return {output,false,"destination_camera_table"};
    const auto overlap=[&](uintptr_t address,size_t bytes) {
        return output<address+bytes && address<output+0x90;
    };
    if(overlap(camera+0xb0,0x1b0) || overlap(before.controller+0x1f0,0x90) || overlap(base+0xdff9b0,0x90))
        return {output,false,"destination_overlap"};
    std::array<uint32_t,36> localView{};
    if(!read(output,localView.data(),sizeof(localView)))return {output,false,"destination_output_unreadable"};
    Identity after{};
    if(!identity(after) || after!=before)return {output,false,"destination_identity_changed"};
    if(const auto refusal=sameStack())return {output,false,refusal};
    return {output,true,nullptr};
}
}
