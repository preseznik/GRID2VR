#pragma once
#include "driving_controls_model.h"
#include <optional>

namespace grid2vr::drivingcontrols {
enum class InputKind { SignedAxis, PositiveAxis, Button, Hat, Key };
struct InputControl { InputKind kind{};unsigned index{},direction{}; };

inline std::optional<InputControl> ParseInputControl(const Binding& binding) {
    if(!Input(binding.device,binding.input))return {};
    const auto& name=binding.input;
    unsigned index{};
    if(binding.device=="keyboard") {
        std::from_chars(name.data()+10,name.data()+12,index,16);return InputControl{InputKind::Key,index};
    }
    if(binding.device=="xinput_pad") {
        constexpr std::string_view axes[]{"xinput_left_stick_x","xinput_left_stick_y","xinput_right_stick_x","xinput_right_stick_y","xinput_left_trigger","xinput_right_trigger"};
        constexpr std::string_view buttons[]{"xinput_dpad_up","xinput_dpad_down","xinput_dpad_left","xinput_dpad_right","xinput_button_start","xinput_button_back","xinput_button_left_stick","xinput_button_right_stick","xinput_button_left_shoulder","xinput_button_right_shoulder","","","xinput_button_a","xinput_button_b","xinput_button_x","xinput_button_y"};
        for(unsigned i=0;i<std::size(axes);++i)if(axes[i]==name)return InputControl{i<4?InputKind::SignedAxis:InputKind::PositiveAxis,i};
        for(unsigned i=0;i<std::size(buttons);++i)if(buttons[i]==name)return InputControl{InputKind::Button,i};
        return {};
    }
    constexpr std::string_view axes[]{"di_x_axis","di_y_axis","di_z_axis","di_x_axis_rotation","di_y_axis_rotation","di_z_axis_rotation","di_slider_0","di_slider_1"};
    for(unsigned i=0;i<std::size(axes);++i)if(axes[i]==name)return InputControl{InputKind::SignedAxis,i};
    if(name.starts_with("di_button_")) {
        std::from_chars(name.data()+10,name.data()+name.size(),index);return InputControl{InputKind::Button,index};
    }
    constexpr std::string_view directions[]{"up","right","down","left"};
    for(unsigned i=0;i<std::size(directions);++i)if(std::string_view(name).substr(10)==directions[i])return InputControl{InputKind::Hat,unsigned(name[8]-'0'),i};
    return {};
}

inline float HatValue(uint32_t angle,unsigned direction) {
    if(angle>=36000 || direction>=4 || (angle&0xffff)==0xffff)return 0;
    const auto center=direction*9000;
    const auto distance=angle>center?angle-center:center-angle;
    return std::min(distance,36000-distance)<=4500?1.f:0.f;
}

inline float CalibrateInput(float raw,InputKind kind,const Binding& binding) {
    if(!std::isfinite(raw) || !std::isfinite(binding.dead) || !std::isfinite(binding.saturation) ||
       binding.dead<0 || binding.dead>=binding.saturation || binding.saturation>1 || binding.calibration>3)return 0;
    raw=std::clamp(raw,kind==InputKind::SignedAxis?-1.f:0.f,1.f);
    float value{};
    switch(binding.calibration) {
    case 0:value=kind==InputKind::SignedAxis?(raw+1)*.5f:raw;break;
    case 1:value=kind==InputKind::SignedAxis?(1-raw)*.5f:1-raw;break;
    case 2:value=std::max(0.f,raw);break;
    case 3:value=std::max(0.f,-raw);break;
    }
    return std::clamp((value-binding.dead)/(binding.saturation-binding.dead),0.f,1.f);
}

// Each binding must visit neutral after startup, disconnect or focus loss.
// This prevents a held gear or fully depressed pedal becoming a synthetic press
// as a device reconnects. Focus loss clears edges as well as continuous values.
struct BindingInputState {
    State previous{};
    bool armed{};
    State Update(bool available,float value) {
        if(!available || !std::isfinite(value)) {previous={};armed=false;return {};}
        value=std::clamp(value,0.f,1.f);
        if(!armed) {armed=value<=.02f;previous={};return {};}
        State state;state.value=value;state.held=value>.5f;
        state.pressed=state.held && !previous.held;state.released=!state.held && previous.held;
        previous=state;return state;
    }
};
}
