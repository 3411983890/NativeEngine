#include "../host/scene_loader.hpp"
#include "../third_party/json.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <windows.h>

static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
int main() {
    try {
        host::SceneDocument scene;
        std::string error;
        check(host::loadScene(".screen/courtyard.scene.json",scene,error),error.c_str());
        const auto count=scene.scene.entities.size();
        const auto name=scene.name;
        check(scene.scene.lights.size()==2,"canonical scene must expose two lights");
        check(scene.scene.entities[15].hasLight && scene.scene.entities[15].hasMaterial,
              "canonical light must carry light and material data");
        check(!host::loadScene("assets/missing.scene.json",scene,error),"missing file must fail");
        check(scene.scene.entities.size()==count && scene.name==name,"failure changed active scene");
        nlohmann::json doc;
        std::ifstream(".screen/courtyard.scene.json") >> doc;
        const auto temp=std::filesystem::temp_directory_path() / ("nativegpu-scene-test-" + std::to_string(GetCurrentProcessId()) + ".json");
        auto bad=doc;
        bad["entities"][0]["scale"][0]=0;
        std::ofstream(temp) << bad.dump();
        check(!host::loadScene(temp,scene,error),"zero scale must fail");
        bad=doc;
        bad["entities"][1]["id"]="A1E00000-0000-4000-8000-000000000010";
        std::ofstream(temp) << bad.dump();
        check(!host::loadScene(temp,scene,error),"case-insensitive duplicate UUID must fail");
        bad=doc;
        bad["settings"]["gravity"]="NaN";
        std::ofstream(temp) << bad.dump();
        check(!host::loadScene(temp,scene,error),"invalid gravity must fail");
        bad=doc;
        bad["unexpected"]=true;
        std::ofstream(temp) << bad.dump();
        check(!host::loadScene(temp,scene,error),"unknown top-level field must fail");
        bad=doc;
        bad["entities"].erase(0);
        std::ofstream(temp) << bad.dump();
        check(!host::loadScene(temp,scene,error),"missing ground must fail");
        bad=doc;
        bad["name"]=std::string(129,'x');
        std::ofstream(temp) << bad.dump();
        check(!host::loadScene(temp,scene,error),"overlong name must fail");
        bad=doc;
        bad["name"]="   ";
        std::ofstream(temp) << bad.dump();
        check(!host::loadScene(temp,scene,error),"blank name must fail");
        bad=doc;
        bad["entities"][0]["position"][0]=100001;
        std::ofstream(temp) << bad.dump();
        check(!host::loadScene(temp,scene,error),"position outside range must fail");
        bad=doc;
        bad["entities"][0]["extra"]=true;
        std::ofstream(temp) << bad.dump();
        check(!host::loadScene(temp,scene,error),"unknown entity field must fail");
        bad=doc;
        bad["entities"][15].erase("light");
        std::ofstream(temp) << bad.dump();
        check(!host::loadScene(temp,scene,error),"missing light object must fail");
        bad=doc;
        bad["entities"][15]["light"]["intensity"]=0;
        std::ofstream(temp) << bad.dump();
        check(!host::loadScene(temp,scene,error),"zero light intensity must fail");
        bad=doc;
        bad["entities"][0]["material"]["pattern"]="glow";
        std::ofstream(temp) << bad.dump();
        check(!host::loadScene(temp,scene,error),"unknown material pattern must fail");
        bad=doc;
        bad["entities"][0]["material"]["extra"]=true;
        std::ofstream(temp) << bad.dump();
        check(!host::loadScene(temp,scene,error),"unknown material field must fail");
        std::ofstream(temp) << "{\"broken\":";
        check(!host::loadScene(temp,scene,error),"malformed JSON must fail");
        auto quality=doc;
        quality["lighting"]={{"quality",2},{"maxShadowLights",2},{"ibl",true},{"ddgi",false},{"temporal",false}};
        quality["entities"][15]["light"]["castSurfaceShadow"]=false;
        quality["entities"][15]["light"]["castVolumetricShadow"]=true;
        quality["entities"][15]["light"]["volumetricIntensity"]=0.25;
        std::ofstream(temp)<<quality.dump();
        host::SceneDocument qualityScene;
        check(host::loadScene(temp,qualityScene,error),error.c_str());
        check(qualityScene.scene.lighting.quality==2 && qualityScene.scene.lighting.maxShadowLights==2 &&
              !qualityScene.scene.lighting.ddgi && !qualityScene.scene.lighting.temporal,"lighting controls not loaded");
        check(!qualityScene.scene.lights[0].castSurfaceShadow && qualityScene.scene.lights[0].castVolumetricShadow &&
              qualityScene.scene.lights[0].volumetricIntensity==0.25f,"independent light shadow controls not loaded");
        quality["entities"][15]["light"]["range"]=0.001;
        quality["lighting"]["exposure"]=0.01;
        quality["lighting"]["fogVolumes"]={{{"center",{0,0,0}},{"extent",{0.001,1,1}},{"density",0.03}}};
        std::ofstream(temp)<<quality.dump();
        check(host::loadScene(temp,qualityScene,error),"decimal minimum lighting bounds must be accepted");
        for(const auto& invalid: {nlohmann::json{{"quality",0.5}},nlohmann::json{{"maxShadowLights",5}},
                                nlohmann::json{{"ddgi",1}},nlohmann::json{{"keyDirection",{0,0,0}}}}) {
            quality["lighting"]=invalid;std::ofstream(temp)<<quality.dump();
            check(!host::loadScene(temp,qualityScene,error),"invalid lighting controls accepted");
        }
        auto physicsDoc=doc;
        physicsDoc["entities"][7]["physics"]={{"mode","dynamic"},{"mass",4.0},{"friction",0.7},{"restitution",0.12}};
        physicsDoc["entities"][7]["destructible"]={{"enabled",true},{"strength",42.5},{"fragments",12},
                                                   {"debrisLifetime",20},{"structural",true},{"seed",101}};
        std::ofstream(temp)<<physicsDoc.dump();
        host::SceneDocument physicsScene;
        check(host::loadScene(temp,physicsScene,error),error.c_str());
        const auto& physical=physicsScene.scene.entities[7];
        check(physical.hasPhysics&&physical.dynamicBody&&physical.physicsMass==4.0f &&
              physical.friction==0.7f&&physical.restitution==0.12f,"physics fields not loaded");
        check(physical.destructible&&physical.strength==42.5f&&physical.fragments==12&&
              physical.debrisLifetime==20.0f&&physical.structural&&physical.fractureSeed==101,
              "destructible fields not loaded");
        physicsDoc["entities"][0]["physics"]={{"mode","dynamic"}};
        std::ofstream(temp)<<physicsDoc.dump();
        check(!host::loadScene(temp,physicsScene,error),"dynamic ground must fail");
        physicsDoc=doc;
        physicsDoc["entities"][7]["destructible"]={{"enabled",true},{"strength",40},{"fragments",3}};
        std::ofstream(temp)<<physicsDoc.dump();
        check(!host::loadScene(temp,physicsScene,error),"too few fragments must fail");
        physicsDoc["entities"][7]["destructible"]["fragments"]=12;
        physicsDoc["entities"][7]["collider"]=false;
        std::ofstream(temp)<<physicsDoc.dump();
        check(!host::loadScene(temp,physicsScene,error),"destructible without collider must fail");
        if(std::filesystem::exists(".screen/character-rig.scene.json")){
            host::SceneDocument rigScene;
            check(host::loadScene(".screen/character-rig.scene.json",rigScene,error),error.c_str());
            const engine::Entity* player=nullptr;
            for(const auto& entity:rigScene.scene.entities)if(entity.kind=="player")player=&entity;
            check(player!=nullptr&&!player->character.rig.empty(),"rig scene must carry a player rig override");
            check(player->character.rig.value("turnCooldown",0.f)==0.15f,"scene rig override must load");
            nlohmann::json rigDoc;
            std::ifstream(".screen/character-rig.scene.json")>>rigDoc;
            for(auto& entity:rigDoc["entities"])if(entity.value("kind",std::string())=="player")entity["character"]["rig"]={{"turnThreshold",-1}};
            std::ofstream(temp)<<rigDoc.dump();
            check(!host::loadScene(temp,rigScene,error),"out-of-range rig override must fail");
            for(auto& entity:rigDoc["entities"])if(entity.value("kind",std::string())=="player")entity["character"]["rig"]={{"unknown",1}};
            std::ofstream(temp)<<rigDoc.dump();
            check(!host::loadScene(temp,rigScene,error),"unknown rig override key must fail");
        }
        std::filesystem::remove(temp);
        check(scene.scene.entities.size()==count && scene.name==name,"invalid inputs changed active scene");
        std::cout << "Native scene loader: legacy, lighting, physics and destruction contracts passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
