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
        check(host::loadScene("assets/courtyard.scene.json",scene,error),error.c_str());
        const auto count=scene.scene.entities.size();
        const auto name=scene.name;
        check(scene.scene.lights.size()==2,"canonical scene must expose two lights");
        check(scene.scene.entities[15].hasLight && scene.scene.entities[15].hasMaterial,
              "canonical light must carry light and material data");
        check(!host::loadScene("assets/missing.scene.json",scene,error),"missing file must fail");
        check(scene.scene.entities.size()==count && scene.name==name,"failure changed active scene");
        nlohmann::json doc;
        std::ifstream("assets/courtyard.scene.json") >> doc;
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
        std::filesystem::remove(temp);
        check(scene.scene.entities.size()==count && scene.name==name,"invalid inputs changed active scene");
        std::cout << "Native scene loader: 19 checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
