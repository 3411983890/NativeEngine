#include "../host/scene_loader.hpp"
#include "../engine/native/animation.hpp"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
int main(int argc,char** argv){unsigned checks=0;const auto check=[&](bool value,const std::string& message){++checks;if(!value)throw std::runtime_error(message);};try{
    check(argc>=4,"Expected colored scene, colored character and legacy character");host::SceneDocument scene;std::string error;check(host::loadScene(argv[1],scene,error),error);
    check(scene.scene.materials[1].unlit,"Native unlit material flag");check(scene.scene.importedMeshes.size()==1,"Colored static mesh");const auto& mesh=scene.scene.importedMeshes[0];check(mesh.vertices.size()==8&&mesh.hasTextureUvs,"Colored back faces and slot UV layout");
    check(mesh.vertices[0].color[0]==0&&mesh.vertices[0].color[1]==1&&std::abs(mesh.vertices[0].color[2]-.8f)<.00001,"Linear source color survives native loading");check(mesh.vertices[4].color[2]==mesh.vertices[0].color[2],"Back-face color preserved");
    const auto identity=engine::gameplay::sceneIdentity(scene.scene);auto lit=scene.scene;lit.materials[1].unlit=false;check(engine::gameplay::sceneIdentity(lit)!=identity,"Unlit participates in snapshot identity");
    engine::Game game;game.initialize(scene.scene);game.update({},1.f/60.f);const auto snapshot=game.snapshot();game.update({},1.f/60.f);game.restore(snapshot);check(game.snapshot()==snapshot,"Colored unlit scene snapshot round trip");
    const auto asset=engine::animation::loadAsset(argv[2]),legacy=engine::animation::loadAsset(argv[3]);check(asset->hasVertexColors&&asset->hasTextureUvs,"Character combined color and UV sidecars");check(!legacy->hasVertexColors,"Legacy character has no color sidecar");
    bool white=true;for(const auto& v:legacy->vertices)for(float c:v.color)white &= c==1;check(white,"Legacy vertices default to white");
    check(asset->vertices[0].color[0]==0&&asset->vertices[0].color[1]==1,"Character color survives skin merge");check(!asset->morphs.empty(),"Colors preserve authored morphs");
    nlohmann::json header;std::ifstream(argv[2])>>header;std::ifstream file(std::filesystem::path(argv[2]).parent_path()/"animation.bin",std::ios::binary);const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
    const auto load=[&](nlohmann::json data,const std::vector<std::uint8_t>& binary){engine::determinism::Hasher hash;for(auto b:binary)hash.u8(b);data["binaryHash"]=hash.hex();const auto text=data.dump();return engine::animation::loadAssetData({text.begin(),text.end()},binary);};
    const auto reject=[&](const nlohmann::json& data,const std::vector<std::uint8_t>& binary){bool rejected=false;try{load(data,binary);}catch(const engine::animation::Error&){rejected=true;}check(rejected,"Invalid color section rejected");};
    auto bad=header;bad["mesh"].erase("colorOffset");reject(bad,bytes);bad=header;bad["mesh"]["colorBytes"]=0;reject(bad,bytes);bad=header;bad["mesh"]["colorOffset"]=header["mesh"]["textureUvOffset"];reject(bad,bytes);bad=header;bad["mesh"]["indexOffset"]=header["mesh"]["colorOffset"];reject(bad,bytes);
    const std::size_t colorAt=header["mesh"]["colorOffset"];for(float value:{-.1f,1.1f,std::nanf("")}){auto corrupt=bytes;std::memcpy(corrupt.data()+colorAt,&value,4);reject(header,corrupt);}
    auto changed=bytes;const float color=.4f;std::memcpy(changed.data()+colorAt,&color,4);check(load(header,changed)->geometryHash!=asset->geometryHash,"Color bytes participate in shared geometry identity");
    for(int i=4;i<argc;i++){check(!host::loadScene(argv[i],scene,error),"Invalid colored material/mesh rejected");check(engine::gameplay::sceneIdentity(scene.scene)==identity,"Colored scene rejection is atomic");}
    std::cout<<"{\"success\":true,\"checks\":"<<checks<<"}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
