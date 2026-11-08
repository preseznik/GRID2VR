#include "seat_menu.h"
#include <iostream>
#include <stdexcept>
#include <string>
using namespace grid2vr::seat;
#define CHECK(value) do {if(!(value))throw std::runtime_error("Failed line "+std::to_string(__LINE__)+": " #value);} while(false)
int main() {
    try {
        Menu menu;uint64_t now{};
        auto tick=[&](unsigned keys,bool foreground=true,bool eligible=true,bool ready=true) {menu.Tick(keys,now,foreground,eligible,ready);now+=10;};
        auto press=[&](unsigned key) {tick(0);tick(key);tick(0);};
        CHECK(menu.Select(2,{10,2,3},10,true,true));
        tick(Open);CHECK(!menu.Opened()); // A key held before activation must be released.
        press(Open);CHECK(menu.Opened());
        press(Up);press(Forward);press(Left);press(Wider);
        CHECK((menu.Current()==Position{11,3,2}) && menu.Fov()==11 && !menu.TakeCommit());
        press(Cancel);CHECK(!menu.Opened() && (menu.Current()==Position{10,2,3}) && menu.Fov()==10);
        press(Open);press(Down);press(Narrower);press(Save);
        auto saved=menu.TakeCommit();CHECK(saved && saved->car==2 && saved->position.height==9 && saved->fov==9 && saved->positions && saved->desktop);
        CHECK(!menu.TakeCommit() && !menu.Opened());
        // A failed disk save reopens the edited preview. Cancel returns to the
        // last persisted values; retry emits the same change once.
        press(Open);press(Up);press(Save);CHECK(menu.TakeCommit());
        menu.RejectSave({9,2,3},9);CHECK(menu.Opened() && menu.Current().height==10 && !menu.TakeCommit());
        press(Cancel);CHECK(!menu.Opened() && menu.Current().height==9);
        press(Open);press(Up);press(Save);CHECK(menu.TakeCommit());
        menu.RejectSave({9,2,3},9);press(Save);
        saved=menu.TakeCommit();CHECK(saved && saved->position.height==10 && !menu.Opened());
        CHECK(menu.Select(2,{9,2,3},9,true,true));
        press(Open);press(Up);press(Open);CHECK(!menu.Opened() && menu.TakeCommit());
        press(Open);press(Up);tick(0,false);CHECK(!menu.Opened() && menu.Current().height==10 && !menu.TakeCommit());
        press(Open);press(Up);tick(0,true,false);CHECK(!menu.Opened() && menu.Current().height==10);
        press(Open);press(Up);tick(0,true,true,false);CHECK(!menu.Opened() && menu.Current().height==10);
        press(Open);press(Up);CHECK(menu.Select(3,{-4,0,0},9,true,true));CHECK(!menu.Opened() && menu.Current().height==-4 && !menu.TakeCommit());
        tick(0);tick(Up);tick(Open|Up);CHECK(menu.Opened() && menu.Current().height==-4);
        tick(Up);CHECK(menu.Current().height==-4);tick(0);tick(Up);CHECK(menu.Current().height==-3);
        now+=340;tick(Up);CHECK(menu.Current().height==-2); // 350 ms since the press.
        now+=10000;tick(Up);CHECK(menu.Current().height==-1); // One repeat, no catch-up jump.
        press(Up|Down|Forward|Back|Left|Right|Wider|Narrower);CHECK((menu.Current()==Position{-1,0,0}) && menu.Fov()==9);
        CHECK(menu.Select(0,{50,-50,50},30,true,true));press(Open);press(Up|Back|Right|Wider);
        CHECK((menu.Current()==Position{50,-50,50}) && menu.Fov()==30);
        CHECK(menu.Select(0,{-50,50,-50},-30,true,true));press(Open);press(Down|Forward|Left|Narrower);
        CHECK((menu.Current()==Position{-50,50,-50}) && menu.Fov()==-30);
        CHECK(menu.Select(1,{12,0,0},10,true,false));press(Open);press(Up);press(Wider);press(Save);
        saved=menu.TakeCommit();CHECK(saved && !saved->positions && saved->desktop && saved->position.height==12 && saved->fov==11);
        CHECK(menu.Select(1,{12,0,0},10,false,true));press(Open);press(Wider);press(Save);CHECK(menu.Fov()==0 && !menu.TakeCommit());
        for(auto bad: {Position{51,0,0},Position{0,-51,0},Position{0,0,51}})CHECK(!menu.Select(0,bad,0,true,true));
        CHECK(!menu.Select(96,{},0,true,true) && !menu.Select(0,{},31,true,true));press(Open);CHECK(!menu.Opened());
        std::cout<<"Seat menu: save/cancel, fresh input, eligibility/focus loss, car change, bounded repeat, limits and Desktop/VR separation passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
