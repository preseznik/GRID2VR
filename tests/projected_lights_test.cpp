#include "projected_lights.h"
#include <cstdio>
#include <vector>
using namespace grid2vr::projectedlights;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(){
    PreparedCalls calls;Camera center{};center[0x70]=4;center[0x150]=5;
    Call a{};a.self=1;a.light=2;a.context=3;a.material=4;a.index=1;a.supported=true;a.camera=center;
    struct Seen{uintptr_t self,light,context,material;unsigned index;Camera camera;};std::vector<Seen> seen;
    auto valid=[](const Call&){return true;};
    auto invoke=[&](const Call& c,uintptr_t context){seen.push_back({c.self,c.light,context,c.material,c.index,c.camera});};
    CHECK(calls.Begin(10));calls.Remember(10,a);
    CHECK(!calls.Snapshot(11,3,center));CHECK(calls.Snapshot(10,3,center));
    CHECK(calls.Apply(10,3,valid,invoke));CHECK(calls.Dirty());CHECK(!calls.Clear());CHECK(!calls.Begin(11));
    CHECK(calls.Apply(10,3,valid,invoke));CHECK(!calls.Apply(10,3,valid,invoke));
    CHECK(calls.Restore(10,3,valid,invoke));CHECK(!calls.Dirty());CHECK(seen.size()==3);
    for(const auto& s:seen){CHECK(s.self==1 && s.light==2 && s.material==4 && s.index==1);CHECK(s.camera==center);}
    CHECK(seen[0].context==3 && seen[1].context==3 && seen[2].context!=3 && seen[2].context%16==0);
    CHECK(calls.Clear());CHECK(calls.Begin(11));
    auto wrong=a;wrong.camera[0x150]^=1;calls.Remember(11,wrong);CHECK(!calls.Snapshot(11,3,center));
    CHECK(calls.Begin(12));wrong=a;wrong.supported=false;calls.Remember(12,wrong);CHECK(!calls.Snapshot(12,3,center));
    CHECK(calls.Begin(13));calls.Remember(13,a);CHECK(calls.Snapshot(13,3,center));
    auto reject=[](const Call&){return false;};seen.clear();CHECK(!calls.Apply(13,3,reject,invoke));CHECK(seen.empty() && !calls.Dirty());
    CHECK(calls.Apply(13,3,valid,invoke));CHECK(!calls.Restore(13,3,reject,invoke));CHECK(calls.Dirty());
    CHECK(calls.Restore(13,3,valid,invoke));CHECK(!calls.Dirty());
    CHECK(calls.Begin(14));for(unsigned i=0;i<513;++i)calls.Remember(14,a);CHECK(!calls.Snapshot(14,3,center));
    CHECK(calls.Begin(15));CHECK(calls.Snapshot(15,3,center));calls.Remember(15,a);CHECK(!calls.Apply(15,3,valid,invoke));
    CHECK(calls.Begin(16));wrong=a;wrong.context=5;wrong.supported=false;calls.Remember(16,wrong);
    CHECK(calls.Snapshot(16,3,center));CHECK(calls.Count()==0);CHECK(calls.Apply(16,3,valid,invoke));CHECK(calls.Restore(16,3,valid,invoke));
    CHECK(calls.Begin(17));calls.Remember(16,a);CHECK(!calls.Snapshot(17,3,center));
    CHECK(calls.Begin(18));calls.Remember(18,a);CHECK(calls.Snapshot(18,3,center));seen.clear();
    CHECK(calls.Apply(18,3,valid,invoke));CHECK(calls.Restore(18,3,valid,invoke));
    CHECK(calls.Apply(18,3,valid,invoke));CHECK(calls.Restore(18,3,valid,invoke));
    CHECK(seen.size()==4 && seen[0].context==3 && seen[2].context==3);
    CHECK(seen[1].context==seen[3].context && seen[1].context!=3 && !calls.Dirty());
    puts("Projected lights: argument order, two eyes, saved-camera restore, frame/context ownership and partial/overflow refusals pass.");
}
