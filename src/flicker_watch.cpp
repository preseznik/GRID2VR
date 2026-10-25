#include "flicker_watch.h"
#include <wrl/client.h>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace grid2vr::flickerwatch {
namespace {
using Microsoft::WRL::ComPtr;
constexpr unsigned MaxDraws=24,MaxAccepted=180,MaxScenes=240,MaxObjects=4096;
constexpr size_t MaxLogBytes=16*1024*1024;
constexpr GUID uploadId{0x8b64f037,0xa29c,0x4d25,{0xbd,0x73,0x3a,0xa8,0x2d,0x8e,0x51,0xf2}};
struct UploadStamp {
    uint64_t scene{},generation{};
    UINT bytes{};
    std::array<uint8_t,256> prefix{};
};
struct Observation {
    ComPtr<ID3D11Query> query;
    unsigned pass{},ordinal{},drawOrdinal{},object{};
    const char* status{"pending"};
};
struct Frame {
    uint64_t scene{};
    std::array<unsigned,3> candidates{},draws{};
    std::array<unsigned,3> flagCandidates{},genericCandidates{};
    std::vector<Observation> observations;
};
std::ofstream log;
std::map<std::string,unsigned> identities;
std::deque<Frame> pending;
Frame current;
ComPtr<ID3D11DeviceContext> queryContext;
size_t inFlight=SIZE_MAX,logBytes{};
unsigned completed{},accepted{};
uint64_t generation{};
DWORD owner{};
bool enabled{},frameOpen{},logTruncated{};

void Line(const std::string& value) {
    if(!log || logTruncated)return;
    if(logBytes+value.size()+1>MaxLogBytes-256) {
        log << "{\"type\":\"truncated\",\"reason\":\"log_budget\"}\n";
        logTruncated=true;return;
    }
    log << value << '\n';logBytes+=value.size()+1;
}
bool Selected(const char* kind,std::span<const UINT> args,uint64_t vs,uint64_t ps) {
    if(!kind || args.empty() || !args[0])return false;
    const bool indexed=!strcmp(kind,"Indexed") && args.size()==3;
    const bool instanced=!strcmp(kind,"IndexedInstanced") && args.size()==5;
    if(!indexed && !instanced)return false;
    if(vs==0x5365b368abb08738ull && ps==0xaa19112331ae0872ull)return true;
    bool knownIndices=false;
    switch(args[0]) {case 6:case 12:case 36:case 54:case 72:case 126:knownIndices=true;break;}
    return knownIndices && ps==0x938d132c1aa95cbeull &&
        ((vs==0xd946f27a1c896b78ull && indexed) || (vs==0x13dd94df551eb6d9ull && instanced));
}
uint32_t Bits(float value) {uint32_t bits{};memcpy(&bits,&value,4);return bits;}
void ModelIdentity(std::ostream& s,ID3D11DeviceContext* c,uint64_t vs) {
    const UINT offset=vs==0x5365b368abb08738ull?176:vs==0xd946f27a1c896b78ull?144:UINT_MAX;
    const char* status="unsupported_shader";bool known=false;UploadStamp stamp{};
    if(offset!=UINT_MAX) {
        ComPtr<ID3D11Buffer> cb;c->VSGetConstantBuffers(0,1,&cb);
        if(!cb)status="no_constant_buffer";
        else {
            UINT bytes=sizeof(stamp);
            if(FAILED(cb->GetPrivateData(uploadId,&bytes,&stamp)) || bytes!=sizeof(stamp))status="unobserved";
            else if(stamp.scene!=current.scene || stamp.generation!=generation)status="stale";
            else if(stamp.bytes>stamp.prefix.size() || stamp.bytes<offset+64)status="short";
            else {status="current_cpu_upload";known=true;}
        }
    }
    s << ",\"modelKnown\":" << (known?"true":"false") << ",\"modelHex\":";
    if(!known)s << "null";
    else {
        constexpr char hex[]="0123456789abcdef";s << '"';
        for(UINT i=offset;i<offset+64;++i)s << hex[stamp.prefix[i]>>4] << hex[stamp.prefix[i]&15];
        s << '"';
    }
    s << ",\"modelStatus\":\"" << status << '"';
}
unsigned Identity(ID3D11DeviceContext* c,unsigned slot,const char* kind,std::span<const UINT> args,
                  uint64_t vs,uint64_t ps,std::array<uintptr_t,2> source) {
    ComPtr<ID3D11Buffer> vertex,index;UINT stride{},offset{},indexOffset{};DXGI_FORMAT format{};
    c->IAGetVertexBuffers(0,1,&vertex,&stride,&offset);c->IAGetIndexBuffer(&index,&format,&indexOffset);
    ComPtr<ID3D11ShaderResourceView> textureView;c->PSGetShaderResources(0,1,&textureView);
    ComPtr<ID3D11Resource> texture;if(textureView)textureView->GetResource(&texture);
    ComPtr<ID3D11RenderTargetView> targetView;ComPtr<ID3D11DepthStencilView> depthView;
    c->OMGetRenderTargets(1,&targetView,&depthView);
    ComPtr<ID3D11Resource> target,depth;
    if(targetView)targetView->GetResource(&target);if(depthView)depthView->GetResource(&depth);
    UINT count=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    std::array<D3D11_VIEWPORT,D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
    c->RSGetViewports(&count,viewports.data());
    std::ostringstream s;s << "\"slot\":" << slot << ",\"kind\":\"" << kind << "\",\"vs\":" << vs << ",\"ps\":" << ps
        << ",\"source\":[" << source[0] << ',' << source[1] << "],\"args\":[";
    for(size_t i=0;i<args.size();++i) {if(i)s<<',';s<<args[i];}
    s << "],\"geometry\":[" << uintptr_t(vertex.Get()) << ',' << stride << ',' << offset << ',' << uintptr_t(index.Get())
      << ',' << format << ',' << indexOffset << "],\"texture\":" << uintptr_t(texture.Get())
      << ",\"target\":" << uintptr_t(target.Get()) << ",\"depth\":" << uintptr_t(depth.Get());
    ModelIdentity(s,c,vs);
    ID3D11Buffer* streams[3]{};UINT strides[3]{},offsets[3]{};
    c->IAGetVertexBuffers(1,3,streams,strides,offsets);s << ",\"instanceStreams\":[";
    for(unsigned i=0;i<3;++i) {
        ComPtr<ID3D11Buffer> stream;stream.Attach(streams[i]);if(i)s<<',';
        s << '[' << i+1 << ',' << uintptr_t(stream.Get()) << ',' << strides[i] << ',' << offsets[i] << ']';
    }
    s << "],\"viewportBits\":[";
    for(UINT i=0;i<count && i<viewports.size();++i) {
        if(i)s<<',';const auto& v=viewports[i];s << '[' << Bits(v.TopLeftX) << ',' << Bits(v.TopLeftY) << ','
            << Bits(v.Width) << ',' << Bits(v.Height) << ',' << Bits(v.MinDepth) << ',' << Bits(v.MaxDepth) << ']';
    }
    s << ']';const auto value=s.str();const auto found=identities.find(value);
    if(found!=identities.end())return found->second;
    if(identities.size()>=MaxObjects)return 0;
    const auto id=unsigned(identities.size()+1);identities.emplace(value,id);
    Line("{\"type\":\"object\",\"id\":"+std::to_string(id)+','+value+'}');return id;
}
void EndInFlight(const char* status) {
    if(inFlight==SIZE_MAX)return;
    auto& observation=current.observations[inFlight];
    if(queryContext && observation.query)queryContext->End(observation.query.Get());
    observation.status=status;inFlight=SIZE_MAX;queryContext.Reset();
}
void Resolve(ID3D11DeviceContext* c,Frame& frame,const char* stopped=nullptr) {
    for(auto& observation:frame.observations) {
        UINT64 samples{};HRESULT result=S_FALSE;const char* status=observation.status;
        if(stopped)status=stopped;
        else if(observation.query && !strcmp(status,"pending")) {
            // Exactly one nonblocking read. S_FALSE is missing evidence, never zero samples.
            result=c?c->GetData(observation.query.Get(),&samples,sizeof(samples),D3D11_ASYNC_GETDATA_DONOTFLUSH):E_POINTER;
            status=result==S_OK?"ok":result==S_FALSE?"pending":"unavailable";
        }
        std::ostringstream s;s << "{\"type\":\"sample\",\"scene\":" << frame.scene << ",\"pass\":" << observation.pass
            << ",\"ordinal\":" << observation.ordinal << ",\"drawOrdinal\":" << observation.drawOrdinal
            << ",\"object\":" << observation.object << ",\"status\":\"" << status << '"';
        if(!strcmp(status,"ok"))s << ",\"samples\":" << samples;
        else if(!strcmp(status,"unavailable"))s << ",\"hresult\":" << static_cast<int32_t>(result);
        s << '}';Line(s.str());observation.query.Reset();
    }
}
void End(ID3D11DeviceContext* c,const char* reason) {
    EndInFlight("missing_after_draw");
    if(frameOpen)Resolve(c,current,"incomplete_scene");
    for(auto& frame:pending)Resolve(c,frame,c?nullptr:"stopped");
    pending.clear();current={};identities.clear();frameOpen=false;enabled=false;
    // The reserved tail ensures truncation still ends with an explicit bounded receipt.
    if(log)log << "{\"type\":\"end\",\"scenes\":" << completed << ",\"accepted\":" << accepted
        << ",\"reason\":\"" << reason << "\",\"truncated\":" << (logTruncated?"true":"false") << "}\n";
    log.close();
}
}
bool Active() {return enabled && owner==GetCurrentThreadId();}
bool Start(const std::filesystem::path& folder) {
    if(enabled)return false;
    current={};pending.clear();identities.clear();queryContext.Reset();inFlight=SIZE_MAX;
    completed=accepted=0;logBytes=0;frameOpen=logTruncated=false;owner=GetCurrentThreadId();
    ++generation;
    log.clear();log.open(folder/L"flicker-watch.jsonl",std::ios::binary|std::ios::trunc);
    if(!log)return false;
    enabled=true;
    Line("{\"type\":\"begin\",\"version\":1,\"acceptedLimit\":180,\"sceneLimit\":240,\"drawsPerPass\":24,\"genericPerPass\":23,\"flagsPerPass\":1,\"genericIndexCounts\":[6,12,36,54,72,126],\"pendingFrames\":3,\"identityLimit\":4096,\"maxBytes\":16777216,\"scope\":\"passive_draw_occlusion_samples_not_final_object_visibility\",\"identityScope\":\"binding_identity_not_resource_contents_or_model_location\",\"modelScope\":\"fresh_observed_cpu_model_when_known_not_gpu_contents_proof\"}");
    log.flush();return true;
}
void Begin(uint64_t scene) {
    if(!Active())return;
    if(frameOpen) {End(nullptr,"incomplete_scene");return;}
    current={};current.scene=scene;frameOpen=true;
}
void Upload(ID3D11Resource* resource,const void* data,UINT bytes) {
    if(!Active() || !frameOpen || !resource)return;
    ComPtr<ID3D11Buffer> buffer;
    if(FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer))))return;
    D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);
    if(!(desc.BindFlags&D3D11_BIND_CONSTANT_BUFFER))return;
    UploadStamp stamp{};stamp.scene=current.scene;stamp.generation=generation;
    if(data && bytes<=desc.ByteWidth) {
        stamp.bytes=bytes<stamp.prefix.size()?bytes:UINT(stamp.prefix.size());
        if(stamp.bytes)memcpy(stamp.prefix.data(),data,stamp.bytes);
    }
    // The stamp owns just a bounded prefix, not the mapped pointer or full buffer.
    buffer->SetPrivateData(uploadId,sizeof(stamp),&stamp);
}
void Draw(ID3D11DeviceContext* c,unsigned pass,unsigned slot,const char* kind,std::span<const UINT> args,
          uint64_t vs,uint64_t ps,std::array<uintptr_t,2> source) {
    if(!Active() || !frameOpen || pass>=3 || logTruncated)return;
    EndInFlight("missing_after_draw");++current.draws[pass];
    if(slot!=6 || !Selected(kind,args,vs,ps))return;
    const auto ordinal=++current.candidates[pass];
    // Reserve one query for the flag even when many generic boards precede it.
    if(vs==0x5365b368abb08738ull) {if(++current.flagCandidates[pass]>1)return;}
    else if(++current.genericCandidates[pass]>MaxDraws-1)return;
    Observation observation;observation.pass=pass;observation.ordinal=ordinal;observation.drawOrdinal=current.draws[pass];
    if(!c || c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)observation.status="context";
    else {
        observation.object=Identity(c,slot,kind,args,vs,ps,source);
        if(!observation.object)observation.status="identity_budget";
        else {
            ComPtr<ID3D11Device> device;c->GetDevice(&device);
            const D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION,0};
            if(!device || FAILED(device->CreateQuery(&desc,&observation.query)))observation.status="query_creation";
        }
    }
    current.observations.push_back(std::move(observation));
    auto& saved=current.observations.back();
    if(saved.query) {inFlight=current.observations.size()-1;queryContext=c;c->Begin(saved.query.Get());}
}
void AfterDraw(ID3D11DeviceContext* c) {
    if(!Active() || inFlight==SIZE_MAX)return;
    EndInFlight(c==queryContext.Get()?"pending":"context_mismatch");
}
void Complete(ID3D11DeviceContext* c,uint64_t scene,bool sceneAccepted) {
    if(!Active())return;
    if(!frameOpen || current.scene!=scene) {End(c,"scene_mismatch");return;}
    EndInFlight("missing_after_draw");++completed;if(sceneAccepted)++accepted;
    std::ostringstream s;s << "{\"type\":\"scene\",\"scene\":" << scene << ",\"accepted\":" << (sceneAccepted?"true":"false")
        << ",\"candidates\":[" << current.candidates[0] << ',' << current.candidates[1] << ',' << current.candidates[2]
        << "],\"draws\":[" << current.draws[0] << ',' << current.draws[1] << ',' << current.draws[2] << "],\"truncated\":[";
    for(unsigned i=0;i<3;++i) {if(i)s<<',';s << (current.genericCandidates[i]>MaxDraws-1 || current.flagCandidates[i]>1?"true":"false");}s << "]}";Line(s.str());
    pending.push_back(std::move(current));current={};frameOpen=false;
    if(pending.size()>=3) {Resolve(c,pending.front());pending.pop_front();}
    if(logTruncated)End(c,"log_budget");
    else if(accepted>=MaxAccepted)End(c,"complete");
    else if(completed>=MaxScenes)End(c,"scene_budget");
    else log.flush();
}
void Stop() {if(Active())End(nullptr,"stopped");}
}
