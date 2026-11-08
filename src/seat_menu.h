#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>

namespace grid2vr::seat {
// Whole centimetres, matching Controls -> Seat. Positive is up, forward, right.
struct Position {
    int height{},forward{},right{};
    bool operator==(const Position&) const=default;
};
inline bool Valid(Position p) {
    return p.height>=-50 && p.height<=50 && p.forward>=-50 && p.forward<=50 && p.right>=-50 && p.right<=50;
}
enum Input : unsigned {
    Open=1u<<0,Save=1u<<1,Cancel=1u<<2,Up=1u<<3,Down=1u<<4,
    Back=1u<<5,Forward=1u<<6,Left=1u<<7,Right=1u<<8,
    Wider=1u<<9,Narrower=1u<<10
};
struct Commit {int car{};Position position;int fov{};bool positions{},desktop{};};

// Render-thread state only. Input filtering, camera guards and persistence are
// separate adapters. Preview changes are cancelled on focus/eligibility loss.
class Menu {
public:
    bool Select(int car,Position saved,int fov,bool desktop,bool positions) {
        Deactivate();
        if(car<0 || car>=96 || !Valid(saved) || fov < -30 || fov > 30) {car_=-1;return false;}
        car_=car;current_=saved_=saved;fov_=savedFov_=desktop?fov:0;
        desktop_=desktop;positions_=positions;return true;
    }
    void Tick(unsigned held,uint64_t now,bool foreground,bool eligible,bool inputReady) {
        if(!foreground || !eligible || !inputReady || car_<0) {Deactivate();return;}
        const unsigned pressed=held&~previous_;previous_=held;
        disarmed_&=held;
        if(pressed&Open) {
            if(open_) {Store();open_=false;return;}
            open_=true;opening_=current_;openingFov_=fov_;
            disarmed_=held&MovementMask;lastDirections_=0;repeatAt_=0;
        }
        if(!open_)return;
        if(pressed&Cancel) {Revert();return;}
        if(pressed&Save) {Store();open_=false;return;}
        const unsigned directions=held&MovementMask&~disarmed_;
        const bool changed=directions!=lastDirections_;
        if(changed) {lastDirections_=directions;repeatAt_=now+350;}
        if(!directions || (!changed && now<repeatAt_))return;
        if(!changed)repeatAt_=now+200; // 1 cm per press, then 5 cm/s; no long-frame catch-up.
        if(positions_) {
            current_.height=Step(current_.height,directions,Up,Down);
            current_.forward=Step(current_.forward,directions,Forward,Back);
            current_.right=Step(current_.right,directions,Right,Left);
        }
        if(desktop_)fov_=std::clamp(fov_+Direction(directions,Wider,Narrower),-30,30);
    }
    void Deactivate() {
        if(open_)Revert();
        previous_=~0u;disarmed_=lastDirections_=0;repeatAt_=0;
    }
    bool Opened() const {return open_;}
    int Car() const {return car_;}
    Position Current() const {return current_;}
    int Fov() const {return fov_;}
    bool Desktop() const {return desktop_;}
    bool PositionsEnabled() const {return positions_;}
    std::optional<Commit> TakeCommit() {auto result=commit_;commit_.reset();return result;}
    void RejectSave(Position saved,int fov) {
        saved_=saved;opening_=saved;savedFov_=openingFov_=fov;
        commit_.reset();open_=true;
    }
private:
    static constexpr unsigned MovementMask=Up|Down|Back|Forward|Left|Right|Wider|Narrower;
    static int Direction(unsigned held,unsigned positive,unsigned negative) {
        return int((held&positive)!=0)-int((held&negative)!=0);
    }
    static int Step(int value,unsigned held,unsigned positive,unsigned negative) {
        return std::clamp(value+Direction(held,positive,negative),-50,50);
    }
    void Revert() {current_=opening_;fov_=openingFov_;open_=false;}
    void Store() {
        if(current_!=saved_ || fov_!=savedFov_) {
            commit_=Commit{car_,current_,fov_,positions_,desktop_};saved_=current_;savedFov_=fov_;
        }
    }
    int car_{-1},fov_{},savedFov_{},openingFov_{};
    Position current_,saved_,opening_;
    bool open_{},desktop_{},positions_{};
    unsigned previous_{~0u},disarmed_{},lastDirections_{};
    uint64_t repeatAt_{};
    std::optional<Commit> commit_;
};
}
