#include "game.hpp"
#include <memory>
#include <cmath>
namespace engine {
using nlohmann::json;
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Vec3,x,y,z)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Material,albedo,roughness,metalness,pattern)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MaterialDefinition,name,albedo,normal,orm,uvScale,roughness,metalness,wetness,wear,clearcoat,clearcoatRoughness)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Light,position,direction,color,intensity,range,cosOuter,cosInner,type,castSurfaceShadow,castVolumetricShadow,volumetricIntensity)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FogVolume,center,extent,density)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Lighting,ambient,skyColor,keyDirection,keyColor,keyIntensity,fogColor,fogDensity,fogHeightFalloff,fogBaseHeight,toneMap,exposure,cloudCoverage,cloudHeight,cloudThickness,cloudSpeed,quality,localShadows,ibl,ddgi,cloudShadows,temporal,maxShadowLights,fogVolumes)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Entity,components,id,name,kind,position,scale,color,material,hasMaterial,materialIndex,hasLight,lightIntensity,lightRange,lightColor,castSurfaceShadow,castVolumetricShadow,volumetricIntensity,collider,hasPhysics,dynamicBody,physicsMass,friction,restitution,destructible,strength,debrisLifetime,fragments,structural,fractureSeed,vehicleSpeed,vehicleDirection,bonded,bondedCells,voronoi)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SceneJoint,type,bodyA,bodyB,anchorA,anchorB,axis,hingeSpan,restLength,compliance,lowerLimit,upperLimit,breakForce,collideConnected,motorSpeed,motorMaxTorque,friction)
// Joints participate in the scene fingerprint: two scenes that differ only in how a door
// is hinged must not share a content hash, or a save could be restored against the wrong
// level. Baked geometry is fingerprinted separately by sceneIdentity below.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Scene,entities,lights,materials,lighting,walkSpeed,runSpeed,jumpSpeed,gravity,joints)
}
namespace engine::physics {
using nlohmann::json;
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Quat,w,x,y,z)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Mat3,m)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Body,id,entity,dynamic,debris,projectile,visible,active,sleeping,half,position,orientation,velocity,angularVelocity,mass,invMass,invInertiaLocal,invInertiaWorld,friction,restitution,linearDamping,angularDamping,sleepTimer,lifetime,color,material,texture,roughness,metalness,ageTicks)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Impact,indexA,indexB,entityA,entityB,point,normal,normalImpulse,approachSpeed)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Settings,velocityIterations,positionIterations,maxSubsteps,maxTravelPerSubstep,sleepLinear,sleepAngular,sleepDelay,restitutionThreshold,baumgarte,positionSlop,maxBiasSpeed)
}
namespace engine::fracture {
using nlohmann::json;
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Cell,center,half,volume,mass,body,detached,damage)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Bond,a,b,area,damage,broken)
}
namespace engine::animation {
using nlohmann::json;
using Json = nlohmann::json;
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(V3,x,y,z)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PoseBuffer,translation,rotation,scale,layerWeight,curves)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Selection,clip,frame,cost)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BoneMask,bits)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Event,tick,name,clip)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ControllerConfig,clips,walkSpeed,runSpeed,transitionSeconds,motionMatching,proceduralRig,muscles,softTissue,rigOverride,rig)
// The contact solver grew its own state (contact mode, sole roll, contact pivot
// and horizontal pelvis shift). Saves written before that split do not carry
// those keys, so they are read with defaults: the mode and the pivot are
// re-derived on the next tick anyway, and rejecting an in-flight save outright
// would be a worse outcome than restoring one tick of flat contact.
inline void to_json(Json& j,const animation::RigSettings& v) {
  j=Json{{"height",v.height},{"turnThreshold",v.turnThreshold},{"turnStepHeight",v.turnStepHeight},
    {"turnStepDuration",v.turnStepDuration},{"turnCooldown",v.turnCooldown},{"maxTwist",v.maxTwist},
    {"maxSlopeCosine",v.maxSlopeCosine},{"swingPrediction",v.swingPrediction},
    {"maxFootCorrection",v.maxFootCorrection},{"pelvisLower",v.pelvisLower},{"pelvisUpper",v.pelvisUpper},
    {"maxStepHeight",v.maxStepHeight},{"maxPelvisShift",v.maxPelvisShift},{"soleRoll",v.soleRoll},
    {"footLock",v.footLock},{"turnInPlace",v.turnInPlace}};
}
inline void from_json(const Json& j,animation::RigSettings& v) {
  auto number=[&](const char* key,float& out){if(j.contains(key)&&j.at(key).is_number())j.at(key).get_to(out);};
  number("height",v.height);number("turnThreshold",v.turnThreshold);number("turnStepHeight",v.turnStepHeight);
  number("turnStepDuration",v.turnStepDuration);number("turnCooldown",v.turnCooldown);number("maxTwist",v.maxTwist);
  number("maxSlopeCosine",v.maxSlopeCosine);number("swingPrediction",v.swingPrediction);
  number("maxFootCorrection",v.maxFootCorrection);number("pelvisLower",v.pelvisLower);
  number("pelvisUpper",v.pelvisUpper);number("maxStepHeight",v.maxStepHeight);
  number("maxPelvisShift",v.maxPelvisShift);number("soleRoll",v.soleRoll);
  if(j.contains("footLock")&&j.at("footLock").is_boolean())j.at("footLock").get_to(v.footLock);
  if(j.contains("turnInPlace")&&j.at("turnInPlace").is_boolean())j.at("turnInPlace").get_to(v.turnInPlace);
}
inline void to_json(Json& j,const animation::FootState& v) {
  j=Json{{"locked",v.locked},{"surface",v.surface},{"anchor",v.anchor},{"previous",v.previous},
    {"normal",v.normal},{"weight",v.weight},{"phase",v.phase},{"target",v.target},
    {"targetNormal",v.targetNormal},{"swingStart",v.swingStart},{"swingEnd",v.swingEnd},
    {"swingTime",v.swingTime},{"swingDuration",v.swingDuration},{"turnStep",v.turnStep},
    {"cooldown",v.cooldown},{"contact",v.contact},{"anchorHeading",v.anchorHeading},
    {"animated",v.animated},{"mode",v.mode},{"release",v.release},{"roll",v.roll},
    {"pivot",v.pivot},{"previousPivot",v.previousPivot},{"pivotValid",v.pivotValid},
    {"pivotRoll",v.pivotRoll},{"pivoting",v.pivoting},{"pivoted",v.pivoted},
    {"flat",v.flat},{"reachAtPlant",v.reachAtPlant}};
}
inline void from_json(const Json& j,animation::FootState& v) {
  j.at("locked").get_to(v.locked);j.at("surface").get_to(v.surface);j.at("anchor").get_to(v.anchor);
  j.at("previous").get_to(v.previous);j.at("normal").get_to(v.normal);j.at("weight").get_to(v.weight);
  j.at("phase").get_to(v.phase);j.at("target").get_to(v.target);j.at("targetNormal").get_to(v.targetNormal);
  j.at("swingStart").get_to(v.swingStart);j.at("swingEnd").get_to(v.swingEnd);
  j.at("swingTime").get_to(v.swingTime);j.at("swingDuration").get_to(v.swingDuration);
  j.at("turnStep").get_to(v.turnStep);j.at("cooldown").get_to(v.cooldown);
  j.at("contact").get_to(v.contact);j.at("anchorHeading").get_to(v.anchorHeading);
  j.at("animated").get_to(v.animated);
  if(j.contains("mode"))j.at("mode").get_to(v.mode);
  if(j.contains("release"))j.at("release").get_to(v.release);
  if(j.contains("roll"))j.at("roll").get_to(v.roll);
  if(j.contains("pivot"))j.at("pivot").get_to(v.pivot);
  if(j.contains("previousPivot"))j.at("previousPivot").get_to(v.previousPivot);
  if(j.contains("pivotValid"))j.at("pivotValid").get_to(v.pivotValid);
  if(j.contains("pivotRoll"))j.at("pivotRoll").get_to(v.pivotRoll);
  if(j.contains("pivoting"))j.at("pivoting").get_to(v.pivoting);
  // The stance budget reads the plant-time hip distance, so it has to survive a
  // save; the flat target and the pivot carry-over are re-derived on the next
  // tick and stay optional for older saves.
  if(j.contains("pivoted"))j.at("pivoted").get_to(v.pivoted);
  if(j.contains("flat"))j.at("flat").get_to(v.flat);
  if(j.contains("reachAtPlant"))j.at("reachAtPlant").get_to(v.reachAtPlant);
}
// Generic (non-foot) contacts. Serialized whole: the anchor is surface-local, so
// a save taken on a moving platform restores the hold where it belongs.
inline void to_json(Json& j,const animation::ContactConstraint& v) {
  j=Json{{"id",v.id},{"active",v.active},{"bound",v.bound},{"chain",v.chain},
    {"anchor",v.anchor},{"surface",v.surface},{"pose",v.pose},{"normal",v.normal},
    {"weight",v.weight},{"priority",v.priority},{"slip",v.slip},{"release",v.release},
    {"validUntil",v.validUntil},{"previous",v.previous},{"measured",v.measured},
    {"error",v.error}};
}
inline void from_json(const Json& j,animation::ContactConstraint& v) {
  j.at("id").get_to(v.id);j.at("active").get_to(v.active);
  if(j.contains("bound"))j.at("bound").get_to(v.bound);
  j.at("chain").get_to(v.chain);j.at("anchor").get_to(v.anchor);
  if(j.contains("surface"))j.at("surface").get_to(v.surface);
  if(j.contains("pose"))j.at("pose").get_to(v.pose);
  if(j.contains("normal"))j.at("normal").get_to(v.normal);
  if(j.contains("weight"))j.at("weight").get_to(v.weight);
  if(j.contains("priority"))j.at("priority").get_to(v.priority);
  // Saves written before the slip rule restore a pinned hold, which is the
  // behaviour they were recorded with.
  if(j.contains("slip"))j.at("slip").get_to(v.slip);
  if(j.contains("release"))j.at("release").get_to(v.release);
  if(j.contains("validUntil"))j.at("validUntil").get_to(v.validUntil);
  if(j.contains("previous"))j.at("previous").get_to(v.previous);
  if(j.contains("measured"))j.at("measured").get_to(v.measured);
  if(j.contains("error"))j.at("error").get_to(v.error);
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SoftParticle,position,previous,velocity)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SoftState,particles,previousCenter,initialized)
inline void to_json(Json& j,const animation::FeatureState& v) {
  j=Json{{"feet",v.feet},{"morphWeights",v.morphWeights},{"soft",v.soft},
    {"previousPosition",v.previousPosition},{"previousHeading",v.previousHeading},
    {"pelvisOffset",v.pelvisOffset},{"lodWeight",v.lodWeight},{"pelvisShift",v.pelvisShift},
    {"steppingFoot",v.steppingFoot},{"turnResidual",v.turnResidual},{"turnSteps",v.turnSteps},
    {"fallback",v.fallback},{"lastSwitch",v.lastSwitch},{"matchedClip",v.matchedClip},
    {"matchedSeconds",v.matchedSeconds},{"matching",v.matching},
    {"hasPreviousPosition",v.hasPreviousPosition},{"contacts",v.contacts}};
}
inline void from_json(const Json& j,animation::FeatureState& v) {
  j.at("feet").get_to(v.feet);j.at("morphWeights").get_to(v.morphWeights);j.at("soft").get_to(v.soft);
  j.at("previousPosition").get_to(v.previousPosition);j.at("previousHeading").get_to(v.previousHeading);
  j.at("pelvisOffset").get_to(v.pelvisOffset);j.at("lodWeight").get_to(v.lodWeight);
  j.at("steppingFoot").get_to(v.steppingFoot);j.at("turnResidual").get_to(v.turnResidual);
  j.at("turnSteps").get_to(v.turnSteps);j.at("fallback").get_to(v.fallback);
  j.at("lastSwitch").get_to(v.lastSwitch);j.at("matchedClip").get_to(v.matchedClip);
  j.at("matchedSeconds").get_to(v.matchedSeconds);j.at("matching").get_to(v.matching);
  j.at("hasPreviousPosition").get_to(v.hasPreviousPosition);
  if(j.contains("pelvisShift"))j.at("pelvisShift").get_to(v.pelvisShift);
  // Saves written before the contact layer simply have no holds to restore.
  if(j.contains("contacts"))j.at("contacts").get_to(v.contacts);
}
}
namespace engine::gameplay {
std::string sceneIdentity(const Scene& scene) {
    Json content(scene);
    if(scene.lighting.panorama)content["lighting"]["environment"]={{"sha256",scene.lighting.environmentHash},{"intensity",scene.lighting.environmentIntensity},{"rotation",scene.lighting.environmentRotation}};
    for(std::size_t i=0;i<scene.materials.size();++i)if(scene.materials[i].ior!=1.5f)content["materials"][i]["ior"]=scene.materials[i].ior;
    for(std::size_t i=0;i<scene.materials.size();++i){const auto& m=scene.materials[i];if(!m.coat.empty()||!m.coatRoughness.empty()||!m.coatNormal.empty())content["materials"][i]["coating"]={m.coat,m.coatRoughness,m.coatNormal};}
    for(std::size_t i=0;i<scene.materials.size();++i)if(scene.materials[i].opacity!=1)content["materials"][i]["opacity"]=scene.materials[i].opacity;
    for(std::size_t i=0;i<scene.materials.size();++i)if(scene.materials[i].unlit)content["materials"][i]["unlit"]=true;
    for(std::size_t i=0;i<scene.materials.size();++i)if(!scene.materials[i].occlusion.empty()||scene.materials[i].occlusionStrength!=1)content["materials"][i]["occlusion"]={{"texture",scene.materials[i].occlusion},{"strength",scene.materials[i].occlusionStrength}};
    for(std::size_t i=0;i<scene.materials.size();++i)if(scene.materials[i].hasTextureMappings)content["materials"][i]["textureMappings"]=materialSamplingIdentity(scene.materials[i]);
    for(std::size_t i=0;i<scene.materials.size();++i)if(!scene.materials[i].layers.empty())content["materials"][i]["layered"]=scene.materials[i].layeredAuthoring;
    for(std::size_t i=0;i<scene.materials.size();++i)if(scene.materials[i].alphaMasked)
        content["materials"][i]["alpha"]={{"mode","MASK"},{"cutoff",scene.materials[i].alphaCutoff}};
    for(std::size_t i=0;i<scene.materials.size();++i)if(scene.materials[i].alphaBlended)content["materials"][i]["alpha"]={{"mode","BLEND"}};
    for(std::size_t i=0;i<scene.materials.size();++i){const auto& material=scene.materials[i];
        if(!material.emissive.empty()||material.emissiveStrength!=1||material.emissiveFactor[0]!=1||material.emissiveFactor[1]!=1||material.emissiveFactor[2]!=1)
            content["materials"][i]["emission"]={{"texture",material.emissive},{"factor",{material.emissiveFactor[0],material.emissiveFactor[1],material.emissiveFactor[2]}},{"strength",material.emissiveStrength}};
    }
    // Include geometry content, so a save cannot restore against a different import.
    for(std::size_t i=0;i<scene.entities.size();++i) if(!scene.entities[i].meshFile.empty())
        content["entities"][i]["mesh"]={{"file",scene.entities[i].meshFile},{"sha256",scene.entities[i].meshHash}};
    for(std::size_t i=0;i<scene.entities.size();++i)if(!scene.entities[i].character.file.empty())
        content["entities"][i]["character"]=scene.entities[i].character.authoring;
    if(!scene.voronoiPackages.empty()){
        content["bakedGeometry"]=Json::array();
        for(const auto& package:scene.voronoiPackages){
            Json data={{"source",package.source},{"half",{package.half[0],package.half[1],package.half[2]}},{"volume",package.volume},{"boxVolume",package.boxVolume}};
            data["meshes"]=Json::array();for(const auto& mesh:package.meshes){Json vertices=Json::array();for(const auto& v:mesh.vertices)vertices.push_back({v.position[0],v.position[1],v.position[2],v.normal[0],v.normal[1],v.normal[2],v.uv[0],v.uv[1]});data["meshes"].push_back({{"vertices",vertices},{"indices",mesh.indices},{"min",{mesh.min[0],mesh.min[1],mesh.min[2]}},{"max",{mesh.max[0],mesh.max[1],mesh.max[2]}},{"baked",mesh.baked}});}
            data["shards"]=Json::array();for(const auto& shard:package.shards)data["shards"].push_back({{"center",{shard.center[0],shard.center[1],shard.center[2]}},{"half",{shard.half[0],shard.half[1],shard.half[2]}},{"volume",shard.volume},{"mesh",shard.mesh},{"interior",shard.meshInterior}});
            data["links"]=Json::array();for(const auto& link:package.links)data["links"].push_back({link.a,link.b,link.area});
            content["bakedGeometry"].push_back(fingerprint(data));
        }
    }
    return fingerprint(content);
}
struct NativeSnapshot {
 static Json physics(const physics::World& value) {
 Json out=Json::object();
 out["settings_"]=value.settings_;
 out["gravity_"]=value.gravity_;
 out["bodies_"]=value.bodies_;
 out["frameImpacts_"]=value.frameImpacts_;
 out["nextId_"]=value.nextId_;
 out["lastContactCount_"]=value.lastContactCount_;
 out["substepsLast_"]=value.substepsLast_;
 out["totalSteps_"]=value.totalSteps_;
 return out;
 }
 static void physics(physics::World& value,const Json& in) {
 check(in.at("bodies_").is_array()&&in.at("bodies_").size()<=30000&&in.at("frameImpacts_").is_array()&&in.at("frameImpacts_").size()<=100000,"NG-E-SNAPSHOT-STATE","Physics snapshot budget");
 in.at("settings_").get_to(value.settings_);
 in.at("gravity_").get_to(value.gravity_);
 in.at("bodies_").get_to(value.bodies_);
 in.at("frameImpacts_").get_to(value.frameImpacts_);
 in.at("nextId_").get_to(value.nextId_);
 in.at("lastContactCount_").get_to(value.lastContactCount_);
 in.at("substepsLast_").get_to(value.substepsLast_);
 in.at("totalSteps_").get_to(value.totalSteps_);
 check(value.settings_.velocityIterations>=1&&value.settings_.velocityIterations<=128&&value.settings_.positionIterations>=0&&value.settings_.positionIterations<=128&&value.settings_.maxSubsteps>=1&&value.settings_.maxSubsteps<=32&&std::isfinite(value.settings_.maxTravelPerSubstep)&&value.settings_.maxTravelPerSubstep>0,"NG-E-SNAPSHOT-STATE","Invalid physics settings");
 auto finite=[](Vec3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);};
 check(finite(value.gravity_),"NG-E-SNAPSHOT-STATE","Invalid gravity");
 std::set<unsigned> ids;
 for(const auto& body:value.bodies_) {
  const float norm=body.orientation.w*body.orientation.w+body.orientation.x*body.orientation.x+body.orientation.y*body.orientation.y+body.orientation.z*body.orientation.z;
  check(body.id>0&&body.id<value.nextId_&&ids.insert(body.id).second&&finite(body.position)&&finite(body.velocity)&&finite(body.angularVelocity)&&finite(body.half)&&body.half.x>0&&body.half.y>0&&body.half.z>0&&std::isfinite(body.mass)&&body.mass>0&&std::isfinite(norm)&&norm>.99f&&norm<1.01f,"NG-E-SNAPSHOT-STATE","Invalid rigid body");
 }
 for(const auto& impact:value.frameImpacts_)check(impact.indexA<value.bodies_.size()&&impact.indexB<value.bodies_.size(),"NG-E-SNAPSHOT-STATE","Invalid impact body");
 value.contacts_.clear();
 }
 static Json inertial(const animation::Inertializer& value) {
 Json out=Json::object();
 out["offset_"]=value.offset_;
 out["velocity_"]=value.velocity_;
 out["omega_"]=value.omega_;
 return out;
 }
 static void inertial(animation::Inertializer& value,const Json& in) {
 in.at("offset_").get_to(value.offset_);
 in.at("velocity_").get_to(value.velocity_);
 in.at("omega_").get_to(value.omega_);
 }
 static Json animation(const animation::Controller& value) {
 Json out=Json::object();
 out["config_"]=value.config_;
 out["clips_"]=value.clips_;
 out["poses_"]=value.poses_;
 out["previousPrevious_"]=value.previousPrevious_;
 out["front_"]=value.front_;
 out["selection_"]=value.selection_;
 out["state_"]=value.state_;
 out["tick_"]=value.tick_;
 out["stateTicks_"]=value.stateTicks_;
 out["transitionTick_"]=value.transitionTick_;
 out["montageTicks_"]=value.montageTicks_;
 out["initialized_"]=value.initialized_;
 out["transitioning_"]=value.transitioning_;
 out["montageActive_"]=value.montageActive_;
 out["montageClip_"]=value.montageClip_;
 out["montageMask_"]=value.montageMask_;
 out["montageWeight_"]=value.montageWeight_;
 out["speed_"]=value.speed_;
 out["locomotionSeconds_"]=value.locomotionSeconds_;
 out["playbackRate_"]=value.playbackRate_;
 out["stateTransitionTick_"]=value.stateTransitionTick_;
 out["stateTransitionActive_"]=value.stateTransitionActive_;
 out["rootMotion_"]=value.rootMotion_;
 out["events_"]=value.events_;
 out["features"]=value.features_;
 out["inertializer"]=inertial(value.inertializer_);
 out["stageHashes"]=Json::array();for(const auto& stage:value.graph_.stages_)out["stageHashes"].push_back(stage.hash);
 out["deformScale"]=value.graph_.deformScale_;
 return out;
 }
 static void animation(animation::Controller& value,const Json& in) {
 check(in.at("poses_").is_array()&&in.at("poses_").size()==2,"NG-E-SNAPSHOT-ANIMATION","Invalid pose buffers");
 auto pose=[](const Json& p){check(p.at("translation").size()==animation::MaxBones&&p.at("rotation").size()==animation::MaxBones&&p.at("scale").size()==animation::MaxBones,"NG-E-SNAPSHOT-ANIMATION","Invalid pose dimensions");};
 pose(in.at("poses_")[0]);pose(in.at("poses_")[1]);pose(in.at("previousPrevious_"));
 in.at("config_").get_to(value.config_);
 in.at("clips_").get_to(value.clips_);
 in.at("poses_").get_to(value.poses_);
 in.at("previousPrevious_").get_to(value.previousPrevious_);
 in.at("front_").get_to(value.front_);
 in.at("selection_").get_to(value.selection_);
 in.at("state_").get_to(value.state_);
 in.at("tick_").get_to(value.tick_);
 in.at("stateTicks_").get_to(value.stateTicks_);
 in.at("transitionTick_").get_to(value.transitionTick_);
 in.at("montageTicks_").get_to(value.montageTicks_);
 in.at("initialized_").get_to(value.initialized_);
 in.at("transitioning_").get_to(value.transitioning_);
 in.at("montageActive_").get_to(value.montageActive_);
 in.at("montageClip_").get_to(value.montageClip_);
 in.at("montageMask_").get_to(value.montageMask_);
 in.at("montageWeight_").get_to(value.montageWeight_);
 in.at("speed_").get_to(value.speed_);
 value.locomotionSeconds_=in.value("locomotionSeconds_",0.0);
 value.playbackRate_=in.value("playbackRate_",1.f);
 value.stateTransitionTick_=in.value("stateTransitionTick_",std::uint64_t(0));
 value.stateTransitionActive_=in.value("stateTransitionActive_",false);
 check(std::isfinite(value.locomotionSeconds_)&&value.locomotionSeconds_>=0&&std::isfinite(value.playbackRate_)&&value.playbackRate_>=.1f&&value.playbackRate_<=10.f,"NG-E-SNAPSHOT-ANIMATION","Invalid locomotion clock");
 in.at("rootMotion_").get_to(value.rootMotion_);
 in.at("events_").get_to(value.events_);
 check(in.at("features").at("soft").is_array()&&in.at("features").at("soft").size()<=16,"NG-E-SNAPSHOT-ANIMATION","Soft snapshot budget");
 in.at("features").get_to(value.features_);
 inertial(value.inertializer_,in.at("inertializer"));
 for(std::size_t i=0;i<value.graph_.stages_.size();++i)value.graph_.stages_[i].hash=in.at("stageHashes").at(i);
 value.graph_.deformScale_=in.at("deformScale");
 check(value.front_<2 && static_cast<unsigned>(value.state_)<=4,"NG-E-SNAPSHOT-ANIMATION","Invalid animation state");
 if(value.asset_) {
  check(value.features_.soft.size()==value.asset_->softRegions.size()&&value.features_.matchedClip<value.asset_->clips.size()&&std::isfinite(value.features_.matchedSeconds)&&value.features_.matchedSeconds>=0,"NG-E-SNAPSHOT-ANIMATION","Invalid feature state");
  auto finite=[](animation::V3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);};
  check(finite(value.features_.previousPosition)&&std::isfinite(value.features_.previousHeading)&&std::isfinite(value.features_.pelvisOffset)&&std::isfinite(value.features_.lodWeight)&&value.features_.lodWeight>=0&&value.features_.lodWeight<=1,"NG-E-SNAPSHOT-ANIMATION","Invalid feature history");
  check(finite(value.features_.pelvisShift)&&std::abs(value.features_.pelvisShift.y)<1e-4f,"NG-E-SNAPSHOT-ANIMATION","Invalid pelvis compensation");
  check(value.features_.steppingFoot>=-1&&value.features_.steppingFoot<2&&std::isfinite(value.features_.turnResidual)&&value.features_.turnResidual>=0&&static_cast<unsigned>(value.features_.fallback)<=static_cast<unsigned>(animation::RigFallback::Teleport),"NG-E-SNAPSHOT-ANIMATION","Invalid turn state");
  check(value.features_.contacts.size()<=64,"NG-E-SNAPSHOT-ANIMATION","Contact budget exceeded");
  for(const auto& c:value.features_.contacts){
    check(std::isfinite(c.weight)&&c.weight>=0&&c.weight<=1&&std::isfinite(c.priority)&&c.priority>=0&&c.priority<=1&&finite(c.anchor)&&finite(c.pose)&&finite(c.normal)&&finite(c.previous)&&std::isfinite(c.error)&&static_cast<unsigned>(c.release)<=static_cast<unsigned>(animation::ContactRelease::Expired),"NG-E-SNAPSHOT-ANIMATION","Invalid contact constraint");
  }
  for(const auto& foot:value.features_.feet){
    check(finite(foot.anchor)&&finite(foot.animated)&&finite(foot.previous)&&finite(foot.normal)&&std::isfinite(foot.weight)&&foot.weight>=0&&foot.weight<=1&&finite(foot.target)&&finite(foot.targetNormal)&&finite(foot.swingStart)&&finite(foot.swingEnd)&&std::isfinite(foot.swingTime)&&foot.swingTime>=0&&std::isfinite(foot.swingDuration)&&foot.swingDuration>=0&&std::isfinite(foot.cooldown)&&foot.cooldown>=0&&std::isfinite(foot.anchorHeading)&&static_cast<unsigned>(foot.phase)<=static_cast<unsigned>(animation::FootPhase::Support),"NG-E-SNAPSHOT-ANIMATION","Invalid foot state");
    check(std::isfinite(foot.roll)&&std::abs(foot.roll)<=1.6f&&finite(foot.pivot)&&finite(foot.previousPivot)&&static_cast<unsigned>(foot.mode)<=static_cast<unsigned>(animation::ContactMode::Toe)&&static_cast<unsigned>(foot.release)<=static_cast<unsigned>(animation::ContactRelease::Expired),"NG-E-SNAPSHOT-ANIMATION","Invalid contact constraint");
  }
  for(float w:value.features_.morphWeights)check(std::isfinite(w)&&w>=0&&w<=1,"NG-E-SNAPSHOT-ANIMATION","Invalid morph weight");
  for(const auto& region:value.features_.soft)for(const auto& p:region.particles)check(finite(p.position)&&finite(p.previous)&&finite(p.velocity),"NG-E-SNAPSHOT-ANIMATION","Invalid soft particle");
  check(value.selection_.clip<value.asset_->clips.size() && value.montageClip_<value.asset_->clips.size(),"NG-E-SNAPSHOT-ANIMATION","Invalid animation clip");
  check(value.selection_.frame<value.asset_->clips[value.selection_.clip].frameCount,"NG-E-SNAPSHOT-ANIMATION","Invalid animation frame");
  value.rebuildDeformation();
 }
 }
 static Json rng() {
 const auto& r=determinism::registry(); Json streams=Json::array();
 for(const auto& stream:r.streams_) streams.push_back({{"name",stream.name_},{"state",stream.state_},{"counter",stream.counter_}});
 return {{"seed",r.seed_},{"streams",streams}};
 }
 static determinism::Registry rng(const Json& in) {
 determinism::Registry candidate;candidate.reset(in.at("seed"));
 for(const auto& saved:in.at("streams")) {auto& stream=candidate.stream(saved.at("name"));saved.at("state").get_to(stream.state_);stream.counter_=saved.at("counter");}
 return candidate;
 }
 static Json game(const Game& value) {
 Json out=Json::object();
 out["vehicleSpawns_"]=value.vehicleSpawns_;
 out["animationEvents_"]=value.animationEvents_;
 out["spawn_"]=value.spawn_;
 out["player_"]=value.player_;
 out["verticalVelocity_"]=value.verticalVelocity_;
 out["yaw_"]=value.yaw_;
 out["bodyYaw_"]=value.bodyYaw_;
 out["visualY_"]=value.visualY_;
 out["stepGrace_"]=value.stepGrace_;
 out["pitch_"]=value.pitch_;
 out["animationPhase_"]=value.animationPhase_;
 out["animationSpeed_"]=value.animationSpeed_;
 out["accumulator_"]=value.accumulator_;
 out["grounded_"]=value.grounded_;
 out["hasKey_"]=value.hasKey_;
 out["doorOpen_"]=value.doorOpen_;
 out["complete_"]=value.complete_;
 out["jumpWasDown_"]=value.jumpWasDown_;
 out["interactWasDown_"]=value.interactWasDown_;
 out["fireWasDown_"]=value.fireWasDown_;
 out["blastWasDown_"]=value.blastWasDown_;
 out["jumpPending_"]=value.jumpPending_;
 out["interactPending_"]=value.interactPending_;
 out["firePending_"]=value.firePending_;
 out["blastPending_"]=value.blastPending_;
 out["feedbackTime_"]=value.feedbackTime_;
 out["feedback_"]=value.feedback_;
 out["ticks_"]=value.ticks_;
 out["appliedAnimationRequest"]=value.appliedAnimationRequest_;
 out["publishedVelocity"]=value.publishedVelocity_;
 out["runtimeBodies"]=value.runtimeBodies_;out["publishedTransforms"]=value.publishedTransforms_;
 out["entityBody_"]=value.entityBody_;
 out["fractured_"]=value.fractured_;
 out["damage_"]=value.damage_;
 out["fractureCount_"]=value.fractureCount_;
 out["fragmentsSpawned_"]=value.fragmentsSpawned_;
 out["structuralCollapses_"]=value.structuralCollapses_;
 out["projectileCount_"]=value.projectileCount_;
 out["explosionCount_"]=value.explosionCount_;
 out["impactCount_"]=value.impactCount_;
 out["destructibleCount_"]=value.destructibleCount_;
 out["peakImpactImpulse_"]=value.peakImpactImpulse_;
 out["totalImpactEnergy_"]=value.totalImpactEnergy_;
 out["bondedHoles_"]=value.bondedHoles_;
 out["bondedBrokenBonds_"]=value.bondedBrokenBonds_;
 {
  // Joints are rebuilt from the scene, so only the state a solve can change is stored.
  // Without this a save/load would silently drop every hinge, because restore() replaces
  // the rigid world wholesale.
  Json joints=Json::array();
  for(const auto& joint:value.physics_.joints()){
   Json entry=Json::object();
   // No id: joint ids are allocated per world and a restored world continues its own
   // counter, so storing them would make two equivalent worlds compare unequal. Order is
   // the identity, and restore matches by position.
   entry["broken"]=joint.broken;
   entry["angle"]=joint.angle;
   joints.push_back(std::move(entry));
  }
  out["joints_"]=std::move(joints);
 }
 out["bondedActive_"]=value.bondedActive_;
 {
  // Only bonded entities are serialised. A cell keeps its body index so the rigid
  // body mapping survives the restore; cells that never got a body stay at -1.
  Json structures=Json::array();
  for(std::size_t index=0;index<value.bonded_.size();++index){
   const fracture::Structure& structure=value.bonded_[index];
   if(structure.cellCount()==0)continue;
   Json entry=Json::object();
   entry["entity"]=index;
   entry["cells"]=structure.cells();
   entry["bonds"]=structure.bonds();
   structures.push_back(std::move(entry));
  }
  out["bonded_"]=std::move(structures);
 }
 out["format"]="nativegpu.simulation/1";
 out["content"]=value.contentId_;
 out["animationAsset"]=value.animation_.asset_?value.animation_.asset_->contentHash:"";
 out["gameplay"]=value.gameplay_.snapshot();out["physics"]=physics(value.physics_);out["animation"]=animation(value.animation_);out["rng"]=rng();
 Json positions=Json::array();for(const auto& e:value.scene_.entities)positions.push_back(e.position);out["scenePositions"]=positions;
 Json scales=Json::array();for(const auto& e:value.scene_.entities)scales.push_back(e.scale);out["sceneScales"]=scales;
 return out;
 }
 static void game(Game& value,const Json& in) {
 in.at("vehicleSpawns_").get_to(value.vehicleSpawns_);
 in.at("animationEvents_").get_to(value.animationEvents_);
 in.at("spawn_").get_to(value.spawn_);
 in.at("player_").get_to(value.player_);
 in.at("verticalVelocity_").get_to(value.verticalVelocity_);
 in.at("yaw_").get_to(value.yaw_);
 if(in.contains("bodyYaw_"))in.at("bodyYaw_").get_to(value.bodyYaw_);else value.bodyYaw_=value.yaw_;
 if(in.contains("visualY_"))in.at("visualY_").get_to(value.visualY_);else value.visualY_=value.player_.y;
 if(in.contains("stepGrace_"))in.at("stepGrace_").get_to(value.stepGrace_);else value.stepGrace_=0.f;
 in.at("pitch_").get_to(value.pitch_);
 in.at("animationPhase_").get_to(value.animationPhase_);
 in.at("animationSpeed_").get_to(value.animationSpeed_);
 in.at("accumulator_").get_to(value.accumulator_);
 in.at("grounded_").get_to(value.grounded_);
 in.at("hasKey_").get_to(value.hasKey_);
 in.at("doorOpen_").get_to(value.doorOpen_);
 in.at("complete_").get_to(value.complete_);
 in.at("jumpWasDown_").get_to(value.jumpWasDown_);
 in.at("interactWasDown_").get_to(value.interactWasDown_);
 in.at("fireWasDown_").get_to(value.fireWasDown_);
 in.at("blastWasDown_").get_to(value.blastWasDown_);
 in.at("jumpPending_").get_to(value.jumpPending_);
 in.at("interactPending_").get_to(value.interactPending_);
 in.at("firePending_").get_to(value.firePending_);
 in.at("blastPending_").get_to(value.blastPending_);
 in.at("feedbackTime_").get_to(value.feedbackTime_);
 in.at("feedback_").get_to(value.feedback_);
 in.at("ticks_").get_to(value.ticks_);
 in.at("appliedAnimationRequest").get_to(value.appliedAnimationRequest_);
 in.at("publishedVelocity").get_to(value.publishedVelocity_);
 in.at("runtimeBodies").get_to(value.runtimeBodies_);value.publishedTransforms_=in.at("publishedTransforms");
 in.at("entityBody_").get_to(value.entityBody_);
 in.at("fractured_").get_to(value.fractured_);
 in.at("damage_").get_to(value.damage_);
 in.at("fractureCount_").get_to(value.fractureCount_);
 in.at("fragmentsSpawned_").get_to(value.fragmentsSpawned_);
 in.at("structuralCollapses_").get_to(value.structuralCollapses_);
 in.at("projectileCount_").get_to(value.projectileCount_);
 in.at("explosionCount_").get_to(value.explosionCount_);
 in.at("impactCount_").get_to(value.impactCount_);
 in.at("destructibleCount_").get_to(value.destructibleCount_);
 in.at("peakImpactImpulse_").get_to(value.peakImpactImpulse_);
 in.at("totalImpactEnergy_").get_to(value.totalImpactEnergy_);
 in.at("bondedHoles_").get_to(value.bondedHoles_);
 in.at("bondedBrokenBonds_").get_to(value.bondedBrokenBonds_);
 value.gameplay_.restore(in.at("gameplay"));physics(value.physics_,in.at("physics"));animation(value.animation_,in.at("animation"));
 {
  // The rigid world was just replaced, so the authored joints are rebuilt against the
  // restored bodies. joints() is cleared first because the previous world's joints are
  // still there: appending would double every hinge. State is matched by position, not
  // by id, because the id counter is not part of the snapshot and a restored world would
  // otherwise look up ids it never issued.
  value.physics_.joints().clear();
  value.createJoints();
  if(in.contains("joints_")){
   check(in.at("joints_").is_array(),"NG-E-SNAPSHOT-STATE","Invalid joint table");
   auto& live=value.physics_.joints();
   for(std::size_t index=0;index<in.at("joints_").size()&&index<live.size();++index){
    const auto& entry=in.at("joints_")[index];
    live[index].broken=entry.at("broken").get<bool>();
    live[index].angle=entry.at("angle").get<float>();
   }
  }
 }
 {
  // Reconstruct immutable grids AND baked Voronoi topology from the initial scene.
  // A save only supplies damage/broken bonds/body mappings. Hull storage is rebuilt
  // with the same mesh indices and restored body pointers are rebound below.
  auto topology=std::make_unique<Game>();topology->scene_=value.initialScene_;topology->initializePhysics();
  value.bonded_=std::move(topology->bonded_);value.convexMeshes_=std::move(topology->convexMeshes_);
  value.hullPoints_=std::move(topology->hullPoints_);value.hullIndices_=std::move(topology->hullIndices_);
  in.at("bondedActive_").get_to(value.bondedActive_);
  check(value.bondedActive_.size()==value.scene_.entities.size(),"NG-E-SNAPSHOT-STATE","Bonded active size changed");
  check(in.at("bonded_").is_array(),"NG-E-SNAPSHOT-STATE","Invalid bonded table");
  std::set<std::size_t> seenStructures;std::set<int> seenCellBodies;std::size_t expectedStructures=0;
  for(const auto& structure:value.bonded_)if(structure.cellCount())++expectedStructures;
  check(in.at("bonded_").size()==expectedStructures,"NG-E-SNAPSHOT-STATE","Bonded topology table changed");
  for(const auto& entry:in.at("bonded_")){
   const std::size_t index=entry.at("entity").get<std::size_t>();
   check(index<value.bonded_.size(),"NG-E-SNAPSHOT-STATE","Bonded entity out of range");
   check(seenStructures.insert(index).second,"NG-E-SNAPSHOT-STATE","Duplicate bonded entity");
   const Entity& source=value.scene_.entities[index];
   check(source.bonded,"NG-E-SNAPSHOT-STATE","Bonded state on a non-bonded entity");
   fracture::Structure& structure=value.bonded_[index];
   check(entry.at("cells").size()==structure.cellCount(),"NG-E-SNAPSHOT-STATE","Bonded cell count changed");
   check(entry.at("bonds").size()==structure.bondCount(),"NG-E-SNAPSHOT-STATE","Bonded bond count changed");
   const std::vector<fracture::Cell> savedCells=entry.at("cells").get<std::vector<fracture::Cell>>();
   const std::vector<fracture::Bond> savedBonds=entry.at("bonds").get<std::vector<fracture::Bond>>();
   for(std::size_t cell=0;cell<savedCells.size();++cell){
    fracture::Cell& live=structure.cells()[cell];
    const auto& saved=savedCells[cell];const auto equal=[](Vec3 a,Vec3 b){return a.x==b.x&&a.y==b.y&&a.z==b.z;};
    check(equal(saved.center,live.center)&&equal(saved.half,live.half)&&saved.volume==live.volume&&saved.mass==live.mass&&std::isfinite(saved.damage)&&saved.damage>=0,"NG-E-SNAPSHOT-STATE","Bonded immutable cell changed");
    live.body=savedCells[cell].body;
    live.detached=savedCells[cell].detached;
    live.damage=savedCells[cell].damage;
    check(live.body<0||static_cast<std::size_t>(live.body)<value.physics_.bodies_.size(),"NG-E-SNAPSHOT-STATE","Invalid bonded cell body");
    if(live.body>=0){check(seenCellBodies.insert(live.body).second,"NG-E-SNAPSHOT-STATE","Duplicate bonded cell body");
     if(live.mesh>=0){const auto mesh=static_cast<std::size_t>(live.mesh);check(mesh<value.hullPoints_.size(),"NG-E-SNAPSHOT-STATE","Missing baked hull");auto& body=value.physics_.bodies_[static_cast<std::size_t>(live.body)];body.hullPoints=value.hullPoints_[mesh].data();body.hullPointCount=static_cast<int>(value.hullPoints_[mesh].size());body.hullIndices=value.hullIndices_[mesh].data();body.hullIndexCount=static_cast<int>(value.hullIndices_[mesh].size());}
    }
   }
   for(std::size_t bond=0;bond<savedBonds.size();++bond){
    const auto& saved=savedBonds[bond];const auto& live=structure.bonds()[bond];
    check(saved.a==live.a&&saved.b==live.b&&saved.area==live.area&&std::isfinite(saved.damage)&&saved.damage>=0,"NG-E-SNAPSHOT-STATE","Bonded immutable bond changed");
    structure.bonds()[bond].damage=savedBonds[bond].damage;
    structure.bonds()[bond].broken=savedBonds[bond].broken;
   }
  }
 }
 check(in.at("scenePositions").size()==value.scene_.entities.size(),"NG-E-SNAPSHOT-STATE","Scene size changed");
 check(in.at("sceneScales").size()==value.scene_.entities.size(),"NG-E-SNAPSHOT-STATE","Scene scale size changed");
 for(std::size_t i=0;i<value.scene_.entities.size();++i){in.at("scenePositions")[i].get_to(value.scene_.entities[i].position);in.at("sceneScales")[i].get_to(value.scene_.entities[i].scale);}
 check(value.entityBody_.size()==value.scene_.entities.size()&&value.fractured_.size()==value.scene_.entities.size()&&value.damage_.size()==value.scene_.entities.size(),"NG-E-SNAPSHOT-STATE","Entity mapping mismatch");
 for(int index:value.entityBody_)check(index<0||static_cast<std::size_t>(index)<value.physics_.bodies_.size(),"NG-E-SNAPSHOT-STATE","Invalid body mapping");
 for(const auto& [id,index]:value.runtimeBodies_)check(index>=0&&static_cast<std::size_t>(index)<value.physics_.bodies_.size(),"NG-E-SNAPSHOT-STATE","Invalid runtime body mapping");
 check(value.publishedTransforms_.is_object(),"NG-E-SNAPSHOT-STATE","Invalid published transforms");
 for(auto it=value.publishedTransforms_.begin();it!=value.publishedTransforms_.end();++it)value.gameplay_.world.registry.normalize("transform",it.value());
 check(std::isfinite(value.accumulator_)&&value.accumulator_>=0&&value.accumulator_<.101,"NG-E-SNAPSHOT-STATE","Invalid accumulator");
 }
 static void restore(Game& value,const Json& state) {
 check(state.at("format")=="nativegpu.simulation/1"&&state.at("content")==value.contentId_,"NG-E-SNAPSHOT-CONTENT","Scene content changed");
 check(state.at("animationAsset")== (value.animation_.asset_?value.animation_.asset_->contentHash:""),"NG-E-SNAPSHOT-CONTENT","Animation asset changed");
 // Validate into a separate world before touching the running simulation.
 auto candidate=std::make_unique<Game>();candidate->initialScene_=value.initialScene_;candidate->scene_=value.scene_;
 candidate->contentId_=value.contentId_;
 if(value.animation_.asset_) candidate->animation_.initialize(value.animation_.asset_,value.animation_.config_);
 candidate->gameplay_=value.gameplay_;
 game(*candidate,state);
 auto restoredRng=rng(state.at("rng"));
 game(value,state);determinism::registry()=std::move(restoredRng);
 }
};
}
namespace engine {
gameplay::Json Game::snapshot() const {return gameplay::NativeSnapshot::game(*this);}
void Game::restore(const gameplay::Json& state) {gameplay::NativeSnapshot::restore(*this,state);}
void Game::save(const std::string& path) const {gameplay::atomicSave(path,snapshot());}
void Game::load(const std::string& path) {restore(gameplay::verifiedLoad(path));}
}
