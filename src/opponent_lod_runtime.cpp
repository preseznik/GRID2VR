#include "opponent_lod_runtime.h"
#include "opponent_lod_native.h"
#include "opponent_lod_build_mode.h"
#include "opponent_lod_updater_table.h"
#include "nearby_opponent_lod.h"
#include "game_executable.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <intrin.h>
#include <limits>
#include <locale>
#include <vector>

#if !defined(_M_IX86)
#error The audited GRID opponent LOD runtime is MSVC x86 only.
#endif
namespace grid2vr::opponentlod::runtime {
namespace {
constexpr uint32_t ImageSize=0x100e000,ManagerSize=0x14450,RecordStride=0x1810;
constexpr uintptr_t OwnerManager=0xd967fc,UpdaterGlobal=0xdef810,ManagerTable=0xd1222c,
    UpdaterTable=0xcdcb68,CallbackRva=0x2e3930,UpdateRva=0x76e580;
constexpr unsigned MaxAttempts=16,RetryTicks=30,StatusTicks=120;
constexpr bool SingleAttemptDiagnostic=mode::Diagnostic;
constexpr bool ControlledActivation=mode::ControlledActivation;
// Full auxiliary intervals from audit_copies.py guards, plus complete bounded
// registration/router/prepare-scheduler/parent-publication bodies. HIGHLOW
// offsets are from the exact PE HIGHLOW directory, not wildcard operands. Do NOT
// require the loader's discardable .reloc section to remain mapped at runtime.
enum class BusyScope {OwnedRoute,SharedLockDependency};
struct Guard {
    uint32_t rva,size;const char* hash;std::vector<uint32_t> highlow;
    BusyScope scope{BusyScope::OwnedRoute};
};
const Guard auxiliary[]{
    {0xf0b60,0x14cc,"96622538be4021ea70d1d61651938c458e58f66fe9a7baf3e5d2a31300c0d0b1",{0x1a,0x37,0x77,0x92,0xb1,0xc1,0xd1,0xe1,0xf1,0x140,0x162,0x2de,0x2fb,0x305,0x315,0x41f,0x438,0x466,0x47f,0x4bf,0x4c6,0x4f4,0x50d,0x53b,0x554,0x594,0x59b,0x5c9,0x5e2,0x610,0x629,0x669,0x670,0x69e,0x6b7,0x6e5,0x6f7,0x71e,0x730,0x75b,0x76d,0x798,0x7aa,0x7d5,0x7e7,0x812,0x824,0x84f,0x861,0x8b7,0x8ce,0x8d6,0x8ee,0x90c,0x923,0x92b,0x943,0x961,0x978,0x980,0x998,0x9b6,0x9cd,0x9d5,0x9ed,0xa0b,0xa22,0xa2a,0xa42,0xa60,0xa77,0xa7f,0xa97,0xab5,0xacc,0xad4,0xaec,0xb0a,0xb21,0xb29,0xb41,0xb5f,0xb76,0xb7e,0xb96,0xbb4,0xbcb,0xbd3,0xbed,0xc0b,0xc22,0xc2a,0xc44,0xc62,0xc79,0xc81,0xc9b,0xcb9,0xcd0,0xcd8,0xcf2,0xd10,0xd27,0xd2f,0xd49,0xd67,0xd7e,0xd86,0xda0,0xdbe,0xdd5,0xddd,0xdf7,0xe15,0xe2c,0xe34,0xe4e,0xe64,0xe73,0x10b6,0x1114,0x111c,0x112a,0x1168,0x1182,0x1192,0x11a2,0x11aa,0x11ba,0x11ca,0x11da,0x11ea,0x11fa,0x120a,0x125e,0x129d,0x130e,0x1337,0x13fc,0x1404,0x140c,0x1414,0x141c,0x1429,0x1431,0x143e,0x145f,0x147c,0x1489,0x14a3}},
    {0x17d040,0x516,"d106568d0284b5c0a4ac8b22b5a434511b64d4ef8f9e938bb59f31502c5cc57e",{0xea,0x1f4,0x4c6}},
    {0x7aa690,0x1ce,"cb4d687ac107ccdeaaf035d4eb5ba8fb2db2fb9390eeea3fbffe249ee17f979e",{0x10,0x32,0x42,0x62,0x6e,0x86,0x9b,0xbd,0xc3,0xca,0xd8,0x17d,0x192}},
    {0x4eed0,0xb2,"c548dcf0c04d7721b5405573032f372becaba23a092b4d6b0737d40b5a306c06",{0xc}},
    {0x282ed0,0x7f,"cabe4635cc1978234af7e5cf9a09883b5bef55adaca3067b3f6309e9bc1641b6",{0x34,0x53,0x6a,0x6f}},
    {0x3da040,0x22,"337aec1c8e81bb822f4ab60e710651cb415dbe1d83182e8a6acd844e96c621a6",{0xc}},
    {0x6a260,0x81,"2ea9cfc58344b98ac5208c36a8057e5dfbf96c5df2ba3c504b30e9b820eda7f0"},
    {0x215740,0x16,"bf7f0f96197c4b0361704d3fce0799ccfa71c78a2a0ebad1958b7823936a9a30"},
    {0x71a280,0x28,"c6f453fb5a56e4e80befb683133efc8683f7ffd8c7ae796c2e6db615d69aaa6b"},
    {0x17e710,0x12,"244ec7220ccaba52b750ad61d4a193b579cc6aeed44e48efe1055587ff004501"},
    {0x6032a0,0x38,"b84c64f622b31080c7f363b59044c10d7f6c70ae229cd12b9e7ef6a458da3bdd"},
    {0x759750,0x3f,"64a3af0e355e24fe95abaa940292b3ecd6389368c41af5a16f1fc500f690ff20"},
    {0x597bb0,0x6a,"02fa72fad77977e5077c99df177cb76f2a32a117163b97083439c8578a401100"},
    {0x7d4170,0x70,"2d68862641de75aaa82d5c06a6eb24808cc3810809af5dfd1283b549cfc7b9d7"},
    {0x1b3770,0x92,"984bbf0664c8811562e165b483003d4a025889c9986b65a242fc7f4a1bf824ff"},
    {0x489160,0x7c,"bd95c28db33899bc55d8717f953cd896efba3fa1d3a128336eb5ae8a4ea04db2"},
    {0x3bc10,0x66,"67a9edd89342b8ee6f57145cb4bc7e41d4eb944d559b1c324887336f39c0487c"},
    {0x2159f0,0xbcc,"d9b560ddca677043778cc7f8a0622d328c2a58d357241f993f1eeca64ba6c199",{0x17,0x28,0x3b,0x40,0x87,0x91,0xaa,0xc6,0xcd,0xe9,0x10a,0x111,0x12d,0x164,0x1cd,0x1e4,0x268,0x27a,0x2e4,0x352,0x3c1,0x3f0,0x412,0x45a,0x461,0x468,0x4f5,0x50c,0x535,0x597,0x59d,0x693,0x847,0x861,0x873,0x894,0x977,0x981,0x987,0x98f,0xa28,0xa41,0xa58,0xaac,0xae8,0xb42}},
    {0x76fe90,0x29,"03be50430aa952f47bafe6d80be8d9e20a62b276790751429b66be51ef25c2b6"},
    {0x25a3d0,0x17,"4c51edc908fccfba85f98c18765bb0f79a03b4de61189a5f98d045c6347fec8a"},
    {0x56fe00,0x61,"e38fb0dae037f5cf8da0f7f41afa0fb5ce4858fc97fd8e8084b916a5ebfbbf88",{0x5,0x12,0x1d}},
    {0x392680,0xa,"bfbb29b1ece682048a4618f44420011750ac1fbcdf45ff7cdafba437c801fff8",{0x5},BusyScope::SharedLockDependency},
    {0x648910,0xa,"452362d6bf0c1fefbb42bfb0db1c75c530b5a55fc8d74faf7484b38e660abcb0",{0x5},BusyScope::SharedLockDependency},
    {0x458590,0x782,"971249dffc61789e401718bdba1a46d7e15d28316018b7417f3f6f54caebe187",{0x11,0x25,0x2a,0x48,0x5b,0x6f,0x83,0x97,0x9e,0xad,0xb4,0xbf,0xcb,0xdc,0xe2,0xf1,0xf8,0x112,0x118,0x127,0x14e,0x153,0x1b9,0x1cd,0x1d2,0x1d7,0x1dd,0x220,0x229,0x238,0x241,0x259,0x2fe,0x347,0x354,0x35a,0x36d,0x37e,0x38f,0x3a0,0x3b1,0x3c2,0x3d3,0x3e4,0x3f5,0x406,0x417,0x428,0x439,0x44a,0x45b,0x46c,0x47d,0x48e,0x49f,0x4b0,0x4fd,0x50a,0x510,0x523,0x534,0x545,0x556,0x567,0x578,0x589,0x59a,0x5ab,0x5bc,0x5cd,0x5de,0x5ef,0x63c,0x649,0x64f,0x662,0x677,0x68a,0x69d,0x6ae,0x6b3,0x6b8,0x6bd,0x6c2,0x6c7,0x6e5,0x6ea,0x70b,0x71b,0x72d,0x744}},
    {0x3c0340,0xb15,"8df2b27e806046fe32aab7ebde22c6958de26343cb7c6bce8c1aa4854f60a522",{0x3c,0x1fc,0x34f,0x3c8,0x45c,0x489,0x4b8,0x4ef,0x523,0x5cd,0x5df,0x5f1,0x658,0x670,0x685,0x69a,0x6b8,0x6bf,0x6dd,0x708,0x72a,0x733,0x74a,0x768,0x90a,0x91c,0x946,0x976,0x99b}},
    {0x5e6ea0,0x234,"52c172fa43784e41c0228a14c7f3011c93f40f6a7c512ba3f343d0965aa3c804",{0x36,0x50,0x5c,0x88,0xab,0xcf,0xd9,0xdf,0xe7,0xf4,0x139}},
    {0x6d3490,0x18e6,"2ed380d347b5ffb6b3ee42f4c74e07e96b89fdbbd410cf4021d64104b7171b37",{0x1d,0xfd,0x11f,0x139,0x147,0x14c,0x16b,0x1bc,0x1c9,0x1d4,0x1de,0x1ea,0x1f5,0x1ff,0x20d,0x212,0x233,0x271,0x27b,0x281,0x28f,0x294,0x29c,0x2a5,0x2a9,0x2b7,0x2bc,0x2c7,0x2d1,0x2e2,0x2e7,0x302,0x310,0x315,0x327,0x335,0x33a,0x35c,0x387,0x395,0x39a,0x3c4,0x3d1,0x3e2,0x3e7,0x403,0x41e,0x423,0x43e,0x44a,0x454,0x45e,0x46c,0x471,0x479,0x48a,0x48f,0x4a9,0x4b7,0x4bc,0x4cf,0x4d9,0x50b,0x51f,0x533,0x549,0x557,0x55c,0x597,0x5b6,0x5de,0x5ee,0x61f,0x629,0x63e,0x649,0x64e,0x664,0x66e,0x684,0x69d,0x6b8,0x6c6,0x6cb,0x6f4,0x702,0x707,0x718,0x726,0x72b,0x73d,0x74b,0x750,0x76a,0x778,0x77d,0x78e,0x79c,0x7a1,0x7bb,0x7c9,0x7ce,0x7e3,0x7e8,0x7f1,0x816,0x81b,0x824,0x82b,0x847,0x87d,0x888,0x88d,0x8c7,0x8ce,0x909,0x913,0x91d,0x928,0x92d,0x952,0x95f,0x974,0x981,0x98b,0x9b6,0x9c0,0x9df,0x9ed,0x9f2,0x9fd,0xa04,0xa0c,0xa21,0xa2f,0xa40,0xa50,0xa72,0xa7f,0xa8d,0xa9b,0xaa0,0xab1,0xabf,0xac4,0xacc,0xadd,0xae2,0xaf9,0xb06,0xb30,0xb47,0xb54,0xb7e,0xb98,0xba1,0xbaf,0xbb4,0xbea,0xbf8,0xbfd,0xc0e,0xc1c,0xc21,0xc39,0xc50,0xc60,0xc71,0xc76,0xc82,0xc8c,0xc9d,0xca2,0xcb7,0xcd5,0xcf6,0xd04,0xd09,0xd1e,0xd46,0xd66,0xd74,0xd79,0xd8a,0xd9b,0xdac,0xdbd,0xdce,0xddf,0xdf0,0xe01,0xe12,0xe23,0xe34,0xe45,0xe50,0xe5e,0xe63,0xeba,0xec8,0xecd,0xf04,0xf12,0xf17,0xf5d,0xf6b,0xf70,0xfb6,0xfc4,0xfc9,0x100f,0x101d,0x1022,0x1060,0x106b,0x1070,0x10df,0x10ed,0x10f2,0x1130,0x113b,0x1140,0x118b,0x11b9,0x11c4,0x11c9,0x11df,0x11e4,0x1219,0x121e,0x1245,0x124a,0x124f,0x1258,0x1289,0x12f9,0x1308,0x130f,0x1314,0x1322,0x1327,0x133a,0x133f,0x134e,0x1353,0x1358,0x1361,0x138f,0x1394,0x1399,0x13a2,0x13d0,0x13df,0x13f2,0x1465,0x148d,0x1492,0x14c4,0x14c9,0x14d1,0x14e2,0x14e7,0x1516,0x152f,0x1567,0x1574,0x1585,0x15a3,0x15b7,0x15c5,0x15d3,0x15e1,0x15e6,0x1696,0x169b,0x16a1,0x16ab,0x16d2,0x16e3,0x16ef,0x1736,0x1740,0x1762,0x176c,0x179c,0x17ab,0x17b5,0x17ce,0x17d8,0x17e2,0x1812,0x1828,0x1855,0x1864,0x1869,0x1884,0x188b,0x1890,0x189e,0x18a3,0x18ac,0x18be,0x18d3}},
    {0x4a920,1,"ae3f4619b0413d70d3004b9131c3752153074e45725be13b9a148978895e359e"}
};
std::atomic<bool> ready{},world{},stopped{},eyeReplay{};
// The eye pair replays on the render thread; the game's own selection passes run
// elsewhere. Replay suppression applies only to the replaying thread: on another
// thread, refusing the floor lets native low LOD through mid-pair instead.
std::atomic<DWORD> replayThread{};
bool ReplayingHere() noexcept {
    return eyeReplay.load(std::memory_order_acquire) && replayThread.load(std::memory_order_acquire)==GetCurrentThreadId();
}
std::atomic<bool> singleAttemptUsed{},controlledDormant{};
std::atomic<uint32_t> activeWrappers{};
static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);
std::atomic<uintptr_t> attachedManager{},attachedUpdater{};
std::atomic<uint32_t> callbackA{},callbackB{},updaterPasses{},originalRoutes{},privateRoutes{},
    batches{},floors{},refusals{},compareFailures{};
std::atomic<const char*> floorReason{"not_entered"};
std::atomic<uint32_t> policyInspected{},policyNearby{},policyLastRefusal{};
std::array<std::atomic<uint32_t>,9> policyResults{};
static_assert(static_cast<std::size_t>(Result::Count)==9);
std::array<std::atomic<uint32_t>,3> privateByRoute{},selectionPassesByRoute{};
enum class Drift {None,Motion,Selection,Structure};
enum class FloorRefusal : std::size_t {
    Permission,Adapter,Diagnostic,Inactive,Route,Capture,Owner,Batch,Policy,
    FreshCapture,FreshGate,FreshSelection,FreshStructure,Count
};
static_assert(static_cast<std::size_t>(FloorRefusal::Count)==RuntimeReport{}.floorRefusals.size());
std::array<std::atomic<uint32_t>,static_cast<std::size_t>(FloorRefusal::Count)> floorRefusals{};
std::atomic<uint32_t> motionRefreshes{},retainedPasses{};
// Native-route reasons: eye replay, nested wrapper, inactive world, owner/caller.
std::array<std::atomic<uint32_t>,4> originalByReason{};
// Floors written although ready / world was transiently clear or another
// thread was replaying the eye pair at the confirming capture.
std::array<std::atomic<uint32_t>,3> transientGates{};
uintptr_t image{},cache{},table{};
uint32_t peImageBase{};
native::StageResult staged;
TransactionFn transact{};
DWORD controller{};
bool requested{},prepared{},started{};
bool restoreStarted{},cleanupNotice{};
unsigned attempts{},totalAttempts{},attachments{},restorations{},ticks{},nextTry{},receipts{};
unsigned attachmentAttempts{},restoreAttempts{};
uintptr_t retryManager{},retryUpdater{};
std::filesystem::path output;
char lastReason[192]="not_prepared";
std::vector<BusyRange> busy;
struct Graph {
    uintptr_t manager{},updater{},parent{},callback{},vptr{},records{};
    int32_t count{};
    bool operator==(const Graph&) const=default;
};
thread_local unsigned wrapperDepth{};
thread_local bool permission{};
thread_local unsigned wrapperRoute{};
thread_local Graph wrapperOwner{};
uint32_t __cdecl ModCallback() noexcept;
uint32_t __fastcall ModUpdate(void*,void*,uint32_t) noexcept;
void __fastcall FloorOnly(void*,void*,const void*,uint32_t,const void*,uint32_t) noexcept;
#ifdef GRID2VR_OPPONENT_LOD_RUNTIME_TEST
testing::NativeBoundary testBoundary{};
thread_local uintptr_t testReturn{};
void (*testBetweenCaptures)() noexcept{};
#endif

bool Extent(uintptr_t at,std::size_t bytes) noexcept {
    return at>=0x10000 && bytes && bytes<=std::numeric_limits<uintptr_t>::max()-at;
}
bool Region(uintptr_t at,std::size_t bytes,bool privateWrite=false) noexcept {
    if(!Extent(at,bytes))return false;
    const auto end=at+bytes;
    while(at<end) {
        MEMORY_BASIC_INFORMATION m{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&m,sizeof(m)) || m.State!=MEM_COMMIT ||
            (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        const DWORD protection=m.Protect&0xff;
        if(privateWrite && (m.Type!=MEM_PRIVATE || protection!=PAGE_READWRITE))return false;
        if(protection!=PAGE_READONLY && protection!=PAGE_READWRITE && protection!=PAGE_WRITECOPY &&
           protection!=PAGE_EXECUTE_READ && protection!=PAGE_EXECUTE_READWRITE && protection!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto next=reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize;
        if(next<=at)return false;
        at=std::min(end,static_cast<uintptr_t>(next));
    }
    return true;
}
bool Read(uintptr_t at,void* value,std::size_t bytes) noexcept {
    SIZE_T got{};
    return Region(at,bytes) && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),value,bytes,&got) && got==bytes;
}
template<class T> bool Read(uintptr_t at,T& value) noexcept {return Read(at,&value,sizeof(value));}
bool Heap(uintptr_t at,std::size_t bytes) noexcept {return !(at&3) && Region(at,bytes,true);}
bool GraphNow(Graph& g) noexcept {
    uintptr_t vptr{},reciprocal{};
    return Read(image+OwnerManager,g.manager) && Heap(g.manager,ManagerSize) &&
        Read(g.manager,vptr) && vptr==image+ManagerTable &&
        Read(g.manager+0x1104,g.records) && g.records==g.manager+0x1110 &&
        Read(g.manager+0x13260,g.count) && g.count>=0 && g.count<=12 &&
        Read(g.manager+0x13488,g.callback) && Read(image+UpdaterGlobal,g.updater) &&
        Heap(g.updater,0x5c) && Read(g.updater,g.vptr) &&
        (g.vptr==image+UpdaterTable || (table && g.vptr==table)) &&
        Read(g.updater+0xc,g.parent) && Heap(g.parent,0xd0) &&
        Read(g.parent+0xcc,reciprocal) && reciprocal==g.updater;
}
bool Owned(const Graph& g) noexcept {
    return g.manager==attachedManager.load(std::memory_order_acquire) &&
        g.updater==attachedUpdater.load(std::memory_order_acquire) &&
        g.callback==reinterpret_cast<uintptr_t>(&ModCallback) && g.vptr==table;
}
struct VerifiedBytes {uintptr_t address;std::vector<uint8_t> bytes;};
std::vector<VerifiedBytes> verified;
// Never allocate/hash/log/lock while peers are suspended. Compare the already
// verified original bytes with bounded stack scratch instead of hashing again.
bool OriginalsUnchanged() noexcept {
    uint8_t scratch[4096];
    for(const auto& guard:verified) {
        for(std::size_t i=0;i<guard.bytes.size();i+=sizeof(scratch)) {
            const auto n=std::min(sizeof(scratch),guard.bytes.size()-i);
            if(!Read(guard.address+i,scratch,n))return false;
            for(std::size_t j=0;j<n;++j)if(scratch[j]!=guard.bytes[i+j])return false;
        }
    }
    return true;
}
void Reason(const char* reason) noexcept {
    std::size_t i{};
    if(!reason)reason="ok";
    for(;i+1<sizeof(lastReason) && reason[i];++i)lastReason[i]=reason[i];
    lastReason[i]=0;
}
void JsonString(std::ostream& log,const char* value) {
    log<<'"';
    for(;*value;++value) {
        const auto c=static_cast<unsigned char>(*value);
        if(c=='"' || c=='\\')log<<'\\'<<static_cast<char>(c);
        else if(c<32 || c>=127) {
            constexpr char hex[]="0123456789abcdef";
            log<<"\\u00"<<hex[c>>4]<<hex[c&15];
        } else log<<static_cast<char>(c);
    }
    log<<'"';
}
void EmitStatus(std::ostream& log,const RuntimeReport& r) {
    log<<std::boolalpha<<"{\"buildMode\":";JsonString(log,mode::BuildMode);
    log<<",\"reason\":";JsonString(log,r.reason);
    log<<",\"requested\":"<<r.requested<<",\"prepared\":"<<r.prepared
       <<",\"ready\":"<<r.ready<<",\"worldReady\":"<<r.worldReady<<",\"stopped\":"<<r.stopped
       <<",\"eyeReplay\":"<<r.eyeReplay<<",\"cleanupNotice\":"<<r.cleanupNotice
       <<",\"controllerTicks\":"<<r.controllerTicks<<",\"activeWrappers\":"<<r.activeWrappers
       <<",\"attempts\":"<<r.attempts<<",\"attachments\":"<<r.attachments<<",\"restorations\":"<<r.restorations
       <<",\"callbackA\":"<<r.callbackA<<",\"callbackB\":"<<r.callbackB<<",\"updater\":"<<r.updater
       <<",\"privateRoutes\":"<<r.privateRoutes<<",\"originalRoutes\":"<<r.originalRoutes
       <<",\"batches\":"<<r.batches<<",\"floors\":"<<r.floors<<",\"refusals\":"<<r.refusals
       <<",\"compareFailures\":"<<r.compareFailures<<",\"floorReason\":";JsonString(log,r.floorReason);
    log<<",\"privateByRoute\":["<<r.privateByRoute[0]<<','<<r.privateByRoute[1]<<','<<r.privateByRoute[2]<<']'
       <<",\"selectionPassesByRoute\":["<<r.selectionPassesByRoute[0]<<','<<r.selectionPassesByRoute[1]<<','<<r.selectionPassesByRoute[2]<<']'
       <<",\"cache\":"<<r.cache<<",\"adapter\":"<<r.adapter<<",\"manager\":"<<r.manager
       <<",\"updaterObject\":"<<r.updaterObject
       <<",\"imageBase\":"<<r.imageBase<<",\"peImageBase\":"<<r.peImageBase
       <<",\"singleAttemptDiagnostic\":"<<r.singleAttemptDiagnostic<<",\"diagnosticComplete\":"<<r.diagnosticComplete
       <<",\"controlledActivation\":"<<r.controlledActivation<<",\"attachmentAttempted\":"<<r.attachmentAttempted
       <<",\"controlledDormant\":"<<r.controlledDormant<<",\"attachmentAttempts\":"<<r.attachmentAttempts
       <<",\"restoreAttempts\":"<<r.restoreAttempts
       <<",\"integrityBodyCount\":"<<r.integrityBodyCount<<",\"busyRangeCount\":"<<r.busyRangeCount
       <<",\"updaterMethodCount\":"<<r.updaterMethodCount
       <<",\"policyInspected\":"<<r.policyInspected<<",\"policyNearby\":"<<r.policyNearby
       <<",\"policyLastRefusal\":"<<r.policyLastRefusal<<",\"policyResults\":{";
    constexpr const char* names[]{"planned","nativeBestOrBetter","distant","excluded","invalidIdentity",
        "invalidBounds","invalidBestMesh","unsupportedSelection","invalidDistance"};
    for(std::size_t i=0;i<r.policyResults.size();++i) {
        if(i)log<<',';JsonString(log,names[i]);log<<':'<<r.policyResults[i];
    }
    constexpr const char* refusalNames[]{"permission","adapter","diagnostic","inactive","route","capture","owner",
        "batch","policy","freshCapture","freshGate","freshSelection","freshStructure"};
    static_assert(std::size(refusalNames)==RuntimeReport{}.floorRefusals.size());
    log<<"},\"floorRefusals\":{";
    for(std::size_t i=0;i<r.floorRefusals.size();++i) {
        if(i)log<<',';JsonString(log,refusalNames[i]);log<<':'<<r.floorRefusals[i];
    }
    log<<"},\"motionRefreshes\":"<<r.motionRefreshes<<",\"retainedPasses\":"<<r.retainedPasses
       <<",\"originalByReason\":{\"eyeReplay\":"<<r.originalByReason[0]<<",\"nested\":"<<r.originalByReason[1]
       <<",\"inactive\":"<<r.originalByReason[2]<<",\"ownerOrCaller\":"<<r.originalByReason[3]<<'}'
       <<",\"transientGates\":{\"notReady\":"<<r.transientGates[0]<<",\"noWorld\":"<<r.transientGates[1]
       <<",\"eyeReplayElsewhere\":"<<r.transientGates[2]<<'}'
       <<",\"nativeExecutionAcceptance\":\"awaits_manual\",\"hysteresis\":false}\n";
}
void Receipt(bool event=true) noexcept {
    if(output.empty())return;
    try {
        const auto report=Report();
        // Bounded overwrite remains current after the optional 64-event cap.
        std::ofstream current(output/L"opponent-lod-runtime.json",std::ios::trunc);
        current.imbue(std::locale::classic());
        EmitStatus(current,report);
        if(event && receipts<64) {
            // Start a bounded per-process event file, not unbounded restart growth.
            const auto mode=receipts?std::ios::app:std::ios::trunc;
            ++receipts;
            std::ofstream log(output/L"opponent-lod-runtime.jsonl",mode);
            log.imbue(std::locale::classic());
            EmitStatus(log,report);
        }
    }catch(...) {}
}
struct StatusOnExit {
    bool emit{},event{};
    ~StatusOnExit() noexcept {if(emit)Receipt(event);}
};
uint32_t U32(const uint8_t* bytes) noexcept {uint32_t value{};std::memcpy(&value,bytes,4);return value;}
bool Remember(uintptr_t address,std::size_t size) {
    VerifiedBytes v{address,std::vector<uint8_t>(size)};
    if(!Read(address,v.bytes.data(),size))return false;
    verified.push_back(std::move(v));return true;
}
bool Qualify() {
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS32 pe{};
    if(!Read(image,dos)) {Reason("pe_dos_read_guard");return false;}
    if(dos.e_magic!=IMAGE_DOS_SIGNATURE) {Reason("pe_dos_magic_guard");return false;}
    if(dos.e_lfanew<0 || dos.e_lfanew>0x1000) {Reason("pe_nt_offset_guard");return false;}
    if(!Read(image+static_cast<uint32_t>(dos.e_lfanew),pe)) {Reason("pe_nt_read_guard");return false;}
    peImageBase=pe.OptionalHeader.ImageBase;
    if(pe.Signature!=IMAGE_NT_SIGNATURE) {Reason("pe_nt_signature_guard");return false;}
    if(pe.FileHeader.Machine!=IMAGE_FILE_MACHINE_I386) {Reason("pe_machine_guard");return false;}
    if(pe.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC) {Reason("pe_optional_magic_guard");return false;}
    // Windows SEC_IMAGE rebases this header field too. The original file value
    // and this exact loaded base are the only admitted forms, not arbitrary VAs.
    // Body normalization remains relative to the PINNED file preferred base.
    if(peImageBase!=native::PreferredImageBase && peImageBase!=image) {Reason("pe_image_base_guard");return false;}
    if(pe.OptionalHeader.SizeOfImage!=ImageSize) {Reason("pe_image_size_guard");return false;}
    if(pe.OptionalHeader.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_BASERELOC) {Reason("pe_directory_count_guard");return false;}
    const auto directory=pe.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if(directory.VirtualAddress!=0xf47000 || directory.Size!=0x95478) {Reason("pe_relocation_directory_shape");return false;}
    verified.clear();
    for(const auto& guard:auxiliary) {
        if(!Remember(image+guard.rva,guard.size)) {Reason("auxiliary_unreadable");return false;}
        auto normalized=verified.back().bytes;
        for(const auto offset:guard.highlow) {
            if(guard.size<4 || offset>guard.size-4)return false;
            const auto operand=U32(normalized.data()+offset)-static_cast<uint32_t>(image-native::PreferredImageBase);
            std::memcpy(normalized.data()+offset,&operand,4);
        }
        const auto digest=gameexe::Digest(normalized.data(),guard.size);
        if(digest!=guard.hash) {
            char reason[64]{};sprintf_s(reason,"auxiliary_body_hash_%08x",guard.rva);Reason(reason);return false;
        }
    }
    // Preserve the COMPLETE eleven-method subsystem interface, not just the
    // update/destructor/no-op prefix. Loading invokes slot 5 before slot 1.
    static_assert(UpdaterTable==updatertable::TableRva);
    static_assert(UpdateRva==updatertable::NativeRvas[updatertable::PrefixWords+updatertable::UpdateSlot]);
    updatertable::Block nativeTable{};
    if(!Read(image+UpdaterTable-4,nativeTable) || !updatertable::MatchesNative(nativeTable,image)) {
        Reason("native_updater_table_guard");return false;
    }
    if(!Remember(image+UpdaterTable-4,sizeof(nativeTable)))return false;
    uintptr_t ownerCreate{},ownerDestroy{};
    if(!Read(image+0xcf7954,ownerCreate) || ownerCreate!=image+0x7aa690 ||
       !Read(image+0xcf7970,ownerDestroy) || ownerDestroy!=image+0x4eed0 ||
       !Remember(image+0xcf7954,4) || !Remember(image+0xcf7970,4)) {
        Reason("native_owner_lifecycle_table_guard");return false;
    }
    native::BodyInputs inputs{};
    const auto guards=native::Guards();
    for(std::size_t i=0;i<guards.size();++i) {
        if(!Remember(image+guards[i].rva,guards[i].size))return false;
    }
    // verified was grown completely before taking any spans into its elements.
    for(std::size_t i=0;i<guards.size();++i)inputs[i]=verified[verified.size()-guards.size()+i].bytes;
    auto allocation=static_cast<uint8_t*>(VirtualAlloc(nullptr,native::AllocationBytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!allocation)return false;
    const auto address=reinterpret_cast<uintptr_t>(allocation);
    const auto result=native::StageCopies(inputs,static_cast<uint32_t>(image),static_cast<uint32_t>(address),
        reinterpret_cast<uint32_t>(&FloorOnly),{allocation,native::AllocationBytes});
    if(!result) {
        char reason[64]{};sprintf_s(reason,"native_stage_refusal_%u",static_cast<unsigned>(result.failure));Reason(reason);
        VirtualFree(allocation,0,MEM_RELEASE);return false;
    }
    constexpr std::size_t tableOffset=(native::StagingBytes+15)&~std::size_t(15);
    static_assert(tableOffset+sizeof(nativeTable)<=native::AllocationBytes);
    const auto privateTable=updatertable::PrivateCopy(nativeTable,reinterpret_cast<uintptr_t>(&ModUpdate));
    std::memcpy(allocation+tableOffset,privateTable.data(),sizeof(privateTable));
    DWORD previous{};
    if(!VirtualProtect(allocation,native::AllocationBytes,PAGE_EXECUTE_READ,&previous) ||
       !FlushInstructionCache(GetCurrentProcess(),allocation,native::AllocationBytes)) {
        VirtualFree(allocation,0,MEM_RELEASE);return false;
    }
    cache=address;table=cache+tableOffset+4;staged=result;
    busy.clear();busy.reserve(std::size(auxiliary)+native::BodyCount+1);
    for(const auto& guard:auxiliary) {
        if(guard.scope==BusyScope::SharedLockDependency) {
            // These two complete, still-hashed API wrappers do not own LOD
            // state. Every call from an audited LOD body retains an in-range
            // caller return; there are no tail transfers into these wrappers.
            // Full peer scans therefore still reject parked LOD callers while
            // unrelated users of the generic wrappers no longer block attach.
            if(guard.size!=0xa || (guard.rva!=0x392680 && guard.rva!=0x648910)) {
                Reason("invalid_shared_lock_dependency_scope");return false;
            }
            // Only explicit diagnostic/controlled candidates use the audited
            // scoped list. Ordinary write-capable builds remain conservative.
            if constexpr(mode::ScopedOwnership)continue;
        }
        busy.push_back({image+guard.rva,image+guard.rva+guard.size});
    }
    for(const auto& guard:guards)busy.push_back({image+guard.rva,image+guard.rva+guard.size});
    busy.push_back({cache,cache+native::AllocationBytes});
    HMODULE module{};
    // Pin only OUR helper module: an owned native slot can survive Stop refusal
    // or owner replacement. RX cache plus C++ thunks must both outlive that slot.
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&FloorOnly),&module))return false;
    // Do not block unrelated inputSampler/OpenXR stacks in this same module.
    // Active wrappers are instead counted before any eligibility/graph/native
    // work, through the root return. All owned-route/RX ranges remain busy;
    // the two shared lock dependencies remain byte-guarded, not global owners.
    return true;
}

struct TransactionContext {
    Graph graph;
    std::array<PointerChange,2> changes{};
    std::size_t count{};
    bool stopping{},published{};
};
bool ValidateTransaction(void* context) noexcept {
    const auto& c=*static_cast<TransactionContext*>(context);
    if(activeWrappers.load(std::memory_order_seq_cst))return false;
    Graph now{};
    if(!GraphNow(now) || now!=c.graph || !OriginalsUnchanged())return false;
    if(!c.stopping && (!world.load(std::memory_order_acquire) || stopped.load(std::memory_order_acquire)))return false;
    for(std::size_t i=0;i<c.count;++i) {
        uintptr_t value{};const auto& change=c.changes[i];
        if(!Heap(reinterpret_cast<uintptr_t>(change.slot),4) ||
           !Read(reinterpret_cast<uintptr_t>(change.slot),value) || value!=reinterpret_cast<uintptr_t>(change.expected))return false;
    }
    return true;
}
void CommitTransaction(void* context) noexcept {
    auto& c=*static_cast<TransactionContext*>(context);
    c.published=true;
    if(c.stopping) {
        ready.store(false,std::memory_order_release);
        attachedManager.store(0,std::memory_order_release);attachedUpdater.store(0,std::memory_order_release);
    } else {
        attachedManager.store(c.graph.manager,std::memory_order_release);
        attachedUpdater.store(c.graph.updater,std::memory_order_release);
        // Both expected writes have completed, while every peer is still paused.
        ready.store(true,std::memory_order_release);
    }
}
void Attempt(Graph graph,bool stopping) noexcept {
    TransactionContext c{};c.graph=graph;c.stopping=stopping;
    const auto add=[&](uintptr_t slot,uintptr_t expected,uintptr_t replacement) {
        c.changes[c.count++]={reinterpret_cast<void**>(slot),reinterpret_cast<void*>(expected),reinterpret_cast<void*>(replacement)};
    };
    if(stopping) {
        if(graph.manager!=attachedManager.load() || graph.updater!=attachedUpdater.load()) {
            Reason("stop_graph_replaced_no_stale_restore");return;
        }
        if(graph.callback==reinterpret_cast<uintptr_t>(&ModCallback))add(graph.manager+0x13488,graph.callback,image+CallbackRva);
        if(graph.vptr==table)add(graph.updater,graph.vptr,image+UpdaterTable);
        if(!c.count) {
            // Matching live graph contains no owned fields. No stale/foreign
            // write is needed; storage still remains pinned for process lifetime.
            attachedManager.store(0,std::memory_order_release);attachedUpdater.store(0,std::memory_order_release);
            Reason("stop_no_owned_live_slots_foreign_fields_preserved");return;
        }
    } else {
        if(graph.callback!=image+CallbackRva || graph.vptr!=image+UpdaterTable) {
            Reason("foreign_or_partial_routes_preserved");return;
        }
        add(graph.manager+0x13488,image+CallbackRva,reinterpret_cast<uintptr_t>(&ModCallback));
        add(graph.updater,image+UpdaterTable,table);
    }
    if constexpr(mode::SingleAttachment) {
        if(!stopping && singleAttemptUsed.exchange(true,std::memory_order_acq_rel))return;
    }
    ++attempts;++totalAttempts;
    if(stopping)++restoreAttempts;else ++attachmentAttempts;
    const char* notice=transact({c.changes.data(),c.count},busy,ValidateTransaction,&c,CommitTransaction);
    if constexpr(SingleAttemptDiagnostic) {if(!stopping) {
        // A diagnostic must never enable floor work, even if a broken boundary
        // unexpectedly publishes ownership. Retain its ledger/resident storage.
        ready.store(false,std::memory_order_release);world.store(false,std::memory_order_release);
        if(!c.published && notice && !std::strcmp(notice,"diagnostic_probe_safe_no_writes")) {
            Reason(notice);Receipt();return;
        }
    }}
    if(c.published) {
        if(stopping) {++restorations;Reason("stopped_owned_live_slots_restored");}
        else {++attachments;Reason("candidate_ready_manual_native_acceptance_pending");}
    }
    if(notice || !c.published) {
        Reason(notice?notice:"transaction_returned_without_commit");++refusals;
        if constexpr(ControlledActivation) {if(!stopping) {
            controlledDormant.store(true,std::memory_order_release);
            ready.store(false,std::memory_order_release);world.store(false,std::memory_order_release);
        }}
        if(c.published || !notice) {
            // Cleanup failure does not undo authoritative publication. Keep its
            // ledger/cache, disable floors permanently, and allow owned restore.
            cleanupNotice=true;
            stopped.store(true,std::memory_order_release);ready.store(false,std::memory_order_release);
            world.store(false,std::memory_order_release);
        }
    }
    Receipt();
}

// The complete x87 stack/control/status and all XMM/MXCSR bytes are restored,
// including on every refusal. Mask exceptions only inside our saved scope.
struct alignas(16) FloatingState {
    unsigned char bytes[512];
    FloatingState() noexcept {
        _fxsave(bytes);
        const uint32_t safe=0x1f80;
        __asm {fninit}
        __asm {ldmxcsr safe}
    }
    ~FloatingState() noexcept {_fxrstor(bytes);}
};
uint32_t NativeCall(bool copied,bool update,void* self,uint32_t argument) noexcept {
#ifdef GRID2VR_OPPONENT_LOD_RUNTIME_TEST
    // Never execute target code, even if a fixture forgot to supply a boundary.
    return testBoundary?testBoundary(copied,update,self,argument):0;
#else
    const auto entry=copied?staged.copies[update?4:0]:static_cast<uint32_t>(image+(update?UpdateRva:CallbackRva));
    if(update)return reinterpret_cast<uint32_t (__thiscall*)(void*,uint32_t)>(entry)(self,argument);
    return reinterpret_cast<uint32_t (__cdecl*)()>(entry)();
#endif
}
struct ActiveWrapper {
    ActiveWrapper() noexcept {activeWrappers.fetch_add(1,std::memory_order_seq_cst);}
    ~ActiveWrapper() noexcept {activeWrappers.fetch_sub(1,std::memory_order_seq_cst);}
};
uint32_t Route(bool update,void* self,uint32_t argument,uintptr_t caller,uintptr_t dispatcher=0) noexcept {
    // The pre-counter entry window reads neither eligibility nor graph. Callback
    // entries also retain guarded native dispatch returns; updater self is
    // checked after acquisition. Neither code nor tables are ever retired.
    ActiveWrapper active;
    bool allowed{},replaying{},live{};
    Graph owner{};
    {
        FloatingState fp;
        replaying=ReplayingHere();
        live=ready.load(std::memory_order_acquire) && world.load(std::memory_order_acquire) && !stopped.load(std::memory_order_acquire);
        const bool eligible=!SingleAttemptDiagnostic && live && !replaying && !wrapperDepth;
        allowed=eligible && GraphNow(owner) && Owned(owner) &&
            (update?owner.updater==reinterpret_cast<uintptr_t>(self):
                dispatcher==owner.manager && (caller==image+0x1b37d5 || caller==image+0x4891af));
    }
    if(update)++updaterPasses;
    else if(caller==image+0x1b37d5)++callbackA;
    else if(caller==image+0x4891af)++callbackB;
    const unsigned route=update?3u:caller==image+0x1b37d5?1u:2u;
    if(allowed) {++privateRoutes;++privateByRoute[route-1];}
    else {
        ++originalRoutes;
        ++originalByReason[replaying?0:wrapperDepth?1:!live?2:3];
    }
    const bool previous=permission;const auto previousRoute=wrapperRoute;const auto previousOwner=wrapperOwner;
    permission=allowed;wrapperRoute=allowed?route:0;wrapperOwner=allowed?owner:Graph{};++wrapperDepth;
    const auto result=NativeCall(allowed,update,self,argument);
    --wrapperDepth;permission=previous;wrapperRoute=previousRoute;wrapperOwner=previousOwner;
    return result;
}
__declspec(noinline) uint32_t __cdecl CallbackRoute(uintptr_t caller,uintptr_t dispatcher) noexcept {
    return Route(false,nullptr,0,caller,dispatcher);
}
// Both fully guarded native dispatch bodies retain this-manager in ESI. Capture
// it BEFORE a compiler prologue can use that register. The original callback
// ignores ECX, so no fictitious thiscall argument is introduced. This also stops
// an old still-owned callback from borrowing a newly published manager's route.
__declspec(naked) uint32_t __cdecl ModCallback() noexcept {
    __asm {
        mov eax,dword ptr [esp]
        push esi
        push eax
        call CallbackRoute
        add esp,8
        ret
    }
}
__declspec(noinline) uint32_t __fastcall ModUpdate(void* self,void*,uint32_t argument) noexcept {
    return Route(true,self,argument,0);
}

struct Slot {
    uintptr_t car{},item{},alternate{},mesh{};
    uint8_t local{};
    int32_t selected{},best{},worst{};
    std::array<float,3> position{};
    bool operator==(const Slot&) const=default;
};
struct Snapshot {
    Graph graph{};
    uintptr_t views{},list{},begin{},end{};
    int32_t profile{},forced{},targetIndex{},listCount{};
    std::array<float,3> camera{};
    std::array<Slot,MaxCars> slots{};
    bool operator==(const Snapshot&) const=default;
};
const char* Capture(Snapshot& s,uintptr_t self,uintptr_t views,uint32_t count,uintptr_t list,uint32_t flag) noexcept {
    if(count!=1)return "snapshot_view_count";
    if(!Region(views,0x60))return "snapshot_view_memory";
    if(!Heap(list,0xb0))return "snapshot_list_object";
    if(!GraphNow(s.graph) || !Owned(s.graph) || self!=s.graph.manager)return "snapshot_owner_graph";
    if(s.graph.count<1)return "snapshot_no_cars";
    s.views=views;s.list=list;
    if(!Read(views+0x40,s.camera))return "snapshot_camera_memory";
    if(!Read(self+0x1346c,s.profile) || s.profile<0 || s.profile>2)return "snapshot_profile";
    if(!Read(self+0x13350,s.forced) || s.forced!=-1)return "snapshot_forced_selection";
    if(!Read(self+0x134d4,s.targetIndex))return "snapshot_target_index";
    if(!Read(list+0xa8,s.begin) || !Read(list+0xac,s.end) || s.end<s.begin ||
         (s.end-s.begin)%4 || (s.end-s.begin)/4<static_cast<uintptr_t>(s.graph.count) || (s.end-s.begin)/4>MaxCars ||
         !Heap(s.begin,s.end-s.begin))return "snapshot_list_extent";
    if(!Read(self+0x134a0,s.listCount) || s.listCount!=static_cast<int32_t>((s.end-s.begin)/4))return "snapshot_list_count";
    for(std::size_t i=0;i<MaxCars;++i) {
        auto& slot=s.slots[i];const auto record=s.graph.records+i*RecordStride;
        // Native current/player getters scan twelve slots, even above count.
        if(!Read(record+0xdc,slot.car) || !Read(record+0xc6,slot.local))return "snapshot_record_identity";
        if(slot.car && !Heap(slot.car,0x130))return "snapshot_car_object";
        if(i>=static_cast<std::size_t>(s.graph.count) || !slot.car)continue;
        if(!Read(record+0xfc,slot.selected))return "snapshot_selected_value";
        if(!Read(slot.car+0x124,slot.best) || !Read(slot.car+0x120,slot.worst))return "snapshot_lod_bounds";
        if(!Read(s.begin+i*4,slot.item) || !Heap(slot.item,0x34) || !Read(slot.item+0x30,slot.alternate))return "snapshot_list_item";
        // EXACT native indexing: list.a8 is an array of 32-bit item pointers;
        // index is the record slot, item+30 is a pointer (not an inline matrix).
        // Selector tests only the LOW BYTE of flag, then nonnull alternate.
        const bool alternate=(flag&0xff)!=0 && slot.alternate;
        if(alternate && !Heap(slot.alternate,0x340))return "snapshot_alternate_object";
        if(!Read(alternate?slot.alternate+0x330:slot.car+0x60,slot.position))return "snapshot_car_position";
        if(slot.best>=0 && slot.best<=4 && slot.worst>=slot.best && slot.worst<=4) {
            if(!Read(slot.car+0xe0+static_cast<uintptr_t>(slot.best)*4,slot.mesh))return "snapshot_mesh_pointer";
            // Mesh-switch dereferences flags at +d8. Never force asset loading;
            // a null/nonprivate/uncommitted best mesh is not an eligible asset.
            if(slot.mesh && !Heap(slot.mesh,0xdc))return "snapshot_mesh_object";
        }
    }
    return nullptr;
}
Position PositionOf(const std::array<float,3>& xyz) noexcept {return {xyz[0],xyz[1],xyz[2]};}
// Motion is the camera and car positions, which other game threads move
// continuously. Selection is the native record value our CAS expects. Anything
// else (owner graph, list, items, meshes, LOD bounds, profile) is structure.
Drift Compare(const Snapshot& before,const Snapshot& fresh) noexcept {
    if(fresh==before)return Drift::None;
    auto aligned=fresh;aligned.camera=before.camera;
    for(std::size_t i=0;i<MaxCars;++i)aligned.slots[i].position=before.slots[i].position;
    if(aligned==before)return Drift::Motion;
    for(std::size_t i=0;i<MaxCars;++i)aligned.slots[i].selected=before.slots[i].selected;
    return aligned==before?Drift::Selection:Drift::Structure;
}
Plan EvaluateSnapshot(const Snapshot& before,uint32_t sample,uint32_t count) noexcept {
    Context context{};
    // Fresh per-selection-batch tokens, NOT claims of persistent car lifetimes.
    // State{} below intentionally disables 40-unit carry/hysteresis on reuse.
    context.sessionEpoch=sample;context.managerEpoch=sample;context.sample=sample;
    context.manager=before.graph.manager;context.profile=before.profile;context.forcedLod=before.forced;
    context.camera=PositionOf(before.camera);context.mainViewCount=count;
    for(const auto& slot:before.slots)if(slot.car && slot.local) {context.player=slot.car;break;}
    if(before.targetIndex>=0) {
        // Guarded native indexed lookup maps an invalid nonnegative slot to 0.
        const auto index=before.targetIndex<12?before.targetIndex:0;
        context.currentTarget=before.slots[static_cast<std::size_t>(index)].car;
    } else context.currentTarget=context.player;
    context.requested=requested;context.sessionActive=world.load();context.vrActive=context.sessionActive;
    context.mainView=count==1;context.race=before.profile>=0 && before.profile<=2;
    context.identitiesValidated=context.player!=0;context.stopped=stopped.load();
    std::array<Car,MaxCars> cars{};
    for(std::size_t i=0;i<static_cast<std::size_t>(before.graph.count);++i) {
        const auto& s=before.slots[i];auto& car=cars[i];
        car.identity=s.car;car.epoch=s.car?sample:0;car.bestMesh=s.mesh;
        car.position=PositionOf(s.position);car.best=s.best;car.worst=s.worst;car.selected=s.selected;
        car.opponent=s.car && !s.local;car.bestMeshLoaded=s.mesh!=0;
    }
    const Frame frame{context,{cars.data(),static_cast<std::size_t>(before.graph.count)}};
    return Evaluate(frame,State{});
}
void Refuse(FloorRefusal kind,const char* reason) noexcept {
    floorReason.store(reason);++refusals;++floorRefusals[static_cast<std::size_t>(kind)];
}
void FloorBody(uintptr_t caller,void* self,const void* views,uint32_t count,const void* list,uint32_t flag) noexcept {
    if(!permission || wrapperDepth!=1) {Refuse(FloorRefusal::Permission,"no_current_owned_wrapper_permission");return;}
    if(caller!=staged.adapter+0x12) {Refuse(FloorRefusal::Adapter,"wrong_actual_adapter_return");return;}
    if constexpr(SingleAttemptDiagnostic) {
        Refuse(FloorRefusal::Diagnostic,"diagnostic_floors_disabled");return;
    } else {
    if(!ready.load(std::memory_order_acquire) || !world.load(std::memory_order_acquire) || stopped.load(std::memory_order_acquire) ||
       ReplayingHere()) {
        Refuse(FloorRefusal::Inactive,"inactive_vr_route");return;
    }
    if(wrapperRoute<1 || wrapperRoute>3) {Refuse(FloorRefusal::Route,"invalid_wrapper_route");return;}
    ++selectionPassesByRoute[wrapperRoute-1];
    Snapshot before{};
    if(const auto failure=Capture(before,reinterpret_cast<uintptr_t>(self),reinterpret_cast<uintptr_t>(views),count,
         reinterpret_cast<uintptr_t>(list),flag)) {Refuse(FloorRefusal::Capture,failure);return;}
    if(before.graph!=wrapperOwner) {Refuse(FloorRefusal::Owner,"wrapper_entry_owner_graph_changed");return;}
    uint32_t sample=++batches;
    if(!sample) {Refuse(FloorRefusal::Batch,"batch_counter_wrapped");return;}
    auto plan=EvaluateSnapshot(before,sample,count);
    policyLastRefusal.store(static_cast<uint32_t>(plan.Refused()));
    policyInspected+=static_cast<uint32_t>(plan.Stats().inspected);policyNearby+=static_cast<uint32_t>(plan.Stats().nearby);
    for(std::size_t i=0;i<policyResults.size();++i)
        if(plan.Stats().results[i])policyResults[i]+=static_cast<uint32_t>(plan.Stats().results[i]);
    if(plan.Refused()!=Refusal::None) {Refuse(FloorRefusal::Policy,"pure_policy_refused_identity_position_or_context");return;}
    if(plan.Changes().empty()) {floorReason.store("native_selection_retained");++retainedPasses;return;}
#ifdef GRID2VR_OPPONENT_LOD_RUNTIME_TEST
    if(testBetweenCaptures)testBetweenCaptures();
#endif
    Snapshot fresh{};
    if(const auto failure=Capture(fresh,reinterpret_cast<uintptr_t>(self),reinterpret_cast<uintptr_t>(views),count,
         reinterpret_cast<uintptr_t>(list),flag)) {Refuse(FloorRefusal::FreshCapture,failure);return;}
    // Permission was established at entry. The host clears ready/world at the
    // start of every Present and sets eyeReplay around the render thread's eye
    // pair; on this (game) thread those are transient and refusing here only
    // lets native low LOD through. Stop and any owner change still refuse.
    if(stopped.load(std::memory_order_acquire) || fresh.graph!=wrapperOwner) {
        Refuse(FloorRefusal::FreshGate,"fresh_owner_changed_or_stopped");return;
    }
    if(!ready.load(std::memory_order_acquire))++transientGates[0];
    if(!world.load(std::memory_order_acquire))++transientGates[1];
    if(eyeReplay.load(std::memory_order_acquire))++transientGates[2];
    switch(Compare(before,fresh)) {
    case Drift::None:break;
    case Drift::Motion:
        // Cars and camera keep moving on other threads between the two reads.
        // Structure and every CAS expectation are unchanged, so decide again
        // from the newer positions instead of letting native low LOD through.
        ++motionRefreshes;plan=EvaluateSnapshot(fresh,sample,count);
        if(plan.Refused()!=Refusal::None) {Refuse(FloorRefusal::Policy,"pure_policy_refused_identity_position_or_context");return;}
        if(plan.Changes().empty()) {floorReason.store("native_selection_retained");++retainedPasses;return;}
        break;
    case Drift::Selection:Refuse(FloorRefusal::FreshSelection,"fresh_native_selection_changed");return;
    case Drift::Structure:Refuse(FloorRefusal::FreshStructure,"fresh_snapshot_structure_changed");return;
    }
    floorReason.store("selected_floor_only");
    // Structure, ownership and expected selections are recaptured before ANY
    // write. No native apply, selection, simulation, mesh or saved-selection write.
    for(const auto change:plan.Changes()) {
        const auto address=before.graph.records+change.slot*RecordStride+0xfc;
        if(InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(address),change.selected,change.expected)==change.expected)++floors;
        else {floorReason.store("selected_compare_exchange_failed");++compareFailures;}
    }
    }
}
__declspec(noinline) void __fastcall FloorOnly(void* self,void*,const void* views,uint32_t count,const void* list,uint32_t flag) noexcept {
    FloatingState fp;
    auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
#ifdef GRID2VR_OPPONENT_LOD_RUNTIME_TEST
    caller=testReturn;
#endif
    FloorBody(caller,self,views,count,list,flag);
}
} // namespace

bool Prepare(uintptr_t base,const std::filesystem::path& folder,TransactionFn transaction) noexcept {
    if(started)return prepared && image==base && controller==GetCurrentThreadId();
    controller=GetCurrentThreadId();
    try {
        for(const auto* name:{L"runtime-session.request",L"xr-session.request",L"xr-projection.request"}) {
            if(!std::filesystem::is_regular_file(folder/name)) {Reason("dormant_desktop");return true;}
        }
        requested=true;started=true;output=folder;
        if(!transaction) {Reason("missing_complete_quiescence_transaction");Receipt();return false;}
        if((base&0xffff) || !Extent(base,ImageSize)) {Reason("invalid_image_base");Receipt();return false;}
        image=base;transact=transaction;
        Reason("live_body_auxiliary_or_pe_guard_refused");
        if(!Qualify()) {Receipt();return false;}
        prepared=true;Reason("prepared_waiting_for_active_vr_world_and_objects");Receipt();return true;
    }catch(...) {Reason("preparation_exception");Receipt();return false;}
}
void Tick(bool worldReady) noexcept {
    if(!prepared || controller!=GetCurrentThreadId())return;
    ++ticks;
    StatusOnExit status{ticks%StatusTicks==0,false};
    if((DiagnosticComplete() || controlledDormant.load(std::memory_order_acquire)) && !stopped.load(std::memory_order_acquire)) {
        world.store(false,std::memory_order_release);ready.store(false,std::memory_order_release);return;
    }
    world.store(worldReady && !stopped.load(std::memory_order_acquire),std::memory_order_release);
    if(stopped.load(std::memory_order_acquire) && !attachedManager.load(std::memory_order_acquire))return;
    Graph graph{};
    if(!GraphNow(graph)) {
        ready.store(false,std::memory_order_release);
        if constexpr(ControlledActivation) {
            if(singleAttemptUsed.load(std::memory_order_acquire) && !stopped.load(std::memory_order_acquire)) {
                controlledDormant.store(true,std::memory_order_release);world.store(false,std::memory_order_release);
                Reason("controlled_owner_unavailable_floors_disabled");return;
            }
        }
        Reason(stopped.load()?"stop_graph_not_live_no_stale_restore":"waiting_for_live_owner_manager_updater");return;
    }
    if(!stopped.load() && Owned(graph)) {
        const bool active=world.load(std::memory_order_acquire);
        ready.store(active,std::memory_order_release);
        Reason(active?"candidate_ready_manual_native_acceptance_pending":"waiting_for_active_vr_world");return;
    }
    ready.store(false,std::memory_order_release);
    if constexpr(ControlledActivation) {
        if(singleAttemptUsed.load(std::memory_order_acquire) && !stopped.load(std::memory_order_acquire)) {
            controlledDormant.store(true,std::memory_order_release);world.store(false,std::memory_order_release);
            Reason("controlled_owner_or_routes_changed_floors_disabled");return;
        }
    }
    if(!stopped.load() && !world.load()) {Reason("waiting_for_active_vr_world");return;}
    if(graph.manager!=retryManager || graph.updater!=retryUpdater) {
        retryManager=graph.manager;retryUpdater=graph.updater;attempts=0;nextTry=ticks;
    }
    if(attempts>=MaxAttempts) {Reason("bounded_transaction_retries_exhausted");return;}
    if(ticks<nextTry)return;
    nextTry=ticks+RetryTicks;
    Attempt(graph,stopped.load());
}
void Stop() noexcept {
    // Always disable first, even if called from a non-controller shutdown path.
    stopped.store(true,std::memory_order_release);ready.store(false,std::memory_order_release);
    world.store(false,std::memory_order_release);
    if(controller!=GetCurrentThreadId())return;
    StatusOnExit status{true,true}; // Final current status is never event-cap limited.
    if(!prepared || !attachedManager.load(std::memory_order_acquire))return;
    Graph graph{};
    if(!GraphNow(graph)) {Reason("stop_graph_not_live_no_stale_restore");return;}
    // Stop starts one separate bounded restore budget; repeated Stop does not
    // reset it. Failed restoration can be retried by controller Tick(false).
    if(!restoreStarted) {restoreStarted=true;attempts=0;}
    if(attempts>=MaxAttempts)return;
    nextTry=ticks+RetryTicks;
    Attempt(graph,true);
}
void RevokeWorld() noexcept {
    world.store(false,std::memory_order_release);
    ready.store(false,std::memory_order_release);
}
void SetEyeReplay(bool active) noexcept {
    if(active)replayThread.store(GetCurrentThreadId(),std::memory_order_release);
    eyeReplay.store(active,std::memory_order_release);
}
RuntimeReport Report() noexcept {
    RuntimeReport r{};
    r.requested=requested;r.prepared=prepared;r.ready=ready.load();r.worldReady=world.load();r.stopped=stopped.load();
    r.eyeReplay=eyeReplay.load();r.cleanupNotice=cleanupNotice;
    r.activeWrappers=activeWrappers.load();r.controllerTicks=ticks;
    r.attempts=totalAttempts;r.attachments=attachments;r.restorations=restorations;
    r.callbackA=callbackA.load();r.callbackB=callbackB.load();r.updater=updaterPasses.load();
    r.originalRoutes=originalRoutes.load();r.privateRoutes=privateRoutes.load();r.batches=batches.load();
    r.floors=floors.load();r.refusals=refusals.load();r.compareFailures=compareFailures.load();
    for(std::size_t i=0;i<r.privateByRoute.size();++i) {
        r.privateByRoute[i]=privateByRoute[i].load();r.selectionPassesByRoute[i]=selectionPassesByRoute[i].load();
    }
    r.cache=cache;r.adapter=staged.adapter;r.manager=attachedManager.load();r.updaterObject=attachedUpdater.load();
    r.imageBase=image;r.peImageBase=peImageBase;
    r.integrityBodyCount=prepared?static_cast<uint32_t>(std::size(auxiliary)+native::BodyCount):0;
    r.busyRangeCount=static_cast<uint32_t>(busy.size());
    r.updaterMethodCount=prepared?static_cast<uint32_t>(updatertable::MethodCount):0;
    r.singleAttemptDiagnostic=SingleAttemptDiagnostic;r.diagnosticComplete=DiagnosticComplete();
    r.controlledActivation=ControlledActivation;r.attachmentAttempted=attachmentAttempts!=0;
    r.controlledDormant=controlledDormant.load();r.attachmentAttempts=attachmentAttempts;r.restoreAttempts=restoreAttempts;
    r.policyInspected=policyInspected.load();r.policyNearby=policyNearby.load();r.policyLastRefusal=policyLastRefusal.load();
    for(std::size_t i=0;i<r.policyResults.size();++i)r.policyResults[i]=policyResults[i].load();
    for(std::size_t i=0;i<r.floorRefusals.size();++i)r.floorRefusals[i]=floorRefusals[i].load();
    r.motionRefreshes=motionRefreshes.load();r.retainedPasses=retainedPasses.load();
    for(std::size_t i=0;i<r.originalByReason.size();++i)r.originalByReason[i]=originalByReason[i].load();
    for(std::size_t i=0;i<r.transientGates.size();++i)r.transientGates[i]=transientGates[i].load();
    std::memcpy(r.reason,lastReason,sizeof(lastReason));
    const auto floor=floorReason.load();std::size_t i{};
    for(;i+1<sizeof(r.floorReason) && floor[i];++i)r.floorReason[i]=floor[i];
    return r;
}
bool DiagnosticComplete() noexcept {return SingleAttemptDiagnostic && singleAttemptUsed.load(std::memory_order_acquire);}
bool WorldChecksNeeded() noexcept {
    return !stopped.load(std::memory_order_acquire) && !DiagnosticComplete() && !controlledDormant.load(std::memory_order_acquire);
}
#ifdef GRID2VR_OPPONENT_LOD_RUNTIME_TEST
namespace testing {
void Boundary(NativeBoundary boundary) noexcept {testBoundary=boundary;}
uint32_t Callback(uintptr_t nativeReturn,void* actualDispatchManager) noexcept {
    return CallbackRoute(nativeReturn,actualDispatchManager?reinterpret_cast<uintptr_t>(actualDispatchManager):attachedManager.load());
}
uint32_t Update(void* self,uint32_t argument) noexcept {return ModUpdate(self,nullptr,argument);}
void Floor(uintptr_t adapterReturn,void* self,const void* views,uint32_t count,const void* list,uint32_t flag) noexcept {
    const auto previous=testReturn;testReturn=adapterReturn;
    FloorOnly(self,nullptr,views,count,list,flag);testReturn=previous;
}
void BetweenCaptures(void (*hook)() noexcept) noexcept {testBetweenCaptures=hook;}
}
#endif
} // namespace grid2vr::opponentlod::runtime
