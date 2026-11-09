#include "dlc_cockpit.h"
#include "seat_camera_host.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <thread>

using namespace grid2vr::dlccockpit;
#define CHECK(value) do {if(!(value))throw std::runtime_error("Failed line "+std::to_string(__LINE__)+": " #value);} while(false)
using Object=std::array<uint8_t,objectBytes>;
Object ObjectFor(size_t car,size_t view,uint32_t table) {
    Object bytes;bytes.fill(0x5a);std::memcpy(bytes.data(),&table,4);
    const auto name=view?"bonnet_reverse":"bonnet";std::strcpy(reinterpret_cast<char*>(bytes.data()+0x19),name);
    const auto hash=NameHash(name);std::memcpy(bytes.data()+0x98,&hash,4);
    const auto& expected=cars[car].original[view];
    for(size_t i=0;i<floatOffsets.size();++i)std::memcpy(bytes.data()+floatOffsets[i],&expected.values[i],4);
    for(size_t i=0;i<flagOffsets.size();++i)if(expected.flags[i]>=0)bytes[flagOffsets[i]]=expected.flags[i];
    bytes[poseCacheValidOffset]=1; // Native pose already cached before our first Present.
    return bytes;
}
bool ChangedByte(size_t offset,size_t car,size_t view) {
    if(offset==poseCacheValidOffset)return true;
    if(offset>=0xf4 && offset<0xf8)return true;
    for(size_t i=0;i<18;++i)if(offset>=floatOffsets[i] && offset<floatOffsets[i]+4)return true;
    for(size_t i=0;i<flagOffsets.size();++i)if(offset==flagOffsets[i] && cars[car].original[view].flags[i]>=0)return true;
    return false;
}
void PairTests() {
    constexpr uint32_t table=0x1109ef0;
    for(size_t car=0;car<cars.size();++car)for(bool hide:{false,true}) {
        auto forward=ObjectFor(car,0,table),reverse=ObjectFor(car,1,table);const auto a=forward,b=reverse;
        Row row{car,cars[car].seat};CHECK(PatchPair(forward,reverse,table,row,hide));
        for(size_t i=0;i<objectBytes;++i) {
            if(!ChangedByte(i,car,0))CHECK(forward[i]==a[i]);
            if(!ChangedByte(i,car,1))CHECK(reverse[i]==b[i]);
        }
        for(const auto* object:{&forward,&reverse}) {
            for(size_t i=0;i<3;++i)CHECK(FloatAt(*object,0x2230+i*4)==row.seat[i]);
            CHECK(FloatAt(*object,0x10)==.02f && FloatAt(*object,0xf4)==.02f);
            CHECK((*object)[0x2266]==1 && (*object)[0x2268]==1 && (*object)[0x2267]==static_cast<uint8_t>(hide));
            CHECK((*object)[poseCacheValidOffset]==0);
        }
        // Reject either bad view before writing any byte of either object.
        for(size_t field=0;field<floatOffsets.size();++field) {
            forward=a;reverse=b;float changed=FloatAt(reverse,floatOffsets[field])+1.f;
            std::memcpy(reverse.data()+floatOffsets[field],&changed,4);const auto invalid=reverse;
            CHECK(!PatchPair(forward,reverse,table,row,hide));CHECK(forward==a && reverse==invalid);
        }
        for(size_t field=0;field<flagOffsets.size();++field)if(cars[car].original[1].flags[field]>=0) {
            forward=a;reverse=b;reverse[flagOffsets[field]]=2;const auto invalid=reverse;
            CHECK(!PatchPair(forward,reverse,table,row,hide));CHECK(forward==a && reverse==invalid);
        }
        forward=a;reverse=b;reverse[0]^=1;CHECK(!PatchPair(forward,reverse,table,row,hide));CHECK(forward==a);
        forward=a;reverse=b;reverse[poseCacheValidOffset]=2;const auto invalidCache=reverse;
        CHECK(!PatchPair(forward,reverse,table,row,hide));CHECK(forward==a && reverse==invalidCache);
        forward=a;reverse=b;forward[poseCacheValidOffset]=0;reverse[poseCacheValidOffset]=0;
        CHECK(PatchPair(forward,reverse,table,row,hide));
        forward=a;reverse=b;reverse[0x19]='x';CHECK(!PatchPair(forward,reverse,table,row,hide));
        forward=a;CHECK(!PatchPair(forward,forward,table,row,hide));
        CHECK(!PatchPair(forward,std::span<uint8_t>(forward).subspan(1),table,row,hide));
    }
}
void RequestTests() {
    Request request;CHECK(ParseRequest("dlc-cockpit-v1 head-only\n240 .3611 1.0917 -.2968\n",request));
    CHECK(request.count==1 && !request.hideDriver);
    const auto corvette="dlc-cockpit-v1 head-only\nz06 .4379 1.0799 -.4141\n";
    CHECK(!ParseRequest(corvette,request));
    CHECK(ParseRequest(corvette,request,true) && request.rows[0].car==FindCar("z06"));
    CHECK(ParseRequest("dlc-cockpit-v1 hide-driver\r\n240 .3611 1.0917 -.2968\r\n",request));CHECK(request.hideDriver);
    for(const auto* invalid:{"", "dlc-cockpit-v1 head-only\n", "dlc-cockpit-v1 head-only\n240 .3611 1.0917 -.2968",
        "dlc-cockpit-v1 head-only\n240 nan 1 -.2\n", "dlc-cockpit-v1 head-only\n240 0 99 0\n",
        "dlc-cockpit-v1 head-only\n240 1.0 1.0917 -.2968\n", "dlc-cockpit-v1 head-only\ns20 0 1 0\n",
        "dlc-cockpit-v1 unknown\n240 .3611 1.0917 -.2968\n", "dlc-cockpit-v1 head-only\n240 .3611 1.0917 -.2968 extra\n",
        "dlc-cockpit-v1 head-only\n240 .3611 1.0917 -.2968\n240 .3611 1.0917 -.2968\n"}) {
        CHECK(!ParseRequest(invalid,request));CHECK(request.count==0);
    }
    CHECK(!ParseRequest(std::string(4097,'x'),request));
    CHECK(ParseRequest("dlc-cockpit-desktop-v1 head-only 10\n240 .3611 1.0917 -.2968\n",request));
    CHECK(request.desktop && request.fovOffset==10 && !request.hideDriver);
    for(bool runtime:{false,true})for(bool xr:{false,true})for(bool projection:{false,true})
        CHECK(ValidSession(request,runtime,xr,projection)==(runtime && !xr && !projection));
    CHECK(ParseRequest("dlc-cockpit-v1 head-only\n240 .3611 1.0917 -.2968\n",request));
    CHECK(!request.desktop && request.fovOffset==0);
    for(bool runtime:{false,true})for(bool xr:{false,true})for(bool projection:{false,true})
        CHECK(ValidSession(request,runtime,xr,projection)==(runtime && xr && projection));
    for(const auto* value:{"31","-31","1.5","nan","10 extra"})
        CHECK(!ParseRequest(std::string("dlc-cockpit-desktop-v1 head-only ")+value+"\n240 .3611 1.0917 -.2968\n",request));
}

void DesktopFovTests() {
    constexpr uint32_t table=0x1109ef0;
    for(size_t car=0;car<cars.size();++car)for(bool hide:{false,true})for(int fov:{-30,10,30}) {
        auto forward=ObjectFor(car,0,table),reverse=ObjectFor(car,1,table),baseForward=forward,baseReverse=reverse;
        const Row row{car,cars[car].seat};
        CHECK(PatchPair(baseForward,baseReverse,table,row,hide));
        CHECK(PatchPair(forward,reverse,table,row,hide,fov));
        CHECK(PatchedPair(forward,reverse,table,row,hide,fov));
        CHECK(FloatAt(forward,0x14)==cars[car].original[0].values[19]+fov);
        CHECK(FloatAt(reverse,0x14)==cars[car].original[1].values[19]+fov);
        for(size_t byte=0;byte<objectBytes;++byte)if(byte<0x14 || byte>=0x18) {
            CHECK(forward[byte]==baseForward[byte]);CHECK(reverse[byte]==baseReverse[byte]);
        }
        forward=ObjectFor(car,0,table);reverse=ObjectFor(car,1,table);const auto a=forward,b=reverse;
        CHECK(!PatchPair(forward,reverse,table,row,hide,31));CHECK(forward==a && reverse==b);
    }
}
template<class T> void Put(uint8_t* bytes,size_t offset,T value) {std::memcpy(bytes+offset,&value,sizeof(value));}
uint32_t Address(const void* p) {return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));}
uint32_t Word(std::ifstream& in) {uint32_t v{};in.read(reinterpret_cast<char*>(&v),4);CHECK(in.good());return v;}
constexpr size_t imageBytes=0xe70000;
uint8_t* Image(const char* fixture) {
    std::ifstream in(fixture,std::ios::binary);CHECK(in.good());
    auto* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,imageBytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(image);
    const auto base=reinterpret_cast<uintptr_t>(image);std::vector<uint32_t> starts;
    const auto count=Word(in);CHECK(count==32);
    for(uint32_t i=0;i<count;++i) {
        const auto rva=Word(in),size=Word(in),relocations=Word(in);CHECK(size<4096 && rva+size<imageBytes && relocations<256);
        in.read(reinterpret_cast<char*>(image+rva),size);CHECK(in.good());starts.push_back(rva);
        for(uint32_t j=0;j<relocations;++j) {
            const auto at=Word(in);CHECK(at+4<=size);uint32_t value{};std::memcpy(&value,image+rva+at,4);
            value+=static_cast<uint32_t>(base-0x400000);std::memcpy(image+rva+at,&value,4);
        }
    }
    Put(image,mountedTableRva+0x30,static_cast<uint32_t>(base+0x4b3830));
    Put(image,mountedTableRva+0x6c,static_cast<uint32_t>(base+0x5dbd50));
    Put(image,mountedTableRva+0x70,static_cast<uint32_t>(base+0x7af7d0));CHECK(TestCodeMatches(base));
    for(auto at:starts) {image[at]^=1;CHECK(!TestCodeMatches(base));image[at]^=1;}
    for(auto slot:{0x30,0x6c,0x70}) {
        image[mountedTableRva+slot]^=1;CHECK(!TestCodeMatches(base));image[mountedTableRva+slot]^=1;
    }
    return image;
}
struct alignas(16) LiveFixture {
    alignas(16) std::array<uint8_t,0xa68> owner{};
    alignas(16) std::array<uint8_t,0x770> manager{};
    alignas(16) std::array<uint8_t,0x250> parent{};
    alignas(16) std::array<uint8_t,0x160> car{};
    alignas(16) std::array<uint8_t,0xb0> replay{};
    alignas(16) std::array<uint8_t,0x20> group{};
    alignas(16) std::array<uint8_t,0x44> local{};
    alignas(16) std::array<uint8_t,4> ai{};
    alignas(16) std::array<uint8_t,0x14> root{};
    alignas(16) std::array<uint8_t,0x1500> game{};
    alignas(16) std::array<uint8_t,0x400> controller{};
    std::array<uint32_t,6> head{},node{};
    std::array<uint32_t,2> controls{},views{};
    std::array<uint32_t,1> vehicles{};
    alignas(16) Object front{},rear{};
    uint8_t* image;uint32_t base;
    LiveFixture(uint8_t* value):image(value),base(Address(value)) {}
    void Original(size_t index) {
        front=ObjectFor(index,0,base+mountedTableRva);rear=ObjectFor(index,1,base+mountedTableRva);
        Put(front.data(),0x60,Address(car.data())+0x12c);Put(rear.data(),0x60,Address(car.data())+0x12c);
    }
    void Load(size_t index) {
        owner={};manager={};parent={};car={};replay={};group={};local={};ai={};
        root={};game={};controller={};
        Put(image,0xdef810,Address(root.data()));Put(root.data(),0x10,Address(game.data()));
        Put(game.data(),0x14fc,Address(controller.data()));Put(controller.data(),0x33c,Address(manager.data()));
        Put(manager.data(),0x748,Address(front.data()));
        Put(image,0xe61010,0u);Put(image,0xe5917c,2u);
        Put(image,0xd96808,Address(parent.data()));Put(image,0xdaa424,Address(parent.data()));
        Put(image,0xdef814,Address(owner.data()));
        Put(parent.data(),0,base+0xd1266c);vehicles={Address(car.data())};
        Put(parent.data(),0xa8,Address(vehicles.data()));Put(parent.data(),0xac,Address(vehicles.data())+4);
        Put(car.data(),0,base+0xcdfeb4);Put(car.data(),0x148,Address(manager.data()));
        Put(owner.data(),0,base+0xcdd504);Put(owner.data(),0xc,base+0xcdd4f8);
        Put(owner.data(),0xa64,1u);std::memcpy(owner.data()+0x564,cars[index].code,4);
        Put(owner.data(),0x570,Address(manager.data()));
        views={Address(front.data()),Address(rear.data())};
        Put(manager.data(),0x5f0,Address(views.data()));Put(manager.data(),0x5f4,Address(views.data())+8);
        Put(replay.data(),0xa8,Address(parent.data()));
        auto* control=parent.data()+0x190;
        Put(control,0,base+0xd0bc48);Put(control,0x18,1u);control[0x24]=1;
        Put(control,0x2c,4u);Put(control,0x30,2u);Put(control,0x14,Address(head.data()));
        head={Address(node.data()),Address(node.data()),Address(node.data()),0,0,0};
        node={Address(head.data()),Address(head.data()),Address(head.data()),Address(car.data()),Address(group.data()),0};
        controls={Address(local.data()),Address(ai.data())};
        Put(group.data(),0,base+0xd0bb30);Put(group.data(),8,Address(controls.data()));Put(group.data(),12,Address(controls.data())+8);
        Put(local.data(),0,base+0xd0ba08);local[0x40]=1;Put(ai.data(),0,base+0xd039ac);
        Original(index);
    }
};
void SeatHostTests(uint8_t* image) {
    namespace seat=grid2vr::seat;
    LiveFixture graph(image);const auto table=graph.base+mountedTableRva;
    Request dlc{};dlc.count=dlcCars.size();
    for(size_t i=0;i<dlc.count;++i)dlc.rows[i]={i,cars[i].seat};
    TestSetup(Address(image),dlc,true);
    seat::SessionRequest request{};request.count=seat::CarCount;request.positions=true;
    for(size_t car=0;car<seat::CarCount;++car)request.rows[car]={car,{3,-4,5}};
    const auto load=[&](size_t car) {
        graph.Load(0);std::memcpy(graph.owner.data()+0x564,seat::CarCode(car),4);
        seat::CameraPair expected;CHECK(seat::ExpectedPair(request.Applied(car),table,expected));
        graph.front=expected[0];graph.rear=expected[1];
        Put(graph.front.data(),0x60,Address(graph.car.data())+0x12c);Put(graph.rear.data(),0x60,Address(graph.car.data())+0x12c);
        graph.front[poseCacheValidOffset]=graph.rear[poseCacheValidOffset]=1;
    };
    for(bool desktop:{false,true}) {
        request.desktop=desktop;request.fov=desktop?10:0;
        load(0);seat::CameraHost host;CHECK(host.TestInitialize(Address(image),request));
        const std::vector<uint8_t> codeBefore(image,image+imageBytes);
        for(size_t car=0;car<seat::CarCount;++car) {
            load(car);const auto a=graph.front,b=graph.rear;auto desired=request.Applied(car);++desired.position.height;
            if(desktop)++desired.fov;
            unsigned edits{};
            CHECK(host.Update([&](size_t selected,bool changed){CHECK(selected==car && changed);++edits;return desired;}));
            CHECK(edits==1 && graph.front[poseCacheValidOffset]==0 && graph.rear[poseCacheValidOffset]==0);
            seat::CameraPair expected;CHECK(seat::ExpectedPair(desired,table,expected));
            CHECK(seat::Matches(graph.front,expected[0],desired,0,table) && seat::Matches(graph.rear,expected[1],desired,1,table));
            for(size_t byte=0;byte<objectBytes;++byte)if(byte!=poseCacheValidOffset &&
                !(byte>=0x2230 && byte<0x223c) && !(byte>=0x14 && byte<0x18)) {
                CHECK(graph.front[byte]==a[byte] && graph.rear[byte]==b[byte]);
            }
            graph.front[poseCacheValidOffset]=graph.rear[poseCacheValidOffset]=1;
            CHECK(host.Update([&](size_t selected,bool changed){CHECK(selected==car && !changed);return desired;}));
            CHECK(graph.front[poseCacheValidOffset]==1 && graph.rear[poseCacheValidOffset]==1);
            // Initial DLC calibration must not overwrite a live seat edit.
            if(car>=seat::baseCars.size()) {const auto front=graph.front,rear=graph.rear;Present();CHECK(graph.front==front && graph.rear==rear);}
        }
        // A restart can reload the launch values into the same two objects.
        // Recognize only that complete admitted pair and reselect the car.
        load(seat::CarCount-1);
        CHECK(host.Update([&](size_t car,bool changed){CHECK(car==seat::CarCount-1 && changed);auto next=request.Applied(car);next.position.height+=2;return next;}));
        // Only data objects changed; relocated native code remains byte-for-byte.
        CHECK(std::equal(codeBefore.begin(),codeBefore.end(),image));host.Stop();
        const auto initial=request.Applied(seat::CarCount-1);seat::CameraPair expected;
        CHECK(seat::ExpectedPair(initial,table,expected));
        CHECK(seat::Matches(graph.front,expected[0],initial,0,table) && seat::Matches(graph.rear,expected[1],initial,1,table));
    }
    load(0);seat::CameraHost host;CHECK(host.TestInitialize(Address(image),request));
    unsigned edits{};
    const auto edit=[&](size_t car,bool){++edits;auto next=request.Applied(car);++next.position.height;return next;};
    const auto refused=[&] {const auto a=graph.front,b=graph.rear;const auto calls=edits;CHECK(!host.Update(edit));CHECK(edits==calls && graph.front==a && graph.rear==b);};
    Put(graph.controller.data(),0x392,uint8_t(1));refused();
    load(0);Put(graph.controller.data(),0x393,uint8_t(1));refused();
    load(0);Put(graph.controller.data(),0x33c,Address(graph.manager.data())+16);refused();
    load(0);Put(graph.manager.data(),0x748,Address(graph.front.data())+16);refused();
    load(0);Put(graph.manager.data(),0x759,uint8_t(2));refused();
    load(0);Put(graph.manager.data(),0x759,uint8_t(1));Put(graph.front.data(),0x70,Address(graph.front.data())+16);refused();
    load(0);Put(graph.manager.data(),0x759,uint8_t(1));Put(graph.front.data(),0x70,Address(graph.rear.data()));CHECK(host.Update(edit));
    load(0);Put(graph.car.data(),0xb0,1u);refused();
    load(0);Put(graph.front.data(),0x60,Address(graph.car.data())+0x130);refused();
    load(0);graph.rear[0x2233]^=0x40;refused();
    load(0);graph.rear[poseCacheValidOffset]=2;refused();
    load(0);std::thread other([&]{refused();});other.join();
    // Revalidate after the callback: a car/selection change cannot receive even
    // the first camera write from the old graph.
    load(0);const auto a=graph.front,b=graph.rear;
    CHECK(!host.Update([&](size_t car,bool){Put(graph.manager.data(),0x748,Address(graph.front.data())+16);return request.Applied(car);}));
    CHECK(graph.front==a && graph.rear==b);host.Stop();
    load(0);image[0x653b00]^=1;seat::CameraHost invalid;
    CHECK(!invalid.TestInitialize(Address(image),request));image[0x653b00]^=1;
    // Normal -> Custom Event is mode 0, never a direct benchmark launch. It
    // requires the separate seat-only opt-in and still validates both views.
    load(0);Put(image,0xe5917c,0u);TestSetup(Address(image),dlc,true);
    seat::CameraHost unmarked;CHECK(unmarked.TestInitialize(Address(image),request));
    CHECK(!unmarked.Update(edit));unmarked.Stop();
    TestSetup(Address(image),dlc,true,true);
    seat::CameraHost normal;CHECK(normal.TestInitialize(Address(image),request));
    CHECK(normal.Update(edit));normal.Stop();
    // Older launchers can still opt in through the seat marker.
    graph.Load(0);Put(image,0xe5917c,0u);TestSetup(Address(image),dlc,true,true);
    Present();CHECK(TestCounts(0)[0]==1 && TestCounts(0)[1]==0);Stop();
    // The offline Normal entry alone must not enable the seat editor.
    load(0);Put(image,0xe5917c,0u);TestSetup(Address(image),dlc,true,false,true);
    seat::CameraHost withoutSeat;CHECK(withoutSeat.TestInitialize(Address(image),request));
    CHECK(!withoutSeat.Update(edit));withoutSeat.Stop();
}
void LiveTests(uint8_t* image) {
    static_assert(sizeof(void*)==4,"Live graph test must use the Win32 target");
    LiveFixture graph(image);Request request{};request.count=cars.size();
    for(size_t i=0;i<cars.size();++i)request.rows[i]={i,cars[i].seat};
    graph.Load(0);
    // Arm is the shared production tail of Prepare after executable validation.
    // It must leave the entire synthetic image and all native objects untouched.
    const std::vector<uint8_t> beforeArm(image,image+imageBytes);const auto oldFront=graph.front,oldRear=graph.rear;
    TestSetup(Address(image),request);CHECK(std::equal(beforeArm.begin(),beforeArm.end(),image));
    CHECK(graph.front==oldFront && graph.rear==oldRear);
    Put(image,0xe5917c,0u);Present();CHECK(graph.front==oldFront && graph.rear==oldRear && TestCounts(0)[0]==0);
    // Native career enters with mode 0. Every requested DLC pair, including
    // the Season 3 Camaro (z22), should calibrate with the seat tool off.
    TestSetup(Address(image),request,true);
    for(size_t car=0;car<cars.size();++car) {
        graph.Load(car);Put(image,0xe5917c,0u);
        const auto a=graph.front,b=graph.rear;Present();
        CHECK(graph.front==a && graph.rear==b && TestCounts(car)[0]==0);
    }
    TestSetup(Address(image),request,true,false,true);
    for(size_t car=0;car<cars.size();++car) {
        graph.Load(car);Put(image,0xe5917c,0u);Present();Present();
        CHECK(TestCounts(car)[0]==1 && TestCounts(car)[1]==0);
        CHECK(PatchedPair(graph.front,graph.rear,graph.base+mountedTableRva,request.rows[car],false));
    }
    graph.Load(0);Put(image,0xe5917c,0u);const auto originalFront=graph.front,originalRear=graph.rear;
    Put(image,0xe61010,Address(graph.replay.data()));Present();
    CHECK(graph.front==originalFront && graph.rear==originalRear);
    Put(image,0xe61010,0u);Put(graph.car.data(),0xb0,1u);Present();
    CHECK(graph.front==originalFront && graph.rear==originalRear);
    Put(graph.car.data(),0xb0,0u);TestSetup(Address(image),request,true);Present();
    CHECK(graph.front==originalFront && graph.rear==originalRear && TestCounts(0)[0]==0);
    request.count=dlcCars.size(); // Direct launches carry only explicitly selected DLC.
    TestSetup(Address(image),request);
    Put(image,0xe5917c,2u);
    for(size_t car=0;car<request.count;++car) {
        graph.Load(car);const auto a=graph.front,b=graph.rear;
        const std::vector<uint8_t> codeBefore(image,image+imageBytes);
        Present();CHECK(TestCounts(car)[0]==1);
        CHECK(PatchedPair(graph.front,graph.rear,graph.base+mountedTableRva,request.rows[car],false));
        CHECK(graph.front[poseCacheValidOffset]==0 && graph.rear[poseCacheValidOffset]==0);
        CHECK(std::equal(codeBefore.begin(),codeBefore.end(),image));
        for(size_t i=0;i<objectBytes;++i) {
            if(!ChangedByte(i,car,0))CHECK(graph.front[i]==a[i]);
            if(!ChangedByte(i,car,1))CHECK(graph.rear[i]==b[i]);
        }
        // The engine recomputes the pose on its next update. Do not keep
        // invalidating it on every Present or override subsequent camera choices.
        graph.front[poseCacheValidOffset]=1;graph.rear[poseCacheValidOffset]=1;
        const auto patchedFront=graph.front,patchedRear=graph.rear;
        for(unsigned i=0;i<5;++i)Present();
        CHECK(TestCounts(car)[0]==1 && TestCounts(car)[1]==0 && graph.front==patchedFront && graph.rear==patchedRear);
        // An engine reload can reuse the same addresses. Original data must be
        // patched again rather than skipped by a stale address cache.
        graph.Original(car);Present();CHECK(TestCounts(car)[0]==2);
        CHECK(graph.front[poseCacheValidOffset]==0 && graph.rear[poseCacheValidOffset]==0);
    }
    const auto unchanged=[&] {const auto a=graph.front,b=graph.rear;Present();CHECK(graph.front==a && graph.rear==b);};
    graph.Load(0);Put(graph.car.data(),0x148,Address(graph.manager.data())+16);unchanged();
    graph.Load(0);Put(graph.front.data(),0x60,Address(graph.car.data())+0x130);unchanged();
    graph.Load(0);Put(graph.car.data(),0xb0,1u);unchanged();
    graph.Load(0);Put(graph.owner.data(),0,0u);unchanged();
    graph.Load(0);Put(graph.owner.data(),0xc,0u);unchanged();
    graph.Load(0);Put(graph.owner.data(),0xa64,65u);unchanged();
    graph.Load(0);Put(graph.owner.data(),0xa64,2u);std::memcpy(graph.owner.data()+0x578,"240",4);unchanged();
    graph.Load(0);std::memcpy(graph.owner.data()+0x564,"xyz",4);unchanged();
    graph.Load(0);graph.views[1]=graph.views[0];unchanged();
    graph.Load(0);Put(graph.manager.data(),0x5f4,Address(graph.views.data())+65*4);unchanged();
    // Even a matching object graph cannot turn this into an executable-memory
    // writer. Camera destinations must be ordinary private writable data.
    graph.Load(0);
    auto* executable=static_cast<uint8_t*>(VirtualAlloc(nullptr,objectBytes*2,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));CHECK(executable);
    std::memcpy(executable,graph.front.data(),objectBytes);std::memcpy(executable+objectBytes,graph.rear.data(),objectBytes);
    graph.views={Address(executable),Address(executable)+objectBytes};
    const std::vector<uint8_t> protectedBefore(executable,executable+objectBytes*2);Present();
    CHECK(std::equal(protectedBefore.begin(),protectedBefore.end(),executable));
    DWORD previous{};CHECK(VirtualProtect(executable,objectBytes*2,PAGE_READONLY,&previous));Present();
    CHECK(std::equal(protectedBefore.begin(),protectedBefore.end(),executable));VirtualFree(executable,0,MEM_RELEASE);
    graph.Load(0);graph.rear[0x2266]=2;const auto prior=TestCounts(0)[1];unchanged();unchanged();
    CHECK(TestCounts(0)[1]==prior+1);
    graph.Load(0);graph.rear[poseCacheValidOffset]=2;unchanged();
    graph.Load(0);Present();graph.rear[poseCacheValidOffset]=2;unchanged();
    graph.Load(0);Present();const float foreignFov=123.f;Put(graph.front.data(),0x14,foreignFov);unchanged();
    graph.Load(0);Put(image,0xe5917c,0u);Put(image,0xe61010,Address(graph.replay.data()));
    TestSetup(Address(image),request,true);unchanged();
    TestSetup(Address(image),request,false);Present();CHECK(TestCounts(0)[0]==1);
    graph.Load(0);Put(image,0xe5917c,0u);Put(image,0xe61010,Address(graph.replay.data()));
    Put(graph.parent.data(),0,0u);unchanged();
    graph.Load(0);Stop();unchanged();
    graph.Load(0);image[0x653b00]^=1;TestSetup(Address(image),request);unchanged();image[0x653b00]^=1;
    graph.Load(0);request.desktop=true;request.fovOffset=10;TestSetup(Address(image),request);
    Present();CHECK(TestCounts(0)[0]==1);
    CHECK(FloatAt(graph.front,0x14)==cars[0].original[0].values[19]+10);
    CHECK(FloatAt(graph.rear,0x14)==cars[0].original[1].values[19]+10);
    Present();CHECK(TestCounts(0)[0]==1 && TestCounts(0)[1]==0);Stop();
}
int main(int argc,char** argv) {
    try {
        CHECK(argc==2);RequestTests();PairTests();DesktopFovTests();auto* image=Image(argv[1]);LiveTests(image);SeatHostTests(image);VirtualFree(image,0,MEM_RELEASE);
        std::cout<<"25 DLC and 2 embedded base-car pairs, 32 relocated code guards, one-time pose cache refresh, unchanged executable bytes, readiness, live ownership, AI/provider refusal and reloaded graph tests passed.\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
