#include "runtime.hpp"
#include "../determinism.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::effects {
namespace {
constexpr float Dt = 1.0f/120.0f;
constexpr std::uint64_t RateDenominator = 120000;
constexpr float Pi = 3.14159265358979323846f;
// Intensity is a count multiplier, so it must land on whole particles. Rounding the
// per-tick product keeps a fractional intensity from losing or inventing emissions
// over time, and both the CPU reference and the GPU scheduler use the same rule.
std::uint64_t scaledCount(std::uint64_t count,float intensity) {
    return static_cast<std::uint64_t>(std::llround(static_cast<double>(count)*static_cast<double>(intensity)));
}
bool finite(Vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
float length(Vec3 v) { return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); }
float dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 cross(Vec3 a,Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
Vec3 unit(Vec3 v) { const auto l=length(v); return l>1e-8f?v/l:Vec3{0,1,0}; }
float random(std::uint64_t seed,unsigned channel) {
    return static_cast<float>(determinism::mix64(seed+channel*0x9E3779B97F4A7C15ull)>>40)*(1.0f/16777216.0f);
}
Vec3 direction(float u,float v) {
    const float y=2*u-1,r=std::sqrt(std::max(0.0f,1-y*y)),a=v*2*Pi;
    return {r*std::cos(a),y,r*std::sin(a)};
}
std::array<float,4> curve(const std::vector<CurveKey>& keys,float t) {
    if (t<=keys.front().t) return keys.front().value;
    for (std::size_t i=1;i<keys.size();++i) if (t<=keys[i].t) {
        if (t==keys[i].t) return keys[i].value;
        const auto& a=keys[i-1]; const auto& b=keys[i]; const float f=(t-a.t)/(b.t-a.t);
        std::array<float,4> result{};
        for (unsigned k=0;k<4;++k) result[k]=a.value[k]+(b.value[k]-a.value[k])*f;
        return result;
    }
    return keys.back().value;
}
void validate(const std::shared_ptr<const Asset>& asset) {
    if (!asset || asset->id.empty() || asset->hash.empty() || asset->layoutHash.empty() || asset->duration==0 ||
        asset->maxAlive==0 || asset->maxAlive>1000000 || asset->maxSpawnPerTick==0 || asset->maxSpawnPerTick>1000000 ||
        asset->emitters.empty() || asset->emitters.size()>32) throw Error("NG-E-EFFECT-ASSET","Invalid native effect definition");
    for (const auto& e:asset->emitters) {
        if (e.id.empty() || e.lifeMin==0 || e.lifeMin>e.lifeMax || e.lifeMax>432000 || e.modules.empty() ||
            e.modules.front().kind!=ModuleKind::Initialize || e.renderers.empty() || !finite(e.position))
            throw Error("NG-E-EFFECT-ASSET","Invalid native effect emitter");
    }
}
template<class T> void remove(std::vector<T>& values,std::size_t index) { if (index+1!=values.size()) values[index]=std::move(values.back()); values.pop_back(); }
}

void Runtime::Pool::erase(std::size_t i) {
    remove(positions,i); remove(velocities,i); remove(colors,i); remove(sizes,i);
    remove(ages,i); remove(lives,i); remove(emitters,i); remove(ids,i);
}
Runtime::Runtime(std::size_t particles,std::size_t instances) : maxParticles_(particles),maxInstances_(instances) {
    if (!particles || particles>1000000 || !instances || instances>4096)
        throw Error("NG-E-EFFECT-BUDGET","Invalid native effect runtime budget");
}
void Runtime::install(std::shared_ptr<const Asset> asset) {
    validate(asset);
    auto found=assets_.find(asset->id);
    if (found!=assets_.end()) {
        if (found->second->hash==asset->hash) return;
        throw Error("NG-E-EFFECT-REVISION","Use reload to replace an installed effect");
    }
    if (assets_.size()>=4096) throw Error("NG-E-EFFECT-BUDGET","Native effect registry full");
    assets_.emplace(asset->id,std::make_shared<const Asset>(*asset));
}
ReloadResult Runtime::reload(std::shared_ptr<const Asset> asset,bool resetIncompatible) {
    validate(asset);
    auto found=assets_.find(asset->id);
    if (found==assets_.end()) { install(asset); return ReloadResult::Installed; }
    if (asset->revision<found->second->revision ||
        (asset->revision==found->second->revision&&asset->hash!=found->second->hash))
        throw Error("NG-E-EFFECT-REVISION","Effect reload revision did not advance");
    const bool compatible=asset->layoutHash==found->second->layoutHash;
    if (!compatible&&!resetIncompatible) return ReloadResult::ResetRequired;
    for (const auto& instance:instances_) if (instance.active&&instance.asset->id==asset->id) {
        const auto reason=unsupported(*asset);
        if (!reason.empty()) throw Error("NG-E-EFFECT-UNSUPPORTED",reason);
    }
    auto immutable=std::make_shared<const Asset>(*asset);
    found->second=immutable;
    for (auto& instance:instances_) if (instance.active&&instance.asset->id==asset->id) {
        instance.asset=immutable;
        if (!compatible) {
            counters_.killed+=instance.pool.size(); instance.pool={}; instance.emitters.assign(asset->emitters.size(),{});
            instance.age=0; instance.emitting=true;
        }
    }
    return compatible?ReloadResult::Preserved:ReloadResult::Reset;
}
std::string Runtime::unsupported(const Asset& asset) {
    if (asset.smoke) return "Smoke solver effects require an explicitly configured GPU smoke service";
    if (asset.hasField) return "Field ownership and publication require the field runtime adapter";
    for (const auto& rule:asset.rules)
        if (rule.on==EventKind::Threshold) return "Threshold events need an authored comparison value the schema does not carry yet";
    for (const auto& e:asset.emitters) {
        if (e.shape==Shape::Mesh) return "Mesh-surface emission requires the cooked mesh sampler";
        for (const auto& r:e.renderers) if (r.type!="sprite"||r.lit) return "Only unlit sprite drawing is integrated in this fallback";
        for (const auto& m:e.modules) if (m.kind==ModuleKind::FieldCoupling) return "Field coupling requires an active field lease";
        for (const auto& m:e.modules) if (m.kind==ModuleKind::VelocityField) return "Velocity fields require an explicitly bound sampler; CPU fallback has none";
        for (const auto& m:e.modules) if (m.kind==ModuleKind::FieldDeposit) return "Field deposits require an explicitly configured writer; CPU fallback has none";
    }
    return {};
}
SpawnResult Runtime::spawn(const std::string& id,Vec3 origin,std::uint64_t seed,SpawnParams params) {
    if (!std::isfinite(params.intensity)) return {SpawnStatus::InvalidParameter,{},"Effect intensity is not finite"};
    return spawnInternal(id,origin,seed,0,std::clamp(params.intensity,0.0f,8.0f));
}
SpawnResult Runtime::spawnInternal(const std::string& id,Vec3 origin,std::uint64_t seed,std::uint32_t depth,float intensity) {
    const auto found=assets_.find(id);
    if (found==assets_.end()) return {SpawnStatus::UnknownAsset,{},"Unknown effect asset"};
    if (!finite(origin)) return {SpawnStatus::InvalidPosition,{},"Effect position is not finite"};
    const auto reason=unsupported(*found->second);
    if (!reason.empty()) return {SpawnStatus::UnsupportedFeature,{},reason};
    std::size_t index=0;
    while (index<instances_.size()&&(instances_[index].active||instances_[index].generation==0)) ++index;
    if (index>=maxInstances_||serial_==std::numeric_limits<std::uint64_t>::max()) return {SpawnStatus::Capacity,{},"Effect instance capacity exhausted"};
    if (index==instances_.size()) instances_.emplace_back();
    auto& instance=instances_[index];
    instance.active=true; instance.paused=false; instance.emitting=true;
    instance.asset=found->second; instance.origin=origin; instance.age=0; instance.serial=++serial_; instance.seed=seed;
    instance.depth=depth; instance.spawnsThisTick=0; instance.intensity=intensity;
    instance.ruleSpawns.assign(instance.asset->rules.size(),0);
    instance.emitters.assign(instance.asset->emitters.size(),{});
    emit(instance,EventKind::Spawn,origin,{},0);
    return {SpawnStatus::Accepted,{static_cast<std::uint32_t>(index),instance.generation},{}};
}
const Runtime::Instance* Runtime::find(Handle handle) const {
    if (handle.slot>=instances_.size()) return nullptr;
    const auto& instance=instances_[handle.slot];
    return instance.active&&instance.generation==handle.generation?&instance:nullptr;
}
Runtime::Instance* Runtime::find(Handle handle) { return const_cast<Instance*>(static_cast<const Runtime*>(this)->find(handle)); }
bool Runtime::valid(Handle handle) const { return find(handle)!=nullptr; }
void Runtime::retire(Instance& instance) {
    instance.active=false; instance.pool={}; instance.emitters.clear(); instance.asset.reset();
    instance.ruleSpawns.clear(); instance.depth=0; instance.intensity=1.0f;
    instance.generation=instance.generation==std::numeric_limits<std::uint32_t>::max()?0:instance.generation+1;
}
bool Runtime::stop(Handle handle,bool immediate) {
    auto* instance=find(handle); if (!instance) return false;
    instance->emitting=false;
    if (immediate) { counters_.killed+=instance->pool.size(); retire(*instance); }
    return true;
}
bool Runtime::pause(Handle handle,bool paused) { auto* instance=find(handle); if (!instance) return false; instance->paused=paused; return true; }
void Runtime::setColliders(std::vector<Collider> colliders) {
    for (const auto& collider:colliders) {
        if (!finite(collider.centre)||!finite(collider.extent)||!std::isfinite(collider.radius)||collider.radius<=0)
            throw Error("NG-E-EFFECT-COLLIDER","Invalid effect collider");
    }
    if (colliders.size()>64) throw Error("NG-E-EFFECT-COLLIDER","Effect collider budget exceeded");
    colliders_=std::move(colliders);
}
void Runtime::clearEvents() { events_.clear(); }
void Runtime::emit(Instance& instance,EventKind kind,Vec3 position,Vec3 normal,float energy) {
    if (events_.size()>=65536) { ++counters_.eventsDropped; return; }
    Event event;
    event.kind=kind; event.source=instance.asset->id; event.position=position; event.normal=normal;
    event.energy=energy; event.tick=counters_.tick; event.sequence=++eventSerial_; event.depth=instance.depth;
    events_.push_back(event); ++counters_.events;
    const auto& asset=*instance.asset;
    for (std::size_t index=0;index<asset.rules.size();++index) {
        const auto& rule=asset.rules[index];
        if (rule.on!=kind) continue;
        if (instance.depth+1>asset.maxDepth || instance.spawnsThisTick>=asset.maxEventsPerTick ||
            instance.ruleSpawns[index]>=rule.limit || pending_.size()>=4096) { ++counters_.eventsDropped; continue; }
        Pending queued;
        // For a timeline rule delayTicks is the marker time, not an extra delay.
        const auto delay=kind==EventKind::Timeline?0u:rule.delayTicks;
        queued.asset=rule.spawn; queued.position=position; queued.dueTick=counters_.tick+delay;
        queued.seed=determinism::mix64(eventSerial_^static_cast<std::uint64_t>(index))^asset.seed;
        queued.depth=instance.depth+1;
        // A sub-effect is part of the same visual event, so it inherits the strength of
        // the spawn that produced it instead of quietly resetting to full.
        queued.intensity=instance.intensity;
        pending_.push_back(queued);
        ++instance.spawnsThisTick; ++instance.ruleSpawns[index];
    }
}
void Runtime::dispatchPending() {
    std::vector<Pending> due, future;
    due.reserve(pending_.size());
    for (auto& queued:pending_) (queued.dueTick<=counters_.tick?due:future).push_back(queued);
    pending_.swap(future);
    for (const auto& queued:due) {
        if (spawnInternal(queued.asset,queued.position,queued.seed,queued.depth,queued.intensity).status==SpawnStatus::Accepted) ++counters_.subeffects;
        else ++counters_.eventsDropped;
    }
}
void Runtime::collide(Instance& instance) {
    if (colliders_.empty()) return;
    auto& pool=instance.pool;
    for (std::size_t i=0;i<pool.size();++i) {
        for (const auto& collider:colliders_) {
            Vec3 normal{};
            float depth=0;
            if (collider.kind==ColliderKind::Sphere) {
                const Vec3 delta{pool.positions[i].x-collider.centre.x,pool.positions[i].y-collider.centre.y,
                                 pool.positions[i].z-collider.centre.z};
                const float distance=length(delta);
                if (distance>=collider.radius) continue;
                normal=distance>1e-6f?delta/distance:Vec3{0,1,0};
                depth=collider.radius-distance;
            } else {
                const Vec3 half=collider.extent*0.5f;
                const Vec3 local{pool.positions[i].x-collider.centre.x,pool.positions[i].y-collider.centre.y,
                                 pool.positions[i].z-collider.centre.z};
                const float px=half.x-std::fabs(local.x),py=half.y-std::fabs(local.y),pz=half.z-std::fabs(local.z);
                if (px<=0||py<=0||pz<=0) continue;
                if (px<=py&&px<=pz) { normal={local.x<0?-1.0f:1.0f,0,0}; depth=px; }
                else if (py<=pz) { normal={0,local.y<0?-1.0f:1.0f,0}; depth=py; }
                else { normal={0,0,local.z<0?-1.0f:1.0f}; depth=pz; }
            }
            pool.positions[i]+=normal*depth;
            const float approach=dot(pool.velocities[i],normal);
            if (approach<0) {
                pool.velocities[i]-=normal*(approach*1.25f);
                ++counters_.collisions;
                emit(instance,EventKind::Collision,pool.positions[i],normal,-approach);
            }
            break;
        }
    }
}
bool Runtime::move(Handle handle,Vec3 origin) { auto* instance=find(handle); if (!instance||!finite(origin)) return false; instance->origin=origin; return true; }

void Runtime::step() {
    if (counters_.tick==std::numeric_limits<std::uint64_t>::max()) throw Error("NG-E-EFFECT-TIME","Effect tick capacity exceeded");
    dispatchPending();
    std::size_t totalAlive=0; for (const auto& instance:instances_) if (instance.active) totalAlive+=instance.pool.size();
    for (auto& instance:instances_) {
        if (!instance.active||instance.paused) continue;
        instance.spawnsThisTick=0;
        std::fill(instance.ruleSpawns.begin(),instance.ruleSpawns.end(),0);
        auto& pool=instance.pool; const auto& asset=*instance.asset;
        for (std::size_t i=0;i<pool.size();) {
            if (++pool.ages[i]>=pool.lives[i]) {
                emit(instance,EventKind::Death,pool.positions[i],{},0);
                pool.erase(i); --totalAlive; ++counters_.expired; continue;
            }
            const auto& emitter=asset.emitters[pool.emitters[i]];
            const float age=static_cast<float>(pool.ages[i])/pool.lives[i];
            for (const auto& module:emitter.modules) {
                switch (module.kind) {
                case ModuleKind::Acceleration: pool.velocities[i]+=module.vector*Dt; break;
                case ModuleKind::Drag: pool.velocities[i]*=std::exp(-module.coefficient*Dt); break;
                case ModuleKind::ColorCurve: pool.colors[i]=curve(module.keys,age); break;
                case ModuleKind::SizeCurve: { const auto v=curve(module.keys,age); pool.sizes[i]={v[0],v[1]}; break; }
                case ModuleKind::CurlNoise: {
                    const auto p=pool.positions[i]*module.frequency; const float t=static_cast<float>(instance.age%1000000)*Dt;
                    pool.velocities[i]+=Vec3{std::cos(p.y-t),std::cos(p.z+t),std::cos(p.x+t*2)}*(module.coefficient*Dt); break;
                }
                default: break;
                }
            }
            pool.positions[i]+=pool.velocities[i]*Dt;
            ++i;
        }
        // Collisions run once per tick on the committed positions, so an event always
        // refers to a state a consumer can reproduce.
        collide(instance);
        std::size_t spawnBudget=asset.maxSpawnPerTick;
        if (instance.emitting&&(asset.loop||instance.age<asset.duration)) {
            const auto age=static_cast<std::uint32_t>(instance.age%asset.duration);
            for (std::size_t ruleIndex=0;ruleIndex<asset.rules.size();++ruleIndex) {
                const auto& rule=asset.rules[ruleIndex];
                if (rule.on==EventKind::Timeline&&rule.delayTicks==age)
                    emit(instance,EventKind::Timeline,instance.origin,{},0);
            }
            for (std::size_t emitterIndex=0;emitterIndex<asset.emitters.size();++emitterIndex) {
                const auto& emitter=asset.emitters[emitterIndex]; auto& state=instance.emitters[emitterIndex];
                state.phase+=scaledCount(emitter.rateMilli,instance.intensity);
                std::uint64_t requested=state.phase/RateDenominator; state.phase%=RateDenominator;
                for (const auto& burst:emitter.bursts) if (burst.tick==age) requested+=scaledCount(burst.count,instance.intensity);
                if (state.sequence>std::numeric_limits<std::uint64_t>::max()-requested) throw Error("NG-E-EFFECT-TIME","Effect spawn sequence overflow");
                const auto freeInstance=pool.size()<asset.maxAlive?asset.maxAlive-pool.size():0;
                const auto accepted=std::min<std::uint64_t>({requested,spawnBudget,freeInstance,maxParticles_-totalAlive});
                counters_.dropped+=requested-accepted;
                for (std::uint64_t index=0;index<accepted;++index) {
                    const auto sequence=state.sequence+index;
                    const auto seed=determinism::deriveSeed(instance.seed^asset.seed^determinism::mix64(instance.serial),emitter.id)^determinism::mix64(sequence);
                    Vec3 offset{};
                    Vec3 velocity=emitter.modules.front().vector;
                    if (emitter.shape==Shape::Sphere) offset=direction(random(seed,0),random(seed,1))*(std::cbrt(random(seed,2))*emitter.radius);
                    else if (emitter.shape==Shape::Box) offset={ (random(seed,0)-.5f)*emitter.extent.x,(random(seed,1)-.5f)*emitter.extent.y,(random(seed,2)-.5f)*emitter.extent.z };
                    else if (emitter.shape==Shape::Line) offset=emitter.lineEnd*random(seed,0);
                    else if (emitter.shape==Shape::Cone) {
                        const float a=random(seed,0)*2*Pi,r=std::sqrt(random(seed,1))*emitter.radius;
                        offset={std::cos(a)*r,0,std::sin(a)*r};
                        const float cosine=1-random(seed,2)*(1-std::cos(emitter.angle)),sine=std::sqrt(std::max(0.0f,1-cosine*cosine));
                        const auto axis=unit(velocity),right=unit(cross(std::fabs(axis.y)<.9f?Vec3{0,1,0}:Vec3{1,0,0},axis)),up=cross(axis,right);
                        velocity=(axis*cosine+(right*std::cos(a)+up*std::sin(a))*sine)*length(velocity);
                    }
                    auto position=emitter.position+offset;
                    if (!emitter.localSpace) position+=instance.origin;
                    const auto life=emitter.lifeMin+static_cast<std::uint32_t>(random(seed,3)*(static_cast<double>(emitter.lifeMax)-emitter.lifeMin+1));
                    auto color=emitter.modules.front().color; auto size=emitter.modules.front().size;
                    for (const auto& module:emitter.modules) {
                        if (module.kind==ModuleKind::ColorCurve) color=curve(module.keys,0);
                        if (module.kind==ModuleKind::SizeCurve) { const auto v=curve(module.keys,0); size={v[0],v[1]}; }
                    }
                    pool.positions.push_back(position); pool.velocities.push_back(velocity); pool.colors.push_back(color); pool.sizes.push_back(size);
                    pool.ages.push_back(0); pool.lives.push_back(life); pool.emitters.push_back(static_cast<std::uint32_t>(emitterIndex)); pool.ids.push_back(++counters_.emitted);
                }
                state.sequence+=requested; spawnBudget-=static_cast<std::size_t>(accepted); totalAlive+=static_cast<std::size_t>(accepted);
            }
        }
        ++instance.age;
        if (!asset.loop&&instance.age>=asset.duration) instance.emitting=false;
        if (!instance.emitting&&pool.size()==0) retire(instance);
    }
    ++counters_.tick;
}

std::vector<Sprite> Runtime::sprites() const {
    std::vector<Sprite> result;
    for (const auto& instance:instances_) if (instance.active) {
        const auto& pool=instance.pool;
        for (std::size_t i=0;i<pool.size();++i) {
            const auto& emitter=instance.asset->emitters[pool.emitters[i]];
            const auto position=pool.positions[i]+(emitter.localSpace?instance.origin:Vec3{});
            for (const auto& renderer:emitter.renderers) result.push_back({position,pool.colors[i],pool.sizes[i],0,renderer.texture,renderer.blend,pool.ids[i]});
        }
    }
    return result;
}
Stats Runtime::stats() const {
    auto result=counters_;
    for (const auto& instance:instances_) if (instance.active) { ++result.instances; result.alive+=instance.pool.size(); }
    return result;
}
std::uint64_t Runtime::digest() const {
    determinism::Hasher hash; hash.text("nativegpu.effect-cpu-reference/1");
    hash.u64(counters_.tick); hash.u64(counters_.emitted); hash.u64(counters_.expired); hash.u64(counters_.killed); hash.u64(counters_.dropped);
    hash.u64(counters_.events); hash.u64(counters_.subeffects); hash.u64(counters_.eventsDropped); hash.u64(counters_.collisions);
    hash.u64(serial_); hash.u64(eventSerial_); hash.u64(pending_.size());
    for (const auto& collider:colliders_) {
        hash.u8(static_cast<std::uint8_t>(collider.kind));
        hash.quantized(collider.centre.x); hash.quantized(collider.centre.y); hash.quantized(collider.centre.z);
        hash.quantized(collider.extent.x); hash.quantized(collider.extent.y); hash.quantized(collider.extent.z);
        hash.quantized(collider.radius);
    }
    for (const auto& instance:instances_) if (instance.active) {
        hash.text(instance.asset->hash); hash.u64(instance.serial); hash.u64(instance.seed); hash.u64(instance.age);
        hash.u32(instance.depth);
        hash.u8(instance.paused); hash.u8(instance.emitting); hash.quantized(instance.origin.x); hash.quantized(instance.origin.y); hash.quantized(instance.origin.z);
        hash.quantized(instance.intensity);
        for (const auto& e:instance.emitters) { hash.u64(e.phase); hash.u64(e.sequence); }
        for (std::size_t i=0;i<instance.pool.size();++i) {
            hash.u64(instance.pool.ids[i]); hash.u32(instance.pool.ages[i]); hash.u32(instance.pool.lives[i]); hash.u32(instance.pool.emitters[i]);
            const auto p=instance.pool.positions[i],v=instance.pool.velocities[i];
            hash.quantized(p.x); hash.quantized(p.y); hash.quantized(p.z); hash.quantized(v.x); hash.quantized(v.y); hash.quantized(v.z);
            for (auto c:instance.pool.colors[i]) hash.quantized(c); for (auto s:instance.pool.sizes[i]) hash.quantized(s);
        }
        hash.u64(0);
    }
    return hash.value();
}
void Runtime::clear() {
    for (auto& instance:instances_) if (instance.active) retire(instance);
    counters_={}; serial_=0;
}
} // namespace engine::effects
