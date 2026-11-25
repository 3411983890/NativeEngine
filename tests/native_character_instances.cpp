#include "../host/scene_loader.hpp"
#include "../third_party/json.hpp"
#include <fstream>
#include <iostream>
#include <cmath>

namespace {
unsigned checks=0;
void check(bool ok,const std::string& message){++checks;if(!ok)throw std::runtime_error(message);}
bool near(float a,float b){return std::abs(a-b)<.0003f;}
std::string poseHash(const engine::CharacterPose& pose){engine::determinism::Hasher hash;for(const auto& matrix:pose.palette)for(float value:matrix)hash.quantized(value);for(float value:pose.morphWeights)hash.quantized(value);return hash.hex();}
std::string gameHash(const engine::Game& game){engine::determinism::Hasher hash;game.digest(hash);return hash.hex();}
const engine::CharacterPose& named(const std::vector<engine::CharacterPose>& poses,const std::string& id){for(const auto& pose:poses)if(pose.entityId==id)return pose;throw std::runtime_error("Missing character pose");}
}
int main(int argc,char** argv){
  try {
    check(argc==2,"Expected isolated character scene fixture");const std::filesystem::path file=argv[1];host::SceneDocument document;std::string error;
    check(host::loadScene(file,document,error),error);check(document.scene.characterAssets.size()==3,"Shared character headers must load once");
    const auto& entities=document.scene.entities;std::vector<const engine::Entity*> actors;for(const auto& entity:entities)if(entity.characterAsset>=0)actors.push_back(&entity);
    check(actors.size()==4,"Four independently authored roles required");
    const auto& base=*document.scene.characterAssets[static_cast<std::size_t>(actors[0]->characterAsset)],action=*document.scene.characterAssets[static_cast<std::size_t>(actors[2]->characterAsset)];
    check(base.geometryHash==action.geometryHash&&base.contentHash!=action.contentHash,"Motion variants must share immutable geometry independently of clip data");
    engine::Game game;game.initialize(document.scene);game.setCharacter(document.scene.characterAssets[static_cast<std::size_t>(actors[0]->characterAsset)],false);
    for(unsigned i=0;i<60;++i)game.update({},1.f/120.f);
    const auto poses=game.renderCharacters();check(poses.size()==4,"Scene characters must replace procedural geometry");
    const auto& idle=named(poses,actors[1]->id);const auto& moving=named(poses,actors[2]->id);
    check(idle.clip=="idle"&&near(idle.morphWeights[0],.35f),"Per-instance speed and offset did not drive independent morph curves");
    check(moving.clip=="vendor_Raise"&&near(moving.pose.translation[0][0],.25f)&&near(moving.morphWeights[0],.2f),"Paused action and preserved root motion must survive native playback");
    check(poseHash(idle)!=poseHash(moving),"Independent clips must not share a palette");
    check(moving.materials!=idle.materials,"Source material slots must resolve per package");
    for(const auto& actor:actors)for(const auto& pose:game.renderPoses())check(!near(pose.position.x,actor->position.x)||!near(pose.scale.y,actor->scale.y),"Character collision proxy was rendered as an extra box");
    const auto snapshot=game.snapshot();const auto digest=gameHash(game);for(unsigned i=0;i<30;++i)game.update({},1.f/120.f);game.restore(snapshot);
    check(gameHash(game)==digest,"Character snapshot did not restore simulation state");const auto restored=game.renderCharacters();for(const auto& pose:poses)check(poseHash(pose)==poseHash(named(restored,pose.entityId)),"Derived character clocks did not restore from fixed ticks");
    const auto handle=game.gameplayRuntime().world.find(actors[1]->id);auto render=*game.gameplayRuntime().world.get(handle,"renderable");render["enabled"]=false;game.gameplayRuntime().world.set(handle,"renderable",render);
    check(game.renderCharacters().size()==3,"Disabled renderable character still draws");render["enabled"]=true;game.gameplayRuntime().world.set(handle,"renderable",render);check(game.renderCharacters().size()==4,"Reenabled character did not return");
    auto offsetScene=document.scene;offsetScene.entities[2].character.scale={2,2,2};offsetScene.entities[2].character.offset={1,0,0};offsetScene.entities[2].character.yaw=1.57079632679f;offsetScene.entities[2].components["renderable"]={{"offset",{0,2,0}}};
    engine::Game offsetGame;offsetGame.initialize(offsetScene);const auto offsetPoses=offsetGame.renderCharacters();const auto& offsetPose=named(offsetPoses,actors[1]->id);
    check(near(offsetPose.position.x,actors[1]->position.x)&&near(offsetPose.position.y,2)&&near(offsetPose.position.z,2),"Character scale, local offset and gameplay renderable offset must compose once");
    auto changed=document.scene;changed.entities[2].character.authoring["timeOffset"]=.9f;engine::Game other;other.initialize(changed);other.setCharacter(changed.characterAssets[static_cast<std::size_t>(actors[0]->characterAsset)],false);
    bool rejected=false;try{other.restore(snapshot);}catch(const std::exception&){rejected=true;}check(rejected,"Different character authoring must reject a snapshot");
    nlohmann::json data;std::ifstream(file)>>data;const auto scratch=file.parent_path()/"instances-invalid.scene.json";
    const auto originalId=document.scene.entities.front().id;const auto originalAsset=document.scene.characterAssets.front();
    auto reject=[&](nlohmann::json bad,const char* message){std::ofstream(scratch)<<bad.dump();check(!host::loadScene(scratch,document,error),message);check(document.scene.entities.front().id==originalId&&document.scene.characterAssets.front()==originalAsset,"Failed scene load changed the active character scene");};
    auto bad=data;bad["entities"][0]["character"]["sha256"]=std::string(64,'0');reject(bad,"Tampered character header accepted");
    bad=data;bad["entities"][0]["character"]["binarySha256"]=std::string(64,'0');reject(bad,"Tampered character binary accepted");
    bad=data;bad["entities"][0]["character"]["clip"]="absent";reject(bad,"Missing clip accepted");
    bad=data;bad["entities"][0]["character"]["materials"]["absent"]="m0";reject(bad,"Unknown source material slot accepted");
    bad=data;bad["entities"][0]["character"]["file"]="../outside.json";reject(bad,"Escaped character path accepted");
    bad=data;bad["entities"][0]["character"]["speed"]=2;reject(bad,"Locomotion silently ignored timing override");
    bad=data;for(unsigned i=0;i<61;++i){auto actor=data["entities"][2];char uuid[40];std::snprintf(uuid,sizeof(uuid),"a1e00000-0000-4000-8000-%012u",100+i);actor["id"]=uuid;bad["entities"].push_back(actor);}reject(bad,"Character instance budget accepted 65 roles");
    std::cout<<"{\"success\":true,\"checks\":"<<checks<<",\"scope\":\"scene character instances, independent clocks, immutable sharing, snapshots and atomic rejection\"}\n";return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
