#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <istream>

namespace grid2vr::forcefeedback {
// Launcher Force Feedback settings. GRID 2 keeps three force-feedback scales on
// each player's FFB processor: Wheel Strength (profile feedback_force) at +0xa0,
// Wheel Weight (feedback_friction) at +0xa4 and Environmental Effects
// (feedback_effects, kerbs, surfaces and impacts) at +0xa8, each 0..1. The
// setup routine (AVX VA 0x8938d0, called from 0xa7cd17 when a controller is set
// for a player) reads the profile options and stores them at VA 0x893a6a.. .
// With FFB switched off it stores zeros. GRID 2 sends wheels a single constant
// force, so these scales are the only place the three can be separated.
//
// With the launcher override on, one 8-byte store instruction is replaced by a
// jump to a stub that performs the game's own three stores and then writes the
// launcher's values over them. Nothing is written to the profile, the save or
// any data file, so GRID 2's own settings return as soon as the override is off.
struct Settings {
    int strength=100; // percent, GRID 2 default 10/10
    int weight=50;    // percent, GRID 2 default 5/10
    int effects=50;   // percent, GRID 2 default 5/10
};
inline bool ValidSettings(const Settings& value) {
    return value.strength>=0 && value.strength<=100 && value.weight>=1 && value.weight<=100 && value.effects>=0 && value.effects<=100;
}
// force-feedback.request: "<strength> <weight> <effects>" whole percentages.
inline bool ReadRequest(std::istream& input,Settings& result) {
    Settings candidate;
    if(!(input>>candidate.strength>>candidate.weight>>candidate.effects))return false;
    input>>std::ws;
    if(!input.eof() || !ValidSettings(candidate))return false;
    result=candidate;return true;
}

// Relative to the image base. The guarded range contains no relocated bytes:
// the effects default/off path (0x893a5e..0x893a65), both scale loads, the three
// stores and the epilogue through ret 4.
inline constexpr std::uintptr_t GuardRva=0x493a5e,StoreRva=0x493a6a,ResumeRva=0x493a87;
inline constexpr std::array<std::uint8_t,0x32> GuardBytes{
    0xeb,0x05,                                     // jmp 0x893a65
    0xc5,0xfa,0x10,0x45,0xfc,                      // vmovss xmm0,[ebp-4]   (effects)
    0xc5,0xfa,0x10,0x4d,0xf8,                      // vmovss xmm1,[ebp-8]   (strength)
    0xc5,0xfa,0x11,0x8b,0xa0,0x00,0x00,0x00,       // vmovss [ebx+0xa0],xmm1  <- replaced
    0xc5,0xfa,0x10,0x4d,0xf4,                      // vmovss xmm1,[ebp-0xc] (weight)
    0xc5,0xfa,0x11,0x8b,0xa4,0x00,0x00,0x00,       // vmovss [ebx+0xa4],xmm1
    0xc5,0xfa,0x11,0x83,0xa8,0x00,0x00,0x00,       // vmovss [ebx+0xa8],xmm0
    0x5f,0x5b,0x5e,0x8b,0xe5,0x5d,0xc2,0x04,0x00}; // pop edi/ebx/esi; mov esp,ebp; pop ebp; ret 4
inline constexpr std::size_t StoreOffset=StoreRva-GuardRva,StoreBytes=8;
static_assert(ResumeRva-GuardRva==0x29 && StoreOffset==12);

// Replaces the 8-byte strength store at site (which must be the guarded code)
// with "jmp stub" plus three NOPs, restoring the page protection. Refuses, and
// leaves the code unchanged, if the bytes differ or any step fails.
bool PatchStore(std::uint8_t* guard,const void* stub);
// The stub that the patched site jumps to; it resumes at resume.
const void* Stub();
// Values the stub writes: whole percentages scaled to 0..1.
void SetValues(const Settings& value,std::uintptr_t resume);
struct Counters { std::uint64_t calls; std::array<float,3> native,applied; };
Counters Snapshot();

// Runtime: reads force-feedback.request, checks the code and patches it once.
void Install(std::uintptr_t base,const std::filesystem::path& output);
// Present: bounded receipt refresh.
void Tick();
}
