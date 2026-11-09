#include "force_feedback_settings.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
using namespace grid2vr::forcefeedback;
namespace {
struct Args { std::uint8_t* object; float strength,weight,effects; };
using Routine=void(__stdcall*)(Args*);
// A frame like GRID 2's setup routine (push ebp; sub esp,0x10; push esi/ebx/edi),
// with ebx the processor and the three scales in its locals, followed by the
// exact shipped tail from VA 0x893a5e. The routine's own epilogue returns.
constexpr std::uint8_t Prologue[]{
    0x55,0x8b,0xec,0x83,0xec,0x10,0x56,0x53,0x57,
    0x8b,0x45,0x08,0x8b,0x18,                 // mov eax,[ebp+8]; mov ebx,[eax]
    0x8b,0x48,0x04,0x89,0x4d,0xf8,            // strength -> [ebp-8]
    0x8b,0x48,0x08,0x89,0x4d,0xf4,            // weight   -> [ebp-0xc]
    0x8b,0x48,0x0c,0x89,0x4d,0xfc,            // effects  -> [ebp-4]
    0xeb,0x02};                               // over the tail's "jmp +5" onto its effects load
struct Code {
    std::uint8_t* memory{};
    std::uint8_t* guard() const {return memory+sizeof(Prologue);}
    Routine routine() const {return reinterpret_cast<Routine>(memory);}
    explicit Code(bool corrupt=false) {
        memory=static_cast<std::uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        std::memcpy(memory,Prologue,sizeof(Prologue));std::memcpy(guard(),GuardBytes.data(),GuardBytes.size());
        if(corrupt)guard()[20]^=1;
        DWORD old{};VirtualProtect(memory,4096,PAGE_EXECUTE_READ,&old);FlushInstructionCache(GetCurrentProcess(),memory,4096);
    }
    ~Code() {VirtualFree(memory,0,MEM_RELEASE);}
};
std::array<float,3> Run(const Code& code,float s,float w,float e) {
    alignas(16) std::uint8_t object[0x100]{};
    Args args{object,s,w,e};code.routine()(&args);
    std::array<float,3> fields{};std::memcpy(fields.data(),object+0xa0,sizeof(fields));
    for(unsigned i=0;i<0x100;++i) if((i<0xa0 || i>=0xac) && object[i])return {-9,-9,-9}; // nothing else written
    return fields;
}
bool Near(float a,float b) {return std::fabs(a-b)<1e-6f;}
DWORD Protection(const void* address) {MEMORY_BASIC_INFORMATION r{};VirtualQuery(address,&r,sizeof(r));return r.Protect;}
}
int main(int argc,char** argv) {
    if(argc!=2)return 1;
    // Requests: whole percentages, weight at least 1%.
    Settings value;
    for(const char* bad:{"", "100 50", "101 50 50", "100 0 50", "-1 50 50", "100 50 101", "50 50 50 1", "5.5 50 50", "a 50 50"}) {
        std::istringstream input(bad);if(ReadRequest(input,value))return 2;
    }
    {std::istringstream input("0 1 100\n");if(!ReadRequest(input,value) || value.strength!=0 || value.weight!=1 || value.effects!=100)return 3;}
    if(!ValidSettings(Settings{}) || Settings{}.strength!=100 || Settings{}.weight!=50 || Settings{}.effects!=50)return 4;

    // The unpatched tail stores the game's own scales, and nothing else.
    {Code native;const auto fields=Run(native,.9f,.4f,.6f);
     if(!Near(fields[0],.9f) || !Near(fields[1],.4f) || !Near(fields[2],.6f))return 5;}
    // Any other code is refused and left exactly as it was.
    {Code corrupt(true);std::vector<std::uint8_t> before(corrupt.guard(),corrupt.guard()+GuardBytes.size());
     if(PatchStore(corrupt.guard(),Stub()) || std::memcmp(before.data(),corrupt.guard(),before.size()) || PatchStore(nullptr,Stub()))return 6;}

    // Install from a request: patch only the 8-byte store, keep protection.
    const auto folder=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(folder);
    std::filesystem::remove(folder/L"force-feedback-settings.json");
    std::ofstream(folder/L"force-feedback.request")<<"70 25 0\n";
    Code code;const auto base=reinterpret_cast<std::uintptr_t>(code.guard())-GuardRva;
    Install(base,folder);
    const auto* site=code.guard()+StoreOffset;
    std::int32_t rel{};std::memcpy(&rel,site+1,4);
    if(site[0]!=0xe9 || site+5+rel!=Stub() || site[5]!=0x90 || site[6]!=0x90 || site[7]!=0x90 ||
       std::memcmp(code.guard(),GuardBytes.data(),StoreOffset) ||
       std::memcmp(site+StoreBytes,GuardBytes.data()+StoreOffset+StoreBytes,GuardBytes.size()-StoreOffset-StoreBytes) ||
       Protection(site)!=PAGE_EXECUTE_READ)return 7;
    {std::ifstream receipt(folder/L"force-feedback-settings.json");std::string text((std::istreambuf_iterator<char>(receipt)),{});
     if(text.find("\"installed\":true")==std::string::npos || text.find("waiting_for_controller_setup")==std::string::npos ||
        text.find("\"strength\":70,\"weight\":25,\"effects\":0")==std::string::npos)return 8;}
    // The game's values are replaced by the launcher's on every setup, whatever
    // the in-game settings were, including FFB switched off (all zero).
    for(const auto& game:{std::array<float,3>{1,.5f,.5f},std::array<float,3>{0,0,0},std::array<float,3>{.3f,.9f,.1f}}) {
        const auto before=Snapshot().calls;
        const auto fields=Run(code,game[0],game[1],game[2]);
        const auto after=Snapshot();
        if(!Near(fields[0],.7f) || !Near(fields[1],.25f) || !Near(fields[2],0) || after.calls!=before+1 ||
           !Near(after.native[0],game[0]) || !Near(after.native[1],game[1]) || !Near(after.native[2],game[2]) ||
           !Near(after.applied[0],.7f) || !Near(after.applied[1],.25f))return 9;
    }
    Tick();
    {std::ifstream receipt(folder/L"force-feedback-settings.json");std::string text((std::istreambuf_iterator<char>(receipt)),{});
     if(text.find("\"reason\":\"applied\"")==std::string::npos || text.find("\"calls\":3")==std::string::npos ||
        text.find("\"profileWritten\":false")==std::string::npos)return 10;}
    std::filesystem::remove(folder/L"force-feedback.request");
    puts("Launcher force feedback: requests, exact guard, 8-byte patch, native and launcher scales, receipt passed.");
    return 0;
}
