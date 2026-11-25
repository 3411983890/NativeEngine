#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include "scene_loader.hpp"
#include "../engine/native/assets/file_path.hpp"
#include "scene_preview.hpp"
#include "audio_scene.hpp"
#include "texture_asset.hpp"
#include "effect_fields.hpp"
#include "simulation_fields.hpp"
#include "cloth_preview.hpp"
#include "pond_demo.hpp"
#include "wetness.hpp"
#include "../engine/native/assets/sha256.hpp"
#include "adventure_hud.hpp"
#include "budget.hpp"
#include "nativegpu_version.h"
#include "../renderer/d3d12_renderer.h"
#include "../renderer/rhi.hpp"
#include "../engine/audio/audio.hpp"
#include "../engine/audio/audio_device.hpp"
#include "../engine/native/math.hpp"
#include "../engine/native/geometry_class.hpp"
#include "../engine/native/material_handle.hpp"
#include "../engine/native/effects/runtime.hpp"
#include "../engine/native/vegetation/world.hpp"
#include "../engine/native/vegetation/texture.hpp"
#include "../engine/native/water/transfer.hpp"
#include "../renderer/vegetation_draw.hpp"
#include "../third_party/json.hpp"
#include <chrono>
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <tuple>
#include <cstdint>
#include <map>
#include <utility>
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
    float dt=1.f/60.f;
    std::uint64_t hash=0;
    float forward=0.0f,right=0.0f,yaw=0.0f,pitch=0.0f;
    bool run=false,jump=false,interact=false,fire=false,blast=false;
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
    bool mouseLook=false,tour=false;
    bool completionVisible=true;
    std::wstring notice;
    Clock::time_point noticeUntil{};
    std::string tourDirectory;
    unsigned tourCheckpoint=0;
    std::string checkpointPath="saves/fogtown-checkpoint.json";
    std::string progressPath;
    HBRUSH background=nullptr;
    nativegpu::Renderer renderer;
    // Frame path goes through the RHI boundary; the raw renderer stays for test hooks.
    std::unique_ptr<nativegpu::rhi::Device> rhi;
    host::SceneDocument document;
    engine::Game game;
    std::unique_ptr<host::PondDemo> pondDemo;
    bool waterEnabled=true;
    bool surfaceWetness=true;
    unsigned waterDebug=0;
    // 0 = 129 vertices per axis, 1 = 65, 2 = 33, 3 = 17. Presentation only.
    unsigned waterLod=0;
    std::shared_ptr<const engine::animation::Asset> character;
    std::shared_ptr<const engine::animation::Asset> fallbackCharacter;
    std::filesystem::path activeCharacterPath;
    bool animationDemo=false;
    bool animationFeatures=true;
    unsigned animationEventCount=0;
    std::filesystem::path characterPath="build/animation/mannequin/character.json";
    std::filesystem::path importedAssetPath;
    std::filesystem::path scenePath=".screen/courtyard.scene.json", reportPath="reports/native-session.json";
    std::filesystem::path texturePath="assets/textures";
    bool materialsEnabled=false;
    unsigned defaultMaterial=0;
    std::wstring titleText=L"NativeGPU | Native preview",liveStatus;
    std::string error,sceneError;
    bool paused=false,running=true,ready=false,minimized=false,dragging=false,smoke=false,watch=true,probe=false;
    bool embeddedPreview=false;
    bool resizeTest=false,restartTest=false,jumpDown=false,interactDown=false;
    bool fireDown=false,blastDown=false;
    bool physicsDemo=false;
    unsigned physicsDemoShots=0;
    // Authored demo shots, each `frame:x,y,z`. The built-in --physics-demo shots are
    // hardcoded to the physics-lab layout, so a different scene (bonded walls, doors,
    // ropes) needs its own schedule to produce comparable intact/destroyed images.
    struct DemoShot { unsigned frame=0; engine::Vec3 target; };
    std::vector<DemoShot> demoShots;
    bool demoShotsFiredThisFrame=false;
    // DXR static shadow toggle. `rtShadowRequested` is what the caller asked for;
    // `rtShadowEnabled` is what the renderer could actually enable after creation.
    bool rtShadowRequested=true,rtShadowEnabled=false;
    bool screenshotRequested=false,screenshotTaken=false,screenshotAttempted=false;
    int mouseX=0,mouseY=0;
    float yawDelta=0,pitchDelta=0;
    unsigned width=960,height=720,targetFrames=360,frames=0,restarts=0,resizes=0,hotReloads=0;
    unsigned forcedWidth=0,forcedHeight=0,screenshotFrame=150,debugView=0,layerDebugMode=0,layerDebugLayer=0;
    bool validationRequested=false;
    // G1 reliability modes.
    bool deviceRemovalMode=false,deviceRemovalDetected=false,deviceRemovalRecovered=false;
    double deviceRemovalAtFrame=0.0;
    unsigned deviceRemovalFrame=0,deviceRemovalRecoveries=0,deviceRemovalFrameAfterRecovery=0;
    std::string deviceRemovalError;
    unsigned lifecycleCycles=0,lifecycleDone=0;
    bool faultTest=false,faultMaterial=false,noStress=false,meshRequested=false,cullRequested=true;
    std::string faultScenePath,errorCode;
    std::vector<nlohmann::json> faultTestCases,lifecycleEntries;
    bool hasCameraOverride=false;
    float cameraEye[3]={0,0,0},cameraTarget[3]={0,0,0};
    unsigned cameraCutFrame=0;
    float cameraCutEye[3]{},cameraCutTarget[3]{};
    std::string screenshotPath,screenshotError;
    SceneStamp sceneStamp;
    SceneStamp textureStamp;
    std::vector<double> cpuTimes,cpuSubmitTimes,gpuTimes,gpuRtTimes,frameTimes;
    // Determinism: a run seed, an optional per-frame digest log, and the final digest.
    std::uint64_t seed=20261020;
    bool hashEnabled=false;
    std::string hashLogPath,finalHash;
    std::ofstream hashLog;
    unsigned long long hashLines=0;
    std::uint64_t lastDigest=0;
    // Replay and black-box recorder (E08).
    std::string recordPath,replayPath,replayContent,savePath,loadSavePath;
    bool replayV4=false;
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
    bool particlesRequested=false,particleColliders=true,particleEmitterSet=false;
    bool particleTemporal=true;
    bool firePreset=false;
    // Fire light driven by the field: intensity after flicker and burst boost, reported so
    // the evidence can prove the light actually follows the fire.
    bool fireLightApplied=false;
    float fireLightIntensity=0.0f;
    float fireLightFlicker=1.0f;
    unsigned explodeAt=0;
    bool scorchApplied=false;
    unsigned scorchedEntities=0;
    float particleEmitter[3]={0.0f,0.2f,0.0f};
    engine::effects::Runtime effects;
    host::ClothPreviewOptions clothOptions;
    std::unique_ptr<host::ClothPreview> clothPreview;
    // --groom loads a cooked hairstyle pack and draws it in the scene frame. The camera is
    // framed from the pack bounds unless --camera pins one.
    std::filesystem::path groomPath;
    bool groomConfigured=false;
    int groomDebugView=0;
    float groomShadowStrength=0.85f;
    bool groomSimulate=true;
    float groomWind[3]={0,0,0};
    float groomHeadTurnDegrees=0; // extra yaw applied to the player head bone and the hair
    bool groomHeadAttached=false;
    engine::effects::Library effectLibrary;
    std::filesystem::path effectLibraryPath="build/effects/library.json";
    std::filesystem::path effectFieldPath;
    std::unique_ptr<host::EffectFields> effectFields;
    std::filesystem::path simulationFieldPath;
    std::unique_ptr<host::SimulationFields> simulationFields;
    std::string effectId;
    unsigned effectCount=1,effectRepeatTicks=0,effectStopTick=0;
    engine::Vec3 effectOrigin{0,0.2f,0};
    std::uint64_t effectGameTick=0,effectRejected=0;
    bool effectStopped=false;
    bool gpuEffectRequested=false;
    std::uint64_t effectGpuEpoch=0;
    std::vector<engine::effects::Handle> effectHandles;
    std::vector<nativegpu::TextureUpload> effectTextureUploads;
    std::map<std::string,unsigned> effectTextureSlots;
    // Water splash droplets. The water body records interaction events; this stage only
    // turns them into particles, so it owns no authority and never writes back.
    struct WaterSplashState {
        bool requested=false,enabled=false;
        std::string id="water_splash";
        std::filesystem::path libraryPath="build/effects/library.json";
        std::filesystem::path assetRoot="assets";
        engine::effects::Library library;
        std::vector<engine::effects::Handle> handles;
        const engine::water::Body* body=nullptr;
        std::uint64_t gameTick=0,epoch=0;
        // Contact delivery identity, not the occurrence tick: two feet can land in one
        // tick and both must spawn droplets.
        std::uint32_t lastSequence=0,lastTimelineEpoch=0;
        std::uint64_t spawned=0,capacityRejected=0,failed=0,expired=0,stepsDropped=0,contacts=0;
        float lastIntensity=0,peakIntensity=0;
    } splash;
    // Runtime wetness history for rocks, banks and beds that touch the water. Presentation
    // only; it rides in the bindless opaque draw constants and never reaches the solver.
    struct WetEntity { float value=0, authored=0; bool largeFlat=false; engine::Vec3 centre{}, half{}; };
    struct WaterLinkRuntime {
        std::unique_ptr<host::PondDemo> upstream;
        std::unique_ptr<engine::water::TransferLink> link;
        engine::water::TransferSettings settings;
        float widthMetres=1.5f;
    };
    std::vector<WaterLinkRuntime> waterLinks;
    std::map<std::string,WetEntity> wetness;
    unsigned wetEntities=0, wetDraws=0;
    float maxWetness=0;
    bool budgetMode=false;
    double budgetTargetMs=16.6,budgetCpuMs=0.0;
    unsigned long long budgetVramMb=0;
    unsigned budgetWindow=120,budgetSustain=30;
    // Audio runtime (.developmentAgent/animation-audio/AUDIO-PLAN.md). The mixer owns its own 48 kHz clock and is
    // driven by the device thread, never by the frame loop: `dt` only decides when a
    // command is submitted, and every command carries a target sample. `audioOff`
    // keeps the whole subsystem dormant so `--audio off` is a true regression path.
    engine::audio::Engine audio;
    engine::audio::WasapiDevice audioDevice;
    bool audioOff=false,audioConfigured=false,audioDeviceRunning=false,audioDeviceFailed=false;
    bool audioDeviceStarted=false,audioDemo=false;
    // `--audio offline` pulls the mixer from the frame loop instead of a device thread.
    // It is the only mode that works on a machine with no endpoint, and the only one
    // that can hand a deterministic pcmHash to a gate.
    bool audioOffline=false;
    std::vector<float> audioScratch;
    double audioSampleDebt=0.0;
    std::string audioQualityName="spatial",audioLayoutName="7.1.2",audioBackendName="wasapi";
    std::string audioError,audioDeviceName;
    // `--audio-audition <sound>` plays one named sound non-spatially at the listener.
    // It is the scene-independent proof that the mixer can deliver a full-level signal:
    // the demo sweep only proves routing, and a distant emitter is legitimately quiet.
    std::string audioAuditionSound;
    std::filesystem::path audioBankPath="assets/audio/bank.json";
    std::string audioBankHash;
    unsigned audioSoundCount=0,audioPlays=0,audioRejected=0,audioTriggersFired=0;
    std::map<std::string,uint32_t> audioSoundHandles;
    struct TrackedEmitter { std::string id,entity;engine::Vec3 offset;float remaining; };
    std::map<uint32_t,TrackedEmitter> audioEmitterVoices;
    unsigned audioPositionUpdates=0,audioSceneReloads=0;
    std::vector<std::string> audioTriggers;
    // Edge detection so a state change fires exactly one trigger, not one per frame.
    bool audioDoorState=false,audioCompleteState=false,audioKeyState=false;
    engine::Vec3 audioLastEye{};
    bool audioEyeValid=false;
    engine::Vec3 audioOccluderEye{};
    bool audioOccluderEyeValid=false;
    unsigned audioOccluderRevision=~0u;
    Clock::time_point began=Clock::now(),last=began,lastUi=began,lastWatch=began,lastTextureWatch=began;
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
void writeReplayLine(const engine::Input& input,float dt){
    // The digest is always computed: replay compares it even when this run writes no log.
    engine::determinism::Hasher hasher;
    app.game.digest(hasher);
    app.lastDigest=hasher.value();
    app.finalHash=hasher.hex();
    if(!app.hashEnabled||!app.hashLog.is_open())return;
    char buffer[192];
    std::snprintf(buffer,sizeof(buffer),
                  "%u,%llu,%016llx,%08x,%08x,%d,%d,%d,%d,%d,%08x,%08x,%08x",
                  static_cast<unsigned>(app.hashLines),static_cast<unsigned long long>(app.game.ticks()),
                  static_cast<unsigned long long>(app.lastDigest),
                  floatBits(input.forward),floatBits(input.right),
                  input.run?1:0,input.jump?1:0,input.interact?1:0,
                  input.fire?1:0,input.blast?1:0,
                  floatBits(input.yawDelta),floatBits(input.pitchDelta),floatBits(dt));
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
            app.replayV4 = line.find("nativegpu-replay v4") != std::string::npos;
            const auto content=line.find(" content=");
            if(content!=std::string::npos)app.replayContent=line.substr(content+9,line.find(' ',content+9)-(content+9));
            const auto position=line.find("seed=");
            if(position!=std::string::npos){
                seed=std::strtoull(line.c_str()+position+5,nullptr,10);
            }
            continue;
        }
        unsigned frame=0,forward=0,right=0,yaw=0,pitch=0;
        unsigned long long tick=0,hash=0;
        int run=0,jump=0,interact=0,fire=0,blast=0;
        const auto columns=static_cast<unsigned>(std::count(line.begin(),line.end(),','))+1;
        unsigned delta=floatBits(1.f/60.f);int parsed=0;
        if(columns==13)parsed=std::sscanf(line.c_str(),"%u,%llu,%llx,%x,%x,%d,%d,%d,%d,%d,%x,%x,%x",
            &frame,&tick,&hash,&forward,&right,&run,&jump,&interact,&fire,&blast,&yaw,&pitch,&delta);
        else if(columns==12)parsed=std::sscanf(line.c_str(),"%u,%llu,%llx,%x,%x,%d,%d,%d,%d,%d,%x,%x",
            &frame,&tick,&hash,&forward,&right,&run,&jump,&interact,&fire,&blast,&yaw,&pitch);
        else if(columns==10)parsed=std::sscanf(line.c_str(),"%u,%llu,%llx,%x,%x,%d,%d,%d,%x,%x",
            &frame,&tick,&hash,&forward,&right,&run,&jump,&interact,&yaw,&pitch);
        if(parsed!=static_cast<int>(columns)||frame!=app.replayFrames.size()||!std::isfinite(bitsFloat(delta))||bitsFloat(delta)<0){
            error="malformed replay line: "+line;return false;
        }
        ReplayFrame recorded;
        recorded.hash=hash;recorded.dt=bitsFloat(delta);
        recorded.forward=bitsFloat(forward);
        recorded.right=bitsFloat(right);
        recorded.yaw=bitsFloat(yaw);
        recorded.pitch=bitsFloat(pitch);
        recorded.run=run!=0;
        recorded.jump=jump!=0;
        recorded.interact=interact!=0;
        recorded.fire=fire!=0;
        recorded.blast=blast!=0;
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
void applySceneEnvironment();
void audioApplyScene();
bool restartRenderer();
json materialIdentity(const engine::Scene& scene) {
    json values=json::array();for(const auto& material:scene.materials)values.push_back({material.name,material.albedo,material.normal,material.orm,material.uvScale,material.roughness,material.metalness,material.wetness,material.wear,material.clearcoat,material.clearcoatRoughness,material.ior,material.alphaMasked,material.alphaBlended,material.opacity,material.alphaCutoff,material.emissive,{material.emissiveFactor[0],material.emissiveFactor[1],material.emissiveFactor[2]},material.emissiveStrength,material.hasTextureMappings?engine::materialSamplingIdentity(material):json(),material.occlusion,material.occlusionStrength,material.unlit,material.coat,material.coatRoughness,material.coatNormal});return values;
}
void adoptScene(host::SceneDocument next,const std::filesystem::path& path) {
    app.waterLinks.clear();
    std::unique_ptr<host::PondDemo> water;
    if(next.water.present&&app.waterEnabled){
        const auto& w=next.water;auto asset=engine::water::parseCookedWater(w.cookedData);
        if(asset->columns*asset->rows>16384)throw std::runtime_error("NG-E-WATER-BUDGET: CPU surface supports at most 16384 cells");
        if(asset->material.refractionIndex<1.01f||asset->material.refractionIndex>2.f)throw std::runtime_error("NG-E-WATER-OPTICS: IOR outside surface support");
        water=std::make_unique<host::PondDemo>(std::move(asset),w.origin,w.absorption,w.scattering,w.roughness);
        // Opt-in scene coupling: static collider columns that intersect the footprint
        // become water obstacles. A column below the surface raises the bed (water flows
        // over); one that reaches the surface blocks normal flux. Bridge decks and
        // overhead geometry are not classified: their column is treated as solid spans.
        if(next.water.obstaclesFromColliders){
            const auto body=water->bodyHandle();
            const auto& waterAsset=body->asset();
            for(const auto& entity:next.scene.entities){
                if(!entity.collider||entity.dynamicBody)continue;
                if(entity.kind!="prop"&&entity.kind!="wall")continue;
                const float radius=.5f*std::max(entity.scale.x,entity.scale.z);
                if(!(radius>0))continue;
                const float localX=entity.position.x-w.origin.x,localZ=entity.position.z-w.origin.z;
                if(std::fabs(localX)>waterAsset.extentX*.5f+radius||
                   std::fabs(localZ)>waterAsset.extentZ*.5f+radius)continue;
                engine::water::Obstacle obstacle;
                obstacle.x=localX;obstacle.z=localZ;obstacle.radiusMetres=radius;
                obstacle.topHeightMetres=entity.position.y+entity.scale.y*.5f-w.origin.y;
                body->addObstacle(obstacle);
            }
        }
        // Configured upstream -> primary transfers. The upstream body is simulated by the
        // same fixed-tick observer as the primary one; its surface is rendered as a
        // second water pass and the falling sheet is a separate translucent ribbon.
        app.waterLinks.clear();
        for(const auto& spec:next.water.links){
            auto upperAsset=engine::water::parseCookedWater(spec.cookedData);
            if(upperAsset->columns*upperAsset->rows>16384)throw std::runtime_error("NG-E-WATER-BUDGET: CPU surface supports at most 16384 cells");
            if(upperAsset->material.refractionIndex<1.01f||upperAsset->material.refractionIndex>2.f)throw std::runtime_error("NG-E-WATER-OPTICS: IOR outside surface support");
            App::WaterLinkRuntime runtime;
            runtime.upstream=std::make_unique<host::PondDemo>(std::move(upperAsset),spec.origin,
                                                             w.absorption,w.scattering,w.roughness);
            runtime.settings.sourceX=spec.sourceX;runtime.settings.sourceZ=spec.sourceZ;
            runtime.settings.sourceRadiusMetres=spec.sourceRadiusMetres;
            runtime.settings.landingX=spec.landingX;runtime.settings.landingZ=spec.landingZ;
            runtime.settings.landingRadiusMetres=spec.landingRadiusMetres;
            runtime.settings.requestedFlowCubicMetresPerSecond=spec.flowCubicMetresPerSecond;
            runtime.settings.transitSeconds=spec.transitSeconds;
            runtime.settings.exitNormalSpeed=spec.exitNormalSpeed;
            runtime.widthMetres=spec.widthMetres;
            runtime.link=std::make_unique<engine::water::TransferLink>(runtime.upstream->bodyHandle().get(),
                                                                     water->bodyHandle().get(),runtime.settings);
            app.waterLinks.push_back(std::move(runtime));
        }
    }
    const bool materialsChanged=materialIdentity(app.document.scene)!=materialIdentity(next.scene);
    app.document=std::move(next);app.scenePath=path;
    app.pondDemo=std::move(water);app.game.clearWater();app.game.observeFixedTicks({});
    app.character=nullptr;app.activeCharacterPath=app.characterPath;
    for(const auto& entity:app.document.scene.entities)if(entity.kind=="player"&&entity.characterAsset>=0) {
        app.character=app.document.scene.characterAssets[static_cast<std::size_t>(entity.characterAsset)];
        auto characterRoot=path.parent_path();for(auto parent=characterRoot;;parent=parent.parent_path()){if(parent.filename()==".screen"){characterRoot=std::filesystem::u8path(app.document.scene.assetRoot);break;}if(!parent.has_parent_path()||parent==parent.parent_path())break;}
        app.activeCharacterPath=characterRoot/std::filesystem::u8path(entity.character.file);
        if(entity.character.rootMotion!="asset"){auto override=std::make_shared<engine::animation::Asset>(*app.character);override->preserveRootTranslation=entity.character.rootMotion=="preserve";app.character=std::move(override);}
        break;
    }
    if(!app.character){if(!app.fallbackCharacter)app.fallbackCharacter=engine::animation::loadAsset(app.characterPath);app.character=app.fallbackCharacter;}
    app.game.initialize(app.document.scene);
    // The Game owns the fixed tick: wading, footprint interactions, buoyancy and the
    // water step all run there. PondDemo only presents the shared body.
    if(app.pondDemo)app.game.setWater(app.pondDemo->bodyHandle());
    app.game.observeFixedTicks([](std::uint64_t,engine::Vec3){
        for(auto& runtime:app.waterLinks){
            runtime.upstream->bodyHandle()->step(1.f/120.f);
            runtime.link->step(1.f/120.f);
        }
    });
    // Candidate wet surfaces. Ground only counts when its whole footprint is covered,
    // so a courtyard containing a pond does not darken as one sheet.
    app.wetness.clear();app.wetEntities=0;app.wetDraws=0;app.maxWetness=0;
    for(const auto& entity:app.document.scene.entities){
        if(!entity.collider)continue;
        if(entity.kind!="prop"&&entity.kind!="wall"&&entity.kind!="ground")continue;
        auto& state=app.wetness[entity.id];
        state.largeFlat=entity.kind=="ground";
        state.centre=entity.position;state.half=entity.scale*0.5f;
        if(entity.materialIndex>=0&&static_cast<std::size_t>(entity.materialIndex)<app.document.scene.materials.size())
            state.authored=app.document.scene.materials[static_cast<std::size_t>(entity.materialIndex)].wetness;
    }
    if(app.character)app.game.setCharacter(app.character,app.animationFeatures);
    if(app.ready) {
        std::string meshError;
        if(!app.renderer.configureConvexMeshes(app.game.convexMeshes(),meshError))throw std::runtime_error(meshError);
        if(materialsChanged&&!restartRenderer())throw std::runtime_error(app.error);
    }
    applySceneEnvironment();
    audioApplyScene();
    app.error.clear();app.sceneError.clear();app.errorCode.clear();app.sceneStamp=stampOf(path);
    if(app.list)refreshList();
}
bool parseLayerDebugSpec(const std::string& spec, unsigned& mode, unsigned& layer) {
    mode = 0; layer = 0;
    if (spec.empty() || spec == "off") return true;
    std::string name = spec; unsigned index = 0;
    const auto colon = spec.find(':');
    if (colon != std::string::npos) {
        name = spec.substr(0, colon); const auto text = spec.substr(colon + 1);
        if (text.size() != 1 || text[0] < '0' || text[0] > '3') return false;
        index = static_cast<unsigned>(text[0] - '0');
    }
    if (name == "mask") { mode = 1; layer = index; return true; }
    if (name == "height") { mode = 2; layer = 0; return true; }
    if (name == "albedo" || name == "layer") { mode = 3; layer = index; return true; }
    if (name == "reliefdepth") { mode = 4; layer = 0; return true; }
    if (name == "reliefoffset") { mode = 5; layer = 0; return true; }
    if (name == "reliefsteps") { mode = 6; layer = 0; return true; }
    return false;
}
void diagnostic(const std::string& error) {
    app.error=error;
    if(!app.smoke) MessageBoxW(app.window,wide(error).c_str(),L"NativeGPU 璇婃柇",MB_OK|MB_ICONERROR);
}
bool reloadScene(const std::filesystem::path& path) {
    if(app.game.gameplayRuntime().contentLocked()){app.sceneError="Recording/replay content is locked";return false;}
    host::SceneDocument next;
    std::string error;
    if(!host::loadScene(path,next,error)){app.errorCode="NG-E-IO-SCENE";diagnostic(error);return false;}
    try{adoptScene(std::move(next),path);}
    catch(const engine::animation::Error& error){app.errorCode=error.code;diagnostic(error.what());return false;}
    catch(const std::exception& error){app.errorCode="NG-E-IO-SCENE";diagnostic(error.what());return false;}
    app.liveStatus=L"Reloaded revision "+std::to_wstring(app.document.revision);
    return true;
}
// A failed hot reload must leave the playable scene untouched and must not
// block the message loop with a modal dialog. The next valid write retries.
bool restartRenderer();
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
        app.liveStatus=L"Hot reload rejected";
        return;
    }
    if(next.projectId!=app.document.projectId){
        app.sceneStamp=stamp;
        app.sceneError="hot reload refused: projectId changed";
        app.liveStatus=L"Hot reload rejected: project ID mismatch";
        return;
    }
    try{adoptScene(std::move(next),app.scenePath);}catch(const std::exception& error){app.sceneError=error.what();app.liveStatus=L"Hot reload rejected";return;}
    ++app.hotReloads;
    app.liveStatus=L"Hot reload revision "+std::to_wstring(app.document.revision);
}
void pollTextures(){
    if(!app.watch||app.texturePath.empty()||!app.ready)return;
    const auto now=Clock::now();
    if(std::chrono::duration<double>(now-app.lastTextureWatch).count()<0.4)return;
    app.lastTextureWatch=now;
    const SceneStamp stamp=stampOf(app.texturePath/"manifest.json");
    if(!stamp.valid||unchanged(stamp,app.textureStamp))return;
    if(!app.textureStamp.valid){app.textureStamp=stamp;return;}
    app.textureStamp=stamp;
    if(restartRenderer()){
        ++app.hotReloads;
        app.liveStatus=L"Texture hot reload";
    }
}
bool configureRendererMaterials() {
    app.materialsEnabled=false;
    auto definitions=app.document.scene.materials;
    const auto gridDirectory=std::filesystem::u8path(app.document.scene.assetRoot)/"builtin/textures";
    const bool localGrid=std::filesystem::exists(engine::assets::nativeFilePath(app.texturePath/"engine_grid.albedo.ngtex"));
    const bool bundledGrid=std::filesystem::exists(engine::assets::nativeFilePath(gridDirectory/"engine_grid.albedo.ngtex"));
    if(definitions.empty()&&!localGrid&&!bundledGrid){
        app.renderer.clearMaterials();
        return true;
    }
    std::vector<host::TextureAsset> assets;
    std::string error;
    if(!host::loadTextureDirectory(definitions.empty()&&!localGrid?gridDirectory:app.texturePath,assets,error)){
        app.errorCode="NG-E-TEX-LOAD";
        diagnostic("Material texture load failed: "+error);
        return false;
    }
    if(!localGrid&&bundledGrid&&!definitions.empty()) {
        std::vector<host::TextureAsset> builtin;
        if(!host::loadTextureDirectory(gridDirectory,builtin,error)){app.errorCode="NG-E-TEX-LOAD";diagnostic(error);return false;}
        for(auto& texture:builtin)if(texture.name.rfind("engine_grid.",0)==0)assets.push_back(std::move(texture));
    }
    const bool grid=std::any_of(assets.begin(),assets.end(),[](const auto& texture){return texture.name=="engine_grid.albedo";});
    if(grid){engine::MaterialDefinition fallback;fallback.name="__engine_grid_default";fallback.albedo=fallback.normal=fallback.orm="engine_grid";fallback.uvScale=5;fallback.roughness=.88f;fallback.metalness=0;definitions.push_back(fallback);}
    std::map<std::string,unsigned> byName;
    std::vector<nativegpu::TextureUpload> uploads;
    uploads.reserve(assets.size());
    for(auto& asset:assets){
        byName[asset.name]=static_cast<unsigned>(uploads.size());
        nativegpu::TextureUpload upload;
        upload.width=asset.width;upload.height=asset.height;upload.mips=asset.mips;
        upload.format=asset.format;upload.srgb=asset.srgb;
        upload.pixels=std::move(asset.pixels);
        uploads.push_back(std::move(upload));
    }
    std::vector<nativegpu::MaterialUpload> materials;
    materials.reserve(definitions.size());
    unsigned defaultIndex=0;
    for(std::size_t index=0;index<definitions.size();++index){
        const auto& definition=definitions[index];
        const auto albedo=byName.find(definition.albedo+".albedo");
        const auto normal=byName.find(definition.normal+".normal");
        const auto orm=byName.find(definition.orm+".orm");
        const auto emissive=byName.find(definition.emissive+".emissive");
        const auto occlusion=byName.find(definition.occlusion+".occlusion");
        if(albedo==byName.end()||normal==byName.end()||orm==byName.end()){
            std::string missing=albedo==byName.end()?definition.albedo+".albedo":
                normal==byName.end()?definition.normal+".normal":definition.orm+".orm";
            app.errorCode="NG-E-MAT-TEX";
            diagnostic("Material "+definition.name+" missing texture "+missing);
            return false;
        }
        if(definition.name=="default"||definition.name=="__engine_grid_default")defaultIndex=static_cast<unsigned>(index);
        nativegpu::MaterialUpload material;
        material.name=definition.name;
        material.albedo=albedo->second;
        material.normal=normal->second;
        // BC5 stores normal X/Y only; bit 31 tells the shader to reconstruct Z.
        if(uploads[normal->second].format==3)material.normal|=0x80000000u;
        material.orm=orm->second;
        material.uvScale=definition.uvScale;
        material.roughness=definition.roughness;
        material.metalness=definition.metalness;
        material.wetness=definition.wetness;
        material.wear=definition.wear;
        material.clearcoat=definition.clearcoat;
        material.clearcoatRoughness=definition.clearcoatRoughness;
        material.ior=definition.ior;
        material.alphaMasked=definition.alphaMasked;
        material.alphaBlended=definition.alphaBlended;
        material.opacity=definition.opacity;
        material.alphaCutoff=definition.alphaCutoff;
        if(!definition.emissive.empty()){
            if(emissive==byName.end()){
                app.errorCode="NG-E-MAT-TEX";diagnostic("Material "+definition.name+" missing texture "+definition.emissive+".emissive");return false;
            }
            if(assets[emissive->second].semantic!=4||!uploads[emissive->second].srgb){
                app.errorCode="NG-E-MAT-TEX";diagnostic("Independent emissive texture requires emissive semantic and sRGB storage");return false;
            }
            material.hasEmissive=true;material.emissive=emissive->second;
        }
        std::copy(definition.emissiveFactor,definition.emissiveFactor+3,material.emissiveFactor);
        material.emissiveStrength=definition.emissiveStrength;
        if(!definition.occlusion.empty()){
            if(occlusion==byName.end()||assets[occlusion->second].semantic!=5||uploads[occlusion->second].srgb){app.errorCode="NG-E-MAT-TEX";diagnostic("Independent occlusion requires a linear occlusion texture");return false;}
            material.hasOcclusion=true;material.occlusion=occlusion->second;
        }
        material.occlusionStrength=definition.occlusionStrength;
        for(const auto& slot:{std::make_tuple(definition.coat,"coat",6u,&material.coat,&material.hasCoat),std::make_tuple(definition.coatRoughness,"coatRoughness",7u,&material.coatRoughness,&material.hasCoatRoughness),std::make_tuple(definition.coatNormal,"coatNormal",8u,&material.coatNormal,&material.hasCoatNormal)})if(!std::get<0>(slot).empty()){
            const auto found=byName.find(std::get<0>(slot)+"."+std::get<1>(slot));if(found==byName.end()||assets[found->second].semantic!=std::get<2>(slot)||uploads[found->second].srgb){app.errorCode="NG-E-MAT-TEX";diagnostic("Coating requires a matching linear texture");return false;}*std::get<3>(slot)=found->second;*std::get<4>(slot)=true;
            if(std::get<2>(slot)==8&&uploads[found->second].format==3)*std::get<3>(slot)|=0x80000000u;
        }
        material.unlit=definition.unlit;
        constexpr const char* layerNames[]={"albedo","normal","orm","roughness","metallic","occlusion","emissive","height","mask"};
        constexpr unsigned layerSemantics[]={0,1,2,10,11,5,4,9,3};
        material.authoredLayers=static_cast<unsigned>(definition.layers.size());
        std::vector<unsigned> layerRemap(definition.layers.size(),0xffffffffu);
        for(std::size_t layerIndex=0;layerIndex<definition.layers.size();++layerIndex){
            const auto& layer=definition.layers[layerIndex];
            auto gpu=layer.gpu;
            for(unsigned slot=0;slot<9;++slot)if(!layer.textures[slot].empty()){
                const auto found=byName.find(layer.textures[slot]+"."+layerNames[slot]);
                if(found==byName.end()||assets[found->second].semantic!=layerSemantics[slot]){app.errorCode="NG-E-MAT-TEX";diagnostic("Layer "+layer.id+" missing or invalid texture "+layer.textures[slot]+"."+layerNames[slot]);return false;}
                gpu.slots[slot].texture=found->second;
                if(slot==1&&uploads[found->second].format==3)gpu.slots[slot].flags|=1u;
            }
            // Disabled layers and zero-weight constant overlays have no runtime effect;
            // their texture dependencies were still validated above.
            if((gpu.flags&4u)!=0||(layerIndex>0&&gpu.source==0&&gpu.weight==0))continue;
            layerRemap[layerIndex]=static_cast<unsigned>(material.layers.size());
            material.layers.push_back(gpu);
        }
        const auto& relief=definition.surfaceRelief;
        if((relief.gpu.flags&3u)!=0){
            material.hasSurfaceRelief=true;material.surfaceRelief=relief.gpu;
            if((relief.gpu.flags&(1u<<4))!=0){
                const auto found=byName.find(relief.texture+".height");
                if(found==byName.end()||assets[found->second].semantic!=9){app.errorCode="NG-E-MAT-TEX";diagnostic("Surface relief height texture "+relief.texture+".height is missing or invalid");return false;}
                material.surfaceRelief.texture=found->second;
                if(uploads[found->second].format!=5)diagnostic("Surface relief height texture "+relief.texture+" is not R16_UNORM; using the 8-bit fallback");
            }else{
                const auto it=std::find_if(definition.layers.begin(),definition.layers.end(),[&](const auto& layer){return layer.id==relief.layer;});
                if(it==definition.layers.end()){app.errorCode="NG-E-MAT-RELIEF";diagnostic("Surface relief height layer "+relief.layer+" is unavailable");return false;}
                const unsigned mapped=layerRemap[static_cast<std::size_t>(it-definition.layers.begin())];
                if(mapped>=material.layers.size()){app.errorCode="NG-E-MAT-RELIEF";diagnostic("Surface relief height layer "+relief.layer+" has no runtime effect");return false;}
                material.surfaceRelief.heightLayer=mapped;
            }
        }
        std::copy(definition.textureMappings,definition.textureMappings+8,material.textureMappings);material.hasTextureMappings=definition.hasTextureMappings;
        materials.push_back(material);
    }
    if(!app.renderer.configureMaterials(std::move(uploads),std::move(materials),error)){
        app.errorCode="NG-E-MAT-CONFIG";
        diagnostic("Material configuration failed: "+error);
        return false;
    }
    app.materialsEnabled=true;
    app.defaultMaterial=defaultIndex;
    return true;
}
// Characters follow the material assigned to their scene entity; part materials still win.
int characterMaterialIndex(const std::string& entityId) {
    for(const auto& entity:app.document.scene.entities)if(entity.id==entityId)return entity.materialIndex;
    return -1;
}
bool spawnEffectRoots(engine::effects::GpuRuntime* gpu=nullptr) {
    const unsigned columns=static_cast<unsigned>(std::ceil(std::sqrt(static_cast<double>(app.effectCount))));
    for(unsigned i=0;i<app.effectCount;++i) {
        const engine::Vec3 offset{(static_cast<float>(i%columns)-(columns-1)*.5f)*.75f,0,
            (static_cast<float>(i/columns)-(columns-1)*.5f)*.75f};
        const auto spawned=gpu?gpu->spawn(app.effectId,app.effectOrigin+offset,app.seed+i,app.effectFields?app.effectFields->binding():app.simulationFields?app.simulationFields->binding():nullptr):app.effects.spawn(app.effectId,app.effectOrigin+offset,app.seed+i);
        if(spawned.status==engine::effects::SpawnStatus::Capacity){++app.effectRejected;continue;}
        if(spawned.status!=engine::effects::SpawnStatus::Accepted){std::fprintf(stderr,"effect spawn '%s' failed: %s\n",app.effectId.c_str(),spawned.reason.c_str());app.errorCode="NG-E-EFFECT-UNSUPPORTED";app.error=spawned.reason;return false;}
        app.effectHandles.push_back(spawned.handle);
    }
    return true;
}
bool driveGpuEffects(nativegpu::GpuEffectFrame& frame,std::string& error) {
    auto& runtime=frame.runtime;const auto tick=app.game.ticks();
    if(app.effectGpuEpoch!=frame.epoch){
        // Visual state restarts from its seed after device/renderer recreation.
        // Authority and the game clock remain owned by the game runtime.
        app.effectGpuEpoch=frame.epoch;app.effectGameTick=tick;app.effectHandles.clear();app.effectStopped=false;
        if(app.effectFields&&!app.effectFields->activate(frame,error))return false;
        if(!spawnEffectRoots(&runtime)){error=app.error;return false;}
    }
    if(tick<app.effectGameTick){runtime.clear();app.effectHandles.clear();app.effectStopped=false;app.effectGameTick=0;if(app.effectFields&&!app.effectFields->activate(frame,error))return false;if(!spawnEffectRoots(&runtime)){error=app.error;return false;}}
    if(app.simulationFields){auto view=app.simulationFields->binding();for(auto h:app.effectHandles)if(runtime.valid(h)&&!runtime.bindField(h,view,error))return false;}
    while(app.effectGameTick<tick){
        if(app.effectFields?!app.effectFields->step(frame,app.effectHandles,error):app.simulationFields?(app.simulationFields->binding()?!frame.stepField(app.simulationFields->binding()->requestedTick(),error):!frame.step(error)):!frame.step(error))return false;++app.effectGameTick;
        const auto age=runtime.stats().tick;
        if(app.effectStopTick&&age>=app.effectStopTick&&!app.effectStopped){for(auto handle:app.effectHandles)runtime.stop(handle,true);app.effectStopped=true;}
        app.effectHandles.erase(std::remove_if(app.effectHandles.begin(),app.effectHandles.end(),[&](auto h){return !runtime.valid(h);}),app.effectHandles.end());
        if(!app.effectStopped&&app.effectRepeatTicks&&age%app.effectRepeatTicks==0){if(app.effectFields&&!app.effectFields->activate(frame,error))return false;if(!spawnEffectRoots(&runtime)){error=app.error;return false;}}
    }
    if(app.effectFields&&app.effectHandles.empty()&&!app.effectFields->release(frame,error))return false;
    return true;
}
// Effect libraries are engine content, but the host is often launched with a project as
// its working directory. Engine content therefore resolves next to the executable first
// and only then against the working directory: a project named `assets` in the cwd must
// not capture `assets`, and `build/effects/library.json` must mean the same file from
// both places.
std::filesystem::path engineContent(const std::filesystem::path& relative){
    std::error_code ec;
    wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);
    const auto folder=std::filesystem::path(exe).parent_path();
    for(const auto& base:{folder,folder.parent_path(),folder.parent_path().parent_path()}){
        const auto candidate=base/relative;
        if(std::filesystem::exists(candidate,ec))return candidate;
    }
    return relative;
}
// Sprite textures are part of the locked library, so the dependency list decides which
// textures the effect bank must own. The root is explicit because the engine library and
// a project library resolve the same dependency names against different trees.
bool loadEffectTextures(const engine::effects::Library& library,const std::filesystem::path& assetRoot){
    for(const auto& dependency:library.dependencies) {
        const auto file=assetRoot/std::filesystem::u8path(dependency.first);
        if(file.extension()!=L".ngtex")continue;
        std::vector<host::TextureAsset> loaded;std::string error;
        if(!host::loadTextureDirectory(file.parent_path(),loaded,error))throw engine::effects::Error("NG-E-EFFECT-TEXTURE",error);
        const auto found=std::find_if(loaded.begin(),loaded.end(),[&](const host::TextureAsset& texture){return texture.name==file.stem().u8string();});
        if(found==loaded.end())throw engine::effects::Error("NG-E-EFFECT-TEXTURE","Texture dependency not found");
        nativegpu::TextureUpload upload;
        upload.width=found->width;upload.height=found->height;upload.mips=found->mips;upload.format=found->format;upload.srgb=found->srgb;upload.pixels=found->pixels;
        app.effectTextureSlots[dependency.first]=static_cast<unsigned>(app.effectTextureUploads.size());
        app.effectTextureUploads.push_back(std::move(upload));
    }
    if(!app.renderer.configureEffectTextures(app.effectTextureUploads,app.error)){app.errorCode="NG-E-EFFECT-TEXTURE";return false;}
    return true;
}
// Turns recorded water contacts into discrete droplets. Only the number of droplets is
// scaled by the contact speed: particle velocity, lifetime and the water simulation stay
// exactly as the asset and solver define them.
bool driveWaterSplash(nativegpu::GpuEffectFrame& frame,std::string& error){
    auto& runtime=frame.runtime;auto& splash=app.splash;
    const auto tick=app.game.ticks();
    const auto* body=app.pondDemo?app.pondDemo->bodyHandle().get():nullptr;
    if(splash.epoch!=frame.epoch){
        // The renderer owns the GPU runtime; a new epoch means new resources and no
        // surviving instances, so the handles and the local tick mirror restart too. The
        // event watermark follows the current tick: a restart must not replay the
        // contacts that were already on screen before it.
        splash.epoch=frame.epoch;splash.handles.clear();splash.gameTick=tick;
        splash.lastTimelineEpoch=body?body->timelineEpoch():0;
        // Attaching to a body must not replay contacts that predate this render path.
        splash.lastSequence=body&&!body->events().empty()?body->events().back().sequence:0;
        splash.body=body;
    }else if(body!=splash.body){
        // A reloaded scene owns a fresh body whose event timeline restarts; keeping the
        // old watermark would filter out every future contact.
        splash.body=body;splash.handles.clear();splash.gameTick=tick;
        splash.lastTimelineEpoch=body?body->timelineEpoch():0;
        splash.lastSequence=body&&!body->events().empty()?body->events().back().sequence:0;
    }
    if(tick<splash.gameTick){runtime.clear();splash.handles.clear();splash.gameTick=0;splash.lastSequence=0;}
    // The body keeps a bounded history rather than a consumed queue, so the cursor is
    // (timelineEpoch, sequence). Two render paths reading the same ring still spawn one
    // burst, while two contacts in one tick both spawn.
    if(app.pondDemo)for(const auto& event:app.game.waterEvents()){
        if(event.timelineEpoch!=splash.lastTimelineEpoch){
            if(event.timelineEpoch<splash.lastTimelineEpoch)continue;
            splash.lastTimelineEpoch=event.timelineEpoch;splash.lastSequence=0;
        }
        if(event.sequence<=splash.lastSequence)continue;
        splash.lastSequence=event.sequence;++splash.contacts;
        // Droplets belong to entry, landing and rigid impacts. A drag sample every few
        // centimetres is already represented by the water wave and would flood the
        // particle runtime with duplicate bursts.
        if(event.kind==static_cast<std::uint8_t>(engine::water::ContactKind::Drag)||
           event.kind==static_cast<std::uint8_t>(engine::water::ContactKind::Leaving))continue;
        const auto surface=app.pondDemo->query(event.x,event.z);
        if(!surface.valid)continue;
        engine::effects::SpawnParams params;
        float splashStrength=std::max(event.impactSpeed,event.normalSpeed*.6f);
        if(event.kind==static_cast<std::uint8_t>(engine::water::ContactKind::Drag))splashStrength*=.35f;
        params.intensity=std::clamp(splashStrength/2.f,.15f,1.f);
        const auto spawned=runtime.spawn(splash.id,{event.x,surface.surfaceHeightMetres+.01f,event.z},
                                         app.seed+event.sequence*2654435761u+event.timelineEpoch,{},params);
        if(spawned.status==engine::effects::SpawnStatus::Accepted){
            splash.handles.push_back(spawned.handle);++splash.spawned;
            splash.lastIntensity=params.intensity;splash.peakIntensity=std::max(splash.peakIntensity,params.intensity);
        }else if(spawned.status==engine::effects::SpawnStatus::Capacity){++splash.capacityRejected;}
        else {
            ++splash.failed;
            if(splash.failed==1)std::fprintf(stderr,"water splash spawn '%s' failed: %s\n",splash.id.c_str(),spawned.reason.c_str());
        }
    }
    // Particle ticks follow game ticks. A long stall is bounded instead of replayed, and
    // the abandoned ticks are accounted rather than silently skipped.
    unsigned steps=0;
    while(splash.gameTick<tick&&steps<8){if(!frame.step(error))return false;++splash.gameTick;++steps;}
    if(splash.gameTick<tick){splash.stepsDropped+=tick-splash.gameTick;splash.gameTick=tick;}
    const auto before=splash.handles.size();
    splash.handles.erase(std::remove_if(splash.handles.begin(),splash.handles.end(),
        [&](auto handle){return !runtime.valid(handle);}),splash.handles.end());
    splash.expired+=before-splash.handles.size();
    return true;
}
// Per-frame (not per fixed tick) surface history. The value only feeds the material
// wetness override; it never changes the water authority or the replay digest.
void updateWaterWetness(float dtSeconds){
    app.wetEntities=0;app.maxWetness=0;
    const float dt=std::clamp(dtSeconds,0.0f,0.1f);
    for(auto& item:app.wetness){
        auto& state=item.second;
        float target=0;
        engine::physics::Aabb bounds;
        const bool haveBounds=app.game.entityColliderBounds(item.first,bounds);
        if(!haveBounds){bounds.min=state.centre-state.half;bounds.max=state.centre+state.half;}
        if(app.pondDemo&&bounds.valid()){
            const float hx=(bounds.max.x-bounds.min.x)*.5f,hz=(bounds.max.z-bounds.min.z)*.5f;
            const float cx=(bounds.min.x+bounds.max.x)*.5f,cz=(bounds.min.z+bounds.max.z)*.5f;
            const float ox=hx*.45f,oz=hz*.45f;
            const float xs[5]={cx,cx-ox,cx+ox,cx-ox,cx+ox};
            const float zs[5]={cz,cz-oz,cz-oz,cz+oz,cz+oz};
            std::array<host::WetnessSample,5> samples{};
            bool anySurface=false;float surface=0;
            for(int i=0;i<5;++i){
                const auto query=app.game.queryWater(xs[i],zs[i]);
                if(query.valid&&query.depthMetres>0.005f){
                    samples[i].valid=true;samples[i].surfaceHeightMetres=query.surfaceHeightMetres;
                    surface=query.surfaceHeightMetres;anySurface=true;
                }
            }
            // A tall wall would be uniformly darkened over its whole height; that is not
            // a contact mark. Low rocks, banks and beds participate instead.
            const float top=bounds.max.y,height=top-bounds.min.y;
            if(anySurface&&top<=surface+1.2f&&height<=4.0f)
                target=host::wetnessTarget(samples,bounds.min.y,state.largeFlat);
        }
        host::WetnessState value{state.value};
        host::updateWetness(value,target,dt>0?dt:1.f/60.f);
        state.value=value.value;
        if(app.surfaceWetness&&state.value>0.5f)++app.wetEntities;
        app.maxWetness=std::max(app.maxWetness,state.value);
    }
}
void syncWaterfalls(){
    std::vector<nativegpu::WaterfallFrame> frames;
    if(app.pondDemo){
        const auto primaryOrigin=app.pondDemo->body().origin();
        for(const auto& runtime:app.waterLinks){
            const auto& settings=runtime.settings;
            const auto upperOrigin=runtime.upstream->body().origin();
            const float sourceX=upperOrigin.x+settings.sourceX,sourceZ=upperOrigin.z+settings.sourceZ;
            const float landingX=primaryOrigin.x+settings.landingX,landingZ=primaryOrigin.z+settings.landingZ;
            const auto source=runtime.upstream->query(sourceX,sourceZ);
            const auto landing=app.pondDemo->query(landingX,landingZ);
            if(!source.valid||!landing.valid)continue;
            // A sheet needs an actual drop. A level link still transfers water but has
            // nothing to draw.
            if(source.surfaceHeightMetres<=landing.surfaceHeightMetres+.05f)continue;
            nativegpu::WaterfallFrame frame;
            frame.source[0]=sourceX;frame.source[1]=source.surfaceHeightMetres+.01f;frame.source[2]=sourceZ;
            frame.landing[0]=landingX;frame.landing[1]=landing.surfaceHeightMetres+.01f;frame.landing[2]=landingZ;
            frame.widthMetres=runtime.widthMetres;
            frame.flowCubicMetresPerSecond=runtime.link->diagnostics().lastActualFlowCubicMetresPerSecond;
            frame.time=float(app.game.ticks())/120.f;
            frames.push_back(frame);
        }
    }
    app.renderer.setWaterfalls(std::move(frames));
}
bool syncWaterSurfaces(std::string& error){
    updateWaterWetness(1.0f/60.0f);
    if(!app.renderer.setWaterSurface(app.pondDemo?app.pondDemo->surface(app.waterDebug,app.waterLod):nativegpu::WaterSurfaceFrame{},error))return false;
    if(app.waterLinks.empty()){
        if(!app.renderer.setUpperWaterSurface({},error))return false;
    }else if(!app.renderer.setUpperWaterSurface(app.waterLinks.front().upstream->surface(app.waterDebug,app.waterLod),error)){
        return false;
    }
    syncWaterfalls();
    return true;
}
bool initializeSplash(){
    try{
        if(!app.effectId.empty())throw engine::effects::Error("NG-E-SPLASH-CONFLICT","Water splash and --effect share one GPU effect runtime; run them separately");
        if(!app.pondDemo)throw engine::effects::Error("NG-E-SPLASH-WATER","Water splash requires an enabled water body");
        const auto libraryPath=engineContent(app.splash.libraryPath),assetRoot=engineContent(app.splash.assetRoot);
        app.splash.library=engine::effects::loadLibrary(libraryPath.u8string(),assetRoot.u8string());
        const auto found=app.splash.library.assets.find(app.splash.id);
        if(found==app.splash.library.assets.end())throw engine::effects::Error("NG-E-SPLASH-ASSET","Splash asset is missing from the effect library: "+app.splash.id);
        if(!loadEffectTextures(app.splash.library,assetRoot))return false;
        std::vector<std::shared_ptr<const engine::effects::Asset>> assets{found->second};
        if(!app.renderer.configureGpuEffects(std::move(assets),app.effectTextureSlots,driveWaterSplash,app.error)){app.errorCode="NG-E-SPLASH-CONFIG";return false;}
        app.splash.enabled=true;
        return true;
    }catch(const engine::effects::Error& error){app.errorCode=error.code;app.error=error.what();return false;}
    catch(const std::exception& error){app.errorCode="NG-E-SPLASH-ASSET";app.error=error.what();return false;}
}
bool initializeSimulationFields(){
    if(app.simulationFieldPath.empty())return true;
    try{
        if(!app.effectFieldPath.empty()||(app.effectId.empty()&&app.clothOptions.asset.empty())||(!app.effectId.empty()&&!app.gpuEffectRequested))throw std::invalid_argument("NG-E-FIELD-SOURCE: shared fields require GPU consumers and cannot be combined with --effect-field");
        app.simulationFields=std::make_unique<host::SimulationFields>(app.simulationFieldPath.u8string());
        engine::fields::GpuLimits limits;limits.maxSnapshots=16;limits.maxAllocationBytes=16*1024*1024;
        if(!app.renderer.configureSimulationFields(app.simulationFields->world(),limits,{32,2*1024*1024},[](nativegpu::GpuFieldFrame& frame,std::string& error){const bool active=!!app.clothPreview||app.effectGpuEpoch!=frame.effectEpoch||!app.effectHandles.empty()||(app.effectRepeatTicks&&!app.effectStopped);return app.simulationFields->drive(frame,app.game.ticks(),error,active);},app.error)){app.errorCode="NG-E-FIELD-SOURCE";return false;}
        return true;
    }catch(const std::exception& e){app.error=e.what();app.errorCode="NG-E-FIELD-SOURCE";return false;}
}
bool initializeEffects() {
    // Water splash is the only GPU effect consumer in a plain pond scene, so it owns the
    // runtime when no --effect was requested. Both share one runtime by construction.
    if(app.splash.requested)return initializeSplash();
    if(app.effectId.empty()){if(!app.effectFieldPath.empty()){app.errorCode="NG-E-FIELD-SOURCE";app.error="A field source requires an effect";return false;}return true;}
    try {
        app.effectLibrary=engine::effects::loadLibrary(app.effectLibraryPath.u8string(),app.document.scene.assetRoot);
        if(app.simulationFields&&(!app.effectLibrary.assets.count(app.effectId)||!app.effectLibrary.assets.at(app.effectId)->hasField))throw engine::effects::Error("NG-E-FIELD-SOURCE","Shared field particles require an explicit field declaration");
        if(!app.effectFieldPath.empty()){
            if(!app.gpuEffectRequested||!app.effectLibrary.assets.count(app.effectId)||!app.effectLibrary.assets.at(app.effectId)->hasField)throw engine::effects::Error("NG-E-FIELD-SOURCE","A field source requires a GPU effect with an explicit field declaration");
            app.effectFields=std::make_unique<host::EffectFields>(app.effectFieldPath.u8string());
            engine::fields::GpuLimits limits;limits.maxSnapshots=16;limits.maxAllocationBytes=16*1024*1024;
            if(!app.renderer.configureGpuEffectFields(app.effectFields->world(),limits,{32,2*1024*1024},app.error)){app.errorCode="NG-E-FIELD-SOURCE";return false;}
        }
        if(!app.gpuEffectRequested)for(const auto& asset:app.effectLibrary.assets)app.effects.install(asset.second);
        if(!loadEffectTextures(app.effectLibrary,std::filesystem::u8path(app.document.scene.assetRoot)))return false;
        if(app.gpuEffectRequested){
            std::vector<std::shared_ptr<const engine::effects::Asset>> assets;
            for(const auto& item:app.effectLibrary.assets)assets.push_back(item.second);
            if(!app.renderer.configureGpuEffects(std::move(assets),app.effectTextureSlots,driveGpuEffects,app.error)){app.errorCode="NG-E-EFFECT-CONFIG";return false;}
            return true;
        }
        app.effectGameTick=app.game.ticks();
        return spawnEffectRoots();
    }catch(const engine::effects::Error& error){app.errorCode=error.code;app.error=error.what();return false;}
    catch(const std::exception& error){app.error=error.what();app.errorCode=app.error.rfind("NG-E-FIELD-SOURCE",0)==0?"NG-E-FIELD-SOURCE":"NG-E-EFFECT-ASSET";return false;}
}

bool initializeCloth(){
    if(app.clothOptions.asset.empty())return true;
    try{
        app.clothOptions.simulationWind=!!app.simulationFields;
        app.clothPreview=std::make_unique<host::ClothPreview>(app.clothOptions);
        if(!app.renderer.configureGpuCloth(app.clothPreview->configuration(),[](nativegpu::GpuClothFrame& frame,std::string& error){return app.clothPreview->drive(frame,app.game.ticks(),error);},app.error)){app.errorCode="NG-E-CLOTH-CONFIG";return false;}
        return true;
    }catch(const engine::cloth::Error& e){app.errorCode=e.code;app.error=e.what();return false;}
    catch(const std::exception& e){app.errorCode="NG-E-CLOTH-CONFIG";app.error=e.what();return false;}
}
int groomHeadBoneIndex(const engine::animation::Skeleton& skeleton) {
    int fallback = -1;
    for (std::size_t index = 0; index < skeleton.bones.size(); ++index) {
        std::string name = skeleton.bones[index].name;
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, "head") == 0
            && name.find("headtop") == std::string::npos) return static_cast<int>(index);
        if (fallback < 0 && name.find("head") != std::string::npos) fallback = static_cast<int>(index);
    }
    return fallback;
}

bool groomBoneDescendsFrom(const engine::animation::Skeleton& skeleton, int bone, int ancestor) {
    for (int index = bone; index >= 0 && index < static_cast<int>(skeleton.bones.size());
         index = skeleton.bones[index].parent)
        if (index == ancestor) return true;
    return false;
}

// Rotation about the world Y axis through `pivot`, column-major.
nativegpu::Mat4 groomRotationAboutY(float radians, const float pivot[3]) {
    const float cosine = std::cos(radians), sine = std::sin(radians);
    nativegpu::Mat4 result{};
    result.m[0] = cosine; result.m[2] = -sine;
    result.m[5] = 1.0f;
    result.m[8] = sine; result.m[10] = cosine;
    result.m[15] = 1.0f;
    // result = T(pivot) * R * T(-pivot): translation column is pivot - R * pivot.
    for (int row = 0; row < 3; ++row)
        result.m[12 + row] = pivot[row] - (result.m[row] * pivot[0]
            + result.m[4 + row] * pivot[1] + result.m[8 + row] * pivot[2]);
    return result;
}

// Attaches the resident hairstyle to the player's head bone. The head subtree is turned
// about the posed head origin so the mesh and the strands move together; the renderer
// receives the same model * skin transform for the hair pass.
bool applyGroomHeadPose(engine::CharacterPose& character) {
    if (!character.asset) return false;
    const auto& skeleton = character.asset->skeleton;
    const int head = groomHeadBoneIndex(skeleton);
    if (head < 0 || head >= static_cast<int>(character.palette.size())) return false;
    const auto toMat4 = [](const engine::animation::Matrix& value) {
        nativegpu::Mat4 result{};
        std::copy(value.begin(), value.end(), result.m);
        return result;
    };
    const auto toEngine = [](const nativegpu::Mat4& value) {
        engine::animation::Matrix result{};
        std::copy(value.m, value.m + 16, result.begin());
        return result;
    };
    const auto model = engine::compose(character.position, character.scale, character.yaw,
        character.pitch, character.roll);
    nativegpu::Mat4 modelMatrix{};
    std::copy(model.m, model.m + 16, modelMatrix.m);
    nativegpu::Mat4 skin = toMat4(character.palette[head]);
    if (app.groomHeadTurnDegrees != 0.0f) {
        // Ramp over the first second so the turn is a motion, not a teleport.
        const float ramp = std::min(1.0f, static_cast<float>(app.frames) / 60.0f);
        const float radians = app.groomHeadTurnDegrees * ramp * 3.14159265358979323846f / 180.0f;
        const float pivot[3] = {skin.m[12], skin.m[13], skin.m[14]};
        const nativegpu::Mat4 turn = groomRotationAboutY(radians, pivot);
        for (int index = 0; index < static_cast<int>(character.palette.size()); ++index)
            if (groomBoneDescendsFrom(skeleton, index, head))
                character.palette[index] = toEngine(nativegpu::multiplyMatrix(turn,
                    toMat4(character.palette[index])));
        skin = nativegpu::multiplyMatrix(turn, skin);
    }
    app.renderer.setHairHeadTransform(nativegpu::multiplyMatrix(modelMatrix, skin));
    return true;
}

bool initializeGroom(){
    if(app.groomPath.empty())return true;
    try{
        auto pack=engine::hair::loadGpuPack(app.groomPath.u8string());
        if(!app.hasCameraOverride){
            // Frame the strands from their own bounds. A hairstyle is about 0.3 m across,
            // so a fixed scene camera would miss it entirely.
            float centre[3]{}, extent=0;
            for(int axis=0;axis<3;++axis){
                centre[axis]=(pack.boundsMin[axis]+pack.boundsMax[axis])*.5f;
                extent=std::max(extent,pack.boundsMax[axis]-pack.boundsMin[axis]);
            }
            const float distance=std::max(0.25f,extent*2.2f);
            app.hasCameraOverride=true;
            app.cameraEye[0]=centre[0];app.cameraEye[1]=centre[1]+extent*0.05f;app.cameraEye[2]=centre[2]+distance;
            app.cameraTarget[0]=centre[0];app.cameraTarget[1]=centre[1];app.cameraTarget[2]=centre[2];
        }
        nativegpu::HairSetup setup;
        setup.pack=std::move(pack);
        setup.material.albedo[0]=0.055f;setup.material.albedo[1]=0.036f;setup.material.albedo[2]=0.024f;
        setup.material.roughness=0.34f;
        setup.material.rootToTipTint[0]=1;setup.material.rootToTipTint[1]=1;setup.material.rootToTipTint[2]=1;
        setup.shadowStrength=app.groomShadowStrength;
        setup.debugView=app.groomDebugView;
        setup.simulation.enabled=app.groomSimulate;
        for(int axis=0;axis<3;++axis)setup.simulation.wind[axis]=app.groomWind[axis];
        if(!app.renderer.configureHair(std::move(setup),app.error)){app.errorCode="NG-E-HAIR-CONFIG";return false;}
        app.groomConfigured=true;
        return true;
    }catch(const engine::hair::Error& e){app.errorCode=e.code;app.error=e.what();return false;}
    catch(const std::exception& e){app.errorCode="NG-E-HAIR-ASSET";app.error=e.what();return false;}
}
bool synchronizeEffects() {
    if(app.effectId.empty())return true;
    if(app.gpuEffectRequested)return true; // logical ticks are recorded on the renderer's queue
    try {
        const auto tick=app.game.ticks();
        if(tick<app.effectGameTick){app.effects.clear();app.effectHandles.clear();app.effectStopped=false;app.effectGameTick=0;if(!spawnEffectRoots())return false;}
        while(app.effectGameTick<tick) {
            app.effects.step();++app.effectGameTick;
            const auto age=app.effects.stats().tick;
            if(app.effectStopTick&&age>=app.effectStopTick&&!app.effectStopped){for(auto handle:app.effectHandles)app.effects.stop(handle,true);app.effectStopped=true;}
            app.effectHandles.erase(std::remove_if(app.effectHandles.begin(),app.effectHandles.end(),[](auto h){return !app.effects.valid(h);}),app.effectHandles.end());
            if(!app.effectStopped&&app.effectRepeatTicks&&age%app.effectRepeatTicks==0)if(!spawnEffectRoots())return false;
        }
        std::vector<nativegpu::EffectSprite> sprites;
        for(const auto& particle:app.effects.sprites()) {
            const auto texture=app.effectTextureSlots.find(particle.texture);
            if(texture==app.effectTextureSlots.end())throw engine::effects::Error("NG-E-EFFECT-TEXTURE","Sprite texture missing from locked library");
            nativegpu::EffectSprite sprite;
            sprite.position[0]=particle.position.x;sprite.position[1]=particle.position.y;sprite.position[2]=particle.position.z;
            std::copy(particle.color.begin(),particle.color.end(),sprite.color);std::copy(particle.size.begin(),particle.size.end(),sprite.size);
            sprite.texture=texture->second;sprite.blend=static_cast<unsigned>(particle.blend);sprite.stableId=particle.stableId;sprite.rotation=particle.rotation;
            sprites.push_back(sprite);
        }
        if(!app.rhi->setEffectSprites(std::move(sprites),app.error)){app.errorCode="NG-E-EFFECT-SPRITE";return false;}
        return true;
    }catch(const engine::effects::Error& error){app.errorCode=error.code;app.error=error.what();return false;}
}
void applySceneEnvironment() {
    const engine::Lighting& source = app.document.scene.lighting;
    nativegpu::Environment environment;
    environment.panorama=source.panorama;environment.environmentIntensity=source.environmentIntensity;environment.environmentRotation=source.environmentRotation;
    for (int channel = 0; channel < 3; ++channel) {
        environment.ambient[channel] = source.ambient[channel];
        environment.skyColor[channel] = source.skyColor[channel];
        environment.keyDirection[channel] = source.keyDirection[channel];
        environment.keyColor[channel] = source.keyColor[channel];
        environment.fogColor[channel] = source.fogColor[channel];
    }
    environment.keyIntensity = source.keyIntensity;
    environment.fogDensity = source.fogDensity;
    environment.fogHeightFalloff = source.fogHeightFalloff;
    environment.fogBaseHeight = source.fogBaseHeight;
    environment.cloudCoverage = source.cloudCoverage;
    environment.cloudHeight = source.cloudHeight;
    environment.cloudThickness = source.cloudThickness;
    environment.cloudSpeed = source.cloudSpeed;
    environment.toneMap = source.toneMap;
    environment.exposure = source.exposure;
    environment.volumetric = source.fogDensity > 0.0f;
    environment.quality=source.quality;environment.localShadows=source.localShadows;
    environment.ibl=source.ibl;environment.ddgi=source.ddgi;environment.cloudShadows=source.cloudShadows;
    environment.temporal=source.temporal;environment.maxShadowLights=source.maxShadowLights;
    for(const auto& sourceVolume:source.fogVolumes) {
        nativegpu::FogVolume volume;
        volume.center[0]=sourceVolume.center.x;volume.center[1]=sourceVolume.center.y;volume.center[2]=sourceVolume.center.z;
        volume.extent[0]=sourceVolume.extent.x;volume.extent[1]=sourceVolume.extent.y;volume.extent[2]=sourceVolume.extent.z;
        volume.density=sourceVolume.density;environment.fogVolumes.push_back(volume);
    }
    app.renderer.setEnvironment(environment);
}
bool restartRenderer(){
    app.ready=false;
    if(app.rhi)app.rhi->shutdown();
    if(!app.effectId.empty()&&!app.renderer.configureEffectTextures(app.effectTextureUploads,app.error)){app.running=false;return false;}
    if(!configureRendererMaterials()){
        app.running=false;return false;
    }
    applySceneEnvironment();
    nativegpu::rhi::Descriptor descriptor;
    descriptor.window=app.viewport;
    descriptor.width=app.width;
    descriptor.height=app.height;
    descriptor.validation=app.validationRequested;
    // The particle field is built during initialize(), so its toggle must be set first.
    app.rhi->setParticles(app.particlesRequested);
    app.rhi->setFireEffect(app.firePreset,app.explodeAt);
    if(app.particleEmitterSet)app.renderer.setParticleEmitter(app.particleEmitter);
    app.renderer.setParticleTemporal(app.particleTemporal);
    if(!app.rhi||!app.rhi->initialize(descriptor,app.error)){
        app.errorCode="NG-E-RENDERER-INIT";diagnostic(app.error);app.running=false;return false;
    }
    // The DXR state object is rebuilt from scratch, so the requested toggle has to be
    // re-applied and re-read for the report.
    app.rhi->setRtShadows(app.rtShadowRequested);
    app.rhi->setCompositeDebugView(app.debugView);
    app.rhi->setLayerDebugView(app.layerDebugMode,app.layerDebugLayer);
    app.rhi->setMeshShaders(app.meshRequested);
    app.rhi->setFrustumCulling(app.cullRequested);
    app.rhi->setCompute(app.computeRequested,app.computeCorrupt);
    // Re-applied after initialize() because the rebuild resets the particle state.
    app.renderer.setParticleTemporal(app.particleTemporal);
    // Injection must happen after initialize(), which rebuilds the renderer state. It is
    // armed exactly once so the recovery run is a clean renderer.
    if(app.deviceRemovalMode&&app.deviceRemovalAtFrame>0&&!app.deviceRemovalDetected){
        app.renderer.injectDeviceRemovalAtFrame(static_cast<unsigned>(app.deviceRemovalAtFrame));
    }
    app.rtShadowEnabled=app.rhi->rtShadowsEnabled();
    // The device and command lists are new, so the hairstyle buffers have to be uploaded
    // into them again. Skipping this after a device recovery silently drops the hair.
    app.groomConfigured=false;
    if(!initializeGroom()){app.running=false;return false;}
    app.ready=true;++app.restarts;return true;
}
void layout(){
    if(!app.viewport)return;
    RECT r;GetClientRect(app.window,&r);
    int panel=app.game.firstPerson()&&!app.tour?0:310;
    for(auto handle:{app.title,app.mission,app.stats,app.list,GetDlgItem(app.window,201),GetDlgItem(app.window,202),GetDlgItem(app.window,ID_PAUSE),GetDlgItem(app.window,ID_RESET),GetDlgItem(app.window,ID_RELOAD),GetDlgItem(app.window,ID_OPEN),GetDlgItem(app.window,ID_RESTART)})if(handle)ShowWindow(handle,panel?SW_SHOW:SW_HIDE);
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
void mouseLook(bool enabled) {
    if(app.mouseLook==enabled)return;
    app.mouseLook=enabled;app.yawDelta=0;app.pitchDelta=0;
    if(enabled){RECT r;GetClientRect(app.viewport,&r);POINT a{r.left,r.top},b{r.right,r.bottom};ClientToScreen(app.viewport,&a);ClientToScreen(app.viewport,&b);RECT clip{a.x,a.y,b.x,b.y};ClipCursor(&clip);SetCursorPos((a.x+b.x)/2,(a.y+b.y)/2);while(ShowCursor(FALSE)>=0){}SetFocus(app.window);}
    else {ClipCursor(nullptr);while(ShowCursor(TRUE)<0){}}
}
LRESULT CALLBACK viewportProc(HWND h,UINT msg,WPARAM w,LPARAM l) {
    if(app.game.firstPerson()&&(msg==WM_LBUTTONDOWN||msg==WM_RBUTTONDOWN)){app.paused=false;mouseLook(true);return 0;}
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
            app.paused=!app.paused;setText(app.pauseButton,app.paused?L"缁х画杩愯":L"鏆傚仠杩愯");break;
        case ID_RESET:if(!app.game.gameplayRuntime().contentLocked())app.game.reset();break;
        case ID_RELOAD:reloadScene(app.scenePath);break;
        case ID_RESTART:restartRenderer();break;
        case ID_OPEN:{
            wchar_t file[32768]={};
            OPENFILENAMEW ofn={};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=h;
            ofn.lpstrFilter=L"Scene JSON\0*.json\0";ofn.lpstrFile=file;ofn.nMaxFile=32768;
            ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
            if(GetOpenFileNameW(&ofn))reloadScene(std::filesystem::path(file));
            break;
        }}
        SetFocus(h);return 0;
    case WM_INPUT:
        if(app.mouseLook){RAWINPUT raw{};UINT size=sizeof(raw);if(GetRawInputData(reinterpret_cast<HRAWINPUT>(l),RID_INPUT,&raw,&size,sizeof(RAWINPUTHEADER))==size&&raw.header.dwType==RIM_TYPEMOUSE){app.yawDelta+=raw.data.mouse.lLastX*.0024f;app.pitchDelta+=raw.data.mouse.lLastY*.0024f;}}
        return DefWindowProcW(h,msg,w,l);
    case WM_KEYDOWN:
        if((l&(1LL<<30))!=0)return 0;
        if(w==VK_ESCAPE&&app.game.firstPerson()){app.completionVisible=false;app.paused=!app.paused;mouseLook(!app.paused);}
        if(w==VK_F1&&app.game.firstPerson()){mouseLook(false);app.tour=!app.tour;app.paused=true;layout();}
        if(w==VK_F2&&app.tour&&!app.tourDirectory.empty()&&!app.game.gameplayRuntime().contentLocked()){
            const char* names[]={"arrival","warehouse","repair","square"};
            const auto path=std::filesystem::u8path(app.tourDirectory)/(std::string(names[app.tourCheckpoint++%4])+".json");
            try{app.game.load(path.u8string());app.liveStatus=L"已加载完整检查点";}catch(const std::exception& error){app.sceneError=error.what();}
        }
        if(w==VK_F5&&!app.game.gameplayRuntime().contentLocked()){app.game.reset();app.completionVisible=true;}
        if(w==VK_F7&&!app.replayEnabled){try{app.game.save(app.game.firstPerson()?"saves/fogtown-quicksave.json":"saves/quicksave.json");app.notice=L"进度已保存。按 F12 可恢复。";app.liveStatus=L"Saved";}catch(const std::exception& e){app.sceneError=e.what();app.notice=L"保存失败，请查看导览诊断。";}app.noticeUntil=Clock::now()+std::chrono::seconds(5);}
        if(w==VK_F12&&!app.game.gameplayRuntime().contentLocked()){try{app.game.load(app.game.firstPerson()?"saves/fogtown-quicksave.json":"saves/quicksave.json");app.notice=L"进度已恢复。";app.completionVisible=true;app.liveStatus=L"Restored";}catch(const std::exception& e){app.sceneError=e.what();app.notice=L"无法恢复存档，请先按 F7 保存；内容更新后需要重新开始。";}app.noticeUntil=Clock::now()+std::chrono::seconds(5);}
        if(w==VK_F6)SendMessageW(h,WM_COMMAND,ID_PAUSE,0);
        if(w==VK_F8)restartRenderer();
        if(w==VK_F9&&!app.game.gameplayRuntime().contentLocked()){
            app.watch=!app.watch;app.lastWatch=Clock::now();
            app.liveStatus=app.watch?L"Hot reload on":L"Hot reload off";
        }
        if(w==VK_F11){
            app.meshRequested=!app.meshRequested;
            app.rhi->setMeshShaders(app.meshRequested);
            app.liveStatus=app.rhi->meshShadersEnabled()?L"Mesh Shader on":L"Mesh Shader off";
        }
        if(w==VK_F10){
            app.rtShadowRequested=!app.rtShadowRequested;
            app.rhi->setRtShadows(app.rtShadowRequested);
            app.rtShadowEnabled=app.rhi->rtShadowsEnabled();
            app.liveStatus=app.rtShadowEnabled?L"DXR shadows on":L"DXR shadows off";
        }
        return 0;
    case WM_ACTIVATE:
        if(LOWORD(w)==WA_INACTIVE){mouseLook(false);if(app.game.firstPerson()&&!app.smoke)app.paused=true;app.dragging=false;ReleaseCapture();app.yawDelta=0;app.pitchDelta=0;}
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(w),TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(w),RGB(218,226,236));
        return reinterpret_cast<LRESULT>(app.background);
    case WM_ERASEBKGND:{
        RECT r;GetClientRect(h,&r);FillRect(reinterpret_cast<HDC>(w),&r,app.background);return 1;
    }
    case WM_CLOSE:mouseLook(false);app.running=false;return 0;
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
    if(app.game.firstPerson())for(auto actor:app.game.gameplayRuntime().world.query("objective")){
        const auto& objective=*app.game.gameplayRuntime().world.get(actor,"objective");
        task=wide(objective.value("title",std::string()))+L"\n"+wide(objective.value("detail",std::string()));break;
    }
    setText(app.mission,task);
    std::wostringstream s;
    auto stats=app.rhi->stats();
    s<<std::fixed<<std::setprecision(2)<<L"CPU 鎻愪氦 "<<stats.cpuMs<<L" ms   GPU ";
    if(stats.gpuValid)s<<stats.gpuMs<<L" ms";else s<<L"n/a";
    s<<L"\nPosition "<<p.x<<L", "<<p.y<<L", "<<p.z;
    s<<L"\n"<<(app.paused?L"paused":L"running")<<L"  |  revision "<<app.document.revision;
    s<<L"  |  "<<(app.watch?L"Hot reload on":L"Hot reload off");
    const auto physics=app.game.physicsStats();
    s<<L"\nPhysics "<<physics.activeBodies<<L" bodies, "<<physics.debris<<L" debris";
    s<<L"  |  DXR "<<(app.rtShadowEnabled?L"on":L"off");
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
// ---- audio ------------------------------------------------------------------
// The host owns the audio lifecycle but never the mix. Everything below only
// translates scene data and gameplay edges into commands on the mixer's ring;
// the mixer decides when they land, in samples.
engine::audio::Quality audioQualityFromName(const std::string& name){
    if(name=="off")return engine::audio::Quality::Off;
    if(name=="stereo")return engine::audio::Quality::Stereo;
    if(name=="binaural")return engine::audio::Quality::Binaural;
    if(name=="atmos")return engine::audio::Quality::Atmos;
    return engine::audio::Quality::Spatial;
}
engine::audio::Layout audioLayoutFromName(const std::string& name){
    if(name=="stereo"||name=="2.0")return engine::audio::Layout::Stereo;
    if(name=="5.1")return engine::audio::Layout::Surround51;
    if(name=="7.1")return engine::audio::Layout::Surround71;
    return engine::audio::Layout::Surround712;
}
engine::audio::Bus audioBusFromName(const std::string& name){
    if(name=="ambience")return engine::audio::Bus::Ambience;
    if(name=="music")return engine::audio::Bus::Music;
    if(name=="voice")return engine::audio::Bus::Voice;
    if(name=="ui")return engine::audio::Bus::Ui;
    return engine::audio::Bus::Sfx;
}
// Bank paths in the manifest are repo-relative so the same file works from the
// repo root and from a copied run package next to the executable.
std::filesystem::path audioResolve(const std::string& relative){
    std::filesystem::path path=std::filesystem::u8path(relative);
    std::error_code ec;
    if(std::filesystem::exists(path,ec))return path;
    wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);
    const auto folder=std::filesystem::path(exe).parent_path();
    for(const auto& base:{folder,folder.parent_path(),folder.parent_path().parent_path()}){
        const auto candidate=base/path;
        if(std::filesystem::exists(candidate,ec))return candidate;
    }
    return path;
}
bool audioConfigure(){
    if(app.audioOff){app.audioError.clear();return true;}
    engine::audio::Config config;
    config.sampleRate=engine::audio::kSampleRate;
    config.blockFrames=256;
    config.quality=audioQualityFromName(app.audioQualityName);
    if(config.quality==engine::audio::Quality::Off){
        app.audioOff=true;app.audioError.clear();return true;
    }
    config.layout=audioLayoutFromName(app.audioLayoutName);
    config.masterGainDb=app.document.audio.masterGainDb;
    config.loudnessTargetLufs=app.document.audio.loudnessTargetLufs;
    // 64 physical voices is the documented default; binaural costs two delay lines
    // per voice, so the Atmos tier keeps the same budget and relies on virtualization.
    config.maxPhysicalVoices=64;
    config.maxVirtualVoices=1024;
    config.maxObjects=32;
    // Offline pulls whole frames at a time, so the same lead is used there; a fixed
    // lead is what keeps two offline runs bit-identical.
    config.commandLeadSamples=config.blockFrames*2;
    std::string error;
    if(!app.audio.configure(config,error)){
        app.audioError="NG-E-AUDIO-CONFIG: "+error;
        app.audioOff=true;
        return false;
    }
    app.audioConfigured=true;
    // The offline backend renders into a scratch block that holds up to 100 ms, so a
    // 60 fps frame (800 samples) always fits in one pull. Allocated here, once.
    app.audioOffline=(app.audioBackendName=="offline");
    if(app.audioOffline){
        const uint32_t channels=engine::audio::layoutChannels(app.audio.config().layout);
        app.audioScratch.assign(static_cast<size_t>(engine::audio::kSampleRate/10)*channels,0.0f);
    }
    return true;
}
bool audioLoadBank(){
    if(app.audioOff||!app.audioConfigured)return true;
    // Scene references resolve relative to the scene and were decoded atomically
    // by loadScene. A portable imported scene needs no working-directory bank.
    bool needsLegacy=app.document.audio.sounds.empty();
    for(const auto& emitter:app.document.audio.emitters)if(!app.document.audio.sounds.count(emitter.sound))needsLegacy=true;
    if(!app.audioAuditionSound.empty()&&!app.document.audio.sounds.count(app.audioAuditionSound))needsLegacy=true;
    if(!needsLegacy){
        app.audioBankHash=app.document.audio.contentHash;
        for(const auto& item:app.document.audio.sounds)app.audioSoundHandles[item.first]=app.audio.addSound(item.second);
        app.audioSoundCount=app.audio.soundCount();return true;
    }
    const std::filesystem::path bankPath=audioResolve(app.audioBankPath.u8string());
    std::ifstream in(bankPath,std::ios::binary);
    if(!in){
        app.audioError="NG-E-AUDIO-BANK: cannot open "+bankPath.u8string();
        return false;
    }
    json bank;
    try{in>>bank;}catch(const std::exception& error){
        app.audioError=std::string("NG-E-AUDIO-BANK: ")+error.what();
        return false;
    }
    app.audioBankHash=bank.value("hash",std::string());
    if(!bank.contains("sounds")||!bank.at("sounds").is_array()){
        app.audioError="NG-E-AUDIO-BANK: missing sounds array";
        return false;
    }
    const auto bankFolder=bankPath.parent_path();
    for(const auto& entry:bank.at("sounds")){
        const std::string name=entry.value("name",std::string());
        const std::string file=entry.value("file",std::string());
        if(name.empty()||file.empty())continue;
        auto sound=std::make_shared<engine::audio::Sound>();
        std::string error;
        std::filesystem::path soundPath=audioResolve(file);
        std::error_code ec;
        if(!std::filesystem::exists(soundPath,ec)){
            const auto local=bankFolder/std::filesystem::u8path(file).filename();
            if(std::filesystem::exists(local,ec))soundPath=local;
        }
        if(!engine::audio::loadNgsnd(soundPath.u8string(),*sound,error)){
            app.audioError="NG-E-AUDIO-BANK: "+name+": "+error;
            return false;
        }
        sound->name=name;
        app.audioSoundHandles[name]=app.audio.addSound(sound);
    }
    for(const auto& item:app.document.audio.sounds)app.audioSoundHandles[item.first]=app.audio.addSound(item.second);
    app.audioSoundCount=app.audio.soundCount();
    return true;
}
// Occluders come from the scene's collider entities. The trace is a ray-AABB test
// in the mixer, so the host only has to publish boxes; it does not decide occlusion.
void audioBuildOccluders(){
    if(app.audioOff||!app.audioConfigured)return;
    app.audio.setOccluders(host::audioSceneOccluders(app.game,app.audioEyeValid?&app.audioLastEye:nullptr));
    app.audioOccluderRevision=app.document.revision;
    app.audioOccluderEye=app.audioLastEye;
    app.audioOccluderEyeValid=app.audioEyeValid;
}
engine::Vec3 audioEntityPosition(const std::string& id,bool& found){
    engine::Vec3 position;found=host::audioEntityPosition(app.game,id,{},position);return position;
}
void audioStartDevice(){
    if(!app.ready||app.audioOff||!app.audioConfigured||app.audioBackendName!="wasapi"||app.audioDeviceRunning)return;
    std::string error;
    if(app.audioDevice.start(app.audio,error)){
        app.audioDeviceRunning=true;app.audioDeviceStarted=true;
        app.audio.setCommandLead(app.audioDevice.info().bufferFrames+app.audio.config().blockFrames*2);
    }else{app.audioDeviceFailed=true;app.audioError="NG-E-AUDIO-DEVICE: "+error;}
}
bool audioPlayEmitter(const host::AudioEmitter& emitter,uint32_t handle,uint32_t priority){
    engine::Vec3 position;if(!host::audioEntityPosition(app.game,emitter.entityId,emitter.offset,position))return false;
    engine::audio::PlayParams params;params.position=position;params.spatial=emitter.spatial;
    params.bus=audioBusFromName(emitter.bus);params.gainDb=emitter.gainDb;params.pitch=emitter.pitch;
    params.loop=emitter.loop;params.overrideLoop=true;params.minDistance=emitter.minDistance;params.maxDistance=emitter.maxDistance;params.occlusion=emitter.occlusion?1.f:0.f;params.priority=priority;
    // Limit simultaneous scene voices and restart a triggered loop on retrigger.
    unsigned concurrent=0;for(auto it=app.audioEmitterVoices.begin();it!=app.audioEmitterVoices.end();) {
        if(it->second.id==emitter.id&&(emitter.loop||++concurrent>=16)){app.audio.stop(it->first,.02f);it=app.audioEmitterVoices.erase(it);}else ++it;
    }
    const auto voice=app.audio.play(handle,params);if(!voice)return false;
    float lifetime=60;const auto sound=app.document.audio.sounds.find(emitter.sound);
    if(sound!=app.document.audio.sounds.end())lifetime=float(sound->second->samples.size())/sound->second->channels/sound->second->sampleRate/emitter.pitch+.1f;
    app.audioEmitterVoices[voice]={emitter.id,emitter.entityId,emitter.offset,emitter.loop?-1.f:lifetime};++app.audioPlays;return true;
}
// Only the frame thread registers sounds, with the device consumer stopped.
void audioApplyScene(){
    if(app.audioOff||!app.audioConfigured)return;
    if(app.audioDeviceRunning){app.audioDevice.stop();app.audioDeviceRunning=false;}
    app.audio.reset();app.audioConfigured=false;app.audioSoundHandles.clear();app.audioEmitterVoices.clear();app.audioTriggers.clear();
    app.audioSampleDebt=0;app.audioEyeValid=false;app.audioError.clear();++app.audioSceneReloads;
    if(!audioConfigure()||!audioLoadBank()){app.audioOff=true;app.audioConfigured=false;return;}
    app.audioOccluderRevision=~0u;
    audioBuildOccluders();
    app.audioDoorState=app.game.doorOpen();
    app.audioCompleteState=app.game.complete();
    app.audioKeyState=app.game.hasKey();
    for(const auto& emitter:app.document.audio.emitters){
        if(!emitter.trigger.empty()&&emitter.trigger!="ambient")continue;
        if(emitter.sound.empty())continue;
        const auto handle=app.audioSoundHandles.find(emitter.sound);
        if(handle==app.audioSoundHandles.end())continue;
        if(!audioPlayEmitter(emitter,handle->second,96))++app.audioRejected;
    }
    audioStartDevice();
}
void audioTrigger(const std::string& name){
    if(app.audioOff||!app.audioConfigured||name.empty())return;
    app.audioTriggers.push_back(name);
}
// Fired triggers are matched against scene emitters by name. An emitter that never
// fires is legal data: `fail` exists in the bank so the gate can prove the routing
// works without inventing a gameplay path for it.
void audioPumpTriggers(){
    if(app.audioTriggers.empty())return;
    const auto triggers=std::move(app.audioTriggers);
    app.audioTriggers.clear();
    if(app.audioOff||!app.audioConfigured)return;
    for(const auto& trigger:triggers){
        for(const auto& emitter:app.document.audio.emitters){
            if(emitter.trigger!=trigger||emitter.sound.empty())continue;
            const auto handle=app.audioSoundHandles.find(emitter.sound);
            if(handle==app.audioSoundHandles.end()){++app.audioRejected;continue;}
            if(!audioPlayEmitter(emitter,handle->second,160)){++app.audioRejected;continue;}
            ++app.audioTriggersFired;
        }
    }
}
// The listener follows the render camera exactly, so what the player sees and what
// they hear cannot disagree about where "forward" is.
void audioUpdateListener(const engine::Vec3& eye,const engine::Vec3& lookAt,float dt){
    if(app.audioOff||!app.audioConfigured)return;
    engine::audio::Listener listener;
    listener.position=eye;
    engine::Vec3 forward{lookAt.x-eye.x,lookAt.y-eye.y,lookAt.z-eye.z};
    const float length=std::sqrt(forward.x*forward.x+forward.y*forward.y+forward.z*forward.z);
    if(length>1e-4f)forward={forward.x/length,forward.y/length,forward.z/length};
    else forward={0.0f,0.0f,1.0f};
    listener.forward=forward;
    listener.up={0.0f,1.0f,0.0f};
    if(app.audioEyeValid&&dt>1e-5f){
        listener.velocity={(eye.x-app.audioLastEye.x)/dt,(eye.y-app.audioLastEye.y)/dt,(eye.z-app.audioLastEye.z)/dt};
        // A teleport (hot reload, camera cut) would otherwise read as a supersonic
        // source and clamp the Doppler ratio for the next second.
        const float speed=std::sqrt(listener.velocity.x*listener.velocity.x+
                                    listener.velocity.y*listener.velocity.y+
                                    listener.velocity.z*listener.velocity.z);
        if(speed>40.0f)listener.velocity={};
    }
    app.audio.setListener(listener);
    app.audioLastEye=eye;
    app.audioEyeValid=true;
    for(auto it=app.audioEmitterVoices.begin();it!=app.audioEmitterVoices.end();) {
        auto& tracked=it->second;engine::Vec3 position;
        if((tracked.remaining>=0&&(tracked.remaining-=dt)<=0)||!host::audioEntityPosition(app.game,tracked.entity,tracked.offset,position)){
            app.audio.stop(it->first,.02f);it=app.audioEmitterVoices.erase(it);continue;
        }
        if(app.audio.setVoicePosition(it->first,position))++app.audioPositionUpdates;++it;
    }
    audioBuildOccluders();
}
void audioShutdown(){
    if(app.audioDeviceRunning){
        app.audioDevice.stop();
        app.audioDeviceRunning=false;
    }
}
// Offline backend: the frame loop is the clock. The sample debt is carried as a
// double so 1/60 s at 48 kHz (800 samples) never drifts by a sample per frame; the
// mixer still sees exact block boundaries because render() splits internally.
void audioPullOffline(float dt){
    if(!app.audioOffline||!app.audioConfigured||app.audioScratch.empty())return;
    const auto& config=app.audio.config();
    const uint32_t channels=engine::audio::layoutChannels(config.layout);
    const uint32_t capacity=static_cast<uint32_t>(app.audioScratch.size()/channels);
    app.audioSampleDebt+=static_cast<double>(dt)*config.sampleRate;
    if(app.audioSampleDebt<1.0)return;
    uint32_t frames=static_cast<uint32_t>(app.audioSampleDebt);
    if(frames>capacity)frames=capacity;
    app.audioSampleDebt-=frames;
    app.audio.render(app.audioScratch.data(),frames);
}
void report(bool success){
    try{
        json r={
            {"version",NATIVEGPU_VERSION},{"success",success},{"error",app.error},
            {"build",{{"commit",NATIVEGPU_BUILD_COMMIT},{"dirty",NATIVEGPU_BUILD_DIRTY!=0}}},
            {"mode",app.probe?"probe":(app.smoke?"automated-smoke":"interactive")},
            {"frames",app.frames},{"elapsedSeconds",std::chrono::duration<double>(Clock::now()-app.began).count()},
            {"scene",app.scenePath.u8string()},{"revision",app.document.revision},
            {"pondDemo",[&]{const engine::water::Diagnostics diagnostics=app.game.waterDiagnostics();return json{{"enabled",app.game.waterEnabled()},{"disturbances",app.game.waterDisturbances()},{"tick",diagnostics.tick},{"massErrorFraction",diagnostics.massErrorFraction},{"cflClamped",diagnostics.cflClamped},{"lastCflNumber",diagnostics.lastCflNumber},{"timeDebtSeconds",diagnostics.timeDebtSeconds},{"droppedSeconds",diagnostics.droppedSeconds},{"sanitizedCells",diagnostics.sanitizedCells},{"wading",app.game.wading()},{"immersionMetres",app.game.waterImmersionMetres()},{"immersionFraction",app.game.waterImmersionFraction()},{"depthMetres",app.game.waterDepthMetres()},{"maxImmersionMetres",app.game.waterMaxImmersionMetres()},{"events",static_cast<unsigned>(app.game.waterEvents().size())},{"timelineEpoch",diagnostics.timelineEpoch},{"inflowActual",diagnostics.inflowActualCubicMetres},{"outflowActual",diagnostics.outflowActualCubicMetres},{"outflowLimited",diagnostics.outflowLimitedCubicMetres},{"solidCells",diagnostics.solidCells},{"sourceCells",diagnostics.sourceCells},{"drainCells",diagnostics.drainCells},{"droppedEvents",diagnostics.droppedEvents},{"stepMilliseconds",diagnostics.lastStepMilliseconds},{"lod",app.waterLod},{"wetEntities",app.wetEntities},{"maxWetness",app.maxWetness},{"wetnessEnabled",app.surfaceWetness},{"wetDraws",app.wetDraws}};}()},
            {"waterLinks",[&]{json out=json::array();for(const auto& runtime:app.waterLinks){const auto d=runtime.link->diagnostics();out.push_back(json{{"enabled",d.enabled},{"requested",d.requestedVolumeCubicMetres},{"withdrawn",d.withdrawnVolumeCubicMetres},{"delivered",d.deliveredVolumeCubicMetres},{"inTransit",d.inTransitVolumeCubicMetres},{"shortfall",d.shortfallVolumeCubicMetres},{"lost",d.lostVolumeCubicMetres},{"returned",d.returnedVolumeCubicMetres},{"arrivals",d.arrivals},{"landingEvents",d.landingEvents},{"flow",d.lastActualFlowCubicMetresPerSecond}});}return out;}()},
            // Splash droplets are presentation only; the counters exist so a missing
            // splash can be told apart from a silent capacity refusal.
            {"splash",{{"requested",app.splash.requested},{"enabled",app.splash.enabled},{"asset",app.splash.id},
                {"libraryHash",app.splash.library.hash},{"contacts",app.splash.contacts},{"spawned",app.splash.spawned},
                {"alive",app.splash.handles.size()},{"expired",app.splash.expired},{"capacityRejected",app.splash.capacityRejected},
                {"failed",app.splash.failed},{"stepsDropped",app.splash.stepsDropped},{"runtimeTick",app.splash.gameTick},
                {"lastIntensity",app.splash.lastIntensity},{"peakIntensity",app.splash.peakIntensity}}},
            {"scorched",app.scorchedEntities},{"scorchApplied",app.scorchApplied},
            {"effectRuntime",[&]{if(app.gpuEffectRequested){const auto s=app.renderer.gpuEffectStats();return json{{"requested",!app.effectId.empty()},{"asset",app.effectId},{"libraryHash",app.effectLibrary.hash},{"simulationBackend","gpu-visual"},{"tick",s.tick},{"instances",s.instances},{"alive",s.visual.alive},{"emitted",s.visual.emitted},{"expired",s.visual.expired},{"dropped",s.visual.dropped},{"rejectedInstances",app.effectRejected},{"stopped",app.effectStopped},{"telemetryValid",s.telemetryValid},{"telemetryFence",s.telemetryFence}};}return json{{"requested",!app.effectId.empty()},{"asset",app.effectId},{"libraryHash",app.effectLibrary.hash},
                {"simulationBackend","cpu-reference"},{"tick",app.effects.stats().tick},{"instances",app.effects.stats().instances},
                {"alive",app.effects.stats().alive},{"emitted",app.effects.stats().emitted},{"expired",app.effects.stats().expired},
                {"dropped",app.effects.stats().dropped},{"rejectedInstances",app.effectRejected},{"stopped",app.effectStopped}};}()},
            {"fireLight",{{"applied",app.fireLightApplied},{"intensity",app.fireLightIntensity},{"flicker",app.fireLightFlicker}}},
            {"effectField",[&]{if(!app.effectFields)return json{{"requested",false}};const auto& source=app.effectFields->source();return json{{"requested",true},{"asset",app.effectFieldPath.u8string()},{"hash",source.asset().hash},{"domain",source.asset().domain==engine::fields::Domain::CpuAuthority?"cpu-authority":"visual"},{"tick",app.effectFields->tick()},{"sourceTick",source.world()->committedTick()},{"sourcePublications",source.publications()},{"epochs",app.effectFields->epochs()},{"leaseActive",app.effectFields->leased()},{"worldDigest",std::to_string(source.world()->digest())},{"readback",false},{"velocityQuantizationMetresPerSecond",0.001},{"gameAuthorityIntegrated",false}};}()},
            {"simulationField",app.simulationFields?app.simulationFields->report():json{{"requested",!app.simulationFieldPath.empty()}}},
            {"clothRuntime",app.clothPreview?app.clothPreview->report():json{{"requested",!app.clothOptions.asset.empty()},{"asset",app.clothOptions.asset}}},
            {"groomRuntime",[&]{const auto s=app.renderer.hairStats();return json{
                {"requested",!app.groomPath.empty()},{"path",app.groomPath.u8string()},{"configured",app.groomConfigured},
                {"debugView",app.groomDebugView},{"frames",s.frames},{"drawnStrands",s.drawnStrands},
                {"drawnVertices",s.drawnVertices},{"triangles",s.triangles},{"uploadBytes",s.uploadBytes},
                {"shadowBuilds",s.shadowBuilds},{"shadowCoveredTexels",s.shadowCoveredTexels},
                {"shadowDroppedLayers",s.shadowDroppedLayers},{"shadowMaxOpticalDepthMetres",s.shadowMaxOpticalDepthMetres},
                {"simulate",app.groomSimulate},{"wind",{app.groomWind[0],app.groomWind[1],app.groomWind[2]}},
                {"simulating",s.simulating},{"simulationTicks",s.simulationTicks},
                {"simulationTimeSeconds",s.simulationTimeSeconds},{"simulationStaleUploads",s.simulationStaleUploads},
                {"simulationMaxStretch",s.simulationMaxStretch},
                {"simulationMaxPenetrationMetres",s.simulationMaxPenetrationMetres},
                {"simulationMaxSpeedMetresPerSecond",s.simulationMaxSpeedMetresPerSecond},
                {"simulationMaxRootDriftMetres",s.simulationMaxRootDriftMetres},
                {"headAttached",app.groomHeadAttached},{"headTurnDegrees",app.groomHeadTurnDegrees},
                {"depthSorted",s.depthSorted},{"sortedStrands",s.sortedStrands},
                {"orderStaleUploads",s.orderStaleUploads},
                {"transparency",s.transparency},{"backend",s.backend}};}()},
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
            {"physics",[&]{
                const engine::physics::Stats stats=app.game.physicsStats();
                return json{
                    {"implemented",true},
                    {"backend","deterministic-sequential-impulse"},
                    {"fixedStepHz",120},
                    {"demo",app.physicsDemo},
                    {"demoShots",app.physicsDemoShots},
                    {"bodies",stats.bodies},
                    {"activeBodies",stats.activeBodies},
                    {"dynamicBodies",stats.dynamicBodies},
                    {"sleepingBodies",stats.sleepingBodies},
                    {"debris",stats.debris},
                    {"contacts",stats.contacts},
                    {"substeps",stats.substeps},
                    {"broadphase",stats.broadphaseMode==0?"brute-force":"dynamic-aabb-tree"},
                    {"broadphasePairs",stats.broadphasePairs},
                    {"broadphaseNodes",stats.broadphaseNodes},
                    {"broadphaseRebuilds",stats.broadphaseRebuilds},
                    {"joints",stats.joints},
                    {"brokenJoints",stats.brokenJoints},
                    {"totalSteps",stats.totalSteps},
                    {"averageStepMs",stats.averageStepMs},
                    {"lastStepMs",stats.lastStepMs},
                    {"maxStepMs",stats.maxStepMs},
                    {"destructibles",app.game.destructibleCount()},
                    {"intactDestructibles",app.game.intactDestructibleCount()},
                    {"fractures",app.game.fractureCount()},
                    {"bondedHoles",app.game.bondedHoles()},
                    {"bondedBrokenBonds",app.game.bondedBrokenBonds()},
                    {"bakedShards",app.game.bakedShardCount()},
                    {"convexMeshes",app.renderer.convexMeshCount()},
                    {"convexMeshBytes",app.renderer.convexMeshBytes()},
                    {"fragmentsSpawned",app.game.fragmentsSpawned()},
                    {"structuralCollapses",app.game.structuralCollapses()},
                    {"projectilesFired",app.game.projectileCount()},
                    {"explosions",app.game.explosionCount()},
                    {"impacts",app.game.impactCount()},
                    {"peakImpactImpulse",app.game.peakImpactImpulse()},
                    {"totalImpactEnergy",app.game.totalImpactEnergy()},
                    {"limitations",json::array({"collision shapes are OBBs; a baked Voronoi shard is drawn as its hull but simulated as its AABB proxy",
                                                "character collides as a swept AABB against body AABBs, not a dynamic capsule",
                                                "whole-object fracture still uses recursive box shards",
                                                "blast damage has no occlusion test"})}
                };
            }()},
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
        const auto& animation=app.game.animationController();
        json stages=json::array();
        for(const auto& stage:animation.stages())stages.push_back({{"hash",hex64(stage.hash)},{"microseconds",stage.microseconds}});
        r["animation"]={{"implemented",animation.asset()!=nullptr},{"asset",app.activeCharacterPath.u8string()},
            {"runtimeFingerprint",app.character?app.character->contentHash:""},{"bones",app.character?app.character->skeleton.bones.size():0},
            {"clips",app.character?app.character->clips.size():0},{"state",engine::animation::stateName(animation.state())},
            {"meshParts",app.character?app.character->meshParts.size():0},
            {"tickHz",120},{"clipHz",60},{"stages",stages},{"rootMotionMode",app.character&&app.character->preserveRootTranslation?"preserved in pose":"extracted, gameplay drives translation"},
            {"clipName",app.character&&animation.selection().clip<app.character->clips.size()?app.character->clips[animation.selection().clip].name:""},
            {"featuresEnabled",app.animationFeatures},
            {"motionMatching",app.animationFeatures&&!animation.motionDatabase().frames.empty()},{"footIK",app.animationFeatures&&app.character&&!app.character->rig.legs.empty()},
            {"muscles",app.animationFeatures&&app.character&&!app.character->muscles.empty()},{"softTissue",app.animationFeatures&&app.character&&!app.character->softRegions.empty()},{"facial",false},
            {"databaseFrames",animation.featureMetrics().databaseFrames},{"matchedClip",animation.selection().clip},{"matchedFrame",animation.selection().frame},{"matchCost",animation.selection().cost},
            {"secondCost",animation.featureMetrics().secondCost},{"lockedFeet",animation.featureMetrics().lockedFeet},{"footSlipMps",animation.featureMetrics().footSlip},
            {"footPenetrationM",animation.featureMetrics().penetration},{"ikErrorM",animation.featureMetrics().ikError},{"muscleWeight",animation.featureMetrics().muscleWeight},
            {"softPoints",animation.featureMetrics().softPoints},{"softOffsetM",animation.featureMetrics().softMaxOffset},{"softVolumeError",animation.featureMetrics().softVolumeError},
            {"softPenetrationM",animation.featureMetrics().softPenetration},{"softLodWeight",animation.featureState().lodWeight},
            {"footLock",animation.rigSettings().footLock},{"turnInPlace",animation.rigSettings().turnInPlace},
            {"rigSource",app.character?app.character->rig.source:""},{"rigConfidence",app.character?app.character->rig.confidence:0},
            {"rigWarnings",app.character?app.character->rig.warnings:std::vector<std::string>()},
            {"groundQueries",animation.featureMetrics().groundQueries},{"swingCount",animation.featureMetrics().swingCount},
            {"turnSteps",animation.featureMetrics().turnSteps},{"turnStepsTotal",animation.featureState().turnSteps},
            {"ikClamped",animation.featureMetrics().ikClamped},{"turnResidualRad",animation.featureMetrics().turnResidual},
            {"anchorErrorM",animation.featureMetrics().anchorError},{"contactSlipMps",animation.featureMetrics().contactSlip},
            {"pivotSlipMps",animation.featureMetrics().pivotSlip},{"soleAngleRad",animation.featureMetrics().soleAngle},
            {"supportSwitchJumpM",animation.featureMetrics().switchJump},{"supportSwitches",animation.featureMetrics().supportSwitches},
            {"toeOffs",animation.featureMetrics().toeOffs},{"heelStrikes",animation.featureMetrics().heelStrikes},
            {"pelvisShiftM",animation.featureMetrics().pelvisShiftUsed},{"maxPelvisShift",animation.rigSettings().maxPelvisShift},
            {"soleRoll",animation.rigSettings().soleRoll},{"rollLimited",animation.featureMetrics().rollLimited},
            {"activeContacts",animation.featureMetrics().activeContacts},{"contactErrorM",animation.featureMetrics().contactError},
            {"releases",[&]{json counts=json::object();const auto& r=animation.featureMetrics().releaseCounts;for(unsigned i=0;i<r.size();++i)if(r[i])counts[engine::animation::contactReleaseName(static_cast<engine::animation::ContactRelease>(i))]=r[i];return counts;}()},
            {"steppingFoot",animation.featureState().steppingFoot},{"fallback",engine::animation::rigFallbackName(animation.featureState().fallback)},
            {"notifyEvents",app.animationEventCount}};
        r["animation"]["contacts"]=json::array();
        for(const auto& c:animation.contacts())r["animation"]["contacts"].push_back({
            {"id",c.id},{"chain",c.chain},{"active",c.active},{"bound",c.bound},
            {"anchor",{c.anchor.x,c.anchor.y,c.anchor.z}},
            {"position",{c.pose.x,c.pose.y,c.pose.z}},
            {"weight",c.weight},{"priority",c.priority},{"errorM",c.error},
            {"release",engine::animation::contactReleaseName(c.release)},
            {"validUntil",c.validUntil}});
        r["animation"]["feet"]=json::array();
        for(const auto& foot:animation.featureState().feet)r["animation"]["feet"].push_back({
            {"phase",engine::animation::footPhaseName(foot.phase)},{"locked",foot.locked},{"contact",foot.contact},
            {"mode",engine::animation::contactModeName(foot.mode)},{"release",engine::animation::contactReleaseName(foot.release)},
            {"roll",foot.roll},{"surface",foot.surface},{"weight",foot.weight},{"turnStep",foot.turnStep},{"cooldown",foot.cooldown},
            {"anchor",{foot.anchor.x,foot.anchor.y,foot.anchor.z}},{"target",{foot.target.x,foot.target.y,foot.target.z}},
            {"pivot",{foot.pivot.x,foot.pivot.y,foot.pivot.z}},
            {"normal",{foot.normal.x,foot.normal.y,foot.normal.z}},{"worldPenetrationM",animation.featureMetrics().penetration}});
        r["animation"]["decisions"]=json::array();
        for(const auto& decision:animation.decisions())r["animation"]["decisions"].push_back({{"tick",decision.tick},{"clip",decision.chosen.clip},{"frame",decision.chosen.frame},{"cost",decision.chosen.cost},{"secondCost",decision.secondCost}});
        r["animation"]["instances"]=json::array();
        r["characterController"]={{"shape","swept-aabb"},{"height",app.game.playerHeight()},{"radius",app.game.playerRadius()},{"stepHeight",app.game.playerStepHeight()},
            {"eyeHeight",app.game.cameraEye().y-app.game.playerPosition().y},{"orbitTargetHeight",app.game.orbitCameraTarget().y-app.game.playerPosition().y}};
        for(const auto& character:app.game.renderCharacters()) {
            engine::determinism::Hasher poseHash;for(const auto& matrix:character.palette)for(const auto value:matrix)poseHash.quantized(value);
            for(const auto value:character.morphWeights)poseHash.quantized(value);
            r["animation"]["instances"].push_back({{"entity",character.entityId},{"clip",character.clip},{"asset",character.asset->contentHash},
                {"geometry",character.asset->geometryHash},{"poseHash",poseHash.hex()},{"morphWeights",character.morphWeights},
                {"materials",character.materials},{"position",{character.position.x,character.position.y,character.position.z}}});
        }
        // Geometry routing contract (.developmentAgent/rendering/GEOMETRY-LAYOUT.md ?3): every entity is
        // classified so a report proves which raster path the content actually needs.
        json geometryCounts=json::object();
        for(const char* name:{"cluster","simple","deformable","transparent","effect"}){
            geometryCounts[name]=0;
        }
        const unsigned unitCubeTriangles=12;
        for(const auto& entity:app.document.scene.entities){
            engine::GeometryHints hints;
            hints.triangles=unitCubeTriangles;
            hints.animated=(entity.kind=="player");
            const char* className=engine::geometryClassName(engine::classifyGeometry(hints));
            geometryCounts[className]=geometryCounts[className].get<unsigned>()+1u;
        }
        r["geometryClasses"]={
            {"thresholdTriangles",engine::kClusterTriangleThreshold},
            {"unitCubeTriangles",unitCubeTriangles},
            {"counts",geometryCounts},
            {"note","current content is unit cubes; the cluster path has no work yet"}
        };
        std::uint32_t defaultHandle=engine::kMaterialHandleDefault;
        engine::encodeMaterialHandle(0,0,defaultHandle);
        r["materialHandle"]={
            {"default",defaultHandle},
            {"sliceBits",engine::kMaterialSliceBits},
            {"encoding","generation<<16 | slice"},
            {"gpuDecodesSliceOnly",true}
        };
        r["materialHandle"]["count"]=app.renderer.materialCount();
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
        r["gameplay"]=app.game.gameplayRuntime().inspect();
        // Audio evidence: the mixer's own numbers, not the host's intentions. `pcmHash`
        // is what a determinism gate compares; `underruns` is what a device gate reads.
        {
            json audio{
                {"enabled",!app.audioOff},
                {"configured",app.audioConfigured},
                {"backend",app.audioBackendName},
                {"quality",app.audioConfigured?engine::audio::qualityName(app.audio.config().quality):app.audioQualityName.c_str()},
                {"layout",app.audioConfigured?engine::audio::layoutName(app.audio.config().layout):app.audioLayoutName.c_str()},
                {"requested",{{"quality",app.audioQualityName},{"layout",app.audioLayoutName}}},
                // `started` is the session fact; the device is stopped before the report
                // is written, so a live `running` flag would always read false here.
                {"device",{
                    {"started",app.audioDeviceStarted},
                    {"failed",app.audioDeviceFailed},
                    {"name",app.audioDevice.info().name},
                    {"sampleRate",app.audioDevice.info().sampleRate},
                    {"channels",app.audioDevice.info().channels},
                    {"bufferFrames",app.audioDevice.info().bufferFrames},
                    {"floatFormat",app.audioDevice.info().floatFormat}}},
                {"bank",{{"path",app.audioBankPath.u8string()},
                         {"hash",app.audioBankHash},
                         {"sounds",app.audioSoundCount}}},
                {"emitters",static_cast<unsigned>(app.document.audio.emitters.size())},
                {"sceneSounds",app.document.audio.sounds.size()},{"contentHash",app.document.audio.contentHash},
                {"positionUpdates",app.audioPositionUpdates},{"sceneReloads",app.audioSceneReloads},
                {"masterGainDb",app.document.audio.masterGainDb},
                {"loudnessTargetLufs",app.document.audio.loudnessTargetLufs},
                {"plays",app.audioPlays},
                {"triggersFired",app.audioTriggersFired},
                {"audition",app.audioAuditionSound},
                {"rejected",app.audioRejected},
                {"error",app.audioError}};
            if(app.audioConfigured&&!app.audioOff){
                const auto stats=app.audio.stats();
                char hash[32];
                std::snprintf(hash,sizeof(hash),"%016llx",static_cast<unsigned long long>(stats.pcmHash));
                audio["mixer"]={
                    {"physical",stats.physical},
                    {"virtual",stats.virtualVoices},
                    {"objects",stats.objects},
                    {"started",stats.started},
                    {"steals",stats.steals},
                    {"blocks",stats.blocks},
                    {"frames",stats.frames},
                    {"underruns",stats.underruns},
                    {"lateCommands",stats.lateCommands},
                    {"peak",stats.peak},
                    {"peakMax",stats.peakMax},
                    {"rms",stats.rms},
                    {"lufsShort",stats.lufsShort},
                    {"lufsShortMax",stats.lufsShortMax},
                    {"lufsIntegrated",stats.lufsIntegrated},
                    {"cpuMs",stats.cpuMs},
                    {"pcmHash",hash}};
                json receipts=json::array();
                for(const auto& receipt:app.audio.receipts()){
                    receipts.push_back(json{
                        {"id",receipt.id},{"sound",receipt.sound},
                        {"bus",engine::audio::busName(receipt.bus)},
                        {"physical",receipt.physical},{"occluded",receipt.occluded},
                        {"object",receipt.object},{"priority",receipt.priority},
                        {"distance",receipt.distance},{"gainDb",receipt.gainDb},
                        {"cutoffHz",receipt.cutoffHz}});
                }
                audio["voices"]=receipts;
                json objects=json::array();
                for(const auto& object:app.audio.objects()){
                    objects.push_back(json{
                        {"x",object.position.x},{"y",object.position.y},{"z",object.position.z},
                        {"gainDb",object.gainDb},{"spread",object.spread},
                        {"priority",object.priority},{"snap",object.snap},
                        {"binaural",object.binaural}});
                }
                audio["objectAudio"]=objects;
            }
            r["audio"]=audio;
        }
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
#ifndef NATIVEGPU_SCENE_LIBRARY
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show){
    SetProcessDPIAware();
    int argc=0;
    auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    bool explicitScene=false,watchExplicit=false;
    for(int i=1;i<argc;++i){
        std::wstring arg=argv[i];
        if(arg==L"--smoke")app.smoke=true;
        else if(arg==L"--physics-demo"){app.physicsDemo=true;app.smoke=true;app.noStress=true;}
        else if(arg==L"--animation-demo"){app.animationDemo=true;app.smoke=true;}
        else if(arg==L"--animation-features"&&i+1<argc)app.animationFeatures=std::wstring(argv[++i])!=L"off";
        else if(arg==L"--probe"){app.probe=true;app.smoke=true;}
        else if(arg==L"--scene"&&i+1<argc){app.scenePath=argv[++i];explicitScene=true;}
        else if(arg==L"--textures"&&i+1<argc){app.texturePath=argv[++i];}
        else if(arg==L"--character"&&i+1<argc)app.characterPath=argv[++i];
        else if(arg==L"--asset"&&i+1<argc)app.importedAssetPath=argv[++i];
        else if(arg==L"--report"&&i+1<argc)app.reportPath=argv[++i];
        else if(arg==L"--progress-report"&&i+1<argc)app.progressPath=narrow(argv[++i]);
        else if(arg==L"--frames"&&i+1<argc)app.targetFrames=std::max(240,_wtoi(argv[++i]));
        else if(arg==L"--watch"){app.watch=true;watchExplicit=true;}
        else if(arg==L"--no-watch"){app.watch=false;watchExplicit=true;}
        else if(arg==L"--validation")app.validationRequested=true;
        else if(arg==L"--water"&&i+1<argc){app.waterEnabled=std::wstring(argv[++i])!=L"off";}
        else if(arg==L"--wetness"&&i+1<argc){app.surfaceWetness=std::wstring(argv[++i])!=L"off";}
        else if(arg==L"--water-debug"&&i+1<argc){app.waterDebug=static_cast<unsigned>(std::clamp(_wtoi(argv[++i]),0,7));}
        else if(arg==L"--water-lod"&&i+1<argc){app.waterLod=static_cast<unsigned>(std::clamp(_wtoi(argv[++i]),0,3));}
        else if(arg==L"--splash"&&i+1<argc){const std::wstring value=argv[++i];app.splash.requested=value!=L"off";if(app.splash.requested)app.splash.id=narrow(value.c_str());}
        else if(arg==L"--splash-library"&&i+1<argc)app.splash.libraryPath=argv[++i];
        else if(arg==L"--splash-asset-root"&&i+1<argc)app.splash.assetRoot=argv[++i];
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
        else if(arg==L"--tour"&&i+1<argc){app.tour=true;app.tourDirectory=narrow(argv[++i]);}
        else if(arg==L"--save"&&i+1<argc)app.savePath=narrow(argv[++i]);
        else if(arg==L"--load-save"&&i+1<argc)app.loadSavePath=narrow(argv[++i]);
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
        else if(arg==L"--cloth"&&i+1<argc)app.clothOptions.asset=narrow(argv[++i]);
        else if(arg==L"--cloth-execution"&&i+1<argc){const std::wstring mode=argv[++i];if(mode==L"auto")app.clothOptions.execution=engine::cloth::GpuExecution::Automatic;else if(mode==L"dispatch")app.clothOptions.execution=engine::cloth::GpuExecution::Dispatch;else if(mode==L"grouped")app.clothOptions.execution=engine::cloth::GpuExecution::Grouped;else app.error="Cloth execution must be auto, dispatch or grouped";}
        else if(arg==L"--cloth-library"&&i+1<argc)app.clothOptions.library=argv[++i];
        else if(arg==L"--groom"&&i+1<argc)app.groomPath=argv[++i];
        else if(arg==L"--groom-debug"&&i+1<argc){nativegpu::HairDebugView view;if(!nativegpu::parseHairDebugView(narrow(argv[++i]),view))app.error="Groom debug view must be shaded, coverage, tangent, roottotip, strandid, material, depth or shadow";else app.groomDebugView=static_cast<int>(view);}
        else if(arg==L"--groom-shadow"&&i+1<argc){const wchar_t* text=argv[++i];wchar_t* end=nullptr;const float value=std::wcstof(text,&end);if(text==end||*end||!(value>=0&&value<=1))app.error="Groom shadow strength must be in [0,1]";else app.groomShadowStrength=value;}
        else if(arg==L"--groom-sim"&&i+1<argc){const std::wstring mode=argv[++i];if(mode==L"on"||mode==L"1")app.groomSimulate=true;else if(mode==L"off"||mode==L"0")app.groomSimulate=false;else app.error="Groom simulation must be on or off";}
        else if(arg==L"--groom-wind"&&i+1<argc){float values[3]{};if(parseFloatList(argv[++i],values,3)==3)for(int axis=0;axis<3;++axis)app.groomWind[axis]=values[axis];else app.error="Groom wind requires exactly x,y,z";}
        else if(arg==L"--groom-head-turn"&&i+1<argc){const wchar_t* text=argv[++i];wchar_t* end=nullptr;const float value=std::wcstof(text,&end);if(text==end||*end||!(value>=-90&&value<=90))app.error="Groom head turn must be in [-90,90] degrees";else app.groomHeadTurnDegrees=value;}
        else if(arg==L"--cloth-count"&&i+1<argc){const wchar_t* text=argv[++i];wchar_t* end=nullptr;const auto count=std::wcstoul(text,&end,10);if(text==end||*end||*text==L'-'||count<1||count>16)app.error="Cloth count must be an integer in [1,16]";else app.clothOptions.count=static_cast<unsigned>(count);}
        else if(arg==L"--cloth-debug"&&i+1<argc){const wchar_t* text=argv[++i];wchar_t* end=nullptr;const auto mode=std::wcstoul(text,&end,10);if(text==end||*end||*text==L'-'||mode>3)app.error="Cloth debug mode must be an integer in [0,3]";else app.clothOptions.debugMode=static_cast<unsigned>(mode);}
        else if((arg==L"--cloth-origin"||arg==L"--cloth-wind"||arg==L"--cloth-teleport-origin")&&i+1<argc){auto& value=arg==L"--cloth-origin"?app.clothOptions.origin:arg==L"--cloth-wind"?app.clothOptions.wind:app.clothOptions.teleportOrigin;const wchar_t* text=argv[++i];int consumed=0;if(std::swscanf(text,L"%f,%f,%f%n",&value.x,&value.y,&value.z,&consumed)!=3||text[consumed])app.error="Cloth vector requires exactly x,y,z";}
        else if((arg==L"--cloth-pause-tick"||arg==L"--cloth-resume-tick"||arg==L"--cloth-teleport-tick"||arg==L"--cloth-reset-tick")&&i+1<argc){
            const wchar_t* text=argv[++i];wchar_t* end=nullptr;const auto tick=_wcstoui64(text,&end,10);if(text==end||*end||*text==L'-'||tick>10000000)app.error="Cloth event tick must be an integer in [0,10000000]";
            else if(arg==L"--cloth-pause-tick")app.clothOptions.pauseTick=tick;else if(arg==L"--cloth-resume-tick")app.clothOptions.resumeTick=tick;else if(arg==L"--cloth-teleport-tick")app.clothOptions.teleportTick=tick;else app.clothOptions.resetTick=tick;
        }
        else if(arg==L"--effect"&&i+1<argc)app.effectId=narrow(argv[++i]);
        else if(arg==L"--effect-backend"&&i+1<argc){const std::wstring backend=argv[++i];if(backend==L"gpu")app.gpuEffectRequested=true;else if(backend==L"cpu")app.gpuEffectRequested=false;else app.error="Effect backend must be cpu or gpu";}
        else if(arg==L"--effect-library"&&i+1<argc)app.effectLibraryPath=argv[++i];
        else if(arg==L"--simulation-field"&&i+1<argc)app.simulationFieldPath=argv[++i];
        else if(arg==L"--effect-field"&&i+1<argc)app.effectFieldPath=argv[++i];
        else if(arg==L"--effect-count"&&i+1<argc){const int count=_wtoi(argv[++i]);if(count<1||count>128){app.error="Effect count must be in [1,128]";}else app.effectCount=static_cast<unsigned>(count);}
        else if(arg==L"--effect-repeat-ticks"&&i+1<argc)app.effectRepeatTicks=static_cast<unsigned>(std::max(0,_wtoi(argv[++i])));
        else if(arg==L"--effect-stop-tick"&&i+1<argc)app.effectStopTick=static_cast<unsigned>(std::max(0,_wtoi(argv[++i])));
        else if(arg==L"--effect-origin"&&i+1<argc){if(std::swscanf(argv[++i],L"%f,%f,%f",&app.effectOrigin.x,&app.effectOrigin.y,&app.effectOrigin.z)!=3)app.error="Effect origin requires x,y,z";}
        else if(arg==L"--particles"&&i+1<argc){
            app.particlesRequested=(std::wstring(argv[++i])!=L"off");
        }
        else if(arg==L"--particle-colliders"&&i+1<argc){
            app.particleColliders=(std::wstring(argv[++i])!=L"off");
        }
        else if(arg==L"--particle-temporal"&&i+1<argc){
            app.particleTemporal=(std::wstring(argv[++i])!=L"off");
        }
        else if(arg==L"--particle-emitter"&&i+1<argc){
            float x=0.0f,y=0.0f,z=0.0f;
            if(swscanf_s(argv[++i],L"%f,%f,%f",&x,&y,&z)==3){
                app.particleEmitter[0]=x;app.particleEmitter[1]=y;app.particleEmitter[2]=z;
                app.particleEmitterSet=true;
            }
        }
        else if(arg==L"--fire")app.firePreset=true;
        else if(arg==L"--shot"&&i+1<argc){
            // frame:x,y,z. Repeatable; shots fire in frame order.
            const std::wstring spec=argv[++i];
            unsigned frame=0;float x=0.0f,y=0.0f,z=0.0f;
            if(std::swscanf(spec.c_str(),L"%u:%f,%f,%f",&frame,&x,&y,&z)!=4||frame==0){
                app.error="--shot requires frame:x,y,z with frame >= 1";
            } else {
                app.demoShots.push_back({frame,{x,y,z}});
                app.smoke=true;
            }
        }
        else if(arg==L"--explode-at"&&i+1<argc){
            app.explodeAt=static_cast<unsigned>(std::max(0,_wtoi(argv[++i])));
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
        else if(arg==L"--layer-debug"&&i+1<argc){const std::wstring value=argv[++i];const std::string spec(value.begin(),value.end());if(!parseLayerDebugSpec(spec,app.layerDebugMode,app.layerDebugLayer)){std::fprintf(stderr,"unknown --layer-debug value: %s\n",spec.c_str());return 6;}}
        // `--cull off` is the before/after control for the frustum pass: it restores the
        // pre-culling submission pattern so the two runs differ in exactly one variable.
        else if(arg==L"--cull"&&i+1<argc){app.cullRequested=(std::wstring(argv[++i])!=L"off");}
        // `--audio off` is the regression path: nothing is configured, no thread is
        // started and the frame path is byte-identical to a build without audio.
        else if(arg==L"--audio"&&i+1<argc){
            const std::wstring mode=argv[++i];
            app.audioOff=(mode==L"off");
            app.audioBackendName=app.audioOff?"off":(mode==L"offline"?std::string("offline"):std::string("wasapi"));
        }
        else if(arg==L"--audio-quality"&&i+1<argc)app.audioQualityName=narrow(argv[++i]);
        else if(arg==L"--audio-layout"&&i+1<argc)app.audioLayoutName=narrow(argv[++i]);
        else if(arg==L"--audio-bank"&&i+1<argc)app.audioBankPath=argv[++i];
        else if(arg==L"--audio-demo"){app.audioDemo=true;app.smoke=true;app.noStress=true;}
        else if(arg==L"--audio-audition"&&i+1<argc){
            app.audioAuditionSound=narrow(argv[++i]);
            app.smoke=true;app.noStress=true;
        }
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
            else if(view==L"fog")app.debugView=5;
            else if(view==L"cloud")app.debugView=6;
            else if(view==L"cloud-shadow")app.debugView=7;
            else if(view==L"scatter")app.debugView=9;
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
        else if(arg==L"--camera-cut"&&i+1<argc){
            float values[7]{};
            if(parseFloatList(argv[++i],values,7)==7&&values[0]>=1&&values[0]<=1000000){
                app.cameraCutFrame=static_cast<unsigned>(values[0]);
                for(int axis=0;axis<3;axis++){app.cameraCutEye[axis]=values[axis+1];app.cameraCutTarget[axis]=values[axis+4];}
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
    if(app.physicsDemo&&!app.hasCameraOverride){
        // Stable evidence framing: both the intact and the post-destruction runs use
        // this camera, so image differences come from simulation alone.
        app.hasCameraOverride=true;
        app.cameraEye[0]=8.0f;app.cameraEye[1]=4.5f;app.cameraEye[2]=10.5f;
        app.cameraTarget[0]=0.0f;app.cameraTarget[1]=1.4f;app.cameraTarget[2]=-2.0f;
    }
    // A screenshot needs a rendered frame to read back from.
    if(app.screenshotRequested)app.targetFrames=std::max(app.targetFrames,app.screenshotFrame+2);
    LocalFree(argv);
    if(!app.error.empty()){app.errorCode="NG-E-EFFECT-ARGUMENT";report(false);return 9;}
    if(!app.importedAssetPath.empty()) {
        try {
            const auto folder=std::filesystem::canonical(app.importedAssetPath);
            std::ifstream input(folder/"import.json",std::ios::binary);const auto imported=json::parse(input);
            if(imported.at("schema")!="nativegpu.import/1" || (imported.at("kind")!="model"&&imported.at("kind")!="character"&&imported.at("kind")!="audio"&&imported.at("kind")!="scene"&&imported.at("kind")!="texture"&&imported.at("kind")!="environment"))throw std::runtime_error("Expected an imported model, character, texture, environment, audio or scene package");
            if(imported.at("identity").value("version",0)!=2)throw std::runtime_error("Reimport this asset to generate a version 2 content identity");
            if(imported.at("identity").value("version",1)>=2) {
                const auto identityText=imported.at("identityJson").get<std::string>();
                if(engine::assets::sha256(identityText)!=imported.at("id")||json::parse(identityText)!=imported.at("identity"))throw std::runtime_error("Import identity changed");
                const auto& identity=imported.at("identity");
                for(const auto& value:identity.at("payload").items())
                    if(value.value()!=imported.at(value.key()=="schema"?"assetSchema":value.key()))throw std::runtime_error("Import metadata changed");
                json stableFiles=json::array();unsigned previews=0;
                for(const auto& file:imported.at("files")){if(file.at("file")=="preview.scene.json")++previews;else stableFiles.push_back(file);}
                if(stableFiles!=identity.at("files")||previews!=1)throw std::runtime_error("Import file manifest changed");
                std::ifstream previewInput(folder/"preview.scene.json",std::ios::binary);auto preview=json::parse(previewInput);
                if(imported.at("kind")=="scene"){if(!imported.value("preservePreviewIds",false)||preview!=identity.at("previewScene"))throw std::runtime_error("Scene preview content changed");}
                else {
                auto uuid=[&](const std::string& key){const auto h=engine::assets::sha256(imported.at("id").get<std::string>()+":"+key);return h.substr(0,8)+"-"+h.substr(8,4)+"-4"+h.substr(13,3)+"-8"+h.substr(17,3)+"-"+h.substr(20,12);};
                if(preview.at("projectId")!=uuid("project"))throw std::runtime_error("Import preview project changed");
                preview["projectId"]=nullptr;unsigned n=0;
                if(preview.contains("audio"))for(auto& emitter:preview["audio"]["emitters"]){int index=0;bool found=false;for(const auto& entity:preview["entities"]){if(entity["id"]==emitter["entity"]){emitter["entity"]="entity:"+std::to_string(index);found=true;break;}++index;}if(!found)throw std::runtime_error("Invalid preview audio entity");}
                for(auto& entity:preview.at("entities")){if(entity.at("id")!=uuid(std::to_string(n++)))throw std::runtime_error("Import preview entity changed");entity["id"]=nullptr;}
                if(preview!=identity.at("previewScene"))throw std::runtime_error("Import preview content changed");
                }
            }
            std::set<std::string> packageFiles;
            for(const auto& entry:imported.at("files")) {
                if(!packageFiles.insert(entry.at("file").get<std::string>()).second)throw std::runtime_error("Duplicate imported dependency");
                const auto relative=std::filesystem::u8path(entry.at("file").get<std::string>());
                if(relative.is_absolute())throw std::runtime_error("Absolute asset dependency path");
                for(const auto& part:relative)if(part=="..")throw std::runtime_error("Asset dependency escapes package");
                const auto file=std::filesystem::canonical(folder/relative),resolved=file.lexically_relative(folder);
                if(resolved.empty()||*resolved.begin()==".."||std::filesystem::file_size(file)>256*1024*1024)throw std::runtime_error("Invalid imported dependency");
                std::ifstream stream(file,std::ios::binary);const std::string bytes((std::istreambuf_iterator<char>(stream)),std::istreambuf_iterator<char>());
                if(bytes.size()!=entry.at("bytes").get<std::size_t>()||engine::assets::sha256(bytes)!=entry.at("sha256"))throw std::runtime_error("Imported asset changed: "+relative.u8string());
            }
            if(imported.at("kind")=="character") {
                for(const auto* required:{"character/character.json","character/animation.bin"})if(!packageFiles.count(required))throw std::runtime_error("Character dependency is missing from import identity");
                if(imported.contains("previewCharacter")&&!packageFiles.count(imported.at("previewCharacter").get<std::string>()))throw std::runtime_error("Character preview is missing from import identity");
            }
            if(imported.at("kind")=="scene"&&(!packageFiles.count("scene.scene.json")||imported.at("scene")!="scene.scene.json"))throw std::runtime_error("Scene dependency is missing from import identity");
            if(imported.at("kind")=="environment"){
                if(imported.at("environment")!="environment/sky.ngenv"||!packageFiles.count("environment/sky.ngenv"))throw std::runtime_error("Environment dependency is missing from import identity");
                std::ifstream previewFile(folder/"preview.scene.json",std::ios::binary);const auto preview=json::parse(previewFile);const auto& ref=preview.at("lighting").at("environment");if(ref.at("file")!=imported.at("environment")||ref.at("sha256")!=imported.at("environmentSha256"))throw std::runtime_error("Environment preview differs from payload");
            }
            if(imported.at("kind")=="scene"){std::ifstream sceneInput(folder/"scene.scene.json",std::ios::binary);if(json::parse(sceneInput)!=imported.at("identity").at("previewScene"))throw std::runtime_error("Scene entry differs from preview identity");}
            app.scenePath=folder/"preview.scene.json";explicitScene=true;app.texturePath=folder/"textures";
            if(imported.at("kind")=="character") {
                if(imported.contains("previewCharacter")&&imported.at("previewCharacter")!="character/preview.json")throw std::runtime_error("Invalid imported character preview path");
                app.characterPath=folder/(imported.contains("previewCharacter")?"character/preview.json":"character/character.json");
            }
            if(!app.hasCameraOverride&&imported.at("kind")!="scene") {
                app.hasCameraOverride=true;float center[3]{0,1,0},radius=3;
                if(imported.at("kind")=="character")center[2]=5;
                if(imported.at("kind")=="model"&&imported.contains("parts")) {
                    float low[3]{1.e9f,1.e9f,1.e9f},high[3]{-1.e9f,-1.e9f,-1.e9f};
                    for(const auto& part:imported.at("parts"))for(int k=0;k<3;++k) {
                        const float p=part.at("position")[k],s=part.at("scale")[k];low[k]=std::min(low[k],p-s*.5f);high[k]=std::max(high[k],p+s*.5f);
                    }
                    radius=1;for(int k=0;k<3;++k){center[k]=(low[k]+high[k])*.5f;radius=std::max(radius,high[k]-low[k]);}
                }
                for(int k=0;k<3;++k)app.cameraTarget[k]=center[k];
                app.cameraEye[0]=center[0]+radius;app.cameraEye[1]=center[1]+radius*.65f;app.cameraEye[2]=center[2]+radius*1.5f;
            }
        }catch(const std::exception& e){app.errorCode="NG-E-ASSET-LOAD";app.error=e.what();report(false);return 10;}
    }
    if(app.characterPath==std::filesystem::path("build/animation/mannequin/character.json")&&!std::filesystem::exists(app.characterPath)){
        wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);
        app.characterPath=std::filesystem::path(exe).parent_path()/"animation/mannequin/character.json";
    }
    if(!explicitScene && !std::filesystem::exists(app.scenePath)){
        wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);
        auto folder=std::filesystem::path(exe).parent_path();
        auto candidate=folder/"assets/courtyard.scene.json";
        if(!std::filesystem::exists(candidate))candidate=folder/".screen/courtyard.scene.json";
        if(!std::filesystem::exists(candidate))candidate=folder.parent_path()/".screen/courtyard.scene.json";
        app.scenePath=candidate;
        app.reportPath=folder/"reports/native-session.json";
    }
    if(!reloadScene(app.scenePath)){report(false);return 2;}
    {
        if(!std::filesystem::exists(app.texturePath)){
            wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);
            auto folder=std::filesystem::path(exe).parent_path();
            for(const auto& candidate:{folder/"assets/textures",folder.parent_path()/"assets/textures",folder/"textures"}){
                if(std::filesystem::exists(candidate)){app.texturePath=candidate;break;}
            }
        }
        if(!configureRendererMaterials()){report(false);return 8;}
        applySceneEnvironment();
    }
    // Audio comes up after the scene so the mixer can adopt its gain/loudness targets,
    // and it is never allowed to abort the run: a missing endpoint or a broken bank is
    // reported and the frame loop keeps rendering (.developmentAgent/animation-audio/AUDIO-PLAN.md §7).
    if(!app.audioOff){
        if(!audioConfigure()){
            app.audioOff=true;
            app.audioConfigured=false;
        }
    }
    // The frame path talks to this device, not to Renderer directly, so a second backend
    // only has to implement nativegpu::rhi::Device (.developmentAgent/rendering/RHI.md).
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
    app.game.reset();
    if(app.physicsDemo&&!app.hashLogPath.empty()) {
        app.errorCode="NG-E-REPLAY-MODE";app.error="Physics demo uses unrecorded scripted actions; record player inputs instead";report(false);return 6;
    }
    const auto gameplayContent=engine::gameplay::fingerprint(json{{"gameplay",app.game.gameplayRuntime().manifest()},{"audio",app.document.audio.contentHash},
        {"scene",engine::gameplay::sceneIdentity(app.document.scene)},{"animation",app.character->contentHash},
        {"simulation",4},{"build",NATIVEGPU_BUILD_FINGERPRINT},{"version",NATIVEGPU_VERSION}});
    try {
        if(app.replayEnabled&&app.replayV4) {
            engine::gameplay::check(app.replayContent==gameplayContent,"NG-E-REPLAY-CONTENT","Replay scene/scripts/build changed");
            app.game.load(app.replayPath+".initial.json");
        } else if(!app.loadSavePath.empty())app.game.load(app.loadSavePath);
        if(!app.hashLogPath.empty())app.game.save(app.hashLogPath+".initial.json");
    }catch(const std::exception& e){app.errorCode="NG-E-REPLAY-CONTENT";app.error=e.what();report(false);return 6;}
    app.game.gameplayRuntime().lockContent(app.replayEnabled||!app.hashLogPath.empty());
    if(app.replayEnabled||!app.hashLogPath.empty())app.watch=false;
    // Loops and the acoustic environment are re-adopted now that the mixer exists and
    // the gameplay state is final: the trigger edges must be baselined against the
    // state the run actually starts from, not the freshly loaded scene file.
    if(!initializeSimulationFields() || !initializeEffects()){report(false);return 9;}
    if(!initializeCloth()){report(false);return 9;}
    audioApplyScene();

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
        app.hashLog<<"# nativegpu-replay v4 seed="<<app.seed<<" step=1/120 mode="
                   <<(app.recordEnabled?"record":"hash")<<" content="<<gameplayContent<<" character="<<app.character->contentHash<<"\n";
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
    app.viewport=CreateWindowExW(0,L"NativeGPUViewport",L"NativeGPU 3D viewport",WS_CHILD|WS_VISIBLE,0,0,960,720,app.window,nullptr,instance,nullptr);
    app.title=label(L"",980,20,270,60);SendMessageW(app.title,WM_SETFONT,reinterpret_cast<WPARAM>(app.titleFont),TRUE);
    app.mission=label(L"",980,95,270,95);
    app.stats=label(L"",980,206,270,76);
    auto sceneLabel=label(L"场景实体  /  实时同步",980,296,270,22);SetWindowLongPtrW(sceneLabel,GWLP_ID,201);
    app.list=CreateWindowExW(WS_EX_CLIENTEDGE,L"LISTBOX",L"场景实体",WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOINTEGRALHEIGHT,980,318,270,240,app.window,nullptr,instance,nullptr);
    SendMessageW(app.list,WM_SETFONT,reinterpret_cast<WPARAM>(app.font),TRUE);
    auto help=label(L"WASD 移动 · Shift 跑 · 空格 跳 · E 交互 · F 投掷 · G 爆破 · 右键拖动 / 方向键 转动镜头",16,740,900,22);
    SetWindowLongPtrW(help,GWLP_ID,202);
    app.pauseButton=button(L"暂停运行",ID_PAUSE,980,600);
    button(L"重置玩法",ID_RESET,1120,600);
    button(L"重载场景",ID_RELOAD,980,644);
    button(L"打开场景",ID_OPEN,1120,644);
    button(L"重建渲染器  /  F8",ID_RESTART,980,688,268);
    RAWINPUTDEVICE mouse{0x01,0x02,0,app.window};RegisterRawInputDevices(&mouse,1,sizeof(mouse));
    refreshList();layout();
    nativegpu::rhi::Descriptor descriptor;
    descriptor.window=app.viewport;
    descriptor.width=app.width;
    descriptor.height=app.height;
    descriptor.validation=app.validationRequested;
    app.rhi->setParticles(app.particlesRequested);
    app.rhi->setFireEffect(app.firePreset,app.explodeAt);
    if(app.particleEmitterSet)app.renderer.setParticleEmitter(app.particleEmitter);
    app.renderer.setParticleTemporal(app.particleTemporal);
    // Baked Voronoi shard hulls. Configured before initialize() so the renderer uploads
    // the pool once with the rest of the scene resources.
    {
        std::string convexError;
        if(!app.renderer.configureConvexMeshes(app.game.convexMeshes(),convexError)){
            app.errorCode="NG-E-CONVEX-CONFIG";
            diagnostic("Convex mesh configuration failed: "+convexError);
            report(false);return 5;
        }
    }
    if(!app.rhi->initialize(descriptor,app.error)){
        diagnostic(app.error);report(false);return 4;
    }
    app.rhi->setRtShadows(app.rtShadowRequested);
    app.rhi->setCompositeDebugView(app.debugView);
    app.rhi->setLayerDebugView(app.layerDebugMode,app.layerDebugLayer);
    app.rhi->setMeshShaders(app.meshRequested);
    app.rhi->setFrustumCulling(app.cullRequested);
    app.rhi->setCompute(app.computeRequested,app.computeCorrupt);
    // Re-applied after initialize() because the rebuild resets the particle state.
    app.renderer.setParticleTemporal(app.particleTemporal);
    // Injection must happen after initialize(), which rebuilds the renderer state. It is
    // armed exactly once so the recovery run is a clean renderer.
    if(app.deviceRemovalMode&&app.deviceRemovalAtFrame>0&&!app.deviceRemovalDetected){
        app.renderer.injectDeviceRemovalAtFrame(static_cast<unsigned>(app.deviceRemovalAtFrame));
    }
    app.rtShadowEnabled=app.rhi->rtShadowsEnabled();
    // The hairstyle upload needs the renderer's command list, so it is configured after
    // initialize() rather than with the other scene subsystems.
    app.groomConfigured=false;
    if(!initializeGroom()){diagnostic(app.error);report(false);return 6;}
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
    // The device thread is the only consumer of the mixer. Starting it after the
    // renderer means a headless smoke run with `--audio offline` never touches COM,
    // and a missing endpoint degrades to a silent run instead of a failed one.
    if(app.audioConfigured&&!app.audioOff&&app.audioBackendName=="wasapi"){
        std::string error;
        if(app.audioDevice.start(app.audio,error)){
            app.audioDeviceRunning=true;
            app.audioDeviceStarted=true;
            // One device period of lead: the backend advances the clock by a whole
            // period at a time, so a command stamped at "now" would otherwise arrive
            // already in the past and be counted as late.
            app.audio.setCommandLead(app.audioDevice.info().bufferFrames+
                                     app.audio.config().blockFrames*2);
        }else{
            app.audioDeviceFailed=true;
            app.audioError="NG-E-AUDIO-DEVICE: "+error;
        }
    }
    if(!app.smoke){ShowWindow(app.window,show);UpdateWindow(app.window);if(app.game.firstPerson()){app.paused=true;SetForegroundWindow(app.window);}}
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
        pollTextures();
        engine::Input input;
        if(app.replayEnabled){
            if(app.replayIndex>=app.replayFrames.size())break;
            const ReplayFrame& recorded=app.replayFrames[app.replayIndex];
            input.forward=recorded.forward;
            input.right=recorded.right;
            input.run=recorded.run;
            input.jump=recorded.jump;
            input.interact=recorded.interact;
            input.fire=recorded.fire;
            input.blast=recorded.blast;
            input.yawDelta=recorded.yaw;
            input.pitchDelta=recorded.pitch;
        } else if(app.animationDemo){
            if(app.frames>=30&&app.frames<150)input.forward=1;
            input.run=app.frames>=90&&app.frames<150;
            input.jump=app.frames==150;
            input.interact=app.frames==210;
        } else if(!app.smoke && GetForegroundWindow()==app.window){
            input.forward=static_cast<float>(down('W'))-static_cast<float>(down('S'));
            // Third-person Game input uses world-right; the LH orbit view's
            // screen-right points in the opposite direction.
            input.right=(static_cast<float>(down('D'))-static_cast<float>(down('A')))*(app.game.firstPerson()?1.f:-1.f);
            input.run=down(VK_SHIFT);
            const bool j=down(VK_SPACE),e=down('E');
            const bool f=down('F'),g=down('G');
            input.jump=j&&!app.jumpDown;input.interact=e&&!app.interactDown;
            input.fire=f&&!app.fireDown;input.blast=g&&!app.blastDown;
            if(app.game.firstPerson()){input.fire=false;input.blast=false;}
            app.jumpDown=j;app.interactDown=e;app.fireDown=f;app.blastDown=g;
            input.yawDelta=app.yawDelta+(down(VK_RIGHT)-down(VK_LEFT))*dt*1.6f;
            input.pitchDelta=app.pitchDelta+(down(VK_DOWN)-down(VK_UP))*dt;
        }
        app.yawDelta=0;app.pitchDelta=0;
        // Cinematic physics demo. The scene is authored for these coordinates; the
        // same code path is used by F/G, so evidence covers production projection and
        // blast code rather than a scripted visual fake.
        if(app.physicsDemo&&!app.replayEnabled&&!app.paused){
            if(app.frames==45){app.game.fireProjectileAt({0.0f,0.32f,-2.0f},36.0f);++app.physicsDemoShots;audioTrigger("fire");}
            else if(app.frames==85){app.game.fireProjectileAt({2.8f,1.0f,-2.6f},37.0f);++app.physicsDemoShots;audioTrigger("fire");}
            else if(app.frames==125){app.game.fireProjectileAt({-3.1f,0.85f,-1.8f},35.0f);++app.physicsDemoShots;audioTrigger("fire");}
            else if(app.frames==165){app.game.detonate({0.0f,1.1f,-2.0f},6.5f,10.0f);++app.physicsDemoShots;audioTrigger("blast");}
            else if(app.frames==200){app.game.detonate({3.0f,0.6f,-2.6f},4.5f,14.0f);++app.physicsDemoShots;audioTrigger("blast");}
        }
        // Authored shots. Same production fire path as F; only the target and frame
        // come from the command line, so any scene can be given reproducible evidence
        // without a scene-specific branch in the host.
        if(!app.demoShots.empty()&&!app.replayEnabled&&!app.paused){
            for(const auto& shot:app.demoShots){
                if(shot.frame!=app.frames)continue;
                app.game.fireProjectileAt(shot.target,36.0f);
                ++app.physicsDemoShots;
                audioTrigger("fire");
            }
        }
        // Scripted trigger sweep for audio evidence: one of every scene trigger, in a
        // fixed order and at fixed frames, so a routing regression shows up as a
        // missing play rather than as a subjective "sounds wrong".
        if(app.audioDemo&&!app.replayEnabled&&!app.paused){
            static const char* const sequence[]={"door-open","key","complete","fire","blast"};
            const unsigned index=app.frames/24;
            if(app.frames%24==0&&index<sizeof(sequence)/sizeof(sequence[0]))audioTrigger(sequence[index]);
        }
        // Audition: one sound, non-spatial, looped at the listener. It loops because a
        // 0.3 s one-shot cannot fill the 3 s short-term loudness window, so a level
        // reading from it would understate the mix by ~10 dB. The meter reading this
        // produces is the only level claim that does not depend on where a scene
        // author happened to put an emitter.
        if(!app.audioAuditionSound.empty()&&app.frames==30&&!app.paused){
            const auto handle=app.audioSoundHandles.find(app.audioAuditionSound);
            if(handle!=app.audioSoundHandles.end()){
                engine::audio::PlayParams params;
                params.position=app.audioLastEye;
                params.spatial=false;
                params.bus=engine::audio::Bus::Sfx;
                params.gainDb=0.0f;
                params.loop=true;
                params.priority=255;
                if(app.audio.play(handle->second,params)!=0)++app.audioPlays;else ++app.audioRejected;
            }else{
                ++app.audioRejected;
            }
        }
        if(!app.paused){
            if(app.watch&&!app.replayEnabled&&!app.recordEnabled&&app.frames%60==0)app.game.gameplayRuntime().pollReload();
            const float simulationDt=app.replayEnabled?app.replayFrames[app.replayIndex].dt:app.smoke?1.f/60.f:dt;
            try{app.game.update(input,simulationDt);}
            catch(const engine::gameplay::Error& error){app.errorCode=error.code;app.error=error.what();success=false;break;}
            catch(const engine::animation::Error& error){app.errorCode=error.code;app.error=error.what();success=false;break;}
            catch(const engine::water::Error& error){app.errorCode=error.code;app.error=error.what();success=false;break;}
            app.animationEventCount+=static_cast<unsigned>(app.game.takeAnimationEvents().size());
            const auto presentation=app.game.gameplayRuntime().takePresentation();
            bool saveCheckpoint=false;
            for(const auto& request:presentation){
                const auto name=request.value("name",std::string());
                if(name=="checkpoint")saveCheckpoint=true;
                // `audio.<trigger>` is the gameplay side of the contract: scripts name
                // the event, the scene maps it to a sound. The previous path fired one
                // file per event through the Win32 asynchronous playback API, with no
                // position, no bus and no mix.
                if(name.rfind("audio.",0)==0)audioTrigger(name.substr(6));
            }
            // State edges are the other half: the door and the goal are engine state,
            // not script events, so the host turns each transition into one trigger.
            if(app.game.doorOpen()!=app.audioDoorState){
                app.audioDoorState=app.game.doorOpen();
                if(app.audioDoorState)audioTrigger("door-open");
            }
            if(app.game.hasKey()!=app.audioKeyState){
                app.audioKeyState=app.game.hasKey();
                if(app.audioKeyState)audioTrigger("key");
            }
            if(app.game.complete()!=app.audioCompleteState){
                app.audioCompleteState=app.game.complete();
                if(app.audioCompleteState)audioTrigger("complete");
            }
            if(input.fire)audioTrigger("fire");
            if(input.blast)audioTrigger("blast");
            if(saveCheckpoint&&!app.replayEnabled&&!app.recordEnabled)try{app.game.save(app.checkpointPath);app.liveStatus=L"检查点已保存";}catch(const std::exception& error){app.sceneError=error.what();}
            if(!app.smoke&&app.frames%60==0)try{engine::gameplay::atomicSave("reports/gameplay/live.json",json{{"gameplay",app.game.gameplayRuntime().inspect()}});}catch(const std::exception&){}
            writeReplayLine(input,simulationDt);
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
            // Finish this tick's listener, mixer and rendering before the replay
            // boundary is checked at the next loop iteration.
        }
        engine::Vec3 eye=app.game.orbitCameraEye(),lookAt=app.game.orbitCameraTarget();
        if(app.game.firstPerson()){eye=app.game.cameraEye();lookAt=app.game.cameraTarget();}
        // --camera pins the viewpoint so shadow evidence is reproducible frame to frame.
        if(app.hasCameraOverride){
            eye={app.cameraEye[0],app.cameraEye[1],app.cameraEye[2]};
            lookAt={app.cameraTarget[0],app.cameraTarget[1],app.cameraTarget[2]};
        }
        if(app.cameraCutFrame&&app.frames>=app.cameraCutFrame){
            eye={app.cameraCutEye[0],app.cameraCutEye[1],app.cameraCutEye[2]};
            lookAt={app.cameraCutTarget[0],app.cameraCutTarget[1],app.cameraCutTarget[2]};
        }
        // Audio is submitted after the camera is final and before the frame is
        // rendered, so a sound queued this frame is positioned by this frame's
        // listener. Nothing here reads back into the simulation.
        //
        // The audio clock is the simulation clock, not the wall clock: in smoke mode
        // that is a fixed 1/60 s, which is what makes two runs land on the same pcm
        // hash. An interactive session still follows the real frame time.
        if(!synchronizeEffects()){success=false;break;}
        const float audioDt=app.smoke?1.f/60.f:dt;
        audioUpdateListener(eye,lookAt,audioDt);
        audioPumpTriggers();
        audioPullOffline(audioDt);
        auto view=engine::lookAtLH(eye,lookAt);
        const float nearPlane=std::min(app.game.firstPerson()?.05f:.1f,(app.game.firstPerson()?.05f:.1f)*app.game.playerHeight()/1.8f);
        const auto cameraDelta=eye-lookAt;const float farPlane=std::max(app.game.firstPerson()?180.f:120.f,std::sqrt(cameraDelta.x*cameraDelta.x+cameraDelta.y*cameraDelta.y+cameraDelta.z*cameraDelta.z)+app.game.playerHeight()*4.f);
        auto projection=engine::perspectiveLH(app.game.firstPerson()?app.game.cameraFov():1.05f,float(app.width)/float(app.height),nearPlane,farPlane);
        auto vp=engine::multiply(projection,view);
        nativegpu::Mat4 viewProjection;std::copy(vp.m,vp.m+16,viewProjection.m);
        std::vector<nativegpu::DrawItem> draws;
        for(const auto& pose:app.game.renderPoses()){
            nativegpu::DrawItem d{};
            auto m=engine::compose(pose.position,pose.scale,pose.yaw,pose.pitch,pose.roll);
            std::copy(m.m,m.m+16,d.model.m);
            for(int i=0;i<3;++i)d.color[i]=pose.color[i];d.color[3]=1;
            d.roughness=pose.roughness;d.metalness=pose.metalness;
            d.rayVisibilityMask=pose.rayVisibilityMask;
            // Material table slots 1..3 are the resident checker/brick/metal textures;
            // slot 0 is the missing-material default.
            d.texture=pose.texture+1;d.textureGeneration=0;
            d.material=app.materialsEnabled
                ?(pose.material>=0?pose.material:static_cast<int>(app.defaultMaterial)):-1;
            d.mesh=pose.mesh;
            const auto wet=app.wetness.find(pose.id);
            if(app.surfaceWetness&&wet!=app.wetness.end()&&wet->second.value>wet->second.authored+0.02f){
                d.wetness=wet->second.value;++app.wetDraws;
            }
            draws.push_back(d);
        }
        if(!syncWaterSurfaces(app.error)){success=false;break;}
        auto palette=app.character->skeleton.palette(app.game.animationController().pose());
        nativegpu::DrawItem actor{};
        auto actorModel=engine::compose(app.game.playerPosition(),{1,1,1},app.game.yaw());
        std::copy(actorModel.m,actorModel.m+16,actor.model.m);
        actor.color[0]=0.06f;actor.color[1]=0.65f;actor.color[2]=0.82f;actor.color[3]=1;
        actor.texture=3;actor.roughness=0.55f;actor.skin=app.character.get();actor.palette=&palette;
        actor.morphWeights=&app.game.animationController().featureState().morphWeights;
        actor.deformation=&app.game.animationController().deformation();
        actor.material=app.materialsEnabled?static_cast<int>(app.defaultMaterial):-1;
        const bool scenePlayerCharacter=std::any_of(app.document.scene.entities.begin(),app.document.scene.entities.end(),[](const auto& entity){return entity.kind=="player"&&entity.characterAsset>=0;});
        if(!app.game.firstPerson()&&!scenePlayerCharacter) {
            if(app.character->meshParts.empty())draws.push_back(actor);
            else for(const auto& part:app.character->meshParts) {
                auto draw=actor;draw.firstIndex=part.firstIndex;draw.indexCount=part.indexCount;
                draw.color[0]=draw.color[1]=draw.color[2]=1;
                if(app.materialsEnabled)for(std::size_t material=0;material<app.document.scene.materials.size();++material)
                    if(app.document.scene.materials[material].name==part.material){draw.material=static_cast<int>(material);break;}
                draws.push_back(draw);
            }
        }
        if(app.groomConfigured){app.renderer.setHairHeadTransform(nativegpu::identityMatrix());app.groomHeadAttached=false;}
        std::string groomPlayerEntity;
        for(const auto& entity:app.document.scene.entities)if(entity.kind=="player"){groomPlayerEntity=entity.id;break;}
        auto characters=app.game.renderCharacters();
        for(auto& character:characters) {
            if(app.groomConfigured&&!groomPlayerEntity.empty()&&character.entityId==groomPlayerEntity)app.groomHeadAttached=applyGroomHeadPose(character);
            nativegpu::DrawItem draw{};const auto model=engine::compose(character.position,character.scale,character.yaw,character.pitch,character.roll);
            std::copy(model.m,model.m+16,draw.model.m);std::copy(character.color,character.color+3,draw.color);draw.color[3]=1;
            draw.texture=3;draw.roughness=.55f;draw.rayVisibilityMask=2;draw.skin=character.asset;draw.palette=&character.palette;
            draw.morphWeights=&character.morphWeights;draw.deformation=character.deformation;
            const int sceneMaterial=characterMaterialIndex(character.entityId);
            draw.material=app.materialsEnabled?(sceneMaterial>=0?sceneMaterial:static_cast<int>(app.defaultMaterial)):-1;
            if(character.asset->meshParts.empty())draws.push_back(draw);
            else for(std::size_t index=0;index<character.asset->meshParts.size();++index) {
                const auto& part=character.asset->meshParts[index];draw.firstIndex=part.firstIndex;draw.indexCount=part.indexCount;
                if(app.materialsEnabled&&index<character.materials.size())draw.material=character.materials[index];draws.push_back(draw);
            }
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
            light.isStatic=true;
            light.castSurfaceShadow=source.castSurfaceShadow;light.castVolumetricShadow=source.castVolumetricShadow;
            light.volumetricIntensity=source.volumetricIntensity;
            lights.push_back(light);
        }
        // The field drives the fire light (FIELD-REALISM-GOAL batch 4): the scene light
        // nearest the emitter flickers with a deterministic per-frame noise and swells
        // during the explosion, so geometry, fog and the plume share one light event.
        if(app.firePreset&&app.particlesRequested&&!lights.empty()){
            int nearest=-1;
            float nearestDistance=1e9f;
            for(size_t index=0;index<lights.size();++index){
                const float dx=lights[index].position[0]-app.particleEmitter[0];
                const float dy=lights[index].position[1]-app.particleEmitter[1];
                const float dz=lights[index].position[2]-app.particleEmitter[2];
                const float distance=std::sqrt(dx*dx+dy*dy+dz*dz);
                if(distance<nearestDistance){nearestDistance=distance;nearest=static_cast<int>(index);}
            }
            if(nearest>=0&&nearestDistance<3.5f){
                unsigned hash=static_cast<unsigned>(app.frames)*2654435761u;
                hash^=hash>>13;hash*=0x5bd1e995u;hash^=hash>>15;
                const float noise=static_cast<float>(hash&0xFFFFu)/65535.0f;
                const float slow=std::sin(static_cast<float>(app.frames)*0.071f);
                const float flicker=0.82f+0.10f*slow+0.14f*noise;
                const float burstBoost=1.0f+2.6f*app.renderer.particleBurst();
                lights[nearest].intensity*=flicker*burstBoost;
                // A few centimetres of wander keeps the fire shadows alive without
                // making the light look detached from the pit.
                lights[nearest].position[0]+=(noise-0.5f)*0.05f;
                lights[nearest].position[2]+=(slow*0.5f)*0.04f;
                app.fireLightApplied=true;
                app.fireLightIntensity=lights[nearest].intensity;
                app.fireLightFlicker=flicker;
            }
        }
        for(const auto& source:app.game.dynamicLights()){
            nativegpu::LightDesc light{};
            light.position[0]=source.position.x;light.position[1]=source.position.y;light.position[2]=source.position.z;
            for(int i=0;i<3;++i)light.color[i]=source.color[i];
            light.intensity=source.intensity;light.range=source.range;
            light.direction[0]=source.direction.x;
            light.direction[1]=source.direction.y;
            light.direction[2]=source.direction.z;
            light.cosOuter=source.cosOuter;light.cosInner=source.cosInner;light.type=source.type;
            light.castSurfaceShadow=source.castSurfaceShadow;light.castVolumetricShadow=source.castVolumetricShadow;
            light.volumetricIntensity=source.volumetricIntensity;
            lights.push_back(light);
        }
        if(app.frames%6==0)app.renderer.setOverlay(app.game.firstPerson()?host::adventureHud(app.game,app.width,app.height,app.paused,app.tour,app.completionVisible,Clock::now()<app.noticeUntil?app.notice:L""):std::vector<nativegpu::OverlayLayer>{});
        const bool shotFrame=app.screenshotRequested&&!app.screenshotAttempted&&(app.frames+1)>=app.screenshotFrame;
        if(shotFrame&&!app.rhi->requestScreenshot(app.screenshotPath,app.screenshotError)){
            success=false;app.screenshotAttempted=true;
            app.liveStatus=L"Screenshot prepare failed: "+wide(app.screenshotError);
            if(app.smoke)break;
        }
        // Particle obstacles: scene colliders as axis-aligned boxes. Entities carry no
        // rotation, so the AABB is exact rather than a conservative approximation.
        if(app.particlesRequested){
            std::vector<nativegpu::FieldCollider> colliders;
            if(app.particleColliders){
                colliders.reserve(app.document.scene.entities.size());
                for(const auto& entity:app.document.scene.entities){
                    if(!entity.collider)continue;
                    nativegpu::FieldCollider box{};
                    box.center[0]=entity.position.x;box.center[1]=entity.position.y;box.center[2]=entity.position.z;
                    box.extent[0]=entity.scale.x;box.extent[1]=entity.scale.y;box.extent[2]=entity.scale.z;
                    colliders.push_back(box);
                }
            }
            app.rhi->setFieldColliders(colliders);
        }
        const float eyePosition[3]={eye.x,eye.y,eye.z};
        if(!app.rhi->submit(viewProjection,eyePosition,draws,lights,app.error)){
            // Device-removal path: stop rendering, record the failure, rebuild the renderer
            // once and keep going if recovery works. The scene file is never touched.
            if(app.deviceRemovalMode&&app.error.find("DEVICE_REMOVED")!=std::string::npos){
                app.deviceRemovalDetected=true;
                app.deviceRemovalFrame=app.frames;
                app.deviceRemovalError=app.error;
                app.liveStatus=L"Device removed; recovering";
                if(!restartRenderer()){app.deviceRemovalRecovered=false;success=false;break;}
                app.deviceRemovalRecovered=true;
                ++app.deviceRemovalRecoveries;
                app.deviceRemovalFrameAfterRecovery=app.frames;
                app.liveStatus=L"Device recovered";
                continue;
            }
            if(app.errorCode.rfind("NG-E-EFFECT-",0)!=0)app.errorCode="NG-E-RENDER-FRAME";
            diagnostic(app.error);success=false;break;
        }
        // The explosion leaves a mark: once the field reports a strong burst, entities near
        // the fire are charred in memory. This is a visual consequence only; the scene file
        // is untouched and a reload restores the original colours.
        if(!app.scorchApplied&&app.particlesRequested&&app.renderer.particleBurst()>0.5f){
            app.scorchApplied=true;
            for(auto& entity:app.document.scene.entities){
                if(entity.kind=="light")continue;
                const float dx=entity.position.x-app.particleEmitter[0];
                const float dy=entity.position.y-app.particleEmitter[1];
                const float dz=entity.position.z-app.particleEmitter[2];
                const float distance=std::sqrt(dx*dx+dy*dy+dz*dz);
                if(distance>4.5f)continue;
                const float char01=1.0f-distance/4.5f;
                const float keep=1.0f-0.65f*char01;
                for(int channel=0;channel<3;++channel)entity.color[channel]*=keep;
                ++app.scorchedEntities;
            }
        }
        ++app.frames;
        if(!app.progressPath.empty()&&app.frames%30==0)try{engine::gameplay::atomicSave(app.progressPath,json{{"schema","nativegpu.render-progress/1"},{"frames",app.frames},{"revision",app.document.revision},{"environmentSha256",app.document.scene.lighting.environmentHash}});}catch(const std::exception& e){app.errorCode="NG-E-PROGRESS-IO";app.error=e.what();success=false;break;}
        if(shotFrame){
            app.screenshotAttempted=true;
            if(!app.rhi->captureScreenshot(app.screenshotPath,app.screenshotError)){
                success=false;
                app.liveStatus=L"Screenshot capture failed: "+wide(app.screenshotError);
                if(app.smoke)break;
            } else {
                app.screenshotTaken=true;
                app.liveStatus=L"Screenshot saved";
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
                app.liveStatus=L"Budget downgrade applied: "+wide(name);
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
    mouseLook(false);
    if(app.error.find("NG-E-MAT-TRANSPARENCY-BUDGET")!=std::string::npos||app.screenshotError.find("NG-E-MAT-TRANSPARENCY-BUDGET")!=std::string::npos){app.errorCode="NG-E-MAT-TRANSPARENCY-BUDGET";if(app.error.empty())app.error=app.screenshotError;}
    // The device thread must be stopped before the report is written: the report
    // prints a snapshot of the mixer, and a running thread would keep changing it.
    audioShutdown();
    if(success){std::string completionError;if(!app.renderer.synchronize(completionError)){app.errorCode=completionError.find("NG-E-MAT-TRANSPARENCY")!=std::string::npos?"NG-E-MAT-TRANSPARENCY-BUDGET":"NG-E-GPU-SYNC";app.error=completionError;success=false;}}
    if(success&&!app.savePath.empty())try{app.game.save(app.savePath);}catch(const std::exception& e){app.errorCode="NG-E-SAVE-IO";app.error=e.what();success=false;}
    report(success);
    if(app.rhi)app.rhi->shutdown();app.ready=false;
    DestroyWindow(app.window);DeleteObject(app.font);DeleteObject(app.titleFont);DeleteObject(app.background);
    return success?0:5;
}
#endif

namespace {

struct PreviewVegetationAsset {
    nativegpu::VegetationMeshData mesh;
    nativegpu::VegetationTextureSet textures;
    nativegpu::VegetationMaterial material;
    float bounds[3]{1,1,0};
};
struct PreviewVegetationRegion {
    engine::VegetationRegionReference reference;
    std::filesystem::path cooked,textures;
    std::shared_ptr<engine::vegetation::RegionFile> file;
    std::map<unsigned,std::shared_ptr<PreviewVegetationAsset>> assets;
};
struct PreviewEntityOverride {
    bool active = true;
    float position[3]{};
    float scale[3]{1, 1, 1};
};
struct PreviewRuntime {
    bool active=false;
    std::string mode="scene",digest;
    std::filesystem::path projectRoot;
    engine::vegetation::World vegetationWorld;
    std::vector<PreviewVegetationRegion> vegetationRegions;
    std::string vegetationSignature;
    engine::vegetation::WorldStats vegetationStats;
    // Editor state. Physics/gameplay only advance in simulate/run; the camera is
    // independent of the game camera in edit and simulate.
    host::EditorCamera camera;
    bool cameraReady=false;
    std::string play="edit";
    bool paused=false;
    std::map<std::string,PreviewEntityOverride> overrides;
    unsigned long long overlayRevision=0;
} preview;

std::string previewRead(const std::filesystem::path& path) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot open "+path.u8string()+" (win32 "+std::to_string(GetLastError())+")");
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(file,&size)||size.QuadPart<0){const DWORD code=GetLastError();CloseHandle(file);throw std::runtime_error("cannot size "+path.u8string()+" (win32 "+std::to_string(code)+")");}
    std::string bytes(static_cast<std::size_t>(size.QuadPart),'\0');DWORD read=0;
    if(!bytes.empty()&&(!ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)||read!=bytes.size())){const DWORD code=GetLastError();CloseHandle(file);throw std::runtime_error("cannot read "+path.u8string()+" (win32 "+std::to_string(code)+")");}
    CloseHandle(file);return bytes;
}

nativegpu::VegetationTexture previewTexture(const engine::vegetation::TextureImage& image) {
    nativegpu::VegetationTexture result;result.width=image.width;result.height=image.height;result.srgb=image.srgb;
    for(const auto& level:image.levels){nativegpu::VegetationTextureLevel target;target.width=level.width;target.height=level.height;target.pixels=level.pixels;result.levels.push_back(std::move(target));}
    return result;
}

nativegpu::VegetationTextureSet previewTextures(const engine::vegetation::Species& species,const std::filesystem::path& directory) {
    nativegpu::VegetationTextureSet set;const auto library=json::parse(previewRead(directory/"library.json"));
    const auto role=[&](const std::string& name)->json{for(const auto& entry:library.at("roles"))if(entry.at("role").get<std::string>()==name)return entry;return json{};};
    const auto assign=[&](nativegpu::VegetationTexture& target,const json& roles,const char* slot){
        if(!roles.is_object()||!roles.contains(slot)||roles.at(slot).is_null())return;
        const auto entry=role(roles.at(slot).at("role").get<std::string>());
        if(!entry.is_object()||!entry.contains("file"))throw std::runtime_error("vegetation texture role is not cooked");
        target=previewTexture(engine::vegetation::parseTexture(previewRead(directory/entry.at("file").get<std::string>())));
    };
    if(species.textures.contains("bark")&&!species.textures.at("bark").is_null()){const auto& roles=species.textures.at("bark").at("roles");assign(set.barkBaseColor,roles,"baseColor");assign(set.barkNormal,roles,"normal");assign(set.barkOrm,roles,"orm");assign(set.barkHeight,roles,"height");}
    if(species.textures.contains("leaf")&&!species.textures.at("leaf").is_null()){const auto& roles=species.textures.at("leaf").at("roles");assign(set.leafBaseColor,roles,"baseColor");assign(set.leafNormal,roles,"normal");assign(set.leafOrm,roles,"orm");assign(set.leafThickness,roles,"thickness");assign(set.leafOpacity,roles,"opacity");}
    return set;
}

std::shared_ptr<PreviewVegetationAsset> previewVegetationAsset(PreviewVegetationRegion& region,unsigned index) {
    const auto found=region.assets.find(index);if(found!=region.assets.end())return found->second;
    const auto& ref=region.file->species().at(index);const auto library=json::parse(previewRead(region.cooked/"library.json"));
    std::string file;
    for(const auto& entry:library.at("species"))if(entry.at("id").get<std::string>()==ref.id){file=entry.at("file").get<std::string>();break;}
    if(file.empty())throw std::runtime_error("vegetation region species is absent from cooked library: "+ref.id);
    const auto bytes=previewRead(region.cooked/file);const auto species=engine::vegetation::parseSpecies(bytes,ref.digest);
    auto result=std::make_shared<PreviewVegetationAsset>();const auto& mesh=species.meshForLod(0);
    result->mesh.positions=mesh.positions;result->mesh.normals=mesh.normals;result->mesh.uvs=mesh.uvs;result->mesh.windWeights=mesh.windWeights;result->mesh.colors=mesh.colors;result->mesh.indices=mesh.indices;
    unsigned first=0,count=0;bool any=false;
    for(const auto& primitive:species.primitives)if(primitive.lod==0){nativegpu::VegetationPrimitive target;target.material=primitive.material;target.firstIndex=primitive.firstIndex;target.indexCount=primitive.indexCount;result->mesh.primitives.push_back(target);first=any?std::min(first,primitive.firstIndex):primitive.firstIndex;count=any?std::max(count,primitive.firstIndex+primitive.indexCount):primitive.firstIndex+primitive.indexCount;any=true;}
    result->mesh.firstIndex=first;result->mesh.indexCount=any?count-first:0;
    result->textures=previewTextures(species,region.textures);nativegpu::materialFromSpecies(species,result->material);
    result->bounds[0]=species.lod.at(0).heightMetres;result->bounds[1]=species.lod.at(0).radiusMetres;
    region.assets.emplace(index,result);return result;
}

void previewInitializeVegetation(const host::ScenePreviewOptions& options) {
    preview.vegetationRegions.clear();preview.vegetationWorld=engine::vegetation::World{};preview.vegetationSignature.clear();
    for(const auto& reference:app.document.scene.vegetationRegions) {
        PreviewVegetationRegion region;region.reference=reference;
        const auto resolve=[&](const std::string& path){const auto value=std::filesystem::u8path(path);return value.is_absolute()?value:options.projectRoot/value;};
        region.cooked=resolve(reference.cooked);region.textures=resolve(reference.textures);
        region.file=std::make_shared<engine::vegetation::RegionFile>(resolve(reference.file).u8string());
        preview.vegetationRegions.push_back(std::move(region));
    }
    std::vector<std::shared_ptr<engine::vegetation::RegionFile>> files;files.reserve(preview.vegetationRegions.size());
    for(auto& region:preview.vegetationRegions){files.push_back(region.file);preview.vegetationWorld.addRegion(region.file);}
    preview.vegetationWorld.setLoader([files](uint32_t region,uint32_t page){return files.at(region)->readPage(page);});
    preview.vegetationWorld.setFallbackReserve(64.0f);
}

bool previewSyncVegetation(const engine::Vec3& eye,const nativegpu::Mat4& vp,std::string& error) {
    if(preview.vegetationRegions.empty())return true;
    preview.vegetationWorld.beginFrame(app.frames);preview.vegetationWorld.requestAround({eye.x,eye.y,eye.z},{},240.0f);preview.vegetationWorld.tick();
    const auto frustum=nativegpu::Frustum::fromViewProjection(vp.m);
    (void)preview.vegetationWorld.cull(frustum,nullptr,{eye.x,eye.y,eye.z},1000.0f,vp.m,app.width,app.height);
    std::string signature;
    for(uint32_t region=0;region<preview.vegetationWorld.regionCount();++region)for(uint32_t page=0;page<preview.vegetationRegions[region].file->pageCount();++page)
        if(preview.vegetationWorld.pageInstances(region,page)){signature+=std::to_string(region)+":"+std::to_string(page)+";";}
    if(signature==preview.vegetationSignature)return true;
    try {
        std::vector<nativegpu::VegetationLayer> layers;
        for(uint32_t regionIndex=0;regionIndex<preview.vegetationWorld.regionCount();++regionIndex) {
            auto& region=preview.vegetationRegions[regionIndex];
            for(uint32_t pageIndex=0;pageIndex<region.file->pageCount();++pageIndex) {
                const auto* instances=preview.vegetationWorld.pageInstances(regionIndex,pageIndex);if(!instances)continue;
                for(uint32_t speciesIndex=0;speciesIndex<region.file->species().size();++speciesIndex) {
                    nativegpu::VegetationLayer layer;const auto asset=previewVegetationAsset(region,speciesIndex);
                    layer.mesh=asset->mesh;layer.textures=asset->textures;layer.material=asset->material;std::copy(asset->bounds,asset->bounds+3,layer.bounds);layer.page=region.file->page(pageIndex);
                    for(auto instance:*instances)if(instance.species==speciesIndex){instance.species=0;layer.instances.push_back(instance);}
                    if(!layer.instances.empty())layers.push_back(std::move(layer));
                }
            }
        }
        if(!app.renderer.configureVegetation(std::move(layers),error))return false;
        preview.vegetationSignature=signature;
    }catch(const std::exception& exception){error=exception.what();return false;}
    return true;
}

engine::ConvexMesh previewSphere() {
    engine::ConvexMesh mesh;mesh.min[0]=mesh.min[1]=mesh.min[2]=-1;mesh.max[0]=mesh.max[1]=mesh.max[2]=1;mesh.hasTextureUvs=true;
    constexpr unsigned columns=48,rings=24;constexpr float pi=3.14159265358979323846f;
    for(unsigned y=0;y<=rings;++y)for(unsigned x=0;x<=columns;++x) {
        const float u=float(x)/columns,v=float(y)/rings,phi=v*pi,theta=u*2*pi,sp=std::sin(phi);
        engine::ConvexVertex vertex;vertex.position[0]=sp*std::cos(theta);vertex.position[1]=std::cos(phi);vertex.position[2]=sp*std::sin(theta);
        std::copy(vertex.position,vertex.position+3,vertex.normal);vertex.uv[0]=u;vertex.uv[1]=v;vertex.textureUvs[0][0]=u;vertex.textureUvs[0][1]=v;
        vertex.tangent[0]=-std::sin(theta);vertex.tangent[1]=0;vertex.tangent[2]=std::cos(theta);vertex.tangent[3]=1;mesh.vertices.push_back(vertex);
    }
    for(unsigned y=0;y<rings;++y)for(unsigned x=0;x<columns;++x){const uint32_t a=y*(columns+1)+x,b=a+1,c=a+columns+1,d=c+1;mesh.indices.insert(mesh.indices.end(),{a,c,b,b,c,d});}
    return mesh;
}

void previewReset() {
    audioShutdown();if(app.rhi){app.rhi->shutdown();app.rhi.reset();}app.ready=false;app.renderer.clearMaterials();
    app.effects.clear();app.effectHandles.clear();app.effectTextureUploads.clear();app.effectTextureSlots.clear();app.effectId.clear();app.effectLibrary=engine::effects::Library{};
    app.splash.handles.clear();app.splash.gameTick=0;app.splash.lastSequence=0;app.splash.library=engine::effects::Library{};
    app.effectFields.reset();app.simulationFields.reset();app.clothPreview.reset();app.groomConfigured=false;preview.vegetationRegions.clear();
}

using PreviewEntityMap = std::map<std::string, const engine::Entity*>;

bool previewPlayValid(const std::string& play) {
    return play == "edit" || play == "simulate" || play == "run";
}

// Returning to edit (or starting simulate/run) always rebuilds the authored state.
// Simulation results are therefore never written back implicitly, and a stopped
// simulation restores the exact layout the user had before pressing simulate.
void previewRestoreAuthoredState() {
    app.game.initialize(app.document.scene);
    if (app.character) app.game.setCharacter(app.character, app.animationFeatures);
    engine::determinism::registry().reset(app.seed);
    app.game.reset();
    app.game.takeAnimationEvents();
}

void previewInitializeCamera() {
    const auto eye = app.game.orbitCameraEye();
    const auto target = app.game.orbitCameraTarget();
    preview.camera.target[0] = target.x;
    preview.camera.target[1] = target.y;
    preview.camera.target[2] = target.z;
    const float dx = eye.x - target.x, dy = eye.y - target.y, dz = eye.z - target.z;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    preview.camera.distance = std::clamp(distance, 1.5f, 100000.0f);
    preview.camera.yaw = std::atan2(dx, -dz);
    preview.camera.pitch = std::asin(std::clamp(-dy / preview.camera.distance, -1.0f, 1.0f));
    preview.camera.orthographic = false;
    host::editorCameraClamp(preview.camera);
    preview.cameraReady = true;
}

PreviewEntityMap previewEntityMap() {
    PreviewEntityMap map;
    for (const auto& entity : app.document.scene.entities) map.emplace(entity.id, &entity);
    return map;
}

// Draft overrides are deltas from the authored transform. That keeps compound poses
// (vehicle parts, bonded cells) in their internal arrangement instead of collapsing
// every part onto the override position.
bool previewApplyPoseOverride(engine::Pose& pose, const PreviewEntityMap& authored) {
    const auto override = preview.overrides.find(pose.id);
    if (override == preview.overrides.end()) return true;
    if (!override->second.active) return false;
    const auto found = authored.find(pose.id);
    if (found == authored.end()) return true;
    const auto& source = *found->second;
    pose.position.x += override->second.position[0] - source.position.x;
    pose.position.y += override->second.position[1] - source.position.y;
    pose.position.z += override->second.position[2] - source.position.z;
    pose.scale.x *= override->second.scale[0] / std::max(1.0e-4f, source.scale.x);
    pose.scale.y *= override->second.scale[1] / std::max(1.0e-4f, source.scale.y);
    pose.scale.z *= override->second.scale[2] / std::max(1.0e-4f, source.scale.z);
    return true;
}

bool previewApplyCharacterOverride(engine::CharacterPose& character,
                                   const PreviewEntityMap& authored) {
    const auto override = preview.overrides.find(character.entityId);
    if (override == preview.overrides.end()) return true;
    if (!override->second.active) return false;
    const auto found = authored.find(character.entityId);
    if (found == authored.end()) return true;
    const auto& source = *found->second;
    character.position.x += override->second.position[0] - source.position.x;
    character.position.y += override->second.position[1] - source.position.y;
    character.position.z += override->second.position[2] - source.position.z;
    character.scale.x *= override->second.scale[0] / std::max(1.0e-4f, source.scale.x);
    character.scale.y *= override->second.scale[1] / std::max(1.0e-4f, source.scale.y);
    character.scale.z *= override->second.scale[2] / std::max(1.0e-4f, source.scale.z);
    return true;
}

bool previewFocusBounds(const std::string& entityId, const PreviewEntityMap& authored,
                        float minimum[3], float maximum[3]) {
    bool any = false;
    const auto include = [&](const engine::Entity& entity) {
        float center[3] = {entity.position.x, entity.position.y, entity.position.z};
        float scale[3] = {entity.scale.x, entity.scale.y, entity.scale.z};
        if (const auto override = preview.overrides.find(entity.id); override != preview.overrides.end()) {
            if (!override->second.active) return;
            for (int axis = 0; axis < 3; ++axis) {
                scale[axis] = override->second.scale[axis];
                center[axis] = override->second.position[axis];
            }
        }
        float extent[3]{};
        for (int axis = 0; axis < 3; ++axis) extent[axis] = std::max(0.05f, scale[axis] * 0.5f);
        for (int axis = 0; axis < 3; ++axis) {
            if (!any) {
                minimum[axis] = center[axis] - extent[axis];
                maximum[axis] = center[axis] + extent[axis];
            } else {
                minimum[axis] = std::min(minimum[axis], center[axis] - extent[axis]);
                maximum[axis] = std::max(maximum[axis], center[axis] + extent[axis]);
            }
        }
        any = true;
    };
    if (entityId.empty()) {
        for (const auto& entity : app.document.scene.entities) include(entity);
    } else {
        const auto found = authored.find(entityId);
        if (found == authored.end()) return false;
        include(*found->second);
    }
    return any;
}

} // namespace

namespace host {

bool scenePreviewOpen(HWND window,unsigned width,unsigned height,const ScenePreviewOptions& options,std::string& error) {
    scenePreviewClose();previewReset();preview=PreviewRuntime{};preview.active=false;preview.mode=options.mode;preview.projectRoot=options.projectRoot;
    preview.play = options.mode=="scene" ? (previewPlayValid(options.play)?options.play:"edit") : "run";
    preview.paused = false;
    app.document=host::SceneDocument{};app.character.reset();app.fallbackCharacter.reset();
    app.embeddedPreview=true;app.smoke=true;app.watch=false;app.viewport=window;app.width=std::max(16u,width);app.height=std::max(16u,height);
    app.error.clear();app.sceneError.clear();app.errorCode.clear();app.frames=0;app.running=true;app.paused=false;app.materialsEnabled=false;app.defaultMaterial=0;
    app.audioConfigured=false;app.audioDeviceRunning=false;app.audioDeviceStarted=false;app.audioDeviceFailed=false;app.audioError.clear();app.audioEmitterVoices.clear();app.audioTriggers.clear();
    app.scenePath=options.scene;app.texturePath=options.textures;app.characterPath=options.character;app.effectLibraryPath=options.effectLibrary;
    app.effectId=options.effect;app.gpuEffectRequested=options.mode=="effect";app.effectOrigin={0,1,0};app.audioOff=!options.audio;app.audioBackendName=options.audio?"wasapi":"off";
    app.targetFrames=0;app.hasCameraOverride=options.mode!="scene";
    if(options.mode=="material"){app.cameraEye[0]=2.4f;app.cameraEye[1]=1.6f;app.cameraEye[2]=3.6f;app.cameraTarget[0]=app.cameraTarget[1]=app.cameraTarget[2]=0;}
    else if(options.mode=="character"||options.mode=="clip"){app.cameraEye[0]=2.4f;app.cameraEye[1]=1.5f;app.cameraEye[2]=4.2f;app.cameraTarget[0]=0;app.cameraTarget[1]=0.9f;app.cameraTarget[2]=0;}
    else if(options.mode!="scene"){app.cameraEye[0]=2.2f;app.cameraEye[1]=1.6f;app.cameraEye[2]=3.4f;app.cameraTarget[0]=0;app.cameraTarget[1]=0.9f;app.cameraTarget[2]=0;}
    try {
        preview.digest=engine::assets::sha256(previewRead(options.scene));
        if(!reloadScene(options.scene)){error=app.error;return false;}
        if(options.mode=="clip"||options.mode=="character") {
            bool found=false;for(auto& entity:app.document.scene.entities)if(entity.characterAsset>=0){if(!options.clip.empty())entity.character.clip=options.clip;found=true;}
            if(!found&&app.character) {
                engine::Entity actor;actor.id="native-character-preview";actor.name="Character preview";actor.kind="player";actor.position={0,0,0};actor.characterAsset=0;actor.character.clip=options.clip;
                app.document.scene.characterAssets.push_back(app.character);app.document.scene.entities={actor};found=true;
            }
            if(!found){error="character preview scene contains no character entity or animation asset";return false;}
            if(!options.clip.empty())app.document.scene.characterAssets.front()->clipIndex(options.clip);
            if(options.asset.empty()) {
                auto selected=std::find_if(app.document.scene.entities.begin(),app.document.scene.entities.end(),[](const auto& entity){return entity.characterAsset>=0&&entity.kind=="player";});
                if(selected==app.document.scene.entities.end())selected=std::find_if(app.document.scene.entities.begin(),app.document.scene.entities.end(),[](const auto& entity){return entity.characterAsset>=0;});
                auto previewEntity=*selected;previewEntity.position={0,0,0};
                app.document.scene.entities={previewEntity};
            }
        }
        if(options.mode=="material") {
            int selected=-1;for(std::size_t i=0;i<app.document.scene.materials.size();++i)if(app.document.scene.materials[i].name==options.material){selected=static_cast<int>(i);break;}
            if(selected<0){error="material not found in preview scene: "+options.material;return false;}
            engine::Entity sphere;sphere.id="native-material-preview";sphere.name="Material UV sphere";sphere.kind="prop";sphere.position={0,0,0};sphere.scale={1,1,1};sphere.color[0]=sphere.color[1]=sphere.color[2]=1;sphere.hasMaterial=true;sphere.materialIndex=selected;sphere.importedMesh=0;
            app.document.scene.entities={sphere};app.document.scene.lights.clear();app.document.scene.importedMeshes={previewSphere()};
            app.game.initialize(app.document.scene);if(app.character)app.game.setCharacter(app.character,false);
        } else if(options.mode=="character"&&!options.clip.empty()) {app.game.initialize(app.document.scene);if(app.character)app.game.setCharacter(app.character,app.animationFeatures);}
        if(!configureRendererMaterials()){error=app.error;return false;}
        applySceneEnvironment();app.rhi=nativegpu::rhi::wrap("d3d12",app.renderer,error);if(!app.rhi)return false;
        engine::determinism::registry().reset(app.seed);app.game.reset();
        if(!initializeEffects()){error=app.error;return false;}
        if(!app.audioOff&&!audioConfigure()){app.audioOff=true;app.audioConfigured=false;}
        audioApplyScene();
        if(!app.renderer.configureConvexMeshes(app.game.convexMeshes(),error))return false;
        nativegpu::rhi::Descriptor descriptor;descriptor.window=window;descriptor.width=app.width;descriptor.height=app.height;
        if(!app.rhi->initialize(descriptor,error))return false;
        app.rhi->setRtShadows(true);app.rhi->setFrustumCulling(true);app.ready=true;
        if(app.audioConfigured&&!app.audioOff){std::string audioError;if(app.audioDevice.start(app.audio,audioError)){app.audioDeviceRunning=true;app.audioDeviceStarted=true;app.audio.setCommandLead(app.audioDevice.info().bufferFrames+app.audio.config().blockFrames*2);}else app.audioError=audioError;}
        if(!app.document.scene.vegetationRegions.empty())previewInitializeVegetation(options);
        previewInitializeCamera();
        app.began=app.last=Clock::now();preview.active=true;return true;
    }catch(const std::exception& exception){error=exception.what();return false;}
}

bool scenePreviewResize(unsigned width,unsigned height,std::string& error) {
    if(!preview.active||!app.rhi){error="scene preview is not open";return false;}
    app.width=std::max(16u,width);app.height=std::max(16u,height);return app.rhi->resize(app.width,app.height,error);
}

bool scenePreviewFrame(float deltaSeconds,std::string& error) {
    if(!preview.active||!app.ready){error="scene preview is not open";return false;}
    try {
        const float dt=std::isfinite(deltaSeconds)?std::clamp(deltaSeconds,0.0f,0.1f):1.0f/60.0f;
        // Editing is a frozen clock: gameplay, physics and animation do not advance while the
        // user arranges the scene. Simulate advances physics without player input; run
        // advances the whole game. Switching modes restores the authored state first.
        if(preview.play!="edit"&&!preview.paused){app.game.update(engine::Input{},dt);app.game.takeAnimationEvents();}
        if(!synchronizeEffects()){error=app.error;return false;}
        const PreviewEntityMap authored=previewEntityMap();
        const bool editCamera=preview.mode=="scene"&&preview.play!="run"&&preview.cameraReady;
        engine::Vec3 eye,target;
        float vp[16];
        nativegpu::Mat4 viewProjection;
        if(editCamera) {
            const auto pose=host::editorCameraPose(preview.camera);
            eye={pose.eye[0],pose.eye[1],pose.eye[2]};
            target={pose.target[0],pose.target[1],pose.target[2]};
            const float aspect=app.height>0?float(app.width)/float(app.height):1.0f;
            host::editorCameraViewProjection(preview.camera,aspect,vp);
            std::copy(vp,vp+16,viewProjection.m);
        } else {
            eye=app.game.orbitCameraEye();target=app.game.orbitCameraTarget();
            if(app.game.firstPerson()){eye=app.game.cameraEye();target=app.game.cameraTarget();}
            if(app.hasCameraOverride){eye={app.cameraEye[0],app.cameraEye[1],app.cameraEye[2]};target={app.cameraTarget[0],app.cameraTarget[1],app.cameraTarget[2]};}
            const auto matrix=engine::multiply(engine::perspectiveLH(app.game.firstPerson()?app.game.cameraFov():1.05f,float(app.width)/float(app.height),.05f,1000.0f),engine::lookAtLH(eye,target));
            std::copy(matrix.m,matrix.m+16,viewProjection.m);
        }
        audioUpdateListener(eye,target,dt);audioPumpTriggers();
        std::vector<nativegpu::DrawItem> draws;
        const auto poses=app.game.renderPoses();
        for(const auto& sourcePose:poses){
            auto pose=sourcePose;
            if(!previewApplyPoseOverride(pose,authored))continue;
            nativegpu::DrawItem draw{};const auto model=engine::compose(pose.position,pose.scale,pose.yaw,pose.pitch,pose.roll);std::copy(model.m,model.m+16,draw.model.m);std::copy(pose.color,pose.color+3,draw.color);draw.color[3]=1;draw.roughness=pose.roughness;draw.metalness=pose.metalness;draw.rayVisibilityMask=pose.rayVisibilityMask;draw.texture=pose.texture+1;draw.material=app.materialsEnabled?(pose.material>=0?pose.material:static_cast<int>(app.defaultMaterial)):-1;draw.mesh=pose.mesh;{const auto wet=app.wetness.find(pose.id);if(app.surfaceWetness&&wet!=app.wetness.end()&&wet->second.value>wet->second.authored+0.02f){draw.wetness=wet->second.value;++app.wetDraws;}}draws.push_back(draw);
        }
        std::vector<engine::animation::Matrix> playerPalette;
        if(!app.embeddedPreview&&!app.game.firstPerson()) {
            playerPalette=app.character->skeleton.palette(app.game.animationController().pose());nativegpu::DrawItem actor{};const auto model=engine::compose(app.game.playerPosition(),{1,1,1},app.game.yaw());std::copy(model.m,model.m+16,actor.model.m);actor.color[0]=actor.color[1]=actor.color[2]=1;actor.color[3]=1;actor.texture=3;actor.skin=app.character.get();actor.palette=&playerPalette;actor.morphWeights=&app.game.animationController().featureState().morphWeights;actor.deformation=&app.game.animationController().deformation();actor.material=app.materialsEnabled?static_cast<int>(app.defaultMaterial):-1;draws.push_back(actor);
        }
        if(app.groomConfigured){app.renderer.setHairHeadTransform(nativegpu::identityMatrix());app.groomHeadAttached=false;}
        std::string groomPlayerEntity;
        for(const auto& entity:app.document.scene.entities)if(entity.kind=="player"){groomPlayerEntity=entity.id;break;}
        auto characters=app.game.renderCharacters();
        for(auto& character:characters) {
            if(app.groomConfigured&&!groomPlayerEntity.empty()&&character.entityId==groomPlayerEntity)app.groomHeadAttached=applyGroomHeadPose(character);
            if(!previewApplyCharacterOverride(character,authored))continue;
            nativegpu::DrawItem base{};const auto model=engine::compose(character.position,character.scale,character.yaw,character.pitch,character.roll);std::copy(model.m,model.m+16,base.model.m);std::copy(character.color,character.color+3,base.color);base.color[3]=1;base.texture=3;base.rayVisibilityMask=2;base.skin=character.asset;base.palette=&character.palette;base.morphWeights=&character.morphWeights;base.deformation=character.deformation;{const int sceneMaterial=characterMaterialIndex(character.entityId);base.material=app.materialsEnabled?(sceneMaterial>=0?sceneMaterial:static_cast<int>(app.defaultMaterial)):-1;}
            if(character.asset->meshParts.empty())draws.push_back(base);else for(std::size_t index=0;index<character.asset->meshParts.size();++index){auto draw=base;draw.firstIndex=character.asset->meshParts[index].firstIndex;draw.indexCount=character.asset->meshParts[index].indexCount;if(app.materialsEnabled&&index<character.materials.size())draw.material=character.materials[index];draws.push_back(draw);}
        }
        std::vector<nativegpu::LightDesc> lights;
        const auto addLight=[&](const engine::Light& source,bool dynamic){nativegpu::LightDesc light{};light.position[0]=source.position.x;light.position[1]=source.position.y;light.position[2]=source.position.z;std::copy(source.color,source.color+3,light.color);light.intensity=source.intensity;light.range=source.range;light.isStatic=!dynamic;if(dynamic){light.direction[0]=source.direction.x;light.direction[1]=source.direction.y;light.direction[2]=source.direction.z;light.cosOuter=source.cosOuter;light.cosInner=source.cosInner;light.type=source.type;}light.castSurfaceShadow=source.castSurfaceShadow;light.castVolumetricShadow=source.castVolumetricShadow;light.volumetricIntensity=source.volumetricIntensity;lights.push_back(light);};
        for(const auto& light:app.document.scene.lights)addLight(light,false);for(const auto& light:app.game.dynamicLights())addLight(light,true);
        if(!previewSyncVegetation(eye,viewProjection,error))return false;
        if(!syncWaterSurfaces(error))return false;
        const float eyePosition[3]={eye.x,eye.y,eye.z};if(!app.rhi->submit(viewProjection,eyePosition,draws,lights,error))return false;
        ++app.frames;return true;
    }catch(const std::exception& exception){error=exception.what();return false;}
}

bool scenePreviewCapture(const std::string& path,std::string& error) {
    if(!preview.active||!app.rhi){error="scene preview is not open";return false;}
    if(!app.rhi->requestScreenshot(path,error))return false;if(!scenePreviewFrame(1.0f/60.0f,error))return false;return app.rhi->captureScreenshot(path,error);
}

bool scenePreviewSetPlay(const std::string& play, bool paused, std::string& error) {
    if (!preview.active) { error = "scene preview is not open"; return false; }
    if (!previewPlayValid(play)) { error = "invalid play mode '" + play + "'"; return false; }
    if (preview.mode != "scene" && play != "run") {
        error = "only a scene preview can switch between edit and simulate";
        return false;
    }
    if (play != preview.play) {
        previewRestoreAuthoredState();
        preview.play = play;
    }
    preview.paused = paused;
    return true;
}

bool scenePreviewStep(float deltaSeconds, std::string& error) {
    if (!preview.active) { error = "scene preview is not open"; return false; }
    if (preview.play == "edit") { error = "step requires simulate or run mode"; return false; }
    if (!preview.paused) { error = "pause before stepping"; return false; }
    const float dt = std::isfinite(deltaSeconds)
        ? std::clamp(deltaSeconds, 0.0f, 0.1f) : 1.0f / 60.0f;
    app.game.update(engine::Input{}, dt);
    app.game.takeAnimationEvents();
    return true;
}


bool scenePreviewSetLayerDebug(const std::string& spec, std::string& error) {
    unsigned mode = 0, layer = 0;
    if (!parseLayerDebugSpec(spec, mode, layer)) { error = "unknown layer debug view '" + spec + "'"; return false; }
    app.layerDebugMode = mode; app.layerDebugLayer = layer;
    if (app.rhi) app.rhi->setLayerDebugView(mode, layer);
    return true;
}
bool scenePreviewSetView(const std::string& view, std::string& error) {
    if (!preview.active) { error = "scene preview is not open"; return false; }
    if (view != "perspective" && view != "top" && view != "front" && view != "side") {
        error = "unknown camera view '" + view + "'";
        return false;
    }
    if (!preview.cameraReady) previewInitializeCamera();
    host::editorCameraSetView(preview.camera, view);
    return true;
}

void scenePreviewCameraOrbit(float dxPixels, float dyPixels) {
    if (!preview.cameraReady) return;
    host::editorCameraOrbit(preview.camera, dxPixels, dyPixels);
}

void scenePreviewCameraPan(float dxPixels, float dyPixels) {
    if (!preview.cameraReady) return;
    host::editorCameraPan(preview.camera, dxPixels, dyPixels, static_cast<float>(app.height));
}

void scenePreviewCameraDolly(float wheelSteps) {
    if (!preview.cameraReady) return;
    host::editorCameraDolly(preview.camera, wheelSteps);
}

void scenePreviewCameraMove(float forward, float right, float up, float deltaSeconds,
                            float metresPerSecond) {
    if (!preview.cameraReady) return;
    host::editorCameraMove(preview.camera, forward, right, up, deltaSeconds, metresPerSecond);
}

bool scenePreviewFocus(const std::string& entityId, std::string& error) {
    if (!preview.active) { error = "scene preview is not open"; return false; }
    if (!preview.cameraReady) previewInitializeCamera();
    float minimum[3]{}, maximum[3]{};
    if (!previewFocusBounds(entityId, previewEntityMap(), minimum, maximum)) {
        error = entityId.empty() ? "the scene has no bounds to focus"
                                 : "cannot focus an entity that is not in the scene: " + entityId;
        return false;
    }
    if (!host::editorCameraFocus(preview.camera, minimum, maximum)) {
        error = "focus produced a non-finite camera target";
        return false;
    }
    return true;
}

ScenePreviewCameraState scenePreviewCamera() {
    ScenePreviewCameraState state;
    state.camera = preview.camera;
    state.valid = preview.cameraReady;
    return state;
}

bool scenePreviewSetEntityOverride(const std::string& entityId, const float position[3],
                                   const float scale[3], bool active, std::string& error) {
    if (!preview.active) { error = "scene preview is not open"; return false; }
    if (entityId.empty() || entityId.size() > 128) {
        error = "override needs an entity UUID";
        return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(position[axis]) || std::abs(position[axis]) > 100000.0f) {
            error = "override position must be finite and within +/-100000";
            return false;
        }
        // Positive scale only: zero or mirrored scale changes winding, normals and
        // collision semantics, which this preview does not model yet.
        if (!std::isfinite(scale[axis]) || scale[axis] < 0.001f || scale[axis] > 10000.0f) {
            error = "override scale must be in [0.001, 10000]";
            return false;
        }
    }
    PreviewEntityOverride value;
    value.active = active;
    for (int axis = 0; axis < 3; ++axis) {
        value.position[axis] = position[axis];
        value.scale[axis] = scale[axis];
    }
    preview.overrides[entityId] = value;
    ++preview.overlayRevision;
    return true;
}

void scenePreviewClearOverrides() {
    preview.overrides.clear();
    ++preview.overlayRevision;
}

void scenePreviewClose() {
    if(app.rhi)app.rhi->shutdown();audioShutdown();app.ready=false;preview.active=false;
    preview.play="edit";preview.paused=false;preview.cameraReady=false;
    preview.overrides.clear();preview.overlayRevision=0;
}

ScenePreviewStats scenePreviewStats() {
    ScenePreviewStats stats;stats.frames=app.frames;stats.digest=preview.digest;stats.vegetationLayers=app.rhi?app.renderer.vegetationLayerCount():0;
    stats.overlayRevision=preview.overlayRevision;
    stats.play=preview.mode=="scene"?preview.play:"run";
    stats.paused=preview.paused;
    stats.cameraOrthographic=preview.camera.orthographic;
    stats.cameraYaw=preview.camera.yaw;
    stats.cameraPitch=preview.camera.pitch;
    stats.cameraDistance=preview.camera.distance;
    if(!preview.vegetationRegions.empty()){const auto world=preview.vegetationWorld.stats();stats.worldInstances=world.worldInstances;stats.residentInstances=world.residentInstances;}
    return stats;
}

} // namespace host
