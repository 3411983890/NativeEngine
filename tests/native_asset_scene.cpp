#include "../host/scene_loader.hpp"
#include <iostream>
#include <stdexcept>
using namespace engine;
int main(int argc,char** argv){unsigned checks=0;const auto check=[&](bool ok,const std::string& message){++checks;if(!ok)throw std::runtime_error(message);};try{
    check(argc==2,"scene argument");host::SceneDocument doc;std::string error;check(host::loadScene(argv[1],doc,error),error);
    check(doc.scene.characterAssets.size()==1,"imported character");check(doc.audio.sounds.size()==1,"imported sound");
    Game game;game.initialize(doc.scene);for(int i=0;i<60;i++)game.update({},1.f/60.f);
    const auto& world=game.gameplayRuntime().world;bool found=false;for(const auto h:world.query("behavior"))if(const auto* renderable=world.get(h,"renderable")){const auto& offset=renderable->at("offset");if(std::abs(offset[1].get<float>()-.35f)<1.e-5f)found=true;}
    check(found,"external Lua script executes after source relocation");check(game.convexMeshes().size()>=2,"imported geometry");check(game.renderCharacters().size()==1,"imported character pose");
    check(doc.scene.voronoiPackages.size()==1&&game.bakedShardCount()==48,"relocated Voronoi data becomes native bonded shard geometry");
    const auto snapshot=game.snapshot();game.update({},1.f/60.f);game.restore(snapshot);check(game.snapshot()==snapshot,"relocated scene snapshot");
    auto invalid=snapshot;invalid["bonded_"][0]["cells"][0]["center"]["x"]=900;bool rejected=false;try{game.restore(invalid);}catch(...){rejected=true;}check(rejected&&game.snapshot()==snapshot,"corrupt baked cell restore is atomic");
    auto changed=doc.scene;changed.voronoiPackages[0].meshes[0].vertices[0].position[0]+=.1f;Game changedGame;changedGame.initialize(changed);rejected=false;try{changedGame.restore(snapshot);}catch(...){rejected=true;}check(rejected,"baked hull content participates in save identity");
    const auto wall=std::find_if(doc.scene.entities.begin(),doc.scene.entities.end(),[](const auto& e){return !e.voronoi.empty();});game.detonate(wall->position,4,20);game.update({},1.f/60.f);const auto fractured=game.snapshot();check(fractured.at("physics").at("bodies_").size()>snapshot.at("physics").at("bodies_").size(),"baked wall activates real shard bodies");
    std::vector<gameplay::Json> continuation;for(int i=0;i<30;i++){game.update({},1.f/60.f);continuation.push_back(game.snapshot());}
    Game resumed;resumed.initialize(doc.scene);resumed.restore(fractured);check(resumed.convexMeshes().size()==98&&resumed.bakedShardCount()==48,"baked render and collision geometry survives restore");for(const auto& expected:continuation){resumed.update({},1.f/60.f);const auto actual=resumed.snapshot();if(actual!=expected)std::cerr<<gameplay::Json::diff(expected,actual).dump().substr(0,6000)<<"\n";check(actual==expected,"baked hull physics continues identically after restore");}
    auto gridScene=doc.scene;gridScene.voronoiPackages.clear();for(auto& e:gridScene.entities)e.voronoi.clear();Game grid;grid.initialize(gridScene);grid.update({},1.f/60.f);const auto gridState=grid.snapshot();grid.update({},1.f/60.f);grid.restore(gridState);check(grid.snapshot()==gridState&&grid.bakedShardCount()==0,"procedural bonded grids retain snapshot compatibility");
    std::cout<<"{\"success\":true,\"checks\":"<<checks<<"}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
