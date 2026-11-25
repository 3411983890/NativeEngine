#include "../host/scene_loader.hpp"
#include "../engine/native/assets/file_path.hpp"
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv){unsigned checks=0;auto check=[&](bool ok,const std::string& message){++checks;if(!ok)throw std::runtime_error(message);};try{
    check(argc>=6,"Expected updated rigid, character, both packaged scenes and invalid scenes");host::SceneDocument rigid,character,packaged,packagedCharacter;std::string error;
    const auto native=engine::assets::nativeFilePath(std::filesystem::u8path(argv[1]));check(engine::assets::nativeFilePath(native)==native,"Native I/O path conversion is idempotent");
    check(host::loadScene(argv[1],rigid,error),error);check(!rigid.scene.importedMeshes.empty(),"Updated rigid mesh dependencies load");
    check(host::loadScene(argv[2],character,error),error);check(character.scene.characterAssets.size()==1,"Updated character with source provenance loads");
    check(host::loadScene(argv[3],packaged,error),error);check(packaged.scene.importedMeshes.size()==rigid.scene.importedMeshes.size(),"Packaged updated geometry loads");
    check(host::loadScene(argv[4],packagedCharacter,error),error);
    for(const auto& entity:packagedCharacter.scene.entities)if(!entity.character.file.empty()){
        const auto header=std::filesystem::u8path(argv[4]).parent_path()/std::filesystem::u8path(entity.character.file);
        check(header.u8string().size()>260,"Fixture must exercise a real long character path");
        check(engine::animation::loadAsset(header)->vertices.size()==packagedCharacter.scene.characterAssets.front()->vertices.size(),"Direct character loading supports long paths");
    }
    engine::Game game;game.initialize(rigid.scene);for(int i=0;i<60;++i)game.update({},1.f/60.f);const auto state=game.snapshot();
    const auto identity=engine::gameplay::sceneIdentity(rigid.scene);
    for(int i=5;i<argc;++i){check(!host::loadScene(argv[i],rigid,error),"Malformed imported provenance or escaped dependencies must reject");check(engine::gameplay::sceneIdentity(rigid.scene)==identity,"Invalid provenance cannot partially replace the loaded document");}
    game.update({},1.f/60.f);game.restore(state);check(game.snapshot()==state,"Updated scene snapshot restores");
    std::cout<<"{\"success\":true,\"checks\":"<<checks<<"}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
