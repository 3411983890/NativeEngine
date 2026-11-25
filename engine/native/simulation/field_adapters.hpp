#pragma once
#include "../fields/world.hpp"
#include "../vec3.hpp"

namespace engine::cloth {class Solver;}
namespace engine::water {class Body;}
namespace engine::simulation {
// Explicit metres-to-lattice mapping. The caller binds channels authored on
// this grid; World itself has no implicit global cell size or origin.
struct FieldGrid {
    Vec3 originMetres{};
    double cellMetres=1;
    bool valid() const;
    bool locate(Vec3 metres,fields::SamplePosition& out) const;
    bool point(fields::Cell cell,Vec3& out) const;
};
struct VectorSample {
    fields::Status status=fields::Status::Unavailable;
    Vec3 metresPerSecond{};
    fields::Tick sourceTick=0,snapshotTick=0,ageTicks=0;
    std::uint64_t version=0;
    double positionQuantizationMetres=0;
    bool clamped=false;
    bool valid() const {return status==fields::Status::Ok;}
};
// Borrows a caller-owned read lease. Authority mode requires all three CPU
// channels to be fresh at the requested committed tick, even for a Stale lease.
// It never reads GPU data or treats an unavailable component as zero wind.
class WindField {
public:
    WindField(const fields::World& world,fields::Lease reader,std::array<fields::ChannelId,3> channels,
              FieldGrid grid,fields::Domain requiredDomain);
    VectorSample sample(Vec3 positionMetres,fields::Tick tick) const;
private:
    const fields::World& world_;
    fields::Lease reader_;
    std::array<fields::ChannelId,3> channels_;
    std::array<double,3> scale_;
    FieldGrid grid_;
    fields::Domain domain_;
};
struct WindApplication {
    bool applied=false;
    fields::Status firstFailure=fields::Status::Ok;
    std::size_t sampledVertices=0,unavailableVertices=0;
    fields::Tick oldestSourceTick=0;
    std::uint64_t version=0;
};
// Samples every vertex before replacing any solver input. On failure the
// previous input remains installed and the caller chooses its fallback policy.
WindApplication applyWindField(cloth::Solver& cloth,const WindField& wind,fields::Tick tick);

struct WaterProjection {
    fields::Lease writer;
    // Height, depth, X velocity, Z velocity. Body/source identity stays in the
    // object system; source-selecting queries keep overlapping bodies separate.
    std::array<fields::ChannelId,4> channels{};
    fields::Region region;
    FieldGrid grid;
    std::uint64_t source=0,epoch=0;
    fields::Tick sourceTick=0,publishTick=0;
    std::size_t maxSamples=65536;
};
struct WaterPublication {
    fields::PublishResult result=fields::PublishResult::Invalid;
    std::size_t knownSamples=0,unknownSamples=0;
    std::uint64_t bodyTick=0;
    std::array<double,4> valueQuantization{};
    float bodyQueryErrorMetres=0;
    std::string error;
    bool accepted() const {return result==fields::PublishResult::Accepted||result==fields::PublishResult::Duplicate;}
};
// CPU authority projection onto one horizontal lattice layer. This publishes
// query proxies, not water mass or solver ownership. It never commits World.
WaterPublication publishWater(fields::World& world,const water::Body& body,const WaterProjection& spec);
}
