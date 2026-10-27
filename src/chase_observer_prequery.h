#pragma once
#include "chase_observer_owner.h"
#include <array>
#include <cstring>

namespace grid2vr::chaseobserver {
constexpr const char* preQueryCaptureReasons[]{"prequery_address_invalid","prequery_return_mismatch",
    "prequery_identity_unreadable","prequery_camera_table","prequery_provider_mismatch","prequery_provider_table",
    "prequery_gate_unreadable","prequery_camera_gate","prequery_name_unreadable","prequery_camera_name",
    "prequery_record_unreadable","prequery_record_gate","prequery_payload_unreadable","prequery_output_mismatch",
    "prequery_context_unreadable","prequery_changed"};
struct PreQueryObservation {
    uintptr_t camera{},timing{},provider{},output{},returned{},record{},cameraTable{},providerTable{},entity{};
    uint32_t camera0c{},nameHash{},provider0c{},geometry24c0{},geometry24c4{},constraint24d8{},entity11a8{};
    uint8_t cameraa4{},cameraae{},camera2564{};
    std::array<char,32> name{};
    std::array<uint32_t,108> recordWords{};
    std::array<uint32_t,4> timingWords{},outputWords{},entityPositionWords{},obstructionWords{};
    std::array<uint32_t,12> entityBasisWords{},localOffsetWords{};
    bool operator==(const PreQueryObservation&) const=default;
};

// The caller qualifies the exact getter return (base+1f78ed), scoped native
// slot-10 execution and executable guards, and forwards each native call once.
// Read is bool(uintptr_t,void*,size_t). This function never calls game code or
// writes native memory. Raw stable samples are not proof of displayed ownership,
// collision hits, simulation-frame identity or safe camera modification.
template<class Read>
const char* CapturePreQuery(uintptr_t base,uintptr_t camera,uintptr_t timing,uintptr_t provider,
                           uintptr_t output,uintptr_t returned,Read&& read,PreQueryObservation& out) {
    out={};out.camera=camera;out.timing=timing;out.provider=provider;out.output=output;out.returned=returned;
    if(!OwnerNativePointer(base) || base>UINT32_MAX-0xd0c068-0x20 || !OwnerNativePointer(camera) ||
       !OwnerNativePointer(timing) || !OwnerNativePointer(provider) || !OwnerNativePointer(output) || output%16)
        return "prequery_address_invalid";
    if(returned!=output)return "prequery_return_mismatch";
    out.record=camera+0xb0;
    const auto snapshot=[&](PreQueryObservation& value)->const char* {
        value.camera=camera;value.timing=timing;value.provider=provider;
        value.output=output;value.returned=returned;value.record=camera+0xb0;
        const auto pointer=[&](uintptr_t address,uintptr_t& destination) {
            uint32_t native{};
            if(!read(address,&native,sizeof(native)))return false;
            destination=native;return OwnerNativePointer(destination);
        };
        uintptr_t cameraProvider{};
        if(!pointer(camera,value.cameraTable) || !pointer(camera+0x60,cameraProvider) ||
           !pointer(provider,value.providerTable) || !pointer(provider+8,value.entity))return "prequery_identity_unreadable";
        if(value.cameraTable!=base+0xd09948)return "prequery_camera_table";
        if(cameraProvider!=provider)return "prequery_provider_mismatch";
        if(value.providerTable!=base+0xd0c068)return "prequery_provider_table";
        if(!read(camera+0x0c,&value.camera0c,sizeof(value.camera0c)) ||
           !read(camera+0xa4,&value.cameraa4,1))return "prequery_gate_unreadable";
        if(!value.camera0c || value.cameraa4)return "prequery_camera_gate";
        if(!read(camera+0x19,value.name.data(),sizeof(value.name)) ||
           !read(camera+0x98,&value.nameHash,sizeof(value.nameHash)))return "prequery_name_unreadable";
        if(std::memcmp(value.name.data(),"chase_close",12) && std::memcmp(value.name.data(),"chase_far",10))
            return "prequery_camera_name";
        if(!read(value.record,value.recordWords.data(),sizeof(value.recordWords)))return "prequery_record_unreadable";
        if(value.recordWords[0x1a4/4]&2)return "prequery_record_gate";
        if(!read(timing,value.timingWords.data(),sizeof(value.timingWords)) ||
           !read(output,value.outputWords.data(),sizeof(value.outputWords)) ||
           !read(value.entity+0x300,value.entityBasisWords.data(),sizeof(value.entityBasisWords)) ||
           !read(value.entity+0x330,value.entityPositionWords.data(),sizeof(value.entityPositionWords)) ||
           !read(camera+0x2430,value.localOffsetWords.data(),sizeof(value.localOffsetWords)))return "prequery_payload_unreadable";
        if(value.outputWords!=value.entityPositionWords)return "prequery_output_mismatch";
        if(!read(provider+0x0c,&value.provider0c,sizeof(value.provider0c)) ||
           !read(camera+0x24c0,&value.geometry24c0,sizeof(value.geometry24c0)) ||
           !read(camera+0x24c4,&value.geometry24c4,sizeof(value.geometry24c4)) ||
           !read(camera+0x24d8,&value.constraint24d8,sizeof(value.constraint24d8)) ||
           !read(value.entity+0x11a8,&value.entity11a8,sizeof(value.entity11a8)) ||
           !read(camera+0xae,&value.cameraae,1) || !read(camera+0x2564,&value.camera2564,1))
            return "prequery_context_unreadable";
        constexpr std::array<uintptr_t,4> stateOffsets{0x253c,0x2540,0x2544,0x254c};
        for(size_t i=0;i<stateOffsets.size();++i)
            if(!read(camera+stateOffsets[i],&value.obstructionWords[i],sizeof(uint32_t)))return "prequery_context_unreadable";
        return nullptr;
    };
    if(const auto refusal=snapshot(out))return refusal;
    PreQueryObservation again{};
    // Two complete bounded reads reject observed identity, gate or source-data
    // changes. They do not guarantee atomicity or detect change-and-return ABA.
    if(snapshot(again) || again!=out)return "prequery_changed";
    return nullptr;
}
}
