#include "../host/scene_loader.hpp"
#include "../engine/native/animation.hpp"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
int main(int argc,char** argv){unsigned checks=0;const auto check=[&](bool value,const std::string& message){++checks;if(!value)throw std::runtime_error(message);};try{
    check(argc>=4,"Expected tangent scene, tangent character and legacy character");host::SceneDocument scene;std::string error;check(host::loadScene(argv[1],scene,error),error);check(!scene.scene.importedMeshes.empty(),"Imported tangent geometry");
    for(const auto& mesh:scene.scene.importedMeshes){check(mesh.hasTextureUvs,"Tangent layout retains per-slot UVs");const auto count=mesh.vertices.size()/2;for(std::size_t i=0;i<count;i++){const auto& a=mesh.vertices[i];const auto& b=mesh.vertices[i+count];check(a.tangent[3]==-b.tangent[3]&&std::abs(a.tangent[3])==1,"Back-face handedness");float dot=0,length=0;for(unsigned k=0;k<3;k++){dot+=a.normal[k]*a.tangent[k];length+=a.tangent[k]*a.tangent[k];}check(std::abs(dot)<.0001f&&std::abs(length-1)<.0001f,"Native orthonormal frame");}}
    const auto identity=engine::gameplay::sceneIdentity(scene.scene);engine::Game game;game.initialize(scene.scene);game.update({},1.f/60.f);const auto snapshot=game.snapshot();game.update({},1.f/60.f);game.restore(snapshot);check(game.snapshot()==snapshot,"Tangent scene snapshot round trip");
    const auto asset=engine::animation::loadAsset(argv[2]),legacy=engine::animation::loadAsset(argv[3]);check(asset->hasTangents&&asset->hasTextureUvs&&asset->hasVertexColors,"Tangent, color and UV sidecars");check(!legacy->hasTangents,"Old character omits tangent sidecar");bool zero=true;for(const auto& v:legacy->vertices)for(float f:v.tangent)zero &= f==0;check(zero,"Legacy tangent defaults select derivative basis");
    bool authored=false;for(const auto& v:asset->vertices)authored |= std::abs(v.tangent[3])==1;check(authored,"Source tangent reached native skin vertices");check(!asset->morphs.empty(),"Morphs retained with tangents");
    nlohmann::json header;std::ifstream(argv[2])>>header;std::ifstream file(std::filesystem::path(argv[2]).parent_path()/"animation.bin",std::ios::binary);const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
    const auto load=[&](nlohmann::json data,const std::vector<std::uint8_t>& binary){engine::determinism::Hasher hash;for(auto b:binary)hash.u8(b);data["binaryHash"]=hash.hex();const auto text=data.dump();return engine::animation::loadAssetData({text.begin(),text.end()},binary);};
    const auto reject=[&](const nlohmann::json& data,const std::vector<std::uint8_t>& binary){bool rejected=false;try{load(data,binary);}catch(const engine::animation::Error&){rejected=true;}check(rejected,"Invalid tangent section rejected");};
    auto bad=header;bad["mesh"].erase("tangentOffset");reject(bad,bytes);bad=header;bad["mesh"]["tangentBytes"]=0;reject(bad,bytes);bad=header;bad["mesh"]["tangentOffset"]=0;reject(bad,bytes);bad=header;bad["mesh"]["indexOffset"]=header["mesh"]["tangentOffset"];reject(bad,bytes);
    const std::size_t at=header["mesh"]["tangentOffset"];for(const auto& edit:std::initializer_list<std::pair<unsigned,float>>{{3,.5f},{0,2.f},{0,std::nanf("")},{3,0.f}}){auto corrupt=bytes;std::memcpy(corrupt.data()+at+edit.first*4,&edit.second,4);reject(header,corrupt);}
    auto changed=bytes;float sign;std::memcpy(&sign,changed.data()+at+12,4);sign=-sign;std::memcpy(changed.data()+at+12,&sign,4);check(load(header,changed)->geometryHash!=asset->geometryHash,"Tangent data participates in geometry identity");
    for(int i=4;i<argc;i++){check(!host::loadScene(argv[i],scene,error),"Invalid tangent mesh rejected");check(engine::gameplay::sceneIdentity(scene.scene)==identity,"Invalid tangent scene leaves current scene intact");}
    std::cout<<"{\"success\":true,\"checks\":"<<checks<<"}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
