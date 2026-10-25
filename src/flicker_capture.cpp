#include "flicker_capture.h"
#include "headset_visibility_candidate.h"
#include "flicker_fields.h"
#include "flicker_pixels.h"
#include "flicker_watch.h"
#include "sign_visibility.h"
#include "pixels.h"
#include "texture_digest.h"
#include <wrl/client.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <set>
#include <vector>

namespace grid2vr::flicker {
namespace {
using Microsoft::WRL::ComPtr;
constexpr unsigned MaxScenes=8,MaxDraws=4096;
constexpr uint64_t MaxImageBytes=96ull*1024*1024,MaxLogBytes=32ull*1024*1024;
constexpr GUID uploadId{0x13d09b43,0xcc65,0x4127,{0xb2,0x38,0x52,0x8b,0x43,0xf9,0xaa,0x16}};
constexpr GUID layoutId{0x13d09b44,0xcc65,0x4127,{0xb2,0x38,0x52,0x8b,0x43,0xf9,0xaa,0x16}};
struct Element { char name[32]{};UINT index{},format{},slot{},offset{},classification{},step{}; };
struct Layout { UINT count{};std::array<Element,32> elements{}; };
struct Samples { ComPtr<ID3D11Query> query;unsigned pass{},ordinal{}; };
std::vector<Samples> queries;
ComPtr<ID3D11Query> pendingQuery;
std::set<uintptr_t> loggedLayouts;
uint64_t inputBytes{};
uint64_t instanceBytes{};
std::array<unsigned,3> instanceReads{};
unsigned nativeQueries{};
uint64_t mipBudget{};
std::array<std::set<uintptr_t>,3> sampledAlpha;
std::array<bool,3> colourSeen{};
PixelWitness colourPixels;
unsigned colourPass=3,colourOrdinal{};
struct MeshPixels { PixelWitness pixels;unsigned ordinal{}; };
std::array<MeshPixels,4> meshPixels;
unsigned meshPass=3,meshCount{},pendingMesh=4;
struct UploadStamp {
    uint64_t hash{},scene{};UINT bytes{};std::array<uint32_t,4> firstWords{};
    // A bounded prefix of an observed CPU upload, not a GPU-content readback.
    std::array<uint8_t,512> prefix{};
};
std::filesystem::path output;
std::ofstream log;
DWORD owner{};
bool enabled{},armed{},used{},active{},logTruncated{},compact{},watchRequested{},visibilityOnly{};
bool signTarget{},waitingForSign{},signSeen{};
unsigned signIndex{};
uint64_t current{},imageBytes{},logBytes{};
unsigned scenes{};
std::array<unsigned,3> draws{};
std::array<bool,3> truncated{};
uint64_t Hash(const void* data,size_t bytes) {
    uint64_t value=14695981039346656037ull;
    for (size_t i=0;i<bytes;++i) value=(value^static_cast<const unsigned char*>(data)[i])*1099511628211ull;
    return value;
}
void ObserveSign(ID3D11DeviceContext* c,unsigned pass,unsigned slot,const char* kind,uint64_t vs,uint64_t ps,std::array<uintptr_t,2> source) {
    if(!signTarget || (!waitingForSign && !active) || GetCurrentThreadId()!=owner || pass || slot!=6 || strcmp(kind,"Indexed") ||
        vs!=0xd946f27a1c896b78ull || ps!=0x938d132c1aa95cbeull)return;
    ComPtr<ID3D11Buffer> model,camera;c->VSGetConstantBuffers(0,1,&model);c->VSGetConstantBuffers(3,1,&camera);
    UploadStamp m{},v{};UINT mb=sizeof(m),vb=sizeof(v);
    if(!model || !camera || FAILED(model->GetPrivateData(uploadId,&mb,&m)) || FAILED(camera->GetPrivateData(uploadId,&vb,&v)) ||
        mb!=sizeof(m) || vb!=sizeof(v) || m.scene!=current || v.scene!=current || m.bytes<208 || v.bytes<128)return;
    std::array<float,3> position{};std::array<float,16> vp{};
    memcpy(position.data(),m.prefix.data()+192,12);memcpy(vp.data(),v.prefix.data()+64,64);
    constexpr std::array<std::array<float,3>,2> targets{{{89.756325f,26.448267f,-379.425201f},{102.956688f,23.536945f,-363.218719f}}};
    for(unsigned i=0;i<targets.size();++i) {
        bool match=true;for(unsigned axis=0;axis<3;++axis)match&=std::isfinite(position[axis]) && std::abs(position[axis]-targets[i][axis])<.01f;
        if(!match)continue;
        signvisibility::Remember(i,source[0],source[1]);
        std::array<float,4> clip{};
        for(unsigned j=0;j<4;++j)clip[j]=position[0]*vp[j]+position[1]*vp[4+j]+position[2]*vp[8+j]+vp[12+j];
        // The earlier disappearance happened while these static boards remained
        // in front of the camera. Align the passive burst to that approach;
        // CPU stamps select timing only, never establish GPU transform contents.
        bool finite=true;for(float value:clip)finite&=std::isfinite(value);
        if(finite && clip[3]>=40 && clip[3]<=75 && std::abs(clip[0])<=clip[3] && std::abs(clip[1])<=clip[3] &&
            clip[2]>=0 && clip[2]<=clip[3]) {signSeen=true;signIndex=i;}
    }
}
bool Trackside(uint64_t vs) {
    switch(vs) {
    case 0x3a1b0384014b0834ull:case 0x1c547389c33a53f8ull:case 0xc90ec9a844625ee6ull:
    case 0x5365b368abb08738ull:case 0x8b6c766bb894ac89ull:case 0xd946f27a1c896b78ull:
    case 0xec328dc7892bc754ull:case 0x9f5f7d2c416172e4ull:case 0xfbb75a032d043c42ull:return true;
    default:return false;
    }
}
void HexBytes(std::ostream& s,const void* data,size_t bytes) {
    constexpr char hex[]="0123456789abcdef";const auto* p=static_cast<const uint8_t*>(data);
    s << '"';for(size_t i=0;i<bytes;++i)s << hex[p[i]>>4] << hex[p[i]&15];s << '"';
}
// Copy actual bound data, not a cached Map stamp. Restricted to small flag
// buffers and a 2 MiB burst budget; no files or retained GPU copies accumulate.
std::vector<uint8_t> BufferInput(std::ostream& s,ID3D11DeviceContext* c,ID3D11Buffer* buffer,UINT offset=0,UINT length=UINT_MAX,uint64_t* instanceBudget=nullptr) {
    s << "{\"resource\":" << uintptr_t(buffer);
    if(!buffer) {s << ",\"status\":\"unbound\"}";return {};}
    D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);
    const bool range=length!=UINT_MAX;
    if(!range)length=desc.ByteWidth;
    s << ",\"bytes\":" << length;
    if(range)s << ",\"resourceBytes\":" << desc.ByteWidth << ",\"offset\":" << offset;
    auto& usedBytes=instanceBudget?*instanceBudget:inputBytes;
    const uint64_t limit=instanceBudget?512*1024:2*1024*1024;
    if(!length || length>8192 || offset>desc.ByteWidth || length>desc.ByteWidth-offset || usedBytes+length>limit) {s << ",\"status\":\"budget\"}";return {};}
    if(c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) {s << ",\"status\":\"context\"}";return {};}
    usedBytes+=length;
    ComPtr<ID3D11Device> device;buffer->GetDevice(&device);ComPtr<ID3D11Buffer> copy;
    desc.ByteWidth=length;desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(FAILED(device->CreateBuffer(&desc,nullptr,&copy))) {s << ",\"status\":\"allocation\"}";return {};}
    if(range) {const D3D11_BOX box{offset,0,0,offset+length,1,1};c->CopySubresourceRegion(copy.Get(),0,0,0,0,buffer,0,&box);}
    else c->CopyResource(copy.Get(),buffer);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(c->Map(copy.Get(),0,D3D11_MAP_READ,0,&mapped))) {s << ",\"status\":\"map\"}";return {};}
    std::vector<uint8_t> bytes(length);memcpy(bytes.data(),mapped.pData,length);
    s << ",\"status\":\"ok\",\"hash\":" << Hash(bytes.data(),length) << ",\"hex\":";
    HexBytes(s,bytes.data(),length);s << '}';c->Unmap(copy.Get(),0);return bytes;
}
void ObjectInputs(std::ostream& s,ID3D11DeviceContext* context,uint64_t shader) {
    const auto fields=ObjectFields(shader);
    if(fields.empty())return;
    ComPtr<ID3D11Buffer> buffer;context->VSGetConstantBuffers(0,1,&buffer);
    UploadStamp stamp{};UINT bytes=sizeof(stamp);
    const bool known=buffer && SUCCEEDED(buffer->GetPrivateData(uploadId,&bytes,&stamp)) && bytes==sizeof(stamp);
    s << ",\"objectInputs\":{\"known\":" << known << ",\"scene\":" << stamp.scene
      << ",\"uploadHash\":" << stamp.hash << ",\"fields\":{";
    constexpr char hex[]="0123456789abcdef";
    bool comma=false;
    for(const auto& field:fields) {
        if(comma)s<<',';comma=true;
        s << '"' << field.name << "\":";
        if(!known || field.offset+field.bytes>stamp.bytes || field.offset+field.bytes>stamp.prefix.size()) {
            s << "null";continue;
        }
        s << '"';
        for(unsigned i=field.offset;i<field.offset+field.bytes;++i)
            s << hex[stamp.prefix[i]>>4] << hex[stamp.prefix[i]&15];
        s << '"';
    }
    s << "}}";
}
void Line(const std::string& line) {
    if (!log || logTruncated) return;
    // Leave space for an explicit truncation record instead of silently rotating.
    if (logBytes+line.size()+1>MaxLogBytes-128) {
        log << "{\"type\":\"truncated\",\"reason\":\"log_budget\"}\n";logTruncated=true;
    } else {log << line << '\n';logBytes+=line.size()+1;}
}
void AlphaContents(ID3D11DeviceContext* c,unsigned pass,unsigned ordinal,uint64_t vs,uint64_t ps) {
    if(ps!=0x59bc124b8c63bdaaull && ps!=0xaa19112331ae0872ull && ps!=0x938d132c1aa95cbeull)return;
    if(!Trackside(vs))return;
    const bool flag=vs==0xc90ec9a844625ee6ull || vs==0x5365b368abb08738ull;
    ComPtr<ID3D11ShaderResourceView> view;c->PSGetShaderResources(0,1,&view);
    ComPtr<ID3D11Resource> resource;if(view)view->GetResource(&resource);
    // Always observe the exact flag draws afresh. Other alpha materials sample
    // at most four distinct resources per pass; never treat that as full coverage.
    auto& sampled=sampledAlpha[pass];
    if(!flag && (sampled.size()>=4 || !sampled.insert(uintptr_t(resource.Get())).second))return;
    const auto digest=DigestTexture(c,resource.Get(),mipBudget,true);
    std::ostringstream row;row << "{\"type\":\"alphaTexture\",\"scene\":" << current << ",\"pass\":" << pass
      << ",\"ordinal\":" << ordinal << ",\"view\":" << uintptr_t(view.Get()) << ",\"resource\":" << uintptr_t(resource.Get())
      << ",\"remainingBudget\":" << mipBudget << ",\"digest\":";
    WriteTextureDigest(row,digest);row << '}';Line(row.str());
}
void LayoutInput(std::ostream& s,ID3D11DeviceContext* c) {
    ComPtr<ID3D11InputLayout> layout;c->IAGetInputLayout(&layout);
    const auto id=uintptr_t(layout.Get());s << ",\"inputLayout\":" << id;
    if(!layout || loggedLayouts.size()>=256 || !loggedLayouts.insert(id).second)return;
    Layout data{};UINT bytes=sizeof(data);
    const bool known=SUCCEEDED(layout->GetPrivateData(layoutId,&bytes,&data)) && bytes==sizeof(data) && data.count<=32;
    std::ostringstream row;row << "{\"type\":\"layout\",\"id\":" << id << ",\"known\":" << known << ",\"elements\":[";
    for(UINT i=0;known && i<data.count;++i) {
        if(i)row<<',';const auto& e=data.elements[i];
        row << "[\"" << e.name << "\"," << e.index << ',' << e.format << ',' << e.slot << ',' << e.offset << ',' << e.classification << ',' << e.step << ']';
    }row << "]}";Line(row.str());
}
void FlagInputs(std::ostream& s,ID3D11DeviceContext* c) {
    ComPtr<ID3D11Buffer> cb,vertex,index;UINT stride{},offset{},indexOffset{};DXGI_FORMAT format{};
    s << ",\"flagInputs\":{\"cb0\":";c->VSGetConstantBuffers(0,1,&cb);BufferInput(s,c,cb.Get());
    cb.Reset();c->VSGetConstantBuffers(3,1,&cb);s << ",\"cb3\":";BufferInput(s,c,cb.Get());
    c->IAGetVertexBuffers(0,1,&vertex,&stride,&offset);s << ",\"vertex0\":";BufferInput(s,c,vertex.Get());
    c->IAGetIndexBuffer(&index,&format,&indexOffset);s << ",\"index\":";BufferInput(s,c,index.Get());
    s << '}';
}
void IndexedInputs(std::ostream& s,ID3D11DeviceContext* c,std::span<const UINT> args,uint64_t* instanceBudget=nullptr) {
    ComPtr<ID3D11Buffer> vertex,index;UINT stride{},offset{},indexOffset{};DXGI_FORMAT format{};
    c->IAGetVertexBuffers(0,1,&vertex,&stride,&offset);c->IAGetIndexBuffer(&index,&format,&indexOffset);
    const UINT indexBytes=format==DXGI_FORMAT_R16_UINT?2:format==DXGI_FORMAT_R32_UINT?4:0;
    const uint64_t indexStart=uint64_t(indexOffset)+uint64_t(args[1])*indexBytes;
    std::vector<uint8_t> indices;
    s << ",\"index\":";
    if(indexBytes && indexStart<=UINT_MAX)indices=BufferInput(s,c,index.Get(),UINT(indexStart),args[0]*indexBytes,instanceBudget);
    else s << "{\"status\":\"unsupported_index\"}";
    int64_t first=INT64_MAX,last=-1;
    if(!indices.empty())for(UINT i=0;i<args[0];++i) {
        UINT value{};memcpy(&value,indices.data()+i*indexBytes,indexBytes);
        const auto actual=int64_t(value)+int32_t(args[2]);first=(std::min)(first,actual);last=(std::max)(last,actual);
    }
    s << ",\"vertexFirst\":" << (first==INT64_MAX?-1:first) << ",\"vertex0\":";
    const uint64_t begin=first>=0 && first!=INT64_MAX?uint64_t(offset)+uint64_t(first)*stride:UINT64_MAX;
    const uint64_t bytes=first>=0 && last>=first?uint64_t(last-first+1)*stride:0;
    if(begin<=UINT_MAX && bytes && bytes<=8192)BufferInput(s,c,vertex.Get(),UINT(begin),UINT(bytes),instanceBudget);
    else s << "{\"status\":\"unsupported_range\"}";
}
void SmallMeshInputs(std::ostream& s,ID3D11DeviceContext* c,std::span<const UINT> args) {
    ComPtr<ID3D11Buffer> cb;
    s << ",\"meshInputs\":{\"cb0\":";c->VSGetConstantBuffers(0,1,&cb);BufferInput(s,c,cb.Get());
    cb.Reset();c->VSGetConstantBuffers(3,1,&cb);s << ",\"cb3\":";BufferInput(s,c,cb.Get());
    IndexedInputs(s,c,args);
    s << '}';
}
void InstanceInputs(std::ostream& s,ID3D11DeviceContext* c,unsigned pass,std::span<const UINT> args) {
    s << ",\"instanceInputs\":{";
    if(!args[1] || args[1]>64) {s << "\"status\":\"unsupported_count\"}";return;}
    if(instanceReads[pass]>=12) {s << "\"status\":\"draw_budget\"}";return;}
    ++instanceReads[pass];
    ComPtr<ID3D11InputLayout> layout;c->IAGetInputLayout(&layout);
    Layout data{};UINT size=sizeof(data);unsigned found=0;bool valid=true;
    if(!layout || FAILED(layout->GetPrivateData(layoutId,&size,&data)) || size!=sizeof(data) || data.count>32)valid=false;
    for(UINT i=0;valid && i<data.count;++i) {
        const auto& e=data.elements[i];
        if(strcmp(e.name,"TANGENT") || e.index<1 || e.index>4)continue;
        const UINT bit=1u<<(e.index-1),expectedFormat=e.index==4?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT;
        valid=!(found&bit) && e.slot==1 && e.offset==(e.index-1)*8 && e.format==expectedFormat &&
            e.classification==D3D11_INPUT_PER_INSTANCE_DATA && e.step==1;
        found|=bit;
    }
    if(!valid || found!=15) {s << "\"status\":\"unsupported_layout\"}";return;}
    ComPtr<ID3D11Buffer> stream;UINT stride{},offset{};c->IAGetVertexBuffers(1,1,&stream,&stride,&offset);
    const uint64_t start=uint64_t(offset)+uint64_t(args[4])*stride,bytes=uint64_t(args[1])*stride;
    if(!stream || stride<40 || stride>256 || start>UINT_MAX || bytes>8192) {
        s << "\"status\":\"unsupported_binding\"}";return;
    }
    s << "\"status\":\"ok\",\"cb3\":";
    ComPtr<ID3D11Buffer> cb;c->VSGetConstantBuffers(3,1,&cb);BufferInput(s,c,cb.Get(),0,UINT_MAX,&instanceBytes);
    const std::array<UINT,3> indexed{args[0],args[2],args[3]};IndexedInputs(s,c,indexed,&instanceBytes);
    // This fingerprint consumes only slot1 for instance transforms. Bound
    // slots2/3 are not shader inputs and must not be interpreted as transforms.
    s << ",\"streams\":[{\"slot\":1,\"firstElement\":" << args[4] << ",\"elementCount\":" << args[1]
      << ",\"stride\":" << stride << ",\"bindingOffset\":" << offset << ",\"stepRate\":1,\"buffer\":";
    BufferInput(s,c,stream.Get(),UINT(start),UINT(bytes),&instanceBytes);s << "}]}";
}
void AlphaSampling(std::ostream& s,ID3D11DeviceContext* c,uint64_t ps) {
    UINT slot{};
    switch(ps) {
    case 0x59bc124b8c63bdaaull:slot=3;break;
    case 0xaa19112331ae0872ull:slot=4;break;
    case 0x938d132c1aa95cbeull:slot=5;break;
    default:return;
    }
    auto bits=[](float value) { uint32_t word{};memcpy(&word,&value,4);return word; };
    ComPtr<ID3D11SamplerState> sampler;c->PSGetSamplers(slot,1,&sampler);
    D3D11_SAMPLER_DESC desc{};if(sampler)sampler->GetDesc(&desc);
    s << ",\"alphaSampling\":{\"slot\":" << slot << ",\"bound\":" << bool(sampler) << ",\"descriptor\":[";
    if(sampler) {
        s << desc.Filter << ',' << desc.AddressU << ',' << desc.AddressV << ',' << desc.AddressW << ','
          << bits(desc.MipLODBias) << ',' << desc.MaxAnisotropy << ',' << desc.ComparisonFunc;
        for(float value:desc.BorderColor)s << ',' << bits(value);
        s << ',' << bits(desc.MinLOD) << ',' << bits(desc.MaxLOD);
    }
    ComPtr<ID3D11ShaderResourceView> view;c->PSGetShaderResources(0,1,&view);
    ComPtr<ID3D11Resource> resource;D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    if(view) {view->GetResource(&resource);view->GetDesc(&vd);}
    s << "],\"view\":" << uintptr_t(view.Get()) << ",\"resource\":" << uintptr_t(resource.Get())
      << ",\"viewFormat\":" << vd.Format << ",\"viewDimension\":" << vd.ViewDimension;
    if(vd.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D)
        s << ",\"mostDetailedMip\":" << vd.Texture2D.MostDetailedMip << ",\"mipLevels\":" << vd.Texture2D.MipLevels;
    if(resource)s << ",\"resourceMinLodBits\":" << bits(c->GetResourceMinLOD(resource.Get()));
    ComPtr<ID3D11Texture2D> texture;
    s << ",\"texture\":[";
    if(resource && SUCCEEDED(resource.As(&texture))) {
        D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
        s << td.Width << ',' << td.Height << ',' << td.MipLevels << ',' << td.ArraySize << ',' << td.Format;
    }
    s << "]}";
}
void DrawSurface(std::ostream& s,ID3D11DeviceContext* c) {
    ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11DepthStencilView> depth;c->OMGetRenderTargets(1,&target,&depth);
    ComPtr<ID3D11Resource> resource;if(depth)depth->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;D3D11_TEXTURE2D_DESC desc{};
    if(resource && SUCCEEDED(resource.As(&texture)))texture->GetDesc(&desc);
    D3D11_DEPTH_STENCIL_VIEW_DESC view{};if(depth)depth->GetDesc(&view);
    s << ",\"surface\":[" << uintptr_t(target.Get()) << ',' << uintptr_t(depth.Get()) << ',' << uintptr_t(resource.Get())
      << ',' << view.Format << ',' << desc.Width << ',' << desc.Height << ',' << desc.SampleDesc.Count << ']';
    D3D11_VIEWPORT viewport{};UINT count=1;c->RSGetViewports(&count,&viewport);
    s << ",\"viewport\":[" << count << ',' << viewport.TopLeftX << ',' << viewport.TopLeftY << ',' << viewport.Width << ',' << viewport.Height << ',' << viewport.MinDepth << ',' << viewport.MaxDepth << ']';
    ComPtr<ID3D11RasterizerState> raster;c->RSGetState(&raster);D3D11_RASTERIZER_DESC rs{};
    if(raster)raster->GetDesc(&rs);
    s << ",\"raster\":[" << bool(raster) << ',' << rs.FillMode << ',' << rs.CullMode << ',' << rs.FrontCounterClockwise << ',' << rs.DepthBias
      << ',' << rs.DepthBiasClamp << ',' << rs.SlopeScaledDepthBias << ',' << rs.DepthClipEnable << ',' << rs.ScissorEnable << ',' << rs.MultisampleEnable << ']';
}
const char* Image(ID3D11DeviceContext* context,ID3D11Texture2D* texture,const std::string& name) {
    if (!texture) return "missing";
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    const bool bgra=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool rgb10=desc.Format==DXGI_FORMAT_R10G10B10A2_UNORM;
    const bool rgba=desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (!desc.Width || !desc.Height || desc.Width>4096 || desc.Height>4096 || desc.ArraySize!=1 ||
        desc.MipLevels!=1 || desc.SampleDesc.Count!=1 || (!bgra && !rgb10 && !rgba)) return "unsupported";
    const uint64_t bytes=uint64_t(desc.Width)*desc.Height*3+64;
    if (imageBytes+bytes>MaxImageBytes) return "budget";
    // Reserve attempted bytes too: failures cannot cause unbounded retries/files.
    imageBytes+=bytes;
    ComPtr<ID3D11Device> device;texture->GetDevice(&device);
    ComPtr<ID3D11Device> actual;context->GetDevice(&actual);
    if (device!=actual || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return "device";
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> copy;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&copy))) return "allocation";
    context->CopyResource(copy.Get(),texture);
    std::vector<char> row(desc.Width*3);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(copy.Get(),0,D3D11_MAP_READ,0,&mapped))) return "map";
    // Prepare storage before opening a file. No native binding/state is changed.
    std::ofstream file(output/name,std::ios::binary);
    file << "P6\n" << desc.Width << ' ' << desc.Height << "\n255\n";
    for (UINT y=0;y<desc.Height;++y) {
        const auto* pixels=static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch;
        for (UINT x=0;x<desc.Width;++x) {
            if (rgb10) {
                uint32_t packed{};memcpy(&packed,pixels+x*4,4);const auto rgb=Rgb10ToRgb8(packed);
                for (unsigned c=0;c<3;++c) row[x*3+c]=rgb[c];
            } else for (unsigned c=0;c<3;++c) row[x*3+c]=pixels[x*4+(bgra?2-c:c)];
        }
        file.write(row.data(),row.size());
    }
    context->Unmap(copy.Get(),0);file.close();return file.good()?"saved":"write";
}
}
void Configure(const std::filesystem::path& folder) {
    flickerwatch::Stop();
    if (log.is_open()) log.close();
    output=folder;owner=GetCurrentThreadId();armed=used=active=logTruncated=false;
    signTarget=waitingForSign=signSeen=false;
    current=imageBytes=logBytes=0;scenes=0;draws={};truncated={};
    queries.clear();pendingQuery.Reset();loggedLayouts.clear();inputBytes=instanceBytes=0;instanceReads={};nativeQueries=0;
    mipBudget=64ull*1024*1024;sampledAlpha={};colourSeen={};colourPixels.Reset();colourPass=3;
    for(auto& item:meshPixels)item.pixels.Reset();meshPass=3;meshCount=0;pendingMesh=4;
    std::error_code error;
    enabled=std::filesystem::is_regular_file(output/L"flicker-diagnostic.request",error);
    compact=enabled && std::filesystem::is_regular_file(output/L"flicker-compact.request",error);
    watchRequested=compact && std::filesystem::is_regular_file(output/L"flicker-watch.request",error);
    signTarget=compact && std::filesystem::is_regular_file(output/L"flicker-sign-target.request",error);
    visibilityOnly=std::filesystem::is_regular_file(output/L"flicker-visibility-only.request",error);
    if(visibilityOnly && (!compact || signTarget ||
        (!std::filesystem::is_regular_file(output/L"tracked-visibility.request",error) &&
         !std::filesystem::is_regular_file(output/L"native-visibility-control.request",error)) ||
        (!std::filesystem::is_regular_file(output/L"flicker-unattended.request",error) &&
         !HeadsetVisibilityCandidate(folder))))enabled=false;
    signvisibility::Configure(folder,signTarget || (compact &&
        (std::filesystem::exists(output/L"native-visibility-control.request",error) ||
         std::filesystem::exists(output/L"tracked-visibility.request",error))));
}
void Poll() {
    if (!enabled || used || GetCurrentThreadId()!=owner) return;
    std::error_code error;
    if (!std::filesystem::remove(output/L"flicker-capture.request",error)) return;
    used=true;log.open(output/L"flicker-draws.jsonl",std::ios::binary);
    waitingForSign=signTarget && log.good();armed=!signTarget && log.good();
    if(armed)signvisibility::Start();
    Line(std::string("{\"type\":\"start\",\"captureScope\":\"")+(visibilityOnly?"visibility":"draws")+
        "\",\"maxScenes\":8,\"maxDrawsPerPass\":"+std::to_string(visibilityOnly?UINT_MAX:MaxDraws)+
        ",\"maxImageBytes\":100663296,\"maxLogBytes\":33554432,\"wholeBufferStamps\":"+(compact?"false}":"true}"));
    log.flush();
}
void Begin(uint64_t scene) {
    if(flickerwatch::Active())flickerwatch::Begin(scene);
    if(waitingForSign && GetCurrentThreadId()==owner && scene) {current=scene;signSeen=false;return;}
    if (!armed || GetCurrentThreadId()!=owner || !scene || scene==current) return;
    if (active) Line("{\"type\":\"incomplete\",\"scene\":"+std::to_string(current)+"}");
    active=false;
    if (scenes>=MaxScenes) {armed=false;log.flush();return;}
    current=scene;++scenes;active=true;draws={};truncated={};instanceReads={};
    queries.clear();pendingQuery.Reset();
    sampledAlpha={};colourSeen={};colourPixels.Reset();colourPass=3;
    for(auto& item:meshPixels)item.pixels.Reset();meshPass=3;meshCount=0;pendingMesh=4;
    Line("{\"type\":\"scene\",\"scene\":"+std::to_string(scene)+",\"sample\":"+std::to_string(scenes)+"}");
}
bool Capturing(uint64_t scene) { return active && GetCurrentThreadId()==owner && (!scene || scene==current); }
bool Observing(uint64_t scene) {return Capturing(scene) || flickerwatch::Active() ||
    (waitingForSign && GetCurrentThreadId()==owner && (!scene || scene==current));}
void Upload(ID3D11Resource* resource,const void* data,UINT bytes) {
    if(flickerwatch::Active())flickerwatch::Upload(resource,data,bytes);
    if(visibilityOnly)return;
    if ((!Capturing() && !(waitingForSign && GetCurrentThreadId()==owner)) || !resource || !data || !bytes || bytes>65536) return;
    UploadStamp stamp{Hash(data,bytes),current,bytes};
    if(bytes>=sizeof(stamp.firstWords))memcpy(stamp.firstWords.data(),data,sizeof(stamp.firstWords));
    memcpy(stamp.prefix.data(),data,(std::min)(size_t(bytes),stamp.prefix.size()));
    resource->SetPrivateData(uploadId,sizeof(stamp),&stamp);
}
void RememberLayout(ID3D11InputLayout* layout,const D3D11_INPUT_ELEMENT_DESC* elements,UINT count) {
    if(!layout || !elements || count>32)return;
    Layout data{};data.count=count;
    for(UINT i=0;i<count;++i) {
        const auto& e=elements[i];auto& d=data.elements[i];
        if(!e.SemanticName || strlen(e.SemanticName)>=sizeof(d.name))return;
        // Semantic names in JSON must remain bounded identifiers.
        for(const char* p=e.SemanticName;*p;++p)if(!isalnum(static_cast<unsigned char>(*p)) && *p!='_')return;
        strcpy_s(d.name,e.SemanticName);d.index=e.SemanticIndex;d.format=e.Format;d.slot=e.InputSlot;
        d.offset=e.AlignedByteOffset;d.classification=e.InputSlotClass;d.step=e.InstanceDataStepRate;
    }layout->SetPrivateData(layoutId,sizeof(data),&data);
}
void Draw(ID3D11DeviceContext* c,unsigned pass,unsigned slot,const char* kind,std::span<const UINT> args,
          uint64_t vs,uint64_t ps,std::array<uintptr_t,2> source,std::array<uint32_t,12> nativeList) {
    if(flickerwatch::Active())flickerwatch::Draw(c,pass,slot,kind,args,vs,ps,source);
    ObserveSign(c,pass,slot,kind,vs,ps,source);
    if (!Capturing() || pass>=3 || args.size()>5 || logTruncated) return;
    // Route coverage needs exact totals and completed images, not thousands
    // of buffer/texture dumps. The later bounded moving-object watch is unchanged.
    if(visibilityOnly) {if(draws[pass]==UINT_MAX)truncated[pass]=true;else ++draws[pass];return;}
    if (draws[pass]>=MaxDraws) {truncated[pass]=true;return;}
    const bool smallMesh=compact && slot==6 && vs==0xd946f27a1c896b78ull && ps==0x938d132c1aa95cbeull &&
        !strcmp(kind,"Indexed") && args.size()==3 && (args[0]==6 || args[0]==12 || args[0]==36 || args[0]==54 || args[0]==72 || args[0]==126);
    std::ostringstream s;
    s << "{\"type\":\"draw\",\"scene\":" << current << ",\"pass\":" << pass << ",\"ordinal\":" << ++draws[pass]
      << ",\"slot\":" << slot << ",\"kind\":\"" << kind << "\",\"vs\":" << vs << ",\"ps\":" << ps
      << ",\"drawable\":" << source[0] << ",\"properties\":" << source[1] << ",\"args\":[";
    for (size_t i=0;i<args.size();++i) {if(i)s<<',';s<<args[i];}s << ']';
    // caller RVA, wrapper caller RVA, nine native arguments, global mask.
    // An empty array means provenance was unavailable, never an empty list.
    s << ",\"nativeList\":[";
    if(nativeList[0])for(size_t i=0;i<nativeList.size();++i){if(i)s<<',';s<<nativeList[i];}
    s << ']';
    ComPtr<ID3D11Buffer> vertex,index;UINT stride{},offset{},indexOffset{};DXGI_FORMAT format{};
    c->IAGetVertexBuffers(0,1,&vertex,&stride,&offset);c->IAGetIndexBuffer(&index,&format,&indexOffset);
    s << ",\"geometry\":[" << uintptr_t(vertex.Get()) << ',' << stride << ',' << offset << ',' << uintptr_t(index.Get()) << ',' << format << ',' << indexOffset << ']';
    // Instanced transforms can live outside stream zero. Record binding
    // identity, without claiming that a stable pointer proves stable contents.
    ID3D11Buffer* streams[32]{};UINT strides[32]{},offsets[32]{};
    c->IAGetVertexBuffers(0,32,streams,strides,offsets);s << ",\"instanceStreams\":[";
    bool comma=false;
    for(unsigned i=0;i<32;++i) {
        ComPtr<ID3D11Buffer> stream;stream.Attach(streams[i]);
        if(!i || !stream)continue;
        if(comma)s<<',';comma=true;
        s << '[' << i << ',' << uintptr_t(stream.Get()) << ',' << strides[i] << ',' << offsets[i] << ']';
    }s << ']';
    ComPtr<ID3D11Predicate> predicate;BOOL value{};c->GetPredication(&predicate,&value);
    s << ",\"predicate\":[" << uintptr_t(predicate.Get()) << ',' << value << ']';
    ComPtr<ID3D11RasterizerState> raster;ComPtr<ID3D11DepthStencilState> depth;ComPtr<ID3D11BlendState> blend;
    UINT reference{},mask{};FLOAT factors[4]{};c->RSGetState(&raster);c->OMGetDepthStencilState(&depth,&reference);c->OMGetBlendState(&blend,factors,&mask);
    s << ",\"pipeline\":[" << uintptr_t(raster.Get()) << ',' << uintptr_t(depth.Get()) << ',' << reference << ',' << uintptr_t(blend.Get()) << ',' << mask << ']';
    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    if(depth)depth->GetDesc(&depthDesc);
    else {depthDesc.DepthEnable=TRUE;depthDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;depthDesc.DepthFunc=D3D11_COMPARISON_LESS;}
    s << ",\"depthTest\":[" << depthDesc.DepthEnable << ',' << depthDesc.DepthWriteMask << ',' << depthDesc.DepthFunc << ']';
    // Reflected alpha-test shaders in the retained burst read PS CB5. This is
    // an observed upload, not a GPU read or proof the current shader uses it.
    ComPtr<ID3D11Buffer> alpha;c->PSGetConstantBuffers(5,1,&alpha);
    UploadStamp alphaStamp{};UINT alphaBytes=sizeof(alphaStamp);
    const bool alphaKnown=alpha && SUCCEEDED(alpha->GetPrivateData(uploadId,&alphaBytes,&alphaStamp)) && alphaBytes==sizeof(alphaStamp);
    s << ",\"pcb5\":[" << uintptr_t(alpha.Get()) << ',' << alphaKnown << ',' << alphaStamp.hash << ',' << alphaStamp.scene << ',' << alphaStamp.bytes;
    if(alphaKnown && alphaStamp.bytes==16)for(auto word:alphaStamp.firstWords)s << ',' << word;
    s << ']';
    ObjectInputs(s,c,vs);
    LayoutInput(s,c);
    if(compact)AlphaSampling(s,c,ps);
    if(compact && Trackside(vs)) {
        DrawSurface(s,c);
        if(vs==0xc90ec9a844625ee6ull || vs==0x5365b368abb08738ull)FlagInputs(s,c);
        if(smallMesh)SmallMeshInputs(s,c,args);
    }
    if(compact && slot==6 && vs==0x13dd94df551eb6d9ull && ps==0x938d132c1aa95cbeull &&
        !strcmp(kind,"IndexedInstanced") && args.size()==5 && (args[0]==6 || args[0]==30 || args[0]==126 || args[0]==210)) {
        DrawSurface(s,c);InstanceInputs(s,c,pass,args);
    }
    // Automated object-input captures retain exact selected fields and PS CB5.
    // Omit the eight redundant whole-buffer stamps to fit eight busy scenes in
    // the existing 32 MiB cap. The full diagnostic remains available unchanged.
    for (unsigned stage=0;!compact && stage<2;++stage) {
        ID3D11Buffer* buffers[4]{};
        if (stage) c->PSGetConstantBuffers(0,4,buffers);else c->VSGetConstantBuffers(0,4,buffers);
        s << (stage?",\"pcb\":[":",\"vcb\":[");
        for (unsigned i=0;i<4;++i) {
            ComPtr<ID3D11Buffer> buffer;buffer.Attach(buffers[i]);UploadStamp stamp{};UINT bytes=sizeof(stamp);
            const bool known=buffer && SUCCEEDED(buffer->GetPrivateData(uploadId,&bytes,&stamp)) && bytes==sizeof(stamp);
            if(i)s<<',';
            s << '[' << uintptr_t(buffer.Get()) << ',' << known << ',' << stamp.hash << ',' << stamp.scene << ',' << stamp.bytes << ']';
        }s << ']';
    }
    ID3D11ShaderResourceView* resources[8]{};c->PSGetShaderResources(0,8,resources);s << ",\"textures\":[";
    for (unsigned i=0;i<8;++i) {ComPtr<ID3D11ShaderResourceView> view;view.Attach(resources[i]);if(i)s<<',';s<<uintptr_t(view.Get());}
    s << "]}";Line(s.str());
    if(compact)AlphaContents(c,pass,draws[pass],vs,ps);
    if(compact && !colourSeen[pass] && vs==0x5365b368abb08738ull && ps==0xaa19112331ae0872ull) {
        colourSeen[pass]=true;colourPass=pass;colourOrdinal=draws[pass];colourPixels.Before(c);
    }
    // Fingerprinted placement candidates: the Intel bridge banner, two A-frame
    // boards and braking-marker index counts. Other draws retain bounded inputs.
    UploadStamp placement{};UINT placementBytes=sizeof(placement);ComPtr<ID3D11Buffer> modelBuffer;
    bool board=false;
    if(smallMesh && (args[0]==6 || args[0]==36 || args[0]==126)) {
        c->VSGetConstantBuffers(0,1,&modelBuffer);
        if(modelBuffer && SUCCEEDED(modelBuffer->GetPrivateData(uploadId,&placementBytes,&placement)) &&
            placementBytes==sizeof(placement) && placement.scene==current && placement.bytes>=208) {
            std::array<float,3> xyz{};memcpy(xyz.data(),placement.prefix.data()+192,12);
            const auto x=xyz[0];
            board=(args[0]!=6 && ((x>89.70f && x<89.81f) || (x>102.90f && x<103.01f))) ||
                (args[0]==6 && x>142.70f && x<142.74f && xyz[1]>29.85f && xyz[1]<29.90f && xyz[2]>-174.90f && xyz[2]<-174.85f);
        }
    }
    if(smallMesh && (args[0]==54 || args[0]==72 || board) && meshCount<meshPixels.size()) {
        meshPass=pass;pendingMesh=meshCount++;
        auto& item=meshPixels[pendingMesh];item.ordinal=draws[pass];item.pixels.Before(c);
    }
    // Bracket only the existing draw. Resolve after the image readback, without
    // spinning, flushing or substituting geometry/depth state.
    if(compact && Trackside(vs) && queries.size()<1536 && !pendingQuery) {
        ComPtr<ID3D11Device> device;c->GetDevice(&device);ComPtr<ID3D11Query> query;
        const D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION,0};
        if(SUCCEEDED(device->CreateQuery(&desc,&query))) {
            queries.push_back({query,pass,draws[pass]});pendingQuery=query;c->Begin(query.Get());
        }
    }
}
void AfterDraw(ID3D11DeviceContext* context) {
    if(flickerwatch::Active())flickerwatch::AfterDraw(context);
    if(!Capturing())return;
    if(pendingQuery) {context->End(pendingQuery.Get());pendingQuery.Reset();}
    if(colourPass<3)colourPixels.After(context);
    if(pendingMesh<meshPixels.size()) {meshPixels[pendingMesh].pixels.After(context);pendingMesh=4;}
}
void FinishPass(ID3D11DeviceContext* context,uint64_t scene,unsigned pass,ID3D11Texture2D* finalSource) {
    if(!Capturing(scene))return;
    if(colourPass==pass) {
        std::ostringstream row;row << "{\"type\":\"flagPixels\",\"scene\":" << scene << ",\"pass\":" << pass
          << ",\"ordinal\":" << colourOrdinal << ',' << colourPixels.Finish(context,finalSource) << '}';Line(row.str());
        colourPass=3;
    }
    if(meshPass==pass) {
        for(unsigned i=0;i<meshCount;++i) {
            auto& item=meshPixels[i];std::ostringstream row;
            row << "{\"type\":\"meshPixels\",\"scene\":" << scene << ",\"pass\":" << pass << ",\"ordinal\":" << item.ordinal
                << ',' << item.pixels.Finish(context,finalSource) << '}';Line(row.str());
        }
        meshPass=3;meshCount=0;pendingMesh=4;
    }
}
void DepthClear(ID3D11DepthStencilView* view,unsigned pass,UINT flags,FLOAT depth,UINT8 stencil) {
    if(!Capturing() || visibilityOnly || !compact || pass>=3)return;
    ComPtr<ID3D11Resource> resource;if(view)view->GetResource(&resource);
    std::ostringstream row;row << "{\"type\":\"clearDepth\",\"scene\":" << current << ",\"pass\":" << pass
        << ",\"afterOrdinal\":" << draws[pass] << ",\"view\":" << uintptr_t(view) << ",\"resource\":" << uintptr_t(resource.Get())
        << ",\"flags\":" << flags << ",\"depth\":" << depth << ",\"stencil\":" << unsigned(stencil) << '}';Line(row.str());
}
void NativeQuery(ID3D11Asynchronous* async,unsigned pass,const char* operation,uintptr_t caller,HRESULT result,const void* data,UINT bytes) {
    if(!Capturing() || visibilityOnly || !compact || !async || pass>=3 || nativeQueries>=2048)return;
    ++nativeQueries;ComPtr<ID3D11Query> query;D3D11_QUERY_DESC desc{};
    const bool known=SUCCEEDED(async->QueryInterface(IID_PPV_ARGS(&query)));if(known)query->GetDesc(&desc);
    std::ostringstream row;row << "{\"type\":\"nativeQuery\",\"scene\":" << current << ",\"pass\":" << pass
        << ",\"afterOrdinal\":" << draws[pass] << ",\"query\":" << uintptr_t(async) << ",\"kindKnown\":" << known
        << ",\"kind\":" << desc.Query << ",\"operation\":\"" << operation << "\",\"callerRva\":" << caller << ",\"hr\":" << result << ",\"bytes\":" << bytes;
    if(result==S_OK && data && bytes && bytes<=16) {row << ",\"hex\":";HexBytes(row,data,bytes);}
    row << '}';Line(row.str());
}
void Complete(ID3D11DeviceContext* context,uint64_t frame,uint64_t scene,bool accepted,const std::array<ID3D11Texture2D*,3>& images) {
    if(flickerwatch::Active())flickerwatch::Complete(context,scene,accepted);
    if(waitingForSign && GetCurrentThreadId()==owner && current==scene && accepted && signSeen) {
        waitingForSign=false;armed=true;
        signvisibility::Start();
        Line("{\"type\":\"signTrigger\",\"scene\":"+std::to_string(scene)+",\"target\":"+std::to_string(signIndex)+
            ",\"basis\":\"current_cpu_model_and_camera_timing_only\"}");log.flush();
    }
    if (!Capturing(scene)) return;
    std::ostringstream s;s << "{\"type\":\"complete\",\"frame\":" << frame << ",\"scene\":" << scene << ",\"accepted\":" << accepted << ",\"draws\":[";
    for (unsigned i=0;i<3;++i) {if(i)s<<',';s<<draws[i];}s << "],\"truncated\":[";
    for (unsigned i=0;i<3;++i) {if(i)s<<',';s<<truncated[i];}s << "]}";Line(s.str());
    // Rejected or partial eyes must never be presented as one matched image set.
    if (accepted && context && !logTruncated) for (unsigned eye=0;eye<3;++eye) {
        const auto name="stage-"+std::to_string(scene)+"-flicker-eye"+std::to_string(eye)+".ppm";
        const auto status=Image(context,images[eye],name);
        Line("{\"type\":\"image\",\"scene\":"+std::to_string(scene)+",\"eye\":"+std::to_string(eye)+",\"file\":\""+name+"\",\"status\":\""+status+"\"}");
    }
    if(context && !pendingQuery)for(const auto& item:queries) {
        UINT64 samples{};const auto hr=context->GetData(item.query.Get(),&samples,sizeof(samples),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        Line("{\"type\":\"samples\",\"scene\":"+std::to_string(scene)+",\"pass\":"+std::to_string(item.pass)+
            ",\"ordinal\":"+std::to_string(item.ordinal)+",\"hr\":"+std::to_string(hr)+",\"passed\":"+std::to_string(samples)+"}");
    }
    queries.clear();active=false;
    if (scenes==MaxScenes) {armed=false;signvisibility::Stop();Line("{\"type\":\"end\",\"scenes\":8,\"nativeQueryRecords\":"+std::to_string(nativeQueries)+",\"nativeQueryLimitReached\":"+(nativeQueries>=2048?"true":"false")+"}");}
    log.flush();
    if(scenes==MaxScenes && watchRequested) {watchRequested=false;flickerwatch::Start(output);}
}
}
