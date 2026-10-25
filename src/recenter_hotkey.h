#pragma once
#include <Windows.h>
#include "recenter_shortcut.h"

namespace grid2vr::recenter {
bool Attach(HWND window,RecenterShortcut binding);
void Enable(bool enabled);
bool Consume();
}
