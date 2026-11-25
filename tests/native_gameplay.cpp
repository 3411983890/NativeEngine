#include "../engine/native/gameplay/runtime.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace engine::gameplay;
namespace {
Json object(Json fields=Json::object()) { return {{"type","object"},{"properties",fields}}; }
Json integer(int initial=0) { return {{"type","integer"},{"default",initial}}; }
Json descriptor(bool update=false) {
    return {{"apiVersion",1},{"version",1},{"update",update},{"properties",object()},
            {"state",object({{"count",integer()}})}};
}
void rejects(const std::string& code,const std::function<void()>& fn){try{fn();assert(false);}catch(const Error&e){assert(e.code==code);}}
Runtime runtime() {Runtime r;r.initialize("assets",123);r.world.create("actor");r.world.set(r.world.find("actor"),"inventory",Json::object());return r;}
void identityAndSchedule() {
    auto r=runtime(); auto first=r.world.find("actor");r.world.destroy(first);auto second=r.world.create("replacement");
    assert(first.index==second.index&&!r.world.valid(first));rejects("NG-E-ENTITY-STALE",[&]{r.world.entity(first);});
    auto s=r.world.snapshot();World w;w.registry=r.world.registry;w.restore(s);assert(w.snapshot()==s);
    rejects("NG-E-STATE-SCHEMA",[&]{w.set(second,"collider",{{"enabled",7}});});
    Scheduler scheduler;std::vector<int> order;
    scheduler.add({"physics",Phase::Simulation,{},[&]{order.push_back(2);}});
    scheduler.add({"script",Phase::Behaviors,{},[&]{order.push_back(1);}});
    scheduler.add({"collect",Phase::Collect,{"physics"},[&]{order.push_back(3);}});
    scheduler.run();assert((order==std::vector<int>{1,2,3}));
    Scheduler cycle;cycle.add({"a",Phase::Behaviors,{"b"},{}});cycle.add({"b",Phase::Behaviors,{"a"},{}});
    rejects("NG-E-SYSTEM-CYCLE",[&]{cycle.compile();});
}
void rollbackAndIsolation() {
    auto r=runtime();r.world.create("bad");r.world.create("good");
    r.loadModule("bad",descriptor(true),"return {on_update=function(c) c.state.count=9; c:set('actor','inventory',{items={gold_key=99}}); error('intentional') end}");
    r.loadModule("good",descriptor(true),"return {on_update=function(c) c.state.count=c.state.count+1 end}");
    r.attach("bad","behavior","bad");r.attach("good","behavior","good");r.tick();r.tick();
    assert(r.behaviors()[0].disabled&&r.behaviors()[0].state["count"]==0);
    assert(r.behaviors()[1].state["count"]==2&&r.world.get(r.world.find("actor"),"inventory")->at("items").empty());
    assert(r.diagnostics()[0]["code"]=="NG-E-SCRIPT-RUNTIME");
    auto b=runtime();b.loadModule("loop",descriptor(true),"return {on_update=function(c) while true do end end}");b.attach("actor","loop","loop");b.tick();
    assert(b.behaviors()[0].disabled&&b.diagnostics()[0]["code"]=="NG-E-SCRIPT-BUDGET");
    auto mem=runtime();mem.loadModule("memory",descriptor(true),"return {on_update=function(c) local t={} for i=1,100 do t[i]=string.rep('x',100000) end end}");mem.attach("actor","oom","memory");mem.tick();
    assert(mem.behaviors()[0].disabled&&mem.diagnostics()[0]["code"]=="NG-E-SCRIPT-MEMORY");
    auto alias=runtime();alias.loadModule("alias",descriptor(true),R"(return {on_update=function(c)
        local t={leaf=string.rep('x',60000)};for i=1,8 do t={a=t,b=t,c=t,d=t} end;c.state.expansion=t
    end})");alias.attach("actor","alias","alias");alias.tick();
    assert(alias.behaviors()[0].disabled&&alias.diagnostics()[0]["code"]=="NG-E-SCRIPT-BUDGET");
}
void deterministicTimerSaveReload() {
    auto r=runtime();
    const std::string source=R"(return {
      on_start=function(c) c:timer('pulse',3,'pulse',{},2) end,
      on_event=function(c,e) c.state.count=c.state.count+c:random('loot',10)+1 end
    })";
    r.loadModule("timer",descriptor(),source);r.attach("actor","timer","timer");r.tick();r.tick();
    auto snapshot=r.snapshot();auto other=r;other.restore(snapshot);
    for(int i=0;i<20;++i){r.tick();other.tick();assert(r.digest()==other.digest());}
    assert(r.behaviors()[0].state["count"].get<int>()>0);
    r.save("reports/gameplay/runtime.save.json");auto saved=r.digest();r.tick();r.load("reports/gameplay/runtime.save.json");assert(r.digest()==saved);
    auto before=r.snapshot();auto next=descriptor();next["version"]=2;next["state"]["properties"]["extra"]=integer();
    assert(r.reload({{"timer",{next,"return {migrate=function(c,e) c.state.extra=42 end}",""}}}));
    assert(r.behaviors()[0].state["extra"]==42);
    auto valid=r.snapshot();assert(!r.reload({{"timer",{next,"broken lua!",""}}}));assert(r.snapshot()==valid);
    rejects("NG-E-SNAPSHOT-CONTENT",[&]{r.restore(before);});r.lockContent(true);
    assert(!r.reload({{"timer",{next,source,""}}}));
}
void independentContent() {
    auto r=runtime();
    for(const auto& id:{"key1","key2","door1","door2"}) {auto h=r.world.create(id);r.world.set(h,"renderable",Json::object());r.world.set(h,"collider",Json::object());r.world.set(h,"interactable",Json::object());}
    r.attach("key1","pickup","gameplay/pickup",{{"item","red"}});r.attach("key2","pickup","gameplay/pickup",{{"item","blue"}});
    r.attach("door1","lock","gameplay/locked_door",{{"requiredItem","red"}});r.attach("door2","lock","gameplay/locked_door",{{"requiredItem","blue"}});
    r.start();r.interact("key1","actor");r.interact("door1","actor");r.interact("door2","actor");
    assert(!r.world.get(r.world.find("door1"),"collider")->at("enabled").get<bool>());
    assert(r.world.get(r.world.find("door2"),"collider")->at("enabled").get<bool>());
    r.interact("key2","actor");r.interact("door2","actor");assert(!r.world.get(r.world.find("door2"),"collider")->at("enabled").get<bool>());
    assert(r.diagnostics().empty());
}
void structuralAndSystems() {
    auto r=runtime();
    r.loadModule("child",descriptor(),"return {on_start=function(c)c.state.count=1 end,on_destroy=function(c)c:hud('actor','destroyed',12) end}");
    r.loadModule("parent",descriptor(),R"(return {on_start=function(c)
        c:spawn('child',{transform={position={1,2,3}},behavior={instances={{id='child',script='child',properties={}}}}})
    end})");
    r.attach("actor","parent","parent");
    std::vector<std::string> systems;
    r.registerSystem({"observe",Phase::Collect,{"gameplay.behaviors"},[&](Runtime& current){systems.push_back(current.world.valid(current.world.find("child"))?"present":"pending");}});
    r.tick();assert(systems[0]=="pending");assert(r.world.valid(r.world.find("child")));
    r.tick();assert(systems[1]=="present");
    r.destroy("child");assert(!r.world.valid(r.world.find("child")));
    assert(r.world.get(r.world.find("actor"),"hud")->at("text")=="destroyed");
    rejects("NG-E-ENTITY-ID",[&]{r.world.create("child");});
    assert(r.diagnostics().empty());
}
void triggerRepeat() {
    auto r=runtime();auto actor=r.world.find("actor");r.world.set(actor,"transform",{{"position",{0,0,0}}});
    auto h=r.world.create("zone");r.world.set(h,"transform",Json::object());r.world.set(h,"trigger",Json::object());
    r.loadModule("zone",descriptor(),"return {on_trigger_enter=function(c)c.state.count=c.state.count+1 end}");r.attach("zone","zone","zone");
    r.collectTriggers("actor");r.tick();assert(r.behaviors()[0].state["count"]==1);
    r.collectTriggers("actor");r.tick();assert(r.behaviors()[0].state["count"]==1);
    r.world.set(actor,"transform",{{"position",{5,0,0}}});r.collectTriggers("actor");r.tick();
    r.world.set(actor,"transform",Json::object());r.collectTriggers("actor");r.tick();assert(r.behaviors()[0].state["count"]==2);
}
void diskReloadAndCorruptRestore() {
    const auto root=std::filesystem::path("reports/gameplay/reload-assets");
    std::filesystem::create_directories(root/"gameplay");std::filesystem::create_directories(root/"scripts");
    std::filesystem::copy_file("assets/gameplay/components.json",root/"gameplay/components.json",std::filesystem::copy_options::overwrite_existing);
    auto write=[&](const std::string& source){std::ofstream(root/"scripts/hot.lua")<<source;std::ofstream(root/"scripts/hot.behavior.json")<<descriptor(true).dump();};
    write("return {on_update=function(c)c.state.count=c.state.count+1 end}");
    Runtime r;r.initialize(root,123);r.world.create("actor");r.attach("actor","hot","hot");r.tick();
    write("return {on_update=function(c)c.state.count=c.state.count+7 end}");
    assert(r.pollReload());r.tick();assert(r.behaviors()[0].state["count"]==8);
    write("broken lua!");const auto before=r.snapshot();assert(!r.pollReload());assert(r.snapshot()==before);
    r.tick();assert(r.behaviors()[0].state["count"]==15);
    auto invalid=r.snapshot();invalid["events"]=Json::array({{{"due",1},{"seq",999},{"target",7},{"name","bad"},{"payload",Json::object()}}});
    const auto valid=r.snapshot();rejects("NG-E-SNAPSHOT-STATE",[&]{r.restore(invalid);});assert(r.snapshot()==valid);
    invalid=valid;invalid["behaviors"][0]["rng"]={{"bad",-1}};
    rejects("NG-E-STATE-SCHEMA",[&]{r.restore(invalid);});assert(r.snapshot()==valid);
    write("return {on_update=function(c)c.state.count=c.state.count+1 end}");
    auto parent=descriptor();parent["dependencies"]={"hot"};
    std::ofstream(root/"scripts/parent.behavior.json")<<parent.dump();
    std::ofstream(root/"scripts/parent.lua")<<R"(return {on_start=function(c)c:spawn('child',{behavior={instances={{id='hot',script='hot'}}}}) end})";
    Runtime locked;locked.initialize(root);locked.world.create("actor");locked.attach("actor","parent","parent");locked.lockContent(true);locked.tick();locked.tick();
    assert(locked.world.valid(locked.world.find("child"))&&locked.diagnostics().empty());
    assert(locked.manifest()["modules"].size()==2);
    auto cycle=descriptor();cycle["dependencies"]={"parent"};std::ofstream(root/"scripts/hot.behavior.json")<<cycle.dump();
    Runtime circular;circular.initialize(root);circular.world.create("actor");
    rejects("NG-E-SCRIPT-DEPENDENCY",[&]{circular.attach("actor","parent","parent");});
}
}
int main(){identityAndSchedule();rollbackAndIsolation();deterministicTimerSaveReload();independentContent();structuralAndSystems();triggerRepeat();diskReloadAndCorruptRestore();
 std::cout<<"Gameplay: handles, scheduler, transactional isolation, infinite-loop/heap budgets, named RNG, timers, restore, migration, content lock, independent doors passed.\n";}
