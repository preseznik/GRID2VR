#include "device_claim_model.h"
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace grid2vr::drivingcontrols;
void Check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
int main() {
    try {
        constexpr std::string_view wheel="{C24F046D-0000-0000-0000-504944564944}";
        ClaimRequest request;
        Check(ParseClaimRequest("GRID2VR_DEVICE_CLAIM_1\nmode\tauto\n",request) && request.automatic &&
            request.families==std::vector<std::string>{"xinput_pad"},"auto without steering devices falls back to the pad only");
        Check(ParseClaimRequest("GRID2VR_DEVICE_CLAIM_1\nmode\tauto\nfamily\t"+std::string(wheel)+"\n",request) &&
            request.families==std::vector<std::string>{std::string(wheel),"xinput_pad"},"steering wheel precedes the pad");
        Check(ParseClaimRequest("GRID2VR_DEVICE_CLAIM_1\nmode\tauto\nfamily\txinput_pad\nfamily\t"+std::string(wheel)+"\n",request) &&
            request.families==std::vector<std::string>{"xinput_pad",std::string(wheel)},"an explicit pad keeps its steering order");
        Check(ParseClaimRequest("GRID2VR_DEVICE_CLAIM_1\nmode\tdevice\nfamily\t"+std::string(wheel)+"\n",request) && !request.automatic &&
            request.families.size()==1,"one explicit device has no fallback");
        for(const auto& bad:std::vector<std::string>{"","GRID2VR_DEVICE_CLAIM_2\nmode\tauto\n","GRID2VR_DEVICE_CLAIM_1\nmode\tdevice\n",
            "GRID2VR_DEVICE_CLAIM_1\nmode\tdevice\nfamily\txinput_pad\nfamily\t"+std::string(wheel)+"\n",
            "GRID2VR_DEVICE_CLAIM_1\nmode\tauto\nfamily\tkeyboard\n","GRID2VR_DEVICE_CLAIM_1\nmode\tauto\nfamily\tdi_default\n",
            "GRID2VR_DEVICE_CLAIM_1\nmode\tauto\nfamily\txinput_pad\nfamily\tXINPUT_PAD\n","GRID2VR_DEVICE_CLAIM_1\nmode\tauto\nfamily\txinput_pad",
            "GRID2VR_DEVICE_CLAIM_1\nmode\tkeyboard\n","GRID2VR_DEVICE_CLAIM_1\nmode\tauto\nfamily\t{C24F046D-0000-0000-0000-50494456494}\n",
            "GRID2VR_DEVICE_CLAIM_1\r\nmode\tauto\r\n",std::string("GRID2VR_DEVICE_CLAIM_1\nmode\tauto\n")+std::string(1100,'x')}) {
            ClaimRequest rejected;Check(!ParseClaimRequest(bad,rejected),"malformed, keyboard, generic or duplicate requests are refused");
        }
        std::string many="GRID2VR_DEVICE_CLAIM_1\nmode\tauto\n";
        for(int i=0;i<9;++i)many+="family\t{C24F046D-0000-0000-0000-50494456494"+std::to_string(i)+"}\n";
        Check(!ParseClaimRequest(many,request),"at most eight requested families");

        constexpr uint32_t keyboard=7,pad=11,wheelId=21,wheelUpper=22,handbrake=31;
        const std::vector<Device> devices{{0x1000,keyboard,0},{0x2000,handbrake,0},{0x3000,pad,1},{0x3000,pad,0},{0x4000,wheelUpper,0}};
        const std::vector<ClaimCandidate> auto_{{wheelId,wheelUpper},{pad,pad}};
        auto chosen=SelectClaimDevice(devices,auto_,keyboard);
        Check(chosen && chosen->family==wheelUpper && chosen->backend==0x4000,"wheel matched through the upper-case GUID family");
        const std::vector<ClaimCandidate> padOnly{{pad,pad}};
        chosen=SelectClaimDevice(devices,padOnly,keyboard);
        Check(chosen && chosen->family==pad && chosen->index==1,"first enumerated pad, in native order");
        const std::vector<Device> keyboardOnly{{0x1000,keyboard,0},{0x2000,handbrake,0}};
        Check(!SelectClaimDevice(keyboardOnly,auto_,keyboard),"an unrequested DirectInput device is never claimed");
        const std::vector<ClaimCandidate> keyboardCandidate{{keyboard,keyboard}};
        Check(!SelectClaimDevice(devices,keyboardCandidate,keyboard),"keyboard is never a claim target");
        const std::vector<Device> invalid{{0,pad,0},{0x3000,pad,UINT32_MAX},{0x3000,0,0}};
        Check(!SelectClaimDevice(invalid,padOnly,keyboard),"incomplete native device records are refused");

        const std::vector<uint32_t> before{keyboard},mixed{keyboard,pad},none{};
        Check(KeyboardOnly(before,keyboard) && !KeyboardOnly(mixed,keyboard) && !KeyboardOnly(none,keyboard) && !KeyboardOnly(before,0),
            "only a keyboard-only driving map is claimed");
        Check(CarriesFamily(mixed,pad) && !CarriesFamily(before,pad) && !CarriesFamily(mixed,0),"claim verification needs the bound family");

        // Stock generic preset fallback: the Controls menu's 16-byte entry.
        const PresetEntry entry{{0x4000,wheelUpper,2},0x55};
        uint32_t words[4]{};std::memcpy(words,&entry,sizeof(words));
        Check(words[0]==0x4000 && words[1]==wheelUpper && words[2]==2 && words[3]==0x55,"generic entry is the device record then the preset name");
        std::set<std::string_view> names(std::begin(GenericPresetActions),std::end(GenericPresetActions));
        Check(names.size()==52 && names.size()==std::size(GenericPresetActions),"every dinput_default.xml action, once");
        for(auto name:{"Look Up","Look Down","Look Left","Look Right","Steer Left","Menu Down","Change View","Horn","Activate Replay System","Fe View Tweak In"})
            Check(names.count(name)==1,"colliding defaults are muted");
        Axis wheelLook{},padLook{},keyboardLook{};
        wheelLook.device={0x4000,wheelUpper,0};wheelLook.state={0.8f,1,1,0,0};
        padLook.device={0x3000,pad,0};padLook.state={0.4f,1,0,0,0};
        keyboardLook.device={0x1000,keyboard,0};
        std::vector<Axis> look{wheelLook,padLook,keyboardLook};
        Check(MuteFamily(look,wheelUpper)==1 && SameState(look[0].state,State{}) && look[1].state.value==0.4f && look[1].state.held,
            "only the generic wheel's carrier goes neutral");
        Check(MuteFamily(look,wheelUpper)==0 && MuteFamily(look,0)==0,"neutral carriers and an unset family are not counted");
        Check(look[0].device==wheelLook.device && look[0].input==wheelLook.input,"muting changes state only, never the binding");
        const auto load=WithAddress(std::array<uint8_t,2>{0x8b,0x15},0x0126a0ecu);
        Check(load==std::array<uint8_t,6>{0x8b,0x15,0xec,0xa0,0x26,0x01},"absolute operand guard bytes are little-endian");
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
    std::cout<<"device claim model passed\n";return 0;
}
