#include "../host/scene_loader.hpp"
#include "../engine/native/animation.hpp"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>

int main(int argc,char** argv){
    unsigned checks=0;
    const auto check=[&](bool value,const std::string& message){++checks;if(!value)throw std::runtime_error(message);};
    try{
        check(argc>=5,"Expected static scene, character header, legacy character and UV expectations");
        host::SceneDocument scene;std::string error;check(host::loadScene(argv[1],scene,error),error);
        nlohmann::json expected;std::ifstream(argv[4])>>expected;
        check(scene.scene.importedMeshes.size()==1,"Expected one multi-UV mesh");
        const auto& mesh=scene.scene.importedMeshes[0];check(mesh.hasTextureUvs,"Multi-UV static flag");
        const auto count=expected["static"][0].size();
        for(unsigned slot=0;slot<5;slot++){
            bool same=true;for(std::size_t v=0;v<mesh.vertices.size();v++)for(unsigned k=0;k<2;k++){
                const auto value=slot==0?mesh.vertices[v].uv[k]:mesh.vertices[v].textureUvs[slot-1][k];
                same &= std::abs(value-expected["static"][slot][v%count][k].get<float>())<.00001f;
            }check(same,"Static source UV selection for slot "+std::to_string(slot));
        }
        auto character=engine::animation::loadAsset(argv[2]),legacy=engine::animation::loadAsset(argv[3]);
        check(character->hasTextureUvs&&!legacy->hasTextureUvs,"Optional sidecar keeps legacy character readable");
        check(character->geometryHash!=legacy->geometryHash,"UV data participates in geometry identity");
        const auto characterCount=expected["character"][0].size();
        for(unsigned slot=0;slot<5;slot++){
            bool same=true;for(std::size_t v=0;v<character->vertices.size();v++)for(unsigned k=0;k<2;k++){
                const auto value=slot==0?character->vertices[v].uv[k]:character->vertices[v].textureUvs[slot-1][k];
                same &= std::abs(value-expected["character"][slot][v%characterCount][k].get<float>())<.00001f;
            }check(same,"Character source UV selection for slot "+std::to_string(slot));
        }
        engine::animation::Controller controller;engine::animation::ControllerConfig config;config.clips=character->controllerClips;
        config.motionMatching=config.proceduralRig=config.muscles=config.softTissue=false;controller.initialize(character,config);
        engine::animation::Intent intent;intent.grounded=true;for(unsigned i=0;i<120;i++){intent.tick=i+1;controller.tick(intent);}
        check(!character->skeleton.palette(controller.pose()).empty(),"Multi-UV character executes skeletal controller");
        nlohmann::json header;std::ifstream(argv[2])>>header;std::ifstream binaryFile(std::filesystem::path(argv[2]).parent_path()/"animation.bin",std::ios::binary);
        const std::vector<std::uint8_t> binary((std::istreambuf_iterator<char>(binaryFile)),{});
        const auto reject=[&](nlohmann::json data,std::vector<std::uint8_t> bytes){
            engine::determinism::Hasher hash;for(auto b:bytes)hash.u8(b);data["binaryHash"]=hash.hex();
            const auto text=data.dump();bool rejected=false;try{engine::animation::loadAssetData({text.begin(),text.end()},bytes);}catch(const engine::animation::Error&){rejected=true;}check(rejected,"Malformed UV sidecar rejected");
        };
        auto bad=header;bad["mesh"]["textureUvOffset"]=1;reject(bad,binary);
        bad=header;bad["mesh"]["textureUvBytes"]=0;reject(bad,binary);
        bad=header;bad["mesh"].erase("textureUvOffset");reject(bad,binary);
        auto corrupt=binary;const std::uint32_t nan=0x7fc00000u;const std::size_t at=header["mesh"]["textureUvOffset"];std::memcpy(corrupt.data()+at,&nan,4);reject(header,corrupt);
        const auto identity=engine::gameplay::sceneIdentity(scene.scene);
        for(int i=5;i<argc;i++){check(!host::loadScene(argv[i],scene,error),"Invalid multi-UV mesh rejected");check(engine::gameplay::sceneIdentity(scene.scene)==identity,"Mesh rejection is atomic");}
        std::cout<<"{\"success\":true,\"checks\":"<<checks<<"}\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
