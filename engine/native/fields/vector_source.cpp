#include "vector_source.hpp"
#include "../assets/sha256.hpp"
#include "../../../third_party/json.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <limits>
#include <set>
#include <stdexcept>

namespace engine::fields {
namespace {
using Json=nlohmann::json;
void require(bool ok,const char* message){if(!ok)throw std::invalid_argument(std::string("NG-E-FIELD-SOURCE: ")+message);}
void keys(const Json& j,std::initializer_list<const char*> allowed){require(j.is_object(),"expected object");std::set<std::string> names;for(auto p:allowed){names.insert(p);require(j.contains(p),"missing key");}for(auto it=j.begin();it!=j.end();++it)require(names.count(it.key())!=0,"unknown key");}
double number(const Json& j,double minimum,double maximum){require(j.is_number(),"expected finite number");double v=j.get<double>();require(std::isfinite(v)&&v>=minimum&&v<=maximum,"number out of range");return v;}
Tick integer(const Json& j,Tick maximum){const auto v=number(j,0,static_cast<double>(maximum));require(std::floor(v)==v,"expected integer");return static_cast<Tick>(v);}
void triple(const Json& j){require(j.is_array()&&j.size()==3,"expected three components");}
std::int32_t cell(const Json& j){return static_cast<std::int32_t>(number(j,-8191,8191));}
void validate(const VectorSourceAsset& a){
    require(!a.id.empty()&&a.id.size()<=64&&std::all_of(a.id.begin(),a.id.end(),[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';}),"invalid source id");
    require(std::isfinite(a.cellMetres)&&a.cellMetres>=.001f&&a.cellMetres<=100&&std::isfinite(a.originMetres.x)&&std::isfinite(a.originMetres.y)&&std::isfinite(a.originMetres.z),"invalid metre grid");
    require(a.domain==Domain::CpuAuthority||a.domain==Domain::VisualReference,"invalid data domain");
    require(a.haloCells<=1&&a.maxAgeTicks<=432000&&a.loopTicks<=432000&&a.tracks.size()>=1&&a.tracks.size()<=2,"source limits exceeded");
    LeaseRequest r;r.region=a.region;r.haloCells=a.haloCells;Region coverage;require(r.storageRegion(coverage),"invalid region");
    std::uint64_t volume=1;const int lo[]={coverage.min.x,coverage.min.y,coverage.min.z},hi[]={coverage.max.x,coverage.max.y,coverage.max.z};
    for(unsigned i=0;i<3;++i){require(lo[i]>=-8192&&hi[i]<=8192&&hi[i]>lo[i]&&hi[i]-lo[i]<=64,"invalid source coverage");volume*=hi[i]-lo[i];}
    require(volume<=32768,"source sample budget exceeded");
    std::set<std::string> names;for(const auto& track:a.tracks){require((track.name=="windVelocity"||track.name=="waterVelocity")&&names.insert(track.name).second,"invalid or repeated velocity track");require(!track.keys.empty()&&track.keys.size()<=256&&track.keys.front().tick==0,"keys must start at zero");
        Tick previous=0;bool first=true;for(const auto& key:track.keys){require(key.tick<=432000&&(first||key.tick>previous)&&(!a.loopTicks||key.tick<=a.loopTicks),"invalid key time ordering");for(auto v:key.velocityMilli)require(v>=-10000000&&v<=10000000,"velocity out of range");first=false;previous=key.tick;}
        if(a.loopTicks&&track.keys.size()>1)require(track.keys.back().tick==a.loopTicks&&track.keys.front().velocityMilli==track.keys.back().velocityMilli,"loop endpoints must agree");}
}
std::int64_t divideRounded(std::int64_t value,std::int64_t denominator){return value<0?-((-value+denominator/2)/denominator):(value+denominator/2)/denominator;}
}
VectorSourceAsset parseVectorSource(const std::string& bytes){
    require(bytes.size()<=2*1024*1024,"asset exceeds 2 MiB");
    try{auto j=Json::parse(bytes);keys(j,{"schema","id","originMetres","cellMetres","region","haloCells","maxAgeTicks","loopTicks","domain","tracks"});require(j.at("schema")=="nativegpu.vector-field/1","unknown schema");
        VectorSourceAsset a;a.id=j.at("id").get<std::string>();a.hash=assets::sha256(bytes);triple(j.at("originMetres"));a.originMetres={static_cast<float>(number(j["originMetres"][0],-1e6,1e6)),static_cast<float>(number(j["originMetres"][1],-1e6,1e6)),static_cast<float>(number(j["originMetres"][2],-1e6,1e6))};a.cellMetres=static_cast<float>(number(j["cellMetres"],.001,100));
        keys(j["region"],{"min","max"});triple(j["region"]["min"]);triple(j["region"]["max"]);for(unsigned i=0;i<3;++i){require(std::floor(number(j["region"]["min"][i],-8191,8191))==number(j["region"]["min"][i],-8191,8191)&&std::floor(number(j["region"]["max"][i],-8191,8191))==number(j["region"]["max"][i],-8191,8191),"region cells must be integers");}
        a.region={{cell(j["region"]["min"][0]),cell(j["region"]["min"][1]),cell(j["region"]["min"][2])},{cell(j["region"]["max"][0]),cell(j["region"]["max"][1]),cell(j["region"]["max"][2])}};
        a.haloCells=static_cast<std::uint32_t>(integer(j["haloCells"],1));a.maxAgeTicks=integer(j["maxAgeTicks"],432000);a.loopTicks=integer(j["loopTicks"],432000);
        const auto domain=j["domain"].get<std::string>();require(domain=="cpu-authority"||domain=="visual","unknown data domain");a.domain=domain=="cpu-authority"?Domain::CpuAuthority:Domain::VisualReference;
        require(j["tracks"].is_object()&&!j["tracks"].empty()&&j["tracks"].size()<=2,"invalid tracks");for(auto it=j["tracks"].begin();it!=j["tracks"].end();++it){VectorTrack track;track.name=it.key();require(it.value().is_array()&&!it.value().empty()&&it.value().size()<=256,"invalid key count");for(const auto& key:it.value()){keys(key,{"tick","metresPerSecond"});VectorKey k;k.tick=integer(key["tick"],432000);triple(key["metresPerSecond"]);for(unsigned i=0;i<3;++i)k.velocityMilli[i]=static_cast<std::int32_t>(std::llround(number(key["metresPerSecond"][i],-10000,10000)*1000));track.keys.push_back(k);}a.tracks.push_back(std::move(track));}
        validate(a);return a;
    }catch(const Json::exception& e){throw std::invalid_argument(std::string("NG-E-FIELD-SOURCE: ")+e.what());}
}
VectorSourceAsset loadVectorSource(const std::string& file){std::ifstream in(std::filesystem::u8path(file),std::ios::binary);require(static_cast<bool>(in),"cannot open asset");in.seekg(0,std::ios::end);auto size=in.tellg();require(size>=0&&size<=2*1024*1024,"invalid asset size");in.seekg(0);std::string bytes(static_cast<std::size_t>(size),'\0');in.read(bytes.data(),bytes.size());require(static_cast<bool>(in),"asset read failed");return parseVectorSource(bytes);}
VectorSource::VectorSource(std::shared_ptr<World> world,VectorSourceAsset asset):world_(std::move(world)),asset_(std::move(asset)){
    require(static_cast<bool>(world_),"missing World");validate(asset_);for(const auto& track:asset_.tracks){std::array<ChannelId,3> ids{};for(unsigned axis=0;axis<3;++axis){Channel c{asset_.id+"."+track.name+"."+std::to_string(axis),"m/s",asset_.id,asset_.domain,Merge::Sum};c.minimum=-10000000;c.maximum=10000000;c.unitsPerInteger=1000;ids[axis]=world_->registerChannel(std::move(c));}channels_[track.name]=ids;}
    auto request=reader();require(request.storageRegion(coverage_),"invalid source coverage");
}
VectorSource::~VectorSource(){release();}
bool VectorSource::release(){if(!active_)return true;if(!world_->release(writer_))return false;writer_={};active_=false;world_->collect(0);return true;}
LeaseRequest VectorSource::reader() const{LeaseRequest r;r.region=asset_.region;r.haloCells=asset_.haloCells;r.owner=asset_.id+".reader";r.consistency=Consistency::Eventual;r.maxAgeTicks=asset_.maxAgeTicks;r.maxPages=128;for(const auto& item:channels_)for(auto id:item.second)r.channels.push_back(id);return r;}
std::array<std::int32_t,3> VectorSource::evaluate(const std::string& name,Tick tick) const{
    auto track=std::find_if(asset_.tracks.begin(),asset_.tracks.end(),[&](const auto& t){return t.name==name;});require(track!=asset_.tracks.end(),"unknown track");if(asset_.loopTicks)tick%=asset_.loopTicks;const auto& keys=track->keys;if(tick>=keys.back().tick)return keys.back().velocityMilli;
    auto upper=std::upper_bound(keys.begin(),keys.end(),tick,[](Tick t,const VectorKey& k){return t<k.tick;});const auto& a=*(upper-1);const auto& b=*upper;std::array<std::int32_t,3> out{};
    for(unsigned i=0;i<3;++i){const auto sum=static_cast<std::int64_t>(a.velocityMilli[i])*static_cast<std::int64_t>(b.tick-tick)+static_cast<std::int64_t>(b.velocityMilli[i])*static_cast<std::int64_t>(tick-a.tick);out[i]=static_cast<std::int32_t>(divideRounded(sum,b.tick-a.tick));}return out;
}
bool VectorSource::stage(Tick tick,std::string& error){
    error.clear();if((staged_&&tick<lastTick_)||publications_==UINT64_MAX){error="NG-E-FIELD-SOURCE: source clock or epoch overflow";return false;}if(staged_&&tick==lastTick_&&active_)return true;
    bool acquiredNow=false;if(!active_){auto request=reader();request.region=coverage_;request.haloCells=0;request.owner=asset_.id;request.writable=true;auto acquired=world_->acquire(request);if(!acquired.accepted){error=acquired.error;return false;}writer_=acquired.lease;active_=true;acquiredNow=true;}
    std::vector<Publication> batch;for(const auto& item:channels_){auto values=evaluate(item.first,tick);for(unsigned axis=0;axis<3;++axis){std::vector<Write> writes;for(int z=coverage_.min.z;z<coverage_.max.z;++z)for(int y=coverage_.min.y;y<coverage_.max.y;++y)for(int x=coverage_.min.x;x<coverage_.max.x;++x)writes.push_back({{x,y,z},values[axis]});batch.push_back({writer_,item.second[axis],1,publications_+1,tick,tick,asset_.domain,std::move(writes)});}}
    auto result=world_->publishAtomic(batch);if(result!=PublishResult::Accepted&&result!=PublishResult::Duplicate){if(acquiredNow)release();error="NG-E-FIELD-SOURCE: publication rejected ("+std::to_string(static_cast<unsigned>(result))+")";return false;}lastTick_=tick;staged_=true;++publications_;return true;
}
}
