#include "chase_camera_adapter.h"
#include <iostream>
#include <map>
#include <vector>
using namespace grid2vr;
using namespace grid2vr::chasecamera;
int main() {
    std::map<uintptr_t,std::vector<uint8_t>> memory;
    const auto put=[&](uintptr_t at,const auto& value) {
        const auto bytes=reinterpret_cast<const uint8_t*>(&value);
        memory[at]={bytes,bytes+sizeof(value)};
    };
    const auto read=[&](uintptr_t at,void* out,size_t size) {
        const auto it=memory.find(at);
        if(it==memory.end() || it->second.size()!=size)return false;
        std::memcpy(out,it->second.data(),size);return true;
    };
    unsigned writes{};
    const auto write=[&](uintptr_t at,const void* value,size_t bytes) {
        if(bytes!=sizeof(Pose))return false;++writes;
        const auto begin=static_cast<const uint8_t*>(value);memory[at]={begin,begin+bytes};return true;
    };
    const Pose native{0,0,0,1,0,2,-4,1.0001f};
    auto changed=native;
    if(!OrbitChasePose(changed,{0,2,0},{0,1,0},{1,0,0},.5f,.2f,1.8f))return 1;
    PoseOverlay overlay{0x100000,0x200000,0x300000,native,changed,true};
    put(overlay.camera+0xb0,changed);
    if(!overlay.Restore(overlay.camera,overlay.provider,overlay.entity,read,write) || writes!=1 || overlay.valid)return 2;
    Pose result{};read(overlay.camera+0xb0,result.data(),sizeof(result));if(result!=native)return 3;
    // Native obstruction adjustments belong in displayed, never in the saved
    // baseline. Neither a second restore nor an intervening native change writes.
    if(!overlay.Restore(overlay.camera,overlay.provider,overlay.entity,read,write) || writes!=1)return 4;
    overlay.valid=true;result[6]+=1;put(overlay.camera+0xb0,result);
    if(!overlay.Restore(overlay.camera,overlay.provider,overlay.entity,read,write) || writes!=1)return 5;
    overlay.valid=true;put(overlay.camera+0xb0,changed);
    if(overlay.Restore(overlay.camera,overlay.provider,overlay.entity+4,read,write) || writes!=1)return 6;
    chaseobserver::PreQueryObservation snapshot{};
    const auto bits=[](float f){return std::bit_cast<uint32_t>(f);};
    snapshot.entityPositionWords={bits(10),bits(20),bits(30),bits(1)};
    snapshot.entityBasisWords={bits(1),0,0,0,0,bits(1),0,0,0,0,bits(1),0};
    snapshot.localOffsetWords[8]=bits(1);snapshot.localOffsetWords[9]=bits(2);snapshot.localOffsetWords[10]=bits(3);
    ChaseVector pivot{};
    if(!VehicleOrbitPivot(snapshot,pivot) || pivot.x!=10 || pivot.y!=20 || pivot.z!=30)return 7;
    // Regression: a forward native look target must not move the orbit centre.
    // Turning the car also cannot rotate that target offset into our pivot.
    snapshot.entityBasisWords={0,0,bits(-1),0,0,bits(1),0,bits(1),0,0,0};
    snapshot.localOffsetWords[10]=bits(1.8122f);
    const auto captured=snapshot;
    if(!VehicleOrbitPivot(snapshot,pivot) || pivot.x!=10 || pivot.y!=20 || pivot.z!=30 || snapshot!=captured)return 14;
    const auto near=[](float a,float b){return std::abs(a-b)<.0001f;};
    const Pose behind{0,0,0,1,10,22,25,1.0001f};
    auto turned=behind;
    if(!OrbitChasePose(turned,pivot,{0,1,0},{1,0,0},3.14159265f,0) ||
       !near(turned[4],10) || !near(turned[5],22) || !near(turned[6],35) || turned[7]!=behind[7])return 15;
    auto raised=behind;
    if(!OrbitChasePose(raised,pivot,{0,1,0},{1,0,0},0,1.57079633f) ||
       !near(raised[4],10) || !near(raised[5],25) || !near(raised[6],32))return 16;
    auto zoomed=behind;
    if(!OrbitChasePose(zoomed,pivot,{0,1,0},{1,0,0},0,0,.6f) ||
       !near(zoomed[4],10) || !near(zoomed[5],21.2f) || !near(zoomed[6],27))return 17;
    auto neutral=behind;
    if(!OrbitChasePose(neutral,pivot,{0,1,0},{1,0,0},0,0) || neutral!=behind)return 18;
    for(unsigned axis=0;axis<3;++axis)for(uint32_t invalid:{0x7fc00000u,0x7f800000u,0xff800000u}) {
        auto bad=snapshot;bad.entityPositionWords[axis]=invalid;
        if(VehicleOrbitPivot(bad,pivot) || pivot.x!=10 || pivot.y!=20 || pivot.z!=30)return 8;
    }
    auto bad=snapshot;bad.localOffsetWords[9]=0x7fc00000;
    if(VehicleOrbitPivot(bad,pivot))return 19;
    bad=snapshot;bad.localOffsetWords[10]=bits(21);
    if(VehicleOrbitPivot(bad,pivot))return 20;
    bad=snapshot;bad.entityBasisWords[0]=bits(2);
    if(VehicleOrbitPivot(bad,pivot))return 21;
    constexpr uint32_t base=0x400000,root=0x100000,state=0x110000,controller=0x120000,manager=0x130000,camera=0x140000,provider=0x150000,entity=0x160000;
    put(base+0xdef810,root);put(root+0x10,state);put(state+0x14fc,controller);put(controller+0x33c,manager);
    put(controller+0x392,uint8_t{});put(controller+0x393,uint8_t{});put(manager+0x748,camera);put(manager+0x759,uint8_t{});
    put(camera,base+0xd09948);put(camera+0x60,provider);put(provider+8,entity);
    uintptr_t selected{};
    if(!SelectedOwner(base,camera,read,selected) || selected!=manager)return 9;
    put(manager+0x759,uint8_t{1});if(SelectedOwner(base,camera,read,selected))return 10;
    put(manager+0x759,uint8_t{});put(controller+0x392,uint8_t{1});if(SelectedOwner(base,camera,read,selected))return 11;
    put(controller+0x392,uint8_t{});put(manager+0x748,camera+4);if(SelectedOwner(base,camera,read,selected))return 12;
    put(manager+0x748,camera);unsigned observations{};
    const auto changing=[&](uintptr_t at,void* out,size_t size) {
        if(at==base+0xdef810 && ++observations==2)put(manager+0x748,camera+4);
        return read(at,out,size);
    };
    if(SelectedOwner(base,camera,changing,selected))return 13;
    std::cout<<"Owned overlay restoration, pivot and selected-camera refusal passed.\n";
    return 0;
}
