#include "../host/audio_scene.hpp"
#include "../engine/audio/asset_data.hpp"
#include <iostream>
#include <stdexcept>
using namespace engine;
static unsigned checks=0;
static void check(bool v,const char* message){++checks;if(!v)throw std::runtime_error(message);}
int main(int argc,char** argv){try {
    check(argc==2,"scene argument");host::SceneDocument doc;std::string error;check(host::loadScene(argv[1],doc,error),error.c_str());
    check(doc.audio.sounds.size()==1,"verified scene sound");check(!doc.audio.contentHash.empty(),"audio content identity");
    Game game;game.initialize(doc.scene);const auto& e=doc.audio.emitters[0];Vec3 p;
    check(host::audioEntityPosition(game,e.entityId,{1,2,3},p),"live position");
    auto& world=game.gameplayRuntime().world;const auto h=world.find(e.entityId);auto t=*world.get(h,"transform");t["position"]={4,5,6};world.set(h,"transform",t);
    check(host::audioEntityPosition(game,e.entityId,{1,2,3},p)&&p.x==5&&p.y==7&&p.z==9,"live entity movement and offset");
    audio::Engine mixer;audio::Config config;config.quality=audio::Quality::Spatial;config.layout=audio::Layout::Surround51;config.commandLeadSamples=0;
    check(mixer.configure(config,error),"configure");const auto sound=doc.audio.sounds.begin()->second;const auto handle=mixer.addSound(sound);audio::PlayParams params;params.loop=true;params.spatial=true;params.position={0,0,2};
    const auto a=mixer.play(handle,params),b=mixer.play(handle,params);check(a&&b&&a!=b,"distinct producer voice handles");
    check(mixer.setVoicePosition(a,{0,0,8}),"queue moving voice before render");mixer.renderOffline(.05f);auto receipts=mixer.receipts();
    check(receipts.size()==2,"two voices");for(const auto& r:receipts)check(std::abs(r.distance-(r.id==a?8.f:2.f))<.01f,"position routes to correct voice");
    mixer.stop(a,0);mixer.renderOffline(.05f);check(mixer.receipts().size()==1&&mixer.receipts()[0].id==b,"stop by returned id");
    mixer.reset();check(mixer.soundCount()==0&&mixer.configure(config,error),"stopped consumer resets bank");
    auto loop=std::make_shared<audio::Sound>(*sound);loop->loop=true;const auto c=mixer.addSound(loop);params.loop=false;params.overrideLoop=true;params.spatial=false;mixer.play(c,params);mixer.renderOffline(.5f);check(mixer.receipts().empty(),"authored one-shot overrides asset loop");
    audio::Sound decoded;check(!audio::decodeNgsndData(std::string(39,'x'),decoded,error),"truncated header rejected");
    const auto ground=std::find_if(doc.scene.entities.begin(),doc.scene.entities.end(),[](const auto& entity){return entity.kind=="ground";});physics::Aabb bounds;
    check(ground!=doc.scene.entities.end()&&game.entityColliderBounds(ground->id,bounds),"live acoustic bounds");world.set(world.find(ground->id),"collider",{{"enabled",false}});check(!game.entityColliderBounds(ground->id,bounds),"disabled acoustic collider");
    world.destroy(h);check(!host::audioEntityPosition(game,e.entityId,{},p),"destroyed emitter has no position");
    std::cout<<"{\"success\":true,\"checks\":"<<checks<<"}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
