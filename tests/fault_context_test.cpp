#include "fault_context.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

int main(int argc,char** argv) {
    if (argc!=2) return 1;
    auto* page=static_cast<DWORD*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if (!page) return 2;
    const auto function=reinterpret_cast<DWORD>(&grid2vr::WriteFaultContext);
    for (unsigned i=0;i<1024;++i) page[i]=function;
    HANDLE file=CreateFileA(argv[1],GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return 3;
    CONTEXT context{};
    context.Eip=0xdededede;context.Esp=reinterpret_cast<DWORD>(page);
    context.Ecx=context.Esp;context.Ebp=context.Esp;
    context.Eax=context.Ebx=context.Edx=context.Esi=context.Edi=0xdededede;
    const auto original=context;
    grid2vr::WriteFaultContext(file,context);
    DWORD bytes=GetFileSize(file,nullptr);
    if (!bytes || bytes>grid2vr::faultContextByteLimit || memcmp(&context,&original,sizeof(context))) return 4;
    CloseHandle(file);
    std::ifstream input(argv[1]);
    std::string text((std::istreambuf_iterator<char>(input)),{});input.close();
    char expected[64]{};sprintf_s(expected,"stack[0]=%08lx",function);
    if (text.find(expected)==std::string::npos || text.find("fault_context_test.exe")==std::string::npos ||
        text.find("eax[0]=unreadable")==std::string::npos) return 5;
    file=CreateFileA(argv[1],GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return 6;
    DWORD previous{};
    if (!VirtualProtect(page,4096,PAGE_READWRITE|PAGE_GUARD,&previous)) return 7;
    grid2vr::WriteFaultContext(file,context);
    MEMORY_BASIC_INFORMATION region{};VirtualQuery(page,&region,sizeof(region));
    if (!(region.Protect&PAGE_GUARD)) return 8; // Reporting must not consume a guard page.
    context.Esp=0xfffffffe; // Integer wrap and unreadable addresses remain bounded.
    grid2vr::WriteFaultContext(file,context);
    if (GetFileSize(file,nullptr)>2*grid2vr::faultContextByteLimit) return 9;
    CloseHandle(file);
    input.open(argv[1]);text.assign(std::istreambuf_iterator<char>(input),{});input.close();
    if (text.find("stack[0]=unreadable")==std::string::npos || text.find("address=fffffffe")==std::string::npos) return 10;
    VirtualFree(page,0,MEM_RELEASE);
    puts("Fault capture retains return-address candidates, tolerates invalid memory, preserves guards/context, and stays bounded.");
}
