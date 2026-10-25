#include "replay_packet.h"
#include "lazy_list_code.h"
#include "lazy_auxiliary_code.h"
#include <cstdio>
#include <fstream>
#include <filesystem>
using namespace grid2vr;
int main(int argc,char** argv) {
    if (argc!=5) return 1;
    ScenePacket packet{};
    std::ifstream packetFile(argv[1],std::ios::binary),codeFile(argv[2],std::ios::binary);
    std::array<uint8_t,31> code{};
    if (!packetFile.read(reinterpret_cast<char*>(packet.data()),packet.size()) || packetFile.peek()!=EOF ||
        !codeFile.read(reinterpret_cast<char*>(code.data()),code.size()) || codeFile.peek()!=EOF || PacketHasLazyLists(packet)) return 2;
    const auto original=packet;
    for (size_t offset:{0x84u,0x88u,0xe4u}) {
        packet[offset]=4;if (!PacketHasLazyLists(packet)) return 3;packet=original;
    }
    constexpr uintptr_t base=0x400000,main=0x20000000;
    uint32_t mode=3,builder=0x21000000;uint8_t enabled=1;
    uintptr_t unreadable{};unsigned reads{};
    auto read=[&](uintptr_t address,void* out,size_t size) {
        ++reads;if (address==unreadable) return false;
        if (address==base+0x784b08 && size==code.size()) memcpy(out,code.data(),size);
        else if (address==base+0xe2fa7c && size==4) memcpy(out,&mode,4);
        else if (address==main+0xe4 && size==4) memcpy(out,&builder,4);
        else if (address==uintptr_t(builder)+0x68 && size==1) memcpy(out,&enabled,1);
        else return false;
        return true;
    };
    auto refusal=[&](uintptr_t b,uintptr_t m) { LazyListGate gate{};const auto error=ReadLazyListGate(b,m,read,gate);return error?error:gate.Open()?"enabled":nullptr; };
    if (refusal(base,main) || reads!=2) return 4;
    mode=4;builder=0;reads=0;
    if (refusal(base,main) || reads!=3) return 5;
    builder=0x21000000;enabled=0;reads=0;
    if (refusal(base,main) || reads!=4) return 6;
    enabled=1;
    if (!refusal(base,main)) return 7;
    enabled=0;
    for (auto address:{base+0x784b08,base+0xe2fa7c,main+0xe4,uintptr_t(builder)+0x68}) {
        unreadable=address;if (!refusal(base,main)) return 8;
    }
    unreadable=0;
    for (auto& byte:code) {
        byte^=1;if (!refusal(base,main)) return 9;byte^=1;
    }
    if (!refusal(UINT32_MAX,main) || !refusal(base,UINT32_MAX)) return 10;
    builder=UINT32_MAX;
    if (!refusal(base,main) || packet!=original) return 11;
    // Entire supported native function, including all mutating calls and the
    // void/ret-4 epilogue. Every changed byte must reject with no permitted detour form.
    std::array<uint8_t,144> native{};
    std::ifstream nativeFile(argv[3],std::ios::binary);
    if (!nativeFile.read(reinterpret_cast<char*>(native.data()),native.size()) || nativeFile.peek()!=EOF || native!=nativeLazyListCode) return 12;
    auto readCode=[&](uintptr_t address,void* out,size_t size) {
        if (address!=base+0x63db10 || size!=native.size()) return false;
        memcpy(out,native.data(),size);return true;
    };
    if (!LazyListCodeMatches(base,readCode)) return 13;
    for (auto& byte:native) { byte^=1;if (LazyListCodeMatches(base,readCode)) return 14;byte^=1; }
    // The old inline detour is explicitly refused: it changes a native seed.
    native[0]=0xe9;
    if (LazyListCodeMatches(base,readCode) || LazyListCodeMatches(UINT32_MAX,readCode)) return 15;
    native=nativeLazyListCode;

    // All four observed native call sites must match their executable bytes.
    // Check every byte, unknown/unreadable callers, and relocated image bases.
    std::ifstream callersFile(argv[4],std::ios::binary);
    for (const auto& site:nativeLazyListCallSites) {
        std::array<uint8_t,39> callerCode{};
        if (!callersFile.read(reinterpret_cast<char*>(callerCode.data()),site.size) || callerCode!=site.code) return 58;
        for (uintptr_t imageBase:{base,uintptr_t(0x10000000)}) {
            const auto selector=static_cast<uint32_t>(imageBase+0xe2fa7c);
            memcpy(callerCode.data()+2,&selector,4);
            bool deny=false;
            auto readCaller=[&](uintptr_t address,void* out,size_t size) {
                if (deny || address!=imageBase+site.start || size!=site.size) return false;
                memcpy(out,callerCode.data(),size);return true;
            };
            if (!LazyListCallerMatches(imageBase,site.returnRva,readCaller)) return 59;
            for (size_t i=0;i<site.size;++i) {
                callerCode[i]^=1;
                if (LazyListCallerMatches(imageBase,site.returnRva,readCaller)) return 60;
                callerCode[i]^=1;
            }
            if (LazyListCallerMatches(imageBase,site.returnRva+1,readCaller) ||
                LazyListCallerMatches(imageBase+1,site.returnRva,readCaller) ||
                LazyListCallerMatches(UINT32_MAX,site.returnRva,readCaller)) return 61;
            deny=true;
            if (LazyListCallerMatches(imageBase,site.returnRva,readCaller)) return 62;
        }
    }
    if (callersFile.peek()!=EOF) return 63;

    // Independent shipped bytes for both extra flag consumers and the native
    // auxiliary pending early-return path, including ASLR relocation fields.
    std::ifstream auxiliaryFile(std::filesystem::path(argv[3]).parent_path()/"native-lazy-auxiliary.bin",std::ios::binary);
    for(const auto& fragment:nativeLazyAuxiliaryCode) {
        std::array<uint8_t,91> actual{};
        if(!auxiliaryFile.read(reinterpret_cast<char*>(actual.data()),fragment.size) || actual!=fragment.code) return 68;
    }
    if(auxiliaryFile.peek()!=EOF) return 69;
    for(uintptr_t imageBase:{base,uintptr_t(0x10000000)}) {
        uintptr_t changedByte{},deniedCode{};
        auto readAuxiliary=[&](uintptr_t address,void* out,size_t size) {
            if(address==deniedCode) return false;
            for(const auto& fragment:nativeLazyAuxiliaryCode) if(address==imageBase+fragment.rva && size==fragment.size) {
                auto actual=fragment.code;
                for(size_t i=0;i<fragment.relocationCount;++i) {
                    uint32_t value{};const auto at=fragment.relocations[i];memcpy(&value,actual.data()+at,4);
                    value=value-0x400000u+static_cast<uint32_t>(imageBase);memcpy(actual.data()+at,&value,4);
                }
                if(changedByte>=address && changedByte<address+size) actual[changedByte-address]^=1;
                memcpy(out,actual.data(),size);return true;
            }
            return false;
        };
        if(!LazyAuxiliaryCodeMatches(imageBase,readAuxiliary)) return 70;
        for(const auto& fragment:nativeLazyAuxiliaryCode) {
            for(size_t i=0;i<fragment.size;++i) {
                changedByte=imageBase+fragment.rva+i;
                if(LazyAuxiliaryCodeMatches(imageBase,readAuxiliary)) return 71;
            }
            changedByte=0;deniedCode=imageBase+fragment.rva;
            if(LazyAuxiliaryCodeMatches(imageBase,readAuxiliary)) return 72;
            deniedCode=0;
        }
        if(LazyAuxiliaryCodeMatches(UINT32_MAX,readAuxiliary)) return 73;
    }

    LazyListState state{};
    constexpr uintptr_t object=0x22000000;
    std::array<uint32_t,3> trees{0x23000000,0x23000100,0x23000200},roots{};
    memcpy(state.object.data()+0x34,trees.data(),sizeof(trees));state.object[0x68]=1;state.auxiliaryPending=1;
    uintptr_t denied{};
    auto readState=[&](uintptr_t address,void* out,size_t size) {
        if (address==denied) return false;
        if (address==object && size==state.object.size()) { memcpy(out,state.object.data(),size);return true; }
        if (address==object+0x2400 && size==1) { memcpy(out,&state.auxiliaryPending,1);return true; }
        for (unsigned i=0;i<3;++i) if (address==uintptr_t(trees[i])+4 && size==4) { memcpy(out,&roots[i],4);return true; }
        return false;
    };
    LazyListState snapshot{};
    if (!ReadLazyListState(object,readState,snapshot) || snapshot!=state) return 18;
    for (auto address:{object,object+0x2400,uintptr_t(trees[0])+4,uintptr_t(trees[1])+4,uintptr_t(trees[2])+4}) {
        denied=address;if (ReadLazyListState(object,readState,snapshot)) return 19;
    }
    denied=0;
    if (ReadLazyListState(0,readState,snapshot) || ReadLazyListState(UINT32_MAX,readState,snapshot)) return 20;
    uint32_t invalid=UINT32_MAX;memcpy(state.object.data()+0x34,&invalid,4);
    if (ReadLazyListState(object,readState,snapshot)) return 21;
    memcpy(state.object.data()+0x34,trees.data(),sizeof(trees));

    const LazyListIdentity center{41,4675,11,0x24000000,object};
    const LazyListGate open{4,static_cast<uint32_t>(object),1};
    EmptyListWitness witness{};
    EmptyListReplay replay;
    unsigned exchanges{};bool denyAcquire{},denyRestore{},ownerValid=true;
    auto exchange=[&](uintptr_t address,uint8_t expected,uint8_t desired) {
        ++exchanges;
        if(address!=object+0x68 || !ownerValid || (desired==0?denyAcquire:denyRestore) || state.object[0x68]!=expected) return false;
        state.object[0x68]=desired;return true;
    };
    auto begin=[&]() { return replay.Begin(center,packet,open,state,witness,exchange); };
    auto finish=[&](bool readable=true) {
        return replay.Finish(center,readable,packet,{4,static_cast<uint32_t>(object),state.object[0x68]},state);
    };
    // Empty memory alone is insufficient: require the verified completed scene.
    if(begin() || exchanges) return 22;
    witness.Observe(center,false,true,packet,open,state);
    if(begin() || exchanges || witness.rejectionMask!=32) return 23;
    witness.Observe(center,true,true,packet,open,state);
    if(!witness.Matches(center,packet,open,state)) return 24;
    const auto beforePacket=packet;const auto beforeState=state;
    for(unsigned eye=0;eye<2;++eye) {
        if(!begin() || state.object[0x68]!=0 || !replay.Active()) return 25;
        // Native gate is closed only while this eye renders synchronously.
        auto suppressed=beforeState;suppressed.object[0x68]=0;
        if(state!=suppressed || !replay.Release(exchange) || state!=beforeState || !finish() || replay.Active()) return 26;
    }
    if(exchanges!=4 || state!=beforeState || packet!=beforePacket || native!=nativeLazyListCode) return 27;
    // Scene, frame, thread, packet and builder are all part of ownership.
    for(unsigned field=0;field<5;++field) {
        auto wrong=center;
        if(field==0)++wrong.scene;if(field==1)++wrong.frame;if(field==2)++wrong.thread;
        if(field==3)++wrong.packet;if(field==4)++wrong.builder;
        if(replay.Begin(wrong,packet,open,state,witness,exchange)) return 28;
        if(!begin() || !replay.Release(exchange) || replay.Finish(wrong,true,packet,open,state)) return 29;
    }
    // Populated packets and source trees never authorize a byte write.
    for(size_t offset:{0x84u,0x88u,0xe4u}) {
        auto nonempty=packet;nonempty[offset]=4;const auto calls=exchanges;
        EmptyListWitness bad{};bad.Observe(center,true,true,nonempty,open,state);
        if(replay.Begin(center,nonempty,open,state,bad,exchange) || exchanges!=calls) return 30;
    }
    for(unsigned i=0;i<3;++i) {
        auto populated=state;populated.roots[i]=4;const auto calls=exchanges;
        EmptyListWitness bad{};bad.Observe(center,true,true,packet,open,populated);
        if(replay.Begin(center,packet,open,populated,bad,exchange) || exchanges!=calls) return 31;
    }
    // A disabled gate, absent builder or different mode needs no mutation.
    for(const LazyListGate gate:{LazyListGate{4,static_cast<uint32_t>(object),0},LazyListGate{4,0,0},LazyListGate{3,0,0}}) {
        auto identity=center;identity.builder=gate.builder;
        EmptyListWitness closed{};closed.Observe(identity,true,true,packet,gate,{});
        const auto calls=exchanges;
        if(!replay.Begin(identity,packet,gate,{},closed,exchange) || !replay.Release(exchange) ||
            !replay.Finish(identity,true,packet,gate,{}) || exchanges!=calls || state!=beforeState) return 32;
    }
    // Changed packet payload or builder state is rejected before acquiring.
    auto changed=state;changed.object[0x24]^=1;
    auto changedPacket=packet;changedPacket[0x10]^=1;
    if(replay.Begin(center,packet,open,changed,witness,exchange) ||
       replay.Begin(center,changedPacket,open,state,witness,exchange)) return 33;
    auto wrongGate=open;++wrongGate.enabled;
    if(replay.Begin(center,packet,wrongGate,state,witness,exchange)) return 34;
    denyAcquire=true;
    if(begin() || replay.Active() || state!=beforeState) return 35;
    denyAcquire=false;state.object[0x68]=2;
    if(replay.Begin(center,packet,open,beforeState,witness,exchange) || state.object[0x68]!=2) return 36;
    state=beforeState;
    // Release is idempotent and must precede the post-read. Read/other-field
    // failures still release our byte; unsafe ownership or changed flags do not
    // overwrite native data and make the eye fail closed.
    if(!begin() || !replay.Release(exchange)) return 37;
    const auto releasedCount=exchanges;
    if(!replay.Release(exchange) || exchanges!=releasedCount || finish(false) || state!=beforeState) return 38;
    if(!begin()) return 39;
    state.object[0x24]^=1;
    if(!replay.Release(exchange) || state.object[0x68]!=1 || finish()) return 40;
    state=beforeState;
    if(!begin() || !replay.Release(exchange)) return 41;
    packet[0x10]^=1;
    if(finish()) return 42;
    packet=beforePacket;
    if(!begin()) return 43;
    denyRestore=true;
    if(replay.Release(exchange) || finish() || state.object[0x68]!=0) return 44;
    denyRestore=false;state=beforeState;
    if(!begin()) return 45;
    ownerValid=false;
    if(replay.Release(exchange) || finish() || state.object[0x68]!=0) return 46;
    ownerValid=true;state=beforeState;
    if(!begin()) return 47;
    state.object[0x68]=2;
    if(replay.Release(exchange) || finish() || state.object[0x68]!=2) return 48;
    state=beforeState;
    // A nested attempt cannot release or replace the outer lease. Its final
    // check fails, while the outer release still restores exactly its byte.
    if(!begin() || begin() || !replay.Release(exchange) || finish() || state!=beforeState) return 49;
    if(!begin() || !replay.Release(exchange)) return 50;
    replay.Cancel();
    if(replay.Active() || state!=beforeState || !begin() || !replay.Release(exchange) || !finish()) return 51;
    // Failed witness reads and invalid scene IDs cannot be rehabilitated by an
    // empty packet; both eyes still need the original completed boundary.
    EmptyListWitness bad{};bad.Observe(center,true,false,packet,open,state);
    if(replay.Begin(center,packet,open,state,bad,exchange)) return 52;
    auto noPending=state;noPending.auxiliaryPending=0;
    bad.Observe(center,true,true,packet,open,noPending);
    if(replay.Begin(center,packet,open,noPending,bad,exchange) || bad.rejectionMask!=64) return 74;
    if(!begin()) return 75;
    state.auxiliaryPending=0;
    if(!replay.Release(exchange) || finish() || state.object[0x68]!=1) return 76;
    state=beforeState;
    auto invalidIdentity=center;invalidIdentity.scene=0;
    bad.Observe(invalidIdentity,true,true,packet,open,state);
    if(replay.Begin(invalidIdentity,packet,open,state,bad,exchange)) return 53;
    if(native!=nativeLazyListCode || state!=beforeState || packet!=beforePacket) return 54;
    puts("Empty native lists: original code/four gates, completed-scene witness, two-eye byte lease, and guarded restoration.");
}
