#pragma once
#include <windows.h>

namespace grid2vr {
// Supplementary evidence only: never handles the exception or changes context.
// No heap allocation or direct reads of potentially invalid fault addresses.
inline constexpr unsigned faultContextByteLimit=32768;
void WriteFaultContext(HANDLE file,const CONTEXT& context);
}
