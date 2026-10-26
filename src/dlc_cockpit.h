#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <locale>
#include <sstream>
#include <span>
#include <string>
#include <string_view>

namespace grid2vr::dlccockpit {
void Prepare(uintptr_t base,const std::filesystem::path& output);
void Present();
void Stop();
#ifdef GRID2VR_DLC_COCKPIT_TEST
struct Request;
bool TestCodeMatches(uintptr_t image);
void TestSetup(uintptr_t image,const Request& settings,bool nativeLaunch=false);
std::array<uint64_t,2> TestCounts(size_t car);
#endif

// Exact GRID 2 1.0.85.8679 Mounted parser layout. The runtime verifies the
// executable, instruction sites, vtable, identities and original values first.
inline constexpr size_t objectBytes=0x2270;
inline constexpr uint32_t mountedTableRva=0xd09ef0;
inline constexpr std::array<size_t,22> floatOffsets{
    0x2230,0x2234,0x2238,0x2240,0x2248,0x10,
    0x2200,0x2204,0x2208,0x2210,0x2214,0x2218,0x2220,0x2224,0x2228,
    0x2260,0x225c,0x2254,0x2244,0x14,0x2258,0xf4};
inline constexpr size_t checkedFloatCount=floatOffsets.size();
inline constexpr std::array<size_t,17> flagOffsets{
    0x2266,0x2268,0x2267,0x1ed4,0x1f14,0x2068,0x20e4,0x214c,0x21b4,
    0x1df4,0x1df5,0x1df6,0x1df7,0x1a64,0x1e84,0x1e87,0x1e86};
struct Original {std::array<float,checkedFloatCount> values;std::array<int8_t,17> flags;};
struct Car {const char* code;std::array<float,3> seat;std::array<Original,2> original;};
// Native source fingerprints and field provenance are recorded in
// docs/evidence/dlc-cockpit-native-20261026.json. Seats come from the authored
// driver animation and steering-dummy alignment, not a guessed bonnet offset.
inline constexpr std::array<Car,25> cars{{
    {"240",{0.3611f,1.0917f,-0.2968f},{{{{0.f,1.0561f,0.8473f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}},{{0.f,1.1952f,-1.2507f,0.1228f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.1423f,60.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}}}}},
    {"370",{0.3608f,1.1138f,-0.3119f},{{{{0.f,1.1223f,0.5402f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}},{{0.f,1.2202f,-1.3271f,0.085f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.1423f,60.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}}}}},
    {"722",{0.3594f,1.0163f,-0.8145f},{{{{0.f,1.0475f,0.655f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,-1,-1,-1}},{{0.f,1.578f,-0.97f,0.0444f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.142f,65.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,-1,-1,-1}}}}},
    {"b12",{0.3597f,1.1699f,-0.1136f},{{{{0.f,1.112f,0.735f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}},{{0.f,1.2311f,-1.6153f,0.1032f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.1423f,65.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}}}}},
    {"bz4",{0.3291f,1.0349f,-0.5135f},{{{{0.f,1.0433f,0.7662f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}},{{0.f,1.2455f,-1.0832f,0.1015f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.142f,60.0155f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}}}}},
    {"cam",{0.4229f,1.0859f,-0.3768f},{{{{0.f,1.109f,0.9219f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}},{{0.f,1.2007f,-1.5254f,0.0839f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.1423f,65.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}}}}},
    {"ccg",{0.3261f,0.8717f,-0.084f},{{{{0.f,0.822f,1.135f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}},{{0.f,1.079f,-0.739f,-0.025f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.142f,60.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}}}}},
    {"civ",{-0.3691f,1.1663f,-0.0892f},{{{{0.f,1.0919f,1.1998f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}},{{0.f,1.3275f,-1.5028f,0.085f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.142f,64.8f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}}}}},
    {"cr2",{0.4079f,1.1742f,0.0041f},{{{{0.f,1.169f,1.2817f,0.0994f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}},{{0.f,1.0758f,-1.8643f,0.069f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.1423f,70.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}}}}},
    {"e32",{0.3552f,1.1194f,0.0307f},{{{{0.f,1.0497f,0.9808f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}},{{0.f,1.3413f,-1.3238f,0.085f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.142f,65.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}}}}},
    {"evx",{-0.3697f,1.1969f,-0.0808f},{{{{0.f,1.0666f,1.0577f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}},{{0.f,1.722f,-1.444f,0.1032f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.1423f,65.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}}}}},
    {"go2",{0.3592f,1.1256f,-0.1167f},{{{{0.f,1.1405f,0.8699f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}},{{0.f,1.6233f,-1.3459f,0.085f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.142f,60.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}}}}},
    {"gtr",{-0.4f,1.1358f,-0.1379f},{{{{0.f,1.134f,0.81f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,-1,-1,-1}},{{0.f,1.243f,-1.437f,-0.062f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.142f,65.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,-1,-1,-1}}}}},
    {"ind",{0.0023f,0.8254f,-0.0317f},{{{{0.f,0.9572f,0.1521f,0.0741f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,55.0045f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}},{{0.f,1.051f,-1.462f,0.0873f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.142f,55.0002f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}}}}},
    {"ma2",{0.4242f,1.1342f,-0.2331f},{{{{0.f,1.085f,1.422f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,-1,-1,-1}},{{0.f,1.184f,-1.556f,0.0815f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.142f,65.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,-1,-1,-1}}}}},
    {"mpg",{0.3079f,1.0083f,-0.0863f},{{{{0.f,0.9222f,1.1717f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}},{{0.f,1.5557f,-1.3076f,0.1875f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.142f,65.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}}}}},
    {"r32",{-0.3717f,1.097f,-0.1955f},{{{{0.f,1.0814f,0.7434f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,-1,-1,-1}},{{0.f,1.3308f,-1.0361f,0.1875f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.142f,65.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,-1,-1,-1}}}}},
    {"r3b",{-0.3717f,1.0834f,-0.2104f},{{{{0.f,1.0814f,0.7434f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}},{{0.f,1.226f,-1.31f,-0.023f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.142f,65.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}}}}},
    {"rx2",{-0.3723f,1.0006f,-0.2959f},{{{{0.f,1.0614f,0.5078f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,-1,-1,-1}},{{0.f,1.4498f,-1.1325f,0.0523f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.142f,65.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,-1,-1,-1}}}}},
    {"srs",{-0.3721f,1.1649f,-0.0869f},{{{{0.f,1.1703f,0.6789f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}},{{0.f,1.5375f,-1.3073f,0.085f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.1423f,60.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}}}}},
    {"utf",{-0.4095f,1.2634f,-0.0839f},{{{{0.f,1.159f,1.107f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}},{{0.f,1.703f,-1.764f,0.1143f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.142f,65.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}}}}},
    {"wrx",{-0.3403f,1.0681f,-0.1255f},{{{{0.f,0.9722f,0.8828f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,-1,-1,-1}},{{0.f,1.5326f,-0.8095f,-0.0741f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.142f,65.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,-1,-1,-1}}}}},
    {"z22",{0.3674f,1.1081f,-0.2973f},{{{{0.f,1.095f,1.038f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,-1,-1,-1}},{{0.f,1.1818f,-1.4808f,0.2103f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.1423f,65.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,-1,-1,-1}}}}},
    {"zig",{0.3347f,1.0414f,-0.443f},{{{{0.f,1.0319f,0.6898f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}},{{0.f,1.2694f,-1.0635f,0.1872f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.142f,60.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,0,0,0}}}}},
    {"zon",{0.3121f,0.9896f,0.306f},{{{{0.f,0.95f,1.294f,0.085f,0.f,0.1f,0.f,0.04f,0.02f,0.2f,0.2f,0.2f,-0.1423f,0.f,0.4107f,0.1f,1.4f,1.f,0.f,60.f,1.f,0.1f},{1,1,1,1,1,0,1,1,0,0,0,0,0,0,-1,-1,-1}},{{0.f,1.267f,-0.687f,0.022f,0.f,0.1f,0.f,0.f,0.f,0.2f,0.2f,0.2f,0.f,0.f,0.f,0.f,0.f,0.f,3.142f,65.f,1.f,0.1f},{0,1,1,1,1,0,1,1,0,0,0,0,0,0,-1,-1,-1}}}}},
}};
struct Row {size_t car{};std::array<float,3> seat{};};
struct Request {bool hideDriver{};std::array<Row,25> rows{};size_t count{};};
inline size_t FindCar(std::string_view code) {
    for(size_t i=0;i<cars.size();++i)if(code==cars[i].code)return i;
    return cars.size();
}
inline bool ValidSeat(size_t car,const std::array<float,3>& seat) {
    if(car>=cars.size())return false;
    constexpr std::array<float,3> low{-1.5f,.2f,-2.f},high{1.5f,2.5f,2.f};
    for(size_t i=0;i<3;++i)
        if(!std::isfinite(seat[i]) || seat[i]<low[i] || seat[i]>high[i] ||
           std::abs(seat[i]-cars[car].seat[i])>.50001f)return false;
    return true;
}
inline bool ParseRequest(std::string_view text,Request& result) {
    result={};if(text.empty() || text.size()>4096 || text.back()!='\n')return false;
    for(unsigned char c:text)if(c!=10 && c!=13 && (c<32 || c>126))return false;
    std::istringstream input{std::string(text)};input.imbue(std::locale::classic());
    std::string line;std::getline(input,line);if(!line.empty() && line.back()=='\r')line.pop_back();
    if(line!="dlc-cockpit-v1 head-only" && line!="dlc-cockpit-v1 hide-driver")return false;
    Request parsed{};parsed.hideDriver=line=="dlc-cockpit-v1 hide-driver";
    while(std::getline(input,line)) {
        if(parsed.count==parsed.rows.size())return false;
        std::istringstream values(line);values.imbue(std::locale::classic());std::string code,extra;
        Row row{};if(!(values>>code>>row.seat[0]>>row.seat[1]>>row.seat[2]) || (values>>extra))return false;
        row.car=FindCar(code);if(!ValidSeat(row.car,row.seat))return false;
        for(size_t i=0;i<parsed.count;++i)if(parsed.rows[i].car==row.car)return false;
        parsed.rows[parsed.count++]=row;
    }
    if(!parsed.count)return false;result=parsed;return true;
}
inline uint32_t NameHash(std::string_view value) {
    uint32_t hash=5381;for(unsigned char c:value)hash=hash*33+c;return hash;
}
inline float FloatAt(std::span<const uint8_t> object,size_t at) {
    float value{};std::memcpy(&value,object.data()+at,4);return value;
}
inline bool ValidObject(std::span<const uint8_t> object,uint32_t table,size_t car,size_t view) {
    if(object.size()!=objectBytes || car>=cars.size() || view>1)return false;
    uint32_t actual{},hash{};std::memcpy(&actual,object.data(),4);std::memcpy(&hash,object.data()+0x98,4);
    const std::string_view name=view?"bonnet_reverse":"bonnet";
    if(actual!=table || hash!=NameHash(name) ||
       std::memcmp(object.data()+0x19,name.data(),name.size()) || object[0x19+name.size()])return false;
    const auto& expected=cars[car].original[view];
    for(size_t i=0;i<checkedFloatCount;++i) {
        const auto value=FloatAt(object,floatOffsets[i]);
        if(!std::isfinite(value) || std::abs(value-expected.values[i])>.0001f)return false;
    }
    for(size_t i=0;i<flagOffsets.size();++i)
        if(expected.flags[i]>=0 && object[flagOffsets[i]]!=expected.flags[i])return false;
    return true;
}
inline bool PatchPair(std::span<uint8_t> forward,std::span<uint8_t> reverse,uint32_t table,const Row& row,bool hideDriver) {
    const auto a=reinterpret_cast<uintptr_t>(forward.data()),b=reinterpret_cast<uintptr_t>(reverse.data());
    if(!ValidSeat(row.car,row.seat) || (a>b?a-b:b-a)<objectBytes ||
       !ValidObject(forward,table,row.car,0) || !ValidObject(reverse,table,row.car,1))return false;
    for(size_t view=0;view<2;++view) {
        auto object=view?reverse:forward;
        for(size_t i=0;i<18;++i) {
            const float value=i<3?row.seat[i]:(i==5?.02f:0.f);
            std::memcpy(object.data()+floatOffsets[i],&value,4);
        }
        const float clip=.02f;std::memcpy(object.data()+0xf4,&clip,4); // Initialized native near-clip cache.
        for(size_t i=0;i<flagOffsets.size();++i) {
            if(cars[row.car].original[view].flags[i]<0)continue;
            object[flagOffsets[i]]=i<2?1:(i==2 && hideDriver?1:0);
        }
    }
    return true;
}
inline bool PatchedPair(std::span<const uint8_t> forward,std::span<const uint8_t> reverse,uint32_t table,const Row& row,bool hideDriver) {
    if(forward.size()!=objectBytes || reverse.size()!=objectBytes || !ValidSeat(row.car,row.seat))return false;
    std::array<std::array<uint8_t,objectBytes>,2> expected{};
    for(size_t view=0;view<2;++view) {
        const auto object=view?reverse:forward;
        std::memcpy(expected[view].data(),object.data(),objectBytes);
        const auto& original=cars[row.car].original[view];
        for(size_t i=0;i<floatOffsets.size();++i)
            std::memcpy(expected[view].data()+floatOffsets[i],&original.values[i],4);
        for(size_t i=0;i<flagOffsets.size();++i)
            if(original.flags[i]>=0)expected[view][flagOffsets[i]]=original.flags[i];
    }
    if(!PatchPair(expected[0],expected[1],table,row,hideDriver))return false;
    for(size_t view=0;view<2;++view) {
        const auto object=view?reverse:forward;
        for(auto at:floatOffsets) {
            const auto value=FloatAt(object,at);
            if(!std::isfinite(value) || std::abs(value-FloatAt(expected[view],at))>.0001f)return false;
        }
        for(size_t i=0;i<flagOffsets.size();++i)
            if(cars[row.car].original[view].flags[i]>=0 && object[flagOffsets[i]]!=expected[view][flagOffsets[i]])return false;
    }
    return true;
}
}
