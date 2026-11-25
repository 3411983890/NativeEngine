#include "../host/scene_loader.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
int main(int argc,char** argv){unsigned checks=0;const auto check=[&](bool value,const std::string& message){++checks;if(!value)throw std::runtime_error(message);};try{
    check(argc==2,"Expected BLEND scene");host::SceneDocument scene;std::string error;check(host::loadScene(argv[1],scene,error),error);
    check(scene.scene.materials.size()==3,"Three native materials");const auto& glass=scene.scene.materials[1];check(glass.alphaBlended&&!glass.alphaMasked,"Native BLEND flag");check(glass.opacity==.5f,"Native floating alpha");
    check(!scene.scene.materials[0].alphaBlended&&scene.scene.materials[0].opacity==1,"Legacy opaque defaults");const auto identity=engine::gameplay::sceneIdentity(scene.scene);
    auto changed=scene.scene;changed.materials[1].alphaBlended=false;check(engine::gameplay::sceneIdentity(changed)!=identity,"BLEND participates in snapshot identity");changed=scene.scene;changed.materials[1].opacity=.25f;check(engine::gameplay::sceneIdentity(changed)!=identity,"Opacity participates in snapshot identity");
    engine::Game game;game.initialize(scene.scene);game.update({},1.f/60.f);const auto snapshot=game.snapshot();game.update({},1.f/60.f);game.restore(snapshot);check(game.snapshot()==snapshot,"Transparent scene snapshot round trip");
    nlohmann::json source;std::ifstream(argv[1])>>source;const auto file=std::filesystem::path(argv[1]).parent_path()/"invalid-alpha.scene.json";
    for(const nlohmann::json& value:{nlohmann::json(-.1),nlohmann::json(1.1),nlohmann::json(nullptr),nlohmann::json("0.5")}){auto bad=source;bad["materials"]["m1"]["opacity"]=value;std::ofstream(file)<<bad.dump();check(!host::loadScene(file,scene,error),"Invalid opacity rejected");check(engine::gameplay::sceneIdentity(scene.scene)==identity,"Opacity rejection is atomic");}
    auto bad=source;bad["materials"]["m1"]["alphaMode"]="INVALID";std::ofstream(file)<<bad.dump();check(!host::loadScene(file,scene,error),"Invalid alpha mode rejected");check(engine::gameplay::sceneIdentity(scene.scene)==identity,"Alpha mode rejection is atomic");std::filesystem::remove(file);
    std::cout<<"{\"success\":true,\"checks\":"<<checks<<"}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
