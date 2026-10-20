#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include "scene_loader.hpp"
#include "budget.hpp"
#include "nativegpu_version.h"
#include "../renderer/d3d12_renderer.h"
#include "../renderer/rhi.hpp"
#include "../engine/native/math.hpp"
#include "../third_party/json.hpp"
#include <chrono>
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdint>
#include <vector>
#include <cmath>
#include <filesystem>

using Clock=std::chrono::steady_clock;
using json=nlohmann::json;
namespace {
// Atomic scene replacement changes the file identity; polling both size and
// write time keeps hot reload deterministic without a filesystem watcher.
struct SceneStamp {
    std::filesystem::file_time_type modified{};
    std::uintmax_t size=0;
    bool valid=false;
};
// One recorded frame. Inputs are stored as exact float bit patterns so replay cannot
// drift by a rounding step: a decimal or milli-unit encoding would change the state.
struct ReplayFrame {
    std::uint64_t hash=0;
    float forward=0.0f,right=0.0f,yaw=0.0f,pitch=0.0f;
    bool run=false,jump=false,interact=false;
};
SceneStamp stampOf(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size=std::filesystem::file_size(path,ec);
    if(ec)return {};
    const auto modified=std::filesystem::last_write_time(path,ec);
    if(ec)return {};
    return {modified,size,true};
}
bool unchanged(const SceneStamp& a,const SceneStamp& b){
    return a.valid&&b.valid&&a.modified==b.modified&&a.size==b.size;
}
struct App {
    HWND window=nullptr,viewport=nullptr,title=nullptr,mission=nullptr,stats=nullptr,list=nullptr,pauseButton=nullptr;
    HFONT font=nullptr,titleFont=nullptr;
    HBRUSH background=nullptr;
    nativegpu::Renderer renderer;
    // Frame path goes through the RHI boundary; the raw renderer stays for test hooks.
    std::unique_ptr<nativegpu::rhi::Device> rhi;
    host::SceneDocument document;
    engine::Game game;
    std::filesystem::path scenePath="assets/courtyard.scene.json", reportPath="reports/native-session.json";
    std::wstring titleText=L"NativeGPU | 原生开发预览",liveStatus;
    std::string error,sceneError;
    bool paused=false,running=true,ready=false,minimized=false,dragging=false,smoke=false,watch=true,probe=false;
    bool resizeTest=false,restartTest=false,jumpDown=false,interactDown=false;
    // DXR static shadow toggle. `rtShadowRequested` is what the caller asked for;
    // `rtShadowEnabled` is what the renderer could actually enable after creation.
    bool rtShadowRequested=true,rtShadowEnabled=false;
    bool screenshotRequested=false,screenshotTaken=false,screenshotAttempted=false;
    int mouseX=0,mouseY=0;
    float yawDelta=0,pitchDelta=0;
    unsigned width=960,height=720,targetFrames=360,frames=0,restarts=0,resizes=0,hotReloads=0;
    unsigned forcedWidth=0,forcedHeight=0,screenshotFrame=150,debugView=0;
    bool validationRequested=false;
    // G1 reliability modes.
    bool deviceRemovalMode=false,deviceRemovalDetected=false,deviceRemovalRecovered=false;
    double deviceRemovalAtFrame=0.0;
    unsigned deviceRemovalFrame=0,deviceRemovalRecoveries=0,deviceRemovalFrameAfterRecovery=0;
    std::string deviceRemovalError;
    unsigned lifecycleCycles=0,lifecycleDone=0;
    bool faultTest=false,faultMaterial=false,noStress=false,meshRequested=true;
    std::string faultScenePath,errorCode;
    std::vector<nlohmann::json> faultTestCases,lifecycleEntries;
    bool hasCameraOverride=false;
    float cameraEye[3]={0,0,0},cameraTarget[3]={0,0,0};
    std::string screenshotPath,screenshotError;
    SceneStamp sceneStamp;
    std::vector<double> cpuTimes,cpuSubmitTimes,gpuTimes,gpuRtTimes,frameTimes;
    // Determinism: a run seed, an optional per-frame digest log, and the final digest.
    std::uint64_t seed=20261020;
    bool hashEnabled=false;
    std::string hashLogPath,finalHash;
    std::ofstream hashLog;
    unsigned long long hashLines=0;
    std::uint64_t lastDigest=0;
    // Replay and black-box recorder (E08).
    std::string recordPath,replayPath;
    bool replayEnabled=false,replayStrict=false,recordEnabled=false;
    std::vector<ReplayFrame> replayFrames;
    std::size_t replayIndex=0;
    unsigned replayMismatches=0,replayFirstMismatch=0,replayCompared=0;
    bool blackboxTriggered=false,blackboxWritten=false;
    unsigned blackboxFrame=0,stallFrame=0,stallMs=0;
    double blackboxDtMs=0.0,blackboxStallMs=25.0;
    // Frame budget governor. Disabled unless --budget auto is passed.
    host::BudgetGovernor budget;
    bool computeRequested=false,computeCorrupt=false;
    bool budgetMode=false;
    double budgetTargetMs=16.6,budgetCpuMs=0.0;
    unsigned long long budgetVramMb=0;
    unsigned budgetWindow=120,budgetSustain=30;
    Clock::time_point began=Clock::now(),last=began,lastUi=began,lastWatch=began;
} app;
constexpr int ID_PAUSE=101,ID_RESET=102,ID_RELOAD=103,ID_OPEN=104,ID_RESTART=105;
std::wstring wide(const std::string& s) {
    if(s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0);
    std::wstring out(static_cast<size_t>(n),L'\0');
    MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),out.data(),n);
    return out;
}
// Parses "a,b,c" into up to `max` floats and returns how many were found.
int parseFloatList(const std::wstring& text, float* out, int max) {
    int found = 0;
    size_t start = 0;
    for (size_t index = 0; index <= text.size() && found < max; ++index) {
        if (index == text.size() || text[index] == L',') {
            if (index > start) {
                out[found++] = static_cast<float>(_wtof(text.substr(start, index - start).c_str()));
            }
            start = index + 1;
        }
    }
    return found;
}
// UTF-8 for command-line paths that are handed to the renderer and the JSON report.
std::string narrow(const wchar_t* s) {
    if(!s||!*s) return {};
    const int length=static_cast<int>(wcslen(s));
    const int n=WideCharToMultiByte(CP_UTF8,0,s,length,nullptr,0,nullptr,nullptr);
    if(n<=0) return {};
    std::string out(static_cast<size_t>(n),'\0');
    WideCharToMultiByte(CP_UTF8,0,s,length,out.data(),n,nullptr,nullptr);
    return out;
}
void setText(HWND h,const std::wstring& text){SetWindowTextW(h,text.c_str());}
unsigned floatBits(float value){unsigned bits=0;std::memcpy(&bits,&value,sizeof(bits));return bits;}
float bitsFloat(unsigned bits){float value=0.0f;std::memcpy(&value,&bits,sizeof(value));return value;}
// One line per simulated frame: frame,tick,hash,input bits. The input columns are the
// difference between a digest log and a replayable recording.
void writeReplayLine(const engine::Input& input){
    // The digest is always computed: replay compares it even when this run writes no log.
    engine::determinism::Hasher hasher;
    app.game.digest(hasher);
    app.lastDigest=hasher.value();
    app.finalHash=hasher.hex();
    if(!app.hashEnabled||!app.hashLog.is_open())return;
    char buffer[192];
    std::snprintf(buffer,sizeof(buffer),"%u,%llu,%016llx,%08x,%08x,%d,%d,%d,%08x,%08x",
                  app.frames,static_cast<unsigned long long>(app.game.ticks()),
                  static_cast<unsigned long long>(app.lastDigest),
                  floatBits(input.forward),floatBits(input.right),
                  input.run?1:0,input.jump?1:0,input.interact?1:0,
                  floatBits(input.yawDelta),floatBits(input.pitchDelta));
    app.hashLog<<buffer<<'\n';
    app.hashLog.flush();
    ++app.hashLines;
}
bool loadReplay(const std::string& path,std::uint64_t& seed,std::string& error){
    std::ifstream in(path,std::ios::binary);
    if(!in){error="cannot open replay: "+path;return false;}
    std::string line;
    while(std::getline(in,line)){
        if(line.empty())continue;
        if(line[0]=='#'){
            const auto position=line.find("seed=");
            if(position!=std::string::npos){
                seed=std::strtoull(line.c_str()+position+5,nullptr,10);
            }
            continue;
        }
        unsigned frame=0,forward=0,right=0,yaw=0,pitch=0;
        unsigned long long tick=0,hash=0;
        int run=0,jump=0,interact=0;
        if(std::sscanf(line.c_str(),"%u,%llu,%llx,%x,%x,%d,%d,%d,%x,%x",
                       &frame,&tick,&hash,&forward,&right,&run,&jump,&interact,
                       &yaw,&pitch)!=10){
            error="malformed replay line: "+line;
            return false;
        }
        ReplayFrame recorded;
        recorded.hash=hash;
        recorded.forward=bitsFloat(forward);
        recorded.right=bitsFloat(right);
        recorded.yaw=bitsFloat(yaw);
        recorded.pitch=bitsFloat(pitch);
        recorded.run=run!=0;
        recorded.jump=jump!=0;
        recorded.interact=interact!=0;
        app.replayFrames.push_back(recorded);
    }
    if(app.replayFrames.empty()){
        error="replay contains no frames: "+path;
        return false;
    }
    return true;
}
// Anomaly sidecar written the moment a frame exceeds the stall threshold. It carries the
// recording path, so the recording itself stays a plain replay log.
void writeAnomaly(){
    if(!app.recordEnabled||app.blackboxWritten||app.recordPath.empty())return;
    app.blackboxWritten=true;
    const std::filesystem::path path(app.recordPath+".anomaly.json");
    std::ofstream out(path,std::ios::binary);
    if(!out)return;
    out<<"{\n  \"schema\": \"nativegpu.blackbox/1\",\n"
       <<"  \"trigger\": \"stall\",\n"
       <<"  \"frame\": "<<app.blackboxFrame<<",\n"
       <<"  \"tick\": "<<app.game.ticks()<<",\n"
       <<"  \"dtMs\": "<<app.blackboxDtMs<<",\n"
       <<"  \"thresholdMs\": "<<app.blackboxStallMs<<",\n"
       <<"  \"hash\": \""<<app.finalHash<<"\",\n"
       <<"  \"recording\": \""<<app.recordPath<<"\",\n"
       <<"  \"replayCommand\": \"--replay "<<app.recordPath<<" --replay-strict\"\n}\n";
}
HWND label(const wchar_t* text,int x,int y,int w,int h,DWORD extra=0) {
    auto control=CreateWindowExW(0,L"STATIC",text,WS_CHILD|WS_VISIBLE|extra,x,y,w,h,app.window,nullptr,nullptr,nullptr);
    SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(app.font),TRUE);
    return control;
}
HWND button(const wchar_t* text,int id,int x,int y,int w=128) {
    auto control=CreateWindowExW(0,L"BUTTON",text,WS_CHILD|WS_VISIBLE|WS_TABSTOP,x,y,w,34,app.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
    SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(app.font),TRUE);
    return control;
}
void refreshList() {
    SendMessageW(app.list,LB_RESETCONTENT,0,0);
    for(const auto& e:app.document.scene.entities){
        auto text=wide(e.name+"  ["+e.kind+"]");
        SendMessageW(app.list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));
    }
    setText(app.title,wide(app.document.name)+L"\nD3D12 · 原生基线 0.3");
}
void adoptScene(host::SceneDocument next,const std::filesystem::path& path) {
    app.document=std::move(next);app.scenePath=path;
    app.game.initialize(app.document.scene);
    app.error.clear();app.sceneError.clear();app.errorCode.clear();app.sceneStamp=stampOf(path);
    if(app.list)refreshList();
}
void diagnostic(const std::string& error) {
    app.error=error;
    if(!app.smoke) MessageBoxW(app.window,wide(error).c_str(),L"NativeGPU 诊断",MB_OK|MB_ICONERROR);
}
bool reloadScene(const std::filesystem::path& path) {
    host::SceneDocument next;
    std::string error;
    if(!host::loadScene(path,next,error)){app.errorCode="NG-E-IO-SCENE";diagnostic(error);return false;}
    adoptScene(std::move(next),path);
    app.liveStatus=L"已重载 revision "+std::to_wstring(app.document.revision);
    return true;
}
// A failed hot reload must leave the playable scene untouched and must not
// block the message loop with a modal dialog. The next valid write retries.
void pollScene(){
    if(!app.watch||app.scenePath.empty())return;
    const auto now=Clock::now();
    if(std::chrono::duration<double>(now-app.lastWatch).count()<0.4)return;
    app.lastWatch=now;
    const SceneStamp stamp=stampOf(app.scenePath);
    if(!stamp.valid||unchanged(stamp,app.sceneStamp))return;
    host::SceneDocument next;
    std::string error;
    if(!host::loadScene(app.scenePath,next,error)){
        app.sceneStamp=stamp;
        app.sceneError="NG-E-IO-HOTRELOAD: "+error;
        app.liveStatus=L"热重载失败，保留上一有效场景";
        return;
    }
    if(next.projectId!=app.document.projectId){
        app.sceneStamp=stamp;
        app.sceneError="hot reload refused: projectId changed";
        app.liveStatus=L"热重载被拒绝：项目 ID 不一致";
        return;
    }
    adoptScene(std::move(next),app.scenePath);
    ++app.hotReloads;
    app.liveStatus=L"场景已热重载 revision "+std::to_wstring(app.document.revision);
}
bool restartRenderer(){
    app.ready=false;
    if(app.rhi)app.rhi->shutdown();
    nativegpu::rhi::Descriptor descriptor;
    descriptor.window=app.viewport;
    descriptor.width=app.width;
    descriptor.height=app.height;
    descriptor.validation=app.validationRequested;
    if(!app.rhi||!app.rhi->initialize(descriptor,app.error)){
        app.errorCode="NG-E-RENDERER-INIT";diagnostic(app.error);app.running=false;return false;
    }
    // The DXR state object is rebuilt from scratch, so the requested toggle has to be
    // re-applied and re-read for the report.
    app.rhi->setRtShadows(app.rtShadowRequested);
    app.rhi->setCompositeDebugView(app.debugView);
    app.rhi->setMeshShaders(app.meshRequested);
    app.rhi->setCompute(app.computeRequested,app.computeCorrupt);
    // Injection must happen after initialize(), which rebuilds the renderer state. It is
    // armed exactly once so the recovery run is a clean renderer.
    if(app.deviceRemovalMode&&app.deviceRemovalAtFrame>0&&!app.deviceRemovalDetected){
        app.renderer.injectDeviceRemovalAtFrame(static_cast<unsigned>(app.deviceRemovalAtFrame));
    }
    app.rtShadowEnabled=app.rhi->rtShadowsEnabled();
    app.ready=true;++app.restarts;return true;
}
void layout(){
    if(!app.viewport)return;
    RECT r;GetClientRect(app.window,&r);
    int panel=310;
    app.width=static_cast<unsigned>(std::max(160L,r.right-panel));
    app.height=static_cast<unsigned>(std::max(160L,r.bottom));
    // --render-size pins the render resolution for reproducible timing and screenshots.
    if(app.forcedWidth&&app.forcedHeight){app.width=app.forcedWidth;app.height=app.forcedHeight;}
    MoveWindow(app.viewport,0,0,app.width,app.height,TRUE);
    const int x=static_cast<int>(app.width)+20;
    MoveWindow(app.title,x,20,270,60,TRUE);
    MoveWindow(app.mission,x,95,270,95,TRUE);
    MoveWindow(app.stats,x,200,270,108,TRUE);
    MoveWindow(app.list,x,318,270,std::max(100L,r.bottom-500),TRUE);
    for(auto id:{ID_PAUSE,ID_RESET,ID_RELOAD,ID_OPEN,ID_RESTART}){
        HWND h=GetDlgItem(app.window,id);
        int row=(id-ID_PAUSE)/2, col=(id-ID_PAUSE)%2;
        if(id==ID_RESTART){row=2;col=0;}
        MoveWindow(h,x+col*140,r.bottom-150+row*44,id==ID_RESTART?268:128,34,TRUE);
    }
    // Static labels carry stable IDs so resize keeps help text readable.
    MoveWindow(GetDlgItem(app.window,201),x,296,270,20,TRUE);
    MoveWindow(GetDlgItem(app.window,202),16,r.bottom-28,static_cast<int>(app.width)-32,22,TRUE);
    if(app.ready && !app.minimized){
        if(!app.rhi->resize(app.width,app.height,app.error)){
            diagnostic(app.error);app.running=false;
        } else ++app.resizes;
    }
}
LRESULT CALLBACK viewportProc(HWND h,UINT msg,WPARAM w,LPARAM l) {
    if(msg==WM_RBUTTONDOWN){
        app.dragging=true;app.mouseX=GET_X_LPARAM(l);app.mouseY=GET_Y_LPARAM(l);
        SetCapture(h);SetFocus(app.window);return 0;
    }
    if(msg==WM_RBUTTONUP){app.dragging=false;ReleaseCapture();return 0;}
    if(msg==WM_CAPTURECHANGED){app.dragging=false;return 0;}
    if(msg==WM_MOUSEMOVE && app.dragging){
        int x=GET_X_LPARAM(l),y=GET_Y_LPARAM(l);
        app.yawDelta+=(x-app.mouseX)*.005f;app.pitchDelta+=(y-app.mouseY)*.005f;
        app.mouseX=x;app.mouseY=y;return 0;
    }
    if(msg==WM_LBUTTONDOWN){SetFocus(app.window);return 0;}
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_PAINT){PAINTSTRUCT ps;BeginPaint(h,&ps);EndPaint(h,&ps);return 0;}
    return DefWindowProcW(h,msg,w,l);
}
LRESULT CALLBACK windowProc(HWND h,UINT msg,WPARAM w,LPARAM l){
    switch(msg){
    case WM_SIZE:
        app.minimized=w==SIZE_MINIMIZED;
        if(!app.minimized)layout();
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize={900,700};return 0;
    case WM_COMMAND:
        switch(LOWORD(w)){
        case ID_PAUSE:
            app.paused=!app.paused;setText(app.pauseButton,app.paused?L"继续运行":L"暂停运行");break;
        case ID_RESET:app.game.reset();break;
        case ID_RELOAD:reloadScene(app.scenePath);break;
        case ID_RESTART:restartRenderer();break;
        case ID_OPEN:{
            wchar_t file[32768]={};
            OPENFILENAMEW ofn={};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=h;
            ofn.lpstrFilter=L"场景 JSON\0*.json\0";ofn.lpstrFile=file;ofn.nMaxFile=32768;
            ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
            if(GetOpenFileNameW(&ofn))reloadScene(std::filesystem::path(file));
            break;
        }}
        SetFocus(h);return 0;
    case WM_KEYDOWN:
        if(w==VK_F5)app.game.reset();
        if(w==VK_F6)SendMessageW(h,WM_COMMAND,ID_PAUSE,0);
        if(w==VK_F8)restartRenderer();
        if(w==VK_F9){
            app.watch=!app.watch;app.lastWatch=Clock::now();
            app.liveStatus=app.watch?L"热重载已开启":L"热重载已关闭";
        }
        if(w==VK_F11){
            app.meshRequested=!app.meshRequested;
            app.rhi->setMeshShaders(app.meshRequested);
            app.liveStatus=app.rhi->meshShadersEnabled()?L"Mesh Shader 路径已开启":L"Mesh Shader 已关闭（顶点路径）";
        }
        if(w==VK_F10){
            app.rtShadowRequested=!app.rtShadowRequested;
            app.rhi->setRtShadows(app.rtShadowRequested);
            app.rtShadowEnabled=app.rhi->rtShadowsEnabled();
            app.liveStatus=app.rtShadowEnabled?L"DXR 阴影已开启":L"DXR 阴影已关闭（光栅回退）";
        }
        return 0;
    case WM_ACTIVATE:
        if(LOWORD(w)==WA_INACTIVE){app.dragging=false;ReleaseCapture();app.yawDelta=0;app.pitchDelta=0;}
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(w),TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(w),RGB(218,226,236));
        return reinterpret_cast<LRESULT>(app.background);
    case WM_ERASEBKGND:{
        RECT r;GetClientRect(h,&r);FillRect(reinterpret_cast<HDC>(w),&r,app.background);return 1;
    }
    case WM_CLOSE:app.running=false;return 0;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(h,msg,w,l);
}
bool down(int key){return (GetAsyncKeyState(key)&0x8000)!=0;}
void updateUi(){
    const auto p=app.game.playerPosition();
    std::wstring task=L"任务  探索小院\n";
    task+=app.game.hasKey()?L"✓ 已拾取钥匙\n":L"○ 靠近金色钥匙，按 E 拾取\n";
    task+=app.game.doorOpen()?L"✓ 门已开启\n":L"○ 携带钥匙，靠近门按 E\n";
    task+=app.game.complete()?L"✓ 已到达终点！F5 重新体验":L"○ 穿过门，进入绿色终点";
    setText(app.mission,task);
    std::wostringstream s;
    auto stats=app.rhi->stats();
    s<<std::fixed<<std::setprecision(2)<<L"CPU 提交 "<<stats.cpuMs<<L" ms   GPU ";
    if(stats.gpuValid)s<<stats.gpuMs<<L" ms";else s<<L"采样中";
    s<<L"\n位置 "<<p.x<<L", "<<p.y<<L", "<<p.z;
    s<<L"\n"<<(app.paused?L"已暂停":L"运行中")<<L"  ·  revision "<<app.document.revision;
    s<<L"  ·  "<<(app.watch?L"热重载开":L"热重载关");
    s<<L"\nDXR 阴影 "<<(app.rtShadowEnabled?L"开启":L"关闭 / 回退");
    if(!app.liveStatus.empty())s<<L"\n"<<app.liveStatus;
    if(!app.sceneError.empty())s<<L"\n错误 "<<wide(app.sceneError);
    setText(app.stats,s.str());
}
double percentile(std::vector<double> x,double p){
    if(x.empty())return 0;
    std::sort(x.begin(),x.end());
    return x[static_cast<size_t>(std::ceil((x.size()-1)*p))];
}
std::string hex64(unsigned long long value){
    char buffer[17];
    std::snprintf(buffer,sizeof(buffer),"%016llx",value);
    return std::string(buffer);
}
void report(bool success){
    try{
        json r={
            {"version",NATIVEGPU_VERSION},{"success",success},{"error",app.error},
            {"build",{{"commit",NATIVEGPU_BUILD_COMMIT},{"dirty",NATIVEGPU_BUILD_DIRTY!=0}}},
            {"mode",app.probe?"probe":(app.smoke?"automated-smoke":"interactive")},
            {"frames",app.frames},{"elapsedSeconds",std::chrono::duration<double>(Clock::now()-app.began).count()},
            {"scene",app.scenePath.u8string()},{"revision",app.document.revision},
            {"resolution",{app.width,app.height}},{"rendererReinitializations",app.restarts},
            {"resizeEvents",app.resizes},{"watchEnabled",app.watch},{"hotReloads",app.hotReloads},
            {"lastSceneError",app.sceneError},{"deviceRemovalTested",app.deviceRemovalMode},
            {"errorCode",app.errorCode},
            {"deviceRemoval",{
                {"tested",app.deviceRemovalMode},
                {"mode",app.deviceRemovalMode?"injected-fault":"none"},
                {"requestedFrame",app.deviceRemovalAtFrame},
                {"detected",app.deviceRemovalDetected},
                {"detectedAtFrame",app.deviceRemovalFrame},
                {"recovered",app.deviceRemovalRecovered},
                {"recoveries",app.deviceRemovalRecoveries},
                {"frameAfterRecovery",app.deviceRemovalFrameAfterRecovery},
                {"error",app.deviceRemovalError}}},
            {"lifecycle",{
                {"requested",app.lifecycleCycles},
                {"completed",app.lifecycleDone},
                {"entries",app.lifecycleEntries}}},
            {"faultTests",app.faultTestCases},
            {"cpuP50Ms",percentile(app.cpuTimes,.5)},
            {"cpuSubmitP50Ms",app.cpuSubmitTimes.empty()?json(nullptr):json(percentile(app.cpuSubmitTimes,.5))},
            {"cpuSubmitP95Ms",app.cpuSubmitTimes.empty()?json(nullptr):json(percentile(app.cpuSubmitTimes,.95))},
            {"gpuP50Ms",app.gpuTimes.empty()?json(nullptr):json(percentile(app.gpuTimes,.5))},
            {"gpuRtP50Ms",app.gpuRtTimes.empty()?json(nullptr):json(percentile(app.gpuRtTimes,.5))},
            {"gpuRtP95Ms",app.gpuRtTimes.empty()?json(nullptr):json(percentile(app.gpuRtTimes,.95))},
            {"gpuP95Ms",app.gpuTimes.empty()?json(nullptr):json(percentile(app.gpuTimes,.95))},
            {"frameIntervalP95Ms",percentile(app.frameTimes,.95)},
            {"gpuSamples",app.gpuTimes.size()},
            {"game",{{"hasKey",app.game.hasKey()},{"doorOpen",app.game.doorOpen()},{"complete",app.game.complete()}}},
            {"rtShadows",{
                {"requested",app.rtShadowRequested},
                {"enabled",app.rtShadowEnabled},
                {"implemented",app.renderer.rtShadowsImplemented()},
                {"screenshotRequested",app.screenshotRequested},
                {"screenshotTaken",app.screenshotTaken},
                {"screenshotFrame",app.screenshotFrame},
                {"screenshotPath",app.screenshotPath},
                {"screenshotError",app.screenshotError}}},
            {"capabilities",app.rhi?json::parse(app.rhi->capabilitiesJson()):json(nullptr)},
            {"probes",app.rhi?json::parse(app.rhi->probesJson()):json(nullptr)}
        };
        json rhiBackends=json::array();
        for(const auto& name:nativegpu::rhi::backends())rhiBackends.push_back(name);
        r["rhi"]={
            {"backend",app.rhi?app.rhi->backend():"none"},
            {"supported",rhiBackends},
            {"framePathThroughInterface",app.rhi!=nullptr},
            {"nativeHandleUsedForTestHooks",true}
        };
        json rngStreams=json::array();
        for(const auto& stream:engine::determinism::registry().streams()){
            rngStreams.push_back(json{{"name",stream.name()},{"counter",stream.counter()}});
        }
        r["determinism"]={
            {"seed",app.seed},
            {"fixedStepHz",120},
            {"ticks",app.game.ticks()},
            {"hashLog",app.hashLogPath},
            {"hashLines",app.hashLines},
            {"finalHash",app.finalHash},
            {"rng",{{"streams",rngStreams},
                    {"fingerprint",hex64(engine::determinism::registry().fingerprint())}}}
        };
        json budgetLadder=json::array();
        for(const auto& step:app.budget.config().steps)budgetLadder.push_back(step);
        json budgetSteps=json::array();
        for(const auto& step:app.budget.steps()){
            budgetSteps.push_back(json{
                {"name",step.name},{"frame",step.frame},{"reason",step.reason},
                {"gpuMedianBeforeMs",step.gpuMedianBefore},{"cpuMedianBeforeMs",step.cpuMedianBefore}});
        }
        r["budget"]={
            {"enabled",app.budget.config().enabled},
            {"status",app.budget.status()},
            {"level",app.budget.level()},
            {"targetGpuMs",app.budget.config().targetGpuMs},
            {"windowFrames",app.budget.config().windowFrames},
            {"sustainFrames",app.budget.config().sustainFrames},
            {"samples",app.budget.samples()},
            {"gpuSamples",app.budget.gpuSamples()},
            {"overBudgetFrames",app.budget.overBudgetFrames()},
            {"gpuP95Ms",app.budget.samples()?json(app.budget.gpuP95()):json(nullptr)},
            {"cpuP95Ms",app.budget.samples()?json(app.budget.cpuP95()):json(nullptr)},
            {"vramPeakBytes",app.budget.vramPeak()},
            {"ladder",budgetLadder},
            {"steps",budgetSteps}
        };
        r["replay"]={
            {"enabled",app.replayEnabled},
            {"source",app.replayPath},
            {"frames",static_cast<unsigned>(app.replayFrames.size())},
            {"compared",app.replayCompared},
            {"mismatches",app.replayMismatches},
            {"firstMismatchFrame",app.replayFirstMismatch},
            {"strict",app.replayStrict}
        };
        r["blackbox"]={
            {"enabled",app.recordEnabled},
            {"path",app.recordPath},
            {"triggered",app.blackboxTriggered},
            {"frame",app.blackboxFrame},
            {"dtMs",app.blackboxDtMs},
            {"thresholdMs",app.blackboxStallMs},
            {"anomalyWritten",app.blackboxWritten}
        };
        if(!app.reportPath.parent_path().empty())std::filesystem::create_directories(app.reportPath.parent_path());
        std::ofstream out(app.reportPath,std::ios::binary);out<<r.dump(2)<<"\n";
    }catch(...){/* Reporting must not hide the original device diagnostic. */}
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show){
    SetProcessDPIAware();
    int argc=0;
    auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    bool explicitScene=false,watchExplicit=false;
    for(int i=1;i<argc;++i){
        std::wstring arg=argv[i];
        if(arg==L"--smoke")app.smoke=true;
        else if(arg==L"--probe"){app.probe=true;app.smoke=true;}
        else if(arg==L"--scene"&&i+1<argc){app.scenePath=argv[++i];explicitScene=true;}
        else if(arg==L"--report"&&i+1<argc)app.reportPath=argv[++i];
        else if(arg==L"--frames"&&i+1<argc)app.targetFrames=std::max(240,_wtoi(argv[++i]));
        else if(arg==L"--watch"){app.watch=true;watchExplicit=true;}
        else if(arg==L"--no-watch"){app.watch=false;watchExplicit=true;}
        else if(arg==L"--validation")app.validationRequested=true;
        else if(arg==L"--device-removal-after"&&i+1<argc){
            app.deviceRemovalMode=true;app.smoke=true;
            app.deviceRemovalAtFrame=static_cast<double>(_wtoi(argv[++i]));
        }
        else if(arg==L"--lifecycle"&&i+1<argc){
            app.lifecycleCycles=static_cast<unsigned>(std::max(1,_wtoi(argv[++i])));app.smoke=true;
        }
        else if(arg==L"--fault-test"){app.faultTest=true;app.smoke=true;app.faultMaterial=true;}
        else if(arg==L"--no-stress")app.noStress=true;
        else if(arg==L"--seed"&&i+1<argc)app.seed=_wcstoui64(argv[++i],nullptr,10);
        else if(arg==L"--hash-log"&&i+1<argc)app.hashLogPath=narrow(argv[++i]);
        else if(arg==L"--record"&&i+1<argc){
            app.recordPath=narrow(argv[++i]);
            app.hashLogPath=app.recordPath;
            app.recordEnabled=true;
        }
        else if(arg==L"--replay"&&i+1<argc){
            app.replayPath=narrow(argv[++i]);
            app.replayEnabled=true;
            app.smoke=true;
        }
        else if(arg==L"--replay-strict")app.replayStrict=true;
        else if(arg==L"--compute"&&i+1<argc){
            const std::wstring mode=argv[++i];
            app.computeRequested=(mode!=L"off");
            app.computeCorrupt=(mode==L"corrupt");
        }
        else if(arg==L"--stall-frame"&&i+1<argc)app.stallFrame=static_cast<unsigned>(std::max(0,_wtoi(argv[++i])));
        else if(arg==L"--stall-ms"&&i+1<argc)app.stallMs=static_cast<unsigned>(std::max(1,_wtoi(argv[++i])));
        else if(arg==L"--blackbox-stall-ms"&&i+1<argc)app.blackboxStallMs=_wtof(argv[++i]);
        else if(arg==L"--budget"&&i+1<argc)app.budgetMode=(std::wstring(argv[++i])!=L"off");
        else if(arg==L"--budget-target-ms"&&i+1<argc)app.budgetTargetMs=_wtof(argv[++i]);
        else if(arg==L"--budget-cpu-ms"&&i+1<argc)app.budgetCpuMs=_wtof(argv[++i]);
        else if(arg==L"--budget-vram-mb"&&i+1<argc)app.budgetVramMb=_wcstoui64(argv[++i],nullptr,10);
        else if(arg==L"--budget-window"&&i+1<argc)app.budgetWindow=static_cast<unsigned>(std::max(1,_wtoi(argv[++i])));
        else if(arg==L"--budget-sustain"&&i+1<argc)app.budgetSustain=static_cast<unsigned>(std::max(1,_wtoi(argv[++i])));
        else if(arg==L"--mesh-shaders"&&i+1<argc){app.meshRequested=(std::wstring(argv[++i])!=L"off");}
        else if(arg==L"--fault-material")app.faultMaterial=true;
        else if(arg==L"--fault-scene"&&i+1<argc)app.faultScenePath=narrow(argv[++i]);
        else if(arg==L"--rt-shadows"&&i+1<argc){
            const std::wstring mode=argv[++i];
            if(mode==L"off")app.rtShadowRequested=false;
            else app.rtShadowRequested=true;
        }
        else if(arg==L"--screenshot"&&i+1<argc){app.screenshotPath=narrow(argv[++i]);app.screenshotRequested=true;}
        else if(arg==L"--debug-view"&&i+1<argc){
            const std::wstring view=argv[++i];
            if(view==L"mask")app.debugView=1;
            else if(view==L"key")app.debugView=2;
            else if(view==L"base")app.debugView=3;
            else if(view==L"solid")app.debugView=4;
            else app.debugView=0;
        }
        else if(arg==L"--screenshot-frame"&&i+1<argc)app.screenshotFrame=static_cast<unsigned>(std::max(1,_wtoi(argv[++i])));
        else if(arg==L"--camera"&&i+1<argc){
            float values[6]={0,0,0,0,0,0};
            if(parseFloatList(argv[++i],values,6)==6){
                app.hasCameraOverride=true;
                for(int axis=0;axis<3;++axis){app.cameraEye[axis]=values[axis];app.cameraTarget[axis]=values[axis+3];}
            }
        }
        else if(arg==L"--render-size"&&i+1<argc){
            const std::wstring size=argv[++i];
            const auto split=size.find(L'x');
            if(split!=std::wstring::npos){
                app.forcedWidth=static_cast<unsigned>(std::max(160,_wtoi(size.substr(0,split).c_str())));
                app.forcedHeight=static_cast<unsigned>(std::max(160,_wtoi(size.substr(split+1).c_str())));
                app.width=app.forcedWidth;app.height=app.forcedHeight;
            }
        }
    }
    // Eight frames per lifecycle cycle: the frame budget must not end the run first.
    if(app.lifecycleCycles>0){
        app.targetFrames=std::max(app.targetFrames,app.lifecycleCycles*8+8);
    }
    if(app.probe){
        app.smoke=true;
        if(!watchExplicit)app.watch=false;
        app.targetFrames=std::max(app.targetFrames,240u);
    } else if(!watchExplicit)app.watch=!app.smoke;
    // A screenshot needs a rendered frame to read back from.
    if(app.screenshotRequested)app.targetFrames=std::max(app.targetFrames,app.screenshotFrame+2);
    LocalFree(argv);
    if(!explicitScene && !std::filesystem::exists(app.scenePath)){
        wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);
        auto folder=std::filesystem::path(exe).parent_path();
        auto candidate=folder/"assets/courtyard.scene.json";
        if(!std::filesystem::exists(candidate))candidate=folder.parent_path()/"assets/courtyard.scene.json";
        app.scenePath=candidate;
        app.reportPath=folder/"reports/native-session.json";
    }
    if(!reloadScene(app.scenePath)){report(false);return 2;}
    // The frame path talks to this device, not to Renderer directly, so a second backend
    // only has to implement nativegpu::rhi::Device (docs/RHI.md).
    app.rhi=nativegpu::rhi::wrap("d3d12",app.renderer,app.error);
    if(!app.rhi){
        app.errorCode="NG-E-RHI-BACKEND";diagnostic(app.error);report(false);return 4;
    }
    if(app.replayEnabled){
        std::uint64_t replaySeed=0;
        if(!loadReplay(app.replayPath,replaySeed,app.error)){
            app.errorCode="NG-E-REPLAY-READ";diagnostic(app.error);report(false);return 6;
        }
        // A replay must use the seed the recording used, otherwise the streams differ.
        if(replaySeed!=0)app.seed=replaySeed;
        app.targetFrames=std::max(app.targetFrames,static_cast<unsigned>(app.replayFrames.size()));
    }
    // Determinism: the run seed owns every random stream. Streams are registered up front
    // so the report proves which consumers exist even before they draw any numbers.
    engine::determinism::registry().reset(app.seed);
    engine::determinism::registry().stream("gameplay");
    engine::determinism::registry().stream("cosmetic");
    if(!app.hashLogPath.empty()){
        const std::filesystem::path path(app.hashLogPath);
        if(path.has_parent_path())std::filesystem::create_directories(path.parent_path());
        app.hashLog.open(path,std::ios::binary|std::ios::trunc);
        if(!app.hashLog){
            app.error="cannot open hash log: "+app.hashLogPath;
            app.errorCode="NG-E-IO-HASH-LOG";
            report(false);return 6;
        }
        app.hashEnabled=true;
        app.hashLog<<"# nativegpu-replay v2 seed="<<app.seed<<" step=1/120 mode="
                   <<(app.recordEnabled?"record":"hash")<<"\n";
        app.hashLog.flush();
    }
    if(app.faultTest){
        // Invalid pipeline: a deliberately broken shader must fail with a stable code.
        std::string code,text;
        const bool shaderRejected=app.renderer.selfTestInvalidPipeline(code,text);
        app.faultTestCases.push_back(json{
            {"case","invalid-pipeline"},{"expectedCode","NG-E-PIPE-COMPILE"},
            {"actualCode",code},{"ok",shaderRejected},{"detail",text}});
        if(!app.faultScenePath.empty()){
            host::SceneDocument document;std::string error;
            const bool loaded=host::loadScene(app.faultScenePath,document,error);
            app.faultTestCases.push_back(json{
                {"case","invalid-scene"},{"expectedCode","NG-E-IO-SCENE"},
                {"actualCode",loaded?"loaded":"NG-E-IO-SCENE"},{"ok",!loaded},{"detail",error}});
        }
    }
    app.background=CreateSolidBrush(RGB(24,31,43));
    app.font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    app.titleFont=CreateFontW(-21,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    WNDCLASSW wc={};wc.hInstance=instance;wc.lpfnWndProc=windowProc;wc.lpszClassName=L"NativeGPUHost";
    wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=app.background;RegisterClassW(&wc);
    wc.lpfnWndProc=viewportProc;wc.lpszClassName=L"NativeGPUViewport";wc.hbrBackground=nullptr;RegisterClassW(&wc);
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA,0,&workArea,0);
    const int workWidth=std::max(900L,workArea.right-workArea.left);
    const int workHeight=std::max(700L,workArea.bottom-workArea.top);
    const int windowWidth=std::min(1300,workWidth-40);
    const int windowHeight=std::min(820,workHeight-40);
    const int windowX=workArea.left+(workWidth-windowWidth)/2;
    const int windowY=workArea.top+(workHeight-windowHeight)/2;
    app.window=CreateWindowExW(0,L"NativeGPUHost",app.titleText.c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,windowX,windowY,windowWidth,windowHeight,nullptr,nullptr,instance,nullptr);
    if(!app.window){app.error="CreateWindow failed";report(false);return 3;}
    app.viewport=CreateWindowExW(0,L"NativeGPUViewport",L"原生 3D 视口",WS_CHILD|WS_VISIBLE,0,0,960,720,app.window,nullptr,instance,nullptr);
    app.title=label(L"",980,20,270,60);SendMessageW(app.title,WM_SETFONT,reinterpret_cast<WPARAM>(app.titleFont),TRUE);
    app.mission=label(L"",980,95,270,95);
    app.stats=label(L"",980,206,270,76);
    auto sceneLabel=label(L"场景实体  /  实时同步",980,296,270,22);SetWindowLongPtrW(sceneLabel,GWLP_ID,201);
    app.list=CreateWindowExW(WS_EX_CLIENTEDGE,L"LISTBOX",L"场景实体",WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOINTEGRALHEIGHT,980,318,270,240,app.window,nullptr,instance,nullptr);
    SendMessageW(app.list,WM_SETFONT,reinterpret_cast<WPARAM>(app.font),TRUE);
    auto help=label(L"WASD 移动  ·  Shift 跑  ·  空格 跳  ·  E 交互  ·  右键拖动 / 方向键 转动镜头",16,740,900,22);
    SetWindowLongPtrW(help,GWLP_ID,202);
    app.pauseButton=button(L"暂停运行",ID_PAUSE,980,600);
    button(L"重置玩法",ID_RESET,1120,600);
    button(L"重载场景",ID_RELOAD,980,644);
    button(L"打开场景",ID_OPEN,1120,644);
    button(L"重建渲染器  /  F8",ID_RESTART,980,688,268);
    refreshList();layout();
    nativegpu::rhi::Descriptor descriptor;
    descriptor.window=app.viewport;
    descriptor.width=app.width;
    descriptor.height=app.height;
    descriptor.validation=app.validationRequested;
    if(!app.rhi->initialize(descriptor,app.error)){
        diagnostic(app.error);report(false);return 4;
    }
    app.rhi->setRtShadows(app.rtShadowRequested);
    app.rhi->setCompositeDebugView(app.debugView);
    app.rhi->setMeshShaders(app.meshRequested);
    app.rhi->setCompute(app.computeRequested,app.computeCorrupt);
    // Injection must happen after initialize(), which rebuilds the renderer state. It is
    // armed exactly once so the recovery run is a clean renderer.
    if(app.deviceRemovalMode&&app.deviceRemovalAtFrame>0&&!app.deviceRemovalDetected){
        app.renderer.injectDeviceRemovalAtFrame(static_cast<unsigned>(app.deviceRemovalAtFrame));
    }
    app.rtShadowEnabled=app.rhi->rtShadowsEnabled();
    // Budget policy is configured after the renderer so the ladder only contains steps
    // that can actually change quality in this run.
    if(app.budgetMode){
        host::BudgetConfig config;
        config.enabled=true;
        config.targetGpuMs=app.budgetTargetMs;
        config.targetCpuSubmitMs=app.budgetCpuMs;
        config.targetVramBytes=app.budgetVramMb*1024ull*1024ull;
        config.windowFrames=app.budgetWindow;
        config.sustainFrames=app.budgetSustain;
        if(app.rtShadowRequested)config.steps.push_back("rt-shadows-off");
        if(app.meshRequested)config.steps.push_back("mesh-path-off");
        if(config.steps.empty())config.enabled=false;
        app.budget.configure(config);
    }
    app.ready=true;
    if(!app.smoke){ShowWindow(app.window,show);UpdateWindow(app.window);}
    app.began=app.last=Clock::now();
    bool success=true;
    while(app.running){
        MSG msg;
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){
            if(msg.message==WM_QUIT)app.running=false;
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        if(!app.running)break;
        // Fault injection for the black-box recorder: a real wall-clock stall on one frame.
        if(app.stallFrame>0&&app.frames==app.stallFrame)Sleep(app.stallMs);
        const auto now=Clock::now();
        float dt=std::chrono::duration<float>(now-app.last).count();app.last=now;
        if(app.minimized){Sleep(30);continue;}
        pollScene();
        engine::Input input;
        if(app.replayEnabled){
            if(app.replayIndex>=app.replayFrames.size())break;
            const ReplayFrame& recorded=app.replayFrames[app.replayIndex];
            input.forward=recorded.forward;
            input.right=recorded.right;
            input.run=recorded.run;
            input.jump=recorded.jump;
            input.interact=recorded.interact;
            input.yawDelta=recorded.yaw;
            input.pitchDelta=recorded.pitch;
        } else if(!app.smoke && GetForegroundWindow()==app.window){
            input.forward=static_cast<float>(down('W'))-static_cast<float>(down('S'));
            input.right=static_cast<float>(down('D'))-static_cast<float>(down('A'));
            input.run=down(VK_SHIFT);
            const bool j=down(VK_SPACE),e=down('E');
            input.jump=j&&!app.jumpDown;input.interact=e&&!app.interactDown;
            app.jumpDown=j;app.interactDown=e;
            input.yawDelta=app.yawDelta+(down(VK_RIGHT)-down(VK_LEFT))*dt*1.6f;
            input.pitchDelta=app.pitchDelta+(down(VK_DOWN)-down(VK_UP))*dt;
        }
        app.yawDelta=0;app.pitchDelta=0;
        if(!app.paused){
            app.game.update(input,app.smoke?1.f/60.f:dt);
            writeReplayLine(input);
            if(app.replayEnabled&&app.replayIndex<app.replayFrames.size()){
                const ReplayFrame& recorded=app.replayFrames[app.replayIndex];
                ++app.replayCompared;
                if(recorded.hash!=app.lastDigest){
                    if(app.replayMismatches==0)app.replayFirstMismatch=app.frames;
                    ++app.replayMismatches;
                }
                ++app.replayIndex;
            }
            // Black box: the first frame over the stall threshold writes the snapshot next
            // to the recording, so a bug report is a replayable file plus its trigger.
            if(app.recordEnabled&&!app.blackboxTriggered&&dt*1000.0>app.blackboxStallMs){
                app.blackboxTriggered=true;
                app.blackboxFrame=app.frames;
                app.blackboxDtMs=dt*1000.0;
                writeAnomaly();
            }
            if(app.replayEnabled&&app.replayIndex>=app.replayFrames.size()){
                success=true;
                break;
            }
        }
        auto p=app.game.playerPosition();
        const float yaw=app.game.yaw(),pitch=app.game.pitch();
        engine::Vec3 eye={p.x-std::sin(yaw)*7.f*std::cos(pitch),p.y+1.4f+std::sin(pitch)*7.f,p.z+std::cos(yaw)*7.f*std::cos(pitch)};
        engine::Vec3 lookAt{p.x,p.y+1.1f,p.z};
        // --camera pins the viewpoint so shadow evidence is reproducible frame to frame.
        if(app.hasCameraOverride){
            eye={app.cameraEye[0],app.cameraEye[1],app.cameraEye[2]};
            lookAt={app.cameraTarget[0],app.cameraTarget[1],app.cameraTarget[2]};
        }
        auto view=engine::lookAtLH(eye,lookAt);
        auto projection=engine::perspectiveLH(1.05f,float(app.width)/float(app.height),.1f,120.f);
        auto vp=engine::multiply(projection,view);
        nativegpu::Mat4 viewProjection;std::copy(vp.m,vp.m+16,viewProjection.m);
        std::vector<nativegpu::DrawItem> draws;
        for(const auto& pose:app.game.renderPoses()){
            nativegpu::DrawItem d{};
            auto m=engine::compose(pose.position,pose.scale,pose.yaw,pose.pitch,pose.roll);
            std::copy(m.m,m.m+16,d.model.m);
            for(int i=0;i<3;++i)d.color[i]=pose.color[i];d.color[3]=1;
            d.roughness=pose.roughness;d.metalness=pose.metalness;
            // Material table slots 1..3 are the resident checker/brick/metal textures;
            // slot 0 is the missing-material default.
            d.texture=pose.texture+1;d.textureGeneration=0;
            draws.push_back(d);
        }
        // --fault-material feeds an out-of-range slot and the missing-texture sentinel
        // through the real draw path so the diagnostics are produced by production code.
        if(app.faultMaterial&&draws.size()>=2){
            draws[0].texture=99;
            draws[1].texture=0xFFFFFFFFu;
        }
        std::vector<nativegpu::LightDesc> lights;
        for(const auto& source:app.document.scene.lights){
            nativegpu::LightDesc light{};
            light.position[0]=source.position.x;light.position[1]=source.position.y;light.position[2]=source.position.z;
            for(int i=0;i<3;++i)light.color[i]=source.color[i];
            light.intensity=source.intensity;light.range=source.range;
            lights.push_back(light);
        }
        const bool shotFrame=app.screenshotRequested&&!app.screenshotAttempted&&(app.frames+1)>=app.screenshotFrame;
        if(shotFrame&&!app.rhi->requestScreenshot(app.screenshotPath,app.screenshotError)){
            success=false;app.screenshotAttempted=true;
            app.liveStatus=L"截图准备失败："+wide(app.screenshotError);
            if(app.smoke)break;
        }
        const float eyePosition[3]={eye.x,eye.y,eye.z};
        if(!app.rhi->submit(viewProjection,eyePosition,draws,lights,app.error)){
            // Device-removal path: stop rendering, record the failure, rebuild the renderer
            // once and keep going if recovery works. The scene file is never touched.
            if(app.deviceRemovalMode&&app.error.find("DEVICE_REMOVED")!=std::string::npos){
                app.deviceRemovalDetected=true;
                app.deviceRemovalFrame=app.frames;
                app.deviceRemovalError=app.error;
                app.liveStatus=L"设备丢失，尝试重建渲染器";
                if(!restartRenderer()){app.deviceRemovalRecovered=false;success=false;break;}
                app.deviceRemovalRecovered=true;
                ++app.deviceRemovalRecoveries;
                app.deviceRemovalFrameAfterRecovery=app.frames;
                app.liveStatus=L"渲染器已重建，继续运行";
                continue;
            }
            app.errorCode="NG-E-RENDER-FRAME";
            diagnostic(app.error);success=false;break;
        }
        ++app.frames;
        if(shotFrame){
            app.screenshotAttempted=true;
            if(!app.rhi->captureScreenshot(app.screenshotPath,app.screenshotError)){
                success=false;
                app.liveStatus=L"截图失败："+wide(app.screenshotError);
                if(app.smoke)break;
            } else {
                app.screenshotTaken=true;
                app.liveStatus=L"截图已保存";
            }
        }
        auto stats=app.rhi->stats();
        if(app.frames>30){
            app.cpuTimes.push_back(stats.cpuMs);
            app.cpuSubmitTimes.push_back(stats.cpuSubmitMs);
            if(stats.gpuValid)app.gpuTimes.push_back(stats.gpuMs);
            if(stats.gpuRtValid)app.gpuRtTimes.push_back(stats.gpuRtMs);
            app.frameTimes.push_back(dt*1000.);
        }
        // Frame budget: a sustained over-target window walks a fixed quality ladder. The
        // governor only decides when; the host applies the vertex-path toggle here and
        // records the step so the report can prove what changed and why.
        if(app.budget.config().enabled){
            const unsigned long long vram=app.rhi->videoMemoryBytes();
            const char* step=app.budget.observe(app.frames,stats.gpuMs,stats.gpuValid,
                                                stats.cpuSubmitMs,vram);
            if(step&&step[0]){
                const std::string name(step);
                if(name=="rt-shadows-off"){
                    app.rhi->setRtShadows(false);
                    app.rtShadowEnabled=app.rhi->rtShadowsEnabled();
                } else if(name=="mesh-path-off"){
                    app.rhi->setMeshShaders(false);
                }
                app.budget.noteApplied(name,app.frames,app.budget.pendingReason());
                app.liveStatus=L"预算超限降档："+wide(name);
            }
        }
        // Lifecycle stress: rebuild the renderer every few frames and record the object
        // and video-memory trend after each shutdown.
        if(app.lifecycleCycles>0&&app.frames>0&&app.frames%8==0&&app.lifecycleDone<app.lifecycleCycles){
            // Video memory must be sampled while the device is still alive.
            const unsigned long long vram=app.rhi->videoMemoryBytes();
            if(app.rhi)app.rhi->shutdown();
            app.ready=false;
            const long liveAfterShutdown=nativegpu::Renderer::liveObjectCount();
            const unsigned long long created=nativegpu::Renderer::objectsCreatedCount();
            if(!restartRenderer()){success=false;break;}
            json entry={
                {"cycle",app.lifecycleDone+1},
                {"framesRendered",app.frames},
                {"liveObjectsAfterShutdown",liveAfterShutdown},
                {"objectsCreated",created},
                {"videoMemoryBytes",vram},
                {"resolution",{app.width,app.height}}
            };
            app.lifecycleEntries.push_back(entry);
            ++app.lifecycleDone;
            if(app.lifecycleDone>=app.lifecycleCycles)break;
        }
        if(std::chrono::duration<double>(now-app.lastUi).count()>.2){
            updateUi();app.lastUi=now;
        }
        if(app.smoke){
            if(!app.noStress&&app.frames==80){
                SetWindowPos(app.window,nullptr,0,0,1100,760,SWP_NOMOVE|SWP_NOZORDER);
                app.resizeTest=true;
            }
            if(!app.noStress&&app.frames==160&&app.lifecycleCycles==0){
                if(!restartRenderer()){success=false;break;}
                app.restartTest=true;
            }
            if(app.frames>=app.targetFrames)break;
        }
        // Bound long interactive sessions without unbounded diagnostics allocation.
        if(app.cpuTimes.size()>60000){app.cpuTimes.erase(app.cpuTimes.begin(),app.cpuTimes.begin()+30000);app.gpuTimes.clear();app.frameTimes.clear();}
    }
    if(app.smoke){
        if(app.replayEnabled){
            // A replay succeeds when every recorded frame was compared. Strict mode also
            // requires a zero mismatch count: a diverged replay is not evidence.
            success=success&&(app.replayCompared==app.replayFrames.size());
            if(app.replayStrict&&app.replayMismatches>0){
                success=false;
                app.errorCode="NG-E-REPLAY-MISMATCH";
            }
        } else if(app.lifecycleCycles>0){
            // The lifecycle run ends when the requested cycles are complete.
            success=success&&app.lifecycleDone>=app.lifecycleCycles;
        } else if(app.deviceRemovalMode){
            success=success&&app.deviceRemovalDetected&&app.deviceRemovalRecovered;
        } else {
            success=success&&app.frames>=app.targetFrames&&(app.noStress||(app.resizeTest&&app.restartTest));
        }
    }
    report(success);
    if(app.rhi)app.rhi->shutdown();app.ready=false;
    DestroyWindow(app.window);DeleteObject(app.font);DeleteObject(app.titleFont);DeleteObject(app.background);
    return success?0:5;
}
