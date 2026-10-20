#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include "scene_loader.hpp"
#include "../renderer/d3d12_renderer.h"
#include "../engine/native/math.hpp"
#include "../third_party/json.hpp"
#include <chrono>
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
    host::SceneDocument document;
    engine::Game game;
    std::filesystem::path scenePath="assets/courtyard.scene.json", reportPath="reports/native-session.json";
    std::wstring titleText=L"NativeGPU | 原生开发预览",liveStatus;
    std::string error,sceneError;
    bool paused=false,running=true,ready=false,minimized=false,dragging=false,smoke=false,watch=true,probe=false;
    bool resizeTest=false,restartTest=false,jumpDown=false,interactDown=false;
    int mouseX=0,mouseY=0;
    float yawDelta=0,pitchDelta=0;
    unsigned width=960,height=720,targetFrames=360,frames=0,restarts=0,resizes=0,hotReloads=0;
    SceneStamp sceneStamp;
    std::vector<double> cpuTimes,gpuTimes,frameTimes;
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
void setText(HWND h,const std::wstring& text){SetWindowTextW(h,text.c_str());}
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
    app.error.clear();app.sceneError.clear();app.sceneStamp=stampOf(path);
    if(app.list)refreshList();
}
void diagnostic(const std::string& error) {
    app.error=error;
    if(!app.smoke) MessageBoxW(app.window,wide(error).c_str(),L"NativeGPU 诊断",MB_OK|MB_ICONERROR);
}
bool reloadScene(const std::filesystem::path& path) {
    host::SceneDocument next;
    std::string error;
    if(!host::loadScene(path,next,error)){diagnostic(error);return false;}
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
        app.sceneError=error;
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
    app.renderer.shutdown();
    if(!app.renderer.initialize(app.viewport,app.width,app.height,app.error)){
        diagnostic(app.error);app.running=false;return false;
    }
    app.ready=true;++app.restarts;return true;
}
void layout(){
    if(!app.viewport)return;
    RECT r;GetClientRect(app.window,&r);
    int panel=310;
    app.width=static_cast<unsigned>(std::max(160L,r.right-panel));
    app.height=static_cast<unsigned>(std::max(160L,r.bottom));
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
        if(!app.renderer.resize(app.width,app.height,app.error)){
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
    auto stats=app.renderer.stats();
    s<<std::fixed<<std::setprecision(2)<<L"CPU 提交 "<<stats.cpuMs<<L" ms   GPU ";
    if(stats.gpuValid)s<<stats.gpuMs<<L" ms";else s<<L"采样中";
    s<<L"\n位置 "<<p.x<<L", "<<p.y<<L", "<<p.z;
    s<<L"\n"<<(app.paused?L"已暂停":L"运行中")<<L"  ·  revision "<<app.document.revision;
    s<<L"  ·  "<<(app.watch?L"热重载开":L"热重载关");
    if(!app.liveStatus.empty())s<<L"\n"<<app.liveStatus;
    if(!app.sceneError.empty())s<<L"\n错误 "<<wide(app.sceneError);
    setText(app.stats,s.str());
}
double percentile(std::vector<double> x,double p){
    if(x.empty())return 0;
    std::sort(x.begin(),x.end());
    return x[static_cast<size_t>(std::ceil((x.size()-1)*p))];
}
void report(bool success){
    try{
        json r={
            {"version","0.3.0"},{"success",success},{"error",app.error},
            {"mode",app.probe?"probe":(app.smoke?"automated-smoke":"interactive")},
            {"frames",app.frames},{"elapsedSeconds",std::chrono::duration<double>(Clock::now()-app.began).count()},
            {"scene",app.scenePath.u8string()},{"revision",app.document.revision},
            {"resolution",{app.width,app.height}},{"rendererReinitializations",app.restarts},
            {"resizeEvents",app.resizes},{"watchEnabled",app.watch},{"hotReloads",app.hotReloads},
            {"lastSceneError",app.sceneError},{"deviceRemovalTested",false},
            {"cpuSubmitP95Ms",percentile(app.cpuTimes,.95)},
            {"gpuP95Ms",app.gpuTimes.empty()?json(nullptr):json(percentile(app.gpuTimes,.95))},
            {"frameIntervalP95Ms",percentile(app.frameTimes,.95)},
            {"gpuSamples",app.gpuTimes.size()},
            {"game",{{"hasKey",app.game.hasKey()},{"doorOpen",app.game.doorOpen()},{"complete",app.game.complete()}}},
            {"capabilities",json::parse(app.renderer.capabilitiesJson())},
            {"probes",json::parse(app.renderer.probesJson())}
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
    }
    if(app.probe){
        app.smoke=true;
        if(!watchExplicit)app.watch=false;
        app.targetFrames=std::max(app.targetFrames,240u);
    } else if(!watchExplicit)app.watch=!app.smoke;
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
    if(!app.renderer.initialize(app.viewport,app.width,app.height,app.error)){
        diagnostic(app.error);report(false);return 4;
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
        const auto now=Clock::now();
        float dt=std::chrono::duration<float>(now-app.last).count();app.last=now;
        if(app.minimized){Sleep(30);continue;}
        pollScene();
        engine::Input input;
        if(!app.smoke && GetForegroundWindow()==app.window){
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
        if(!app.paused)app.game.update(input,app.smoke?1.f/60.f:dt);
        auto p=app.game.playerPosition();
        const float yaw=app.game.yaw(),pitch=app.game.pitch();
        engine::Vec3 eye={p.x-std::sin(yaw)*7.f*std::cos(pitch),p.y+1.4f+std::sin(pitch)*7.f,p.z+std::cos(yaw)*7.f*std::cos(pitch)};
        auto view=engine::lookAtLH(eye,{p.x,p.y+1.1f,p.z});
        auto projection=engine::perspectiveLH(1.05f,float(app.width)/float(app.height),.1f,120.f);
        auto vp=engine::multiply(projection,view);
        nativegpu::Mat4 viewProjection;std::copy(vp.m,vp.m+16,viewProjection.m);
        std::vector<nativegpu::DrawItem> draws;
        for(const auto& pose:app.game.renderPoses()){
            nativegpu::DrawItem d{};
            auto m=engine::compose(pose.position,pose.scale,pose.yaw,pose.pitch,pose.roll);
            std::copy(m.m,m.m+16,d.model.m);
            for(int i=0;i<3;++i)d.color[i]=pose.color[i];d.color[3]=1;
            d.roughness=pose.roughness;d.metalness=pose.metalness;d.texture=pose.texture;
            draws.push_back(d);
        }
        std::vector<nativegpu::LightDesc> lights;
        for(const auto& source:app.document.scene.lights){
            nativegpu::LightDesc light{};
            light.position[0]=source.position.x;light.position[1]=source.position.y;light.position[2]=source.position.z;
            for(int i=0;i<3;++i)light.color[i]=source.color[i];
            light.intensity=source.intensity;light.range=source.range;
            lights.push_back(light);
        }
        if(!app.renderer.render(viewProjection,draws,lights,app.error)){diagnostic(app.error);success=false;break;}
        ++app.frames;
        auto stats=app.renderer.stats();
        if(app.frames>30){
            app.cpuTimes.push_back(stats.cpuMs);
            if(stats.gpuValid)app.gpuTimes.push_back(stats.gpuMs);
            app.frameTimes.push_back(dt*1000.);
        }
        if(std::chrono::duration<double>(now-app.lastUi).count()>.2){
            updateUi();app.lastUi=now;
        }
        if(app.smoke){
            if(app.frames==80){
                SetWindowPos(app.window,nullptr,0,0,1100,760,SWP_NOMOVE|SWP_NOZORDER);
                app.resizeTest=true;
            }
            if(app.frames==160){
                if(!restartRenderer()){success=false;break;}
                app.restartTest=true;
            }
            if(app.frames>=app.targetFrames)break;
        }
        // Bound long interactive sessions without unbounded diagnostics allocation.
        if(app.cpuTimes.size()>60000){app.cpuTimes.erase(app.cpuTimes.begin(),app.cpuTimes.begin()+30000);app.gpuTimes.clear();app.frameTimes.clear();}
    }
    if(app.smoke)success=success&&app.resizeTest&&app.restartTest&&app.frames>=app.targetFrames;
    report(success);
    app.renderer.shutdown();app.ready=false;
    DestroyWindow(app.window);DeleteObject(app.font);DeleteObject(app.titleFont);DeleteObject(app.background);
    return success?0:5;
}
