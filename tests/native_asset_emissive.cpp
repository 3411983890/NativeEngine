#include "../host/scene_loader.hpp"
#include <iostream>
#include <stdexcept>
#include <cmath>
int main(int argc,char** argv){unsigned checks=0;auto check=[&](bool value,const std::string& message){++checks;if(!value)throw std::runtime_error(message);};try{
    check(argc>=3,"scene and legacy arguments");host::SceneDocument doc;std::string error;check(host::loadScene(argv[1],doc,error),error);
    check(doc.scene.materials.size()==6,"five colored emission materials plus default");
    const auto& first=doc.scene.materials[1];check(first.emissive=="m1"&&std::abs(first.emissiveFactor[0]-.4f)<1e-6&&first.emissiveFactor[1]==0&&first.emissiveFactor[2]==0,"native independent red factor");
    check(doc.scene.materials[5].emissiveStrength==4,"native strength multiplier");
    const auto identity=engine::gameplay::sceneIdentity(doc.scene);
    for(unsigned field=0;field<5;field++){auto changed=doc.scene;auto& material=changed.materials[1];if(field<3)material.emissiveFactor[field]+=.1f;else if(field==3)material.emissiveStrength=2;else material.emissive="another";check(engine::gameplay::sceneIdentity(changed)!=identity,"emission participates in scene save identity");}
    engine::Game game;game.initialize(doc.scene);game.update({},1.f/60.f);const auto snapshot=game.snapshot();game.update({},1.f/60.f);game.restore(snapshot);check(game.snapshot()==snapshot,"emission scene snapshot restores");
    auto changed=doc.scene;changed.materials[1].emissiveStrength=2;engine::Game other;other.initialize(changed);bool rejected=false;try{other.restore(snapshot);}catch(...){rejected=true;}check(rejected,"changed emission refuses old snapshot");
    for(int arg=3;arg<argc;arg++){check(!host::loadScene(argv[arg],doc,error),"invalid native emission must reject");check(engine::gameplay::sceneIdentity(doc.scene)==identity,"invalid native scene leaves active scene intact");}
    // Existing scenes have no extra emission identity fields, retaining old save IDs.
    auto legacy=doc.scene;for(auto& material:legacy.materials){material.emissive.clear();std::fill(material.emissiveFactor,material.emissiveFactor+3,1.f);material.emissiveStrength=1;}host::SceneDocument old;check(host::loadScene(argv[2],old,error),error);check(engine::gameplay::sceneIdentity(legacy)==engine::gameplay::sceneIdentity(old.scene),"explicit defaults preserve legacy material scene identity");
    std::cout<<"{\"success\":true,\"checks\":"<<checks<<"}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
