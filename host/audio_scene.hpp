#pragma once
#include "scene_loader.hpp"

namespace host {
inline bool audioEntityPosition(const engine::Game& game,const std::string& id,engine::Vec3 offset,engine::Vec3& result) {
    const auto& world=game.gameplayRuntime().world;const auto handle=world.find(id);
    if(!world.valid(handle))return false;
    const auto* transform=world.get(handle,"transform");if(!transform)return false;
    const auto& p=transform->at("position");result={p[0].get<float>()+offset.x,p[1].get<float>()+offset.y,p[2].get<float>()+offset.z};return true;
}
inline std::vector<engine::audio::Occluder> audioSceneOccluders(const engine::Game& game,const engine::Vec3* eye=nullptr) {
    std::vector<engine::audio::Occluder> result;result.reserve(256);
    const auto& world=game.gameplayRuntime().world;
    for(const auto handle:world.query("collider")) {
        engine::physics::Aabb bounds;if(!game.entityColliderBounds(world.entity(handle).id,bounds))continue;
        if(eye&&eye->x>bounds.min.x&&eye->x<bounds.max.x&&eye->y>bounds.min.y&&eye->y<bounds.max.y&&eye->z>bounds.min.z&&eye->z<bounds.max.z)continue;
        engine::audio::Occluder box;box.min=bounds.min;box.max=bounds.max;box.gainDb=-9;box.cutoffHz=1200;result.push_back(box);
        if(result.size()==256)break;
    }
    return result;
}
}
