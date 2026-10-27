#pragma once
#include "chase_camera_behavior.h"
#include <array>
#include <charconv>
#include <string_view>

namespace grid2vr {
// One bounded ASCII record. Reject extra fields, signs, embedded NULs and
// truncated requests rather than partially applying a launcher's settings.
inline bool ParseChaseCameraRequest(std::string_view text,ChaseCameraConfig& config) {
    constexpr std::string_view prefix="chase-v1 ";
    if(text.size()>80 || !text.starts_with(prefix) || !text.ends_with('\n'))return false;
    text.remove_prefix(prefix.size());text.remove_suffix(1);
    std::array<int,7> fields{};
    for(size_t i=0;i<fields.size();++i) {
        const auto end=text.find(' ');
        const auto token=text.substr(0,end);
        if(token.empty() || token.size()>4 || token.front()<'0' || token.front()>'9')return false;
        const auto parsed=std::from_chars(token.data(),token.data()+token.size(),fields[i]);
        if(parsed.ec!=std::errc{} || parsed.ptr!=token.data()+token.size())return false;
        if(i+1==fields.size()) { if(end!=std::string_view::npos)return false; }
        else { if(end==std::string_view::npos)return false;text.remove_prefix(end+1); }
    }
    if(fields[0]>1 || fields[1]>1 || fields[2]>2 || fields[6]>1)return false;
    ChaseCameraConfig parsed{};
    parsed.enabled=fields[0]!=0;parsed.keepAngle=fields[1]!=0;
    parsed.mouseMode=static_cast<ChaseMouseMode>(fields[2]);
    parsed.maxZoomOutPercent=fields[3];parsed.mouseSensitivityPercent=fields[4];
    parsed.stickSensitivityPercent=fields[5];parsed.invertVertical=fields[6]!=0;
    if(!parsed.Valid())return false;
    config=parsed;return true;
}
}
