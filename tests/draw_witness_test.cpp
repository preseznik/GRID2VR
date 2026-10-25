#include "draw_witness.h"
#include <cstdio>

int main() {
    static grid2vr::CenterDrawWitness witness;
    witness.Add({1,1,0,1296,"Draw"});
    if (witness.Current().count || witness.Previous().scene) return 1;
    witness.Begin(355);
    witness.Add({0xcfc3cf441f08e879ull,1504,0,1296,"Indexed"});
    witness.Begin(416);
    for (unsigned i=0;i<witness.MaxDraws+3;++i) witness.Add({i,i+1,6,224,"Draw"});
    const auto& failed=witness.Current();
    if (failed.scene!=416 || failed.count!=witness.MaxDraws || !failed.truncated ||
        failed.draws.back().ordinal!=witness.MaxDraws) return 2;
    const auto& previous=witness.Previous();
    if (previous.scene!=355 || previous.count!=1 || previous.truncated ||
        previous.draws[0].shader!=0xcfc3cf441f08e879ull || previous.draws[0].cb0Bytes!=1296) return 3;
    witness.Begin(477);
    if (witness.Current().scene!=477 || witness.Current().count || witness.Current().truncated ||
        witness.Previous().scene!=416 || witness.Previous().count!=witness.MaxDraws) return 4;
    witness.Truncate();
    if (!witness.Current().truncated) return 5;
    puts("Native draw witnesses retain only two separate bounded scenes and expose truncation.");
}
