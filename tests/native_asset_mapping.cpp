#include "../host/scene_loader.hpp"
#include <iostream>
#include <cmath>
#include <set>
int main(int argc,char** argv){unsigned checks=0;const auto check=[&](bool v,const std::string& message){++checks;if(!v)throw std::runtime_error(message);};try{
    check(argc>=3,"mapped scene and sampler atlas required");host::SceneDocument doc,atlas;std::string error;check(host::loadScene(argv[1],doc,error),error);const auto& material=doc.scene.materials[1];const auto& mapping=material.textureMappings[0];check(material.hasTextureMappings,"native mapping flag");check(std::abs(mapping.row0[0])<1e-5&&std::abs(mapping.row0[1]-.5f)<1e-5&&std::abs(mapping.row0[2]-.25f)<1e-5,"native UV first row");check(std::abs(mapping.row1[0]-2)<1e-5&&std::abs(mapping.row1[1])<1e-5&&std::abs(mapping.row1[2]-.75f)<1e-5,"native UV second row");check(mapping.sampler==71,"native clamp/mirror trilinear/nearest sampler index");
    check(host::loadScene(argv[2],atlas,error),error);std::set<unsigned> indices;for(const auto& material:atlas.scene.materials)if(material.hasTextureMappings)indices.insert(material.textureMappings[3].sampler?material.textureMappings[3].sampler:material.textureMappings[0].sampler);check(indices.size()==108&&*indices.begin()==1&&*indices.rbegin()==108,"all sampler combinations load independently");
    const auto identity=engine::gameplay::sceneIdentity(doc.scene);for(unsigned field=0;field<3;field++){auto changed=doc.scene;auto& material=changed.materials[1];if(field==0)material.textureMappings[0].row0[2]+=.1f;else if(field==1)material.textureMappings[0].sampler=1;else material.hasTextureMappings=false;check(engine::gameplay::sceneIdentity(changed)!=identity,"sampling participates in scene save identity");}
    engine::Game game;game.initialize(doc.scene);game.update({},1.f/60.f);const auto snapshot=game.snapshot();game.update({},1.f/60.f);game.restore(snapshot);check(game.snapshot()==snapshot,"mapped scene restores snapshot");
    for(int i=3;i<argc;i++){check(!host::loadScene(argv[i],doc,error),"invalid mapping must reject");check(engine::gameplay::sceneIdentity(doc.scene)==identity,"invalid mapping is atomic");}
    std::cout<<"{\"success\":true,\"checks\":"<<checks<<"}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
