#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <cmath>
namespace grid2vr::chasecamera {
using LookPacket=std::array<uint32_t,5>;
inline bool NeutralLookPacket(uintptr_t base,const LookPacket& input,LookPacket& output) {
    // Native producer511710 constructs20-byte packets. The manager broadcasts
    // the same object to every camera, so modify only our private copy.
    float horizontal{},vertical{};
    std::memcpy(&horizontal,&input[2],4);std::memcpy(&vertical,&input[3],4);
    if(input[0]!=base+0xce095c || (input[1]!=1 && input[1]!=2) ||
       (input[4]&0xff)>1 || !std::isfinite(horizontal) || !std::isfinite(vertical))return false;
    output=input;output[2]=output[3]=0;return true;
}
}
