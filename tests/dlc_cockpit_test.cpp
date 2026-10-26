#include "dlc_cockpit.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

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
    return bytes;
}
bool ChangedByte(size_t offset,size_t car,size_t view) {
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
        forward=a;reverse=b;reverse[0x19]='x';CHECK(!PatchPair(forward,reverse,table,row,hide));
        forward=a;CHECK(!PatchPair(forward,forward,table,row,hide));
        CHECK(!PatchPair(forward,std::span<uint8_t>(forward).subspan(1),table,row,hide));
    }
}
void RequestTests() {
    Request request;CHECK(ParseRequest("dlc-cockpit-v1 head-only\n240 .3611 1.0917 -.2968\n",request));
    CHECK(request.count==1 && !request.hideDriver);
    CHECK(ParseRequest("dlc-cockpit-v1 hide-driver\r\n240 .3611 1.0917 -.2968\r\n",request));CHECK(request.hideDriver);
    for(const auto* invalid:{"", "dlc-cockpit-v1 head-only\n", "dlc-cockpit-v1 head-only\n240 .3611 1.0917 -.2968",
        "dlc-cockpit-v1 head-only\n240 nan 1 -.2\n", "dlc-cockpit-v1 head-only\n240 0 99 0\n",
        "dlc-cockpit-v1 head-only\n240 1.0 1.0917 -.2968\n", "dlc-cockpit-v1 head-only\ns20 0 1 0\n",
        "dlc-cockpit-v1 unknown\n240 .3611 1.0917 -.2968\n", "dlc-cockpit-v1 head-only\n240 .3611 1.0917 -.2968 extra\n",
        "dlc-cockpit-v1 head-only\n240 .3611 1.0917 -.2968\n240 .3611 1.0917 -.2968\n"}) {
        CHECK(!ParseRequest(invalid,request));CHECK(request.count==0);
    }
    CHECK(!ParseRequest(std::string(4097,'x'),request));
}
template<class T> void Put(uint8_t* bytes,size_t offset,T value) {std::memcpy(bytes+offset,&value,sizeof(value));}
uint32_t Address(const void* p) {return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));}
uint32_t Word(std::ifstream& in) {uint32_t v{};in.read(reinterpret_cast<char*>(&v),4);CHECK(in.good());return v;}
constexpr size_t imageBytes=0xe70000;
uint8_t* Image(const char* fixture) {
    std::ifstream in(fixture,std::ios::binary);CHECK(in.good());
    auto* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,imageBytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(image);
    const auto base=reinterpret_cast<uintptr_t>(image);std::vector<uint32_t> starts;
    const auto count=Word(in);CHECK(count==30);
    for(uint32_t i=0;i<count;++i) {
        const auto rva=Word(in),size=Word(in),relocations=Word(in);CHECK(size<4096 && rva+size<imageBytes && relocations<256);
        in.read(reinterpret_cast<char*>(image+rva),size);CHECK(in.good());starts.push_back(rva);
        for(uint32_t j=0;j<relocations;++j) {
            const auto at=Word(in);CHECK(at+4<=size);uint32_t value{};std::memcpy(&value,image+rva+at,4);
            value+=static_cast<uint32_t>(base-0x400000);std::memcpy(image+rva+at,&value,4);
        }
    }
    Put(image,mountedTableRva+0x30,static_cast<uint32_t>(base+0x4b3830));CHECK(TestCodeMatches(base));
    for(auto at:starts) {image[at]^=1;CHECK(!TestCodeMatches(base));image[at]^=1;}
    image[mountedTableRva+0x30]^=1;CHECK(!TestCodeMatches(base));image[mountedTableRva+0x30]^=1;
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
    Put(image,0xe5917c,2u);
    for(size_t car=0;car<cars.size();++car) {
        graph.Load(car);const auto a=graph.front,b=graph.rear;
        const std::vector<uint8_t> codeBefore(image,image+imageBytes);
        Present();CHECK(TestCounts(car)[0]==1);
        CHECK(PatchedPair(graph.front,graph.rear,graph.base+mountedTableRva,request.rows[car],false));
        CHECK(std::equal(codeBefore.begin(),codeBefore.end(),image));
        for(size_t i=0;i<objectBytes;++i) {
            if(!ChangedByte(i,car,0))CHECK(graph.front[i]==a[i]);
            if(!ChangedByte(i,car,1))CHECK(graph.rear[i]==b[i]);
        }
        const auto patchedFront=graph.front,patchedRear=graph.rear;
        for(unsigned i=0;i<5;++i)Present();
        CHECK(TestCounts(car)[0]==1 && TestCounts(car)[1]==0 && graph.front==patchedFront && graph.rear==patchedRear);
        // An engine reload can reuse the same addresses. Original data must be
        // patched again rather than skipped by a stale address cache.
        graph.Original(car);Present();CHECK(TestCounts(car)[0]==2);
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
    graph.Load(0);Present();const float foreignFov=123.f;Put(graph.front.data(),0x14,foreignFov);unchanged();
    graph.Load(0);Put(image,0xe5917c,0u);Put(image,0xe61010,Address(graph.replay.data()));
    TestSetup(Address(image),request,true);unchanged();
    TestSetup(Address(image),request,false);Present();CHECK(TestCounts(0)[0]==1);
    graph.Load(0);Put(image,0xe5917c,0u);Put(image,0xe61010,Address(graph.replay.data()));
    Put(graph.parent.data(),0,0u);unchanged();
    graph.Load(0);Stop();unchanged();
    graph.Load(0);image[0x653b00]^=1;TestSetup(Address(image),request);unchanged();image[0x653b00]^=1;
}
int main(int argc,char** argv) {
    try {
        CHECK(argc==2);RequestTests();PairTests();auto* image=Image(argv[1]);LiveTests(image);VirtualFree(image,0,MEM_RELEASE);
        std::cout<<"25 car pairs, 30 relocated code guards, unchanged executable bytes, readiness, live ownership, AI/provider refusal and reloaded graph tests passed.\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
