#include "opponent_lod_bridge.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#define CHECK(value) do {if(!(value)){std::printf("bridge failure line %d: %s\n",__LINE__,#value);std::exit(1);}}while(false)
namespace {
int MarkerOne() {return 1;} int MarkerTwo() {return 2;}
struct State {bool approve=true;unsigned validated{},published{};};
bool Validate(void* pointer) noexcept {auto& s=*static_cast<State*>(pointer);++s.validated;return s.approve;}
void Publish(void* pointer) noexcept {++static_cast<State*>(pointer)->published;}
struct WorkerState {volatile LONG ready{},stop{};uintptr_t marker{};};
DWORD WINAPI HoldMarker(void* pointer) {
    auto& state=*static_cast<WorkerState*>(pointer);
    volatile uintptr_t marker=state.marker;
    InterlockedExchange(&state.ready,1);
    while(!state.stop)Sleep(1);
    return static_cast<DWORD>(marker);
}
std::string Text(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};
}
}
int main() {
    namespace bridge=grid2vr::opponentlod::bridge;
    namespace runtime=grid2vr::opponentlod::runtime;
    auto slots=static_cast<void**>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));CHECK(slots);
    slots[0]=reinterpret_cast<void*>(&MarkerOne);slots[1]=reinterpret_cast<void*>(&MarkerTwo);
    const std::array original{slots[0],slots[1]};
    std::array<runtime::PointerChange,2> writes{{{slots,original[0],original[1]},{slots+1,original[1],original[0]}}};
    // The qualified runtime has more than the old 32-range capacity. Use an
    // uncalled fixture-code interval, not low integers like 0x10000 which are
    // common flag values in unrelated OS worker stacks. Repeated intervals are
    // valid and exercise descriptor capacity without creating false busy flags.
    std::array<runtime::BusyRange,35> ranges{};
    const auto marker=reinterpret_cast<uintptr_t>(&MarkerOne);
    for(auto& range:ranges)range={marker+1,marker+2};
    State state;
    const auto folder=std::filesystem::current_path()/(L"bridge-receipts-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);bridge::Configure(folder);
#ifdef GRID2VR_OPPONENT_LOD_SINGLE_ATTEMPT_DIAGNOSTIC
    const auto probe=bridge::Transaction(writes,ranges,Validate,&state,Publish);
    CHECK(probe && !std::strcmp(probe,"diagnostic_probe_safe_no_writes"));
    CHECK(bridge::LastTransaction().probePassed && !bridge::LastTransaction().committed && !bridge::LastTransaction().writtenMask);
    CHECK(state.validated==1 && !state.published && slots[0]==original[0] && slots[1]==original[1]);
    state.approve=false;CHECK(bridge::Transaction(writes,ranges,Validate,&state,Publish));
    CHECK(bridge::LastTransaction().failure==grid2vr::quiescence::Failure::ValidationRefused && !state.published);
    state.approve=true;
#else
    const auto firstFailure=bridge::Transaction(writes,ranges,Validate,&state,Publish);
    if(firstFailure) {
        const auto detail=bridge::LastTransaction();
        std::printf("transaction reason=%s failure=%u index=%zu win32=%lu nt=%lx peers=%zu layout=%u\n",firstFailure,
            static_cast<unsigned>(detail.failure),detail.index,detail.win32Error,static_cast<unsigned long>(detail.ntStatus),detail.peerCount,detail.layoutValidated);
    }
    CHECK(!firstFailure);
    CHECK(state.published==1 && slots[0]==original[1] && slots[1]==original[0]);
    CHECK(bridge::LastTransaction().committed && bridge::LastTransaction().writtenMask==3);
    const auto swapped=std::array{slots[0],slots[1]};
    CHECK(bridge::Transaction(writes,ranges,Validate,&state,Publish)!=nullptr);
    CHECK(state.published==1 && slots[0]==swapped[0] && slots[1]==swapped[1]);
    std::array<runtime::PointerChange,2> restore{{{slots,swapped[0],original[0]},{slots+1,swapped[1],original[1]}}};
    state.approve=false;CHECK(bridge::Transaction(restore,ranges,Validate,&state,Publish)!=nullptr);
    CHECK(state.published==1 && slots[0]==swapped[0] && slots[1]==swapped[1]);
    state.approve=true;CHECK(!bridge::Transaction(restore,ranges,Validate,&state,Publish));
    CHECK(state.published==2 && slots[0]==original[0] && slots[1]==original[1]);
#endif
    const auto beforePublication=state.published;
    WorkerState worker;worker.marker=marker+1;
    const auto handle=CreateThread(nullptr,0,HoldMarker,&worker,0,nullptr);CHECK(handle);
    for(unsigned i=0;i<5000 && !worker.ready;++i)Sleep(1);
    CHECK(worker.ready);
    const auto refusal=bridge::Transaction(writes,ranges,Validate,&state,Publish);
    const auto busy=bridge::LastTransaction();
    InterlockedExchange(&worker.stop,1);CHECK(WaitForSingleObject(handle,10000)==WAIT_OBJECT_0);
    CHECK(CloseHandle(handle));
    CHECK(refusal && !std::strcmp(refusal,"BusyStack") && busy.failure==grid2vr::quiescence::Failure::BusyStack);
    CHECK(busy.threadId && busy.instruction && busy.stackPointer && busy.busyValue==marker+1 && busy.busyStackAddress>=busy.stackPointer);
    CHECK(busy.busyRangeIndex==0 && busy.busyRangeBegin==marker+1 && busy.busyRangeEnd==marker+2);
    CHECK(busy.timingAvailable && busy.pauseScopeMeasured && busy.totalMicroseconds>=busy.pauseScopeMicroseconds);
    CHECK(!busy.committed && !busy.writtenMask && !busy.resumeFailures && !busy.closeFailures);
    const auto receipt=Text(folder/L"opponent-lod-transaction.json");
    CHECK(receipt.size()<8192 && receipt.find("\"failure\":\"BusyStack\"")!=std::string::npos);
#ifdef GRID2VR_OPPONENT_LOD_CONTROLLED_ACTIVATION
    CHECK(receipt.find("\"controlledActivation\":true")!=std::string::npos && receipt.find("\"probeOnly\":false")!=std::string::npos);
#else
    CHECK(receipt.find("\"controlledActivation\":false")!=std::string::npos);
#endif
    CHECK(receipt.find("\"busyValue\":"+std::to_string(busy.busyValue))!=std::string::npos &&
        receipt.find("\"busyStackAddress\":"+std::to_string(busy.busyStackAddress))!=std::string::npos &&
        receipt.find("\"pauseScopeMicroseconds\":"+std::to_string(busy.pauseScopeMicroseconds))!=std::string::npos &&
        receipt.find("\"rawStackRetained\":false")!=std::string::npos);
    std::array<runtime::PointerChange,grid2vr::quiescence::MaxWrites+1> tooMany{};
    for(unsigned i=0;i<70;++i)CHECK(bridge::Transaction(tooMany,ranges,Validate,&state,Publish)!=nullptr);
    const auto journal=Text(folder/L"opponent-lod-transaction.jsonl");
    CHECK(std::count(journal.begin(),journal.end(),'\n')==64 && journal.size()<64*8192);
    CHECK(state.published==beforePublication && slots[0]==original[0] && slots[1]==original[1]);
    std::filesystem::remove(folder/L"opponent-lod-transaction.json");
    std::filesystem::remove(folder/L"opponent-lod-transaction.jsonl");std::filesystem::remove(folder);
    CHECK(VirtualFree(slots,0,MEM_RELEASE));
    std::puts("PASS: real complete-stack transaction bridge, 35 ranges, two-pointer commit/refusal/restore; no game execution");
}
