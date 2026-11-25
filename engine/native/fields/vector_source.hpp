#pragma once
#include "world.hpp"
#include "../vec3.hpp"

namespace engine::fields {
struct VectorKey {Tick tick=0;std::array<std::int32_t,3> velocityMilli{};};
struct VectorTrack {std::string name;std::vector<VectorKey> keys;};
struct VectorSourceAsset {
    std::string id,hash;
    Vec3 originMetres{};float cellMetres=1;
    Region region;std::uint32_t haloCells=1;
    Tick loopTicks=0,maxAgeTicks=4;
    Domain domain=Domain::VisualReference;
    std::vector<VectorTrack> tracks;
};
VectorSourceAsset parseVectorSource(const std::string& json);
VectorSourceAsset loadVectorSource(const std::string& file);
// CPU producer of authored uniform vector tracks over a bounded lattice. It
// stages all scalar components atomically; the scheduler owns World::commit.
// Quantization is 1 mm/s; interpolation is signed integer rational arithmetic.
class VectorSource {
public:
    VectorSource(std::shared_ptr<World> world,VectorSourceAsset asset);
    ~VectorSource();
    VectorSource(const VectorSource&)=delete;
    bool stage(Tick tick,std::string& error);
    bool release();
    const VectorSourceAsset& asset() const{return asset_;}
    const std::map<std::string,std::array<ChannelId,3>>& channels() const{return channels_;}
    std::shared_ptr<World> world() const{return world_;}
    LeaseRequest reader() const;
    std::array<std::int32_t,3> evaluate(const std::string& track,Tick tick) const;
    std::uint64_t publications() const{return publications_;}
    bool active() const{return active_;}
private:
    std::shared_ptr<World> world_;VectorSourceAsset asset_;Region coverage_;
    std::map<std::string,std::array<ChannelId,3>> channels_;Lease writer_;
    bool staged_=false,active_=false;Tick lastTick_=0;std::uint64_t publications_=0;
};
}
