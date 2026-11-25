#include "../host/scene_loader.hpp"
#include <iostream>
#include <cmath>
int main(int argc,char** argv){unsigned checks=0;const auto check=[&](bool ok,const std::string& message){++checks;if(!ok)throw std::runtime_error(message);};try{
    check(argc>=2,"AO scene argument");host::SceneDocument doc;std::string error;check(host::loadScene(argv[1],doc,error),error);const auto& material=doc.scene.materials[1];check(material.occlusion=="m1","independent AO texture reference");check(std::abs(material.occlusionStrength-.6f)<1e-6,"independent AO strength");check(material.textureMappings[4].sampler==71,"AO uses fifth mapping and sampler");check(std::abs(material.textureMappings[4].row0[2]-.25f)<1e-6,"AO transform offset");
    const auto identity=engine::gameplay::sceneIdentity(doc.scene);for(unsigned field=0;field<3;field++){auto changed=doc.scene;if(field==0)changed.materials[1].occlusion="other";else if(field==1)changed.materials[1].occlusionStrength=.3f;else changed.materials[1].textureMappings[4].sampler=1;check(engine::gameplay::sceneIdentity(changed)!=identity,"AO participates in scene identity");}
    engine::Game game;game.initialize(doc.scene);game.update({},1.f/60.f);const auto snapshot=game.snapshot();game.update({},1.f/60.f);game.restore(snapshot);check(game.snapshot()==snapshot,"AO scene snapshot restores");auto changed=doc.scene;changed.materials[1].occlusionStrength=0;engine::Game other;other.initialize(changed);bool rejected=false;try{other.restore(snapshot);}catch(...){rejected=true;}check(rejected,"AO change rejects old save");
    for(int i=2;i<argc;i++){check(!host::loadScene(argv[i],doc,error),"invalid AO scene rejects");check(engine::gameplay::sceneIdentity(doc.scene)==identity,"AO scene rejection is atomic");}
    std::cout<<"{\"success\":true,\"checks\":"<<checks<<"}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
