#include "garage_presentation.h"
#include <map>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <string>

struct Memory {
    std::map<uintptr_t,uint8_t> bytes;
    void Word(uintptr_t address,uint32_t value) {
        for(unsigned i=0;i<4;++i)bytes[address+i]=static_cast<uint8_t>(value>>(8*i));
    }
    void Text(uintptr_t address,const char* value) {
        for(size_t i=0;i<=std::strlen(value);++i)bytes[address+i]=static_cast<uint8_t>(value[i]);
    }
    bool Read(uintptr_t address,void* value,size_t count) const {
        for(size_t i=0;i<count;++i)if(!bytes.count(address+i))return false;
        for(size_t i=0;i<count;++i)static_cast<uint8_t*>(value)[i]=bytes.at(address+i);
        return true;
    }
};

int main(int argc,char** argv) {
    using namespace grid2vr;
    constexpr uint32_t base=0x400000,app=0x2000000,frontend=0x2100000,wrapper=0x2200000,
        manager=0x2300000,registry=0x2400000,list=0x2500000,data=0x2600000,descriptor=0x2700000,
        map=0x2800000,entries=0x2900000,garage=0x2a00000,context=0x2b00000,key=0x2c00000,other=0x2d00000;
    Memory memory;
    const auto read=[&](uintptr_t at,void* out,size_t size){return memory.Read(at,out,size);};
    const auto state=[&]{return ReadGarageMenu(base,read).state;};
    if(state()!=GarageMenuState::Unavailable)return 1;
    memory.Word(base+0xdef804,app);memory.Word(app,base+0xce96b8);memory.Word(app+0x1428,frontend);
    memory.Word(frontend,base+0xccaf98);memory.Word(frontend+0x40,wrapper);memory.Word(frontend+0x10,context);
    memory.Word(wrapper,base+0xd53e64);memory.Word(wrapper+0x10,manager);
    memory.Word(manager,base+0xd50e0c);memory.Word(manager+8,registry);memory.Word(manager+0x1c,list);
    memory.Word(registry,base+0xd51700);memory.Word(registry+8,map);
    memory.Word(list+0x10,data);memory.Word(list+0x14,4);memory.Word(list+0x18,1);
    memory.Word(data,descriptor);memory.Word(descriptor+8,key);memory.Text(key,"57");
    memory.Word(map+4,entries);memory.Word(map+8,entries+16);
    memory.Word(entries,other);memory.Word(entries+4,0x2e00000);memory.Word(entries+8,key);memory.Word(entries+12,garage);
    memory.Text(other,"90");memory.Word(garage,base+0xcd21c0);memory.Word(garage+0x28,context);
    if(state()!=GarageMenuState::Active || ReadGarageMenu(base,read).object!=garage)return 2;
    const auto valid=memory;
    // A resident garage is insufficient: only membership of the active list counts.
    memory.Word(descriptor+8,other);
    if(state()!=GarageMenuState::Inactive)return 3;
    memory.Word(list+0x18,0);
    if(state()!=GarageMenuState::Inactive)return 4;
    memory=valid;
    for(auto address:{base+0xdef804,app,app+0x1428,frontend,frontend+0x40,wrapper,wrapper+0x10,
        manager,manager+8,manager+0x1c,registry,registry+8,list+0x10,data,descriptor+8,map+4,map+8,
        entries,entries+8,entries+12,garage,garage+0x28,frontend+0x10}) {
        memory.Word(address,1);
        if(state()!=GarageMenuState::Unavailable)return 5;
        memory=valid;
    }
    for(auto count:{5u,65u,UINT32_MAX}) {
        memory.Word(list+0x18,count);
        if(state()!=GarageMenuState::Unavailable)return 6;
    }
    memory=valid;memory.Word(map+8,entries+8193);
    if(state()!=GarageMenuState::Unavailable)return 7;
    memory=valid;memory.Word(list+0x18,2);memory.Word(data+4,descriptor);
    if(state()!=GarageMenuState::Unavailable)return 8;
    memory=valid;memory.Word(entries,key);
    if(state()!=GarageMenuState::Unavailable)return 9;
    memory=valid;memory.Text(key,"570");
    if(state()!=GarageMenuState::Inactive)return 10;
    for(auto changed:{list+0x18,list+0x14,entries+12}) {
        memory=valid;unsigned countReads{};
        const auto transition=[&](uintptr_t at,void* out,size_t size) {
            if(at==changed && ++countReads==2)memory.Word(changed,0);
            return memory.Read(at,out,size);
        };
        if(ReadGarageMenu(base,transition).state!=GarageMenuState::Unavailable)return 11;
    }
    for(auto value:{GarageMenuState::Inactive,GarageMenuState::Active,GarageMenuState::Unavailable}) {
        if(!GaragePresentationAllowed(true,value) ||
            GaragePresentationAllowed(false,value)!=(value==GarageMenuState::Inactive))return 12;
    }
    if(ReadGarageMenu(0,read).state!=GarageMenuState::Unavailable)return 13;
    if(argc==2) {
        // Bounded read-only snapshot from the actual Normal-mode garage.
        std::ifstream fixture(argv[1]);uint32_t liveBase{};fixture>>std::hex>>liveBase;
        memory.bytes.clear();uintptr_t address{};std::string bytes;
        while(fixture>>std::hex>>address>>bytes) {
            if(bytes.size()%2)return 14;
            for(size_t i=0;i<bytes.size();i+=2)memory.bytes[address+i/2]=static_cast<uint8_t>(std::stoul(bytes.substr(i,2),nullptr,16));
        }
        if(!fixture.eof() || !liveBase || ReadGarageMenu(liveBase,read).state!=GarageMenuState::Active)return 15;
    }
    std::puts("Garage classification: active vs resident states, owner tables, bounded lists, malformed/changing snapshots and default/flat presentation passed.");
}
