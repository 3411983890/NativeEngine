#include "../engine/native/game.hpp"
#include <cassert>
#include <iostream>

using namespace engine;
namespace {
Entity entity(const std::string& id,const std::string& kind,Vec3 position,Vec3 scale,bool collider=false) {
    Entity e;e.id=id;e.name=id;e.kind=kind;e.position=position;e.scale=scale;e.collider=collider;return e;
}
Scene scene() {
    Scene s;s.entities={entity("actor","player",{0,0,5},{.6f,1.8f,.6f}),entity("floor","ground",{0,-.2f,0},{20,.4f,20},true),
        entity("crate","prop",{2,1,2},{1,1,1},true),entity("key","key",{4,.8f,1},{.25f,.5f,.25f}),
        entity("door","door",{0,1.5f,-8},{2.4f,3,.25f},true),entity("goal","goal",{0,0,-9},{1.5f,.1f,1.5f})};
    s.entities[2].hasPhysics=true;s.entities[2].dynamicBody=true;s.entities[2].destructible=true;return s;
}
std::string digest(Game& game) {determinism::Hasher h;game.digest(h);return h.hex();}
void restoreContinuation(bool animated) {
    auto first=std::make_unique<Game>();first->initialize(scene());
    std::shared_ptr<const animation::Asset> character;
    if(animated){character=animation::loadAsset("build/animation/mannequin/character.json");first->setCharacter(character);}
    Input input;input.forward=.3f;input.right=.4f;
    for(int i=0;i<90;++i)first->update(input,1.f/120);
    first->detonate({2,1,2},3,12);
    input.jump=true;input.interact=true;first->update(input,1.f/240);
    const auto snapshot=first->snapshot();const auto before=digest(*first);
    first->save("reports/gameplay/full.save.json");
    auto resumed=std::make_unique<Game>();resumed->initialize(scene());if(animated)resumed->setCharacter(character);
    resumed->load("reports/gameplay/full.save.json");if(digest(*resumed)!=before) {std::cerr<<gameplay::Json::diff(snapshot,resumed->snapshot()).dump(2)<<"\n";}assert(digest(*resumed)==before);
    for(int i=0;i<360;++i){input.jump=i%60==0;input.interact=i%95==0;first->update(input,1.f/120);resumed->update(input,1.f/120);assert(digest(*first)==digest(*resumed));assert(first->snapshot()==resumed->snapshot());}
    first->restore(snapshot);assert(digest(*first)==before);
    auto invalid=snapshot;invalid["entityBody_"][0]=999999;
    try{first->restore(invalid);assert(false);}catch(const gameplay::Error&){}
    assert(digest(*first)==before);
}
void runtimeBodyLifecycle() {
    auto install=[](Game& game){
        auto& runtime=game.gameplayRuntime();
        runtime.loadModule("factory",{{"apiVersion",1},{"version",1},{"properties",{{"type","object"}}},{"state",{{"type","object"}}}},R"(return {on_start=function(c)
          c:spawn('runtime-crate',{transform={position={-4,3,0}},collider={},rigidbody={dynamic=true,mass=2},renderable={}})
        end})");
        runtime.attach("actor","factory","factory");
    };
    Game first;first.initialize(scene());install(first);
    const auto initialCount=first.renderPoses().size();first.update({},1.f/120);
    auto& runtime=first.gameplayRuntime();const auto h=runtime.world.find("runtime-crate");assert(runtime.world.valid(h));
    assert(first.renderPoses().size()==initialCount+1);
    for(int i=0;i<12;++i)first.update({},1.f/120);
    assert(runtime.world.get(h,"transform")->at("position")[1].get<float>()<3.f);
    auto transform=*runtime.world.get(h,"transform");transform["scale"]={2,1,1};runtime.world.set(h,"transform",transform);
    auto rigid=*runtime.world.get(h,"rigidbody");rigid["mass"]=4;runtime.world.set(h,"rigidbody",rigid);first.update({},1.f/120);
    const auto crate=runtime.world.find("crate");auto authored=*runtime.world.get(crate,"transform");authored["scale"]={1.4,1,1};runtime.world.set(crate,"transform",authored);first.update({},1.f/120);
    const auto state=first.snapshot();const int body=state.at("runtimeBodies").at("runtime-crate");
    assert(state.at("physics").at("bodies_")[body].at("half").at("x")==1);
    assert(state.at("physics").at("bodies_")[body].at("mass")==4);
    Game resumed;resumed.initialize(scene());install(resumed);resumed.restore(state);
    assert(digest(first)==digest(resumed));assert(first.snapshot()==resumed.snapshot());
    for(int i=0;i<60;++i){first.update({},1.f/120);resumed.update({},1.f/120);assert(digest(first)==digest(resumed));assert(first.snapshot()==resumed.snapshot());}
    runtime.world.set(h,"collider",{{"enabled",false}});first.update({},1.f/120);
    assert(!first.snapshot().at("physics").at("bodies_")[body].at("active").get<bool>());
    runtime.destroy("runtime-crate");first.update({},1.f/120);assert(first.renderPoses().size()==initialCount);
    const auto actor=runtime.world.find("actor");auto teleport=*runtime.world.get(actor,"transform");teleport["position"]={-2,2,2};runtime.world.set(actor,"transform",teleport);first.update({},1.f/120);
    assert(first.playerPosition().x==-2&&first.playerPosition().z==2);
    assert(runtime.diagnostics().empty());
}
}
int main(){restoreContinuation(false);restoreContinuation(true);runtimeBodyLifecycle();std::cout<<"Gameplay integration: full save/restart, pending input edges, half tick, active physics/destruction and animation continuation, runtime body lifecycle, atomic invalid-restore rejection passed.\n";}
