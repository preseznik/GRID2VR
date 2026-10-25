#pragma once
#include <array>
#include <cstdint>

inline std::array<unsigned char,3> Rgb10ToRgb8(uint32_t pixel) {
    auto channel=[](uint32_t value) { return static_cast<unsigned char>((value*255+511)/1023); };
    return {channel(pixel&1023),channel((pixel>>10)&1023),channel((pixel>>20)&1023)};
}
