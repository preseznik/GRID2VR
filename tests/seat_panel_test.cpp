#include "seat_panel.h"
#include <iostream>
using namespace grid2vr::seat;
int main(int argc,char** argv) {
    if(argc!=2)return 1;
    const auto folder=std::filesystem::path(argv[1]);std::filesystem::create_directories(folder);
    PanelImage panel;Menu menu;
    if(!menu.Select(0,{10,2,-3},10,true,true) || !panel.SavePreview(folder/"seat-panel-desktop.png",L"Honda S2000",menu,false))return 1;
    if(!menu.Select(0,{-50,50,50},0,false,true) || !panel.SavePreview(folder/"seat-panel-vr.png",L"Ariel Atom 3 Supercharged",menu,true))return 1;
    if(!menu.Select(0,{12,2,3},-30,true,false) || !panel.SavePreview(folder/"seat-panel-fov-only.png",L"Caterham-Lola SP/300.R",menu,false))return 1;
    std::cout<<"Passive seat-panel captures saved; no window, input or game\n";return 0;
}
