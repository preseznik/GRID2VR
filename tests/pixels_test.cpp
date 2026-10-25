#include "pixels.h"
#include <cstdio>
int main() {
    if (Rgb10ToRgb8(0)!=std::array<unsigned char,3>{0,0,0}) return 1;
    if (Rgb10ToRgb8(0xffffffff)!=std::array<unsigned char,3>{255,255,255}) return 2;
    if (Rgb10ToRgb8(1023)!=std::array<unsigned char,3>{255,0,0}) return 3;
    if (Rgb10ToRgb8(1023<<10)!=std::array<unsigned char,3>{0,255,0}) return 4;
    if (Rgb10ToRgb8((1023u<<20)|(3u<<30))!=std::array<unsigned char,3>{0,0,255}) return 5;
    if (Rgb10ToRgb8(512|(512<<10)|(512<<20))!=std::array<unsigned char,3>{128,128,128}) return 6;
    puts("10-bit capture channels, rounding and alpha separation passed.");
}
