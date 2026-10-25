#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace grid2vr::instancecapacity {
constexpr unsigned LaneCount=16;
constexpr uint32_t MaxCapacity=16384;
constexpr uint32_t Stride=40;
// Exact observed native layouts. Candidate XML changes only high/main_scene;
// test and medium retain the game's existing capacities.
inline bool ExpectedCapacities(uint32_t style,uint32_t count,bool expanded,
                               std::array<uint32_t,LaneCount>& output) {
    if(count!=10)return false;
    if(style==2)output={1800,0,1500,1500,100,100,100,100,100,100,1600,100,100,100,1800,400};
    else if(style==3)output={1200,0,500,500,100,100,100,100,100,100,1000,100,100,100,1200,400};
    else if(style==4)output={800,0,300,300,100,100,100,100,100,100,350,100,100,100,800,400};
    else return false;
    if(expanded && style==3)output[0]=output[14]=4096;
    return true;
}
// RVA 0x185965 refuses equality too; evaluate in wider arithmetic so a bad
// input cannot wrap the game's signed used + requested comparison.
inline bool ReserveFits(int used,int count,int capacity) {
    return used>=0 && count>=0 && capacity>0 &&
        static_cast<int64_t>(used)+count<capacity;
}

// Failed native reservations do not advance their used count. Sum every
// request, including failures, to retain actual demand for a bounded frame set.
// External synchronization belongs to the hook, which already owns its log lock.
class RequestTotals {
public:
    static constexpr size_t MaxFrames=2048;
    bool Add(uint64_t frame,int lane,int count) {
        if(lane<0 || lane>=static_cast<int>(LaneCount) || count<0)return false;
        size_t index=last_;
        if(index>=size_ || frames_[index].id!=frame) {
            for(index=0;index<size_ && frames_[index].id!=frame;++index){}
        }
        if(index==size_) {
            if(size_==MaxFrames){truncated_=true;return false;}
            frames_[index]={};frames_[index].id=frame;++size_;
        }
        auto& value=frames_[index].demand[static_cast<unsigned>(lane)];
        if(value>std::numeric_limits<uint64_t>::max()-static_cast<uint64_t>(count)) {
            truncated_=true;return false;
        }
        value+=static_cast<uint64_t>(count);last_=index;
        auto& maximum=maxima_[static_cast<unsigned>(lane)];
        if(value>maximum)maximum=value;
        return true;
    }
    uint64_t MaxDemand(unsigned lane) const {return lane<LaneCount?maxima_[lane]:0;}
    size_t FrameCount() const {return size_;}
    bool Truncated() const {return truncated_;}
    void Reset(){size_=0;last_=0;maxima_={};truncated_=false;}
private:
    struct Frame {uint64_t id{};std::array<uint64_t,LaneCount> demand{};};
    std::array<Frame,MaxFrames> frames_{};
    std::array<uint64_t,LaneCount> maxima_{};
    size_t size_{},last_{};
    bool truncated_{};
};
}
