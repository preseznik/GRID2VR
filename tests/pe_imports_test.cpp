#include "pe_imports.h"
#include <cstdio>
#include <vector>

#if !defined(_M_IX86)
#error The import-slot fixture is MSVC x86, like the renderer.
#endif
using grid2vr::FindImportSlot;
#define CHECK(value) do {if(!(value)){std::printf("pe imports check line %d: %s\n",__LINE__,#value);return 1;}} while(false)

namespace {
// Minimal mapped Win32 image: one import descriptor for XINPUT1_3.dll with
// ordinals 4, 2 and 3, mirroring the game's import order.
struct Synthetic {
    std::vector<uint8_t> bytes=std::vector<uint8_t>(0x2000);
    uintptr_t Base() {return reinterpret_cast<uintptr_t>(bytes.data());}
    template<class T> T& At(uint32_t offset) {return *reinterpret_cast<T*>(bytes.data()+offset);}
    Synthetic() {
        auto& dos=At<IMAGE_DOS_HEADER>(0);dos.e_magic=IMAGE_DOS_SIGNATURE;dos.e_lfanew=0x80;
        auto& nt=At<IMAGE_NT_HEADERS32>(0x80);
        nt.Signature=IMAGE_NT_SIGNATURE;nt.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        nt.OptionalHeader.SizeOfImage=static_cast<DWORD>(bytes.size());
        nt.OptionalHeader.NumberOfRvaAndSizes=IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]={0x400,2*sizeof(IMAGE_IMPORT_DESCRIPTOR)};
        auto& entry=At<IMAGE_IMPORT_DESCRIPTOR>(0x400);
        entry.OriginalFirstThunk=0x500;entry.FirstThunk=0x600;entry.Name=0x700;
        std::memcpy(bytes.data()+0x700,"XINPUT1_3.dll",14);
        const DWORD ordinals[]{4,2,3};
        for(unsigned i=0;i<3;++i) {
            At<DWORD>(0x500+i*4)=IMAGE_ORDINAL_FLAG32|ordinals[i];
            At<DWORD>(0x600+i*4)=0x1000+i; // bound addresses
        }
    }
};
}

int main() {
    // By-name import of this very test program, resolved by the real loader.
    const auto self=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto** tick=FindImportSlot(self,"kernel32.dll",0,"GetTickCount64");
    CHECK(tick && *tick==reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"GetTickCount64")));
    CHECK(FindImportSlot(self,"KERNEL32.DLL",0,"GetTickCount64")==tick);
    CHECK(!FindImportSlot(self,"kernel32.dll",0,"NoSuchExport"));
    CHECK(!FindImportSlot(self,"absent.dll",0,"GetTickCount64"));
    volatile auto now=GetTickCount64();(void)now;

    Synthetic image;const auto base=image.Base();
    CHECK(FindImportSlot(base,"XINPUT1_3.dll",3,nullptr)==reinterpret_cast<void**>(base+0x608));
    CHECK(FindImportSlot(base,"xinput1_3.dll",2,nullptr)==reinterpret_cast<void**>(base+0x604));
    CHECK(!FindImportSlot(base,"XINPUT1_3.dll",1,nullptr));
    CHECK(!FindImportSlot(base,"XINPUT1_3.dll",0,"XInputSetState")); // ordinal-only imports
    CHECK(!FindImportSlot(base,"XINPUT1_4.dll",3,nullptr));
    CHECK(!FindImportSlot(0,"XINPUT1_3.dll",3,nullptr) && !FindImportSlot(base,nullptr,3,nullptr));
    // Malformed images are refused rather than followed outside the image.
    {Synthetic bad;bad.At<IMAGE_IMPORT_DESCRIPTOR>(0x400).FirstThunk=0x1ffe;CHECK(!FindImportSlot(bad.Base(),"XINPUT1_3.dll",3,nullptr));}
    {Synthetic bad;bad.At<IMAGE_IMPORT_DESCRIPTOR>(0x400).Name=0x1ffc;CHECK(!FindImportSlot(bad.Base(),"XINPUT1_3.dll",3,nullptr));}
    {Synthetic bad;bad.At<IMAGE_NT_HEADERS32>(0x80).OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress=0x3000;
     CHECK(!FindImportSlot(bad.Base(),"XINPUT1_3.dll",3,nullptr));}
    {Synthetic bad;bad.At<IMAGE_DOS_HEADER>(0).e_magic=0;CHECK(!FindImportSlot(bad.Base(),"XINPUT1_3.dll",3,nullptr));}
    std::puts("PASS: import slots by name (live loader) and ordinal (synthetic), case-insensitive DLL, bounds refusals");
    return 0;
}
