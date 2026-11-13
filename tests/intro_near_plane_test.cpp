#include "intro_near_plane.h"
#include "flashback_observer_model.h"
#include <cstdio>

using namespace grid2vr;
using Action=IntroNearPlane::Action;
int main() {
    constexpr uint32_t near02=IntroNearPlane::gameNear,near005=IntroNearPlane::introNear;
    constexpr uint32_t bonnet=0x3dcccccdu,chaseReplay=0x3de147aeu,cockpit=0x3ca3d70au,above02=0x3e4cccceu;
    constexpr uint32_t far2300=0x450fc000u,far15000=0x466a6000u,far100=0x42c80000u,far101=0x42ca0000u;
    float value{};memcpy(&value,&near005,4);if(value!=.05f)return 1;
    memcpy(&value,&near02,4);if(value!=.2f)return 2;

    IntroNearPlane intro;
    // Off, or not a pre-race/replay shot: nothing changes.
    if(intro.Next(false,true,near02,far2300)!=Action::None || intro.Active())return 3;
    if(intro.Next(true,false,near02,far2300)!=Action::None || intro.Active())return 4;
    // GRID 2's short/menu pass (.2/100) and anything not exactly 0.2 m stays native.
    for(uint32_t farBits:{far100,0u,0x7f800000u,0x7fc00000u})
        if(intro.Next(true,true,near02,farBits)!=Action::None || intro.Active())return 5;
    for(uint32_t nearBits:{bonnet,chaseReplay,cockpit,above02,0x3e4cccccu,near005})
        if(intro.Next(true,true,nearBits,far2300)!=Action::None || intro.Active())return 6;
    if(intro.Next(true,true,near02,far101)!=Action::Apply)return 7; // any world far above 100 m
    intro.Reset();

    // Apply once, keep while the shots play.
    if(intro.Next(true,true,near02,far15000)!=Action::Apply || !intro.Active() || intro.Original()!=near02)return 8;
    for(int frame=0;frame<3;++frame)
        if(intro.Next(true,true,near005,far15000)!=Action::None || !intro.Active())return 9;
    // A game that writes its own near every frame is re-applied every frame.
    if(intro.Next(true,true,near02,far15000)!=Action::Apply || !intro.Active())return 10;
    // Shots end (racing, or VR stops rendering): the game's value comes back once.
    if(intro.Next(true,false,near005,far15000)!=Action::Restore || intro.Active() || intro.Original()!=near02)return 11;
    if(intro.Next(true,false,near02,far15000)!=Action::None || intro.Active())return 12;
    // A near-object/menu pass during the shots (.2/100) restores, never rewrites it.
    if(intro.Next(true,true,near02,far2300)!=Action::Apply)return 13;
    if(intro.Next(true,true,near005,far100)!=Action::Restore || intro.Active())return 14;
    // The game changed camera meanwhile (bonnet 0.1 m): its value is kept as is.
    if(intro.Next(true,true,near02,far2300)!=Action::Apply)return 15;
    if(intro.Next(true,true,bonnet,far2300)!=Action::None || intro.Active())return 16;

    // Self-check at scene entry: the native camera must have been built from it.
    if(!intro.Verified(near02,near02))return 17; // inactive: nothing to check
    if(intro.Next(true,true,near02,far2300)!=Action::Apply)return 18;
    if(!intro.Verified(near005,near005) || intro.Verified(near02,near005) || intro.Verified(near005,near02) || intro.Verified(near02,near02))return 19;

    // Phase from the flashback sample: car on track before its race monitor exists
    // (intro and start sequence), or a playing replay; never menus or racing.
    using flashbackobserver::Found;using flashbackobserver::Sample;using flashbackobserver::CinematicPhase;
    Sample s;
    if(CinematicPhase(s))return 20;                                     // no local car (menus, garage)
    s.replayActive=1;if(CinematicPhase(s))return 21;                    // no car: not a race replay
    s={};s.found=Found::NoMonitor;s.inputEnabled=0;if(!CinematicPhase(s))return 22; // Normal intro shots
    s.inputEnabled=1;if(!CinematicPhase(s))return 23;                   // grid / start sequence
    s={};s.found=Found::Monitor;s.raceState=0;s.inputEnabled=1;s.replayActive=0;if(CinematicPhase(s))return 24; // racing
    s.replayActive=1;if(!CinematicPhase(s))return 25;                   // replay of that race
    s.replayActive=-1;if(CinematicPhase(s))return 26;                   // unreadable: native
    // VR cockpit near plane (prototype): only the cockpit bank's exact 2 cm, only while
    // driving a world camera; the game's per-frame rewrite re-applies; cinematics, the
    // .2/100 near-object pass and other cameras restore or are left alone.
    CockpitNearPlane cockpitNear;const uint32_t target=cockpitNear.Target();
    if(target!=0x3dcccccdu)return 30;
    if(cockpitNear.Next(true,true,near02,far2300)!=Action::None || cockpitNear.Active())return 31;   // chase/menu .2: untouched
    if(cockpitNear.Next(false,true,cockpit,far2300)!=Action::None)return 32;                         // disabled
    if(cockpitNear.Next(true,false,cockpit,far2300)!=Action::None)return 33;                         // cinematic shots
    if(cockpitNear.Next(true,true,cockpit,far100)!=Action::None)return 34;                           // near-object pass
    if(cockpitNear.Next(true,true,cockpit,far2300)!=Action::Apply || !cockpitNear.Active())return 35;
    if(cockpitNear.Next(true,true,target,far2300)!=Action::None || !cockpitNear.Active())return 36;  // still ours
    if(cockpitNear.Next(true,true,cockpit,far2300)!=Action::Apply || !cockpitNear.Active())return 37; // game rewrote 2 cm
    if(!cockpitNear.Verified(target,target) || cockpitNear.Verified(cockpit,target) || cockpitNear.Verified(target,cockpit))return 38;
    if(cockpitNear.Next(true,false,target,far2300)!=Action::Restore || cockpitNear.Active())return 39; // cinematic: restore once
    if(cockpitNear.Next(true,true,bonnet,far2300)!=Action::None)return 40;                           // bonnet camera: never
    if(!cockpitNear.Verified(cockpit,cockpit))return 41;                                             // inactive: nothing to check
    if(CockpitNearPlane(0x3d4ccccdu).Target()!=near005)return 42;
    // The cockpit exposure limit recognises the cockpit by the game's 2 cm, also under the override.
    if(cockpitNear.GameNear(target)!=target || cockpitNear.GameNear(cockpit)!=cockpit)return 43;         // inactive: unchanged
    if(cockpitNear.Next(true,true,cockpit,far2300)!=Action::Apply)return 44;
    if(cockpitNear.GameNear(target)!=cockpit || cockpitNear.GameNear(near02)!=near02)return 45;          // ours reads as 2 cm only
    puts("Intro near plane: 5 cm only for world cameras at 0.2 m in pre-race/replay shots; restore, menu pass, camera change, self-check and phase passed.");
    puts("Cockpit near plane: only the 2 cm world camera while driving; per-frame re-apply, cinematic restore, near-object pass and self-check passed.");
    return 0;
}
