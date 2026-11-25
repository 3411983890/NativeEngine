#include "../../host/scene_loader.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace engine;
namespace {
unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
void tick(Game& game,Input input,unsigned count){for(unsigned i=0;i<count;++i)game.update(input,1.f/120.f);}
void initialize(Game& game,Scene scene){game.initialize(scene);game.setCharacter(scene.characterAssets.front(),false);}
}
int main(int argc,char** argv){try{
    check(argc==2,"Expected the starter project scene");
    host::SceneDocument document;std::string error;
    check(host::loadScene(std::filesystem::u8path(argv[1]),document,error),error.c_str());
    auto scene=document.scene;
    const auto actor=std::find_if(scene.entities.begin(),scene.entities.end(),[](const auto& e){return e.kind=="player";});
    check(actor!=scene.entities.end()&&actor->characterAsset>=0,"Starter robot was not loaded");
    check(scene.characterAssets.front()->meshParts.size()==1,"Robot mesh material was lost");
    Game game;initialize(game,scene);tick(game,{},30);
    check(!game.firstPerson(),"Starter camera must be third person");
    check(game.grounded()&&std::abs(game.playerPosition().y)<.005f,"Robot does not stand on the floor");
    check(game.animationController().state()==animation::State::Idle,"Idle clip is not selected");
    check(game.renderCharacters().size()==1,"Starter robot is invisible");
    auto base=scene;
    base.entities.erase(std::remove_if(base.entities.begin(),base.entities.end(),[](const auto& e){return e.kind!="ground"&&e.kind!="player";}),base.entities.end());
    const Vec3 spawn=actor->position;
    Input move;move.forward=1;initialize(game,base);tick(game,move,120);
    const float walkDistance=spawn.z-game.playerPosition().z;
    check(std::abs(walkDistance-base.walkSpeed)<.04f,"Walk speed is incorrect");
    check(game.animationController().state()==animation::State::Walk,"Walk clip is not selected");
    move.run=true;initialize(game,base);tick(game,move,120);
    const float runDistance=spawn.z-game.playerPosition().z;
    check(std::abs(runDistance-base.runSpeed)<.04f&&runDistance>walkDistance,"Run speed is incorrect");
    check(game.animationController().state()==animation::State::Run,"Run clip is not selected");
    initialize(game,base);Input jump;jump.jump=true;game.update(jump,1.f/120);
    check(!game.grounded()&&game.animationController().state()==animation::State::Jump,"Jump did not leave the floor");
    float peak=game.playerPosition().y;bool fall=false;
    for(unsigned i=0;i<180;++i){game.update({},1.f/120);peak=std::max(peak,game.playerPosition().y);fall|=game.animationController().state()==animation::State::Fall;}
    check(peak>1.1f&&peak<1.3f&&fall&&game.grounded(),"Jump/fall/landing is incorrect");
    // Holding space cannot repeatedly jump without a fresh press.
    initialize(game,base);tick(game,jump,240);check(game.grounded(),"Held jump retriggers on landing");
    initialize(game,scene);move.run=false;tick(game,move,240);
    const auto crate=std::find_if(scene.entities.begin(),scene.entities.end(),[](const auto& e){return e.name=="物理方块 A";});
    physics::Aabb pushed;
    check(game.entityColliderBounds(crate->id,pushed)&&(pushed.min.z+pushed.max.z)*.5f<-.1f,"Character did not push the dynamic cube");
    auto wallScene=base;const auto wall=std::find_if(scene.entities.begin(),scene.entities.end(),[](const auto& e){return e.kind=="wall";});wallScene.entities.push_back(*wall);
    initialize(game,wallScene);move.run=true;tick(game,move,600);
    const float wallFront=wall->position.z+wall->scale.z*.5f;
    check(game.playerPosition().z>=wallFront+game.playerRadius()-.005f,"Character tunneled through the wall");
    const auto eye=game.orbitCameraEye(),target=game.orbitCameraTarget();
    check(std::abs(target.z-game.playerPosition().z)<.001f&&eye.z>target.z+4,"Camera does not follow the robot");
    game.reset();check(std::abs(game.playerPosition().z-spawn.z)<.001f,"Reset did not restore spawn");
    // In-place hips stay inside the collision proxy throughout every locomotion clip.
    const auto& asset=*scene.characterAssets.front();
    const auto hips=std::find_if(asset.skeleton.bones.begin(),asset.skeleton.bones.end(),[](const auto& b){return b.name=="mixamorig:Hips";});
    const auto bone=std::distance(asset.skeleton.bones.begin(),hips);
    for(const auto& clip:asset.clips)for(unsigned frame=0;frame<clip.frameCount;frame++){
        const auto pose=clip.sample(double(frame)/60);
        check(std::abs(pose.translation[bone][0]-asset.skeleton.bones[bone].rest.translation.x)<.0002f&&std::abs(pose.translation[bone][2]-asset.skeleton.bones[bone].rest.translation.z)<.0002f,"Visual root drift escaped the collision proxy");
    }
    std::cout<<"{\"success\":true,\"checks\":"<<checks<<",\"walkDistance\":"<<walkDistance<<",\"runDistance\":"<<runDistance<<",\"jumpPeak\":"<<peak<<"}\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
