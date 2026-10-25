#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <cmath>
#include "axes.h"

// A read-only range and a non-DIJOYSTATE-native layout are common on wheels.
struct FakeInput {
    HRESULT GetObjectInfo(DIDEVICEOBJECTINSTANCEW* out,DWORD offset,DWORD mode) {
        if(mode!=DIPH_BYOFFSET || (offset!=DIJOFS_X && offset!=DIJOFS_SLIDER(1))) return E_FAIL;
        out->dwType=DIDFT_ABSAXIS;out->dwOfs=999;return S_OK;
    }
    HRESULT SetProperty(REFGUID,const DIPROPHEADER*) { return E_FAIL; }
    HRESULT GetProperty(REFGUID,DIPROPHEADER* value) {
        auto* range=reinterpret_cast<DIPROPRANGE*>(value);
        range->lMin=-100;range->lMax=900;return S_OK;
    }
};
int main() {
    FakeInput input;const auto axes=ReadAxes(&input);
    if(axes.mask!=0x81 || axes.ranges[7].minimum!=-100 || axes.ranges[7].maximum!=900) return 1;
    if(NormalizeAxis(-100,axes.ranges[7])!=-1 || NormalizeAxis(400,axes.ranges[7])!=0 || NormalizeAxis(900,axes.ranges[7])!=1) return 2;
    if(NormalizeAxis(-10000,axes.ranges[7])!=-1 || NormalizeAxis(10000,axes.ranges[7])!=1) return 3;
    return 0;
}
