#pragma once
#include "../engine/native/game.hpp"
#include "../engine/audio/audio.hpp"
#include <map>
#include <filesystem>
#include <string>

namespace host {
// Scene-authored audio emitters. They are host data, not engine::Scene data: the
// runtime only needs a position and a sound id, and keeping them here means the
// audio block cannot change the gameplay digest.
struct AudioEmitter {
    std::string id;
    std::string entityId;
    std::string sound;
    std::string trigger;
    std::string bus = "sfx";
    float gainDb = 0.0f;
    float minDistance = 1.5f;
    float maxDistance = 28.0f;
    bool loop = false;
    bool occlusion = true;
    bool spatial = true;
    float pitch = 1.0f;
    engine::Vec3 offset{}; // world metres relative to the live entity origin
};

struct SceneAudio {
    float masterGainDb = -3.0f;
    float loudnessTargetLufs = -16.0f;
    std::vector<AudioEmitter> emitters;
    std::map<std::string,std::shared_ptr<const engine::audio::Sound>> sounds;
    std::string contentHash;
};

struct SceneDocument {
    struct Water {
        bool present=false;
        std::string file,sha256,cookedData;
        engine::Vec3 origin{},absorption{.65f,.24f,.14f},scattering{.035f,.12f,.095f};
        float roughness=.09f;
        // Configured upstream -> primary transfer. The upstream is its own cooked water
        // body; source is authored in upstream-local metres, landing in primary-local.
        struct Link {
            std::string file,sha256,cookedData;
            engine::Vec3 origin{};
            float sourceX=0,sourceZ=0,sourceRadiusMetres=.6f;
            float landingX=0,landingZ=0,landingRadiusMetres=.8f;
            float flowCubicMetresPerSecond=.1f;
            float transitSeconds=0;
            float widthMetres=1.5f;
            float exitNormalSpeed=3.0f;
        };
        std::vector<Link> links;
        // Opt-in: static collider props/walls whose column intersects the water become
        // impermeable obstacles (or raised beds when they stay below the surface).
        bool obstaclesFromColliders=false;
    } water;
    engine::Scene scene;
    std::string projectId;
    std::string name;
    unsigned long long revision = 0;
    SceneAudio audio;
};
// Loading is all-or-nothing. A malformed document cannot modify the active scene.
bool loadScene(const std::filesystem::path& path, SceneDocument& result, std::string& error);
}
