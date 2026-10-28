#include "spotlight_witness.h"
#include "engine_trace.h"
#include <wrl/client.h>
#include <atomic>
#include <cstring>
#include <fstream>
#include <sstream>

namespace grid2vr::spotlightwitness {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID uploadId{0xf65438d5,0xd027,0x4ab9,{0xa3,0x39,0xad,0xee,0x58,0x48,0xd4,0x26}};
constexpr unsigned MaxResources=256;
constexpr unsigned MaxTargetResources=256;
constexpr size_t MaxLogBytes=512*1024;
struct UploadStamp {
    uint64_t generation{},serial{},scene{},frame{};
    DWORD thread{};unsigned pass=3;UINT bytes{};bool targeted{};
    std::array<uint8_t,464> data{};
};
std::atomic_bool enabled{};
std::atomic_uint resources{};
std::atomic_uint64_t serial{};
uint64_t generation{};
DWORD owner{};
Selection selection;
struct Pending {uint64_t scene{};bool uploadsPresent=true;std::array<std::string,3> rows;};
std::array<Pending,Selection::MaxGroups> pending;
unsigned targetResources{};
std::ofstream log;
size_t written{};
void Hex(std::ostream& out,const void* data,size_t size) {
    constexpr char digits[]="0123456789abcdef";
    const auto* bytes=static_cast<const uint8_t*>(data);
    out << '"';
    for (size_t i=0;i<size;++i) out << digits[bytes[i]>>4] << digits[bytes[i]&15];
    out << '"';
}
void Hash(std::ostream& out,uint64_t value) {
    const auto flags=out.flags();out << '"' << std::hex << value << '"';out.flags(flags);
}
bool Buffer(std::ostream& out,ID3D11DeviceContext* context,bool pixel,unsigned slot,UINT expected) {
    ComPtr<ID3D11Buffer> buffer;
    if (pixel) context->PSGetConstantBuffers(slot,1,&buffer);
    else context->VSGetConstantBuffers(slot,1,&buffer);
    out << "{\"buffer\":";Hash(out,reinterpret_cast<uintptr_t>(buffer.Get()));
    if (!buffer) {out << ",\"status\":\"unbound\"}";return false;}
    D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);
    const bool layout=expected ? desc.ByteWidth==expected : (desc.ByteWidth==320 || desc.ByteWidth==368);
    out << ",\"bytes\":" << desc.ByteWidth << ",\"layout\":" << (layout?"true":"false");
    UploadStamp stamp{};UINT bytes=sizeof(stamp);
    if (FAILED(buffer->GetPrivateData(uploadId,&bytes,&stamp)) || bytes!=sizeof(stamp) ||
        stamp.generation!=generation || stamp.bytes!=desc.ByteWidth || !TracksSize(stamp.bytes)) {
        // A known draw reserves future CPU uploads independently of the small
        // generic cache, which ordinary model buffers may have filled first.
        if (layout && TracksSize(desc.ByteWidth) && targetResources<MaxTargetResources &&
            (stamp.generation!=generation || !stamp.targeted)) {
            stamp={};stamp.generation=generation;stamp.targeted=true;
            if(SUCCEEDED(buffer->SetPrivateData(uploadId,sizeof(stamp),&stamp)))++targetResources;
        }
        out << ",\"status\":\"cpu_upload_missing\"}";return false;
    }
    out << ",\"status\":\"last_observed_cpu_upload\",\"serial\":" << stamp.serial
        << ",\"uploadScene\":" << stamp.scene << ",\"uploadFrame\":" << stamp.frame
        << ",\"uploadPass\":" << stamp.pass << ",\"uploadThread\":" << stamp.thread << ",\"hex\":";
    Hex(out,stamp.data.data(),stamp.bytes);out << '}';return layout;
}
}
void Configure(const std::filesystem::path& folder,bool requested) {
    enabled=false;log.close();log.clear();selection={};pending={};written=0;resources=0;serial=0;targetResources=0;
    ++generation;owner=GetCurrentThreadId();
    if (!requested) return;
    log.open(folder/L"spotlight-witness.jsonl",std::ios::trunc);
    if (!log) return;
    constexpr char header[]="{\"type\":\"scope\",\"schema\":1,\"stagesPerVariant\":4,\"maxCompleteGroups\":56,\"maxDraws\":168,\"maxTripleAttemptsPerKey\":32,\"maxCachedResources\":256,\"maxTargetResources\":256,\"maxLogBytes\":524288,\"source\":\"last_observed_cpu_uploads\",\"gpuReadback\":false,\"renderChanges\":false,\"unobservedUploadApis\":\"not_covered\",\"incompleteGroups\":\"replaced_in_memory_not_written\",\"missingUploads\":\"first_missing_triple_then_first_populated_triple\"}\n";
    log << header;log.flush();written=sizeof(header)-1;enabled=log.good();
}
bool Enabled() {return enabled.load(std::memory_order_acquire);}
void Upload(ID3D11Resource* resource,const void* data,UINT bytes) {
    if (!Enabled() || !resource || !data || !TracksSize(bytes)) return;
    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer)))) return;
    D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);
    if (!(desc.BindFlags&D3D11_BIND_CONSTANT_BUFFER) || desc.ByteWidth!=bytes) return;
    UploadStamp previous{};UINT previousBytes=sizeof(previous);
    if (FAILED(buffer->GetPrivateData(uploadId,&previousBytes,&previous)) ||
        previousBytes!=sizeof(previous) || previous.generation!=generation) {
        auto count=resources.load(std::memory_order_relaxed);
        do {if(count>=MaxResources)return;}
        while(!resources.compare_exchange_weak(count,count+1,std::memory_order_relaxed));
    }
    UploadStamp stamp{};stamp.generation=generation;stamp.serial=serial.fetch_add(1,std::memory_order_relaxed)+1;
    stamp.targeted=previous.generation==generation && previous.targeted;
    stamp.thread=GetCurrentThreadId();stamp.bytes=bytes;
    if (stamp.thread==owner) {
        const auto scene=engine::CurrentScene();stamp.scene=scene.id;stamp.pass=scene.id?scene.pass:3;
        stamp.frame=engine::NativeCameraFrame();
    }
    memcpy(stamp.data.data(),data,bytes);
    buffer->SetPrivateData(uploadId,sizeof(stamp),&stamp);
}
void Draw(ID3D11DeviceContext* context,uint64_t scene,unsigned pass,unsigned slot,
          unsigned ordinal,const char* kind,std::span<const UINT> args,uint64_t vs,uint64_t ps) {
    // Unrelated draws do not allocate, query state or touch diagnostic storage.
    if (!Enabled() || !Selected(vs,ps) || GetCurrentThreadId()!=owner || !context ||
        context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || !kind || kind[0]=='C') return;
    auto* group=selection.Take(scene,pass,slot,vs,ps);if(!group)return;
    auto& set=pending[size_t(group-selection.groups.data())];
    if(set.scene!=scene) {set={};set.scene=scene;}
    std::ostringstream row;
    row << "{\"type\":\"draw\",\"scene\":" << scene << ",\"frame\":" << engine::NativeCameraFrame()
        << ",\"pass\":" << pass << ",\"slot\":" << slot << ",\"ordinal\":" << ordinal
        << ",\"vs\":";Hash(row,vs);row << ",\"ps\":";Hash(row,ps);
    row << ",\"stencil\":" << (vs==StencilVs?"true":"false") << ",\"args\":[";
    for (size_t i=0;i<args.size() && i<5;++i) {if(i)row << ',';row << args[i];}
    row << "],\"vs0\":";set.uploadsPresent&=Buffer(row,context,false,0,vs==StencilVs?128:192);
    row << ",\"vs3\":";const bool vs3=Buffer(row,context,false,3,0);
    if (vs!=StencilVs) {
        set.uploadsPresent&=vs3;
        row << ",\"ps0\":";set.uploadsPresent&=Buffer(row,context,true,0,PixelBufferSize(ps));
        row << ",\"ps3\":";set.uploadsPresent&=Buffer(row,context,true,3,0);
    }
    row << "}\n";set.rows[pass]=row.str();
    if(group->mask!=7)return;
    if(!Selection::Finish(*group,set.uploadsPresent)){set={};return;}
    size_t bytes=0;for(const auto& line:set.rows)bytes+=line.size();
    if (bytes>MaxLogBytes-written) {set={};return;}
    for(const auto& line:set.rows)log << line;
    log.flush();written+=bytes;set={};
    if (!log) enabled=false;
}
}
