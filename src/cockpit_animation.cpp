#include "cockpit_animation.h"
#include "game_executable.h"
#include "guarded_hooks.h"
#include <array>
#include <atomic>
#include <fstream>
#include <mutex>
#include <vector>

namespace grid2vr::cockpitanimation {
namespace {
struct Guard {uint32_t rva,length;const char* hash;std::vector<uint32_t> relocations;};
const std::array<Guard,10> guards{{
    {0x6c7300,129,"b2ad559f8caab27a79ce9e0b73ec8b9d949c9feb263bbe9f6ef98b1c9f5ebcce",{0xc,0x13,0x1c,0x40,0x45,0x4e}},
    {0x1003d7,19,"451bd5dd43f71fb6fb4bf002b7d212d35663ba92119887ad283c65947a41485e",{5,10}},
    {0x19deed,19,"bfe78e45fe3ee7cfbe8f32ffebcc5f6a657413e67f4797cfd3918a161e343ff8",{5,10}},
    {0x6453ea,17,"683ab8e79d0b8e973009eddc689c8f688b6b3f2b63d7faa6034267ff07428895",{5}},
    {0x3f6c6f,37,"52a4b5e603d045703f324c39343066707be29ab71cdd059312fb8be8e7fb26cf",{2}},
    {0x3daac0,15,"042e5fe1cb294283ab4bfa4edf38ff53ac4311c5b2056e7e0c7d8cbf7e2154ec",{}},
    {0x1564b0,112,"d130aaa4345463feee48496893eeaaaeb52ecd5f8ab9b9858f851bb631458f83",{0x1e}},
    {0x920a0,54,"3b02b948fddb70d6f873518fa43440bdef9dfe674a18af1263a7dfe983b67da0",{}},
    {0xdcb64,48,"3c7a225e20cc98ee33707d2ec20cd9e17e1245d963f7aa043e3196f2a6cf0032",{2}},
    {0x2243e5,69,"cc1e5b9869246fde0967b0a0108268b7ac148cebd9ef4392241ac8d91b3e3ea3",{2}},
}};
bool Writable(uintptr_t address) {
    MEMORY_BASIC_INFORMATION region{};
    return !(address&3) && VirtualQuery(reinterpret_cast<void*>(address),&region,sizeof(region)) &&
        region.State==MEM_COMMIT && (region.Protect==PAGE_READWRITE || region.Protect==PAGE_EXECUTE_READWRITE) &&
        address+sizeof(LONG)<=reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize;
}
bool Exchange(uintptr_t address,int expected,int replacement) {
    if(!Writable(address))return false;
    // The pinned global is permanent image storage. Do not make read-only or
    // foreign memory writable, and do not overwrite a concurrent native change.
    return InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(address),replacement,expected)==expected;
}
QualityOverride quality;
std::mutex mutex;
std::filesystem::path output;
std::atomic<bool> stopped{};
bool attempted{},installed{},installationResult{true};
DWORD owner{};
unsigned receipts{};
void Receipt(const char* event) noexcept {
    if(output.empty() || receipts++>=32)return;
    try {
        std::ofstream out(output/L"cockpit-animation.jsonl",std::ios::app);
        out<<"{\"event\":\""<<event<<"\",\"lastObservedNativeQuality\":"<<quality.Requested()
           <<",\"minimumCharacterQuality\":1,\"changes\":"<<quality.Changes()<<",\"refusals\":"<<quality.Refusals()
           <<",\"overrideActive\":"<<(quality.Active()?"true":"false")
           <<",\"geometryAndLiveryUnchanged\":true,\"savedSettingsModified\":false,\"nativeCodePatched\":false}\n";
    }catch(...) {}
}
}
bool InstructionsMatch(uintptr_t base) {
    if(!base)return false;
    for(const auto& guard:guards) {
        std::vector<uint8_t> bytes(guard.length);
        if(!ReadMemory(reinterpret_cast<void*>(base+guard.rva),bytes.data(),bytes.size()))return false;
        for(const auto at:guard.relocations) {
            uint32_t value{};memcpy(&value,bytes.data()+at,4);
            value-=static_cast<uint32_t>(base-0x400000);memcpy(bytes.data()+at,&value,4);
        }
        if(gameexe::Digest(bytes.data(),static_cast<ULONG>(bytes.size()))!=guard.hash)return false;
    }
    return true;
}
bool QualityOverride::Bind(uintptr_t address) {
    if(base || !InstructionsMatch(address) || !Writable(address+qualityRva))return false;
    base=address;return true;
}
bool QualityOverride::Refresh(bool worldReady) {
    if(!base) {++refusals;return false;}
    if(!worldReady)return true;
    int value{};
    if(!ReadMemory(reinterpret_cast<void*>(base+qualityRva),&value,sizeof(value))) {
        requested=-1;owned=false;++refusals;return false;
    }
    // An unchanged injected minimum is still ours, but an identical later
    // native Medium choice is indistinguishable. Neither is written on stop.
    if(owned && value==1)return true;
    requested=value;
    if(value<0 || value>2) {
        owned=false;++refusals;return false;
    }
    owned=false;
    if(value!=0)return true;
    if(!Exchange(base+qualityRva,0,1)) {++refusals;return false;}
    owned=true;++changes;return true;
}
bool Install(uintptr_t base,const std::filesystem::path& folder) noexcept {
    try {
        std::lock_guard lock(mutex);
        if(attempted)return installationResult;
        attempted=true;
        if(!std::filesystem::is_regular_file(folder/L"runtime-session.request") ||
           !std::filesystem::is_regular_file(folder/L"xr-session.request") ||
           !std::filesystem::is_regular_file(folder/L"xr-projection.request"))return true;
        installationResult=false;
        output=folder;
        if(!quality.Bind(base)) {Receipt("refused_instructions");return false;}
        owner=GetCurrentThreadId();installed=true;Receipt("qualified_waiting_for_world");
        installationResult=true;return true;
    }catch(...) {return false;}
}
void Tick(bool worldReady) noexcept {
    if(stopped.load() || !installed || !worldReady || GetCurrentThreadId()!=owner)return;
    try {
        std::lock_guard lock(mutex);
        if(stopped.load())return;
        const auto changes=quality.Changes(),refusals=quality.Refusals();
        const auto requested=quality.Requested();
        const bool ok=quality.Refresh(worldReady);
        if(changes!=quality.Changes() || refusals!=quality.Refusals() || requested!=quality.Requested())
            Receipt(ok?"world_frame":"refused_world_frame");
    }catch(...) {}
}
void Stop() noexcept {
    stopped.store(true);
    // Native settings writes resume ownership normally. Avoid restoring stale
    // Low over a later identical Medium choice during an in-process VR stop.
    try {std::lock_guard lock(mutex);if(installed){quality.Release();Receipt("stopped_native_value_retained");}}catch(...) {}
}
}
