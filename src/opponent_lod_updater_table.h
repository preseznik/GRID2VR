#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace grid2vr::opponentlod::updatertable {
inline constexpr uint32_t TableRva=0xcdcb68,NextTableRva=0xcdcb94;
inline constexpr std::size_t MethodCount=11,PrefixWords=1,UpdateSlot=1;
using Block=std::array<uintptr_t,PrefixWords+MethodCount>;
static_assert(NextTableRva-TableRva==MethodCount*sizeof(uint32_t));
// Prefix is an opaque code pointer, not claimed to be RTTI. The neighboring
// constructor publishes NextTableRva; the scheduler calls through +14 and +28.
inline constexpr std::array<uint32_t,PrefixWords+MethodCount> NativeRvas{
    0x693370,0x3da040,0x76e580,0x4a920,0x4d950,0x4d950,
    0x99bc30,0x58ddf0,0x15bd40,0x623c20,0x1090f0,0x693370
};
inline bool MatchesNative(const Block& block,uintptr_t image) noexcept {
    for(std::size_t i=0;i<block.size();++i)if(block[i]!=image+NativeRvas[i])return false;
    return true;
}
// Pure value copy. Caller must fingerprint/MatchesNative and qualify ownership
// before installing it. Every other interface word, including lifecycle slots,
// remains identical; no native function is executed here.
inline Block PrivateCopy(Block block,uintptr_t replacementUpdate) noexcept {
    block[PrefixWords+UpdateSlot]=replacementUpdate;return block;
}
}
