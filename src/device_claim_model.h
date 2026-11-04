#pragma once
#include "driving_controls_model.h"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace grid2vr::drivingcontrols {
// Direct Practice/Race boot skips ScreenPressStart, whose update (VA 7dcd50)
// binds the device that pressed Start to the player's map through Input2
// 5c1db0. Without it the consumed map stays keyboard-only and the game has no
// rumble/force-feedback device. GRID2VR_DEVICE_CLAIM_1 requests that same
// native bind once, for a device the launcher selected:
//   mode<TAB>auto   steering-binding families in order, then xinput_pad
//   mode<TAB>device exactly one family
//   family<TAB>xinput_pad | {product GUID}
struct ClaimRequest { bool automatic{};std::vector<std::string> families; };
inline bool ClaimFamily(std::string_view family) { return family=="xinput_pad" || Guid(family); }
inline bool ParseClaimRequest(std::string_view text,ClaimRequest& output) {
    constexpr std::string_view header="GRID2VR_DEVICE_CLAIM_1\n",automatic="mode\tauto\n",device="mode\tdevice\n",prefix="family\t";
    if(text.size()>1024 || !text.starts_with(header))return false;
    text.remove_prefix(header.size());
    ClaimRequest parsed;
    if(text.starts_with(automatic)){parsed.automatic=true;text.remove_prefix(automatic.size());}
    else if(text.starts_with(device))text.remove_prefix(device.size());
    else return false;
    while(!text.empty()) {
        const auto newline=text.find('\n');
        if(newline==text.npos || !text.starts_with(prefix))return false;
        const auto family=text.substr(prefix.size(),newline-prefix.size());text.remove_prefix(newline+1);
        if(!ClaimFamily(family) || std::any_of(parsed.families.begin(),parsed.families.end(),[&](const auto& f){return EqualId(f,family);}))return false;
        parsed.families.emplace_back(family);if(parsed.families.size()>8)return false;
    }
    if(parsed.automatic) {
        // A pad is the stock non-wheel controller; arbitrary DirectInput
        // devices (handbrakes, shifters, button boxes) are never guessed.
        if(std::none_of(parsed.families.begin(),parsed.families.end(),[](const auto& f){return f=="xinput_pad";}))parsed.families.emplace_back("xinput_pad");
    } else if(parsed.families.size()!=1)return false;
    output=std::move(parsed);return true;
}
// Interned native family IDs; DirectInput GUIDs are matched in either case.
struct ClaimCandidate { uint32_t family{},familyUpper{}; };
inline std::optional<Device> SelectClaimDevice(std::span<const Device> devices,std::span<const ClaimCandidate> candidates,uint32_t keyboard) {
    for(const auto& candidate:candidates)
        for(const auto& device:devices)
            if(device.backend && device.family && device.index!=UINT32_MAX && device.family!=keyboard &&
               (device.family==candidate.family || device.family==candidate.familyUpper))return device;
    return std::nullopt;
}
// Families seen on the consumed map's driving actions. Only an all-keyboard
// map is claimed; any other family means a native claim already happened.
inline bool KeyboardOnly(std::span<const uint32_t> families,uint32_t keyboard) {
    return keyboard && !families.empty() && std::all_of(families.begin(),families.end(),[&](uint32_t f){return f==keyboard;});
}
inline bool CarriesFamily(std::span<const uint32_t> families,uint32_t family) {
    return family && std::find(families.begin(),families.end(),family)!=families.end();
}
// A DirectInput wheel GRID 2 ships no preset for (G920, Moza) is left
// keyboard-only by the bind above, so the game gives it no force feedback.
// GRID 2's Controls menu offers such a device its stock generic preset
// (input/dinput_default.xml, "di_default"): 0x5a5290 builds a 16-byte entry,
// the native device record plus the preset name, and StateSetControlsPreset
// (VA babe45) passes it to Input2 473ce0(map, entry). The claim does the
// same, without the menu's career save (b664e0). Stock data only: online play
// and career saves stay intact, unlike an added preset file (ProJict f61544b3).
struct PresetEntry { Device device{};uint32_t preset{}; };
static_assert(sizeof(PresetEntry)==16 && offsetof(PresetEntry,preset)==12);
// Every action dinput_default.xml binds. On such a wheel these defaults collide
// with the launcher bindings: look on the pedals, steering on the clutch, replay,
// view and horn on paddles and shifter. The preset is bound only so the wheel
// gets native force feedback; its own carriers stay neutral and the launcher
// bindings drive.
inline constexpr std::string_view GenericPresetActions[]{
    "Accelerate","Brake","Handbrake","Steer Left","Steer Right","Change View","Gear Up","Gear Down",
    "Look Left","Look Right","Look Up","Look Down","Look Back","Pause","Instant Replay","Horn",
    "Menu Left","Menu Right","Menu Up","Menu Down","Menu Select","Menu Back","Menu Button3","Menu Button4","Menu Start Button",
    "Fe View Tweak Left","Fe View Tweak Right","Fe View Tweak Up","Fe View Tweak Down","Fe View Tweak In","Fe View Tweak Out",
    "Replay Rewind","Replay Fast Forward","Replay Pause","Replay Next Camera","Replay Prev Camera","Replay Jump In","Replay Exit",
    "Replay UI On Off","Replay Youtube","Youtube Drag Left","Youtube Drag Right","Youtube Speed Up","Youtube Speed Down",
    "Youtube Upload","Youtube Exit","Spectator Next Camera","Spectator Previous Camera","Spectator UI On Off","Spectator List On Off",
    "Cycle Forward On Board Cameras","Activate Replay System"};
// Neutral state for one device family's carriers; other devices stay native.
inline size_t MuteFamily(std::span<Axis> axes,uint32_t family) {
    size_t muted{};
    for(auto& axis:axes)if(family && axis.device.family==family && !SameState(axis.state,State{})){axis.state={};++muted;}
    return muted;
}
// An instruction whose last four bytes are an absolute address in the loaded
// image (relocated like the game's own operands).
template<size_t N> inline std::array<uint8_t,N+4> WithAddress(const std::array<uint8_t,N>& prefix,uint32_t address) {
    std::array<uint8_t,N+4> bytes{};
    for(size_t i=0;i<N;++i)bytes[i]=prefix[i];
    for(size_t i=0;i<4;++i)bytes[N+i]=static_cast<uint8_t>(address>>(8*i));
    return bytes;
}
}
