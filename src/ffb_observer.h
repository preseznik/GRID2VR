#pragma once
#include <cstdint>
#include <filesystem>
#include "force_feedback_shaping.h"

namespace grid2vr::ffbobserver {
// Count the game's XInput rumble and DirectInput force-feedback calls and
// results. Every call is forwarded; nothing is suppressed, and only launcher
// Force Feedback (ConfigureShaping) changes a constant-force magnitude.
// Called only after the supported-host fingerprint check.
void Install(uintptr_t base,const std::filesystem::path& output);
// Launcher Force Feedback (force-feedback.request): from then on the game's
// constant-force updates are passed to dinput8 shaped (force_feedback_shaping.h)
// on a private copy. 100/100/100 or no call leaves every call unchanged.
void ConfigureShaping(const forcefeedback::Settings& value);
// From DllMain, after the host layout check: route the game's
// DINPUT8!DirectInput8Create import through a pass-through observer, so the
// device/effect hooks exist before the game creates its wheel effects (it
// does so before the first Present, where Install runs). Only when the image
// size and the import slot RVA match (AVX grid2_avx.exe: 0x100e000, 0xb16070).
inline constexpr uint32_t avxImageSize=0x100e000,avxDirectInputSlotRva=0xb16070;
bool AttachEarly(uintptr_t image,uint32_t imageSize,uint32_t slotRva);
// Bounded status refresh from the controller thread (Present).
void Tick();
void Stop();
}
