#include "chase_camera_request.h"
#include <iostream>
#include <string>
int main() {
    using namespace grid2vr;
    ChaseCameraConfig value{};
    if(!ParseChaseCameraRequest("chase-v1 0 0 1 180 100 100 0\n",value) || value.enabled || value.keepAngle ||
        value.mouseMode!=ChaseMouseMode::Right || value.maxZoomOutPercent!=180)return 1;
    if(!ParseChaseCameraRequest("chase-v1 1 1 2 1000 25 300 1\n",value) || !value.enabled || !value.invertVertical)return 2;
    constexpr const char* invalid[]{"", "chase-v2 1 0 1 180 100 100 0\n", "chase-v1 1 0 1 180 100 100 0",
        "chase-v1 2 0 1 180 100 100 0\n", "chase-v1 1 0 3 180 100 100 0\n",
        "chase-v1 1 0 1 181 100 100 0\n", "chase-v1 1 0 1 170 100 100 0\n",
        "chase-v1 1 0 1 1010 100 100 0\n", "chase-v1 1 0 1 180 24 100 0\n",
        "chase-v1 1 0 1 180 100 301 0\n", "chase-v1 1 0 1 180 100 100 2\n",
        "chase-v1 +1 0 1 180 100 100 0\n", "chase-v1 -1 0 1 180 100 100 0\n",
        "chase-v1 1  0 1 180 100 100 0\n", "chase-v1 1 0 1 180 100 100 0 \n",
        "chase-v1 1 0 1 180 100 100 0\nextra", "chase-v1 1 0 1 180 100 100 0\r\n"};
    for(const auto text:invalid)if(ParseChaseCameraRequest(text,value)) { std::cerr<<text;return 3; }
    std::string nul="chase-v1 1 0 1 180 100 100 0\n";nul[12]='\0';
    if(ParseChaseCameraRequest(nul,value) || ParseChaseCameraRequest(std::string(81,'1'),value))return 4;
    if(!value.enabled || value.maxZoomOutPercent!=1000)return 5; // Failure is transactional.
    return 0;
}
