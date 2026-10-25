#include "sign_visibility.h"
#include "scene_views.h"
#include <iomanip>
#include "guarded_hooks.h"
#include "packet_snapshot.h"
#include "replay_packet.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

namespace grid2vr::signvisibility {
namespace {
constexpr unsigned MaxTargets=8,MaxSnapshots=256,MaxFrames=8,MaxMatches=64;
constexpr size_t MaxBytes=4*1024*1024;
struct Target {
    unsigned label{};uintptr_t drawable{},properties{};
    std::array<uint32_t,3> identity{};
    bool known{},propertiesChanged{};
};
std::filesystem::path output;
std::ofstream log;
std::vector<Target> targets;
std::set<uint64_t> frames;
DWORD owner{};
bool enabled{},started{},active{},truncated{},targetLimit{};
unsigned snapshots{},rejectedPhases{};
size_t logBytes{};
uint64_t viewFrame{};
SceneViews frozenPair{};
bool Owned() {return enabled && owner==GetCurrentThreadId();}
bool Address(uintptr_t base,size_t offset,size_t bytes,uintptr_t& address) {
    if(!base || base>UINT32_MAX || offset>UINT32_MAX-base)return false;
    address=base+offset;return bytes<=UINT32_MAX-address;
}
bool ReadAt(uintptr_t base,size_t offset,void* data,size_t bytes) {
    uintptr_t address{};
    return Address(base,offset,bytes,address) && ReadMemory(reinterpret_cast<const void*>(address),data,bytes);
}
bool Identity(uintptr_t drawable,std::array<uint32_t,3>& value) {
    return !(drawable&3) && ReadAt(drawable,0,&value[0],4) && ReadAt(drawable,0x2c,&value[1],4) &&
        ReadAt(drawable,0x30,&value[2],4) && value[0]!=0;
}
void Hex(std::ostream& out,const uint8_t* bytes,size_t count) {
    constexpr char digits[]="0123456789abcdef";
    out << '"';for(size_t i=0;i<count;++i)out << digits[bytes[i]>>4] << digits[bytes[i]&15];out << '"';
}
void Raw(std::ostream& out,uintptr_t base,size_t offset,size_t bytes,const char* refusal=nullptr) {
    out << "{\"base\":" << base << ",\"offset\":" << offset << ",\"bytes\":" << bytes;
    std::array<uint8_t,256> data{};
    const bool ok=!refusal && bytes<=data.size() && ReadAt(base,offset,data.data(),bytes);
    out << ",\"status\":\"" << (refusal?refusal:ok?"ok":base?"unreadable":"unbound") << '"';
    if(ok) {out << ",\"hex\":";Hex(out,data.data(),bytes);}
    out << '}';
}
void End(const char* reason) {
    if(!active)return;
    if(log)log << "{\"type\":\"end\",\"reason\":\"" << reason << "\",\"snapshots\":" << snapshots
        << ",\"frames\":" << frames.size() << ",\"targets\":" << targets.size()
        << ",\"targetLimitReached\":" << (targetLimit?"true":"false")
        << ",\"rejectedPhases\":" << rejectedPhases << ",\"truncated\":" << (truncated?"true":"false") << "}\n";
    log.close();active=false;
}
bool Line(const std::string& value) {
    if(!active || !log)return false;
    if(logBytes+value.size()+1>MaxBytes-512) {
        truncated=true;log << "{\"type\":\"truncated\",\"reason\":\"log_budget\"}\n";
        End("log_budget");return false;
    }
    log << value << '\n';logBytes+=value.size()+1;
    if(!log.good()) {End("write_failed");return false;}
    return true;
}
void TargetRecord(size_t index) {
    const auto& t=targets[index];std::ostringstream out;
    out << "{\"type\":\"target\",\"id\":" << index+1 << ",\"target\":" << t.label
        << ",\"drawable\":" << t.drawable << ",\"properties\":" << t.properties
        << ",\"identityKnown\":" << (t.known?"true":"false") << ",\"identity\":["
        << t.identity[0] << ',' << t.identity[1] << ',' << t.identity[2] << "]}";
    Line(out.str());
}
bool Phase(const char* phase,std::string& result) {
    if(!phase)return false;
    // Restrict JSON values to bounded identifiers, including invalid callers.
    for(unsigned i=0;i<48;++i) {
        char value{};
        if(!ReadAt(reinterpret_cast<uintptr_t>(phase),i,&value,1))return false;
        if(!value)return !result.empty();
        if(!((value>='a' && value<='z') || (value>='A' && value<='Z') ||
             (value>='0' && value<='9') || value=='_' || value=='-'))return false;
        result.push_back(value);
    }
    return false;
}
void Builder(std::ostream& out,uintptr_t mainObject) {
    uint32_t builder{};
    const bool known=ReadAt(mainObject,0xe4,&builder,4);
    out << "{\"known\":" << (known?"true":"false") << ",\"address\":";
    if(known)out<<builder;else out<<"null";
    out << ",\"raw\":";Raw(out,builder,0,0x6c);
    LazyListState state{};
    const bool complete=known && ReadLazyListState(builder,[](uintptr_t address,void* data,size_t bytes) {
        return ReadAt(address,0,data,bytes);
    },state);
    out << ",\"lazyStateComplete\":" << (complete?"true":"false") << ",\"trees\":[";
    for(unsigned i=0;i<3;++i) {
        if(i)out<<',';
        uint32_t tree{},root{};
        const bool treeKnown=ReadAt(builder,0x34+4*i,&tree,4);
        const bool rootKnown=treeKnown && ReadAt(tree,4,&root,4);
        out << "{\"offset\":" << 0x34+4*i << ",\"treeKnown\":" << (treeKnown?"true":"false") << ",\"tree\":";
        if(treeKnown)out<<tree;else out<<"null";
        out << ",\"rootKnown\":" << (rootKnown?"true":"false") << ",\"root\":";
        if(rootKnown)out<<root;else out<<"null";out<<'}';
    }
    out << "],\"gates\":[";
    constexpr std::array<size_t,3> offsets{0x2a18,0x2a1c,0x320};
    for(unsigned i=0;i<offsets.size();++i) {
        if(i)out<<',';
        uint32_t value{};const unsigned bytes=i==2?4:1;
        const bool readable=ReadAt(builder,offsets[i],&value,bytes);
        out << "{\"offset\":" << offsets[i] << ",\"bytes\":" << bytes << ",\"known\":" << (readable?"true":"false") << ",\"value\":";
        if(readable)out<<value;else out<<"null";out<<'}';
    }
    out << "]}";
}
void SelectionHeaders(std::ostream& out,uintptr_t mainObject) {
    out << '[';
    for(unsigned i=0;i<2;++i) {
        if(i)out<<',';
        const size_t offset=i?0x230:0x188;
        std::array<uint32_t,3> words{};
        const bool known=ReadAt(mainObject,offset,words.data(),sizeof(words));
        out << "{\"offset\":" << offset << ",\"status\":\"" << (known?"ok":mainObject?"unreadable":"unbound")
            << "\",\"count\":";
        if(known)out<<int32_t(words[0]);else out<<"null";
        out << ",\"records\":";
        if(known)out<<words[1];else out<<"null";
        out << ",\"capacity\":";
        if(known)out<<int32_t(words[2]);else out<<"null";
        out << ",\"capacityInterpretation\":\"generic_selection_header\"";
        if(known) {out << ",\"hex\":";Hex(out,reinterpret_cast<const uint8_t*>(words.data()),sizeof(words));}
        out << '}';
    }
    out << ']';
}
void Allocator(std::ostream& out,uintptr_t mainObject) {
    constexpr unsigned MaxLanes=16;
    uint32_t manager{},allocator{};
    const bool managerKnown=ReadAt(mainObject,0xd4,&manager,4);
    const bool allocatorKnown=managerKnown && ReadAt(manager,0xf8,&allocator,4);
    std::array<uint8_t,0x18> header{};
    const bool known=allocatorKnown && ReadAt(allocator,0,header.data(),header.size());
    uint32_t stride{},table{};int32_t count{};
    if(known) {
        memcpy(&stride,header.data()+0xc,4);memcpy(&table,header.data()+0x10,4);
        memcpy(&count,header.data()+0x14,4);
    }
    out << "{\"managerKnown\":" << (managerKnown?"true":"false") << ",\"manager\":";
    if(managerKnown)out<<manager;else out<<"null";
    out << ",\"addressKnown\":" << (allocatorKnown?"true":"false") << ",\"address\":";
    if(allocatorKnown)out<<allocator;else out<<"null";
    out << ",\"status\":\"" << (known?"ok":allocator?"unreadable":allocatorKnown?"unbound":"unknown")
        << "\",\"rawBytes\":24";
    if(known) {out << ",\"hex\":";Hex(out,header.data(),header.size());}
    out << ",\"enabled\":";
    if(known)out<<unsigned(header[0]);else out<<"null";
    out << ",\"stride\":";
    if(known)out<<stride;else out<<"null";
    out << ",\"laneTable\":";
    if(known)out<<table;else out<<"null";
    out << ",\"laneCount\":";
    if(known)out<<count;else out<<"null";
    const unsigned bounded=known && count>0?(std::min)(unsigned(count),MaxLanes):0;
    unsigned observed{};out << ",\"lanes\":[";
    for(unsigned i=0;i<bounded;++i) {
        if(i)out<<',';
        std::array<uint32_t,4> words{};
        const bool readable=ReadAt(table,i*sizeof(words),words.data(),sizeof(words));
        if(readable)++observed;
        out << "{\"index\":" << i << ",\"status\":\"" << (readable?"ok":table?"unreadable":"unbound")
            << "\",\"resource\":";
        if(readable)out<<words[0];else out<<"null";
        out << ",\"mappedBase\":";
        if(readable)out<<words[1];else out<<"null";
        out << ",\"capacity\":";
        if(readable)out<<int32_t(words[2]);else out<<"null";
        out << ",\"used\":";
        if(readable)out<<int32_t(words[3]);else out<<"null";
        if(readable) {out << ",\"hex\":";Hex(out,reinterpret_cast<const uint8_t*>(words.data()),sizeof(words));}
        out << '}';
    }
    out << "],\"observedLanes\":" << observed << ",\"laneLimit\":" << MaxLanes
        << ",\"laneCountStatus\":\"" << (!known?"unknown":count<0?"invalid":count>int32_t(MaxLanes)?"bounded":"ok")
        << "\",\"laneRecordsTruncated\":" << (known && count>int32_t(MaxLanes)?"true":"false")
        << ",\"lanesComplete\":" << (known && count>=0 && count<=int32_t(MaxLanes) && observed==unsigned(count)?"true":"false")
        << ",\"scope\":\"non_atomic_cpu_reads_workers_may_be_pending_used_resets_at_stage3_no_mapped_buffer_reads\"}";
}
void Packet(std::ostream& out,uintptr_t packet) {
    // The sign's ordinary and instanced draws both use +80 (caller 8b2f9).
    constexpr std::array<size_t,7> offsets{0x80,0x84,0x88,0xe4,0xe0,0x8c,0x10};
    std::array<uint32_t,7> roots{};std::array<bool,7> known{};
    for(unsigned i=0;i<offsets.size();++i)known[i]=ReadAt(packet,offsets[i],&roots[i],4);
    out << "{\"address\":" << packet << ",\"roots\":[";
    size_t visited{};unsigned matched{},emitted{};bool complete=true;
    std::vector<std::pair<unsigned,PacketNode>> matching;
    for(unsigned i=0;i<offsets.size();++i) {
        if(i)out<<',';
        out << "{\"offset\":" << offsets[i] << ",\"known\":" << (known[i]?"true":"false") << ",\"root\":";
        if(known[i])out<<roots[i];else out<<"null";
        if(!known[i]) {
            complete=false;out << ",\"status\":\"" << (packet?"unreadable":"unbound") << "\",\"complete\":false}";continue;
        }
        const auto snapshot=SnapshotPacketLists(std::array<uint32_t,3>{roots[i],0,0},[](uint32_t address,void* bytes,size_t size) {
            return ReadMemory(reinterpret_cast<const void*>(address),bytes,size);
        },4096-visited);
        visited+=snapshot.nodes.size();complete=complete && snapshot.complete;
        out << ",\"status\":\"" << (snapshot.complete?"ok":"partial") << "\",\"complete\":"
            << (snapshot.complete?"true":"false") << ",\"observedNodes\":" << snapshot.nodes.size() << '}';
        for(const auto& node:snapshot.nodes) {
            uint32_t drawable{};memcpy(&drawable,node.bytes.data()+0x0c,4);
            bool match=false;
            for(const auto& target:targets)if(drawable && drawable==target.drawable)match=true;
            if(!match)continue;
            ++matched;if(matching.size()<MaxMatches)matching.emplace_back(i,node);
        }
    }
    out << "],\"complete\":" << (complete?"true":"false") << ",\"observedNodes\":" << visited << ",\"matches\":[";
    for(const auto& [rootIndex,node]:matching) {
        if(emitted++)out<<',';
        uint32_t drawable{};memcpy(&drawable,node.bytes.data()+0x0c,4);
        out << "{\"rootOffset\":" << offsets[rootIndex] << ",\"address\":" << node.address << ",\"drawable\":" << drawable << ",\"targets\":[";
        bool comma=false;
        for(unsigned i=0;i<targets.size();++i)if(drawable==targets[i].drawable) {
            if(comma)out<<',';comma=true;out<<i+1;
        }
        out << "],\"hex\":";Hex(out,node.bytes.data(),node.bytes.size());out << '}';
    }
    out << "],\"matchingNodes\":" << matched << ",\"matchingDetailsTruncated\":" << (matched>emitted?"true":"false") << '}';
}
}
void Configure(const std::filesystem::path& folder,bool value) {
    End("reconfigured");output=folder;owner=GetCurrentThreadId();enabled=value;
    targets.clear();frames.clear();started=active=truncated=targetLimit=false;
    snapshots=rejectedPhases=0;logBytes=0;
    viewFrame=0;frozenPair={};
}
void RememberViews(uint64_t frame,const SceneViews& views) {
    if(!Owned())return;
    viewFrame=0;
    if(frame && ValidSceneViews(views)) {frozenPair=views;viewFrame=frame;}
}
void Remember(unsigned target,uintptr_t drawable,uintptr_t properties) {
    if(!Owned())return;
    for(auto& item:targets)if(item.label==target && item.drawable==drawable) {
        if(item.properties!=properties)item.propertiesChanged=true;
        return;
    }
    if(targets.size()>=MaxTargets) {targetLimit=true;return;}
    Target item{target,drawable,properties};item.known=Identity(drawable,item.identity);targets.push_back(item);
    if(active)TargetRecord(targets.size()-1);
}
void Start() {
    if(!Owned() || started)return;
    started=true;log.clear();log.open(output/L"sign-visibility.jsonl",std::ios::binary|std::ios::trunc);
    if(!log)return;
    active=true;
    Line("{\"type\":\"begin\",\"version\":1,\"maxTargets\":8,\"maxSnapshots\":256,\"maxFrames\":8,\"maxBytes\":4194304,\"packetNodeLimit\":4096,\"matchingDetailLimit\":64,\"scope\":\"bounded_raw_cpu_state_not_visibility_proof\"}");
    for(size_t i=0;active && i<targets.size();++i)TargetRecord(i);
    log.flush();
}
void Stop() {if(Owned())End(frames.size()==MaxFrames?"complete":"stopped");}
void Snapshot(uint64_t frame,uint64_t scene,unsigned pass,const char* phase,uintptr_t mainObject,uintptr_t packet) {
    if(!Owned() || !active)return;
    if(snapshots>=MaxSnapshots) {End("snapshot_limit");return;}
    if(!frames.contains(frame) && frames.size()>=MaxFrames) {End("frame_limit");return;}
    std::string safePhase;
    if(pass>2 || !Phase(phase,safePhase)) {++rejectedPhases;return;}
    frames.insert(frame);std::ostringstream out;
    out << "{\"type\":\"snapshot\",\"frame\":" << frame << ",\"scene\":" << scene << ",\"pass\":" << pass
        << ",\"phase\":\"" << safePhase << "\",\"targets\":[";
    for(size_t i=0;i<targets.size();++i) {
        if(i)out<<',';const auto& item=targets[i];std::array<uint32_t,3> now{};
        const bool read=Identity(item.drawable,now);
        const bool same=item.known && read && now==item.identity && !item.propertiesChanged;
        const char* state=!item.known?"identity_unknown":!read?"identity_unreadable":!same?"identity_reused":"same_identity";
        out << "{\"id\":" << i+1 << ",\"target\":" << item.label << ",\"status\":\"" << state
            << "\",\"currentIdentityKnown\":" << (read?"true":"false") << ",\"currentIdentity\":[" << now[0] << ',' << now[1] << ',' << now[2]
            << "],\"rememberPropertiesChanged\":" << (item.propertiesChanged?"true":"false") << ",\"drawableRaw\":";
        Raw(out,item.drawable,0,256);
        out << ",\"propertiesRaw\":";Raw(out,item.properties,0,256,same?nullptr:state);
        out << ",\"boundPropertiesRaw\":";Raw(out,now[2],0,256,same?nullptr:state);
        out << ",\"transformRaw\":";Raw(out,now[1],0,256,same?nullptr:state);
        uint32_t flags{};const bool flagsKnown=same && ReadAt(item.drawable,0x44,&flags,4);
        out << ",\"selectionFlagsKnown\":" << (flagsKnown?"true":"false") << ",\"selectionFlags\":";
        if(flagsKnown)out<<flags;else out<<"null";out << '}';
    }
    out << "],\"main\":{\"visibility330\":";Raw(out,mainObject,0x330,224);
    out << ",\"view5f0\":";Raw(out,mainObject,0x5f0,144);
    out << ",\"view680\":";Raw(out,mainObject,0x680,144);
    out << ",\"builder\":";Builder(out,mainObject);
    out << ",\"selectionHeaders\":";SelectionHeaders(out,mainObject);
    out << ",\"allocator\":";Allocator(out,mainObject);
    out << "},\"packet\":";Packet(out,packet);
    if(safePhase=="visibility-before" && frame==viewFrame) {
        out << ",\"views\":{\"frame\":" << frame << ",\"eyes\":[" << std::setprecision(9);
        for(unsigned eye=0;eye<2;++eye) {
            if(eye)out<<',';const auto& p=frozenPair.poses[eye];const auto& f=frozenPair.frusta[eye];
            out << "{\"eye\":" << eye << ",\"qx\":" << p.orientation[0] << ",\"qy\":" << p.orientation[1]
                << ",\"qz\":" << p.orientation[2] << ",\"qw\":" << p.orientation[3]
                << ",\"x\":" << p.position[0] << ",\"y\":" << p.position[1] << ",\"z\":" << p.position[2]
                << ",\"left\":" << f.left << ",\"right\":" << f.right << ",\"down\":" << f.down << ",\"up\":" << f.up << '}';
        }
        out << "]}";
    }
    out << '}';
    if(Line(out.str())) {++snapshots;log.flush();}
}
}
