#include "driving_controls_input_model.h"
#include "driving_controls_input.h"
#include <cstdio>
#include <limits>

using namespace grid2vr::drivingcontrols;
#define CHECK(value) do {if(!(value)){std::fprintf(stderr,"Input test failed at line %d: %s\n",__LINE__,#value);return 1;}}while(false)
bool Near(float a,float b){return std::abs(a-b)<1e-5f;}
bool Zero(const State& state){return state.value==0 && !state.held && !state.pressed && !state.released;}
int main() {
    const std::string family="{C262046D-0000-0000-0000-504944564944}";
    Binding pedal{family,"Clutch","di_y_axis",1,.03f,1,"{01234567-0000-0000-0000-504944564944}"};
    const auto source=ParseInputControl(pedal);CHECK(source && source->kind==InputKind::SignedAxis && source->index==1);
    CHECK(CalibrateInput(1,source->kind,pedal)==0); // G920 released pedal.
    CHECK(CalibrateInput(-1,source->kind,pedal)==1); // Full depression.
    CHECK(Near(CalibrateInput(0,source->kind,pedal),(.5f-.03f)/.97f));
    pedal.dead=.1f;pedal.saturation=.9f;
    CHECK(CalibrateInput(.8f,source->kind,pedal)<1e-6f);
    CHECK(CalibrateInput(-.8f,source->kind,pedal)==1);
    pedal.calibration=0;
    CHECK(CalibrateInput(-1,source->kind,pedal)==0 && CalibrateInput(1,source->kind,pedal)==1);
    pedal.calibration=2;pedal.dead=0;pedal.saturation=1;
    CHECK(CalibrateInput(-1,source->kind,pedal)==0 && CalibrateInput(0,source->kind,pedal)==0 && CalibrateInput(.4f,source->kind,pedal)==.4f);
    pedal.calibration=3;
    CHECK(CalibrateInput(1,source->kind,pedal)==0 && CalibrateInput(0,source->kind,pedal)==0 && CalibrateInput(-.4f,source->kind,pedal)==.4f);
    CHECK(CalibrateInput(std::numeric_limits<float>::quiet_NaN(),source->kind,pedal)==0);
    pedal.dead=pedal.saturation;CHECK(CalibrateInput(-1,source->kind,pedal)==0);

    Binding trigger{"xinput_pad","Accelerate","xinput_right_trigger",0,0,1,"xinput:3"};
    const auto rightTrigger=ParseInputControl(trigger);CHECK(rightTrigger && rightTrigger->kind==InputKind::PositiveAxis && rightTrigger->index==5);
    CHECK(CalibrateInput(0,rightTrigger->kind,trigger)==0 && CalibrateInput(.6f,rightTrigger->kind,trigger)==.6f && CalibrateInput(1,rightTrigger->kind,trigger)==1);
    trigger.input="xinput_left_stick_y";const auto stick=ParseInputControl(trigger);CHECK(stick && stick->kind==InputKind::SignedAxis && stick->index==1);
    trigger.input="xinput_dpad_right";CHECK(ParseInputControl(trigger)->index==3);
    trigger.input="xinput_button_y";CHECK(ParseInputControl(trigger)->index==15);
    trigger.input="xinput_invented";CHECK(!ParseInputControl(trigger));
    Binding button{family,"Gear 1","di_button_127",0,0,1,pedal.deviceId};
    CHECK(ParseInputControl(button)->kind==InputKind::Button && ParseInputControl(button)->index==127);
    CHECK(CalibrateInput(0,InputKind::Button,button)==0 && CalibrateInput(1,InputKind::Button,button)==1);
    button.input="di_button_128";CHECK(!ParseInputControl(button));
    button.input="di_dpad_3_left";const auto hat=ParseInputControl(button);CHECK(hat && hat->kind==InputKind::Hat && hat->index==3 && hat->direction==3);
    CHECK(HatValue(UINT32_MAX,0)==0 && HatValue(65535,0)==0 && HatValue(36000,0)==0);
    CHECK(HatValue(0,0)==1 && HatValue(9000,1)==1 && HatValue(18000,2)==1 && HatValue(27000,3)==1);
    CHECK(HatValue(4500,0)==1 && HatValue(4500,1)==1 && HatValue(4500,2)==0);
    CHECK(HatValue(31500,3)==1 && HatValue(31500,0)==1);
    Binding key{"keyboard","Gear Up","vk_code_0x57",0,0,1,"keyboard"};
    CHECK(ParseInputControl(key)->kind==InputKind::Key && ParseInputControl(key)->index==87);

    BindingInputState clutch;
    CHECK(Zero(clutch.Update(true,1)) && !clutch.armed);
    CHECK(Zero(clutch.Update(true,.2f)) && !clutch.armed); // Still partly depressed.
    CHECK(Zero(clutch.Update(true,0)) && clutch.armed);
    const auto pressed=clutch.Update(true,1);CHECK(pressed.value==1 && pressed.held && pressed.pressed && !pressed.released);
    const auto held=clutch.Update(true,1);CHECK(held.held && !held.pressed && !held.released);
    const auto released=clutch.Update(true,0);CHECK(released.released && !released.pressed && !released.held && released.value==0);
    CHECK(Zero(clutch.Update(true,0)));
    CHECK(clutch.Update(true,1).pressed);
    CHECK(Zero(clutch.Update(false,1)) && !clutch.armed); // Focus loss/disconnect clears all edges.
    CHECK(Zero(clutch.Update(true,1)) && !clutch.armed); // Held through reconnect cannot drive.
    CHECK(Zero(clutch.Update(true,0)) && clutch.armed);
    CHECK(clutch.Update(true,1).pressed);
    CHECK(Zero(clutch.Update(true,std::numeric_limits<float>::quiet_NaN())) && !clutch.armed);
    CHECK(Zero(clutch.Update(true,1)) && !clutch.armed);

    BindingInputState gearOne,gearTwo;
    CHECK(Zero(gearOne.Update(true,1)) && !gearOne.armed);
    CHECK(Zero(gearTwo.Update(true,0)) && gearTwo.armed);
    CHECK(gearTwo.Update(true,1).pressed && !gearOne.armed); // Independent shifter actions.

    // Configure never touches hardware; protocol v2 remains supported even
    // while missing, so callers cannot silently sample a different instance.
    BindingInput input;
    pedal.calibration=1;pedal.dead=.03f;pedal.saturation=1;
    auto legacy=pedal;legacy.deviceId.clear();
    auto other=pedal;other.deviceId="{01234568-0000-0000-0000-504944564944}";
    input.Configure({pedal,other,legacy,key});
    CHECK(input.Samples().size()==4);
    CHECK(input.Samples()[0].supported && input.Samples()[1].supported && !input.Samples()[2].supported && input.Samples()[3].supported);
    for(const auto& sample:input.Samples())CHECK(!sample.connected && Zero(sample.state));
    input.Configure({});CHECK(input.Samples().empty());
    input.Release();input.Poll();CHECK(input.Samples().empty());
    std::puts("driving_controls_input: calibration, mapping, neutral rearm and protocol identity tests passed");
    return 0;
}
