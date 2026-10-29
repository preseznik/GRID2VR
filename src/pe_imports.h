#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace grid2vr {
// Locates the import-address-table slot for one import of a loaded Win32 image.
// ordinal!=0 selects an ordinal import, otherwise name. Every header, descriptor,
// thunk and name is bounds-checked against SizeOfImage; nothing outside the image
// is followed. Returns nullptr when the import is absent or the image malformed.
inline void** FindImportSlot(uintptr_t image,const char* dll,WORD ordinal,const char* name) noexcept {
    if(!image || !dll || (!ordinal && !name)) return nullptr;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<=0 || dos->e_lfanew>0x10000) return nullptr;
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(image+static_cast<uint32_t>(dos->e_lfanew));
    if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
       nt->OptionalHeader.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_IMPORT) return nullptr;
    const uint32_t size=nt->OptionalHeader.SizeOfImage;
    const auto inside=[size](uint32_t rva,uint32_t bytes) {return rva<size && bytes<=size-rva;};
    const auto string=[&](uint32_t rva,const char* expected) {
        const size_t length=std::strlen(expected)+1;
        return inside(rva,static_cast<uint32_t>(length)) && _strnicmp(reinterpret_cast<const char*>(image+rva),expected,length)==0;
    };
    const auto directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if(!directory.VirtualAddress || !inside(directory.VirtualAddress,directory.Size)) return nullptr;
    const auto count=directory.Size/sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for(uint32_t i=0;i<count;++i) {
        const auto* entry=reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image+directory.VirtualAddress)+i;
        if(!entry->Name) break;
        if(!string(entry->Name,dll)) continue;
        const uint32_t lookup=entry->OriginalFirstThunk?entry->OriginalFirstThunk:entry->FirstThunk;
        for(uint32_t j=0;j<0x10000;++j) {
            const uint32_t at=lookup+j*sizeof(IMAGE_THUNK_DATA32),slot=entry->FirstThunk+j*sizeof(IMAGE_THUNK_DATA32);
            if(!inside(at,sizeof(IMAGE_THUNK_DATA32)) || !inside(slot,sizeof(IMAGE_THUNK_DATA32))) return nullptr;
            const auto value=reinterpret_cast<const IMAGE_THUNK_DATA32*>(image+at)->u1.Ordinal;
            if(!value) break;
            const bool byOrdinal=(value&IMAGE_ORDINAL_FLAG32)!=0;
            if(ordinal ? byOrdinal && IMAGE_ORDINAL32(value)==ordinal
                       : !byOrdinal && string(value+offsetof(IMAGE_IMPORT_BY_NAME,Name),name))
                return reinterpret_cast<void**>(image+slot);
        }
    }
    return nullptr;
}
}
