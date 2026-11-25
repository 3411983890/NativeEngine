#include "../host/scene_loader.hpp"
#include <cmath>
#include <fstream>
#include <iostream>

using namespace engine;
namespace {
unsigned checks=0;
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
bool near(float a,float b,float epsilon=.003f){return std::abs(a-b)<epsilon;}
Entity object(std::string id,std::string kind,Vec3 p,Vec3 scale,bool collider=false){Entity e;e.id=e.name=id;e.kind=kind;e.position=p;e.scale=scale;e.collider=collider;return e;}
Scene room(float height=1.8f,float width=.6f){Scene s;s.entities={object("actor","player",{0,0,2},{width,height,width}),object("floor","ground",{0,-.5f,0},{100,1,100},true)};return s;}
float walk(Game& game,unsigned ticks=240){Input input;input.forward=1;float peak=0;for(unsigned i=0;i<ticks;++i){game.update(input,1.f/120);peak=std::max(peak,game.playerPosition().y);}return peak;}
std::string hash(const Game& game){determinism::Hasher h;game.digest(h);return h.hex();}
}
int main(int argc,char** argv){
  try{
    auto small=room(.9f,.4f),tall=room(1.8f,.6f);for(auto* scene:{&small,&tall})scene->entities.push_back(object("low-ceiling","prop",{0,1.3f,0},{10,.2f,2},true));
    Game a,b;a.initialize(small);b.initialize(tall);walk(a);walk(b);check(a.playerPosition().z<-2,"Short character did not pass below the low ceiling");check(near(b.playerPosition().z,1.3f),"Tall character did not collide using its authored height");
    auto narrow=room(1.8f,.4f),wide=room(1.8f,1.2f);narrow.entities[0].position.z=wide.entities[0].position.z=3;
    for(auto* scene:{&narrow,&wide}){scene->entities.push_back(object("left","wall",{-1.9f,1.5f,0},{3,3,4},true));scene->entities.push_back(object("right","wall",{1.9f,1.5f,0},{3,3,4},true));}
    a.initialize(narrow);b.initialize(wide);walk(a);walk(b);check(a.playerPosition().z<-2,"Narrow character could not pass the corridor");check(near(b.playerPosition().z,2.6f),"Wide character ignored its collision radius");
    for(float height:{.9f,2.1f}){auto scene=room(height);scene.entities[0].position={0,0,0};scene.entities.push_back(object("ceiling","prop",{0,2.3f,0},{10,.2f,10},true));a.initialize(scene);Input jump;jump.jump=true;float peak=0;for(unsigned i=0;i<240;++i){a.update(jump,1.f/120);peak=std::max(peak,a.playerPosition().y);}check(peak<=2.2f-height+.003f&&peak>0,"Vertical sweep used an incorrect character head height");}
    small=room(.9f,.4f);tall=room(1.8f,.6f);for(auto* scene:{&small,&tall})scene->entities.push_back(object("step","prop",{0,.125f,0},{4,.25f,1},true));
    a.initialize(small);b.initialize(tall);const auto smallPeak=walk(a),tallPeak=walk(b);check(smallPeak<.01f&&tallPeak>.24f,"Automatic step height did not scale with body height");
    tall.entities[0].character.controller={{"stepHeight",0}};a.initialize(tall);check(walk(a)<.01f,"Explicitly disabled stepping still climbed the step");
    tall.entities[0].character.controller={{"stepHeight",.4f}};tall.entities.push_back(object("step-ceiling","prop",{0,2.1f,0},{4,.2f,2},true));a.initialize(tall);check(walk(a)<.01f,"Automatic stepping ignored head clearance");
    auto body=room(3.6f,1.2f);a.initialize(body);check(near(a.playerHeight(),3.6f)&&near(a.playerRadius(),.6f)&&near(a.playerStepHeight(),.72f),"Body dimensions were not derived from scene dimensions");check(near(a.cameraEye().y,3.28f)&&near(a.orbitCameraTarget().y,2.2f),"Camera anchors did not scale with body height");
    const auto eye=a.orbitCameraEye(),target=a.orbitCameraTarget();check(eye.z>14.f&&target.y>2,"Orbit camera distance did not adapt to body size");
    auto actor=a.gameplayRuntime().world.find("actor");auto transform=*a.gameplayRuntime().world.get(actor,"transform");transform["scale"]={.4f,.9f,.4f};a.gameplayRuntime().world.set(actor,"transform",transform);a.update({},1.f/120);check(near(a.playerHeight(),.9f)&&near(a.playerRadius(),.2f),"Live ECS resizing did not update the controller");
    const auto snapshot=a.snapshot();const auto before=hash(a);a.reset();check(near(a.playerHeight(),3.6f),"Reset did not restore authored dimensions");a.restore(snapshot);check(near(a.playerHeight(),.9f)&&hash(a)==before,"Snapshot did not restore collision dimensions");
    body.entities[0].character.controller={{"eyeHeight",1.2f},{"interactionHeight",.5f},{"stepHeight",.1f},{"orbitDistance",3},{"orbitHeight",2},{"targetHeight",1}};
    body.entities.push_back(object("switch","prop",{0,.5f,2.5f},{.1f,.1f,.1f}));body.entities.back().components["interactable"]={{"actorOffset",.9f},{"verticalRange",.1f},{"range",2}};
    a.initialize(body);check(near(a.cameraEye().y,1.2f)&&near(a.playerStepHeight(),.1f)&&near(a.orbitCameraTarget().y,1),"Explicit controller anchors were ignored");check(a.interactionTarget()=="switch","Explicit interaction height did not reach the target");
    body.entities[0].character.controller=gameplay::Json::object();body.entities.back().position.y=1.8f;a.initialize(body);check(a.interactionTarget()=="switch","Default interaction offset did not scale with body height");
    auto huge=room(1.8f,40);a.initialize(huge);walk(a,10);check(std::isfinite(a.playerPosition().x)&&std::isfinite(a.playerPosition().z),"Body larger than world limits inverted the movement clamp");
    check(argc==2,"Expected a fitted scene fixture");host::SceneDocument document;std::string error;check(host::loadScene(argv[1],document,error),error.c_str());
    Game fitted;fitted.initialize(document.scene);for(unsigned i=0;i<240;++i)fitted.update({},1.f/120);
    unsigned npcs=0;for(const auto& pose:fitted.renderCharacters())for(const auto& entity:document.scene.entities)if(entity.id==pose.entityId&&entity.kind!="player"){
      ++npcs;const auto* transform=fitted.gameplayRuntime().world.get(fitted.gameplayRuntime().world.find(entity.id),"transform");const float foot=(*transform)["position"][1];check(std::abs(foot)<.04f,"Feet-anchored NPC body floated above or fell through the floor");check(near(pose.position.y-entity.character.offset.y*entity.character.scale.y,foot,.04f),"Dynamic body and character visual used different anchors");}
    check(npcs==2,"Static and dynamic NPC anchors were not both exercised");const auto saved=fitted.snapshot();const auto savedHash=hash(fitted);for(unsigned i=0;i<60;++i)fitted.update({},1.f/120);fitted.restore(saved);check(hash(fitted)==savedHash,"Fitted NPC snapshot restore diverged");
    auto ray=document.scene;ray.entities.resize(3);ray.entities[0].position={0,0,3};ray.entities[0].scale={.6f,1.8f,.6f};ray.entities[0].character.controller=gameplay::Json::object();ray.entities[0].components["camera"]={{"mode","firstPerson"}};ray.entities[2].position={0,0,1.5f};
    ray.entities.push_back(object("ray-target","prop",{0,1.64f,0},{.1f,.1f,.1f}));ray.entities.back().components["interactable"]={{"actorOffset",.9f},{"verticalRange",2},{"range",5}};
    Game aiming;aiming.initialize(ray);check(aiming.interactionTarget().empty(),"Feet-anchored NPC did not occlude the first-person interaction ray");const auto blocker=aiming.gameplayRuntime().world.find(ray.entities[2].id);aiming.gameplayRuntime().world.set(blocker,"collider",{{"enabled",false}});aiming.update({},1.f/120);check(aiming.interactionTarget()=="ray-target","Disabling the NPC collider did not restore the interaction ray");
    gameplay::Json data;std::ifstream(argv[1])>>data;auto bad=data;bad["entities"][0]["character"]["controller"]["orbitDistance"]=0;const auto file=std::filesystem::path(argv[1]).parent_path()/"controller-invalid.scene.json";std::ofstream(file)<<bad.dump();check(!host::loadScene(file,document,error),"Invalid controller profile was accepted");
    std::cout<<"{\"success\":true,\"checks\":"<<checks<<",\"scope\":\"character height/radius, headroom, steps, camera/interactions, ECS resize, NPC anchors and snapshots\"}\n";return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
