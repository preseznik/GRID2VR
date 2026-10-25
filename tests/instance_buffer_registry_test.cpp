#include "instance_buffer_registry.h"
#include <windows.h>
#include <wrl/client.h>
#include <cstdio>
#include <thread>
#include <vector>

#define CHECK(x) do {if(!(x)){std::printf("line %d: %s\n",__LINE__,#x);return 1;}} while(false)
using namespace grid2vr::instancecapacity;
using Microsoft::WRL::ComPtr;

int main() {
    const auto library=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    CHECK(library);
    const auto createDevice=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(library,"D3D11CreateDevice"));
    CHECK(createDevice);
    ComPtr<ID3D11Device> device;
    CHECK(SUCCEEDED(createDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,nullptr)));
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=40;desc.Usage=D3D11_USAGE_DYNAMIC;
    desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    const auto evidence=[&](ID3D11Buffer* buffer) {
        return BufferEvidence{reinterpret_cast<uintptr_t>(buffer),40,40,desc.BindFlags,uint32_t(desc.Usage),desc.CPUAccessFlags,S_OK};
    };
    BufferRegistry registry;
    ComPtr<ID3D11Buffer> held;
    CHECK(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&held)) && registry.Observe(held.Get(),evidence(held.Get())));
    // More allocations than the entire live bound: retiring resources must free
    // their metadata, without holding GPU buffers alive or confusing reused addresses.
    for(unsigned i=0;i<20000;++i) {
        ComPtr<ID3D11Buffer> temporary;CHECK(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&temporary)));
        const auto address=reinterpret_cast<uintptr_t>(temporary.Get());
        CHECK(registry.Observe(temporary.Get(),evidence(temporary.Get())));
        temporary.Reset();BufferEvidence old{};
        CHECK(!registry.Find(address,old) && registry.Stats().live==1);
    }
    std::atomic<bool> workerOk{true};
    std::thread worker([&] {
        for(unsigned i=0;i<1000;++i) {
            ComPtr<ID3D11Buffer> temporary;
            if(FAILED(device->CreateBuffer(&desc,nullptr,&temporary)) || !registry.Observe(temporary.Get(),evidence(temporary.Get())))workerOk=false;
        }
    });
    bool readsOk=true;
    for(unsigned i=0;i<1000;++i) {
        BufferEvidence record{};
        readsOk=readsOk && registry.Find(reinterpret_cast<uintptr_t>(held.Get()),record) && record.observations==1;
    }
    worker.join();CHECK(workerOk && readsOk && registry.Stats().live==1);
    std::vector<ComPtr<ID3D11Buffer>> many;
    for(size_t i=1;i<BufferRegistry::MaxLiveBuffers;++i) {
        ComPtr<ID3D11Buffer> buffer;CHECK(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&buffer)));
        CHECK(registry.Observe(buffer.Get(),evidence(buffer.Get())));many.push_back(std::move(buffer));
    }
    CHECK(registry.Stats().live==BufferRegistry::MaxLiveBuffers);
    ComPtr<ID3D11Buffer> overflow;CHECK(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&overflow)));
    CHECK(!registry.Observe(overflow.Get(),evidence(overflow.Get())));
    BufferEvidence record{};
    CHECK(!registry.Find(reinterpret_cast<uintptr_t>(overflow.Get()),record));
    CHECK(registry.Find(reinterpret_cast<uintptr_t>(held.Get()),record) && record.observations==1);
    CHECK(registry.Stats().failures==1 && registry.Stats().peak==BufferRegistry::MaxLiveBuffers);
    many.clear();overflow.Reset();CHECK(registry.Stats().live==1);
    CHECK(!registry.Observe(held.Get(),evidence(held.Get())));
    CHECK(registry.Find(reinterpret_cast<uintptr_t>(held.Get()),record) && record.observations==2);
    held.Reset();CHECK(registry.Stats().live==0);
    CHECK(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&held)) && registry.Observe(held.Get(),evidence(held.Get())));
    registry.Reset();CHECK(registry.Stats().live==0);
    // Replacing a previous registry's tag must not deadlock or erase the new owner.
    CHECK(registry.Observe(held.Get(),evidence(held.Get())) && registry.Stats().live==1);
    held.Reset();CHECK(registry.Stats().live==0);
    {
        BufferRegistry temporaryRegistry;
        CHECK(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&held)));
        CHECK(temporaryRegistry.Observe(held.Get(),evidence(held.Get())));
    }
    held.Reset(); // Tag state remains valid even when the registry owner has gone.
    std::puts("Live GPU evidence survives churn, retires on destruction, rejects ambiguity, and stays bounded without retaining GPU resources.");
    return 0;
}
