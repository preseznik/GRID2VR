#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>

namespace {
bool runtimeSession{},runtimeStopped{},nativeHooksInstalled{},nativeLaunch{},localRaceReady{},soloReady{},exitReady{},aiBenchmarkReady{};
unsigned exitCalls{};
uintptr_t gameBase=0x400000;
uint64_t f=3000;
std::filesystem::path output;
std::map<uintptr_t,uintptr_t> memory;
std::string installed;
bool EnsureExitHandler() { ++exitCalls;return exitReady; }
bool NormalSoloRaceReady(uintptr_t) { return soloReady; }
template<class Reader> bool BenchmarkAiRaceReady(uintptr_t,Reader) { return aiBenchmarkReady; }
bool ReadMemory(const void* at,void* value,size_t size) {
    const auto found=memory.find(reinterpret_cast<uintptr_t>(at));
    if(size!=sizeof(uintptr_t) || found==memory.end())return false;
    std::memcpy(value,&found->second,size);return true;
}
void Log(const char*,...) {}
namespace backgroundplay { void Install(uintptr_t,const std::filesystem::path&) { installed+='b'; } }
namespace chaseobserver { void Install() { installed+='c'; } }
namespace chasecamera { void Install() { installed+='a'; } }
namespace playercontrol { void Install(uintptr_t,const std::filesystem::path&) { installed+='p'; } }

void Tick() {
    // Generated from diagnostics.cpp, including the outer conditional. Removing
    // its exit-handler dependency must fail the refusal case below.
#include "native_session_hooks.inc"
}
void Reset() {
    runtimeSession=true;runtimeStopped=false;nativeHooksInstalled=false;
    nativeLaunch=false;localRaceReady=false;soloReady=false;exitReady=true;aiBenchmarkReady=false;
    exitCalls=0;installed.clear();memory.clear();
}
void Replay(unsigned cars=1) {
    memory={{gameBase+0xe61010,0x10000000},{0x100000a8,0x10001000},
        {0x10001000,gameBase+0xd1266c},{0x100010a8,0x10002000},
        {0x100010ac,0x10002000+4*cars},{0x10002000,0x10003000}};
}
bool Check(bool condition,const char* what) {
    if(!condition)std::fprintf(stderr,"Native session lifecycle failed: %s\n",what);
    return condition;
}
}
int main() {
    // No XR/capture request or renderer is present in any of these routes.
    Reset();soloReady=true;Tick();
    if(!Check(exitCalls==1 && installed=="bca" && nativeHooksInstalled,"desktop Practice owns exit cleanup"))return 1;
    Tick();
    if(!Check(exitCalls==1 && installed=="bca","native hooks installed once"))return 2;
    Reset();soloReady=true;exitReady=false;Tick();
    if(!Check(exitCalls==1 && installed.empty() && !nativeHooksInstalled,"exit guard refusal blocks native hooks"))return 3;
    Reset();nativeLaunch=true;localRaceReady=true;Tick();
    if(!Check(exitCalls==1 && installed=="bca" && nativeHooksInstalled,"Normal/Race preserves local controller"))return 4;
    Reset();nativeLaunch=true;soloReady=true;Replay();Tick();
    if(!Check(installed.empty() && !nativeHooksInstalled,"Normal/Race cannot use Practice readiness"))return 5;
    Reset();Replay();Tick();
    if(!Check(exitCalls==1 && installed=="bcap" && nativeHooksInstalled,"Practice recorded replay input route"))return 6;
    Reset();Replay(2);Tick();
    if(!Check(installed.empty() && !nativeHooksInstalled,"unsupported replay refuses native hooks"))return 7;
    Reset();Tick();
    if(!Check(installed.empty() && !nativeHooksInstalled,"no native owner refuses hooks"))return 8;
    Reset();runtimeSession=false;soloReady=true;Tick();
    if(!Check(!exitCalls && installed.empty(),"non-runtime session is untouched"))return 9;
    Reset();runtimeStopped=true;soloReady=true;Tick();
    if(!Check(!exitCalls && installed.empty(),"stopped session cannot reinstall"))return 10;
    // Plain -benchmark AI lap: background play only, never for native launches,
    // never without exit cleanup, and the human routes keep precedence.
    Reset();aiBenchmarkReady=true;Tick();
    if(!Check(exitCalls==1 && installed=="b" && nativeHooksInstalled,"AI benchmark installs background play only"))return 11;
    Tick();
    if(!Check(installed=="b","AI benchmark installs once"))return 12;
    Reset();aiBenchmarkReady=true;exitReady=false;Tick();
    if(!Check(installed.empty() && !nativeHooksInstalled,"AI benchmark requires exit cleanup"))return 13;
    Reset();aiBenchmarkReady=true;nativeLaunch=true;Tick();
    if(!Check(installed.empty() && !nativeHooksInstalled,"native launch never uses AI benchmark readiness"))return 14;
    Reset();aiBenchmarkReady=true;soloReady=true;Tick();
    if(!Check(installed=="bca","human Practice readiness keeps precedence"))return 15;
    std::puts("Actual desktop/native startup gate requires exit cleanup and preserves native readiness routes.");
    return 0;
}
