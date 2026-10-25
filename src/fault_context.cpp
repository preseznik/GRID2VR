#include "fault_context.h"
#include <cstdint>
#include <cstdio>
#include <cstdarg>

namespace grid2vr {
namespace {
struct Writer {
    HANDLE file;
    unsigned remaining{faultContextByteLimit};
    void Line(const char* format,...) {
        if (!remaining) return;
        char line[1024]{};
        va_list args;va_start(args,format);
        const int count=vsnprintf_s(line,sizeof(line),_TRUNCATE,format,args);va_end(args);
        const auto size=count<0?sizeof(line)-1:static_cast<size_t>(count);
        const auto bytes=static_cast<DWORD>(size<remaining?size:remaining);
        DWORD written{};
        WriteFile(file,line,bytes,&written,nullptr);remaining-=bytes;
    }
    void Address(const char* label,unsigned index,uintptr_t address) {
        MEMORY_BASIC_INFORMATION region{};wchar_t module[MAX_PATH]{};
        const bool mapped=VirtualQuery(reinterpret_cast<void*>(address),&region,sizeof(region))!=0;
        const bool image=mapped && region.Type==MEM_IMAGE;
        if (image) GetModuleFileNameW(static_cast<HMODULE>(region.AllocationBase),module,MAX_PATH);
        Line("%s[%u]=%08zx allocation=%p offset=%zx state=%lx protect=%lx module=%ls\r\n",
            label,index,address,region.AllocationBase,image?address-reinterpret_cast<uintptr_t>(region.AllocationBase):0,
            region.State,region.Protect,module);
    }
};
bool Word(uintptr_t address,uintptr_t& value) {
    MEMORY_BASIC_INFORMATION region{};
    if (address>UINTPTR_MAX-sizeof(value) || !VirtualQuery(reinterpret_cast<void*>(address),&region,sizeof(region)) ||
        region.State!=MEM_COMMIT || (region.Protect&(PAGE_NOACCESS|PAGE_GUARD)) ||
        static_cast<uint64_t>(address)+sizeof(value)>reinterpret_cast<uintptr_t>(region.BaseAddress)+static_cast<uint64_t>(region.RegionSize)) return false;
    SIZE_T read{};
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&value,sizeof(value),&read) && read==sizeof(value);
}
void Words(Writer& out,const char* label,uintptr_t address,unsigned count) {
    for (unsigned i=0;i<count && out.remaining;++i) {
        uintptr_t value{};
        const auto offset=i*sizeof(value);
        if (offset>UINTPTR_MAX-address) { out.Line("%s[%u]=address_overflow\r\n",label,i);break; }
        const auto next=address+offset;
        if (!Word(next,value)) {
            out.Line("%s[%u]=unreadable address=%zx\r\n",label,i,next);break;
        }
        out.Address(label,i,value);
    }
}
}
void WriteFaultContext(HANDLE file,const CONTEXT& context) {
    if (file==INVALID_HANDLE_VALUE || !file) return;
    Writer out{file};
    out.Line("fault-context begin tid=%lu eip=%08lx esp=%08lx ebp=%08lx\r\n",GetCurrentThreadId(),context.Eip,context.Esp,context.Ebp);
    out.Address("instruction",0,context.Eip);
    // Raw stack candidates, not a claimed unwind. The first word identifies
    // the return address when an indirect call jumps to an invalid target.
    Words(out,"stack",context.Esp,128);
    const uintptr_t registers[]{context.Eax,context.Ebx,context.Ecx,context.Edx,context.Esi,context.Edi,context.Ebp};
    const char* names[]{"eax","ebx","ecx","edx","esi","edi","ebp"};
    for (unsigned i=0;i<7;++i) {
        out.Address(names[i],0,registers[i]);
        Words(out,names[i],registers[i],8);
    }
    out.Line("fault-context end\r\n");
    FlushFileBuffers(file);
}
}
