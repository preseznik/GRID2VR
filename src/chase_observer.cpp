#include "chase_observer.h"
#include "chase_observer_guard.h"
#include "chase_observer_owner.h"
#include "chase_observer_render.h"
#include "chase_observer_prequery.h"
#include "chase_observer_destination.h"
#include "replay_presentation.h"
#include <intrin.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>

namespace grid2vr::chaseobserver {
namespace {
// These native wrappers return no floating-point value. Forward EAX as well as
// their exact stack arguments; do not infer gameplay semantics from the result.
using Update=uintptr_t (__thiscall*)(void*,void*);
using Alternate=uintptr_t (__thiscall*)(void*,void*,uint32_t);
using Getter=uintptr_t (__thiscall*)(void*);
using Scene=uintptr_t (__thiscall*)(void*,uintptr_t,uintptr_t,uintptr_t);
std::array<void*,slots.size()> originals{};
uintptr_t base{};
std::filesystem::path folder;
std::timed_mutex mutex;
bool prepared{},requested{},installed{},failed{},stopped{},restored{};
std::atomic<bool> observing{};
std::atomic<bool> stopRequested{};
std::atomic<uint64_t> frame{},contention{};
std::atomic<uint64_t> exportSerial{};
// Observation generations, not simulation IDs. Keep the old global serial
// for comparison, but freshness belongs to the camera that was evaluated.
std::atomic<uint64_t> evaluationSerial{},activeEvaluations{},providerCalls{};
std::atomic<uint64_t> trackingTaint{},unknownEvaluations{},exportTaint{};
std::atomic<unsigned> window{};
const char* reason="not_requested";
template<class T> bool Read(uintptr_t at,T& value) { return ReadMemory(reinterpret_cast<void*>(at),&value,sizeof(value)); }
struct CameraIdentity {
    uint32_t table{},name{},provider{},entity{},owner{};
    bool readable{};
    bool operator==(const CameraIdentity&) const=default;
};
CameraIdentity Identity(uintptr_t camera) {
    CameraIdentity value{};
    value.readable=Read(camera,value.table) && Read(camera+0x98,value.name) && Read(camera+0x60,value.provider) &&
        Read(camera+0xc,value.owner) && (!value.provider || Read(value.provider+8,value.entity));
    return value;
}
struct EvaluationToken { uint64_t epoch{},generation{},taint{}; };
struct StageEvidence {
    uint64_t sequence{},serial{},frame{};
    EvaluationToken token{};
    unsigned threadId{},calls{};
    uintptr_t camera{},timing{},callerRva{},stageCallerRva{};
    const char* reason="prequery_not_called";
    std::array<uint32_t,recordBytes/4> before{},after{};
    PreQueryObservation snapshot{};
};
struct CameraEntry {
    uintptr_t camera{};
    CameraIdentity identity{};
    uint64_t epoch{},generation{};
    unsigned route{},thread{},claims{};
    std::atomic<unsigned> inFlight{};
    StageEvidence stage{};
    bool sampled{};
};
std::array<CameraEntry,8> cameras{};
uint64_t nextCameraEpoch{};
constexpr unsigned stageBurstLimit=128;
std::array<unsigned,2> stageBurstAttempts{};
CameraEntry* FindCamera(uintptr_t camera) {
    for(auto& entry:cameras)if(entry.camera==camera)return &entry;
    return nullptr;
}
struct EvaluationScope {
    uint64_t serial;
    EvaluationToken token{};
    CameraEntry* entry{};
    bool unknown{};
    EvaluationScope(void* self,unsigned route) {
        ++activeEvaluations;serial=++evaluationSerial;
        if(stopRequested || !observing)return;
        // Unknown tracking loss invalidates all older evidence, including an
        // unchanged-byte ABA. An untracked call remains in flight until return.
        ++unknownEvaluations;unknown=true;
        const auto camera=reinterpret_cast<uintptr_t>(self);const auto identity=Identity(camera);
        std::unique_lock lock(mutex,std::try_to_lock);
        if(!lock.owns_lock()) { ++trackingTaint;return; }
        entry=FindCamera(camera);
        if(!entry) {
            for(auto& candidate:cameras)if(!candidate.inFlight && (!entry || candidate.generation<entry->generation))entry=&candidate;
            if(!entry) { ++trackingTaint;return; }
            entry->camera=camera;entry->epoch=++nextCameraEpoch;entry->stage={};entry->claims=0;
        }
        if(entry->identity!=identity) { entry->epoch=++nextCameraEpoch;entry->identity=identity; }
        entry->generation=serial;entry->route=route;entry->thread=GetCurrentThreadId();entry->sampled=false;
        ++entry->inFlight;token={entry->epoch,serial,trackingTaint.load()};
        if(entry->inFlight!=1)token.epoch=0; // Neither overlapping evaluation can qualify.
        --unknownEvaluations;unknown=false;
    }
    ~EvaluationScope() {
        if(entry)--entry->inFlight;
        if(unknown)--unknownEvaluations;
        --activeEvaluations;
    }
};
const char* StageFreshLocked(uintptr_t camera,const EvaluationToken& token,unsigned expectedActive) {
    if(!token.epoch || token.taint!=trackingTaint || unknownEvaluations)return "stage_tracking_lost";
    const auto* entry=FindCamera(camera);
    if(!entry || entry->epoch!=token.epoch)return "stage_identity_changed_or_evicted";
    if(entry->inFlight!=expectedActive)return "stage_in_flight";
    if(entry->generation!=token.generation)return "stage_superseded";
    if(entry->identity!=Identity(camera))return "stage_identity_changed";
    // A contender can lose this mutex, taint the ledger and forward native
    // work while Identity performs its reads. Check tracking again afterwards.
    if(token.taint!=trackingTaint || unknownEvaluations)return "stage_tracking_lost";
    if(entry->inFlight!=expectedActive || entry->generation!=token.generation)return "stage_in_flight";
    return nullptr;
}
bool StageFresh(const StageEvidence& stage) {
    std::unique_lock lock(mutex,std::try_to_lock);
    return lock.owns_lock() && !StageFreshLocked(stage.camera,stage.token,1);
}
struct Sample {
    uint64_t sequence{},frame{},milliseconds{},exportSerial{},renderFrame{};
    uintptr_t camera{},callerRva{},argument{};
    unsigned route{},window{},replay{},threadId{};
    uint32_t flag{},nameHash{},cameraTable{},providerTable{};
    bool transition{},cameraReadable{},timingReadable{},beforeReadable{},afterReadable{},outputReadable{};
    std::array<uint32_t,8> name{}; // Bounded native name bytes at +19.
    std::array<uint32_t,4> timing{};
    std::array<uint32_t,21> owners{}; // Raw +5c..+ac, not claimed ownership.
    std::array<uint32_t,recordBytes/4> before{},after{},output{};
    std::array<uintptr_t,25> ownerStack{};
    OwnerObservation owner{};
    uintptr_t returnedRecord{};
    const char* ownerReason="owner_stack_unreadable";
    bool ownerStackReadable{};
    RenderObservation render{};
    std::array<uintptr_t,15> sceneStack{};
    const char* renderReason="not_observed";
    unsigned renderThread{};
    bool renderAttempted{},sceneStackReadable{};
    size_t quotaIndex{};
    uint64_t reservation{};
    uintptr_t nativeParent{};
    Destination destination{};
    uint64_t destinationSerial{},destinationTaint{};
    StageEvidence stage{};
    const char* stageReason="stage_not_available";
    const char* legacyStageReason="stage_not_available";
    const char* legacyRenderReason="not_observed";
    uint64_t currentGeneration{},currentEpoch{};
    unsigned invalidatorRoute{},invalidatorThread{};
    bool currentSampled{};
};
struct StageContext;
thread_local StageContext* currentStage{};
struct StageContext {
    StageContext* previous=currentStage;
    StageContext* root=previous?previous->root:this;
    Sample* sample;
    unsigned calls{};
    bool nested=previous!=nullptr;
    explicit StageContext(Sample* value):sample(value) {
        if(previous)root->nested=true;
        currentStage=this;
    }
    ~StageContext() { currentStage=previous; }
};
std::array<Sample,sampleLimit> samples{};
// A bounded identity cache prevents alternating owners from defeating throttling.
struct Quota {
    Sample committed{};
    uintptr_t camera{},parent{};
    unsigned thread{};
    std::atomic<uint64_t> reservation{};
};
std::array<std::array<Quota,8>,slots.size()> previous{};
std::array<uint64_t,slots.size()> calls{};
using Counters=std::array<uint64_t,slots.size()>;
using AtomicCounters=std::array<std::atomic<uint64_t>,slots.size()>;
AtomicCounters entries{},beginDrops{},endDrops{},throttled{},reservationBusy{};
// entryCalls counts every intercepted getter/main-scene invocation. Legacy
// calls retains only qualified entries that acquired the observer mutex.
uint64_t captured{};
uint64_t nextReservation{};
constexpr size_t successLimit=6,transitionLimit=26,tailLimit=sampleLimit-transitionLimit-successLimit;
constexpr size_t successStart=sampleLimit-successLimit;
size_t transitionCount{},tailCount{},tailNext{};
std::array<uint64_t,successLimit> firstSuccess{};
struct PendingExport { size_t index{};uint64_t sequence{}; };
std::array<PendingExport,8> pendingExports{};
struct OutputEntry { uintptr_t output{};uint64_t serial{}; };
std::array<OutputEntry,8> outputs{};
bool OutputFreshLocked(const Sample& sample) {
    if(!sample.destinationSerial || sample.destinationTaint!=exportTaint)return false;
    for(const auto& output:outputs)if(output.output==sample.destination.output)
        return output.serial==sample.destinationSerial && sample.destinationTaint==exportTaint;
    return false;
}
void TrackOutput(Sample& sample,uint64_t serial) {
    std::unique_lock lock(mutex,std::try_to_lock);
    if(!lock.owns_lock() || sample.destination.reason) { ++exportTaint;return; }
    // Distinct addresses can still overlap a 0x90-byte native view. Even an
    // auxiliary stack write invalidates any previously tracked overlapping view.
    for(auto& candidate:outputs)if(candidate.output &&
        candidate.output<sample.destination.output+0x90 && sample.destination.output<candidate.output+0x90)candidate.serial=serial;
    if(sample.destination.auxiliary)return;
    OutputEntry* selected{};
    for(auto& candidate:outputs) {
        if(candidate.output==sample.destination.output) { selected=&candidate;break; }
        if(!selected || candidate.serial<selected->serial)selected=&candidate;
    }
    *selected={sample.destination.output,serial};
    sample.destinationSerial=serial;sample.destinationTaint=exportTaint;
}
unsigned StageClaimBit(uintptr_t parent) { return parent==0x1185af?1:parent==0x21a621?2:0; }
constexpr const char* preQueryContextReasons[]{"captured_prequery","prequery_without_stage","prequery_caller",
    "prequery_unsampled_stage","prequery_nested_stage","prequery_duplicate","prequery_interleaved",
    "prequery_not_called","prequery_source_unreadable","observer_stopped","other_refusal"};
constexpr size_t preQueryResultCount=std::size(preQueryContextReasons)+std::size(preQueryCaptureReasons);
std::array<std::atomic<uint64_t>,preQueryResultCount> preQueryResults{};
const char* PreQueryResultName(size_t index) {
    return index<std::size(preQueryContextReasons)?preQueryContextReasons[index]:preQueryCaptureReasons[index-std::size(preQueryContextReasons)];
}
void CountPreQuery(const char* result) {
    if(!result) { ++preQueryResults[0];return; }
    for(size_t i=1;i<preQueryResultCount;++i)
        if(!std::strcmp(result,PreQueryResultName(i))) { ++preQueryResults[i];return; }
    ++preQueryResults[std::size(preQueryContextReasons)-1];
}
constexpr const char* renderResultNames[]{"captured_main_scene","no_pending_export","sample_evicted",
    "export_superseded","export_present_changed","export_thread_changed","scene_stack_unreadable",
    "export_changed_during_capture","render_address_invalid","render_scene_stack","render_view_arguments",
    "render_main_identity","render_source_changed","render_view_unreadable","render_context_unreadable",
    "render_identity_changed","owner_stack_short","owner_address_invalid","owner_getter_caller",
    "owner_export_caller","owner_stack_ambiguous","owner_output_invalid","owner_root_unreadable",
    "owner_controller_mismatch","owner_flags_unreadable","owner_controller_busy","owner_selection_unreadable",
    "owner_alternate_unreadable","owner_camera_mismatch","owner_camera_unreadable","owner_camera_table",
    "owner_graph_changed","commit_contention","stopped_before_commit","other_refusal"};
std::array<std::atomic<uint64_t>,std::size(renderResultNames)> renderResults{};
void CountRender(const char* result) {
    if(!result) { ++renderResults[0];return; }
    for(size_t i=1;i<std::size(renderResultNames);++i)
        if(!std::strcmp(result,renderResultNames[i])) { ++renderResults[i];return; }
    ++renderResults.back();
}
struct Status {
    uint64_t revision{},captured{},contention{};
    uint64_t trackingTaint{},exportTaint{};
    std::array<unsigned,2> stageBurstAttempts{};
    bool requested{},installed{},failed{},stopped{},restored{};
    const char* reason{};
    Counters calls{},entries{},beginDrops{},endDrops{},throttled{},reservationBusy{};
    std::array<uint64_t,std::size(renderResultNames)> renderResults{};
    std::array<uint64_t,preQueryResultCount> preQueryResults{};
    std::array<uint64_t,successLimit> firstSuccess{};
    std::array<Sample,sampleLimit> samples{};
    size_t count{};
};
std::atomic<std::shared_ptr<const Status>> latestStatus;
std::timed_mutex writerMutex;
uint64_t statusRevision{},writtenRevision{};
#ifdef GRID2VR_CHASE_OBSERVER_TEST
std::atomic<void (*)()> writerHook{};
#endif

void SnapshotLocked() {
    auto snapshot=std::make_shared<Status>();
    snapshot->revision=++statusRevision;snapshot->requested=requested;snapshot->installed=installed;
    snapshot->failed=failed;snapshot->stopped=stopped;snapshot->restored=restored;snapshot->reason=reason;
    snapshot->calls=calls;snapshot->captured=captured;snapshot->contention=contention;
    snapshot->trackingTaint=trackingTaint;snapshot->exportTaint=exportTaint;snapshot->stageBurstAttempts=stageBurstAttempts;
    snapshot->calls[6]=providerCalls;
    snapshot->firstSuccess=firstSuccess;
    for(size_t i=0;i<slots.size();++i) {
        snapshot->entries[i]=entries[i];snapshot->beginDrops[i]=beginDrops[i];snapshot->endDrops[i]=endDrops[i];
        snapshot->throttled[i]=throttled[i];snapshot->reservationBusy[i]=reservationBusy[i];
    }
    for(size_t i=0;i<renderResults.size();++i)snapshot->renderResults[i]=renderResults[i];
    for(size_t i=0;i<preQueryResults.size();++i)snapshot->preQueryResults[i]=preQueryResults[i];
    for(size_t i=0;i<transitionCount+tailCount;++i) {
        const auto index=i<transitionCount?i:transitionLimit+((tailCount==tailLimit?tailNext:0)+i-transitionCount)%tailLimit;
        const auto& sample=samples[index];
        // A promoted pair is serialized once, even while its rolling copy remains.
        bool promoted=false;for(auto sequence:firstSuccess)promoted|=sample.sequence==sequence;
        if(!promoted)snapshot->samples[snapshot->count++]=sample;
    }
    for(size_t i=0;i<successLimit;++i)if(firstSuccess[i]) {
        bool duplicate=false;for(size_t j=0;j<i;++j)duplicate|=firstSuccess[j]==firstSuccess[i];
        if(!duplicate)snapshot->samples[snapshot->count++]=samples[successStart+i];
    }
    // Publication is serialized by the observer mutex; revisions cannot regress.
    latestStatus.store(std::move(snapshot));
}

template<class T> void Words(std::ostream& out,const T& values) {
    out<<'[';bool first=true;for(auto value:values) { if(!first)out<<',';first=false;out<<value; }out<<']';
}
void WritePreObstruction(std::ostream& out,const StageEvidence& stage) {
    out<<"{\"reason\":\""<<(stage.reason?stage.reason:"captured_prequery")<<"\",\"evaluationSerial\":"<<stage.serial
       <<",\"cameraEpoch\":"<<stage.token.epoch<<",\"cameraGeneration\":"<<stage.token.generation<<",\"trackingTaint\":"<<stage.token.taint
       <<",\"sequence\":"<<stage.sequence<<",\"presentFrame\":"<<stage.frame<<",\"threadId\":"<<stage.threadId
       <<",\"camera\":"<<stage.camera<<",\"timing\":"<<stage.timing<<",\"callerRva\":"<<stage.callerRva
       <<",\"stageCallerRva\":"<<stage.stageCallerRva<<",\"calls\":"<<stage.calls;
    if(stage.snapshot.camera) {
        const auto& p=stage.snapshot;
        out<<",\"snapshot\":{\"camera\":"<<p.camera<<",\"timing\":"<<p.timing<<",\"provider\":"<<p.provider
           <<",\"output\":"<<p.output<<",\"returned\":"<<p.returned<<",\"record\":"<<p.record
           <<",\"cameraTable\":"<<p.cameraTable<<",\"providerTable\":"<<p.providerTable<<",\"entity\":"<<p.entity
           <<",\"camera0c\":"<<p.camera0c<<",\"nameHash\":"<<p.nameHash<<",\"provider0c\":"<<p.provider0c
           <<",\"geometry24c0\":"<<p.geometry24c0<<",\"geometry24c4\":"<<p.geometry24c4<<",\"constraint24d8\":"<<p.constraint24d8
           <<",\"entity11a8\":"<<p.entity11a8<<",\"cameraa4\":"<<unsigned(p.cameraa4)<<",\"cameraae\":"<<unsigned(p.cameraae)
           <<",\"camera2564\":"<<unsigned(p.camera2564);
        std::array<uint32_t,8> nameWords{};std::memcpy(nameWords.data(),p.name.data(),p.name.size());
        out<<",\"nameWords\":";Words(out,nameWords);out<<",\"recordWords\":";Words(out,p.recordWords);
        out<<",\"timingWords\":";Words(out,p.timingWords);out<<",\"outputWords\":";Words(out,p.outputWords);
        out<<",\"entityBasisWords\":";Words(out,p.entityBasisWords);out<<",\"entityPositionWords\":";Words(out,p.entityPositionWords);
        out<<",\"localOffsetWords\":";Words(out,p.localOffsetWords);out<<",\"obstructionWords\":";Words(out,p.obstructionWords);out<<'}';
    }
    out<<'}';
}
bool SerializeStatus(const Status& status,const std::filesystem::path& path) {
    std::ofstream out(path,std::ios::trunc);
    out<<std::boolalpha<<"{\"schema\":1,\"mode\":\"observe-v1\",\"requested\":"<<status.requested<<",\"installed\":"<<status.installed
       <<",\"failed\":"<<status.failed<<",\"stopped\":"<<status.stopped<<",\"restored\":"<<status.restored
       <<",\"reason\":\""<<status.reason<<"\",\"cameraWrites\":0,\"inputPolls\":0,\"nativeCodePatches\":0"
         ",\"ownershipProven\":false,\"obstructionOrderProven\":false,\"desktopAccepted\":false"
         ",\"replayAccepted\":false,\"headsetAccepted\":false,\"calls\":";
    Words(out,status.calls);out<<",\"entryCalls\":";Words(out,status.entries);
    out<<",\"trackingTaint\":"<<status.trackingTaint<<",\"exportTaint\":"<<status.exportTaint
       <<",\"stageBurstLimitPerIdentity\":"<<stageBurstLimit<<",\"stageBurstAttempts\":";Words(out,status.stageBurstAttempts);
    out<<",\"beginDrops\":";Words(out,status.beginDrops);out<<",\"endDrops\":";Words(out,status.endDrops);
    out<<",\"throttled\":";Words(out,status.throttled);out<<",\"reservationBusy\":";Words(out,status.reservationBusy);
    out<<",\"renderResults\":{";
    for(size_t i=0;i<status.renderResults.size();++i) { if(i)out<<',';out<<'"'<<renderResultNames[i]<<"\":"<<status.renderResults[i]; }
    out<<"},\"preQueryResults\":{";
    for(size_t i=0;i<status.preQueryResults.size();++i) { if(i)out<<',';out<<'"'<<PreQueryResultName(i)<<"\":"<<status.preQueryResults[i]; }
    out<<"},\"firstSuccessfulPairs\":{\"chase_close\":"<<status.firstSuccess[0]<<",\"chase_far\":"<<status.firstSuccess[1]<<'}'
       <<",\"firstPreQueryRenderPairs\":{\"chase_close\":"<<status.firstSuccess[2]<<",\"chase_far\":"<<status.firstSuccess[3]<<'}';
    out<<",\"firstPreQueryStages\":{\"chase_close\":"<<status.firstSuccess[4]<<",\"chase_far\":"<<status.firstSuccess[5]<<'}';
    out<<",\"receiptRevision\":"<<status.revision<<",\"captured\":"<<status.captured<<",\"contentionDrops\":"<<status.contention<<",\"sampleLimit\":"<<sampleLimit<<",\"samples\":[";
    for(size_t i=0;i<status.count;++i) {
        const auto& s=status.samples[i];if(i)out<<',';
        out<<"{\"sequence\":"<<s.sequence<<",\"presentFrame\":"<<s.frame<<",\"milliseconds\":"<<s.milliseconds
           <<",\"route\":"<<s.route<<",\"camera\":"<<s.camera<<",\"callerRva\":"<<s.callerRva
           <<",\"argument\":"<<s.argument<<",\"flag\":"<<s.flag<<",\"window\":"<<s.window<<",\"recordedReplay\":"<<s.replay<<",\"threadId\":"<<s.threadId<<",\"nativeParentRva\":"<<s.nativeParent
           <<",\"transition\":"<<s.transition<<",\"nameHash\":"<<s.nameHash<<",\"cameraTable\":"<<s.cameraTable<<",\"providerTable\":"<<s.providerTable
           <<",\"cameraReadable\":"<<s.cameraReadable<<",\"timingReadable\":"<<s.timingReadable
           <<",\"beforeReadable\":"<<s.beforeReadable<<",\"afterReadable\":"<<s.afterReadable<<",\"outputReadable\":"<<s.outputReadable<<",\"timingWords\":";
        Words(out,s.timing);out<<",\"nameWords\":";Words(out,s.name);out<<",\"ownerWords\":";Words(out,s.owners);
        out<<",\"beforeWords\":";Words(out,s.before);out<<",\"afterWords\":";Words(out,s.after);
        out<<",\"outputWords\":";Words(out,s.output);
        if(s.route==3) { out<<",\"preObstruction\":";WritePreObstruction(out,s.stage); }
        if(s.route==4) {
            out<<",\"stageAssociation\":{\"reason\":\""<<(s.stageReason?s.stageReason:"matched_stage_record")<<'"';
            out<<",\"legacyReason\":\""<<(s.legacyStageReason?s.legacyStageReason:"matched_stage_record")
               <<"\",\"currentGeneration\":"<<s.currentGeneration<<",\"currentEpoch\":"<<s.currentEpoch
               <<",\"invalidatorCamera\":"<<s.camera<<",\"invalidatorRoute\":"<<s.invalidatorRoute
               <<",\"invalidatorThread\":"<<s.invalidatorThread<<",\"currentSampled\":"<<s.currentSampled;
            if(s.stage.sequence) {
                out<<",\"beforeWords\":";Words(out,s.stage.before);out<<",\"afterWords\":";Words(out,s.stage.after);
                out<<",\"preObstruction\":";WritePreObstruction(out,s.stage);
            }
            out<<'}';
            out<<",\"destination\":{\"reason\":\""<<(s.destination.reason?s.destination.reason:s.destination.auxiliary?"guarded_auxiliary_stack":"guarded_controller_output")
               <<"\",\"output\":"<<s.destination.output<<",\"serial\":"<<s.destinationSerial<<",\"taint\":"<<s.destinationTaint<<'}';
            const auto& o=s.owner;
            out<<",\"exportOwner\":{\"reason\":\""<<(s.ownerReason?s.ownerReason:"matched_export_provenance")
               <<"\",\"serial\":"<<s.exportSerial<<",\"stackReadable\":"<<s.ownerStackReadable<<",\"stackWords\":";Words(out,s.ownerStack);
            out<<",\"callerRva\":"<<o.callerRva<<",\"output\":"<<o.output<<",\"controller\":"<<o.controller
               <<",\"expectedRecord\":"<<o.record<<",\"returnedRecord\":"<<s.returnedRecord
               <<",\"recordPointerMatches\":"<<(o.record && s.returnedRecord==o.record)
               <<",\"root\":"<<o.root<<",\"state\":"<<o.state<<",\"globalController\":"<<o.globalController
               <<",\"manager\":"<<o.manager<<",\"primary\":"<<o.primary<<",\"alternate\":"<<o.alternate
               <<",\"selected\":"<<o.selected<<",\"cameraTable\":"<<o.cameraTable
               <<",\"controller392\":"<<unsigned(o.controller392)<<",\"controller393\":"<<unsigned(o.controller393)
               <<",\"alternateEnabled\":"<<unsigned(o.alternateEnabled)<<'}';
            if(s.renderAttempted) {
                const auto& r=s.render;
                out<<",\"renderObservation\":{\"reason\":\""<<(s.renderReason?s.renderReason:"captured_main_scene")
                   <<"\",\"legacyReason\":\""<<(s.legacyRenderReason?s.legacyRenderReason:"captured_main_scene")
                   <<"\",\"presentFrame\":"<<s.renderFrame<<",\"threadId\":"<<s.renderThread
                   <<",\"stackReadable\":"<<s.sceneStackReadable<<",\"stackWords\":";Words(out,s.sceneStack);
                out<<",\"main\":"<<r.main<<",\"mainTable\":"<<r.mainTable<<",\"type\":"<<r.type<<",\"active\":"<<unsigned(r.active)
                   <<",\"renderer\":"<<r.renderer<<",\"rendererTable\":"<<r.rendererTable<<",\"target\":"<<r.target<<",\"context\":"<<r.context
                   <<",\"exportedReadable\":"<<r.exportedReadable<<",\"globalReadable\":"<<r.globalReadable
                   <<",\"mainReadable\":"<<r.mainReadable<<",\"renderReadable\":"<<r.renderReadable<<",\"contextReadable\":"<<r.contextReadable
                   <<",\"exportedViewWords\":";Words(out,r.exportedView);out<<",\"globalViewWords\":";Words(out,r.globalView);
                out<<",\"mainViewWords\":";Words(out,r.mainView);out<<",\"renderViewWords\":";Words(out,r.renderView);
                out<<",\"contextWords\":";Words(out,r.contextWords);out<<'}';
            }
        }
        out<<'}';
    }
    out<<"]}\n";
    out.flush();return out.good();
}
enum class ReceiptResult { Saved,Pending,Failed };
ReceiptResult WriteStatus(bool bounded=false) {
    // No observer/callback lock is held during file work. A single writer drains
    // the newest immutable snapshot, including a stop published while it wrote.
    std::unique_lock writer(writerMutex,std::defer_lock);
    if(bounded) { if(!writer.try_lock_for(std::chrono::milliseconds(250)))return ReceiptResult::Pending; }
    else writer.lock();
    const auto temporary=folder/L"chase-camera-observer.json.tmp";
    const auto destination=folder/L"chase-camera-observer.json";
    for(;;) {
        const auto snapshot=latestStatus.load();
        if(!snapshot || snapshot->revision<=writtenRevision)return ReceiptResult::Saved;
#ifdef GRID2VR_CHASE_OBSERVER_TEST
        if(const auto hook=writerHook.exchange(nullptr))hook();
#endif
        if(!SerializeStatus(*snapshot,temporary)) {
            std::error_code error;std::filesystem::remove(temporary,error);return ReceiptResult::Failed;
        }
        if(latestStatus.load()->revision!=snapshot->revision)continue;
        if(!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
            std::error_code error;std::filesystem::remove(temporary,error);return ReceiptResult::Failed;
        }
        writtenRevision=snapshot->revision;
        // A newer final snapshot wins even if it arrived during the rename.
    }
}
bool AttachStageLocked(Sample& s) {
    const auto* candidate=FindCamera(s.camera);
    if(!candidate)return false;
    s.currentGeneration=candidate->generation;s.currentEpoch=candidate->epoch;
    s.invalidatorRoute=candidate->route;s.invalidatorThread=candidate->thread;s.currentSampled=candidate->sampled;
    s.stage=candidate->stage;
    s.legacyStageReason=activeEvaluations?"stage_in_flight":s.stage.serial!=evaluationSerial?"stage_superseded":nullptr;
    if(!s.stage.sequence)return false;
    const auto claim=StageClaimBit(s.nativeParent);
    if(!claim) { s.stageReason="stage_export_unqualified";return false; }
    if(const auto refusal=StageFreshLocked(s.camera,s.stage.token,0)) { s.stageReason=refusal;return false; }
    if(s.stage.reason) { s.stageReason=s.stage.reason;return false; }
    if(candidate->claims&claim) { s.stageReason="stage_already_consumed";return false; }
    s.stageReason=nullptr;
    return true;
}
bool BurstStageLocked(const Sample& s) {
    if(s.route!=3 || !s.stage.token.epoch || !s.cameraReadable || s.cameraTable!=base+chaseTableRva)return false;
    // Only spend the consecutive-stage budget on a selected close/far camera.
    // Initialisation before selection commit stays an ordinary observation.
    std::array<uint32_t,8> name{};
    if(!Read(s.camera+0x19,name))return false;
    const unsigned identity=!std::memcmp(name.data(),"chase_close",12)?0:!std::memcmp(name.data(),"chase_far",10)?1:2;
    if(identity==2 || stageBurstAttempts[identity]>=stageBurstLimit || firstSuccess[identity+2])return false;
    uint32_t root{},state{},controller{},manager{},selected{},alternate{};uint8_t enabled{},busy{},busy2{};
    if(!Read(base+0xdef810,root) || !Read(root+0x10,state) || !Read(state+0x14fc,controller) ||
       !Read(controller+0x392,busy) || !Read(controller+0x393,busy2) || busy || busy2 ||
       !Read(controller+0x33c,manager) || !Read(manager+0x748,selected) || !Read(manager+0x759,enabled) ||
       (enabled && !Read(selected+0x70,alternate)) || (alternate?alternate:selected)!=s.camera)return false;
    ++stageBurstAttempts[identity];return true;
}
bool Begin(Sample& s,unsigned route,void* self,void* argument,uint32_t flag,void* caller) {
    if(stopRequested || !observing)return false;
    s.route=route;s.camera=reinterpret_cast<uintptr_t>(self);s.argument=reinterpret_cast<uintptr_t>(argument);
    const auto returnAddress=reinterpret_cast<uintptr_t>(caller);
    s.callerRva=returnAddress>=base && returnAddress-base<0xb15000?returnAddress-base:0;
    s.flag=flag;s.frame=frame;s.window=window;s.milliseconds=GetTickCount64();s.threadId=GetCurrentThreadId();
    s.replay=static_cast<unsigned>(RecordedReplay(base,[](uintptr_t at,void* out,size_t size){return ReadMemory(reinterpret_cast<void*>(at),out,size);}));
    s.cameraReadable=Read(s.camera,s.cameraTable) && Read(s.camera+0x98,s.nameHash);
    if(route==0 || route==1 || route==3)s.timingReadable=Read(s.argument,s.timing);
    std::unique_lock lock(mutex,std::try_to_lock);
    if(!lock.owns_lock()) { ++contention;++beginDrops[route];return false; }
    if(stopRequested || !observing)return false;
    ++calls[route];
    const bool stageReady=route==4 && AttachStageLocked(s);
    Quota* selected{};
    for(auto& candidate:previous[route]) {
        const bool key=candidate.camera==s.camera && (route!=4 ||
            (candidate.thread==s.threadId && candidate.parent==s.nativeParent));
        if(key) { selected=&candidate;break; }
        if(!candidate.reservation && (!selected || candidate.committed.milliseconds<selected->committed.milliseconds))selected=&candidate;
    }
    if(!selected || selected->reservation) { ++reservationBusy[route];return false; }
    if(selected->camera!=s.camera || (route==4 && (selected->thread!=s.threadId || selected->parent!=s.nativeParent))) {
        selected->committed={};selected->camera=s.camera;selected->thread=s.threadId;selected->parent=s.nativeParent;
    }
    const auto& last=selected->committed;
    const bool changed=!last.sequence || s.camera!=last.camera || s.nameHash!=last.nameHash || s.replay!=last.replay ||
        s.window!=last.window || s.flag!=last.flag || s.timingReadable!=last.timingReadable ||
        (s.timing[0]==0)!=(last.timing[0]==0);
    // One forced attempt per completed stage and quota. A refused export must
    // not defeat the ordinary throttle forever while the game is paused.
    const bool freshStageAttempt=stageReady && s.stage.sequence!=last.stage.sequence;
    const bool burst=BurstStageLocked(s);
    if(!burst && !freshStageAttempt && !changed && s.milliseconds-last.milliseconds<1000) { ++throttled[route];return false; }
    // Reserve against reentry without consuming the committed sample's quota.
    // End can cancel this exact reservation atomically when its try-lock fails.
    s.transition=changed;s.quotaIndex=static_cast<size_t>(selected-previous[route].data());
    s.reservation=++nextReservation;selected->reservation=s.reservation;
    lock.unlock();
    Read(s.camera+0x19,s.name);
    s.cameraReadable=Read(s.camera+0x5c,s.owners) && s.cameraReadable;
    if(s.owners[1])Read(s.owners[1],s.providerTable);
    s.beforeReadable=Read(s.camera+0xb0,s.before);
    return true;
}
void CancelReservation(const Sample& s) {
    auto expected=s.reservation;
    previous[s.route][s.quotaIndex].reservation.compare_exchange_strong(expected,0);
}
void End(Sample& s) {
    if(stopRequested || !observing) { CancelReservation(s);return; }
    s.afterReadable=Read(s.camera+0xb0,s.after);
    if(s.route==2)s.outputReadable=Read(s.argument,s.output);
    std::unique_lock lock(mutex,std::try_to_lock);
    if(!lock.owns_lock()) { ++contention;++endDrops[s.route];CancelReservation(s);return; }
    if(stopRequested || !observing) { CancelReservation(s);return; }
    auto& quota=previous[s.route][s.quotaIndex];
    if(quota.reservation!=s.reservation)return;
    s.sequence=++captured;
    if(s.route==3) {
        s.stage.sequence=s.sequence;s.stage.before=s.before;s.stage.after=s.after;
        if(!s.beforeReadable || !s.afterReadable)s.stage.reason="prequery_source_unreadable";
        else if(!s.stage.reason && StageFreshLocked(s.camera,s.stage.token,1))s.stage.reason="prequery_interleaved";
        if(s.stage.reason && (!s.stage.calls || !std::strcmp(s.stage.reason,"prequery_interleaved") ||
                             !std::strcmp(s.stage.reason,"prequery_source_unreadable")))CountPreQuery(s.stage.reason);
    }
    if(s.route==4 && !s.stageReason) {
        auto* candidate=FindCamera(s.camera);
        if(const auto refusal=StageFreshLocked(s.camera,s.stage.token,0))s.stageReason=refusal;
        else if(s.stage.sequence!=candidate->stage.sequence || (candidate->claims&StageClaimBit(s.nativeParent)))s.stageReason="stage_already_consumed";
        else if(s.ownerReason || s.returnedRecord!=s.owner.record)s.stageReason="stage_export_unqualified";
        else if(!s.beforeReadable || !s.afterReadable || s.before!=s.stage.after || s.after!=s.stage.after)
            s.stageReason="stage_record_changed";
        else candidate->claims|=StageClaimBit(s.nativeParent);
    }
    if(s.route==4 && !s.legacyStageReason) {
        s.legacyStageReason=activeEvaluations?"stage_in_flight":s.stage.serial!=evaluationSerial?"stage_superseded":s.stageReason;
    }
    quota.committed=s;quota.reservation=0;
    // Keep initial transitions and a rolling tail, plus first close/far stages,
    // render pairs and complete stage/render chains. Total storage stays96.
    size_t index{};
    if(s.transition && transitionCount<transitionLimit)index=transitionCount++;
    else {
        index=transitionLimit+tailNext;tailNext=(tailNext+1)%tailLimit;
        if(tailCount<tailLimit)++tailCount;
    }
    samples[index]=s;
    if(s.route==3) {
        auto* entry=FindCamera(s.camera);
        if(entry && entry->epoch==s.stage.token.epoch && entry->generation==s.stage.token.generation) {
            entry->stage=s.stage;entry->claims=0;entry->sampled=true;
        }
        if(!s.stage.reason) {
            const size_t identity=std::memcmp(s.stage.snapshot.name.data(),"chase_close",12)?5:4;
            if(!firstSuccess[identity]) { firstSuccess[identity]=s.sequence;samples[successStart+identity]=s; }
        }
    }
    if(s.route==4) {
        if(!s.ownerReason && s.returnedRecord==s.owner.record && s.destinationSerial) {
            PendingExport* selected{};
            for(auto& candidate:pendingExports) {
                const auto& old=samples[candidate.index];
                if(candidate.sequence && old.sequence==candidate.sequence && old.camera==s.camera &&
                   old.threadId==s.threadId && old.nativeParent==s.nativeParent && old.destination.output==s.destination.output) {
                    selected=&candidate;break;
                }
                if(!selected || candidate.sequence<selected->sequence)selected=&candidate;
            }
            *selected={index,s.sequence};
        }
    }
}
uintptr_t __fastcall UpdateObserved(void* self,void*,void* timing) {
    ++entries[0];
    EvaluationScope evaluation(self,0);
    Sample s{};const bool keep=Begin(s,0,self,timing,0,_ReturnAddress());
    const auto result=reinterpret_cast<Update>(originals[0])(self,timing);
    if(keep)End(s);return result;
}
uintptr_t __fastcall AlternateObserved(void* self,void*,void* timing,uint32_t flag) {
    ++entries[1];
    EvaluationScope evaluation(self,1);
    Sample s{};const bool keep=Begin(s,1,self,timing,flag,_ReturnAddress());
    const auto result=reinterpret_cast<Alternate>(originals[1])(self,timing,flag);
    if(keep)End(s);return result;
}
uintptr_t __fastcall CopyObserved(void* self,void*,void* output) {
    ++entries[2];
    Sample s{};const bool keep=Begin(s,2,self,output,0,_ReturnAddress());
    const auto result=reinterpret_cast<Update>(originals[2])(self,output);
    if(keep)End(s);return result;
}
uintptr_t __fastcall StageObserved(void* self,void*,void* timing) {
    ++entries[3];
    EvaluationScope evaluation(self,3);
    Sample s{};s.stage.token=evaluation.token;
    const bool keep=Begin(s,3,self,timing,0,_ReturnAddress());
    if(keep) {
        s.stage.serial=evaluation.serial;s.stage.frame=s.frame;s.stage.threadId=s.threadId;
        s.stage.camera=s.camera;s.stage.timing=s.argument;s.stage.stageCallerRva=s.callerRva;
    }
    StageContext context(keep?&s:nullptr);
    const auto result=reinterpret_cast<Update>(originals[3])(self,timing);
    if(keep && context.nested) { s.stage.reason="prequery_nested_stage";CountPreQuery(s.stage.reason); }
    if(keep)End(s);return result;
}
uintptr_t ForwardProvider(void* self,void* output,void* caller) {
    ++entries[6];
    const auto result=reinterpret_cast<Update>(originals[6])(self,output);
    if(stopRequested || !observing) { CountPreQuery("observer_stopped");return result; }
    if(reinterpret_cast<uintptr_t>(caller)!=base+0x1f78ed) { CountPreQuery("prequery_caller");return result; }
    auto* context=currentStage;
    if(!context) { CountPreQuery("prequery_without_stage");return result; }
    ++providerCalls;++context->calls;
    if(!context->sample) { CountPreQuery("prequery_unsampled_stage");return result; }
    auto& s=*context->sample;s.stage.calls=context->calls;s.stage.callerRva=0x1f78ed;
    if(context->nested || context->root->nested)s.stage.reason="prequery_nested_stage";
    else if(context->calls!=1)s.stage.reason="prequery_duplicate";
    else if(!StageFresh(s.stage))s.stage.reason="prequery_interleaved";
    else s.stage.reason=CapturePreQuery(base,s.camera,s.argument,reinterpret_cast<uintptr_t>(self),
        reinterpret_cast<uintptr_t>(output),result,
        [](uintptr_t at,void* out,size_t size){return ReadMemory(reinterpret_cast<void*>(at),out,size);},s.stage.snapshot);
    // The snapshot helper checks its data twice. A concurrent intercepted
    // evaluation invalidates it even if the native bytes happen to return equal.
    if(!s.stage.reason && !StageFresh(s.stage))s.stage.reason="prequery_interleaved";
    CountPreQuery(s.stage.reason);
    return result;
}
__declspec(noinline) uintptr_t __fastcall ProviderObserved(void* self,void*,void* output) {
    return ForwardProvider(self,output,_ReturnAddress());
}
uintptr_t ForwardExport(void* self,void* caller,const void* returnSlot) {
    // Other uses of the native getter forward without sampling. The qualified
    // caller exports a record; it does not establish renderer consumption.
    Sample s{};
    ++entries[4];
    const bool qualified=reinterpret_cast<uintptr_t>(caller)==base+0x8047a0;
    // Even unsampled evaluations invalidate an older pending export.
    const auto serial=qualified?++exportSerial:0;
    if(qualified && !stopRequested && observing) {
        s.ownerStackReadable=ReadMemory(returnSlot,s.ownerStack.data(),sizeof(s.ownerStack));
        if(s.ownerStackReadable && s.ownerStack[18]>=base && s.ownerStack[18]-base<0xb15000)s.nativeParent=s.ownerStack[18]-base;
        s.destination=s.ownerStackReadable?DecodeDestination(s.ownerStack,reinterpret_cast<uintptr_t>(returnSlot),base,
            reinterpret_cast<uintptr_t>(self),[](uintptr_t at,void* out,size_t size){return ReadMemory(reinterpret_cast<void*>(at),out,size);}):Destination{0,false,"destination_stack_unreadable"};
        TrackOutput(s,serial);
    }
    const bool keep=qualified && Begin(s,4,self,nullptr,0,caller);
    if(keep) {
        s.exportSerial=serial;
        if(s.ownerStackReadable)s.ownerReason=DecodeOwner(s.ownerStack,base,s.camera,
            [](uintptr_t at,void* out,size_t size){return ReadMemory(reinterpret_cast<void*>(at),out,size);},s.owner);
    }
    const auto result=reinterpret_cast<Getter>(originals[4])(self);
    if(keep) { s.returnedRecord=result;End(s); }
    return result;
}
__declspec(noinline) uintptr_t __fastcall ExportObserved(void* self,void*) {
    return ForwardExport(self,_ReturnAddress(),_AddressOfReturnAddress());
}
void ObserveRender(void* self,uintptr_t context,uintptr_t view,uintptr_t packet,const void* returnSlot) {
    if(stopRequested || !observing)return;
    std::unique_lock lock(mutex,std::try_to_lock);
    if(!lock.owns_lock()) { ++contention;++beginDrops[5];return; }
    if(stopRequested || !observing)return;
    ++calls[5];
    PendingExport* pending{};PendingExport* sameThreadFallback{};
    for(auto& candidate:pendingExports) {
        if(!candidate.sequence)continue;
        const auto& sample=samples[candidate.index];
        if(sample.sequence!=candidate.sequence) { candidate.sequence=0;CountRender("sample_evicted");continue; }
        if(sample.threadId!=GetCurrentThreadId())continue;
        if(!sameThreadFallback || candidate.sequence>sameThreadFallback->sequence)sameThreadFallback=&candidate;
        OwnerObservation current{};
        const auto ownerReason=DecodeOwner(sample.ownerStack,base,sample.camera,
            [](uintptr_t at,void* out,size_t size){return ReadMemory(reinterpret_cast<void*>(at),out,size);},current);
        if(!ownerReason && current==sample.owner && OutputFreshLocked(sample) &&
           (!pending || candidate.sequence>pending->sequence))pending=&candidate;
    }
    if(!pending)pending=sameThreadFallback;
    if(!pending) {
        // A main-scene attempt on another thread must not consume the producer's
        // evidence. Keep the refusal visible while preserving the pending entry.
        for(auto& candidate:pendingExports)if(candidate.sequence && (!pending || candidate.sequence>pending->sequence))pending=&candidate;
        if(!pending) { CountRender("no_pending_export");return; }
        auto& sample=samples[pending->index];sample.renderAttempted=true;sample.renderFrame=frame;
        sample.renderThread=GetCurrentThreadId();sample.renderReason="export_thread_changed";
        sample.legacyRenderReason=sample.renderReason;CountRender(sample.renderReason);return;
    }
    const auto index=pending->index;const auto sequence=pending->sequence;
    Sample s=samples[index];
    if(s.sequence!=sequence || s.route!=4) { CountRender("sample_evicted");return; }
    const bool fresh=OutputFreshLocked(s);
    lock.unlock();
    // Native graph and view reads are bounded, but need no observer lock.
    // Destination freshness is rechecked on commit across every thread/parent.
    s.renderAttempted=true;s.renderFrame=frame;s.renderThread=GetCurrentThreadId();
    if(!fresh)s.renderReason="export_superseded";
    else if(s.frame!=s.renderFrame)s.renderReason="export_present_changed";
    else if(s.threadId!=s.renderThread)s.renderReason="export_thread_changed";
    else {
        s.sceneStackReadable=ReadMemory(returnSlot,s.sceneStack.data(),sizeof(s.sceneStack));
        s.renderReason=s.sceneStackReadable?CaptureRender(base,reinterpret_cast<uintptr_t>(self),s.camera,s.owner,s.ownerStack,
            context,view,packet,s.sceneStack,[](uintptr_t at,void* out,size_t size){return ReadMemory(reinterpret_cast<void*>(at),out,size);},s.render):"scene_stack_unreadable";
    }
    s.legacyRenderReason=s.exportSerial!=exportSerial?"export_superseded":s.renderReason;
    if(!lock.try_lock()) { ++contention;++endDrops[5];CountRender("commit_contention");return; }
    if(stopRequested || !observing) { CountRender("stopped_before_commit");return; }
    if(samples[index].sequence!=sequence) { CountRender("sample_evicted");return; }
    if(!s.renderReason && !OutputFreshLocked(s))s.renderReason="export_changed_during_capture";
    if(!s.legacyRenderReason && s.exportSerial!=exportSerial)s.legacyRenderReason="export_changed_during_capture";
    if(!s.legacyRenderReason)s.legacyRenderReason=s.renderReason;
    if(!s.stageReason && StageFreshLocked(s.camera,s.stage.token,0))s.stageReason="stage_superseded_before_render";
    if(const auto* entry=FindCamera(s.camera)) {
        s.currentGeneration=entry->generation;s.currentEpoch=entry->epoch;s.currentSampled=entry->sampled;
        s.invalidatorRoute=entry->route;s.invalidatorThread=entry->thread;
    }
    if(!s.legacyStageReason && (activeEvaluations || s.stage.serial!=evaluationSerial))s.legacyStageReason="stage_superseded_before_render";
    // Consume only the matched entry, after validation. An invalid owner/stack
    // can be retried; a successful or definitively stale export cannot replay.
    if(pending->sequence==sequence && (!s.renderReason || !fresh || s.frame!=s.renderFrame))pending->sequence=0;
    CountRender(s.renderReason);samples[index]=s;
    if(!s.renderReason) {
        constexpr const char* names[]{"chase_close","chase_far"};
        for(size_t i=0;i<std::size(names);++i)
            if(!std::memcmp(s.name.data(),names[i],std::strlen(names[i])+1)) {
                if(!firstSuccess[i]) { samples[successStart+i]=s;firstSuccess[i]=s.sequence; }
                if(!s.stageReason && !firstSuccess[i+2]) { samples[successStart+i+2]=s;firstSuccess[i+2]=s.sequence; }
                break;
            }
    }
}
__declspec(noinline) uintptr_t __fastcall RenderObserved(void* self,void*,uintptr_t context,uintptr_t view,uintptr_t packet) {
    ++entries[5];
    if(reinterpret_cast<uintptr_t>(_ReturnAddress())==base+0x3be09e)
        ObserveRender(self,context,view,packet,_AddressOfReturnAddress());
    return reinterpret_cast<Scene>(originals[5])(self,context,view,packet);
}
const std::array<void*,slots.size()> replacements{reinterpret_cast<void*>(UpdateObserved),reinterpret_cast<void*>(AlternateObserved),
    reinterpret_cast<void*>(CopyObserved),reinterpret_cast<void*>(StageObserved),reinterpret_cast<void*>(ExportObserved),reinterpret_cast<void*>(RenderObserved),reinterpret_cast<void*>(ProviderObserved)};
bool Restore(size_t count) {
    bool ok=true;
    for(size_t i=0;i<count;++i) {
        auto** slot=reinterpret_cast<void**>(base+SlotRva(i));
        void* value{};
        if(!ReadMemory(slot,&value,sizeof(value)) ||
           (value!=originals[i] && !SwapGuardedPointer(slot,replacements[i],originals[i])))ok=false;
    }
    return ok;
}
const char* StopLocked() {
    // Recheck under the same lock as Install, which may have been finishing
    // when the exit thread first disabled observation.
    observing=false;
    if(!requested)return "not_requested";
    if(!stopped) {
        stopped=true;
        if(installed) { restored=Restore(slots.size());if(!restored)failed=true; }
        if(installed && !restored)reason="pointer_restore_failed";
        else if(!failed)reason=installed?"observation_complete_unvalidated":"never_reached_native_readiness";
    }
    if(installed && !restored)return "observer_restore_failed";
    return installed?"observer_restored":"observer_not_installed";
}
const char* FinishStop(std::unique_lock<std::timed_mutex>& lock,bool bounded) {
    const auto result=StopLocked();
    if(!requested)return result;
    const bool wasInstalled=installed;
    SnapshotLocked();lock.unlock();
    const auto receipt=WriteStatus(bounded);
    if(!std::strcmp(result,"observer_restore_failed"))return result;
    if(receipt==ReceiptResult::Failed)return "observer_receipt_failed";
    if(receipt==ReceiptResult::Pending)return wasInstalled?"observer_restored_receipt_pending":"observer_not_installed_receipt_pending";
    return result;
}
}
void Prepare(uintptr_t gameBase,const std::filesystem::path& output) {
    std::unique_lock lock(mutex);if(prepared)return;prepared=true;base=gameBase;folder=output;
    std::error_code error;const auto path=output/L"chase-camera-observer.request";
    if(!std::filesystem::exists(path,error))return;
    requested=true;
    std::array<char,12> bytes{};std::ifstream input(path,std::ios::binary);
    input.read(bytes.data(),bytes.size());
    if(!ValidRequest(std::string_view(bytes.data(),static_cast<size_t>(input.gcount())))) {
        failed=true;reason="invalid_request";
    } else reason="waiting_for_native_readiness";
    SnapshotLocked();lock.unlock();WriteStatus();
}
void Install() {
    std::unique_lock lock(mutex);if(!requested || installed || failed || stopped || stopRequested)return;
    if(!GuardsMatch(base)) { failed=true;reason="instruction_or_vtable_guard";SnapshotLocked();lock.unlock();WriteStatus();return; }
    for(size_t i=0;i<slots.size();++i)originals[i]=reinterpret_cast<void*>(base+routines[i]);
    for(size_t i=0;i<slots.size();++i) {
        if(!SwapGuardedPointer(reinterpret_cast<void**>(base+SlotRva(i)),originals[i],replacements[i])) {
            restored=Restore(i);failed=true;reason=restored?"pointer_install_failed":"pointer_rollback_failed";
            SnapshotLocked();lock.unlock();WriteStatus();return;
        }
    }
    installed=true;
    if(stopRequested) { FinishStop(lock,false);return; }
    observing=true;reason="observing_unclassified_chase_calls";
    SnapshotLocked();lock.unlock();WriteStatus();
}
void Present(uint64_t value,unsigned state) {
    frame=value;window=state;
    if(value%120)return;
    std::unique_lock lock(mutex);if(!requested || stopped)return;
    SnapshotLocked();lock.unlock();WriteStatus();
}
void Stop() {
    stopRequested=true;observing=false;
    std::unique_lock lock(mutex);FinishStop(lock,false);
}
const char* StopForExit() {
    stopRequested=true;observing=false;
    std::unique_lock lock(mutex,std::defer_lock);
    if(!lock.try_lock_for(std::chrono::milliseconds(250)))return "observer_busy";
    return FinishStop(lock,true);
}
#ifdef GRID2VR_CHASE_OBSERVER_TEST
void TestWithLock(void (*callback)()) { std::lock_guard lock(mutex);callback(); }
void TestBeforeWrite(void (*callback)()) { writerHook=callback; }
void TestOriginals(void* update,void* alternate,void* copy,void* stage,void* getter,void* render,void* provider) {
    originals={update,alternate,copy,stage,getter,render,provider?provider:originals[6]};observing=true;
}
uintptr_t TestProvider(void* self,void* output,uintptr_t callerRva) {
    return ForwardProvider(self,output,reinterpret_cast<void*>(base+callerRva));
}
uintptr_t TestRender(void* self,uintptr_t context,uintptr_t view,uintptr_t packet) {
    return reinterpret_cast<Scene>(replacements[5])(self,context,view,packet);
}
uintptr_t TestExport(void* self,const void* stack) { return ForwardExport(self,reinterpret_cast<void*>(base+0x8047a0),stack); }
void TestRenderCapture(void* self,uintptr_t context,uintptr_t view,uintptr_t packet,const void* stack) {
    ++entries[5];
    ObserveRender(self,context,view,packet,stack);
}
uintptr_t TestCall(unsigned route,void* camera,void* argument,uint32_t flag) {
    if(route==1)return reinterpret_cast<Alternate>(replacements[1])(camera,argument,flag);
    if(route==4)return reinterpret_cast<Getter>(replacements[4])(camera);
    return reinterpret_cast<Update>(replacements[route])(camera,argument);
}
#endif
}
