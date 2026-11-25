#include "field_adapters.hpp"
#include "../cloth/solver_cpu.hpp"
#include "../water/body.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace engine::simulation {
namespace {
bool finite(Vec3 v) {return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
}
bool FieldGrid::valid() const {return finite(originMetres)&&std::isfinite(cellMetres)&&cellMetres>0;}
bool FieldGrid::locate(Vec3 metres,fields::SamplePosition& out) const {
    if(!valid()||!finite(metres))return false;
    const double v[]={static_cast<double>(metres.x)-originMetres.x,static_cast<double>(metres.y)-originMetres.y,static_cast<double>(metres.z)-originMetres.z};
    std::int32_t cells[3];std::array<std::uint32_t,3> fractions{};
    for(unsigned i=0;i<3;++i) {
        const double q=std::floor((v[i]/cellMetres)*256.0),base=std::floor(q/256.0);
        if(!std::isfinite(base)||base<std::numeric_limits<std::int32_t>::min()||base>std::numeric_limits<std::int32_t>::max())return false;
        cells[i]=static_cast<std::int32_t>(base);fractions[i]=static_cast<std::uint32_t>(q-base*256.0);
    }
    out={{cells[0],cells[1],cells[2]},fractions};return true;
}
bool FieldGrid::point(fields::Cell cell,Vec3& out) const {
    if(!valid())return false;
    const double v[]={originMetres.x+static_cast<double>(cell.x)*cellMetres,originMetres.y+static_cast<double>(cell.y)*cellMetres,originMetres.z+static_cast<double>(cell.z)*cellMetres};
    for(auto axis:v)if(!std::isfinite(axis)||std::fabs(axis)>std::numeric_limits<float>::max())return false;
    out={static_cast<float>(v[0]),static_cast<float>(v[1]),static_cast<float>(v[2])};return true;
}
WindField::WindField(const fields::World& world,fields::Lease reader,std::array<fields::ChannelId,3> channels,FieldGrid grid,fields::Domain domain)
    :world_(world),reader_(reader),channels_(channels),grid_(grid),domain_(domain) {
    if(!grid.valid()||std::set<fields::ChannelId>(channels.begin(),channels.end()).size()!=3)
        throw std::invalid_argument("NG-E-FIELD-WIND: invalid grid or repeated component");
    for(unsigned i=0;i<3;++i) {
        const auto& channel=world.channel(channels[i]);
        if(channel.unit!="m/s"||channel.domain!=domain)throw std::invalid_argument("NG-E-FIELD-WIND: component unit or authority mismatch");
        scale_[i]=1.0/channel.unitsPerInteger;
    }
}
VectorSample WindField::sample(Vec3 position,fields::Tick tick) const {
    VectorSample out;out.version=world_.version();out.snapshotTick=world_.committedTick();
    fields::SamplePosition p;if(!grid_.locate(position,p)){out.status=fields::Status::InvalidPosition;return out;}
    std::array<fields::Sample,3> samples;
    fields::Tick oldest=std::numeric_limits<fields::Tick>::max();
    for(unsigned i=0;i<3;++i) {
        samples[i]=world_.sampleLinear(reader_,channels_[i],p,tick);
        if(!samples[i].valid()){out.status=samples[i].status;return out;}
        oldest=std::min(oldest,samples[i].sourceTick);out.clamped|=samples[i].clamped;
    }
    out.sourceTick=oldest;out.ageTicks=tick-oldest;
    if(domain_==fields::Domain::CpuAuthority) {
        if(tick!=out.snapshotTick){out.status=fields::Status::TickMismatch;return out;}
        if(oldest!=tick){out.status=fields::Status::Stale;return out;}
    }
    out.metresPerSecond={static_cast<float>(samples[0].value*scale_[0]),static_cast<float>(samples[1].value*scale_[1]),static_cast<float>(samples[2].value*scale_[2])};
    out.positionQuantizationMetres=grid_.cellMetres/256.0;out.status=fields::Status::Ok;return out;
}
WindApplication applyWindField(cloth::Solver& cloth,const WindField& wind,fields::Tick tick) {
    WindApplication out;std::vector<Vec3> values;values.reserve(cloth.vertexCount());
    out.oldestSourceTick=std::numeric_limits<fields::Tick>::max();
    for(std::size_t i=0;i<cloth.vertexCount();++i) {
        auto sample=wind.sample(cloth.position(i),tick);++out.sampledVertices;
        if(!sample.valid()) {if(!out.unavailableVertices)out.firstFailure=sample.status;++out.unavailableVertices;continue;}
        values.push_back(sample.metresPerSecond);out.oldestSourceTick=std::min(out.oldestSourceTick,sample.sourceTick);out.version=sample.version;
    }
    if(out.unavailableVertices){if(out.unavailableVertices==out.sampledVertices)out.oldestSourceTick=0;return out;}
    cloth.setWindSamples(std::move(values));out.applied=true;return out;
}
WaterPublication publishWater(fields::World& world,const water::Body& body,const WaterProjection& spec) {
    WaterPublication out;out.bodyTick=body.diagnostics().tick;
    const auto reject=[&](const char* error){out.error=error;return out;};
    if(!spec.grid.valid()||spec.region.min.x>=spec.region.max.x||spec.region.min.z>=spec.region.max.z||
       static_cast<std::int64_t>(spec.region.max.y)-spec.region.min.y!=1||!spec.maxSamples||spec.maxSamples>262144||
       std::set<fields::ChannelId>(spec.channels.begin(),spec.channels.end()).size()!=4)return reject("NG-E-FIELD-WATER: invalid grid, layer, channels or sample budget");
    const auto nx=static_cast<std::uint64_t>(static_cast<std::int64_t>(spec.region.max.x)-spec.region.min.x);
    const auto nz=static_cast<std::uint64_t>(static_cast<std::int64_t>(spec.region.max.z)-spec.region.min.z);
    if(nx>spec.maxSamples||nz>spec.maxSamples/nx)return reject("NG-E-FIELD-WATER: projection sample budget");
    const auto count=static_cast<std::size_t>(nx*nz);
    std::array<fields::Publication,4> publications;
    try {
        for(unsigned i=0;i<4;++i) {
            const auto& channel=world.channel(spec.channels[i]);
            if(channel.domain!=fields::Domain::CpuAuthority||channel.unit!=(i<2?"m":"m/s"))return reject("NG-E-FIELD-WATER: channel unit or authority mismatch");
            out.valueQuantization[i]=0.5/channel.unitsPerInteger;
            publications[i]={spec.writer,spec.channels[i],spec.source,spec.epoch,spec.sourceTick,spec.publishTick,fields::Domain::CpuAuthority,{}};
            publications[i].writes.reserve(count);
        }
    }catch(const std::out_of_range&) {return reject("NG-E-FIELD-WATER: unknown channel");}
    for(std::int64_t z=spec.region.min.z;z<spec.region.max.z;++z)for(std::int64_t x=spec.region.min.x;x<spec.region.max.x;++x) {
        const fields::Cell cell{static_cast<std::int32_t>(x),spec.region.min.y,static_cast<std::int32_t>(z)};
        Vec3 metres;if(!spec.grid.point(cell,metres))return reject("NG-E-FIELD-WATER: physical coordinate overflow");
        const auto query=body.query(metres.x,metres.z);
        if(!query.valid){++out.unknownSamples;continue;}
        const float values[]={query.surfaceHeightMetres,query.depthMetres,query.velocityX,query.velocityZ};
        for(unsigned i=0;i<4;++i) {
            const auto& channel=world.channel(spec.channels[i]);
            const double rounded=std::round(static_cast<double>(values[i])*channel.unitsPerInteger);
            if(!std::isfinite(rounded)||rounded<channel.minimum||rounded>channel.maximum)return reject("NG-E-FIELD-WATER: quantized value outside channel range");
            publications[i].writes.push_back({cell,static_cast<std::int32_t>(rounded)});
        }
        ++out.knownSamples;out.bodyQueryErrorMetres=std::max(out.bodyQueryErrorMetres,query.errorBoundMetres);
    }
    out.result=world.publishAtomic(std::vector<fields::Publication>(publications.begin(),publications.end()));
    if(!out.accepted())out.error="NG-E-FIELD-WATER: World rejected atomic projection";
    return out;
}
}
