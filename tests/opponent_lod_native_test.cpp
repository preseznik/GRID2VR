#include "opponent_lod_native.h"
#include "game_executable.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <xmmintrin.h>

#if !defined(_M_IX86) || !defined(GRID2VR_OPPONENT_LOD_NATIVE_TEST)
#error This isolated fixture requires MSVC x86 and GRID2VR_OPPONENT_LOD_NATIVE_TEST.
#endif
#define CHECK(x) do {if(!(x)){std::printf("line %d: %s\n",__LINE__,#x);return false;}} while(false)
namespace native=grid2vr::opponentlod::native;
namespace {
uint32_t Read32(const uint8_t* at) {uint32_t value{};std::memcpy(&value,at,4);return value;}
void Write32(uint8_t* at,uint32_t value) {std::memcpy(at,&value,4);}
using Storage=std::array<std::vector<uint8_t>,native::BodyCount>;
native::BodyInputs Inputs(const Storage& storage) {
    native::BodyInputs result{};
    for(std::size_t i=0;i<storage.size();++i)result[i]=storage[i];
    return result;
}
struct Synthetic {
    Storage storage;
    std::array<native::BodyGuard,native::BodyCount> guards{};
    std::array<std::string,native::BodyCount> hashes;
    explicit Synthetic(uint32_t base=native::PreferredImageBase) {
        for(std::size_t i=0;i<storage.size();++i) {
            guards[i]=native::Guards()[i];const auto& guard=guards[i];auto& bytes=storage[i];
            // Deliberately NOT executable bodies. Only the standalone generated
            // adapter and test-owned C++/assembly below are ever made executable.
            bytes.resize(guard.size,0xcc);
            for(const auto offset:guard.highlow)Write32(bytes.data()+offset,native::PreferredImageBase+0xc00000+offset);
            for(const auto call:guard.calls) {
                bytes[call.offset]=0xe8;Write32(bytes.data()+call.offset+1,call.targetRva-(guard.rva+call.offset+5));
            }
            hashes[i]=grid2vr::gameexe::Digest(bytes.data(),guard.size);guards[i].sha256=hashes[i];
            for(const auto offset:guard.highlow)Write32(bytes.data()+offset,Read32(bytes.data()+offset)+base-native::PreferredImageBase);
        }
    }
};

bool CheckCopies(const Storage& before,const Storage& after,std::span<const uint8_t> output,
    const native::StageResult& result,uint32_t image,uint32_t copy,uint32_t helper) {
    CHECK(result && result.used==native::StagingBytes && before==after);
    std::size_t callCount{},relocCount{},privateEdges{};
    for(std::size_t i=0;i<before.size();++i) {
        const auto& guard=native::Guards()[i];const auto offset=result.copies[i]-copy;
        CHECK((result.copies[i]&15)==0 && offset+guard.size<=result.used);
        std::vector<bool> changed(guard.size,false);
        for(const auto call:guard.calls) {
            uint32_t target=image+call.targetRva;
            if(i==0 && call.offset==0x340){target=result.copies[1];++privateEdges;}
            if(i==1 && call.offset==0x774){target=result.copies[2];++privateEdges;}
            if(i==2 && call.offset==0x11){target=result.copies[3];++privateEdges;}
            if(i==3 && call.offset==0x92f){target=result.adapter;++privateEdges;}
            if(i==4 && call.offset==0x5a7){target=result.copies[1];++privateEdges;}
            CHECK(output[offset+call.offset]==0xe8);
            CHECK(result.copies[i]+call.offset+5+Read32(output.data()+offset+call.offset+1)==target);
            for(unsigned j=1;j<5;++j)changed[call.offset+j]=true;
            ++callCount;
        }
        for(const auto reloc:guard.highlow) {
            CHECK(Read32(output.data()+offset+reloc)==Read32(before[i].data()+reloc));++relocCount;
        }
        for(std::size_t j=0;j<guard.size;++j)if(!changed[j])CHECK(output[offset+j]==before[i][j]);
    }
    CHECK(callCount==90 && relocCount==128 && privateEdges==5);
    const auto adapter=result.adapter-copy;
    CHECK(adapter+native::AdapterBytes==result.used && (result.adapter&15)==0);
    CHECK(output[adapter]==0x51 && output[adapter+0x12]==0x59);
    CHECK(result.adapter+0x12+Read32(output.data()+adapter+0x0e)==helper);
    CHECK(result.adapter+0x18+Read32(output.data()+adapter+0x14)==image+native::ApplyRva);
    for(std::size_t i=result.used;i<output.size();++i)CHECK(output[i]==0xa5);
    return true;
}

bool Relocations() {
    for(const auto layout:std::array<std::array<uint32_t,3>,3>{{
        {0x400000,0x50000000,0x51000000},{0x14000000,0x60000000,0x61000000},
        {0x400000,0xf0000000,0x10010000}}}) {
        Synthetic fixture(layout[0]);const auto before=fixture.storage;
        std::array<uint8_t,native::AllocationBytes> output;output.fill(0xa5);
        const auto result=native::testing::StageCopies(fixture.guards,Inputs(fixture.storage),layout[0],layout[1],layout[2],
            std::span(output).first(native::StagingBytes));
        CHECK(CheckCopies(before,fixture.storage,output,result,layout[0],layout[1],layout[2]));
        // Synthetic guards are NEVER accepted by the pinned native entry point.
        output.fill(0xa5);
        CHECK(native::StageCopies(Inputs(fixture.storage),layout[0],layout[1],layout[2],output).failure==native::Failure::BodyHash);
        CHECK(std::all_of(output.begin(),output.end(),[](uint8_t b){return b==0xa5;}));
    }
    return true;
}

bool Refusals() {
    Synthetic fixture;std::array<uint8_t,native::AllocationBytes> output;output.fill(0xa5);
    const auto reject=[&](native::Failure expected,const native::BodyInputs& input,uint32_t image=0x400000,
        uint32_t copy=0x50000000,uint32_t helper=0x51000000,std::size_t capacity=native::AllocationBytes) {
        const auto result=native::testing::StageCopies(fixture.guards,input,image,copy,helper,std::span(output).first(capacity));
        return !result && result.failure==expected && !result.used && !result.adapter &&
            std::all_of(result.copies.begin(),result.copies.end(),[](uint32_t value){return !value;}) &&
            std::all_of(output.begin(),output.end(),[](uint8_t b){return b==0xa5;});
    };
    auto input=Inputs(fixture.storage);
    CHECK(reject(native::Failure::Capacity,input,0x400000,0x50000000,0x51000000,native::StagingBytes-1));
    for(const uint32_t copy:{0u,0x400000u,0x50000001u,0xfffffff0u})CHECK(reject(native::Failure::InvalidAddress,input,0x400000,copy));
    for(const uint32_t image:{0u,0x400001u,0xff000000u})CHECK(reject(native::Failure::InvalidAddress,input,image));
    for(const uint32_t helper:{0u,0x50000000u,0x50001000u})CHECK(reject(native::Failure::InvalidAddress,input,0x400000,0x50000000,helper));
    for(std::size_t i=0;i<input.size();++i) {
        input=Inputs(fixture.storage);input[i]=input[i].first(input[i].size()-1);
        CHECK(reject(native::Failure::InputSize,input));
    }
    input=Inputs(fixture.storage);
    for(std::size_t i=0;i<input.size();++i) {
        auto& bytes=fixture.storage[i];const auto& guard=fixture.guards[i];
        for(const auto at:{std::size_t(0),bytes.size()-1}) {
            bytes[at]^=1;CHECK(reject(native::Failure::BodyHash,input));bytes[at]^=1;
        }
        for(const auto call:guard.calls) {
            bytes[call.offset]^=1;CHECK(reject(native::Failure::CallOperand,input));bytes[call.offset]^=1;
            bytes[call.offset+1]^=1;CHECK(reject(native::Failure::CallOperand,input));bytes[call.offset+1]^=1;
        }
        for(const auto at:guard.highlow) {
            bytes[at]^=1;CHECK(reject(native::Failure::BodyHash,input));bytes[at]^=1;
        }
    }
    auto original=fixture.guards[0];fixture.guards[0].size+=1;
    CHECK(reject(native::Failure::InvalidManifest,input));fixture.guards[0]=original;
    fixture.guards[0].highlow=fixture.guards[0].highlow.subspan(1);
    CHECK(reject(native::Failure::InvalidManifest,input));fixture.guards[0]=original;
    fixture.guards[0].calls=fixture.guards[0].calls.subspan(1);
    CHECK(reject(native::Failure::InvalidManifest,input));fixture.guards[0]=original;
    // Check actual-buffer aliasing separately from symbolic address overlap.
    std::vector<uint8_t> aliased(native::AllocationBytes,0xa5);
    std::copy(fixture.storage[0].begin(),fixture.storage[0].end(),aliased.begin());
    input[0]=std::span<const uint8_t>(aliased).first(original.size);const auto snapshot=aliased;
    CHECK(native::testing::StageCopies(fixture.guards,input,0x400000,0x50000000,0x51000000,aliased).failure==native::Failure::AliasedInput);
    CHECK(aliased==snapshot);
    CHECK(native::BuildAdapter(0x50000000,0x51000000,0x52000000,std::span(output).first(23))==native::Failure::Capacity);
    CHECK(native::BuildAdapter(0xfffffff0,0x51000000,0x52000000,output)==native::Failure::InvalidAddress);
    CHECK(native::BuildAdapter(0x50000000,0x50000001,0x52000000,output)==native::Failure::InvalidAddress);
    CHECK(native::BuildAdapter(0x50000000,0x51000000,0x50000001,output)==native::Failure::InvalidAddress);
    CHECK(std::all_of(output.begin(),output.end(),[](uint8_t b){return b==0xa5;}));
    CHECK(native::Install()==native::Failure::NotQualified && !native::InstallationBlocker.empty());
    return true;
}

// All executed instructions below belong to this fixture, never the game.
uint32_t adapterEntry{},helperCount{},applyCount{},sequence{},helperOrder{},applyOrder{},wouldFloor{};
uint32_t expectedThis=0x11223344,expectedEdx=0x55667788;
uint32_t seenThis{},seenEdx{},seenViews{},seenCount{},seenList{},seenFlag{},applyThis{},applyEsp{};
uint32_t beforeEsp{},afterEsp{},afterEbx{},afterEsi{},afterEdi{},afterEbp{};
uint32_t refuse{};
uint32_t selectorFrame[]{0xabababab,0xcdcdcdcd,0x22334455,1,0x33445566,1};
__declspec(align(16)) uint8_t beforeFp[512]{},afterFp[512]{};

__declspec(noinline) void __fastcall DummyFloor(void* self,void* edx,const void* views,uint32_t count,const void* list,uint32_t flag) noexcept {
    ++helperCount;helperOrder=++sequence;
    seenThis=reinterpret_cast<uint32_t>(self);seenEdx=reinterpret_cast<uint32_t>(edx);
    seenViews=reinterpret_cast<uint32_t>(views);seenCount=count;seenList=reinterpret_cast<uint32_t>(list);seenFlag=flag;
    if(!refuse)++wouldFloor;
    // Require the bridge to restore manager ECX despite a helper clobber.
    __asm {mov ecx,0xdead1234}
    __asm {mov edx,0xdead5678}
    __asm {mov eax,0xdead9abc}
}
__declspec(naked) void DummyApply() {
    __asm {
        mov applyThis,ecx
        mov applyEsp,esp
        inc applyCount
        inc sequence
        mov eax,sequence
        mov applyOrder,eax
        mov ecx,0xeeeeeeee
        mov edx,0xdddddddd
        ret
    }
}
__declspec(naked) void InvokeAdapter() {
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        mov beforeEsp,esp
        mov ebp,0x12345678
        lea ebx,selectorFrame
        mov esi,0x23456789
        mov edi,0x3456789a
        mov ecx,expectedThis
        mov edx,expectedEdx
        fxsave beforeFp
        call dword ptr [adapterEntry]
        fxsave afterFp
        mov afterEsp,esp
        mov afterEbp,ebp
        mov afterEbx,ebx
        mov afterEsi,esi
        mov afterEdi,edi
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}
bool AdapterAbi() {
    struct OwnedCode {
        void* memory{VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)};
        ~OwnedCode(){if(memory)VirtualFree(memory,0,MEM_RELEASE);}
    } code;
    CHECK(code.memory);
    adapterEntry=reinterpret_cast<uint32_t>(code.memory);
    CHECK(native::BuildAdapter(adapterEntry,reinterpret_cast<uint32_t>(&DummyFloor),reinterpret_cast<uint32_t>(&DummyApply),
        {static_cast<uint8_t*>(code.memory),native::AdapterBytes})==native::Failure::None);
    DWORD old{};
    CHECK(VirtualProtect(code.memory,4096,PAGE_EXECUTE_READ,&old));
    CHECK(FlushInstructionCache(GetCurrentProcess(),code.memory,native::AdapterBytes));
    // Preserve the caller's complete FP state, including its preexisting stack.
    __declspec(align(16)) uint8_t callerFp[512]{};
    __asm {fxsave callerFp}
    unsigned short control{};
    __asm {fninit}
    __asm {fnstcw control}
    control=static_cast<unsigned short>((control&~0x0c00)|0x0800);
    __asm {fldcw control}
    __asm {fld1}
    __asm {fldpi}
    _mm_setcsr((_mm_getcsr()&~0x603fu)|0x4000u);
    bool valid=true;
    for(const uint32_t denied:{0u,1u})for(const uint32_t flag:{0u,1u,0xfedcba98u}) {
        refuse=denied;selectorFrame[5]=flag;
        helperCount=applyCount=sequence=helperOrder=applyOrder=wouldFloor=0;
        InvokeAdapter();
        valid=valid && helperCount==1 && applyCount==1 && helperOrder==1 && applyOrder==2 && wouldFloor==static_cast<uint32_t>(!denied) &&
            seenThis==expectedThis && seenEdx==expectedEdx && seenViews==selectorFrame[2] && seenCount==selectorFrame[3] &&
            seenList==selectorFrame[4] && seenFlag==flag && applyThis==expectedThis &&
            beforeEsp==afterEsp && applyEsp==beforeEsp-4 && afterEbp==0x12345678 &&
            afterEbx==reinterpret_cast<uint32_t>(selectorFrame) && afterEsi==0x23456789 && afterEdi==0x3456789a &&
            !std::memcmp(beforeFp,afterFp,5) && !std::memcmp(beforeFp+24,afterFp+24,4);
        // x87 physical register payloads are 80 bits in 16-byte FXSAVE slots.
        // Ignore reserved bytes and historical FP instruction/data pointers.
        for(unsigned slot=0;slot<8;++slot)valid=valid && !std::memcmp(beforeFp+32+slot*16,afterFp+32+slot*16,10);
    }
    __asm {fxrstor callerFp}
    CHECK(valid);
    return true;
}

bool NativeFile(const char* path) {
    // Explicit optional READ-ONLY input. No loader, image mapping, VirtualProtect
    // or execution is applied to these bytes or their staged copies.
    std::ifstream file(path,std::ios::binary|std::ios::ate);CHECK(file);
    const auto size=file.tellg();CHECK(size==15703608);
    std::vector<uint8_t> raw(static_cast<std::size_t>(size));file.seekg(0);
    file.read(reinterpret_cast<char*>(raw.data()),static_cast<std::streamsize>(raw.size()));CHECK(file);
    CHECK(grid2vr::gameexe::Digest(raw.data(),static_cast<ULONG>(raw.size()))==native::ImageSha256);
    const auto rawBefore=raw;
    CHECK(raw[0]=='M' && raw[1]=='Z');const auto pe=Read32(raw.data()+0x3c);
    CHECK(pe<raw.size()-0x100 && Read32(raw.data()+pe)==0x4550);
    IMAGE_FILE_HEADER header{};std::memcpy(&header,raw.data()+pe+4,sizeof(header));
    CHECK(header.Machine==IMAGE_FILE_MACHINE_I386 && header.SizeOfOptionalHeader==sizeof(IMAGE_OPTIONAL_HEADER32));
    IMAGE_OPTIONAL_HEADER32 optional{};std::memcpy(&optional,raw.data()+pe+24,sizeof(optional));
    CHECK(optional.Magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC && optional.ImageBase==native::PreferredImageBase);
    const auto sectionStart=pe+24+header.SizeOfOptionalHeader;
    CHECK(sectionStart+header.NumberOfSections*sizeof(IMAGE_SECTION_HEADER)<=raw.size());
    const auto rawOffset=[&](uint32_t rva,uint32_t bytes,std::size_t& offset) {
        bool found{};
        for(unsigned s=0;s<header.NumberOfSections;++s) {
            IMAGE_SECTION_HEADER section{};std::memcpy(&section,raw.data()+sectionStart+s*sizeof(section),sizeof(section));
            if(rva<section.VirtualAddress || rva-section.VirtualAddress>section.SizeOfRawData)continue;
            const auto relative=rva-section.VirtualAddress;
            if(bytes>section.SizeOfRawData-relative)continue;
            const auto candidate=static_cast<std::size_t>(section.PointerToRawData)+relative;
            if(candidate>raw.size() || bytes>raw.size()-candidate || found)return false;
            offset=candidate;found=true;
        }
        return found;
    };
    // Verify HIGHLOW membership from the PE directory independently of the C++
    // manifest. Rebasing with a self-supplied incomplete list alone proves less.
    std::array<std::vector<uint32_t>,native::BodyCount> peRelocs;
    const auto directory=optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    std::size_t directoryOffset{};CHECK(rawOffset(directory.VirtualAddress,directory.Size,directoryOffset));
    std::size_t cursor{};
    while(cursor<directory.Size) {
        CHECK(directory.Size-cursor>=8);
        const auto page=Read32(raw.data()+directoryOffset+cursor);
        const auto block=Read32(raw.data()+directoryOffset+cursor+4);
        CHECK(block>=8 && block<=directory.Size-cursor && !(block&1));
        for(std::size_t j=8;j<block;j+=2) {
            uint16_t entry{};std::memcpy(&entry,raw.data()+directoryOffset+cursor+j,2);
            if(!(entry>>12))continue;
            const auto rva=page+(entry&0xfffu);
            for(std::size_t i=0;i<peRelocs.size();++i) {
                const auto& guard=native::Guards()[i];
                if(rva<guard.rva || rva-guard.rva>=guard.size)continue;
                CHECK((entry>>12)==IMAGE_REL_BASED_HIGHLOW && rva-guard.rva<=guard.size-4);
                peRelocs[i].push_back(rva-guard.rva);
            }
        }
        cursor+=block;
    }
    Storage original;
    for(std::size_t i=0;i<original.size();++i) {
        const auto& guard=native::Guards()[i];std::size_t offset{};
        CHECK(rawOffset(guard.rva,guard.size,offset));
        original[i].assign(raw.begin()+offset,raw.begin()+offset+guard.size);
        CHECK(peRelocs[i].size()==guard.highlow.size());
        CHECK(std::equal(peRelocs[i].begin(),peRelocs[i].end(),guard.highlow.begin()));
    }
    for(const auto layout:std::array<std::array<uint32_t,3>,3>{{
        {0x400000,0x50000000,0x51000000},{0x14000000,0x60000000,0x61000000},
        {0x400000,0xf0000000,0x10010000}}}) {
        auto rebased=original;
        for(std::size_t i=0;i<rebased.size();++i)for(const auto offset:peRelocs[i])
            Write32(rebased[i].data()+offset,Read32(rebased[i].data()+offset)+layout[0]-native::PreferredImageBase);
        const auto before=rebased;
        std::array<uint8_t,native::AllocationBytes> output;output.fill(0xa5);
        const auto result=native::StageCopies(Inputs(rebased),layout[0],layout[1],layout[2],output);
        CHECK(CheckCopies(before,rebased,output,result,layout[0],layout[1],layout[2]));
    }
    CHECK(raw==rawBefore);
    std::puts("native file: exact fingerprint + five full-body guards + 90 calls/128 PE-verified HIGHLOW, three symbolic layouts; ZERO native execution");
    return true;
}
}
int main(int argc,char** argv) {
    if(argc!=1 && (argc!=3 || std::strcmp(argv[1],"--image"))) {
        std::puts("usage: opponent_lod_native_test [--image read-only-grid2_avx.exe]");return 2;
    }
    if(!Relocations() || !Refusals() || !AdapterAbi() || (argc==3 && !NativeFile(argv[2])))return 1;
    std::puts("PASS: copy staging/refusals; x86 adapter args/fastcall cleanup/ECX/nonvolatile registers/ESP/x87/MXCSR; apply ONCE on success and refusal; Install=NotQualified");
    return 0;
}
