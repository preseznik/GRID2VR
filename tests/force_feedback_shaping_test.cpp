#include "force_feedback_shaping.h"
#include <cmath>
#include <cstdio>
#include <sstream>

using namespace grid2vr::forcefeedback;
#define CHECK(value) do {if(!(value)){std::printf("force feedback shaping check line %d: %s\n",__LINE__,#value);return 1;}} while(false)

namespace {
bool Parses(const char* text,Settings* out=nullptr) {std::istringstream in(text);Settings s;const bool ok=ReadRequest(in,s);if(out)*out=s;return ok;}
constexpr double frame=1.0/60.0;
// Peak-to-peak of the shaped output over the last half of a run of a DC + sine input.
long Swing(Shaper& shaper,double dc,double amplitude,double hz,long* mean=nullptr) {
    long low=nominalMax,high=-nominalMax;double sum{};int n{};
    for(int i=0;i<600;++i) {
        const auto x=std::lround(dc+amplitude*std::sin(2*3.14159265358979323846*hz*i*frame));
        const auto y=shaper.Apply(x,frame);
        if(i>=300) {low=std::min(low,y);high=std::max(high,y);sum+=y;++n;}
    }
    if(mean)*mean=std::lround(sum/n);
    return high-low;
}
}

int main() {
    // Requests: whole percentages, strength 0-100, weight 1-100, vibration 0-100.
    Settings parsed;
    CHECK(Parses("70 25 0",&parsed) && parsed==(Settings{70,25,0}));
    CHECK(Parses("100 100 100\r\n",&parsed) && parsed==Settings{});
    for(const char* bad:{"","100 50","100 0 50","101 50 50","100 50 101","-1 50 50","100 50 50 7","100 50 x"}) CHECK(!Parses(bad));

    // 100/100/100 passes every value through exactly, including clipped peaks.
    Shaper neutral;
    CHECK(neutral.Neutral());
    for(int i=0;i<500;++i) {const long x=std::lround(9999*std::sin(i*0.37));CHECK(neutral.Apply(x,frame)==x);}
    CHECK(neutral.Apply(nominalMax,frame)==nominalMax && neutral.Apply(-nominalMax,0.5)==-nominalMax);

    // Strength alone scales every value, whatever the split.
    Shaper half(Settings{50,100,100});
    for(int i=0;i<200;++i) {const long x=std::lround(8000*std::sin(i*0.9));CHECK(std::labs(half.Apply(x,frame)-x/2)<=1);}

    // Weight scales the steady (aligning) force; vibration leaves it alone.
    Shaper light(Settings{100,40,100});long mean{};
    Swing(light,6000,0,1,&mean);CHECK(std::labs(mean-2400)<=5);
    Shaper noFx(Settings{100,100,0});
    Swing(noFx,6000,0,1,&mean);CHECK(std::labs(mean-6000)<=5);

    // Vibration scales fast detail (a 15 Hz kerb rumble), weight leaves most of it.
    Shaper base;const auto rumble=Swing(base,3000,2000,15);
    CHECK(rumble>=3900);
    Shaper calm(Settings{100,100,0});const auto calmed=Swing(calm,3000,2000,15,&mean);
    CHECK(calmed<rumble/4 && std::labs(mean-3000)<=150);
    Shaper strong(Settings{100,30,100});const auto kept=Swing(strong,3000,2000,15,&mean);
    CHECK(kept>rumble*7/10 && std::labs(mean-900)<=150);
    // Slow steering changes (0.5 Hz) follow weight, not vibration.
    Shaper slowOnly(Settings{100,100,0});const auto turned=Swing(slowOnly,0,5000,0.5);
    Shaper reference;CHECK(turned>Swing(reference,0,5000,0.5)*8/10);

    // A step: the weight part follows within a few frames; vibration carries the edge.
    Shaper edge(Settings{100,100,0});edge.Apply(0,frame);
    const auto first=edge.Apply(6000,frame);CHECK(first>500 && first<6000*6/10);
    long settled{};for(int i=0;i<30;++i)settled=edge.Apply(6000,frame);CHECK(settled>=5990);

    // A pause (menus, loading) restarts the split at the current value.
    Shaper pause(Settings{100,100,0});for(int i=0;i<60;++i)pause.Apply(0,frame);
    CHECK(pause.Apply(7000,1.0)==7000);

    // Output never exceeds DirectInput's nominal range.
    Shaper sharp(Settings{100,1,100});sharp.Apply(-nominalMax,frame);for(int i=0;i<30;++i)sharp.Apply(-nominalMax,frame);
    CHECK(sharp.Apply(nominalMax,frame)==nominalMax);
    for(int i=0;i<300;++i) {const long y=sharp.Apply(i%2?nominalMax:-nominalMax,frame);CHECK(y>=-nominalMax && y<=nominalMax);}

    // Zero strength is silent.
    Shaper off(Settings{0,50,50});for(int i=0;i<50;++i)CHECK(off.Apply(std::lround(9000*std::sin(i*1.3)),frame)==0);
    std::puts("PASS: launcher force feedback request parsing; neutral pass-through; strength scaling; weight/vibration split at 3 Hz; restart after pauses; clamp");
    return 0;
}
