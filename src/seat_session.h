#pragma once
#include "seat_camera.h"
#include <charconv>
#include <sstream>

namespace grid2vr::seat {
struct SessionRow {size_t car{};Position saved;};
struct SessionRequest {
    bool desktop{},positions{},hideDriver{};
    int fov{};size_t count{};
    std::array<SessionRow,CarCount> rows{};
    const SessionRow* Find(size_t car) const {
        for(size_t i=0;i<count;++i)if(rows[i].car==car)return &rows[i];
        return nullptr;
    }
    CameraSettings Applied(size_t car) const {
        const auto* row=Find(car);
        return {car,positions && row?row->saved:Position{},fov,desktop,hideDriver};
    }
};
inline bool Number(const std::string& text,int low,int high,int& value) {
    if(text.empty() || text.size()>4)return false;
    const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
    return result.ec==std::errc{} && result.ptr==text.data()+text.size() && value>=low && value<=high;
}
inline bool ParseSession(std::string_view text,SessionRequest& output) {
    if(text.empty() || text.size()>8192)return false;
    for(unsigned char byte:text)if(byte!=10 && byte!=13 && (byte<32 || byte>126))return false;
    std::istringstream input{std::string(text)};std::string line,protocol,d,p,f,h,extra;
    if(!std::getline(input,line))return false;
    std::istringstream header(line);int desktop{},positions{},fov{},hide{};
    if(!(header>>protocol>>d>>p>>f>>h) || protocol!="grid2vr-seat-menu-v1" || (header>>extra) ||
       !Number(d,0,1,desktop) || !Number(p,0,1,positions) || !Number(f,-30,30,fov) || !Number(h,0,1,hide) || (!desktop && fov))return false;
    SessionRequest next;next.desktop=desktop!=0;next.positions=positions!=0;next.hideDriver=hide!=0;next.fov=fov;
    while(std::getline(input,line)) {
        std::istringstream row(line);std::string code,height,forward,right;
        Position offset;
        if(next.count==CarCount || !(row>>code>>height>>forward>>right) || (row>>extra) ||
           !Number(height,-50,50,offset.height) || !Number(forward,-50,50,offset.forward) || !Number(right,-50,50,offset.right))return false;
        const int car=FindCar(code);if(car<0 || next.Find(static_cast<size_t>(car)))return false;
        next.rows[next.count++]={static_cast<size_t>(car),offset};
    }
    if(!next.count)return false;
    output=next;return true;
}
class CommitBuffer {
public:
    CommitBuffer(const SessionRequest& request,std::string digest) : request_(request),digest_(std::move(digest)),fov_(request.fov) {}
    bool Save(const Commit& value) {
        if(value.car<0 || !request_.Find(static_cast<size_t>(value.car)) || !Valid(value.position) ||
           value.fov < -30 || value.fov > 30 || value.desktop!=request_.desktop || value.positions!=request_.positions || (!value.desktop && value.fov))return false;
        if(value.positions) {
            const auto car=static_cast<size_t>(value.car);
            if(value.position==request_.Find(car)->saved)changed_[car].reset();else changed_[car]=value.position;
        }
        fov_=value.fov;return true;
    }
    Position Saved(size_t car) const {
        const auto* row=request_.Find(car);if(!row)return {};
        return changed_[car].value_or(row->saved);
    }
    int Fov() const {return fov_;}
    std::string Text() const {
        if(digest_.size()!=64 || digest_.find_first_not_of("0123456789abcdef")!=std::string::npos)return {};
        std::string result="grid2vr-seat-commit-v1 "+digest_+" "+std::to_string(fov_)+"\n";
        for(size_t car=0;car<CarCount;++car)if(changed_[car]) {
            const auto value=*changed_[car];
            result+=std::string(CarCode(car))+" "+std::to_string(value.height)+" "+std::to_string(value.forward)+" "+std::to_string(value.right)+"\n";
        }
        return result;
    }
private:
    SessionRequest request_;
    std::string digest_;
    int fov_{};
    std::array<std::optional<Position>,CarCount> changed_;
};
}
