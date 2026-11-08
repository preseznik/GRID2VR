#include "seat_runtime.h"
#include "seat_camera_host.h"
#include "seat_input_filter.h"
#include "seat_input_policy.h"
#include "seat_panel.h"
#include "seat_presenter.h"
#include "game_executable.h"
#include <atomic>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace grid2vr::seat {
namespace {
bool Key(unsigned key){return (GetAsyncKeyState(static_cast<int>(key))&0x8000)!=0;}
using ReadPad=DWORD (WINAPI*)(DWORD,XINPUT_STATE*);
struct State {
    State(uintptr_t image,std::filesystem::path folder,SessionRequest settings,std::string digest,void (*logger)(const char*,...))
        :base(image),output(std::move(folder)),request(settings),committed(request,std::move(digest)),input(Key),log(logger){}
    uintptr_t base{};
    std::filesystem::path output;
    SessionRequest request;
    CommitBuffer committed;
    Menu menu;
    InputPolicy input;
    CameraHost camera;
    PanelImage panel;
    SeatPresenter presenter;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    HWND window{};
    DWORD thread{};
    ReadPad readPad{};
    void (*log)(const char*,...){};
    seatinput::Filters filters;
    bool enabled{},failed{},stopped{},windowReady{},xrReady{},sideways{},graphicsReady{},saveFailed{};
    uint64_t frames{},openings{},saves{};
    const char* reason="prepared";
    std::string signature;
};
std::atomic<State*> state;
std::once_flag prepared;
bool Plain(const std::filesystem::path& path) {
    if(!path.is_absolute())return false;
    for(auto part=path;!part.empty();part=part.parent_path()) {
        const auto flags=GetFileAttributesW(part.c_str());
        if(flags!=INVALID_FILE_ATTRIBUTES && (flags&FILE_ATTRIBUTE_REPARSE_POINT))return false;
        if(part==part.parent_path())break;
    }
    return true;
}
bool WriteCommit(State& s) {
    const auto text=s.committed.Text();if(text.empty() || text.size()>8192)return false;
    const auto temporary=s.output/L"seat-menu.commit.tmp",target=s.output/L"seat-menu.commit";
    if(!Plain(temporary) || !Plain(target))return false;
    const auto file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    DWORD written{};
    const bool valid=WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr) && written==text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if(!valid || !MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());return false;
    }
    return true;
}
void Receipt(State& s,bool force=false) {
    const auto p=s.menu.Current();
    std::ostringstream key;key<<s.reason<<' '<<s.camera.Reason()<<' '<<s.menu.Car()<<' '<<s.menu.Opened()<<' '<<p.height<<' '<<p.forward<<' '<<p.right<<' '<<s.menu.Fov()<<' '<<s.saves;
    if(!force && s.signature==key.str() && s.frames%120)return;
    s.signature=key.str();
    std::ofstream file(s.output/L"seat-menu.json",std::ios::trunc);
    const auto filtered=s.input.Filtered();
    file<<std::boolalpha<<"{\"schema\":1,\"requested\":true,\"enabled\":"<<s.enabled<<",\"failed\":"<<s.failed<<",\"stopped\":"<<s.stopped
        <<",\"reason\":"<<std::quoted(s.reason)<<",\"camera\":"<<std::quoted(s.camera.Reason())
        <<",\"desktop\":"<<s.request.desktop<<",\"positions\":"<<s.request.positions<<",\"open\":"<<s.menu.Opened()
        <<",\"car\":"<<std::quoted(s.menu.Car()>=0?CarCode(static_cast<size_t>(s.menu.Car())):"")
        <<",\"height\":"<<p.height<<",\"forward\":"<<p.forward<<",\"right\":"<<p.right<<",\"fov\":"<<s.menu.Fov()
        <<",\"frames\":"<<s.frames<<",\"openings\":"<<s.openings<<",\"saves\":"<<s.saves<<",\"saveFailed\":"<<s.saveFailed
        <<",\"input\":{\"keyboard\":"<<s.filters.keyboard<<",\"directInput\":"<<s.filters.directInput<<",\"xinput\":"<<s.filters.xinput<<",\"window\":"<<s.windowReady
        <<"},\"blockedKeyReads\":"<<filtered[0]<<",\"blockedPadReads\":"<<filtered[1]
        <<",\"xrPanelReady\":"<<s.xrReady<<",\"gameArchiveWrites\":0,\"saveGameWrites\":0,\"headsetValidated\":false}\n";
}
bool Initialize(State& s,IDXGISwapChain* chain,NativeEyeDraw draw) {
    s.thread=GetCurrentThreadId();
    if(!draw || !s.camera.Initialize(s.base,s.request)) {s.reason="camera_host_guard";return false;}
    DXGI_SWAP_CHAIN_DESC desc{};
    if(FAILED(chain->GetDesc(&desc)) || !desc.OutputWindow || FAILED(chain->GetDevice(IID_PPV_ARGS(&s.device)))) {s.reason="swapchain_unavailable";return false;}
    s.window=desc.OutputWindow;
    seatinput::Policy policy;
    policy.blocksKey=[&s](unsigned key){return s.input.BlocksKey(key);};
    policy.filterXbox=[&s](unsigned slot,WORD& buttons){s.input.FilterXbox(slot,buttons);};
    seatinput::Install(std::move(policy),s.log);s.filters=seatinput::Installed();
    s.windowReady=seatinput::AttachWindow(s.window);
    if(!s.filters.keyboard || !s.windowReady) {s.reason="input_filter_unavailable";return false;}
    s.graphicsReady=s.presenter.Initialize(s.device.Get(),draw);
    if(!s.graphicsReady) {s.reason="panel_presenter_unavailable";return false;}
    if(s.filters.xinput)if(auto library=GetModuleHandleW(L"xinput1_3.dll"))s.readPad=reinterpret_cast<ReadPad>(GetProcAddress(library,"XInputGetState"));
    s.enabled=true;s.reason="waiting_for_local_cockpit";return true;
}
unsigned Held(State& s) {
    bool keyboardSide{};auto held=KeyboardHeld(Key,keyboardSide);bool side=keyboardSide;
    std::array<WORD,4> buttons{};
    if(s.readPad)for(unsigned slot=0;slot<buttons.size();++slot) {
        XINPUT_STATE pad{};if(s.readPad(slot,&pad)==ERROR_SUCCESS)buttons[slot]=pad.Gamepad.wButtons;
        side=side || (buttons[slot]&XINPUT_GAMEPAD_X)!=0;
    }
    if(side && !keyboardSide) {
        if(held&Back){held=(held&~Back)|Left;}
        if(held&Forward){held=(held&~Forward)|Right;}
    }
    for(auto value:buttons){bool ignored{};held|=XboxHeld(static_cast<WORD>(value|(side?XINPUT_GAMEPAD_X:0)),ignored);}
    s.sideways=side;return held;
}
bool Foreground(State& s) {
    const auto window=GetForegroundWindow();DWORD process{};
    return window && GetWindowThreadProcessId(window,&process) && process==GetCurrentProcessId() && GetAncestor(window,GA_ROOT)==GetAncestor(s.window,GA_ROOT) && !IsIconic(s.window);
}
void DrawDesktop(State& s,IDXGISwapChain* chain,ID3D11Texture2D* image) {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    if(FAILED(chain->GetBuffer(0,IID_PPV_ARGS(&back))))throw std::runtime_error("backbuffer");
    D3D11_TEXTURE2D_DESC desc{};back->GetDesc(&desc);
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    if(FAILED(s.device->CreateRenderTargetView(back.Get(),nullptr,&target)))throw std::runtime_error("panel_target");
    const unsigned height=desc.Height*3/5,width=height*3/4;
    if(!s.presenter.DrawRegion(image,target.Get(),desc.Height/20,(desc.Height-height)/2,width,height,SeatPresenter::Mode::Composite))throw std::runtime_error("panel_draw");
}
}
void Prepare(uintptr_t base,const std::filesystem::path& output,void (*log)(const char*,...)) noexcept {
    try {std::call_once(prepared,[&] {
        const auto path=output/L"seat-menu.request";
        if(!Plain(path) || !std::filesystem::is_regular_file(path) || !std::filesystem::is_regular_file(output/L"runtime-session.request"))return;
        std::array<char,8193> bytes{};std::ifstream input(path,std::ios::binary);input.read(bytes.data(),bytes.size());
        const auto length=static_cast<size_t>(input.gcount());SessionRequest request;
        if(!ParseSession(std::string_view(bytes.data(),length),request))return;
        const bool xr=std::filesystem::is_regular_file(output/L"xr-session.request"),projection=std::filesystem::is_regular_file(output/L"xr-projection.request");
        if(request.desktop?(xr || projection):(!xr || !projection))return;
        const auto digest=gameexe::Digest(reinterpret_cast<const unsigned char*>(bytes.data()),static_cast<ULONG>(length));
        if(digest.size()!=64)return;
        auto* next=new State(base,output,request,digest,log);state.store(next);Receipt(*next,true);
    });}catch(...){ }
}
void Present(IDXGISwapChain* chain,NativeEyeDraw draw) noexcept {
    auto* s=state.load();if(!s || s->stopped || s->failed || !chain)return;
    try {
        if(!s->thread && !Initialize(*s,chain,draw)) {s->failed=true;s->input.Stop();Receipt(*s,true);return;}
        if(s->thread!=GetCurrentThreadId())return;
        ++s->frames;const auto held=Held(*s);const auto now=GetTickCount64();const bool front=Foreground(*s),wasOpen=s->menu.Opened();
        const bool ready=s->graphicsReady && (s->request.desktop || s->xrReady);
        const bool camera=s->camera.Update([&](size_t car,bool changed) {
            if(changed) {s->menu.TakeCommit();s->menu.Select(static_cast<int>(car),s->committed.Saved(car),s->committed.Fov(),s->request.desktop,s->request.positions);}
            s->menu.Tick(held,now,front,true,ready);
            return CameraSettings{car,s->request.positions?s->menu.Current():Position{},s->request.desktop?s->menu.Fov():0,s->request.desktop,s->request.hideDriver};
        });
        if(!camera) {s->menu.Deactivate();s->menu.TakeCommit();}
        if(camera)if(const auto commit=s->menu.TakeCommit()) {
            const auto previous=s->committed;
            if(!s->committed.Save(*commit) || !WriteCommit(*s)) {
                s->committed=previous;s->menu.RejectSave(previous.Saved(static_cast<size_t>(commit->car)),previous.Fov());s->saveFailed=true;
            } else {++s->saves;s->saveFailed=false;}
        }
        s->input.Set(camera && ready && front,s->menu.Opened(),s->request.desktop);
        if(s->menu.Opened() && !wasOpen)++s->openings;
        if(s->menu.Opened()) {
            const auto name=CarLabel(static_cast<size_t>(s->menu.Car()));
            auto* image=s->panel.Render(s->device.Get(),name,s->menu,s->sideways,s->saveFailed?L"Save failed; Enter retries, Esc cancels":L"");
            if(!image)throw std::runtime_error("panel_image");
            if(s->request.desktop)DrawDesktop(*s,chain,image);
        }
        s->reason=s->saveFailed?"seat_save_failed":camera?"local_cockpit_ready":"waiting_for_local_cockpit";Receipt(*s);
    }catch(...) {s->failed=true;s->reason="seat_runtime_failed";s->menu.Deactivate();s->input.Stop();Receipt(*s,true);}
}
void Stop() noexcept {
    auto* s=state.load();if(!s || s->stopped)return;
    try {s->menu.Deactivate();s->input.Stop();seatinput::Stop();s->camera.Stop();s->enabled=false;s->stopped=true;s->reason="stopped";Receipt(*s,true);}catch(...){ }
}
bool Requested(){return state.load()!=nullptr;}
bool Opened(){const auto* s=state.load();return s && s->enabled && !s->failed && !s->stopped && s->menu.Opened();}
uint64_t Opening(){const auto* s=state.load();return s?s->openings:0;}
ID3D11Texture2D* Texture() {
    auto* s=state.load();if(!Opened() || s->thread!=GetCurrentThreadId())return nullptr;
    return s->panel.Render(s->device.Get(),CarLabel(static_cast<size_t>(s->menu.Car())),s->menu,s->sideways,s->saveFailed?L"Save failed; Enter retries, Esc cancels":L"");
}
void XrPanelReady(bool ready){if(auto* s=state.load())s->xrReady=ready;}
}
