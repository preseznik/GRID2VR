#pragma once
#include <algorithm>
#include <array>

struct AxisRange { LONG minimum{}, maximum{65535}; };
struct AxisLayout {
    unsigned mask{};
    unsigned neutralFilters{},nonNeutralFilters{};
    std::array<AxisRange,8> ranges{};
};

// Query the selected DIJOYSTATE2 format. EnumObjects.dwOfs is a native device
// offset and need not match these slots (notably on multi-function wheels).
template<class Input> AxisLayout ReadAxes(Input* input) {
    AxisLayout result;
    const DWORD offsets[]={DIJOFS_X,DIJOFS_Y,DIJOFS_Z,DIJOFS_RX,DIJOFS_RY,DIJOFS_RZ,DIJOFS_SLIDER(0),DIJOFS_SLIDER(1)};
    for(unsigned i=0;i<8;++i) {
        DIDEVICEOBJECTINSTANCEW object{}; object.dwSize=sizeof(object);
        if(FAILED(input->GetObjectInfo(&object,offsets[i],DIPH_BYOFFSET)) || !(object.dwType&DIDFT_AXIS)) continue;
        DIPROPRANGE range{};
        range.diph={sizeof(range),sizeof(range.diph),offsets[i],DIPH_BYOFFSET};
        range.lMin=0; range.lMax=65535;
        const bool set=SUCCEEDED(input->SetProperty(DIPROP_RANGE,&range.diph));
        // Some ranges are read-only. Preserve and normalize their actual limits.
        const bool read=SUCCEEDED(input->GetProperty(DIPROP_RANGE,&range.diph));
        if(!read) { if(!set) continue; range.lMin=0; range.lMax=65535; }
        if(range.lMin>=range.lMax || range.lMin==DIPROPRANGE_NOMIN || range.lMax==DIPROPRANGE_NOMAX) continue;
        result.ranges[i]={range.lMin,range.lMax}; result.mask|=1u<<i;
        // Our binding calibration owns deadzone/saturation. Request neutral
        // DirectInput processing on this unacquired reader, not raw mode (which
        // would also disable the range used above). Never touch FFB or the
        // system calibration. Unsupported properties remain explicitly unknown.
        bool verified=true,nonNeutral=false;
        for(unsigned property=0;property<2;++property) {
            const auto& id=property?DIPROP_SATURATION:DIPROP_DEADZONE;
            const DWORD requested=property?10000:0;
            DIPROPDWORD value{};value.diph={sizeof(value),sizeof(value.diph),offsets[i],DIPH_BYOFFSET};value.dwData=requested;
            input->SetProperty(id,&value.diph);
            const bool known=SUCCEEDED(input->GetProperty(id,&value.diph));
            verified=verified && known && value.dwData==requested;
            nonNeutral=nonNeutral || (known && value.dwData!=requested);
        }
        if(verified)result.neutralFilters|=1u<<i;
        if(nonNeutral)result.nonNeutralFilters|=1u<<i;
    }
    return result;
}
inline float NormalizeAxis(LONG value,const AxisRange& range) {
    return static_cast<float>(std::clamp((static_cast<double>(value)-range.minimum)/(static_cast<double>(range.maximum)-range.minimum)*2.0-1.0,-1.0,1.0));
}
