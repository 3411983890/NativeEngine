#include "../host/scene_loader.hpp"
#include "../engine/native/game.hpp"
#include <fstream>
#include <iostream>
int main(int argc,char** argv){unsigned checks=0;const auto check=[&](bool v,const std::string& m){++checks;if(!v)throw std::runtime_error(m);};try{
    check(argc>=2,"Expected IOR scene");host::SceneDocument doc;std::string error;check(host::loadScene(argv[1],doc,error),error);check(doc.scene.materials.size()>=2,"Imported materials loaded");check(std::abs(doc.scene.materials[1].ior-2.42f)<1e-6f,"Source IOR retained");
    const auto source=doc.scene;auto legacy=source;for(auto& material:legacy.materials)material.ior=1.5f;const auto legacyHash=engine::gameplay::sceneIdentity(legacy),identity=engine::gameplay::sceneIdentity(source);check(legacyHash!=identity,"IOR changes content identity");engine::Game game;game.initialize(source);game.update({},1.f/60);const auto snapshot=game.snapshot();game.update({},1.f/60);game.restore(snapshot);check(game.snapshot()==snapshot,"IOR scene restores exact snapshot");
    engine::Game different;different.initialize(legacy);bool rejected=false;try{different.restore(snapshot);}catch(const std::exception&){rejected=true;}check(rejected,"Different IOR rejects snapshot");
    check(engine::materialDielectricF0(0)==1,"IOR zero requires full Fresnel");check(engine::materialDielectricF0(1)==0,"Air normal-incidence F0 is zero");check(std::abs(engine::materialDielectricF0(1.5f)-.04f)<1e-7,"Legacy dielectric F0 remains .04");check(std::isfinite(engine::materialDielectricF0(std::numeric_limits<float>::max())),"Large finite IOR avoids overflow");
    for(double value:{-1.,.5,.999999999999,1e39,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})check(!engine::validMaterialIor(value),"Invalid raw IOR rejected before Float32 rounding");for(double value:{0.,1.,1.33,1.5,2.42,static_cast<double>(std::numeric_limits<float>::max())})check(engine::validMaterialIor(value),"Valid IOR accepted");
    for(int i=2;i<argc;i++){check(!host::loadScene(argv[i],doc,error),"Malformed IOR scene rejected");check(engine::gameplay::sceneIdentity(doc.scene)==identity,"Failed load leaves current IOR scene unchanged");}
    std::cout<<"{\"success\":true,\"checks\":"<<checks<<"}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
