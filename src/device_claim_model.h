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
}
