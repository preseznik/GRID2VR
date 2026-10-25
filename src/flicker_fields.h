#pragma once
#include <cstdint>
#include <span>

namespace grid2vr::flicker {
struct Field { const char* name; unsigned offset,bytes; };
// Shader-used, non-camera fields in VS CB0, verified against the captured
// fingerprints and shipped DX11 bytecode. Observation only: never substitute
// these values, or infer that a stable upload proves stable instance streams.
inline std::span<const Field> ObjectFields(uint64_t shader) {
    static constexpr Field model0[]{{"model",0,64}};
    static constexpr Field model16[]{{"model",16,64}};
    static constexpr Field instance144[]{{"perInstanceModelMatrix",144,64}};
    static constexpr Field instance160[]{{"perInstanceModelMatrix",160,64}};
    static constexpr Field instance176[]{{"perInstanceModelMatrix",176,64}};
    static constexpr Field instance320[]{{"perInstanceModelMatrix",320,64}};
    static constexpr Field tree[]{{"localMatrix",176,64}};
    static constexpr Field flag208[]{{"winddirection",208,16},{"_timer",228,4},{"_WindMagnitude",224,4},{"_Magnitude",232,4},{"_SinScale",236,4}};
    static constexpr Field flag400[]{{"winddirection",400,16},{"_timer",420,4},{"_WindMagnitude",416,4},{"_Magnitude",424,4},{"_SinScale",428,4}};
    static constexpr Field flag336[]{{"winddirection",336,16},{"_timer",356,4},{"perInstanceModelMatrix",368,64},{"_WindMagnitude",352,4},{"_Magnitude",360,4},{"_SinScale",364,4}};
    static constexpr Field flag144[]{{"winddirection",144,16},{"_timer",164,4},{"perInstanceModelMatrix",176,64},{"_WindMagnitude",160,4},{"_Magnitude",168,4},{"_SinScale",172,4}};
    switch(shader) {
    case 0x105f4e5c511149c0ull: case 0x145a9fbb991c0c8aull:
    case 0x15b38cf263b77a19ull: case 0x98f10fb2e3c2851full:
    case 0xa636afe1400f9e4aull: case 0xac1fbbd477a609bdull: return model0;
    case 0x3de6794f456d95f9ull: return model16;
    case 0xd946f27a1c896b78ull: return instance144;
    case 0xd2493eff6077a3bdull: return instance160;
    case 0x8b6c766bb894ac89ull: case 0x9f5f7d2c416172e4ull:
    case 0xec328dc7892bc754ull: case 0xfbb75a032d043c42ull: return instance176;
    case 0x1c547389c33a53f8ull: case 0x3a1b0384014b0834ull: return instance320;
    case 0x29b2af5093421c8dull: return tree;
    case 0x202ee912fb231ddfull: return flag208;
    case 0xb5e99cc6f02065e0ull: return flag400;
    case 0xc90ec9a844625ee6ull: return flag336;
    case 0x5365b368abb08738ull: return flag144;
    default: return {};
    }
}
}
