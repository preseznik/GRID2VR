#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <cmath>
#include "axes.h"

// A read-only range and a non-DIJOYSTATE-native layout are common on wheels.
struct FakeInput {
    bool filtersSupported{};bool ignoreFilterWrites{};DWORD dead=2000,saturation=8000;unsigned filterWrites{};
    HRESULT GetObjectInfo(DIDEVICEOBJECTINSTANCEW* out,DWORD offset,DWORD mode) {
        if(mode!=DIPH_BYOFFSET || (offset!=DIJOFS_X && offset!=DIJOFS_SLIDER(1))) return E_FAIL;
        out->dwType=DIDFT_ABSAXIS;out->dwOfs=999;return S_OK;
    }
    HRESULT SetProperty(REFGUID id,const DIPROPHEADER* value) {
        if(&id==&DIPROP_RANGE)return E_FAIL;
        if(!filtersSupported)return DIERR_UNSUPPORTED;
        if(value->dwHow!=DIPH_BYOFFSET || value->dwSize!=sizeof(DIPROPDWORD))return E_FAIL;
        const auto data=reinterpret_cast<const DIPROPDWORD*>(value)->dwData;++filterWrites;
        if(!ignoreFilterWrites) {if(&id==&DIPROP_DEADZONE)dead=data;else if(&id==&DIPROP_SATURATION)saturation=data;else return E_FAIL;}
        return S_OK;
    }
    HRESULT GetProperty(REFGUID id,DIPROPHEADER* value) {
        if(&id!=&DIPROP_RANGE) {
            if(!filtersSupported)return DIERR_UNSUPPORTED;
            reinterpret_cast<DIPROPDWORD*>(value)->dwData=&id==&DIPROP_DEADZONE?dead:saturation;return S_OK;
        }
        auto* range=reinterpret_cast<DIPROPRANGE*>(value);
        range->lMin=-100;range->lMax=900;return S_OK;
    }
};
int main() {
    FakeInput input;const auto axes=ReadAxes(&input);
    if(axes.mask!=0x81 || axes.ranges[7].minimum!=-100 || axes.ranges[7].maximum!=900) return 1;
    if(NormalizeAxis(-100,axes.ranges[7])!=-1 || NormalizeAxis(400,axes.ranges[7])!=0 || NormalizeAxis(900,axes.ranges[7])!=1) return 2;
    if(NormalizeAxis(-10000,axes.ranges[7])!=-1 || NormalizeAxis(10000,axes.ranges[7])!=1) return 3;
    if(axes.neutralFilters || axes.nonNeutralFilters)return 4;
    input.filtersSupported=true;const auto neutral=ReadAxes(&input);
    if(neutral.mask!=0x81 || neutral.neutralFilters!=0x81 || neutral.nonNeutralFilters || input.filterWrites!=4 || input.dead || input.saturation!=10000)return 5;
    input.ignoreFilterWrites=true;input.dead=2000;input.saturation=8000;const auto refused=ReadAxes(&input);
    if(refused.mask!=0x81 || refused.neutralFilters || refused.nonNeutralFilters!=0x81)return 6;
    return 0;
}
