#include "seat_camera.h"
#include <iostream>
#include <stdexcept>
#include <string>
using namespace grid2vr::seat;
#define CHECK(value) do {if(!(value))throw std::runtime_error("Failed line "+std::to_string(__LINE__)+": " #value);} while(false)
int main() {
    try {
        constexpr uint32_t table=0x12340000;
        for(size_t car=0;car<CarCount;++car)for(bool hide:{false,true})for(bool desktop:{false,true}) {
            CHECK(FindCar(CarCode(car))==static_cast<int>(car));
            CameraSettings from{car,{10,2,3},desktop?10:0,desktop,hide};CameraPair pair;
            CHECK(ExpectedPair(from,table,pair));
            for(auto& object:pair)object[grid2vr::dlccockpit::poseCacheValidOffset]=1;
            const auto before=pair;
            CameraSettings to=from;to.position={-20,-30,40};to.fov=desktop?-30:0;
            CHECK(EditPair(pair[0],pair[1],table,from,to));
            CameraPair expected;CHECK(ExpectedPair(to,table,expected));
            CHECK(Matches(pair[0],expected[0],to,0,table) && Matches(pair[1],expected[1],to,1,table));
            for(size_t view=0;view<2;++view)for(size_t byte=0;byte<pair[view].size();++byte) {
                const bool written=(byte>=0x2230 && byte<0x223c) || (desktop && byte>=0x14 && byte<0x18) || byte==grid2vr::dlccockpit::poseCacheValidOffset;
                if(!written)CHECK(pair[view][byte]==before[view][byte]);
            }
            CHECK(pair[0][grid2vr::dlccockpit::poseCacheValidOffset]==0 && pair[1][grid2vr::dlccockpit::poseCacheValidOffset]==0);
            CHECK(EditPair(pair[0],pair[1],table,to,from));
            for(auto at:grid2vr::dlccockpit::floatOffsets)CHECK(grid2vr::dlccockpit::FloatAt(pair[0],at)==grid2vr::dlccockpit::FloatAt(before[0],at));
            for(size_t byte:{size_t(0),size_t(0x19),size_t(0x98),size_t(0x2233),size_t(0x2266)}) {
                pair=before;pair[0][byte]^=0x40;const auto tampered=pair;
                CHECK(!EditPair(pair[0],pair[1],table,from,to) && pair==tampered);
            }
            pair=before;auto invalid=to;invalid.position.height=51;
            CHECK(!EditPair(pair[0],pair[1],table,from,invalid) && pair==before);
            invalid=to;invalid.fov=31;CHECK(!EditPair(pair[0],pair[1],table,from,invalid) && pair==before);
            CHECK(!EditPair(pair[0],pair[0],table,from,to) && pair==before);
        }
        CHECK(FindCar("bad")==-1 && CarCount==76);
        std::cout<<"76 calibrated cars: live seat/FOV fields, forward/reverse pair, exact restoration and no-write-on-refusal passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
