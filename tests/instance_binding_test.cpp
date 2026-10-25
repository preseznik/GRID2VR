#include "instance_capacity.h"
#include "instance_capacity_policy.h"
#include <windows.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <vector>

#define CHECK(x) do {if(!(x)){std::printf("line %d: %s\n",__LINE__,#x);return 1;}} while(false)
using namespace grid2vr::instancecapacity;
using Microsoft::WRL::ComPtr;

int wmain(int argc,wchar_t** argv) {
    CHECK((argc==2 || argc==3) && sizeof(uintptr_t)==4);
    const auto style=argc==3?static_cast<uint32_t>(std::wcstoul(argv[2],nullptr,10)):3u;
    CHECK(style>=2 && style<=4);
    const std::filesystem::path folder=argv[1];std::filesystem::create_directories(folder);
    for(const auto* name:{L"instance-capacity-observe.request",L"instance-capacity-expanded.request",
        L"tracked-visibility.request",L"flicker-visibility-only.request",L"flicker-unattended.request"})
        std::ofstream(folder/name).put('\n');
    auto* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x1000000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    CHECK(image);const auto base=reinterpret_cast<uintptr_t>(image);
    const auto write=[](uintptr_t address,auto value){std::memcpy(reinterpret_cast<void*>(address),&value,sizeof(value));};
    constexpr uint8_t slot[]{0x83,0x7f,0x28,0,0x8d,0x77,0x28};
    constexpr uint8_t store[]{0xff,0xd2,0x89,0x06,0xc7,0x47,0x2c,0,0,0,0};
    constexpr uint8_t owner[]{0x89,0x48,0x04,0xc7,0};
    constexpr uint8_t gpu[]{0x8b,0x55,0x0c,0x5f,0x89,0x73,0x0c,0x5e,0x89,0x53,0x08};
    std::memcpy(image+0x93fd8b,slot,sizeof(slot));std::memcpy(image+0x93fda0,store,sizeof(store));
    std::memcpy(image+0x93b1b9,owner,sizeof(owner));write(base+0x93b1be,base+0xd49628);
    std::memcpy(image+0x9498d0,gpu,sizeof(gpu));CHECK(Install(base,folder));
    const auto mainObject=base+0x10000,manager=base+0x20000,allocator=base+0x21000,lanes=base+0x22000;
    CHECK(ReadyToBind(mainObject)==BindReadiness::Waiting);
    write(mainObject+0xd4,manager);
    // Exact reproduced early-start case: a manager exists, allocator/style do not.
    CHECK(ReadyToBind(mainObject)==BindReadiness::Waiting && !Bind(mainObject));
    write(manager+0xf8,allocator);CHECK(ReadyToBind(mainObject)==BindReadiness::Waiting);
    write(manager+0xb58,style);write(manager+0xb34,10u);
    auto* header=reinterpret_cast<uint32_t*>(allocator);
    header[0]=1;header[3]=Stride;header[4]=static_cast<uint32_t>(lanes);header[5]=LaneCount;
    CHECK(ReadyToBind(mainObject)==BindReadiness::Waiting);
    std::array<uint32_t,LaneCount> capacity{};CHECK(ExpectedCapacities(style,10,true,capacity));
    for(unsigned i=0;i<LaneCount;++i)reinterpret_cast<uint32_t*>(lanes+16*i)[2]=capacity[i];
    CHECK(ReadyToBind(mainObject)==BindReadiness::Waiting);
    for(unsigned i=0;i<LaneCount;++i)if(capacity[i]) {
        auto* state=reinterpret_cast<uint32_t*>(lanes+16*i);
        state[0]=static_cast<uint32_t>(base+0x23000+0x100*i);state[1]=static_cast<uint32_t>(base+0x27000+0x40*i);
    }
    CHECK(ReadyToBind(mainObject)==BindReadiness::Waiting);
    for(unsigned i=0;i<LaneCount;++i)if(capacity[i]) {
        auto* resource=reinterpret_cast<uint32_t*>(base+0x23000+0x100*i);
        resource[0]=static_cast<uint32_t>(base+0xd4aec8);resource[14]=capacity[i]*Stride;
    }
    CHECK(ReadyToBind(mainObject)==BindReadiness::Waiting);
    for(unsigned i=0;i<LaneCount;++i)if(capacity[i]) {
        auto* resource=reinterpret_cast<uint32_t*>(base+0x23000+0x100*i);
        resource[10]=static_cast<uint32_t>(base+0x25000+0x100*i);
        auto* wrapper=reinterpret_cast<uint32_t*>(resource[10]);
        wrapper[0]=static_cast<uint32_t>(base+0xd49628);wrapper[1]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(resource));
        wrapper[3]=capacity[i]*Stride;
    }
    CHECK(ReadyToBind(mainObject)==BindReadiness::Waiting);
    const auto d3d11=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    CHECK(d3d11);
    const auto createDevice=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(d3d11,"D3D11CreateDevice"));
    CHECK(createDevice);
    ComPtr<ID3D11Device> device;
    CHECK(SUCCEEDED(createDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,nullptr)));
    // Reproduce the installed run: 229 other observations, 15 valid instance
    // lanes, then enough unrelated buffers to exhaust the old 256-event prefix.
    D3D11_BUFFER_DESC other{};other.ByteWidth=40;other.Usage=D3D11_USAGE_DYNAMIC;
    other.BindFlags=D3D11_BIND_VERTEX_BUFFER;other.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    std::vector<ComPtr<ID3D11Buffer>> unrelated;
    const auto addOther=[&]() {
        ComPtr<ID3D11Buffer> buffer;
        if(FAILED(device->CreateBuffer(&other,nullptr,&buffer)))return false;
        BufferCreated(&other,S_OK,buffer.Get(),base+0x9498d0);
        unrelated.push_back(std::move(buffer));return true;
    };
    for(unsigned i=0;i<229;++i)CHECK(addOther());
    std::array<ComPtr<ID3D11Buffer>,LaneCount> buffers;
    std::array<D3D11_BUFFER_DESC,LaneCount> descriptions{};
    for(unsigned i=0;i<LaneCount;++i)if(capacity[i]) {
        auto& desc=descriptions[i];desc.ByteWidth=capacity[i]*Stride;desc.Usage=D3D11_USAGE_DYNAMIC;
        desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        CHECK(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&buffers[i])));
        reinterpret_cast<uint32_t*>(base+0x25000+0x100*i)[2]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buffers[i].Get()));
    }
    CHECK(ReadyToBind(mainObject)==BindReadiness::Waiting); // GPU observations must arrive too.
    for(unsigned i=0;i<LaneCount;++i)if(capacity[i])BufferCreated(&descriptions[i],S_OK,buffers[i].Get(),base+0x9498d0);
    CHECK(ReadyToBind(mainObject)==BindReadiness::Ready && Bind(mainObject));
    for(unsigned i=0;i<13;++i)CHECK(addOther());
    CHECK(ReadyToBind(mainObject)==BindReadiness::Ready && Bind(mainObject));
    const auto logBytes=std::filesystem::file_size(folder/L"instance-capacity.jsonl");
    std::vector<uint8_t> before(image+0x10000,image+0x28000);
    for(unsigned i=0;i<20;++i)CHECK(ReadyToBind(mainObject)==BindReadiness::Ready);
    CHECK(std::filesystem::file_size(folder/L"instance-capacity.jsonl")==logBytes);
    CHECK(!std::memcmp(before.data(),image+0x10000,before.size())); // Quiet and read-only.
    auto* lane0=reinterpret_cast<uint32_t*>(lanes);
    auto* resource0=reinterpret_cast<uint32_t*>(base+0x23000);
    auto* wrapper0=reinterpret_cast<uint32_t*>(base+0x25000);
    --lane0[2];CHECK(ReadyToBind(mainObject)==BindReadiness::Refused && !Bind(mainObject));++lane0[2];
    ++wrapper0[1];CHECK(ReadyToBind(mainObject)==BindReadiness::Refused);--wrapper0[1];
    ++resource0[0];CHECK(ReadyToBind(mainObject)==BindReadiness::Refused);--resource0[0];
    write(manager+0xb34,11u);CHECK(ReadyToBind(mainObject)==BindReadiness::Refused);write(manager+0xb34,10u);
    const auto buffer=wrapper0[2];wrapper0[2]=0;CHECK(ReadyToBind(mainObject)==BindReadiness::Waiting && !Bind(mainObject));wrapper0[2]=buffer;
    const auto resource=lane0[0];lane0[0]=0;CHECK(ReadyToBind(mainObject)==BindReadiness::Waiting);lane0[0]=resource;
    CHECK(ReadyToBind(mainObject)==BindReadiness::Ready && Bind(mainObject));
    // Required buffers created after the bounded log fills must still qualify.
    ComPtr<ID3D11Buffer> replacement;
    CHECK(SUCCEEDED(device->CreateBuffer(&descriptions[0],nullptr,&replacement)));
    wrapper0[2]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(replacement.Get()));
    BufferCreated(&descriptions[0],S_OK,replacement.Get(),base+0x9498d1);
    CHECK(ReadyToBind(mainObject)==BindReadiness::Waiting); // Wrong caller remains unobserved.
    BufferCreated(&descriptions[0],S_OK,replacement.Get(),base+0x9498d0);
    CHECK(ReadyToBind(mainObject)==BindReadiness::Ready && Bind(mainObject));
    replacement.Reset();
    CHECK(ReadyToBind(mainObject)==BindReadiness::Waiting); // No stale GPU address accepted.
    CHECK(SUCCEEDED(device->CreateBuffer(&descriptions[0],nullptr,&replacement)));
    wrapper0[2]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(replacement.Get()));
    auto wrongSize=descriptions[0];wrongSize.ByteWidth+=Stride;
    BufferCreated(&wrongSize,S_OK,replacement.Get(),base+0x9498d0);
    CHECK(ReadyToBind(mainObject)==BindReadiness::Refused && !Bind(mainObject));
    replacement.Reset();wrapper0[2]=buffer;
    CHECK(ReadyToBind(mainObject)==BindReadiness::Ready && Bind(mainObject));
    BufferCreated(&descriptions[0],S_OK,buffers[0].Get(),base+0x9498d0);
    CHECK(ReadyToBind(mainObject)==BindReadiness::Refused && !Bind(mainObject)); // Ambiguous GPU owner.
    Disable();VirtualFree(image,0,MEM_RELEASE);
    std::puts("Binding survives log overflow, qualifies new live buffers, refuses dead/ambiguous/malformed owners, and performs no game writes or readiness log spam.");
    return 0;
}
