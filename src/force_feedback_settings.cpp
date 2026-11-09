#include "force_feedback_settings.h"
#include "guarded_hooks.h"
#include <windows.h>
#include <atomic>
#include <bit>
#include <cstring>
#include <fstream>
#include <mutex>

namespace grid2vr::forcefeedback {
namespace {
std::atomic<std::uint32_t> values[3]{};      // launcher scales, float bits
std::atomic<std::uint32_t> native[3]{},applied[3]{};
std::atomic<std::uint64_t> calls{};
std::uintptr_t resumeAddress{};
std::filesystem::path folder;
std::mutex mutex;
Settings requested;
const char* reason="not_requested";
bool installed{};
std::uint64_t written{};

// Game thread, inside the FFB setup routine. fields is processor+0xa0, already
// holding the game's three scales.
void __stdcall ApplyStores(float* fields) {
    for(unsigned i=0;i<3;++i) {
        native[i].store(std::bit_cast<std::uint32_t>(fields[i]),std::memory_order_relaxed);
        const auto value=values[i].load(std::memory_order_relaxed);
        fields[i]=std::bit_cast<float>(value);applied[i].store(value,std::memory_order_relaxed);
    }
    calls.fetch_add(1,std::memory_order_release);
}
// Entered by "jmp" from the patched store with the routine's registers intact:
// ebx the processor, ebp its frame, xmm1 strength, [ebp-0xc] weight, xmm0
// effects. Performs the game's own three stores, then the launcher's, and
// resumes at pop edi. XMM registers are not live after this point.
__declspec(naked) void Stores() {
    __asm {
        movss dword ptr [ebx+0A0h],xmm1
        movss xmm1,dword ptr [ebp-0Ch]
        movss dword ptr [ebx+0A4h],xmm1
        movss dword ptr [ebx+0A8h],xmm0
        pushad
        lea eax,[ebx+0A0h]
        push eax
        call ApplyStores
        popad
        jmp dword ptr [resumeAddress]
    }
}
void Receipt() {
    if(folder.empty())return;
    const auto counters=Snapshot();
    std::ofstream file(folder/L"force-feedback-settings.json",std::ios::trunc);
    file<<"{\"schema\":1,\"requested\":{\"strength\":"<<requested.strength<<",\"weight\":"<<requested.weight<<",\"effects\":"<<requested.effects
        <<"},\"installed\":"<<(installed?"true":"false")<<",\"reason\":\""<<reason<<"\",\"calls\":"<<counters.calls
        <<",\"native\":["<<counters.native[0]<<','<<counters.native[1]<<','<<counters.native[2]
        <<"],\"applied\":["<<counters.applied[0]<<','<<counters.applied[1]<<','<<counters.applied[2]
        <<"],\"profileWritten\":false,\"dataFilesWritten\":false}\n";
}
}

const void* Stub() {return reinterpret_cast<const void*>(&Stores);}
void SetValues(const Settings& value,std::uintptr_t resume) {
    const float scales[3]{value.strength/100.f,value.weight/100.f,value.effects/100.f};
    for(unsigned i=0;i<3;++i)values[i].store(std::bit_cast<std::uint32_t>(scales[i]));
    resumeAddress=resume;
}
Counters Snapshot() {
    Counters result{calls.load(std::memory_order_acquire),{},{}};
    for(unsigned i=0;i<3;++i) {
        result.native[i]=std::bit_cast<float>(native[i].load(std::memory_order_relaxed));
        result.applied[i]=std::bit_cast<float>(applied[i].load(std::memory_order_relaxed));
    }
    return result;
}
bool PatchStore(std::uint8_t* guard,const void* stub) {
    if(!guard || !stub || !MatchesInstructions(guard,GuardBytes))return false;
    auto* site=guard+StoreOffset;
    const auto target=reinterpret_cast<std::intptr_t>(stub),next=reinterpret_cast<std::intptr_t>(site+5);
    const std::int64_t distance=std::int64_t(target)-next;
    if(distance<INT32_MIN || distance>INT32_MAX)return false;
    std::uint8_t replacement[StoreBytes]{0xe9,0,0,0,0,0x90,0x90,0x90};
    const auto rel=static_cast<std::int32_t>(distance);std::memcpy(replacement+1,&rel,4);
    MEMORY_BASIC_INFORMATION region{};
    if(!VirtualQuery(site,&region,sizeof(region)) || region.State!=MEM_COMMIT ||
       (region.Protect!=PAGE_EXECUTE_READ && region.Protect!=PAGE_EXECUTE_READWRITE) ||
       reinterpret_cast<std::uintptr_t>(site)+StoreBytes>reinterpret_cast<std::uintptr_t>(region.BaseAddress)+region.RegionSize)return false;
    DWORD protection{};
    if(!VirtualProtect(site,StoreBytes,PAGE_EXECUTE_READWRITE,&protection))return false;
    std::memcpy(site,replacement,StoreBytes);
    DWORD ignored{};
    const bool flushed=FlushInstructionCache(GetCurrentProcess(),site,StoreBytes)!=FALSE;
    if(flushed && VirtualProtect(site,StoreBytes,protection,&ignored))return true;
    // A failed install must not leave a partial behaviour change.
    std::memcpy(site,GuardBytes.data()+StoreOffset,StoreBytes);
    FlushInstructionCache(GetCurrentProcess(),site,StoreBytes);
    VirtualProtect(site,StoreBytes,protection,&ignored);
    return false;
}
void Install(std::uintptr_t base,const std::filesystem::path& output) {
    std::lock_guard lock(mutex);
    if(!folder.empty() || !base)return;
    folder=output;
    std::error_code error;
    if(!std::filesystem::exists(output/L"force-feedback.request",error))return;
    std::ifstream request(output/L"force-feedback.request");
    if(!ReadRequest(request,requested)) {reason="invalid_request";Receipt();return;}
    SetValues(requested,base+ResumeRva);
    if(!MatchesInstructions(reinterpret_cast<void*>(base+GuardRva),GuardBytes)) {reason="unexpected_game_code";Receipt();return;}
    installed=PatchStore(reinterpret_cast<std::uint8_t*>(base+GuardRva),Stub());
    reason=installed?"waiting_for_controller_setup":"patch_failed";
    Receipt();
}
void Tick() {
    std::lock_guard lock(mutex);
    if(!installed)return;
    const auto current=calls.load(std::memory_order_acquire);
    if(current==written)return;
    written=current;reason="applied";Receipt();
}
}
