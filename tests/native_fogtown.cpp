#include "../host/scene_loader.hpp"
#include "../engine/native/math.hpp"
#include <iostream>
#include <filesystem>
#include <cmath>
#include <memory>

using namespace engine;
using gameplay::Json;
namespace {
struct Run {
    std::unique_ptr<Game> game=std::make_unique<Game>();
    host::SceneDocument document;
    std::vector<Input> inputs;
    unsigned frames=0;
    bool checkpoints=false;
    explicit Run(bool save=false):checkpoints(save) {
        std::string error;
        if(!host::loadScene(".screen/fogtown.scene.json",document,error))throw std::runtime_error(error);
        game->initialize(document.scene);
        game->setCharacter(animation::loadAsset("build/animation/mannequin/character.json"));
    }
    void require(bool condition,const std::string& message)const {
        if(!condition)throw std::runtime_error(message+"; position="+std::to_string(game->playerPosition().x)+","+std::to_string(game->playerPosition().z)+" diagnostics="+game->gameplayRuntime().diagnostics().dump());
    }
    std::string id(const std::string& name)const {for(const auto& e:document.scene.entities)if(e.name==name)return e.id;throw std::runtime_error("Missing entity "+name);}
    Vec3 position(const std::string& name)const {for(const auto& e:document.scene.entities)if(e.name==name)return e.position;throw std::runtime_error(name);}
    const Json& quest()const {for(const auto& b:game->gameplayRuntime().behaviors())if(b.script=="fogtown/quest")return b.state;throw std::runtime_error("quest missing");}
    bool flag(const std::string& name)const {const auto& world=game->gameplayRuntime().world;return world.get(world.find(id("player")),"progress")->at("flags").value(name,false);}
    void step(Input input={}) {
        game->update(input,1.f/60);inputs.push_back(input);++frames;
        game->takeAnimationEvents();game->gameplayRuntime().takePresentation();
        require(game->gameplayRuntime().diagnostics().empty(),"script error");
    }
    void wait(unsigned count){for(unsigned i=0;i<count;i++)step();}
    void aim(Vec3 p){const auto eye=game->cameraEye();const float dx=p.x-eye.x,dz=p.z-eye.z,dy=p.y-eye.y;Input i;i.yawDelta=std::remainder(game->yaw()-std::atan2(dx,-dz),6.2831853f);i.pitchDelta=std::atan2(-dy,std::sqrt(dx*dx+dz*dz))-game->pitch();step(i);}
    void go(float x,float z,float tolerance=.14f){
        for(unsigned frame=0;frame<1600;++frame){const auto p=game->playerPosition();const float dx=x-p.x,dz=z-p.z,distance=std::sqrt(dx*dx+dz*dz);if(distance<tolerance){step();return;}
            Input i;i.yawDelta=std::remainder(game->yaw()-std::atan2(dx,-dz),6.2831853f);i.pitchDelta=-game->pitch();i.forward=std::min(1.f,distance*60/3.5f);step(i);
        }
        game->save("reports/fogtown/blocked.json");
        require(false,"Movement blocked toward "+std::to_string(x)+","+std::to_string(z));
    }
    void interact(const std::string& name){aim(position(name));require(game->interactionTarget()==id(name),"Cannot aim at "+name+"; target="+game->interactionTarget());Input i;i.interact=true;step(i);wait(3);}
    void save(const std::string& name){if(checkpoints)game->save("outputs/fogtown/checkpoints/"+name+".json");}
    std::string digest()const {determinism::Hasher h;game->digest(h);return h.hex();}
};
void continuation(Run& run) {
    const auto before=run.digest();run.game->save("reports/fogtown/continuation.json");
    Run resumed;resumed.game->load("reports/fogtown/continuation.json");run.require(resumed.digest()==before,"Save/restart digest differs");
    for(int i=0;i<120;++i){run.step();resumed.step();run.require(run.digest()==resumed.digest(),"Snapshot continuation differs");}
}
void complete(Run& run) {
    run.save("arrival");
    run.require(run.game->firstPerson(),"Not first-person");
    run.require(std::abs(run.game->cameraEye().y-run.game->playerPosition().y-1.64f)<.001f,"Eye height");
    run.go(0,10.1f);run.interact("值守员");run.require(run.flag("briefed"),"Briefing did not grant key");
    run.go(-3.4f,10);run.go(-3.4f,2);run.go(-8,2);run.go(-8,1.2f);run.interact("仓库正门");
    run.go(-8,-2.8f);run.go(-6.6f,-3.4f);run.go(-6.6f,-6.7f);run.wait(30);
    run.require(run.flag("plate"),"Crate did not latch pressure plate");
    for(const auto& name:{"后室闸门","侧窗闸门"})run.require(!run.game->gameplayRuntime().world.get(run.game->gameplayRuntime().world.find(run.id(name)),"collider")->at("enabled").get<bool>(),"Plate door did not open");
    run.go(-8,-6.7f);run.go(-8,-9.8f);run.interact("维修工具箱");run.require(run.flag("tools"),"Tool missing");
    run.interact("备用钥匙");
    run.save("warehouse");continuation(run);
    run.go(-8,-7);run.go(-8,-2.6f);run.go(-8,2);run.go(0,2);run.go(0,-18);run.go(2.2f,-18.6f);
    run.interact("受控爆破起爆器");run.wait(600);
    run.require(run.game->fractureCount()>=3,"Native demolition did not fracture all rubble");
    run.go(0,-20.5f);run.go(0,-26);run.go(-2,-26);run.interact("供能发电机");run.require(run.flag("repaired"),"Repair failed");
    run.save("repair");
    run.go(0,-35.8f);run.interact("钟楼 供能节点");run.require(run.quest().at("node")==0,"Wrong order must reset nodes");
    run.go(-3.6f,-32.9f);run.interact("水井 供能节点");run.go(0,-35.8f);run.interact("钟楼 供能节点");run.go(3.6f,-32.9f);run.interact("风车 供能节点");
    run.require(run.flag("square"),"Square sequence failed");run.save("square");
    run.go(5,-32.9f);run.go(5,-36.2f);run.interact("维修柜");
    const auto& world=run.game->gameplayRuntime().world;
    const auto& inventory=world.get(world.find(run.id("player")),"inventory")->at("items");
    run.require(inventory.value("warehouse_key",0)==1&&inventory.value("service_key",0)==1,"Independent key inventories interfered");
    run.go(0,-40);run.go(0,-44.1f);run.interact("信号塔启动控制台");
    run.require(run.quest().at("running").get<bool>(),"Challenge did not start");
    const auto active=run.game->snapshot();
    // Timeout is tested with the real timer, then the exact active state is restored.
    Run timeout;timeout.game->restore(active);timeout.wait(2705);
    run.require(!timeout.quest().at("running").get<bool>()&&!timeout.game->complete(),"Timeout failed");
    run.require(timeout.flag("square")&&timeout.flag("repaired"),"Timeout lost previous progress");
    timeout.interact("信号塔启动控制台");run.require(timeout.quest().at("running").get<bool>(),"Challenge retry failed");
    run.go(-3,-47.4f);run.interact("西侧中继");run.go(0,-48);run.go(3,-50.3f);run.interact("东侧中继");run.go(1,-55.3f);run.interact("塔基发射终端");
    run.require(run.game->complete(),"Final completion failed");run.save("complete");
    run.require(run.game->dynamicLights().size()>=15,"Town lighting was not restored");
}
void screenSpaceControls() {
    // Project a fixed landmark with the same view/projection as the host. This
    // catches reversed controls even when movement and aim agree in world space.
    auto projected = [](const Game& game, Vec3 point) {
        const auto vp=multiply(perspectiveLH(game.cameraFov(),16.f/9,.05f,180.f),lookAtLH(game.cameraEye(),game.cameraTarget()));
        const float w=vp.m[3]*point.x+vp.m[7]*point.y+vp.m[11]*point.z+vp.m[15];
        return Vec3{(vp.m[0]*point.x+vp.m[4]*point.y+vp.m[8]*point.z+vp.m[12])/w,
                    (vp.m[1]*point.x+vp.m[5]*point.y+vp.m[9]*point.z+vp.m[13])/w,
                    (vp.m[2]*point.x+vp.m[6]*point.y+vp.m[10]*point.z+vp.m[14])/w};
    };
    for(float yaw:{0.f,.7f,1.5707963f,3.1415927f,-2.4f})for(float pitch:{-.8f,0.f,.8f}) {
        Scene scene;
        Entity player;player.id="actor";player.kind="player";player.components={{"camera",{{"mode","firstPerson"},{"initialYaw",yaw},{"initialPitch",pitch}}}};
        Entity ground;ground.id="ground";ground.kind="ground";ground.position={0,-.2f,0};ground.scale={100,.4f,100};ground.collider=true;
        scene.entities={player,ground};Game game;game.initialize(scene);
        const auto eye=game.cameraEye(),target=game.cameraTarget();
        const Vec3 landmark{eye.x+5*(target.x-eye.x),eye.y+5*(target.y-eye.y),eye.z+5*(target.z-eye.z)};
        auto check=[&](Input input,const char* name,int axis,float expectedSign) {
            game.reset();game.update(input,1.f/60);
            const auto screen=projected(game,landmark);
            const float coordinate=axis==0?screen.x:screen.y;
            if(!std::isfinite(coordinate)||coordinate*expectedSign<.001f)throw std::runtime_error(std::string("Reversed screen-space control: ")+name);
        };
        Input input;input.right=1;check(input,"D moves right",0,-1);
        input={};input.right=-1;check(input,"A moves left",0,1);
        input={};input.yawDelta=.08f;check(input,"mouse/right arrow turns right",0,-1);
        input={};input.yawDelta=-.08f;check(input,"mouse/left arrow turns left",0,1);
        // NDC Y points upward: looking down moves a fixed landmark upward.
        input={};input.pitchDelta=.08f;check(input,"mouse down looks down",1,1);
        input={};input.pitchDelta=-.08f;check(input,"mouse up looks up",1,-1);
    }
    std::cout<<"Fogtown controls: A/D, horizontal look and vertical look passed at 15 camera orientations.\n";
}
void gazeAndOcclusion() {
    Scene scene;
    Entity player;player.id="actor";player.kind="player";player.position={0,0,0};player.components={{"camera",{{"mode","firstPerson"}}}};
    Entity ground;ground.id="ground";ground.kind="ground";ground.position={0,-.2f,0};ground.scale={20,.4f,20};ground.collider=true;
    Entity target;target.id="target";target.kind="prop";target.position={0,1,-2};target.components={{"interactable",{{"range",3},{"prompt","test"}}}};
    Entity wall;wall.id="wall";wall.kind="wall";wall.position={0,1,-1};wall.scale={2,2,.2f};wall.collider=true;
    scene.entities={player,ground,target,wall};Game game;game.initialize(scene);
    Input input;input.pitchDelta=.31f;game.update(input,0);if(!game.interactionTarget().empty())throw std::runtime_error("Interaction penetrates wall");
    game.gameplayRuntime().world.set(game.gameplayRuntime().world.find("wall"),"collider",{{"enabled",false}});game.update({},1.f/120);
    if(game.interactionTarget()!="target")throw std::runtime_error("Visible target not selected");
    input={};input.yawDelta=3.14159265f;game.update(input,0);if(!game.interactionTarget().empty())throw std::runtime_error("Interaction behind camera");
    input={};input.pitchDelta=20;game.update(input,0);if(game.pitch()>1.451f)throw std::runtime_error("Pitch clamp failed");
}
}
int main(){
    try {
        std::filesystem::create_directories("reports/fogtown");
        screenSpaceControls();gazeAndOcclusion();
        Run first(true);complete(first);const auto digest=first.digest();const auto routeFrames=first.frames;
        // Reapply exactly the recorded movement/interaction frames from a clean start.
        Run repeated;for(const auto& input:first.inputs)repeated.step(input);
        first.require(repeated.game->complete()&&repeated.digest()==digest,"Fixed-input replay diverged");
        continuation(first);
        gameplay::atomicSave("reports/fogtown/evidence.json",{{"format","nativegpu.fogtown-evidence/1"},{"ok",true},{"entities",first.document.scene.entities.size()},{"fixedInputFrames",routeFrames},{"simulationSeconds",routeFrames/60.0},{"digest",digest},{"fractures",first.game->fractureCount()},{"checks",{"first-person camera","all five areas via movement inputs","independent keys and doors","crate pressure plate opens two doors","native controlled destruction","wrong node order resets","ordered power restoration","timer failure and nearby retry","full ending and powered lights","full save/restart continuation","fixed-input replay digest match"}},{"manualNormalSpeedPlaytest",false}});
        std::cout<<"Fogtown: complete first-person adventure, physical route, save/restart, timeout/retry and deterministic replay passed. "<<first.frames<<" frames.\n";
    }catch(const std::exception& e){std::cerr<<"Fogtown failed: "<<e.what()<<"\n";return 1;}
}
