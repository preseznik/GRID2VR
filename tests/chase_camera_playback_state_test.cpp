#include "chase_camera_playback_state.h"
#include <cstring>
#include <map>
#include <vector>

using namespace grid2vr::chasecamera;
namespace {
constexpr uintptr_t image=0x400000,root=0x20000000,state=0x21000000,ui=0x22000000;
struct Memory {
    std::map<uintptr_t,uint8_t> bytes;
    std::vector<uintptr_t> reads;
    size_t failRead{},largest{};
    uintptr_t mutate{};uint32_t mutation{};unsigned hits{};
    void Put(uintptr_t at,uint32_t value) {
        for(unsigned i=0;i<4;++i)bytes[at+i]=static_cast<uint8_t>(value>>(8*i));
    }
    bool Read(uintptr_t at,void* output,size_t size) {
        reads.push_back(at);if(size>largest)largest=size;
        if(reads.size()==failRead || size>48 || at<0x10000 || at>UINT32_MAX-size)return false;
        if(at==mutate && ++hits==2)Put(at,mutation);
        for(size_t i=0;i<size;++i) {
            const auto found=bytes.find(at+i);if(found==bytes.end())return false;
            static_cast<uint8_t*>(output)[i]=found->second;
        }
        return true;
    }
};
Memory Fixture(uintptr_t base=image) {
    Memory m;m.Put(base+0xdef810,root);m.Put(root+0x10,state);m.Put(state+0x148c,ui);
    m.Put(ui,static_cast<uint32_t>(base+0xd10640));m.Put(ui+0x1d8,0);m.Put(ui+0x7640,0);m.Put(ui+0x7604,0);
    // This global is zero in UI-owned playback too. It must not grant eligibility.
    m.Put(base+0xe61010,0);m.Put(ui+0x75ec,0x23000000);
    for(const auto& g:ReplayStateGuards(base))for(size_t i=0;i<g.size;++i)m.bytes[base+g.rva+i]=g.bytes[i];
    return m;
}
const char* Refusal(Memory& m,uintptr_t base=image) {
    return NativeDrivingReplayRefusal(base,[&](uintptr_t at,void* out,size_t size){return m.Read(at,out,size);});
}
bool Guards(Memory& m,uintptr_t base=image) {
    return ReplayStateGuardsMatch(base,[&](uintptr_t at,void* out,size_t size){return m.Read(at,out,size);});
}
bool Reason(const char* actual,const char* expected) {return actual && std::strcmp(actual,expected)==0;}
}
int main() {
    auto m=Fixture();const auto before=m.bytes;
    if(Refusal(m) || m.reads.size()!=14 || m.largest!=4 || m.bytes!=before)return 1;
    for(uint32_t mode=0;mode<18;++mode) {
        m=Fixture();m.Put(ui+0x1d8,mode);
        const auto reason=Refusal(m);
        if(mode==0 || mode==7) {if(reason)return 2;}
        else if(!Reason(reason,"driving_replay_active_or_unknown"))return 2;
    }
    m=Fixture();m.Put(ui+0x1d8,UINT32_MAX);if(!Reason(Refusal(m),"driving_replay_active_or_unknown"))return 3;
    const char* unreadable[]{"driving_replay_root_unreadable","driving_replay_state_unreadable",
        "driving_replay_ui_unreadable","driving_replay_type_unreadable","driving_replay_mode_unreadable",
        "driving_replay_override_unreadable","driving_replay_transport_unreadable"};
    for(size_t call=1;call<=14;++call) {
        m=Fixture();m.failRead=call;
        if(!Reason(Refusal(m),call<=7?unreadable[call-1]:"driving_replay_graph_changed"))return 4;
    }
    size_t pointerIndex=0;
    const char* invalidPointers[]{"driving_replay_root_invalid","driving_replay_state_invalid","driving_replay_ui_invalid"};
    for(uintptr_t address:{image+0xdef810,root+0x10,state+0x148c}) {
        for(uint32_t value:{0u,0x10001u,0xfffffffcu}) {
            m=Fixture();m.Put(address,value);if(!Reason(Refusal(m),invalidPointers[pointerIndex]))return 5;
        }
        ++pointerIndex;
    }
    m=Fixture();m.Put(ui,static_cast<uint32_t>(image+0xd10644));if(!Reason(Refusal(m),"driving_replay_type_mismatch"))return 6;
    for(uintptr_t at:{image+0xdef810,root+0x10,state+0x148c,ui,ui+0x1d8,ui+0x7640,ui+0x7604}) {
        m=Fixture();m.mutate=at;m.mutation=at==ui+0x1d8?7u:at==ui+0x7640?1u:at==ui+0x7604?2u:0u;
        if(!Reason(Refusal(m),"driving_replay_graph_changed"))return 7;
    }
    // Even two independently valid graphs must be the same graph.
    m=Fixture();m.Put(root+0x14,state);m.mutate=image+0xdef810;m.mutation=static_cast<uint32_t>(root+4);
    if(!Reason(Refusal(m),"driving_replay_graph_changed"))return 8;
    for(uintptr_t invalid:{uintptr_t(0),uintptr_t(0x400001),uintptr_t(0xff300000)}) {
        m=Fixture();if(!Reason(Refusal(m,invalid),"driving_replay_image_invalid") || !m.reads.empty() || Guards(m,invalid))return 9;
    }
    for(uintptr_t base:{image,uintptr_t(0x10000000)}) {
        m=Fixture(base);if(!Guards(m,base))return 10;
        for(const auto& g:ReplayStateGuards(base))for(size_t i=0;i<g.size;++i) {
            m=Fixture(base);m.bytes[base+g.rva+i]^=1;if(Guards(m,base))return 11;
        }
        for(size_t call=1;call<=ReplayStateGuards(base).size();++call){m=Fixture(base);m.failRead=call;if(Guards(m,base))return 12;}
    }
    for(uint32_t mode:{0u,7u})for(uint32_t overrideValue:{1u,2u,UINT32_MAX}) {
        m=Fixture();m.Put(ui+0x1d8,mode);m.Put(ui+0x7640,overrideValue);
        if(!Reason(Refusal(m),"driving_replay_override_active_or_unknown"))return 13;
    }
    for(uint32_t mode:{0u,7u})for(uint32_t transport:{0u,1u,2u,3u,UINT32_MAX}) {
        m=Fixture();m.Put(ui+0x1d8,mode);m.Put(ui+0x7604,transport);
        const auto failure=Refusal(m);
        if(transport==0 || transport==2) {if(failure)return 14;}
        else if(!Reason(failure,"driving_replay_transport_active_or_unknown"))return 14;
    }
    return 0;
}
