#include "seat_session.h"
#include <iostream>
#include <stdexcept>
#include <string>
using namespace grid2vr::seat;
#define CHECK(value) do {if(!(value))throw std::runtime_error("Failed line "+std::to_string(__LINE__)+": " #value);} while(false)
int main() {
    try {
        SessionRequest request;
        const std::string text="grid2vr-seat-menu-v1 1 1 10 0\ns20 10 2 3\npag -5 0 0\n";
        CHECK(ParseSession(text,request));const auto s20=static_cast<size_t>(FindCar("s20")),pag=static_cast<size_t>(FindCar("pag"));
        CHECK(request.count==2 && request.Find(s20)->saved.height==10 && request.Applied(s20).position.height==10);
        CommitBuffer saved(request,std::string(64,'a'));
        CHECK(saved.Save({static_cast<int>(s20),{12,2,3},20,true,true}));
        CHECK(saved.Save({static_cast<int>(pag),{-6,0,0},20,true,true}));
        CHECK(saved.Text().find("s20 12 2 3\n")!=std::string::npos && saved.Text().find("pag -6 0 0\n")!=std::string::npos && saved.Fov()==20);
        CHECK(saved.Saved(s20).height==12);
        CHECK(saved.Save({static_cast<int>(s20),{10,2,3},10,true,true}));CHECK(saved.Text().find("s20 ")==std::string::npos && saved.Fov()==10);
        const auto before=saved.Text();CHECK(!saved.Save({static_cast<int>(s20),{51,0,0},0,true,true}) && saved.Text()==before);
        CHECK(!saved.Save({static_cast<int>(s20),{},0,true,false}) && saved.Text()==before);
        SessionRequest fov;CHECK(ParseSession("grid2vr-seat-menu-v1 1 0 -10 1\ns20 12 0 0\n",fov));
        CHECK(fov.Find(s20)->saved.height==12 && fov.Applied(s20).position.height==0 && fov.Applied(s20).hideDriver);
        CommitBuffer fovOnly(fov,std::string(64,'b'));CHECK(fovOnly.Save({static_cast<int>(s20),{12,0,0},-20,false,true}));
        CHECK(fovOnly.Text().find("s20")==std::string::npos && fovOnly.Fov()==-20);
        for(const auto& invalid: {"", "grid2vr-seat-menu-v1 1 1 0 0\n", "grid2vr-seat-menu-v1 0 1 1 0\ns20 0 0 0\n", "grid2vr-seat-menu-v1 1 1 31 0\ns20 0 0 0\n",
            "grid2vr-seat-menu-v1 1 1 0 0 extra\ns20 0 0 0\n", "grid2vr-seat-menu-v1 1 1 0 0\ns20 51 0 0\n", "grid2vr-seat-menu-v1 1 1 0 0\ns20 0 0 0\ns20 1 0 0\n", "grid2vr-seat-menu-v1 1 1 0 0\nbad 0 0 0\n"}) {
            SessionRequest bad=request;CHECK(!ParseSession(invalid,bad));CHECK(bad.count==request.count && bad.fov==request.fov);
        }
        CHECK(CommitBuffer(request,"bad").Text().empty());
        std::cout<<"Seat session: bounded requests, calibrated identities, FOV-only separation, commits and rollback-to-original passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
