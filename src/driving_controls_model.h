#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace grid2vr::drivingcontrols {
inline constexpr std::string_view Actions[]{"Steer Left","Steer Right","Accelerate","Brake","Clutch","Handbrake","Gear Up","Gear Down","Gear 1","Gear 2","Gear 3","Gear 4","Gear 5","Gear 6","Gear 7","Gear Reverse","Change View","Look Back","Look Left","Look Right","Look Up","Look Down","Horn","Boost","Pause","Instant Replay","Menu Start Button","Menu Select","Menu Back","Menu Up","Menu Down","Menu Left","Menu Right","Replay Rewind","Replay Fast Forward","Replay Pause","Replay Next Camera","Replay Prev Camera","Replay Jump In","Replay Exit","Replay UI On Off"};
struct Binding { std::string device,action,input;uint32_t calibration{};float dead{},saturation{};std::string deviceId; };
enum class Transmission { Game, Automatic, Sequential, HPattern, HPatternClutch };
inline constexpr std::string_view TransmissionNames[]{"game","automatic","sequential","hpattern","hpattern_clutch"};
struct DrivingSettings {
    Transmission transmission=Transmission::Game;
    int steeringLinearity{};
    bool wheelSettings{};
    // Older requests retain the native gate. Only v4 explicitly requests an
    // independent shift threshold; this never calibrates the analogue pedal.
    int clutchShiftThreshold=100;
    bool clutchShiftSettings{};
};
// The failed hook candidate is diagnostic-only. A normal saved binding request
// must never opt into native code changes, including pass-through adapters.
inline bool CallsiteDiagnosticRequest(std::string_view text) {
    return text=="GRID2VR_DRIVING_CONTROLS_CALLSITE_DIAGNOSTIC_1\n";
}
inline bool PassthroughRequest(std::string_view text) {
    return text=="GRID2VR_DRIVING_CONTROLS_PASSTHROUGH_1\n";
}
inline constexpr std::array<uint8_t,5> RelativeCall(uint32_t site,uint32_t target) {
    const auto displacement=target-(site+5); // Win32 relative calls wrap at 32 bits.
    return {0xe8,static_cast<uint8_t>(displacement),static_cast<uint8_t>(displacement>>8),
        static_cast<uint8_t>(displacement>>16),static_cast<uint8_t>(displacement>>24)};
}
inline constexpr bool InCallInstruction(uint32_t ip,uint32_t site) { return ip-site<5; }
inline bool Guid(std::string_view s) {
    if(s.size()!=38 || s.front()!='{' || s.back()!='}')return false;
    for(size_t i=1;i<37;++i) {
        if(i==9 || i==14 || i==19 || i==24) { if(s[i]!='-')return false; }
        else if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')||(s[i]>='A'&&s[i]<='F')))return false;
    }return true;
}
inline bool Number(std::string_view s,float& result) {
    if(s.empty() || s.size()>32 || s.find_first_not_of("0123456789.")!=s.npos)return false;
    const auto parsed=std::from_chars(s.data(),s.data()+s.size(),result);
    return parsed.ec==std::errc{} && parsed.ptr==s.data()+s.size() && std::isfinite(result);
}
inline bool Input(std::string_view device,std::string_view s) {
    if(device=="keyboard") {
        unsigned code{};if(s.size()!=12 || !s.starts_with("vk_code_0x"))return false;
        const auto parsed=std::from_chars(s.data()+10,s.data()+12,code,16);
        return parsed.ec==std::errc{} && parsed.ptr==s.data()+12 && code>=8 && code<=254;
    }
    if(device=="xinput_pad") {
        constexpr std::string_view names[]{"xinput_left_stick_x","xinput_left_stick_y","xinput_right_stick_x","xinput_right_stick_y","xinput_left_trigger","xinput_right_trigger","xinput_dpad_up","xinput_dpad_down","xinput_dpad_left","xinput_dpad_right","xinput_button_start","xinput_button_back","xinput_button_left_stick","xinput_button_right_stick","xinput_button_left_shoulder","xinput_button_right_shoulder","xinput_button_a","xinput_button_b","xinput_button_x","xinput_button_y"};
        return std::find(std::begin(names),std::end(names),s)!=std::end(names);
    }
    if(!Guid(device))return false;
    constexpr std::string_view axes[]{"di_x_axis","di_y_axis","di_z_axis","di_x_axis_rotation","di_y_axis_rotation","di_z_axis_rotation","di_slider_0","di_slider_1"};
    if(std::find(std::begin(axes),std::end(axes),s)!=std::end(axes))return true;
    if(s.starts_with("di_button_")) {
        unsigned button{};const auto tail=s.substr(10);const auto parsed=std::from_chars(tail.data(),tail.data()+tail.size(),button);
        return !tail.empty() && parsed.ec==std::errc{} && parsed.ptr==tail.data()+tail.size() && button<128;
    }
    if(s.size()>10 && s.starts_with("di_dpad_") && s[8]>='0' && s[8]<='3' && s[9]=='_') {
        const auto tail=s.substr(10);return tail=="up" || tail=="down" || tail=="left" || tail=="right";
    }return false;
}
inline bool EqualId(std::string_view a,std::string_view b) {
    if(a.size()!=b.size())return false;
    for(size_t i=0;i<a.size();++i) {
        const auto lower=[](char c){return c>='A'&&c<='Z'?char(c-'A'+'a'):c;};
        if(lower(a[i])!=lower(b[i]))return false;
    }return true;
}
inline bool Parse(std::string_view text,std::vector<Binding>& output,DrivingSettings* settings=nullptr) {
    constexpr std::string_view header="GRID2VR_DRIVING_CONTROLS_1\n",header2="GRID2VR_DRIVING_CONTROLS_2\n",header3="GRID2VR_DRIVING_CONTROLS_3\n",header4="GRID2VR_DRIVING_CONTROLS_4\n";
    const bool v4=text.starts_with(header4),v3=v4 || text.starts_with(header3),v2=v3 || text.starts_with(header2);
    if(text.size()>524288 || (!v2 && !text.starts_with(header)))return false;
    std::vector<Binding> parsed;text.remove_prefix(header.size());
    DrivingSettings requested;
    if(v3) {
        constexpr std::string_view prefix="settings\t";
        if(!text.starts_with(prefix))return false;
        text.remove_prefix(prefix.size());
        const auto newline=text.find('\n');if(newline==text.npos)return false;
        const auto row=text.substr(0,newline);const auto tab=row.find('\t');if(tab==row.npos)return false;
        const auto name=row.substr(0,tab);auto linearity=row.substr(tab+1);
        if(v4) {
            const auto next=linearity.find('\t');if(next==linearity.npos)return false;
            const auto threshold=linearity.substr(next+1);linearity=linearity.substr(0,next);
            const auto number=std::from_chars(threshold.data(),threshold.data()+threshold.size(),requested.clutchShiftThreshold);
            if(threshold.empty() || number.ec!=std::errc{} || number.ptr!=threshold.data()+threshold.size() ||
               requested.clutchShiftThreshold<10 || requested.clutchShiftThreshold>100 || std::to_string(requested.clutchShiftThreshold)!=threshold)return false;
            requested.clutchShiftSettings=true;
        }
        const auto found=std::find(std::begin(TransmissionNames),std::end(TransmissionNames),name);
        if(found==std::end(TransmissionNames))return false;
        const auto number=std::from_chars(linearity.data(),linearity.data()+linearity.size(),requested.steeringLinearity);
        if(linearity.empty() || number.ec!=std::errc{} || number.ptr!=linearity.data()+linearity.size() ||
           requested.steeringLinearity<-10 || requested.steeringLinearity>10 || std::to_string(requested.steeringLinearity)!=linearity)return false;
        requested.transmission=static_cast<Transmission>(found-std::begin(TransmissionNames));requested.wheelSettings=true;text.remove_prefix(newline+1);
    }
    while(!text.empty()) {
        const auto newline=text.find('\n');if(newline==text.npos || newline>512)return false;
        auto row=text.substr(0,newline);text.remove_prefix(newline+1);
        std::array<std::string_view,7> fields{};
        const size_t count=v2?7:6;
        for(size_t i=0;i<count;++i) {
            const auto tab=row.find('\t');if((i==count-1)!=(tab==row.npos))return false;
            fields[i]=row.substr(0,tab);if(i!=count-1)row.remove_prefix(tab+1);
        }
        Binding binding{std::string(fields[0]),std::string(fields[1]),std::string(fields[2])};
        if(v2) {
            binding.deviceId=fields[6];
            if(binding.device=="keyboard"?fields[6]!="keyboard":binding.device=="xinput_pad"?
                !(fields[6].size()==8 && fields[6].starts_with("xinput:") && fields[6][7]>='0' && fields[6][7]<='3'):!Guid(fields[6]))return false;
        }
        if(std::find(std::begin(Actions),std::end(Actions),fields[1])==std::end(Actions) || !Input(fields[0],fields[2]))return false;
        constexpr std::string_view calibrations[]{"uniDirPos","uniDirNeg","biDirUpper","biDirLower"};
        const auto found=std::find(std::begin(calibrations),std::end(calibrations),fields[3]);
        if(found==std::end(calibrations) || !Number(fields[4],binding.dead) || !Number(fields[5],binding.saturation) || binding.dead<0 || binding.dead>.9f || binding.saturation<.1f || binding.saturation>1 || binding.dead>=binding.saturation)return false;
        binding.calibration=static_cast<uint32_t>(found-std::begin(calibrations));
        if(binding.device=="xinput_pad" && binding.input.ends_with("_trigger") && binding.calibration!=0)return false;
        std::vector<std::string_view> deviceIds;
        for(const auto& existing:parsed) {
            const auto prior=v2?std::string_view(existing.deviceId):std::string_view(existing.device);
            if(std::none_of(deviceIds.begin(),deviceIds.end(),[&](auto id){return EqualId(id,prior);}))deviceIds.push_back(prior);
            const bool same=EqualId(prior,v2?binding.deviceId:binding.device);
            if(same && !EqualId(existing.device,binding.device))return false;
            if(existing.action==binding.action && same)return false;
            if(binding.action.starts_with("Steer ") && existing.action.starts_with("Steer ") && same && existing.input==binding.input && existing.calibration==binding.calibration)return false;
        }
        if(deviceIds.size()>=16 && std::none_of(deviceIds.begin(),deviceIds.end(),[&](auto id){return EqualId(id,v2?binding.deviceId:binding.device);}))return false;
        parsed.push_back(std::move(binding));if(parsed.size()>std::size(Actions)*16)return false;
    }
    if(parsed.empty() && (!v3 || requested.transmission==Transmission::Game))return false;
    output=std::move(parsed);if(settings)*settings=requested;return true;
}

struct Device { uint32_t backend{},family{},index{};bool operator==(const Device&)const=default; };
struct State { float value{};uint8_t held{},pressed{},released{},padding{}; };
struct Axis { uint32_t registry{},input{};Device device{};std::array<uint32_t,8> calibration{};uint32_t restricted{};State state{}; };
static_assert(sizeof(Axis)==0x40 && offsetof(Axis,state)==0x38);
struct Override { uint32_t action{},family{},familyUpper{}; };
inline constexpr std::array<uint8_t,10> LockPrologue(uint32_t iat) {
    return {0x8b,0x01,0x50,0xff,0x15,static_cast<uint8_t>(iat),static_cast<uint8_t>(iat>>8),static_cast<uint8_t>(iat>>16),static_cast<uint8_t>(iat>>24),0xc3};
}
inline bool Replaces(std::span<const Override> overrides,uint32_t action,uint32_t family) {
    return std::any_of(overrides.begin(),overrides.end(),[&](const auto& b){return b.action==action && (b.family==family || b.familyUpper==family);});
}
inline void Combine(State& result,const State& axis) {
    if(std::isfinite(axis.value))result.value=std::max(result.value,axis.value);
    result.held|=axis.held;result.pressed|=axis.pressed;result.released|=axis.released;
}
// Change only volatile state, never a binding identity, calibration or vector.
// A carrier must belong to the exact native device, including its index.
inline bool SameBinding(const Axis& a,const Axis& b) {
    return a.registry==b.registry && a.input==b.input && a.device==b.device &&
        a.calibration==b.calibration && a.restricted==b.restricted;
}
inline bool SameState(const State& a,const State& b) {
    return a.value==b.value && a.held==b.held && a.pressed==b.pressed &&
        a.released==b.released && a.padding==b.padding;
}
inline size_t OverlayFamily(std::span<Axis> axes,std::span<const Axis> shadows,const Override& binding) {
    size_t carriers{};
    for(size_t i=0;i<axes.size();++i) {
        auto& axis=axes[i];
        if(axis.device.family!=binding.family && axis.device.family!=binding.familyUpper)continue;
        bool first=true;
        for(size_t j=0;j<i;++j)if(axes[j].device==axis.device){first=false;break;}
        axis.state={};
        if(!first)continue;
        ++carriers;
        for(const auto& shadow:shadows)if(shadow.device==axis.device)Combine(axis.state,shadow.state);
    }
    return carriers;
}
inline bool HasCarrier(std::span<const Axis> axes,const Device& device) {
    return std::any_of(axes.begin(),axes.end(),[&](const auto& axis){return axis.device==device;});
}
// A keyboard-only player map can still consume continuous Xbox values. Use
// exactly one existing state slot and retain its native keyboard state. This
// is deliberately separate from family replacement: device metadata remains
// native, so this fallback does not provide Xbox glyphs or wheel attribution.
inline bool OverlayMissingFamily(std::span<Axis> axes,std::span<const Axis> shadows,const Override& binding,uint32_t keyboardFamily) {
    if(!keyboardFamily || axes.empty() || shadows.empty() || std::any_of(axes.begin(),axes.end(),[&](const auto& axis){
        return axis.device.family!=keyboardFamily || axis.device.family==binding.family || axis.device.family==binding.familyUpper;}))return false;
    bool matched{};
    for(const auto& shadow:shadows)if(shadow.device.family==binding.family || shadow.device.family==binding.familyUpper) {
        Combine(axes.front().state,shadow.state);matched=true;
    }
    return matched;
}
inline bool RestoreOverlay(Axis& current,const Axis& native,const Axis& applied) {
    if(!SameBinding(current,native) || !SameState(current.state,applied.state))return false;
    current.state=native.state;return true;
}
inline State Aggregate(std::span<const Axis> nativeAxes,std::span<const Axis> shadowAxes,std::span<const Override> overrides,uint32_t action) {
    State result{};
    for(const auto& axis:nativeAxes)if(!Replaces(overrides,action,axis.device.family))Combine(result,axis.state);
    for(const auto& axis:shadowAxes)Combine(result,axis.state);
    return result;
}
// Queries consume a max/OR of volatile state slots. A carrier's device is not
// used by the value/held/edge query. Merge once per action, so adding a wheel or
// a second controller cannot erase another custom binding or keyboard state.
// Serialized axis descriptors, device selection and FFB routing stay native.
inline bool OverlayAction(std::span<Axis> axes,std::span<const Axis> shadows,std::span<const Override> overrides,uint32_t action) {
    if(axes.empty())return false;
    const auto state=Aggregate(axes,shadows,overrides,action);
    for(auto& axis:axes)axis.state={};
    axes.front().state=state;return true;
}
inline bool PedalAction(std::string_view action) {
    return action=="Accelerate" || action=="Brake" || action=="Clutch" || action=="Handbrake";
}
inline bool DirectAxis(std::string_view input) {
    return input=="di_x_axis" || input=="di_y_axis" || input=="di_z_axis" || input=="di_x_axis_rotation" ||
        input=="di_y_axis_rotation" || input=="di_z_axis_rotation" || input=="di_slider_0" || input=="di_slider_1";
}
inline bool SuppressPedalAlias(Axis& axis,uint32_t pedalInput,const Override& pedal,bool explicitMenu) {
    if(explicitMenu || axis.input!=pedalInput || (axis.device.family!=pedal.family && axis.device.family!=pedal.familyUpper))return false;
    axis.state={};return true;
}
}
