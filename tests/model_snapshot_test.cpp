#include "model_snapshot.h"
#include <cstdlib>
using namespace grid2vr;
void Require(bool value) { if (!value) std::abort(); }
int main() {
    ModelDrawSnapshot original{},current{};
    original.identity.shader=CrowdModelShader; original.identity.drawable=0x1000;
    original.identity.transform=0x2000; original.identity.argCount=3;
    original.identity.args={12,0,0,0,0}; original.constants.fill(1);
    current=original; current.constants.fill(2);
    std::array<uint8_t,64> model{}; model.fill(2);
    std::array<uint8_t,128> output{};
    Require(PrepareModelSnapshot(original,current,model,output));
    Require(output[0]==1 && output[63]==1 && output[64]==2 && output[127]==2);
    auto mismatched=current; mismatched.identity.args[0]++;
    Require(!PrepareModelSnapshot(original,mismatched,model,output));
    mismatched=current; mismatched.identity.transform+=16;
    Require(!PrepareModelSnapshot(original,mismatched,model,output));
    mismatched=current; mismatched.identity.vertex=123;
    Require(!PrepareModelSnapshot(original,mismatched,model,output));
    original.identity.shader=0; current.identity.shader=0;
    Require(!PrepareModelSnapshot(original,current,model,output));
    original.identity.shader=current.identity.shader=CrowdModelShader;
    model[7]=0; Require(!PrepareModelSnapshot(original,current,model,output));
}
