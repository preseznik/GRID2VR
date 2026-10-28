#include "driving_controls_producer.h"
#include <iostream>
#include <stdexcept>
using namespace grid2vr::drivingcontrols;
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<class T> void Put(std::array<uint8_t,0x6c>& data,size_t offset,T value) {std::memcpy(data.data()+offset,&value,sizeof(value));}
template<class T> T Get(const std::array<uint8_t,0x6c>& data,size_t offset) {T value;std::memcpy(&value,data.data()+offset,sizeof(value));return value;}
int main() {
    for(const auto initial:std::array<std::array<uint32_t,2>,4>{{{4,1},{2,1},{2,2},{1,2}}}) {
        for(unsigned mode=1;mode<=4;++mode) {
            std::array<uint8_t,0x6c> current{};current.fill(0xa5);current[0x40]=1;Put(current,4,initial[0]);Put(current,0x44,initial[1]);
            const auto before=current;const auto expected=std::array<std::array<uint32_t,2>,4>{{{4,1},{2,1},{2,2},{1,2}}}[mode-1];
            Check(ApplyTransmission(current,static_cast<Transmission>(mode)) && Get<uint32_t>(current,4)==expected[0] && Get<uint32_t>(current,0x44)==expected[1],
                "all native transmission transitions use the verified engine and query modes");
            for(size_t i=0;i<current.size();++i)if(!(i>=4 && i<8) && !(i>=0x44 && i<0x48))Check(current[i]==before[i],"transmission retains forced clutch, device and output state");
            Check(!ApplyTransmission(current,Transmission::Game),"game setting stays native");
            current[0x40]=0;const auto inactive=current;Check(!ApplyTransmission(current,Transmission::Automatic) && current==inactive,"inactive transmission untouched");
            current[0x40]=1;Put(current,4,uint32_t{7});const auto unknown=current;Check(!ApplyTransmission(current,Transmission::Automatic) && current==unknown,"unknown gearbox shape refused");
        }
    }
    Check(NativeInputEnabled(0,1) && NativeInputEnabled(0x100,1),"only native low-byte block mask applies");
    Check(!NativeInputEnabled(1,1) && !NativeInputEnabled(0xff,1) && !NativeInputEnabled(0,0),"native pause/disabled gates apply");
    Check(HPatternIndex("Gear 1")==0 && HPatternIndex("Gear Reverse")==7 && HPatternIndex("Gear Up")==-1 && HPatternIndex("Gear 8")==-1,"only verified H-pattern actions are supplemental");
    std::array<uint8_t,0x6c> native{};native.fill(0xa5);
    Put(native,0x3c,uint32_t{0});Put(native,0x44,uint32_t{2});Put(native,0x34,uint32_t{1});Put(native,0x24,int32_t{9});Put(native,0x30,.2f);native[0x40]=1;
    SupplementalInput custom;custom.clutchConnected=true;custom.clutch=.8f;custom.gears[2]=true;
    auto state=native;Check(ApplySupplementalInput(state,custom),"custom clutch and third gear delivered");
    Check(Get<float>(state,0x30)==.8f && Get<int32_t>(state,0x24)==3,"native output offsets and gear representation");
    for(size_t i=0;i<state.size();++i)if(!(i>=0x24 && i<0x28) && !(i>=0x30 && i<0x34))Check(state[i]==native[i],"unrelated controller state retained byte-for-byte");
    for(unsigned gear=0;gear<8;++gear) {custom.gears={};custom.gears[gear]=true;state=native;ApplySupplementalInput(state,custom);
        Check(Get<int32_t>(state,0x24)==(gear==7?-1:int32_t(gear+1)),"all H gears/reverse route");}
    custom.gears[0]=true;state=native;Put(state,0x24,int32_t{4});ApplySupplementalInput(state,custom);
    Check(Get<int32_t>(state,0x24)==9,"conflicting shifter contacts fail neutral");
    custom.gears={};custom.clutch=0;state=native;Put(state,0x24,int32_t{5});
    Check(!ApplySupplementalInput(state,custom) && Get<int32_t>(state,0x24)==5 && Get<float>(state,0x30)==.2f,"released lever/pedal retain native keyboard/controller outputs");
    custom.gears[1]=true;custom.clutch=.9f;
    for(uint32_t mode:{0u,1u}) {state=native;Put(state,0x44,mode);ApplySupplementalInput(state,custom);
        Check(Get<int32_t>(state,0x24)==9,"automatic/sequential gear mode never forced");}
    state=native;Put(state,0x3c,uint32_t{1});ApplySupplementalInput(state,custom);
    Check(Get<float>(state,0x30)==.2f,"native forced-clutch state wins");
    custom.gears={};
    for(float value:{0.f,.5f,.979f,std::nextafter(.98f,0.f),.98f,.987919f,.988281f,1.f}) {
        custom.clutch=value;state=native;Put(state,0x30,0.f);const auto before=state;
        ApplySupplementalInput(state,custom);
        Check(Get<float>(state,0x30)==(value>=.98f?1.f:value),"manual clutch retains analog travel and reaches exact full at measured pedal end stop");
        for(size_t i=0;i<state.size();++i)if(i<0x30 || i>=0x34)Check(state[i]==before[i],"end-stop tolerance touches only clutch output");
    }
    custom.clutch=.988281f;
    for(const auto pair:std::array<std::array<uint32_t,2>,4>{{{4,1},{2,1},{2,2},{1,1}}}) {
        state=native;Put(state,0x34,pair[0]);Put(state,0x44,pair[1]);ApplySupplementalInput(state,custom);
        Check(Get<float>(state,0x30)==custom.clutch,"tolerance applies only to manual H-pattern with clutch");
    }
    state=native;Put(state,0x3c,uint32_t{1});ApplySupplementalInput(state,custom);
    Check(Get<float>(state,0x30)==.2f,"end-stop tolerance cannot override forced native clutch");
    custom.clutchConnected=false;state=native;Put(state,0x30,.99f);const auto nativeOnly=state;
    Check(!ApplySupplementalInput(state,custom) && state==nativeOnly,"unbound clutch stays native even near full travel");
    custom.clutchConnected=true;custom.clutch=.1f;state=native;Put(state,0x30,.99f);
    Check(!ApplySupplementalInput(state,custom) && Get<float>(state,0x30)==.99f,"tolerance requires the assigned pedal itself near its end stop");
    state=native;state[0x40]=0;const auto inactive=state;
    Check(!ApplySupplementalInput(state,custom) && state==inactive,"inactive controller stays native");
    for(uint32_t mode:{3u,0xffffffffu}) {state=native;Put(state,0x44,mode);auto before=state;
        Check(!ApplySupplementalInput(state,custom) && state==before,"unknown transmission shape refused");}
    state=native;Put(state,0x24,int32_t{8});const auto unknown=state;
    Check(!ApplySupplementalInput(state,custom) && state==unknown,"unknown H gear representation refused");
    custom.clutch=std::nanf("");state=native;
    Check(!ApplySupplementalInput(state,custom) && state==native,"nonfinite sample refused");
    Check(!ApplySupplementalInput(std::span(state).first(0x68),{}),"short controller refused");
    std::cout<<"Guarded H-pattern and clutch output composition passed.\n";
}
