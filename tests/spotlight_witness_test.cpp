#include "spotlight_witness.h"
#include <cstdio>
using namespace grid2vr::spotlightwitness;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main() {
    CHECK(Selected(SpotlightVs,SpotlightPs[0]));CHECK(Selected(SpotlightVs,SpotlightPs[1]));
    CHECK(Selected(SpotlightVs,SpotlightPs[2]));CHECK(Selected(StencilVs,0));
    CHECK(!Selected(SpotlightVs,0));CHECK(!Selected(0,SpotlightPs[0]));
    for (unsigned size:{128,192,320,368,432,464}) CHECK(TracksSize(size));
    for (unsigned size:{0,127,129,191,193,319,321,367,369,431,433,463,465,65536}) CHECK(!TracksSize(size));
    Selection variants;
    for (unsigned i=0;i<SpotlightPs.size();++i) {
        CHECK(Selected(SpotlightVs,SpotlightPs[i]));
        CHECK(PixelBufferSize(SpotlightPs[i])==(i<3?432:464));
        for (unsigned pass=0;pass<3;++pass) CHECK(variants.Take(1,pass,0,SpotlightVs,SpotlightPs[i]));
    }
    CHECK(!PixelBufferSize(0));
    for (unsigned pass=0;pass<3;++pass) CHECK(variants.Take(1,pass,0,StencilVs,0));
    Selection select;
    CHECK(!select.Take(0,0,6,SpotlightVs,SpotlightPs[0]));
    CHECK(!select.Take(1,3,6,SpotlightVs,SpotlightPs[0]));
    CHECK(!select.Take(1,1,6,SpotlightVs,SpotlightPs[0])); // Require the original scene first.
    CHECK(select.Take(1,0,6,SpotlightVs,SpotlightPs[0]));
    CHECK(!select.Take(1,0,6,SpotlightVs,SpotlightPs[0]));
    CHECK(!select.Take(2,1,6,SpotlightVs,SpotlightPs[0])); // Never combine unrelated scenes.
    CHECK(select.Take(1,1,6,SpotlightVs,SpotlightPs[0]));
    auto* complete=select.Take(1,2,6,SpotlightVs,SpotlightPs[0]);CHECK(complete);
    CHECK(Selection::Finish(*complete,true));
    CHECK(!select.Take(2,0,6,SpotlightVs,SpotlightPs[0])); // Bounded even after completion.
    CHECK(select.Take(2,0,6,SpotlightVs,SpotlightPs[1]));
    CHECK(select.Take(3,0,6,SpotlightVs,SpotlightPs[2]));
    CHECK(select.Take(4,0,6,StencilVs,0));
    CHECK(select.Take(4,1,6,StencilVs,123)); // Unused PS binding cannot consume another group.
    CHECK(select.Take(4,2,6,StencilVs,456));
    CHECK(select.Take(5,0,7,SpotlightVs,SpotlightPs[0])); // Independent stage budget.
    CHECK(select.Take(2,1,6,SpotlightVs,SpotlightPs[1]));
    CHECK(select.Take(2,2,6,SpotlightVs,SpotlightPs[1]));
    CHECK(select.Take(3,1,6,SpotlightVs,SpotlightPs[2]));
    CHECK(select.Take(3,2,6,SpotlightVs,SpotlightPs[2]));
    unsigned total=0;for(const auto& group:select.groups)if(group.mask==7)total+=3;
    CHECK(total==12);
    Selection menus;
    for(uint64_t scene=1;scene<1000;++scene) {
        CHECK(menus.Take(scene,0,6,SpotlightVs,SpotlightPs[0]));
        CHECK(!menus.Take(scene+1,1,6,SpotlightVs,SpotlightPs[0]));
    }
    CHECK(menus.Take(1000,0,6,SpotlightVs,SpotlightPs[0]));
    CHECK(menus.Take(1000,1,6,SpotlightVs,SpotlightPs[0]));
    complete=menus.Take(1000,2,6,SpotlightVs,SpotlightPs[0]);CHECK(complete);
    CHECK(Selection::Finish(*complete,true));
    CHECK(!menus.Take(1001,0,6,SpotlightVs,SpotlightPs[0]));
    for(unsigned slot=0;slot<3;++slot)CHECK(menus.Take(1001,0,slot,SpotlightVs,SpotlightPs[0]));
    CHECK(!menus.Take(1001,0,3,SpotlightVs,SpotlightPs[0])); // Four stages per variant.
    CHECK(menus.Take(1001,0,3,SpotlightVs,SpotlightPs[1])); // Other variant is still available.
    Selection missing;
    for(uint64_t scene=1;scene<=3;++scene) {
        CHECK(missing.Take(scene,0,6,SpotlightVs,SpotlightPs[0]));
        CHECK(missing.Take(scene,1,6,SpotlightVs,SpotlightPs[0]));
        complete=missing.Take(scene,2,6,SpotlightVs,SpotlightPs[0]);CHECK(complete);
        CHECK(Selection::Finish(*complete,scene==3)==(scene!=2));
    }
    CHECK(!missing.Take(4,0,6,SpotlightVs,SpotlightPs[0]));
    Selection neverUploaded;
    for(uint64_t scene=1;scene<=Selection::MaxAttempts;++scene) {
        CHECK(neverUploaded.Take(scene,0,6,SpotlightVs,SpotlightPs[0]));
        CHECK(neverUploaded.Take(scene,1,6,SpotlightVs,SpotlightPs[0]));
        complete=neverUploaded.Take(scene,2,6,SpotlightVs,SpotlightPs[0]);CHECK(complete);
        CHECK(Selection::Finish(*complete,false)==(scene==1));
    }
    CHECK(!neverUploaded.Take(Selection::MaxAttempts+1,0,6,SpotlightVs,SpotlightPs[0]));
    puts("Spotlight witness: exact identities, layout sizes, same-scene matching and bounded quality variants pass.");
}
