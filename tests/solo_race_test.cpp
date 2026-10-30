#include "solo_race.h"
#include <cstdio>
#include <cstring>

int main() {
    auto* memory=static_cast<uint8_t*>(VirtualAlloc(nullptr,0xe70000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!memory)return 1;
    const auto base=reinterpret_cast<uintptr_t>(memory);
    const auto put=[&](size_t offset,uint32_t value) { std::memcpy(memory+offset,&value,4); };
    const auto ptr=[&](size_t offset,size_t target) { put(offset,static_cast<uint32_t>(base+target)); };
    constexpr size_t parent=0x10000,manager=parent+0x190,car=0x11000,head=0x12000,node=0x12100,group=0x13000,local=0x14000,ai=0x15000;
    put(0xe5917c,2);ptr(0xd96808,parent);ptr(0xdaa424,parent);ptr(parent,0xd1266c);
    ptr(parent+0xa8,0x16000);ptr(parent+0xac,0x16004);ptr(0x16000,car);
    ptr(manager,0xd0bc48);put(manager+0x18,1);memory[manager+0x24]=1;put(manager+0x2c,4);put(manager+0x30,2);
    ptr(manager+0x14,head);ptr(head,node);ptr(node+12,car);ptr(node+16,group);
    ptr(group,0xd0bb30);ptr(group+8,0x16100);ptr(group+12,0x16108);ptr(0x16100,local);ptr(0x16104,ai);
    ptr(local,0xd0ba08);memory[local+0x40]=1;ptr(ai,0xd039ac);
    if(!grid2vr::NormalSoloRaceReady(base))return 2;
    ptr(local,0x18000);
    if(grid2vr::NormalSoloRaceReady(base))return 24;
    grid2vr::RegisterLocalControllerShadow(base,base+local,base+0x18000);
    if(!grid2vr::NormalSoloRaceReady(base))return 25;
    grid2vr::RegisterLocalControllerShadow(base,base+local+4,base+0x18000);
    if(grid2vr::NormalSoloRaceReady(base))return 26;
    grid2vr::RegisterLocalControllerShadow(0,0,0);ptr(local,0xd0ba08);
    // Stale globals, an extra vehicle, AI selection, disabled input, loading and
    // an active recording must all keep native activation closed.
    struct Change { size_t offset;uint32_t value; };
    const Change changes[]{{0xdaa424,0},{0xe61010,1},{0xe5917c,1},{parent,0},
        {parent+0xac,static_cast<uint32_t>(base+0x16008)},{car+0xb0,1},
        {manager+0x18,2},{manager+0x24,0},{manager+0x2c,1},{manager+0x30,0},{manager+0x34,1},
        {node+12,0},{group+0x18,1},{group+12,static_cast<uint32_t>(base+0x16104)},
        {local,0},{local+0x40,0},{ai,0}};
    for(const auto& change:changes) {
        uint32_t before{};std::memcpy(&before,memory+change.offset,4);
        put(change.offset,change.value);
        if(grid2vr::NormalSoloRaceReady(base))return 3;
        put(change.offset,before);
    }
    if(!grid2vr::NormalSoloRaceReady(base))return 4;
    DWORD previous{};
    if(!VirtualProtect(memory+0x14000,0x1000,PAGE_NOACCESS,&previous))return 5;
    if(grid2vr::NormalSoloRaceReady(base))return 6;
    if(!VirtualProtect(memory+0x14000,0x1000,previous,&previous))return 7;

    // The two-car fixture has the observed object/table/ownership shape from
    // launch-modes-race-readiness.json. Exercise each supported count as well.
    const auto localRace=[&](unsigned count) {
        std::memset(memory+0x10000,0,0x8000);
        put(0xe61010,0);put(0xe5917c,2);ptr(0xd96808,parent);ptr(0xdaa424,parent);ptr(parent,0xd1266c);
        ptr(parent+0xa8,0x16000);ptr(parent+0xac,0x16000+count*4);
        ptr(manager,0xd0bc48);put(manager+0x18,count);memory[manager+0x24]=1;put(manager+0x2c,4);put(manager+0x30,2);
        ptr(manager+0x14,head);ptr(head,node);ptr(head+4,node);ptr(head+8,node+(count-1)*0x40);
        for(unsigned i=0;i<count;++i) {
            const auto vehicle=car+i*0x100,current=node+i*0x40,owner=group+i*0x80,controller=ai+i*0x80,children=0x16100+i*0x10;
            ptr(0x16000+i*4,vehicle);ptr(vehicle,0xcdfeb4);put(vehicle+0xb0,i?1:0);
            ptr(current,head);ptr(current+4,i?current-0x40:head);ptr(current+8,i+1<count?current+0x40:head);
            ptr(current+12,vehicle);ptr(current+16,owner);memory[current+0x14]=i?0:1;
            ptr(owner,0xd0bb30);ptr(owner+8,children);ptr(owner+12,children+(i?4:8));put(owner+0x1c,1);
            if(i)ptr(children,controller);
            else { ptr(children,local);ptr(children+4,controller); }
            ptr(controller,0xd039ac);
        }
        ptr(local,0xd0ba08);put(local+0x40,0x3e0c6901); // Only the low byte is the enable flag.
    };
    for(unsigned count=1;count<=8;++count) {
        localRace(count);
        std::array<uint8_t,0x8000> snapshot{};std::memcpy(snapshot.data(),memory+0x10000,snapshot.size());
        if(!grid2vr::NormalLocalRaceReady(base))return 8;
        ptr(local,0x18000);grid2vr::RegisterLocalControllerShadow(base,base+local,base+0x18000);
        if(!grid2vr::NormalLocalRaceReady(base))return 27;
        grid2vr::RegisterLocalControllerShadow(0,0,0);
        if(grid2vr::NormalLocalRaceReady(base))return 28;
        ptr(local,0xd0ba08);
        if(std::memcmp(snapshot.data(),memory+0x10000,snapshot.size()))return 9;
        if((count==1)!=grid2vr::NormalSoloRaceReady(base))return 10;
    }
    // Preserve the nonmonotonic car-vector order and full ownership/tree shape
    // in launch-modes-race-eight-readiness.json, relocating its heap pointers.
    // Every AI has role 1, including the third through eighth cars.
    localRace(8);
    constexpr unsigned rank[]{3,2,4,5,0,6,7,1};
    constexpr int left[]{7,-1,-1,2,-1,-1,-1,4};
    constexpr int right[]{3,-1,-1,5,-1,6,-1,1};
    constexpr int owner[]{-1,7,3,0,7,3,5,0};
    constexpr uint32_t flags[]{1,0,1,0,0,1,0x345f0000,1};
    constexpr uint32_t aiField40[]{2107717,556026441,136388868,136391013,694497702,2111943,690305479,138492391};
    const auto treeNode=[](int index) { return index<0?head:node+static_cast<size_t>(index)*0x40; };
    ptr(head,node+4*0x40);ptr(head+8,node+6*0x40);
    put(manager+0x24,0x7101);put(local+0x40,274810369);
    for(unsigned i=0;i<8;++i) {
        const auto vehicle=car+rank[i]*0x100,current=node+i*0x40;
        ptr(0x16000+i*4,vehicle);put(vehicle+0xb0,i?1:0);
        ptr(current,treeNode(left[i]));ptr(current+4,treeNode(owner[i]));ptr(current+8,treeNode(right[i]));
        ptr(current+12,vehicle);put(current+0x14,flags[i]);put(ai+i*0x80+0x40,aiField40[i]);
    }
    std::array<uint8_t,0x8000> eightSnapshot{};std::memcpy(eightSnapshot.data(),memory+0x10000,eightSnapshot.size());
    if(!grid2vr::NormalLocalRaceReady(base))return 15;
    if(std::memcmp(eightSnapshot.data(),memory+0x10000,eightSnapshot.size()))return 16;
    const Change badRoles[]{
        {car+rank[0]*0x100+0xb0,1},
        {car+rank[2]*0x100+0xb0,0},{car+rank[2]*0x100+0xb0,2},
        {car+rank[7]*0x100+0xb0,7}
    };
    for(const auto& change:badRoles) {
        uint32_t before{};std::memcpy(&before,memory+change.offset,4);put(change.offset,change.value);
        if(grid2vr::NormalLocalRaceReady(base))return 17;
        put(change.offset,before);
    }
    localRace(2);
    const auto address=[&](size_t offset){return static_cast<uint32_t>(base+offset);};
    const Change localChanges[]{
        {0xdaa424,0},{0xe61010,1},{0xe5917c,1},{parent,0},{parent+0xac,address(0x16000)},
        {parent+0xac,address(0x16024)},{parent+0xac,address(0x16007)},{parent+0xac,address(0x15ffc)},
        {car,0},{car+0x100,0},{car+0x1b0,0},{0x16004,address(car)},
        {manager,0},{manager+0x18,1},{manager+0x24,0},{manager+0x2c,1},{manager+0x30,0},{manager+0x34,1},
        {head+4,address(head)},{head+8,address(node)},{head,address(node+0x40)},
        {node,address(node)},{node+8,address(head)},{node+0x48,address(node)},
        {node+0x44,address(head)},{node+0x4c,address(car)},{node+0x4c,address(car+0x300)},
        {node+0x50,address(group)},{node+0x54,0x100},
        {group+0x18,1},{group+0x98,1},{group+0x8c,address(0x16118)},
        {0x16100,address(ai)},{0x16110,address(local)},{0x16110,address(ai)},
        {local,0},{local+0x40,0},{ai+0x80,0}
    };
    for(const auto& change:localChanges) {
        uint32_t before{};std::memcpy(&before,memory+change.offset,4);put(change.offset,change.value);
        if(grid2vr::NormalLocalRaceReady(base)) { std::printf("local readiness accepted bad offset %zx\n",change.offset);return 11; }
        put(change.offset,before);
    }
    if(!grid2vr::NormalLocalRaceReady(base))return 12;
    // Mode 0 is a direct launch only while the flashback module holds it cleared for this image.
    put(0xe5917c,0);
    if(grid2vr::NormalLocalRaceReady(base))return 18;
    grid2vr::benchmarkModeClearedImage.store(base+4);
    if(grid2vr::NormalLocalRaceReady(base))return 19;
    grid2vr::benchmarkModeClearedImage.store(base);
    if(!grid2vr::NormalLocalRaceReady(base))return 20;
    put(0xe5917c,1);
    if(grid2vr::NormalLocalRaceReady(base))return 21;
    grid2vr::benchmarkModeClearedImage.store(0);put(0xe5917c,2);
    if(!grid2vr::NormalLocalRaceReady(base))return 22;
    if(!VirtualProtect(memory+0x15000,0x1000,PAGE_NOACCESS,&previous))return 13;
    if(grid2vr::NormalLocalRaceReady(base))return 14;
    VirtualFree(memory,0,MEM_RELEASE);
    std::puts("Solo readiness unchanged; local race readiness validates 1-8 owned cars and refuses malformed/foreign/loading/controller graphs without writes.");
    return 0;
}
