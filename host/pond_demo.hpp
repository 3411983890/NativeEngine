#pragma once

#include "../engine/native/vec3.hpp"
#include "../engine/native/water/body.hpp"
#include "../engine/native/water/coupling.hpp"
#include "../renderer/d3d12_renderer.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace host {
// A bounded CPU reference surface; the Game supplies every actual fixed tick.
// Optical rendering is owned by WaterDraw. Gameplay forces are a later stage.
class PondDemo {
public:
    PondDemo() : PondDemo(makeAsset(),{}, {.65f,.24f,.14f}, {.035f,.12f,.095f}, .09f) {}
    PondDemo(std::shared_ptr<const engine::water::Asset> asset,engine::Vec3 origin,engine::Vec3 absorption,engine::Vec3 scattering,float roughness)
        :body_(std::make_shared<engine::water::Body>(std::move(asset),origin)),origin_(origin),absorption_(absorption),scattering_(scattering),roughness_(roughness){}

    // Standalone harness path. The host Game owns the fixed-tick simulation and calls
    // the same footprint helper; this entry point exists for the CPU water contracts.
    void update(std::uint64_t tick, engine::Vec3 player) {
        if (tick < tick_) { body_->reset(); tick_ = 0; footprint_ = {}; }
        if(tick==tick_)return;
        const float dt=float(tick-tick_)/120.f;
        const float dx=footprint_.observed?player.x-footprint_.previous.x:0.f;
        const float dz=footprint_.observed?player.z-footprint_.previous.z:0.f;
        const auto result=engine::water::updateFootprint(*body_,player,1.8f,dx,dz,dt,footprint_);
        if(result.interacted)++disturbances_;
        while (tick_ < tick) { body_->step(1.f / 120.f); ++tick_; }
    }

    engine::water::Query query(float x, float z) const { return body_->query(x, z); }
    engine::water::Diagnostics diagnostics() const { return body_->diagnostics(); }
    std::uint64_t disturbances() const { return disturbances_; }
    std::shared_ptr<engine::water::Body> bodyHandle() const { return body_; }
    const engine::water::Body& body() const { return *body_; }
    // Diagnostics: paint white water without touching the authoritative body. Used by the
    // transport contract and by debug captures to isolate advection from wave sources.
    void seedFoam(float x, float z, float radiusMetres, float value = 1.0f) {
        if (foamColumns_ < 2 || foamRows_ < 2 || !(radiusMetres > 0)) return;
        for (unsigned row = 0; row < foamRows_; ++row)
            for (unsigned column = 0; column < foamColumns_; ++column) {
                const float px = origin_.x + body_->asset().extentX *
                    (float(column) / float(foamColumns_ - 1) - .5f);
                const float pz = origin_.z + body_->asset().extentZ *
                    (float(row) / float(foamRows_ - 1) - .5f);
                const float dx = px - x, dz = pz - z;
                if (dx * dx + dz * dz > radiusMetres * radiusMetres) continue;
                foam_[std::size_t(row) * foamColumns_ + column] =
                    std::max(foam_[std::size_t(row) * foamColumns_ + column], value);
            }
    }
    // Presentation-only white water, sampled from the last published surface. Tests use
    // it to prove the scalar is carried by the flow instead of staying at the source.
    float foamAt(float x, float z) const {
        if (foamColumns_ < 2 || foamRows_ < 2 || foam_.size() != std::size_t(foamColumns_) * foamRows_) return 0;
        const float fx = std::clamp((x - origin_.x + body_->asset().extentX * .5f) /
                                        (body_->asset().extentX / float(foamColumns_ - 1)), 0.f, float(foamColumns_ - 1));
        const float fz = std::clamp((z - origin_.z + body_->asset().extentZ * .5f) /
                                        (body_->asset().extentZ / float(foamRows_ - 1)), 0.f, float(foamRows_ - 1));
        const unsigned x0 = unsigned(fx), z0 = unsigned(fz);
        const unsigned x1 = std::min(x0 + 1, foamColumns_ - 1), z1 = std::min(z0 + 1, foamRows_ - 1);
        const float tx = fx - float(x0), tz = fz - float(z0);
        const auto at = [&](unsigned x, unsigned z) { return foam_[std::size_t(z) * foamColumns_ + x]; };
        return (at(x0, z0) * (1 - tx) + at(x1, z0) * tx) * (1 - tz) +
               (at(x0, z1) * (1 - tx) + at(x1, z1) * tx) * tz;
    }

    // `lod` lowers only the presentation mesh (129 / 65 / 33 vertices per axis). The
    // authoritative body, event history and ledger are untouched.
    nativegpu::WaterSurfaceFrame surface(unsigned debugMode=0,unsigned lod=0) {
        nativegpu::WaterSurfaceFrame frame;
        const unsigned resolution=std::max(2u,128u>>std::min(lod,3u));frame.columns=frame.rows=resolution+1;
        frame.tick=body_->tick();frame.time=float(body_->tick())/120.f;
        frame.timelineEpoch=body_->timelineEpoch();
        frame.roughness=roughness_;frame.ior=body_->asset().material.refractionIndex;
        frame.debugMode=debugMode;
        frame.absorption={absorption_.x,absorption_.y,absorption_.z};frame.scattering={scattering_.x,scattering_.y,scattering_.z};
        // The residual field maps its texels to world = origin + local, so the published
        // origin is the footprint corner. The body keeps its origin at the centre, and
        // handing that centre over shifted every ripple half an extent away from the
        // interaction that produced it.
        frame.rippleOrigin={origin_.x-body_->asset().extentX*.5f,origin_.z-body_->asset().extentZ*.5f};
        frame.rippleExtent={body_->asset().extentX,body_->asset().extentZ};
        // The body keeps a bounded ring, not a consumed queue: two render paths in one
        // frame observe the same history and the solver drops repeated ticks.
        frame.events.reserve(body_->events().size());
        for(const auto& source:body_->events()){nativegpu::WaterRippleEvent event;
            event.x=source.x;event.z=source.z;event.radius=source.radius;event.displacement=source.displacement;
            event.directionX=source.directionX;event.directionZ=source.directionZ;event.stretch=source.stretch;
            event.tick=static_cast<std::uint32_t>(source.tick);
            event.sequence=source.sequence;event.timelineEpoch=source.timelineEpoch;
            event.normalSpeed=source.normalSpeed;frame.events.push_back(event);}
        frame.vertices.resize(frame.columns*frame.rows);
        // White water is visual state, never authority: it decays over roughly half a
        // second so a wake leaves a fading trail instead of flickering with the solver's
        // instantaneous speed. The asset's threshold describes sustained white water, so
        // the knee starts well below it; a footstep is a transient, not a standing wave.
        const float foamThreshold=std::max(body_->asset().material.foamThreshold,.05f);
        // Bulk current is not white water. The knee has to sit above ordinary wave
        // velocities, otherwise a pond in motion paints itself a flat foam sheet.
        const float foamKnee=std::max(foamThreshold,.25f);
        const float foamSpan=std::max(foamThreshold*.8f,.05f);
        // Horizontal surface speed is the wrong signal at the contact itself: a footstep
        // pushes water down, not sideways, so the depression relative to the still level is
        // what marks the footprint. Waves moving through raise the same measure, which is
        // the white water on a crest.
        const float stillSurface=origin_.y+body_->asset().stillHeightMetres;
        const std::uint64_t foamTick=body_->tick();
        float decay=1.f,stepSeconds=0.f;
        if(foamTick>foamTick_){
            stepSeconds=std::min(0.1f,float(foamTick-foamTick_)/120.f);
            decay=std::exp(-stepSeconds/.45f);
            foamTick_=foamTick;
        }
        const std::size_t count=frame.vertices.size();
        // A LOD change resizes the presentation grid; the old foam field does not map onto
        // it and is dropped instead of being resampled from unrelated texels.
        if(foamColumns_!=frame.columns||foamRows_!=frame.rows){
            foam_.assign(count,0.f);
            velocityX_.assign(count,0.f);velocityZ_.assign(count,0.f);
            foamColumns_=frame.columns;foamRows_=frame.rows;
        }
        std::vector<float> target(count,0.f);
        for(unsigned z=0;z<frame.rows;++z)for(unsigned x=0;x<frame.columns;++x){
            const float px=origin_.x+body_->asset().extentX*(float(x)/float(frame.columns-1)-.5f),pz=origin_.z+body_->asset().extentZ*(float(z)/float(frame.rows-1)-.5f);
            const auto q=body_->query(px,pz);auto& vertex=frame.vertices[z*frame.columns+x];
            const std::size_t index=std::size_t(z)*frame.columns+x;
            vertex.position[0]=px;vertex.position[1]=q.surfaceHeightMetres;vertex.position[2]=pz;
            vertex.normal[0]=q.normal.x;vertex.normal[1]=q.normal.y;vertex.normal[2]=q.normal.z;vertex.depth=q.depthMetres;
            // The visual feed is bounded; a violent transient must not fail frame
            // validation or advect a scalar across the whole footprint in one frame.
            vertex.velocity[0]=std::clamp(q.velocityX,-50.f,50.f);
            vertex.velocity[1]=std::clamp(q.velocityZ,-50.f,50.f);
            const float speed=std::sqrt(vertex.velocity[0]*vertex.velocity[0]+vertex.velocity[1]*vertex.velocity[1]);
            const float disturbed=std::clamp((std::fabs(q.surfaceHeightMetres-stillSurface)-.035f)/.060f,0.f,1.f);
            target[index]=std::max(std::clamp((speed-foamKnee)/foamSpan,0.f,1.f),disturbed);
        }
        // Widen only the fresh source. A wider contact patch reads as disturbed water
        // instead of a one-cell scratch, while the established field is left alone: a max
        // pass over the whole field every frame expands it by a cell per frame, which
        // swamped advection and made foam appear to jump upstream as well as downstream.
        std::vector<float> source(count,0.f);
        for(unsigned z=0;z<frame.rows;++z)for(unsigned x=0;x<frame.columns;++x){
            float value=target[std::size_t(z)*frame.columns+x];
            for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx){
                const int nx=int(x)+dx,nz=int(z)+dz;
                if(nx<0||nz<0||nx>=int(frame.columns)||nz>=int(frame.rows))continue;
                value=std::max(value,target[std::size_t(nz)*frame.columns+std::size_t(nx)]);
            }
            source[std::size_t(z)*frame.columns+x]=value;
        }
        // Foam is a passive scalar: it is carried by the same velocity snapshot the
        // residual uses. Without this, white water stayed pinned to the world while the
        // river moved under it.
        std::vector<float> next(count,0.f);
        const float stepX=body_->asset().extentX/float(frame.columns-1);
        const float stepZ=body_->asset().extentZ/float(frame.rows-1);
        const auto sampleFoam=[&](float px,float pz){
            float fx=(px-origin_.x+body_->asset().extentX*.5f)/stepX;
            float fz=(pz-origin_.z+body_->asset().extentZ*.5f)/stepZ;
            fx=std::clamp(fx,0.f,float(frame.columns-1));fz=std::clamp(fz,0.f,float(frame.rows-1));
            const unsigned x0=std::min(unsigned(fx),frame.columns-1),z0=std::min(unsigned(fz),frame.rows-1);
            const unsigned x1=std::min(x0+1,frame.columns-1),z1=std::min(z0+1,frame.rows-1);
            const float tx=fx-float(x0),tz=fz-float(z0);
            const auto at=[&](unsigned x,unsigned z){return foam_[std::size_t(z)*frame.columns+x];};
            return (at(x0,z0)*(1-tx)+at(x1,z0)*tx)*(1-tz)+(at(x0,z1)*(1-tx)+at(x1,z1)*tx)*tz;
        };
        for(unsigned z=0;z<frame.rows;++z)for(unsigned x=0;x<frame.columns;++x){
            const std::size_t index=std::size_t(z)*frame.columns+x;
            const float px=origin_.x+body_->asset().extentX*(float(x)/float(frame.columns-1)-.5f);
            const float pz=origin_.z+body_->asset().extentZ*(float(z)/float(frame.rows-1)-.5f);
            const float carried=stepSeconds>0?sampleFoam(px-velocityX_[index]*stepSeconds,
                                                         pz-velocityZ_[index]*stepSeconds):foam_[index];
            next[index]=std::max(source[index],carried*decay);
        }
        for(std::size_t index=0;index<count;++index){
            velocityX_[index]=frame.vertices[index].velocity[0];
            velocityZ_[index]=frame.vertices[index].velocity[1];
        }
        foam_.swap(next);
        for(std::size_t i=0;i<frame.vertices.size();++i)frame.vertices[i].foam=foam_[i];
        return frame;
    }
private:
    static std::shared_ptr<const engine::water::Asset> makeAsset() {
        auto asset = std::make_shared<engine::water::Asset>();
        asset->id = "interactive-pond-demo"; asset->backend = engine::water::Backend::Shallow;
        asset->extentX = asset->extentZ = 8.f; asset->cellMetres = .125f;
        asset->columns = asset->rows = 64; asset->stillHeightMetres = .16f;
        asset->bathymetry.heightMetres = -.24f; asset->maxCells = 4096; asset->maxSubsteps = 8;
        return asset;
    }
    std::shared_ptr<engine::water::Body> body_;
    std::uint64_t tick_ = 0;
    std::uint64_t disturbances_ = 0;
    // Per-surface-vertex white water, carried by the previous velocity snapshot and
    // decayed by simulated time. Presentation only.
    std::vector<float> foam_;
    std::vector<float> velocityX_, velocityZ_;
    unsigned foamColumns_ = 0, foamRows_ = 0;
    std::uint64_t foamTick_ = 0;
    engine::water::FootprintState footprint_{};
    engine::Vec3 origin_{},absorption_{.65f,.24f,.14f},scattering_{.035f,.12f,.095f};
    float roughness_=.09f;
};
}
