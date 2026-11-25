#include "runtime.hpp"
#include "../assets/sha256.hpp"
#include "../../../third_party/json.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <set>

namespace engine::effects {
using Json = nlohmann::json;
Error::Error(std::string value, std::string message) : std::runtime_error(std::move(message)), code(std::move(value)) {}
namespace {
void require(bool ok, const std::string& message) {
    if (!ok) throw Error("NG-E-EFFECT-ASSET", message);
}
void keys(const Json& value, std::initializer_list<const char*> required,
          std::initializer_list<const char*> optional = {}) {
    require(value.is_object(), "Expected effect object");
    std::set<std::string> allowed;
    for (auto key : required) { allowed.insert(key); require(value.contains(key), std::string("Missing effect field: ")+key); }
    for (auto key : optional) allowed.insert(key);
    for (const auto& item : value.items()) require(allowed.count(item.key()) != 0, "Unsupported effect field: "+item.key());
}
double scalar(const Json& value, double lo, double hi) {
    require(value.is_number(), "Effect scalar must be numeric");
    const auto result = value.get<double>();
    require(std::isfinite(result) && result >= lo && result <= hi, "Effect scalar outside range");
    return result;
}
std::uint32_t integer(const Json& value, std::uint32_t lo, std::uint32_t hi) {
    const auto result = scalar(value, lo, hi);
    require(std::floor(result) == result, "Effect integer has a fractional part");
    return static_cast<std::uint32_t>(result);
}
std::string text(const Json& value) { require(value.is_string(), "Expected effect string"); return value.get<std::string>(); }
std::string id(const Json& value) {
    auto result = text(value);
    require(!result.empty() && result.size() <= 64 && result[0] >= 'a' && result[0] <= 'z', "Invalid effect identifier");
    for (char c : result) require((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-', "Invalid effect identifier");
    return result;
}
std::string reference(const Json& value) {
    auto result = text(value);
    require(!result.empty() && result.size() <= 240, "Invalid effect dependency");
    std::string part;
    for (char c : result + '/') {
        if (c == '/') {
            require(!part.empty() && part != "." && part != ".." && part[0] != '.', "Invalid effect dependency component");
            part.clear();
        } else {
            require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '-' || c == '.', "Invalid effect dependency character");
            part += c;
        }
    }
    return result;
}
void array(const Json& value, std::size_t min, std::size_t max) {
    require(value.is_array() && value.size() >= min && value.size() <= max, "Invalid effect array length");
}
bool boolean(const Json& value) { require(value.is_boolean(), "Expected effect boolean"); return value.get<bool>(); }
Vec3 vector(const Json& value, double lo = -100000, double hi = 100000) {
    array(value,3,3);
    return {static_cast<float>(scalar(value[0],lo,hi)),static_cast<float>(scalar(value[1],lo,hi)),static_cast<float>(scalar(value[2],lo,hi))};
}
std::string read(const std::filesystem::path& path, std::size_t maximum = 8 * 1024 * 1024) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    require(!error && size <= maximum, "Effect input missing or too large: "+path.u8string());
    std::ifstream stream(path,std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size),'\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(size));
    require(stream.gcount() == static_cast<std::streamsize>(size) && stream.peek() == std::char_traits<char>::eof(), "Effect input changed during read");
    return bytes;
}
bool hashText(const std::string& hash) {
    return hash.size() == 64 && std::all_of(hash.begin(),hash.end(),[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
Json authenticated(const std::string& bytes, const char* schema, const char* hashKey) {
    require(bytes.size() <= 8 * 1024 * 1024, "Effect container too large");
    auto outer = Json::parse(bytes);
    require(outer.is_object() && outer.value("schema",std::string()) == schema && outer.contains("payloadJson"), "Unsupported effect cook schema");
    const auto payload = text(outer.at("payloadJson"));
    const auto expected = text(outer.at(hashKey));
    require(hashText(expected) && sha256(payload) == expected, "Effect payload SHA-256 mismatch");
    auto parsed = Json::parse(payload);
    require(parsed.is_object() && parsed.value("schema",std::string()) == schema, "Effect payload schema mismatch");
    return parsed;
}
Module parseModule(const Json& value) {
    keys(value,{"id","type","params"},{"after"}); id(value.at("id"));
    const auto kind = text(value.at("type")); const auto& p = value.at("params");
    Module result;
    if (kind == "initialize") {
        keys(p,{"velocity","color","size"}); result.kind = ModuleKind::Initialize;
        result.vector = vector(p.at("velocity"),-10000,10000);
        array(p.at("color"),4,4); array(p.at("size"),2,2);
        for (unsigned i=0;i<4;++i) result.color[i]=static_cast<float>(scalar(p.at("color")[i],0,i==3?1:64));
        for (unsigned i=0;i<2;++i) result.size[i]=static_cast<float>(scalar(p.at("size")[i],0,10000));
    } else if (kind == "acceleration") {
        keys(p,{"value"}); result.kind=ModuleKind::Acceleration; result.vector=vector(p.at("value"),-10000,10000);
    } else if (kind == "drag") {
        keys(p,{"coefficient"}); result.kind=ModuleKind::Drag; result.coefficient=static_cast<float>(scalar(p.at("coefficient"),0,10000));
    } else if (kind == "curlNoise") {
        keys(p,{"amplitude","frequency"}); result.kind=ModuleKind::CurlNoise;
        result.coefficient=static_cast<float>(scalar(p.at("amplitude"),0,10000)); result.frequency=static_cast<float>(scalar(p.at("frequency"),.0001,10000));
    } else if (kind == "colorOverLife" || kind == "sizeOverLife") {
        keys(p,{"keys"}); const bool color=kind=="colorOverLife";
        result.kind=color?ModuleKind::ColorCurve:ModuleKind::SizeCurve; array(p.at("keys"),2,64);
        float last=-1;
        for (const auto& key:p.at("keys")) {
            keys(key,{"t","value"}); CurveKey target;
            target.t=static_cast<float>(scalar(key.at("t"),0,1)); require(target.t>last,"Curve times must strictly increase"); last=target.t;
            array(key.at("value"),color?4:2,color?4:2);
            for (unsigned i=0;i<(color?4u:2u);++i) target.value[i]=static_cast<float>(scalar(key.at("value")[i],0,color?(i==3?1:64):10000));
            result.keys.push_back(target);
        }
        require(result.keys.front().t==0 && result.keys.back().t==1,"Curve must cover [0,1]");
    } else if (kind == "velocityField") {
        keys(p,{"channel","response"});result.kind=ModuleKind::VelocityField;result.fieldChannel=text(p.at("channel"));
        require(result.fieldChannel=="windVelocity"||result.fieldChannel=="waterVelocity","Velocity field requires a velocity channel");
        result.coefficient=static_cast<float>(scalar(p.at("response"),0,10000));
    } else if (kind == "fieldDeposit") {
        keys(p,{"channel","quantum"});result.kind=ModuleKind::FieldDeposit;result.fieldChannel=text(p.at("channel"));
        require(result.fieldChannel=="density"||result.fieldChannel=="moisture","Field deposit requires a nonnegative scalar channel");
        result.fieldQuantum=integer(p.at("quantum"),1,UINT32_MAX);
    } else if (kind == "fieldCoupling") {
        keys(p,{"readChannels","writeChannels"}); result.kind=ModuleKind::FieldCoupling;
        for (const auto* name:{"readChannels","writeChannels"}) {
            array(p.at(name),0,5); std::set<std::string> seen;
            for (const auto& c:p.at(name)) {
                const auto channel=text(c);
                require(channel=="density"||channel=="temperatureKelvin"||channel=="windVelocity"||channel=="waterVelocity"||channel=="moisture","Unknown effect field channel");
                require(seen.insert(channel).second,"Duplicate effect channel");
            }
        }
        require(!p.at("readChannels").empty()||!p.at("writeChannels").empty(),"Empty field coupling");
    } else require(false,"Unknown effect module: "+kind);
    return result;
}
std::vector<Module> modules(const Json& values) {
    array(values,1,32); std::map<std::string,const Json*> source;
    for (const auto& value:values) require(source.emplace(id(value.at("id")),&value).second,"Duplicate effect module id");
    std::vector<Module> result; std::map<std::string,int> visited;
    std::function<void(const std::string&)> visit=[&](const std::string& name) {
        require(source.count(name)!=0,"Unknown effect module dependency");
        require(visited[name]!=1,"Cyclic effect modules"); if (visited[name]==2) return;
        visited[name]=1; const auto& value=*source.at(name);
        if (value.contains("after")) {
            array(value.at("after"),0,32); std::set<std::string> seen;
            for (const auto& dependency:value.at("after")) { const auto d=id(dependency); require(seen.insert(d).second,"Duplicate dependency"); visit(d);
                require(text(value.at("type"))=="fieldDeposit"||text(source.at(d)->at("type"))!="fieldDeposit","Simulation module cannot depend on a postSimulation field deposit"); }
        }
        result.push_back(parseModule(value)); visited[name]=2;
    };
    for (const auto& value:values) visit(id(value.at("id")));
    require(result.front().kind==ModuleKind::Initialize && std::count_if(result.begin(),result.end(),[](const Module& m) { return m.kind==ModuleKind::Initialize; })==1,
            "Exactly one initialize module must execute first");
    return result;
}
}

std::string fieldDepositError(const Asset& asset){
    std::map<std::string,std::uint64_t> quanta;
    for(const auto& e:asset.emitters){
        for(const auto& m:e.modules){
            if(m.kind!=ModuleKind::FieldDeposit)continue;
            if(!asset.hasField||!asset.maxAlive||!m.fieldQuantum||(m.fieldChannel!="density"&&m.fieldChannel!="moisture")||std::find(asset.fieldChannels.begin(),asset.fieldChannels.end(),m.fieldChannel)==asset.fieldChannels.end())return "Field deposit requires a declared nonnegative scalar channel and positive quantum";
            if((asset.fieldLevel!="local"&&asset.fieldLevel!="worldVisible")||!std::isfinite(asset.fieldVoxelMetres)||asset.fieldVoxelMetres<.001f||asset.fieldVoxelMetres>100)return "Invalid deposit field grid";
            for(auto d:asset.fieldDimensions)if(d<8||d>256||d%8)return "Invalid deposit field dimensions";
            auto& sum=quanta[m.fieldChannel];const auto upper=UINT32_MAX/asset.maxAlive;
            if(m.fieldQuantum>upper||sum>upper-m.fieldQuantum)return "Field deposit conservative capacity times quantum sum exceeds UInt32";
            sum+=m.fieldQuantum;
        }
    }
    return {};
}

namespace {
smoke::EffectConfig parseSmoke(const Json& value,const Asset& asset){
    keys(value,{"dimensions","cellMetres","originOffset","channels","sources","tailTicks"},{"parameters","unitsPerSource","optics"});smoke::EffectConfig c;
    array(value.at("dimensions"),3,3);for(unsigned a=0;a<3;++a)c.solver.dimensions[a]=integer(value.at("dimensions")[a],2,128);
    c.solver.cellMetres=static_cast<float>(scalar(value.at("cellMetres"),.01,10));c.originOffset=vector(value.at("originOffset"));c.tailTicks=integer(value.at("tailTicks"),0,432000);c.solver.channels=0;array(value.at("channels"),1,3);std::set<std::string> channels;
    for(const auto& channel:value.at("channels")){const auto n=text(channel);require(channels.insert(n).second,"Duplicate smoke channel");require(n=="density"||n=="temperatureKelvin"||n=="fuel","Invalid smoke channel");c.solver.channels|=n=="density"?1:n=="temperatureKelvin"?2:4;}
    if(value.contains("parameters")){const auto& p=value.at("parameters");keys(p,{}, {"ambientKelvin","maxSpeed","courant","densityDecay","cooling","fuelDecay","burnRate","ignitionKelvin","heatPerFuel","sootPerFuel","maxDensity","maxTemperature","maxFuel","buoyancy","smokeWeight","vorticity","pressureRelaxation","pressureIterations","maxSubsteps","macCormack"});
        const std::pair<const char*,float*> fields[]={{"ambientKelvin",&c.solver.ambientKelvin},{"maxSpeed",&c.solver.maxSpeed},{"courant",&c.solver.courant},{"densityDecay",&c.solver.densityDecay},{"cooling",&c.solver.cooling},{"fuelDecay",&c.solver.fuelDecay},{"burnRate",&c.solver.burnRate},{"ignitionKelvin",&c.solver.ignitionKelvin},{"heatPerFuel",&c.solver.heatPerFuel},{"sootPerFuel",&c.solver.sootPerFuel},{"maxDensity",&c.solver.maxDensity},{"maxTemperature",&c.solver.maxTemperature},{"maxFuel",&c.solver.maxFuel},{"buoyancy",&c.solver.buoyancy},{"smokeWeight",&c.solver.smokeWeight},{"vorticity",&c.solver.vorticity},{"pressureRelaxation",&c.solver.pressureRelaxation}};
        for(const auto& f:fields)if(p.contains(f.first)){const std::string name=f.first;const double minimum=name=="ambientKelvin"||name=="ignitionKelvin"||name=="maxTemperature"?100:0;const double maximum=name=="ambientKelvin"?1000:name=="maxSpeed"?100:name=="courant"?.5:name=="pressureRelaxation"?1:10000;*f.second=static_cast<float>(scalar(p.at(f.first),minimum,maximum));}if(p.contains("pressureIterations"))c.solver.pressureIterations=integer(p.at("pressureIterations"),1,256);if(p.contains("maxSubsteps"))c.solver.maxSubsteps=integer(p.at("maxSubsteps"),1,64);if(p.contains("macCormack"))c.solver.macCormack=boolean(p.at("macCormack"));
    }
    if(value.contains("unitsPerSource")){array(value.at("unitsPerSource"),3,3);for(unsigned a=0;a<3;++a)c.unitsPerSource[a]=integer(value.at("unitsPerSource")[a],1,1000000000);}
    array(value.at("sources"),1,32);for(const auto& source:value.at("sources")){keys(source,{"emitter","densityMassRate","heatVolumeRate","fuelMassRate"});const auto name=id(source.at("emitter"));const auto found=std::find_if(asset.emitters.begin(),asset.emitters.end(),[&](const auto& e){return e.id==name;});require(found!=asset.emitters.end(),"Unknown smoke emitter");smoke::EffectSource target;target.emitter=static_cast<unsigned>(found-asset.emitters.begin());const char* rates[]={"densityMassRate","heatVolumeRate","fuelMassRate"};for(unsigned a=0;a<3;++a){const double rate=scalar(source.at(rates[a]),0,1000000);const double q=std::floor(rate*c.unitsPerSource[a]+.5);require(q<=UINT32_MAX&&(rate==0||q>0),"Smoke source quantum overflow/underflow");target.quantum[a]=static_cast<std::uint32_t>(q);}c.sources.push_back(target);}
    if(value.contains("optics")){const auto& o=value.at("optics");keys(o,{}, {"extinctionPerDensity","scatteringAlbedo","emissionPerDensity","emissionStartKelvin"});if(o.contains("extinctionPerDensity"))c.optics.extinctionPerDensity=static_cast<float>(scalar(o.at("extinctionPerDensity"),0,1000));if(o.contains("scatteringAlbedo"))c.optics.scatteringAlbedo=vector(o.at("scatteringAlbedo"),0,1);if(o.contains("emissionPerDensity"))c.optics.emissionPerDensity=static_cast<float>(scalar(o.at("emissionPerDensity"),0,1000));if(o.contains("emissionStartKelvin"))c.optics.emissionStartKelvin=static_cast<float>(scalar(o.at("emissionStartKelvin"),100,10000));}
    const auto error=smoke::effectConfigurationError(c,static_cast<unsigned>(asset.emitters.size()),asset.maxAlive);require(error.empty(),error);return c;
}
}

std::shared_ptr<const Asset> parseCooked(const std::string& bytes) {
    try {
        const auto payload=authenticated(bytes,"nativegpu.effect-cook/2","hash");
        const auto& source=payload.at("source");
        keys(source,{"schema","id","revision","seed","durationTicks","loop","maxAlive","bounds","budget","emitters","events"},{"field","smoke"});
        require(text(source.at("schema"))=="nativegpu.effect/1","Unsupported effect source schema");
        auto asset=std::make_shared<Asset>(); asset->id=id(source.at("id"));
        asset->hash=sha256(text(Json::parse(bytes).at("payloadJson")));
        asset->revision=integer(source.at("revision"),0,0xFFFFFFFFu); asset->seed=integer(source.at("seed"),0,0xFFFFFFFFu);
        asset->duration=integer(source.at("durationTicks"),1,432000); asset->maxAlive=integer(source.at("maxAlive"),1,1000000);
        asset->loop=boolean(source.at("loop")); keys(source.at("bounds"),{"min","max"});
        asset->boundsMin=vector(source.at("bounds").at("min")); asset->boundsMax=vector(source.at("bounds").at("max"));
        require(asset->boundsMin.x<asset->boundsMax.x && asset->boundsMin.y<asset->boundsMax.y && asset->boundsMin.z<asset->boundsMax.z,"Invalid effect bounds");
        const auto& budget=source.at("budget"); keys(budget,{"maxSpawnPerTick","maxEventsPerTick","maxDepth"});
        asset->maxSpawnPerTick=integer(budget.at("maxSpawnPerTick"),1,1000000);
        asset->maxEventsPerTick=integer(budget.at("maxEventsPerTick"),0,65536);
        asset->maxDepth=integer(budget.at("maxDepth"),0,8);
        array(source.at("events"),0,32); asset->hasEvents=!source.at("events").empty();
        for (const auto& event:source.at("events")) {
            keys(event,{"on","spawn","limit","delayTicks"}); const auto on=text(event.at("on"));
            require(on=="spawn"||on=="collision"||on=="death"||on=="threshold"||on=="timeline","Invalid effect event");
            require(id(event.at("spawn"))!=asset->id && asset->maxEventsPerTick>0 && asset->maxDepth>0,
                    "Invalid recursive or unbudgeted effect event");
            EventRule rule;
            rule.on=on=="spawn"?EventKind::Spawn:on=="collision"?EventKind::Collision:on=="death"?EventKind::Death:
                    on=="threshold"?EventKind::Threshold:EventKind::Timeline;
            rule.spawn=id(event.at("spawn"));
            rule.limit=integer(event.at("limit"),1,65536);
            rule.delayTicks=integer(event.at("delayTicks"),0,432000);
            asset->rules.push_back(std::move(rule));
        }
        asset->hasField=source.contains("field");
        std::set<std::string> leasedChannels;
        if (asset->hasField) {
            const auto& f=source.at("field"); keys(f,{"level","channels","dimensions","voxelMetres"});
            const auto level=text(f.at("level")); require(level=="local"||level=="worldVisible","Invalid field level");
            asset->fieldLevel=level;asset->fieldVoxelMetres=static_cast<float>(scalar(f.at("voxelMetres"),.001,100)); array(f.at("dimensions"),3,3);
            for (const auto& d:f.at("dimensions")) require(integer(d,8,256)%8==0,"Invalid field dimensions");
            for(unsigned i=0;i<3;++i)asset->fieldDimensions[i]=integer(f.at("dimensions")[i],8,256);
            array(f.at("channels"),1,5);
            for (const auto& c:f.at("channels")) {
                const auto name=text(c); require(name=="density"||name=="temperatureKelvin"||name=="windVelocity"||name=="waterVelocity"||name=="moisture","Unknown field channel");
                require(leasedChannels.insert(name).second,"Duplicate field channel");
                asset->fieldChannels.push_back(name);
            }
        }
        array(source.at("emitters"),1,32); std::set<std::string> seen;
        Json layout={{"capacity",asset->maxAlive},{"emitters",Json::array()}};
        for (const auto& value:source.at("emitters")) {
            keys(value,{"id","shape","position","ratePerSecond","bursts","lifetimeTicks","localSpace","modules","renderers"});
            Emitter e; e.id=id(value.at("id")); require(seen.insert(e.id).second,"Duplicate emitter id");
            e.position=vector(value.at("position")); e.localSpace=boolean(value.at("localSpace"));
            const double rate=scalar(value.at("ratePerSecond"),0,1000000);
            require(std::round(rate*1000)/1000==rate,"Effect rate supports at most three decimals"); e.rateMilli=static_cast<std::uint64_t>(std::llround(rate*1000));
            array(value.at("lifetimeTicks"),2,2); e.lifeMin=integer(value.at("lifetimeTicks")[0],1,432000); e.lifeMax=integer(value.at("lifetimeTicks")[1],e.lifeMin,432000);
            const auto& shape=value.at("shape"); const auto type=text(shape.at("type"));
            if (type=="point") { keys(shape,{"type"}); e.shape=Shape::Point; }
            else if (type=="sphere"||type=="cone") {
                if (type=="cone") { keys(shape,{"type","radius","angleRadians"}); e.shape=Shape::Cone; e.angle=static_cast<float>(scalar(shape.at("angleRadians"),0,3.141592653589793)); }
                else { keys(shape,{"type","radius"}); e.shape=Shape::Sphere; }
                e.radius=static_cast<float>(scalar(shape.at("radius"),.0001,10000));
            } else if (type=="box") { keys(shape,{"type","extent"}); e.shape=Shape::Box; e.extent=vector(shape.at("extent"),.0001,10000); }
            else if (type=="line") { keys(shape,{"type","end"}); e.shape=Shape::Line; e.lineEnd=vector(shape.at("end"),-10000,10000); }
            else if (type=="mesh") { keys(shape,{"type","mesh"}); e.shape=Shape::Mesh; reference(shape.at("mesh")); }
            else require(false,"Unknown effect shape");
            array(value.at("bursts"),0,256); std::int64_t last=-1;
            for (const auto& burst:value.at("bursts")) {
                keys(burst,{"tick","count"}); Burst b{integer(burst.at("tick"),0,asset->duration-1),integer(burst.at("count"),1,1000000)};
                require(b.tick>last,"Burst ticks must strictly increase"); last=b.tick; e.bursts.push_back(b);
            }
            e.modules=modules(value.at("modules"));
            for(const auto& module:e.modules)if(module.kind==ModuleKind::VelocityField)
                require(asset->hasField&&leasedChannels.count(module.fieldChannel)!=0,"Velocity module accesses an undeclared field channel");
            for (const auto& m:value.at("modules")) if (m.at("type")=="fieldCoupling") {
                require(asset->hasField,"Field module without lease declaration");
                for (const auto* access:{"readChannels","writeChannels"}) for (const auto& c:m.at("params").at(access))
                    require(leasedChannels.count(text(c))!=0,"Effect module accesses unleased channel");
            }
            array(value.at("renderers"),1,6);
            for (const auto& r:value.at("renderers")) {
                Renderer target; target.type=text(r.at("type"));
                if (target.type=="sprite"||target.type=="ribbon"||target.type=="beam"||target.type=="decal") {
                    keys(r,{"type","texture","blend","lit"}); target.texture=reference(r.at("texture")); target.lit=boolean(r.at("lit"));
                    const auto blend=text(r.at("blend")); require(blend=="additive"||blend=="premultiplied"||blend=="cutout","Invalid effect blend");
                    target.blend=blend=="additive"?Blend::Additive:blend=="premultiplied"?Blend::Premultiplied:Blend::Cutout;
                } else if (target.type=="mesh") { keys(r,{"type","mesh","material"}); target.mesh=reference(r.at("mesh")); target.material=reference(r.at("material")); }
                else if (target.type=="light") { keys(r,{"type","intensity","range"}); scalar(r.at("intensity"),0,100000); scalar(r.at("range"),.01,10000); }
                else if (target.type=="volumetric") { keys(r,{"type"}); require(leasedChannels.count("density")!=0,"Volume renderer requires density"); }
                else require(false,"Unknown effect renderer");
                e.renderers.push_back(std::move(target));
            }
            Json moduleTypes=Json::array(); for (const auto& m:e.modules) moduleTypes.push_back(static_cast<unsigned>(m.kind));
            layout["emitters"].push_back({{"id",e.id},{"localSpace",e.localSpace},{"shape",type},{"modules",moduleTypes},{"renderers",value.at("renderers")}});
            asset->emitters.push_back(std::move(e));
        }
        if(source.contains("smoke")){asset->smoke=parseSmoke(source.at("smoke"),*asset);layout["smoke"]=source.at("smoke");}
        asset->layoutHash=sha256(layout.dump());
        require(fieldDepositError(*asset).empty(),fieldDepositError(*asset));
        return asset;
    } catch (const Error&) { throw; }
    catch (const std::exception& error) { throw Error("NG-E-EFFECT-ASSET",error.what()); }
}

std::shared_ptr<const Asset> loadCooked(const std::string& file) { return parseCooked(read(std::filesystem::u8path(file))); }

Library loadLibrary(const std::string& manifestFile, const std::string& assetRoot) {
    try {
        const auto path=std::filesystem::canonical(std::filesystem::u8path(manifestFile));
        const auto bytes=read(path);
        const auto payload=authenticated(bytes,"nativegpu.effect-library/2","packageHash");
        keys(payload,{"schema","libraryHash","dependencies","effects","runtimeIntegrated"});
        Library result; result.hash=sha256(text(Json::parse(bytes).at("payloadJson")));
        const auto root=std::filesystem::canonical(std::filesystem::u8path(assetRoot));
        const auto contained=[](const std::filesystem::path& parent,const std::filesystem::path& child) {
            const auto relative=child.lexically_relative(parent);
            return !relative.empty() && !relative.is_absolute() && *relative.begin()!=std::filesystem::path("..");
        };
        require(payload.at("dependencies").is_object() && payload.at("dependencies").size()<=4096,"Invalid effect dependencies");
        for (const auto& entry:payload.at("dependencies").items()) {
            const auto name=reference(entry.key()),expected=text(entry.value()); require(hashText(expected),"Invalid dependency hash");
            const auto file=std::filesystem::canonical(root/std::filesystem::u8path(name));
            require(contained(root,file),"Effect dependency escapes asset root");
            require(sha256(read(file,64*1024*1024))==expected,"Effect dependency SHA-256 mismatch: "+name);
            result.dependencies[name]=expected;
        }
        array(payload.at("effects"),1,4096);
        for (const auto& entry:payload.at("effects")) {
            keys(entry,{"id","hash","file"}); const auto name=id(entry.at("id")),expected=text(entry.at("hash"));
            require(hashText(expected)&&text(entry.at("file"))==expected+".ngeffect.json","Invalid effect content address");
            const auto file=std::filesystem::canonical(path.parent_path()/std::filesystem::u8path(text(entry.at("file"))));
            require(contained(path.parent_path(),file),"Cooked effect escapes package directory");
            const auto asset=loadCooked(file.u8string()); require(asset->id==name&&asset->hash==expected,"Effect package identity mismatch");
            require(result.assets.emplace(name,asset).second,"Duplicate effect library id");
        }
        return result;
    } catch (const Error&) { throw; }
    catch (const std::exception& error) { throw Error("NG-E-EFFECT-ASSET",error.what()); }
}
} // namespace engine::effects
