#pragma once
#include "../engine/native/game.hpp"
#include <filesystem>
#include <string>

namespace host {
struct SceneDocument {
    engine::Scene scene;
    std::string projectId;
    std::string name;
    unsigned long long revision = 0;
};
// Loading is all-or-nothing. A malformed document cannot modify the active scene.
bool loadScene(const std::filesystem::path& path, SceneDocument& result, std::string& error);
}
