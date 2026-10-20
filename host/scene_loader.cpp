#include "scene_loader.hpp"
#include "../third_party/json.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <cmath>
#include <regex>
#include <initializer_list>

namespace host {
using nlohmann::json;
static void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
static void exactKeys(const json& value, std::initializer_list<const char*> keys,
                      const std::string& where,
                      std::initializer_list<const char*> optional = {}) {
    require(value.is_object(), where + " must be an object");
    std::set<std::string> allowed;
    for (const char* key : keys) allowed.insert(key);
    for (const char* key : optional) allowed.insert(key);
    for (auto item = value.begin(); item != value.end(); ++item) {
        require(allowed.count(item.key()) != 0,
                where + "." + item.key() + " is not supported");
    }
    for (const char* key : keys) {
        require(value.find(key) != value.end(), where + "." + key + " is required");
    }
}
// Scene names are UTF-8, so count code points rather than bytes and reject
// ASCII control characters. This mirrors the Node.js validator.
static bool text(const std::string& value) {
    if (value.empty()) return false;
    std::size_t count = 0;
    for (std::size_t i = 0; i < value.size();) {
        const unsigned char lead = static_cast<unsigned char>(value[i]);
        std::size_t width = 0;
        if (lead < 0x80) width = 1;
        else if ((lead & 0xE0) == 0xC0) width = 2;
        else if ((lead & 0xF0) == 0xE0) width = 3;
        else if ((lead & 0xF8) == 0xF0) width = 4;
        if (!width || i + width > value.size()) return false;
        if (width == 1 && (lead < 0x20 || lead == 0x7F)) return false;
        for (std::size_t offset = 1; offset < width; ++offset) {
            if ((static_cast<unsigned char>(value[i + offset]) & 0xC0) != 0x80)
                return false;
        }
        i += width;
        if (++count > 128) return false;
    }
    return value.find_first_not_of(" \t\r\n\f\v") != std::string::npos;
}
static bool uuid(const std::string& s) {
    static const std::regex pattern("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-8][0-9a-fA-F]{3}-[89aAbB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$");
    return std::regex_match(s, pattern);
}
static float number(const json& value, const std::string& name, float low, float high) {
    require(value.is_number(), name + " must be a number");
    const double n = value.get<double>();
    require(std::isfinite(n) && n >= low && n <= high, name + " outside supported range");
    return static_cast<float>(n);
}
static engine::Vec3 vector(const json& value, const std::string& name, float low, float high) {
    require(value.is_array() && value.size() == 3, name + " must contain 3 numbers");
    return {number(value[0],name,low,high),number(value[1],name,low,high),number(value[2],name,low,high)};
}
bool loadScene(const std::filesystem::path& path, SceneDocument& result, std::string& error) {
    try {
        std::error_code ec;
        auto size = std::filesystem::file_size(path, ec);
        require(!ec && size <= 16 * 1024 * 1024, "Scene is missing or exceeds 16 MiB");
        std::ifstream input(path, std::ios::binary);
        require(input.good(), "Cannot open scene");
        json data = json::parse(input);
        exactKeys(data, {"schemaVersion","projectId","name","revision","entities","settings"}, "scene");
        require(data.at("schemaVersion") == 1, "Unsupported scene schemaVersion");
        SceneDocument next;
        next.projectId = data.at("projectId").get<std::string>();
        require(uuid(next.projectId), "Invalid project UUID");
        next.name = data.at("name").get<std::string>();
        require(text(next.name), "Project name must contain 1-128 printable characters");
        require(data.at("revision").is_number_unsigned() || data.at("revision").is_number_integer(), "Invalid revision");
        const auto rev = data.at("revision").get<double>();
        require(rev >= 0 && rev <= 9007199254740991.0 && std::floor(rev) == rev, "Invalid revision");
        next.revision = static_cast<unsigned long long>(rev);
        const auto& entities = data.at("entities");
        require(entities.is_array() && !entities.empty() && entities.size() <= 10000, "Scene needs 1..10000 entities");
        std::set<std::string> ids;
        const std::set<std::string> kinds = {"ground","wall","prop","player","key","door","goal","light"};
        unsigned players = 0, grounds = 0;
        for (const auto& item : entities) {
            exactKeys(item, {"id","name","kind","position","scale","color","collider"},
                      "scene.entities[]", {"material","light"});
            engine::Entity e;
            e.id = item.at("id").get<std::string>();
            std::string canonicalId = e.id;
            std::transform(canonicalId.begin(), canonicalId.end(), canonicalId.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            require(uuid(e.id) && ids.insert(canonicalId).second,
                    "Invalid or duplicate entity UUID");
            e.name = item.at("name").get<std::string>();
            require(text(e.name), "Entity name must contain 1-128 printable characters");
            e.kind = item.at("kind").get<std::string>();
            require(kinds.count(e.kind) != 0, "Unknown entity kind: " + e.kind);
            e.position = vector(item.at("position"), "position", -100000, 100000);
            e.scale = vector(item.at("scale"), "scale", 0.001f, 10000);
            auto c = vector(item.at("color"), "color", 0, 1);
            e.color[0]=c.x; e.color[1]=c.y; e.color[2]=c.z;
            if (item.contains("material")) {
                const auto& material = item.at("material");
                exactKeys(material, {"albedo","roughness","metalness","pattern"}, "scene.entities[].material");
                auto albedo = vector(material.at("albedo"), "material.albedo", 0, 1);
                e.material.albedo[0]=albedo.x; e.material.albedo[1]=albedo.y; e.material.albedo[2]=albedo.z;
                e.material.roughness = number(material.at("roughness"), "material.roughness", 0, 1);
                e.material.metalness = number(material.at("metalness"), "material.metalness", 0, 1);
                e.material.pattern = material.at("pattern").get<std::string>();
                require(e.material.pattern == "checker" || e.material.pattern == "brick" ||
                        e.material.pattern == "metal", "material.pattern must be checker, brick or metal");
                e.hasMaterial = true;
            }
            if (e.kind == "light") {
                require(item.contains("light"), "light entity requires a light object");
                const auto& light = item.at("light");
                exactKeys(light, {"color","intensity","range"}, "scene.entities[].light");
                auto lightColor = vector(light.at("color"), "light.color", 0, 1);
                e.lightColor[0]=lightColor.x; e.lightColor[1]=lightColor.y; e.lightColor[2]=lightColor.z;
                e.lightIntensity = number(light.at("intensity"), "light.intensity", 0.001f, 100);
                e.lightRange = number(light.at("range"), "light.range", 0.001f, 1000);
                require(next.scene.lights.size() < 4, "At most 4 lights are supported");
                engine::Light sceneLight;
                sceneLight.position = e.position;
                std::copy(e.lightColor, e.lightColor + 3, sceneLight.color);
                sceneLight.intensity = e.lightIntensity;
                sceneLight.range = e.lightRange;
                next.scene.lights.push_back(sceneLight);
                e.hasLight = true;
            } else {
                require(!item.contains("light"), "light is only supported on a light entity");
            }
            e.collider = item.at("collider").get<bool>();
            players += e.kind == "player"; grounds += e.kind == "ground";
            next.scene.entities.push_back(e);
        }
        require(players == 1 && grounds >= 1, "Scene needs exactly one player and at least one ground");
        const auto& settings = data.at("settings");
        exactKeys(settings, {"walkSpeed","runSpeed","jumpSpeed","gravity"}, "scene.settings");
        next.scene.walkSpeed=number(settings.at("walkSpeed"),"walkSpeed",0.1f,30);
        next.scene.runSpeed=number(settings.at("runSpeed"),"runSpeed",0.1f,60);
        require(next.scene.runSpeed>=next.scene.walkSpeed,"runSpeed must be >= walkSpeed");
        next.scene.jumpSpeed=number(settings.at("jumpSpeed"),"jumpSpeed",0.1f,30);
        next.scene.gravity=number(settings.at("gravity"),"gravity",0.1f,100);
        result=std::move(next);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error=std::string("SCENE_INVALID: ") + e.what();
        return false;
    }
}
}
