#include "runtime.hpp"
#include "../assets/file_path.hpp"
// Lua is compiled as C++ so its protected errors unwind C++ conversion frames.
#include "../../../third_party/lua-5.4.9/src/lua.h"
#include "../../../third_party/lua-5.4.9/src/lauxlib.h"
#include "../../../third_party/lua-5.4.9/src/lualib.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <regex>
#include <set>

namespace engine::gameplay {
namespace {
struct Memory { std::size_t used = 0, limit = 0; unsigned remaining = 0; };
void* allocate(void* ud, void* ptr, size_t oldSize, size_t newSize) {
    auto& m = *static_cast<Memory*>(ud);
    if (!ptr) oldSize = 0;
    if (!newSize) { m.used -= oldSize; std::free(ptr); return nullptr; }
    if (newSize > m.limit || m.used - oldSize > m.limit - newSize) return nullptr;
    void* out = std::realloc(ptr, newSize);
    if (out) m.used = m.used - oldSize + newSize;
    return out;
}
void hook(lua_State* L, lua_Debug*) {
    void* ud = nullptr; lua_getallocf(L, &ud); auto& m = *static_cast<Memory*>(ud);
    if (m.remaining <= 100) luaL_error(L, "NG-E-SCRIPT-BUDGET: instruction limit");
    m.remaining -= 100;
}
void push(lua_State* L, const Json& value) {
    check(lua_checkstack(L, 8) != 0, "NG-E-SCRIPT-MEMORY", "Lua stack limit");
    if (value.is_object()) {
        lua_createtable(L, 0, static_cast<int>(value.size()));
        for (auto it = value.begin(); it != value.end(); ++it) {
            push(L, it.value()); lua_setfield(L, -2, it.key().c_str());
        }
    } else if (value.is_array()) {
        lua_createtable(L, static_cast<int>(value.size()), 0);
        for (std::size_t i = 0; i < value.size(); ++i) { push(L, value[i]); lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1)); }
        lua_createtable(L, 0, 1); lua_pushboolean(L, 1); lua_setfield(L, -2, "__ng_array"); lua_setmetatable(L, -2);
    } else if (value.is_boolean()) lua_pushboolean(L, value.get<bool>());
    else if (value.is_number_integer()) lua_pushinteger(L, value.get<lua_Integer>());
    else if (value.is_number()) lua_pushnumber(L, value.get<double>());
    else if (value.is_string()) { const auto& s = value.get_ref<const std::string&>(); lua_pushlstring(L, s.data(), s.size()); }
    else lua_pushnil(L);
}
struct ConversionBudget { unsigned nodes=32768; std::size_t bytes=1024*1024; };
Json take(lua_State* L, int index, unsigned depth = 0, ConversionBudget* budget = nullptr) {
    ConversionBudget initial;if(!budget)budget=&initial;
    check(budget->nodes>0,"NG-E-SCRIPT-BUDGET","Lua result node budget");--budget->nodes;
    check(depth <= 16, "NG-E-SCRIPT-STATE", "Cyclic/deep Lua state");
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
    case LUA_TBOOLEAN: return lua_toboolean(L, index) != 0;
    case LUA_TNUMBER:
        if (lua_isinteger(L, index)) return lua_tointeger(L, index);
        check(std::isfinite(lua_tonumber(L, index)), "NG-E-SCRIPT-STATE", "Nonfinite Lua number");
        return lua_tonumber(L, index);
    case LUA_TSTRING: { size_t size = 0; const char* s = lua_tolstring(L, index, &size);
        check(size <= 65536, "NG-E-SCRIPT-STATE", "Lua string too large");
        check(size<=budget->bytes,"NG-E-SCRIPT-BUDGET","Lua result byte budget");budget->bytes-=size;return std::string(s, size); }
    case LUA_TTABLE: {
        bool array = lua_rawlen(L, index) > 0;
        if (lua_getmetatable(L, index)) { lua_getfield(L, -1, "__ng_array"); array = array || lua_toboolean(L, -1); lua_pop(L, 2); }
        Json out = array ? Json::array() : Json::object();
        unsigned count = 0; lua_pushnil(L);
        while (lua_next(L, index)) {
            check(++count <= 10000, "NG-E-SCRIPT-STATE", "Lua table too large");
            if (array) check(lua_isinteger(L, -2) && lua_tointeger(L, -2) >= 1 && lua_tointeger(L, -2) <= static_cast<lua_Integer>(lua_rawlen(L, index)),
                             "NG-E-SCRIPT-STATE", "Sparse/mixed Lua array");
            else {
                check(lua_type(L, -2) == LUA_TSTRING, "NG-E-SCRIPT-STATE", "Object keys must be strings");
                size_t length=0;const char* key=lua_tolstring(L,-2,&length);
                check(length<=256&&length<=budget->bytes,"NG-E-SCRIPT-BUDGET","Lua result key budget");budget->bytes-=length;
                out[std::string(key,length)] = take(L, -1, depth + 1,budget);
            }
            lua_pop(L, 1);
        }
        if (array) {
            check(count == lua_rawlen(L, index), "NG-E-SCRIPT-STATE", "Sparse Lua array");
            for (unsigned i = 1; i <= count; ++i) { lua_rawgeti(L, index, i); out.push_back(take(L, -1, depth + 1,budget)); lua_pop(L, 1); }
        }
        return out;
    }
    default: throw Error("NG-E-SCRIPT-STATE", "State contains unsupported Lua value");
    }
}
// Captured helpers are private. Scripts cannot persist globals between invocations,
// access unordered iteration, filesystem, clocks, bytecode loaders or coroutines.
const char* prelude = R"LUA(
local rawnext, setmeta = next, setmetatable
local function keys(t)
 local a = {}; for k in rawnext,t do a[#a+1]=k end
 table.sort(a); return a
end
function __prepare(ctx)
 ctx.commands=setmeta({}, {__ng_array=true})
 function ctx:array() return setmeta({}, {__ng_array=true}) end
 function ctx:keys(t) return keys(t) end
 function ctx:get(id, component) return __get(id,component) end
 function ctx:query(component)
  return __query(component)
 end
 function ctx:command(op, args)
  assert(#self.commands < self.commandLimit, 'NG-E-SCRIPT-BUDGET: command limit')
  args.op=op; self.commands[#self.commands+1]=args
 end
 function ctx:set(id, component, value) self:command('set',{entity=id,component=component,value=value}) end
 function ctx:remove(id, component) self:command('remove',{entity=id,component=component}) end
 function ctx:spawn(id, components) self:command('spawn',{entity=id,components=components}) end
 function ctx:destroy(id) self:command('destroy',{entity=id}) end
 function ctx:emit(target, name, payload) self:command('emit',{target=target,name=name,payload=payload or {}}) end
 function ctx:timer(id, delay, name, payload, interval)
  self:command('timer',{id=id,delay=delay,name=name,payload=payload or {},interval=interval or 0})
 end
 function ctx:cancel_timer(id) self:command('cancel_timer',{id=id}) end
 function ctx:presentation(name,payload) self:command('presentation',{name=name,payload=payload or {}}) end
 function ctx:hud(actor,text,duration) self:set(actor,'hud',{text=text,remaining=duration}) end
 function ctx:play(id,clip)
  local a=self:get(id,'animator') or {request=0}; self:set(id,'animator',{clip=clip,request=a.request+1})
 end
 function ctx:random(name,bound)
  assert(type(name)=='string' and #name<=128 and math.type(bound)=='integer' and bound>0 and bound<=2147483647,'invalid RNG arguments')
  local x=self.rng[name]
  if not x then x=self.rngSeed; for i=1,#name do x=(x*33+string.byte(name,i)) & 0xffffffff end end
  x=(x*1664525+1013904223) & 0xffffffff; self.rng[name]=x
  return x % bound
 end
 return ctx
end
for _,name in ipairs({'dofile','loadfile','load','collectgarbage','print','warn','require','next','pairs','setmetatable','getmetatable','rawset','rawget','rawequal','pcall','xpcall'}) do _G[name]=nil end
math.random=nil; math.randomseed=nil; string.dump=nil
)LUA";
struct Job { const Module* module; Json context, event; std::string callback; const World* world; };
int getComponent(lua_State* L) {
    const auto* world=static_cast<const World*>(lua_touserdata(L,lua_upvalueindex(1)));
    const char* id=luaL_checkstring(L,1);const char* component=luaL_checkstring(L,2);
    const auto handle=world->find(id);
    if(!world->valid(handle)){lua_pushnil(L);return 1;}
    const auto* value=world->get(handle,component);
    if(value)push(L,*value);else lua_pushnil(L);
    return 1;
}
int queryComponents(lua_State* L) {
    const auto* world=static_cast<const World*>(lua_touserdata(L,lua_upvalueindex(1)));
    const char* component=luaL_optstring(L,1,"");Json ids=Json::array();
    for(auto h:world->query(component))ids.push_back(world->entity(h).id);
    push(L,ids);return 1;
}
int driver(lua_State* L) {
    auto& job = *static_cast<Job*>(lua_touserdata(L, 1));
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1); lua_pop(L, 1);
    lua_pushlightuserdata(L,const_cast<World*>(job.world));lua_pushcclosure(L,getComponent,1);lua_setglobal(L,"__get");
    lua_pushlightuserdata(L,const_cast<World*>(job.world));lua_pushcclosure(L,queryComponents,1);lua_setglobal(L,"__query");
    if (luaL_loadstring(L, prelude) != LUA_OK) return lua_error(L);
    lua_call(L, 0, 0);
    if (luaL_loadbufferx(L, job.module->source.data(), job.module->source.size(), "@behavior", "t") != LUA_OK) return lua_error(L);
    lua_call(L, 0, 1);
    luaL_checktype(L, -1, LUA_TTABLE);
    const int module = lua_absindex(L, -1);
    lua_getglobal(L, "__prepare"); push(L, job.context); lua_call(L, 1, 1);
    const int context = lua_absindex(L, -1);
    lua_getfield(L, module, job.callback.c_str());
    if (!lua_isnil(L, -1)) { luaL_checktype(L, -1, LUA_TFUNCTION); lua_pushvalue(L, context); push(L, job.event); lua_call(L, 2, 0); }
    else lua_pop(L, 1);
    lua_createtable(L, 0, 3);
    for (const char* field : {"state", "commands", "rng"}) { lua_getfield(L, context, field); lua_setfield(L, -2, field); }
    return 1;
}
bool safeName(const std::string& name) {
    static const std::regex pattern("^[A-Za-z0-9_-]+(/[A-Za-z0-9_-]+)*$");
    return name.size() <= 128 && std::regex_match(name, pattern);
}
bool within(const std::filesystem::path& root, const std::filesystem::path& file) {
    auto r = engine::assets::weaklyCanonicalFilePath(root), f = engine::assets::weaklyCanonicalFilePath(file);
    auto ri = r.begin(), fi = f.begin(); for (; ri != r.end() && fi != f.end(); ++ri, ++fi) if (*ri != *fi) return false;
    return ri == r.end() && fi != f.end();
}
}
void Runtime::initialize(const std::filesystem::path& assetRoot, std::uint64_t seed) {
    *this = Runtime{}; root_ = std::filesystem::weakly_canonical(engine::assets::nativeFilePath(assetRoot)); seed_ = seed;
    world.registry.load(readJson((root_ / "gameplay/components.json").u8string()));
}
Module Runtime::readModule(const std::string& name) const {
    check(safeName(name), "NG-E-SCRIPT-PATH", "Invalid script name");
    const auto sourcePath = engine::assets::nativeFilePath(root_ / "scripts" / std::filesystem::u8path(name + ".lua"));
    const auto descPath = engine::assets::nativeFilePath(root_ / "scripts" / std::filesystem::u8path(name + ".behavior.json"));
    check(within(root_ / "scripts", sourcePath) && within(root_ / "scripts", descPath), "NG-E-SCRIPT-PATH", "Script escaped asset root");
    std::ifstream file(sourcePath, std::ios::binary);
    check(file.good(), "NG-E-SCRIPT-IO", "Missing script " + name);
    check(std::filesystem::file_size(sourcePath) <= 256 * 1024, "NG-E-SCRIPT-IO", "Script too large");
    std::string source((std::istreambuf_iterator<char>(file)), {});
    check(source.size() <= 256 * 1024, "NG-E-SCRIPT-IO", "Script too large");
    auto descriptor = readJson(descPath.u8string());
    return {descriptor, source, fingerprint({descriptor, source})};
}
void Runtime::loadModule(const std::string& name, Json descriptor, std::string source) {
    check(!locked_ && safeName(name) && descriptor.at("apiVersion") == 1 && descriptor.at("version").get<unsigned>() > 0,
          "NG-E-SCRIPT-API", "Invalid module/version");
    Module m{std::move(descriptor), std::move(source), ""}; m.hash = fingerprint({m.descriptor, m.source});
    Behavior probe; probe.state = validate(m.descriptor.at("state"), Json::object());
    probe.properties = Json::object();
    execute(m, probe, "__validate", Json::object());
    modules_[name] = std::move(m);
}
void Runtime::resolveModule(const std::string& name,std::set<std::string>& resolving,std::set<std::string>& resolved) {
    if(resolved.count(name))return;
    check(resolving.size()<64&&modules_.size()<=512&&resolving.insert(name).second,"NG-E-SCRIPT-DEPENDENCY","Behavior dependency cycle/budget: "+name);
    if(!modules_.count(name)){auto m=readModule(name);loadModule(name,std::move(m.descriptor),std::move(m.source));}
    const auto dependencies=modules_.at(name).descriptor.value("dependencies",Json::array());
    check(dependencies.is_array()&&dependencies.size()<=128,"NG-E-SCRIPT-DEPENDENCY","Invalid behavior dependencies");
    for(const auto& dependency:dependencies){check(dependency.is_string()&&safeName(dependency.get<std::string>()),"NG-E-SCRIPT-DEPENDENCY","Invalid dependency name");resolveModule(dependency,resolving,resolved);}
    resolving.erase(name);resolved.insert(name);
}
void Runtime::attach(const std::string& entity, const std::string& id, const std::string& script, const Json& properties) {
    check(world.valid(world.find(entity)) && !id.empty() && id.size() <= 128, "NG-E-BEHAVIOR-ID", "Invalid behavior identity");
    for (const auto& b : behaviors_) check(b.entity != entity || b.id != id, "NG-E-BEHAVIOR-ID", "Duplicate behavior instance");
    std::set<std::string> resolving,resolved;resolveModule(script,resolving,resolved);
    const auto& m = modules_.at(script);
    Behavior b; b.entity = entity; b.id = id; b.script = script;
    b.properties = validate(m.descriptor.at("properties"), properties);
    b.state = validate(m.descriptor.at("state"), Json::object()); behaviors_.push_back(std::move(b));
    std::sort(behaviors_.begin(), behaviors_.end(), [](const auto& a, const auto& c) { return std::tie(a.entity,a.id) < std::tie(c.entity,c.id); });
}
Runtime::Invocation Runtime::execute(const Module& m, const Behavior& b, const std::string& callback, const Json& event) {
    Memory memory{0, limits.memoryBytes, limits.instructions};
    std::unique_ptr<lua_State, decltype(&lua_close)> state(lua_newstate(allocate, &memory), lua_close);
    check(state != nullptr, "NG-E-SCRIPT-MEMORY", "Cannot allocate Lua VM");
    auto* L = state.get(); lua_sethook(L, hook, LUA_MASKCOUNT, 100);
    Job job{&m, {{"entity",b.entity},{"properties",b.properties},{"state",b.state},{"rng",b.rng},
        {"rngSeed", determinism::deriveSeed(seed_, b.entity + "/" + b.id) & 0xffffffffull},
        {"tick",tick_},{"dt",1.0/120.0},{"input",input_},{"commandLimit",limits.commands}}, event, callback, &world};
    lua_pushcfunction(L, driver); lua_pushlightuserdata(L, &job);
    const int status = lua_pcall(L, 1, 1, 0);
    if (status != LUA_OK) {
        const char* message = lua_tostring(L, -1); std::string text = message ? message : "Lua failure";
        throw Error(status == LUA_ERRMEM ? "NG-E-SCRIPT-MEMORY" : text.find("NG-E-SCRIPT-BUDGET") != std::string::npos ? "NG-E-SCRIPT-BUDGET" : "NG-E-SCRIPT-RUNTIME", text);
    }
    const auto result = take(L, -1);
    return {result.at("state"), result.at("commands"), result.at("rng")};
}
void Runtime::note(const std::string& code, const std::string& message, const Behavior* b) {
    if (diagnostics_.size() >= 256) diagnostics_.erase(diagnostics_.begin());
    diagnostics_.push_back({{"tick",tick_},{"code",code},{"message",message.substr(0,4096)},
        {"entity",b ? b->entity : ""},{"behavior",b ? b->id : ""},{"script",b ? b->script : ""}});
}
void Runtime::commit(const Json& commands) {
    if (commands.empty()) return;
    check(commands.is_array() && commands.size() <= limits.commands, "NG-E-SCRIPT-BUDGET", "Command count");
    World candidate = world, shadow=world; auto nextStructural=structural_;
    // Validate against all earlier structural requests without exposing partial entities.
    auto applyStructure=[&](World& target,const Json& c) {
        if(c.at("op")=="destroy")target.destroy(target.find(c.at("entity")));
        else {auto h=target.create(c.at("entity"));for(auto it=c.at("components").begin();it!=c.at("components").end();++it)target.set(h,it.key(),it.value());}
    };
    for(const auto& c:structural_)applyStructure(shadow,c);
    auto nextModules=modules_;
    auto nextEvents = events_, nextTimers = timers_, nextPresentation = presentation_;
    auto sequence = sequence_;
    for (const auto& c : commands) {
        const std::string op = c.at("op");
        if (op == "set" || op=="remove") {
            check(c.at("component")!="behavior","NG-E-BEHAVIOR-LIFECYCLE","Behavior instances change through structural spawn/destroy");
            const auto id=c.at("entity").get<std::string>();
            check(candidate.valid(candidate.find(id)),"NG-E-COMMAND-ORDER","Configure new entities in the spawn payload");
            if(op=="set"){shadow.set(shadow.find(id),c.at("component"),c.at("value"));candidate.set(candidate.find(id),c.at("component"),c.at("value"));}
            else {shadow.remove(shadow.find(id),c.at("component"));candidate.remove(candidate.find(id),c.at("component"));}
        }
        else if (op == "spawn" || op=="destroy") {
            check(!tearingDown_,"NG-E-BEHAVIOR-LIFECYCLE","on_destroy cannot change entity structure");
            applyStructure(shadow,c);
            if(op=="spawn"&&c.at("components").contains("behavior")) {
                Runtime probe=*this;probe.world=shadow;probe.modules_=nextModules;
                for(const auto& b:c["components"]["behavior"]["instances"])probe.attach(c.at("entity"),b.at("id"),b.at("script"),b.value("properties",Json::object()));
                nextModules=std::move(probe.modules_);
            }
            nextStructural.push_back(c);
        }
        else if (op == "emit") {
            check(c.at("target").is_string() && c.at("target").get_ref<const std::string&>().size() <= 128 && c.at("name").is_string() && !c.at("name").get_ref<const std::string&>().empty() && c.at("name").get_ref<const std::string&>().size() <= 128 && c.at("payload").is_object() && c.at("payload").dump().size() <= 16384,
                  "NG-E-EVENT-SCHEMA", "Invalid event");
            nextEvents.push_back({{"due",tick_+1},{"seq",++sequence},{"target",c.at("target")},{"name",c.at("name")},{"payload",c.at("payload")}});
        } else if (op == "timer" || op == "cancel_timer") {
            check(c.at("id").is_string() && !c.at("id").get_ref<const std::string&>().empty() && c.at("id").get_ref<const std::string&>().size() <= 128, "NG-E-TIMER-ID", "Timer ID required");
            const auto owner = c.at("owner"); const auto behavior = c.at("behavior");
            nextTimers.erase(std::remove_if(nextTimers.begin(),nextTimers.end(),[&](const Json& t){return t["owner"]==owner && t["behavior"]==behavior && t["id"]==c["id"]; }),nextTimers.end());
            if (op == "timer") {
                check(c.at("name").is_string() && !c.at("name").get_ref<const std::string&>().empty() && c.at("name").get_ref<const std::string&>().size() <= 128 && c.at("payload").is_object() && c.at("payload").dump().size() <= 16384,"NG-E-EVENT-SCHEMA","Invalid timer event");
                check(c.at("delay").is_number_integer() && c.at("interval").is_number_integer() && c.at("delay").get<std::int64_t>() >= 1 && c.at("delay").get<std::int64_t>() <= 1000000000 && c.at("interval").get<std::int64_t>() >= 0 && c.at("interval").get<std::int64_t>() <= 1000000000,
                      "NG-E-TIMER-DELAY", "Timers use positive integer ticks");
                Json timer=c; timer["due"]=tick_+c["delay"].get<std::uint64_t>(); timer["seq"]=++sequence; nextTimers.push_back(std::move(timer));
            }
        } else if (op == "presentation") {
            check(c.at("name").is_string() && c.at("name").get_ref<const std::string&>().size() <= 128 && c.at("payload").is_object() && c.at("payload").dump().size() <= 16384,"NG-E-EVENT-SCHEMA","Invalid presentation event");
            nextPresentation.push_back({{"tick",tick_},{"name",c.at("name")},{"payload",c.at("payload")}});
        }
        else throw Error("NG-E-COMMAND-TYPE", "Unknown command " + op);
    }
    check(nextStructural.size() <= 1024 && nextEvents.size() <= limits.events && nextTimers.size() <= limits.timers && nextPresentation.size() <= 4096,
          "NG-E-SCRIPT-BUDGET", "Queue budget");
    structural_=std::move(nextStructural);modules_=std::move(nextModules);
    world = std::move(candidate); events_ = std::move(nextEvents); timers_ = std::move(nextTimers);
    presentation_ = std::move(nextPresentation); sequence_ = sequence;
}
void Runtime::invoke(std::size_t index, const std::string& callback, const Json& event) {
    auto& b = behaviors_.at(index);
    if (b.disabled || !world.valid(world.find(b.entity))) return;
    try {
        auto result = execute(modules_.at(b.script), b, callback, event);
        result.state = validate(modules_.at(b.script).descriptor.at("state"), result.state);
        result.rng=validate({{"type","object"},{"additionalProperties",{{"type","integer"},{"minimum",0},{"maximum",4294967295.0}}}},result.rng,"rng");
        for (auto& c : result.commands) if (c.at("op") == "timer" || c.at("op") == "cancel_timer") { c["owner"] = b.entity; c["behavior"] = b.id; }
        commit(result.commands); b.state = std::move(result.state); b.rng = std::move(result.rng);
    } catch (const Error& e) { b.disabled = true; note(e.code,e.what(),&b); }
      catch (const std::exception& e) { b.disabled = true; note("NG-E-SCRIPT-STATE",e.what(),&b); }
}
void Runtime::flushStructural() {
    auto pending=std::move(structural_);structural_=Json::array();
    for(const auto& command:pending) {
        const auto id=command.at("entity").get<std::string>();
        if(command.at("op")=="destroy") {
            tearingDown_=true;
            for(std::size_t i=0;i<behaviors_.size();++i)if(behaviors_[i].entity==id)invoke(i,"on_destroy");
            tearingDown_=false;
            if(world.valid(world.find(id)))world.destroy(world.find(id));
            behaviors_.erase(std::remove_if(behaviors_.begin(),behaviors_.end(),[&](const Behavior& b){return b.entity==id;}),behaviors_.end());
            timers_.erase(std::remove_if(timers_.begin(),timers_.end(),[&](const Json&t){return t["owner"]==id;}),timers_.end());
        } else {
            const auto h=world.create(id);
            for(auto it=command.at("components").begin();it!=command.at("components").end();++it)world.set(h,it.key(),it.value());
            if(const auto* behavior=world.get(h,"behavior"))for(const auto& b:behavior->at("instances"))attach(id,b.at("id"),b.at("script"),b.value("properties",Json::object()));
        }
    }
}
void Runtime::registerSystem(NativeSystem system) {
    check(!running_&&!locked_&&system.name.rfind("gameplay.",0)!=0,"NG-E-SYSTEM-REGISTRATION","System registration requires an unlocked boundary");
    for(const auto& existing:systems_)check(existing.name!=system.name,"NG-E-SYSTEM-DUPLICATE","Duplicate native system");
    systems_.push_back(std::move(system));
}
void Runtime::start() {
    for (std::size_t i=0;i<behaviors_.size();++i) if (!behaviors_[i].started) { behaviors_[i].started=true; invoke(i,"on_start"); }
    if(!running_)flushStructural();
}
void Runtime::tick(const Json& input) {
    check(!running_, "NG-E-TICK-REENTRY", "Recursive tick"); running_=true;
    try {
        ++tick_; input_=input;
        Scheduler scheduler;
        scheduler.add({"gameplay.start",Phase::Input,{},[&]{start();}});
        scheduler.add({"gameplay.events",Phase::Events,{"gameplay.start"},[&]{
        for (auto h : world.query("hud")) { auto hud=*world.get(h,"hud"); if(hud["remaining"].get<unsigned>()>0) {hud["remaining"]=hud["remaining"].get<unsigned>()-1;world.set(h,"hud",hud);} }
        Json ready=Json::array(), pending=Json::array();
        for(const auto& e:events_) (e["due"].get<std::uint64_t>()<=tick_ ? ready : pending).push_back(e);
        events_=std::move(pending);
        Json timers=Json::array();
        for(auto t:timers_) {
            if(!world.valid(world.find(t["owner"])))continue;
            if(t["due"].get<std::uint64_t>()<=tick_) {
                ready.push_back({{"due",t["due"]},{"seq",t["seq"]},{"target",t["owner"]},{"behavior",t["behavior"]},{"name",t["name"]},{"payload",t["payload"]}});
                if(t["interval"].get<unsigned>()>0){t["due"]=tick_+t["interval"].get<unsigned>();timers.push_back(t);}
            }else timers.push_back(t);
        }
        timers_=std::move(timers);
        std::sort(ready.begin(),ready.end(),[](const Json&a,const Json&b){return std::make_pair(a["due"].get<std::uint64_t>(),a["seq"].get<std::uint64_t>()) < std::make_pair(b["due"].get<std::uint64_t>(),b["seq"].get<std::uint64_t>());});
        for(const auto& e:ready) for(std::size_t i=0;i<behaviors_.size();++i) {
            const auto& b=behaviors_[i];
            if((e["target"]==""||e["target"]==b.entity) && (!e.contains("behavior")||e["behavior"]==b.id)) {
                Json payload=e["payload"]; payload["name"]=e["name"];
                invoke(i, e["name"]=="trigger.enter"?"on_trigger_enter":e["name"]=="trigger.exit"?"on_trigger_exit":"on_event",payload);
            }
        }
        }});
        scheduler.add({"gameplay.behaviors",Phase::Behaviors,{"gameplay.events"},[&]{
        for(std::size_t i=0;i<behaviors_.size();++i)
            if(modules_.at(behaviors_[i].script).descriptor.value("update",false)) invoke(i,"on_update");
        }});
        scheduler.add({"gameplay.commit",Phase::Commit,{"gameplay.behaviors"},[&]{flushStructural();}});
        for(const auto& system:systems_)scheduler.add({system.name,system.phase,system.after,[&,callback=system.run]{if(callback)callback(*this);}});
        scheduler.run();
        running_=false;
    }catch(...){running_=false;throw;}
}
void Runtime::emit(const std::string& target,const std::string& name,const Json& payload) {
    commit(Json::array({{{"op","emit"},{"target",target},{"name",name},{"payload",payload}}}));
}
void Runtime::interact(const std::string& target,const std::string& actor) {
    for(std::size_t i=0;i<behaviors_.size();++i)if(behaviors_[i].entity==target)invoke(i,"on_interact",{{"actor",actor}});
    if(!running_)flushStructural();
}
void Runtime::collectTriggers(const std::string& actor) {
    const auto actorHandle=world.find(actor);if(!world.valid(actorHandle))return;
    const auto* at=world.get(actorHandle,"transform");if(!at)return;
    const Json actorPos=at->at("position");
    for(auto h:world.query("trigger")) {
        const auto* transform=world.get(h,"transform");if(!transform)continue;
        auto trigger=*world.get(h,"trigger");
        bool inside=trigger["enabled"].get<bool>();
        for(int axis=0;axis<3;++axis)inside=inside&&std::abs(actorPos[axis].get<double>()-(*transform)["position"][axis].get<double>())<=trigger["half"][axis].get<double>();
        const std::string id=world.entity(h).id,key=Json::array({id,actor}).dump();
        bool was=overlaps_.value(key,false);overlaps_[key]=inside;
        if(inside&&!was&&(!trigger["once"].get<bool>()||!trigger["fired"].get<bool>())){emit(id,"trigger.enter",{{"actor",actor}});trigger["fired"]=true;world.set(h,"trigger",trigger);}
        if(!inside&&was)emit(id,"trigger.exit",{{"actor",actor}});
    }
}
void Runtime::destroy(const std::string& entity) {
    commit(Json::array({{{"op","destroy"},{"entity",entity}}}));
    if(!running_)flushStructural();
}
Json Runtime::manifest() const {
    Json modules=Json::object();for(const auto&[name,m]:modules_)modules[name]={{"version",m.descriptor.at("version")},{"hash",m.hash}};
    Json systems=Json::array();for(const auto& system:systems_)systems.push_back({{"name",system.name},{"phase",static_cast<unsigned>(system.phase)},{"after",system.after}});
    return {{"systems",systems},{"api",1},{"lua","5.4.9"},{"components",fingerprint(world.registry.catalog())},{"modules",modules}};
}
Json Runtime::snapshot() const {
    Json behaviors=Json::array();for(const auto& b:behaviors_)behaviors.push_back({{"entity",b.entity},{"id",b.id},{"script",b.script},{"properties",b.properties},{"state",b.state},{"rng",b.rng},{"started",b.started},{"disabled",b.disabled}});
    return {{"format","nativegpu.gameplay/1"},{"manifest",manifest()},{"world",world.snapshot()},{"behaviors",behaviors},
        {"structural",structural_},{"seed",seed_},{"tick",tick_},{"sequence",sequence_},{"events",events_},{"timers",timers_},{"overlaps",overlaps_},{"input",input_}};
}
void Runtime::restore(const Json& s) {
    check(!running_&&s.at("format")=="nativegpu.gameplay/1"&&s.at("manifest")==manifest(),"NG-E-SNAPSHOT-CONTENT","Snapshot dependencies changed");
    Runtime candidate=*this;candidate.world.restore(s.at("world"));candidate.behaviors_.clear();
    check(s.at("behaviors").size()<=10000&&s.at("events").size()<=limits.events&&s.at("timers").size()<=limits.timers,"NG-E-SNAPSHOT-STATE","Snapshot budget exceeded");
    std::set<std::pair<std::string,std::string>> identities;
    for(const auto& item:s.at("behaviors")) {
        Behavior b;b.entity=item.at("entity");b.id=item.at("id");b.script=item.at("script");
        check(candidate.world.valid(candidate.world.find(b.entity))&&!b.id.empty()&&b.id.size()<=128&&identities.emplace(b.entity,b.id).second&&modules_.count(b.script),"NG-E-SNAPSHOT-STATE","Invalid behavior identity");
        const auto& m=modules_.at(b.script);
        b.properties=validate(m.descriptor.at("properties"),item.at("properties"));b.state=validate(m.descriptor.at("state"),item.at("state"));
        b.rng=validate({{"type","object"},{"additionalProperties",{{"type","integer"},{"minimum",0},{"maximum",4294967295.0}}}},item.at("rng"),"rng");b.started=item.at("started");b.disabled=item.at("disabled");candidate.behaviors_.push_back(std::move(b));
    }
    candidate.seed_=s.at("seed");candidate.tick_=s.at("tick");candidate.sequence_=s.at("sequence");
    candidate.structural_=s.at("structural");candidate.events_=s.at("events");candidate.timers_=s.at("timers");candidate.overlaps_=s.at("overlaps");candidate.input_=s.at("input");
    check(candidate.structural_.is_array()&&candidate.structural_.size()<=1024&&candidate.events_.is_array()&&candidate.timers_.is_array()&&candidate.input_.is_object(),"NG-E-SNAPSHOT-STATE","Invalid queues/input");
    check(candidate.overlaps_.is_object()&&candidate.overlaps_.size()<=100000,"NG-E-SNAPSHOT-STATE","Invalid overlap set");
    for(const auto& overlap:candidate.overlaps_)check(overlap.is_boolean(),"NG-E-SNAPSHOT-STATE","Invalid overlap flag");
    auto text=[](const Json& v,bool empty=false){return v.is_string()&&(empty||!v.get_ref<const std::string&>().empty())&&v.get_ref<const std::string&>().size()<=128;};
    auto nonnegative=[](const Json& v){return v.is_number_unsigned()||(v.is_number_integer()&&v.get<std::int64_t>()>=0);};
    check(nonnegative(s.at("seed"))&&nonnegative(s.at("tick"))&&nonnegative(s.at("sequence")),"NG-E-SNAPSHOT-STATE","Invalid counters");
    std::set<std::uint64_t> sequences;
    auto event=[&](const Json& e){
        check(text(e.at("name"))&&e.at("payload").is_object()&&e.at("payload").dump().size()<=16384&&nonnegative(e.at("due"))&&nonnegative(e.at("seq")),"NG-E-SNAPSHOT-STATE","Invalid event");
        auto sequence=e.at("seq").get<std::uint64_t>();
        check(sequence>0&&sequence<=candidate.sequence_&&sequences.insert(sequence).second,"NG-E-SNAPSHOT-STATE","Invalid event sequence");
    };
    for(const auto& e:candidate.events_){event(e);check(text(e.at("target"),true),"NG-E-SNAPSHOT-STATE","Invalid event target");}
    std::set<std::tuple<std::string,std::string,std::string>> timers;
    for(const auto& t:candidate.timers_){
        event(t);check(text(t.at("id"))&&text(t.at("owner"))&&text(t.at("behavior"))&&nonnegative(t.at("interval"))&&t.at("interval").get<std::uint64_t>()<=1000000000,"NG-E-SNAPSHOT-STATE","Invalid timer");
        const std::string owner=t.at("owner"),behavior=t.at("behavior"),id=t.at("id");
        check(identities.count({owner,behavior})&&timers.emplace(owner,behavior,id).second,"NG-E-SNAPSHOT-STATE","Invalid timer owner");
    }
    World shadow=candidate.world;
    for(const auto& c:candidate.structural_){
        if(c.at("op")=="destroy")shadow.destroy(shadow.find(c.at("entity")));
        else {
            check(c.at("op")=="spawn"&&c.at("components").is_object(),"NG-E-SNAPSHOT-STATE","Invalid structural command");
            const auto h=shadow.create(c.at("entity"));
            for(auto it=c.at("components").begin();it!=c.at("components").end();++it)shadow.set(h,it.key(),it.value());
            if(const auto* b=shadow.get(h,"behavior"))for(const auto& instance:b->at("instances")) {
                const std::string module=instance.at("script");check(modules_.count(module),"NG-E-SNAPSHOT-STATE","Missing structural behavior module");
                validate(modules_.at(module).descriptor.at("properties"),instance.value("properties",Json::object()));
            }
        }
    }
    candidate.presentation_=Json::array();*this=std::move(candidate);
}
void Runtime::save(const std::string& path) const {atomicSave(path,snapshot());}
void Runtime::load(const std::string& path){restore(verifiedLoad(path));}
std::string Runtime::digest() const{return fingerprint(snapshot());}
Json Runtime::takePresentation(){auto out=std::move(presentation_);presentation_=Json::array();return out;}
Json Runtime::inspect() const{return {{"tick",tick_},{"manifest",manifest()},{"entities",world.view()},{"behaviors",snapshot()["behaviors"]},{"diagnostics",diagnostics_}};}
bool Runtime::reload(const std::map<std::string,Module>& replacements) {
    if(locked_){note("NG-E-SCRIPT-LOCKED","Replay content is locked");return false;}
    check(!running_,"NG-E-RELOAD-TICK","Reload requires a tick boundary");
    try {
        Runtime candidate=*this;
        for(const auto&[name,m]:replacements)candidate.loadModule(name,m.descriptor,m.source);
        for(const auto&[name,m]:replacements){(void)m;std::set<std::string> resolving,resolved;candidate.resolveModule(name,resolving,resolved);}
        for(auto& b:candidate.behaviors_)if(replacements.count(b.script)) {
            const auto oldVersion=modules_.at(b.script).descriptor.at("version");
            const auto& module=candidate.modules_.at(b.script);
            b.properties=validate(module.descriptor.at("properties"),b.properties);
            if(oldVersion!=module.descriptor.at("version")) {
                auto result=candidate.execute(module,b,"migrate",{{"oldVersion",oldVersion}});
                check(result.commands.empty(),"NG-E-RELOAD-MIGRATION","Migration cannot write world");b.state=result.state;b.rng=result.rng;
            }
            b.state=validate(module.descriptor.at("state"),b.state);b.disabled=false;
        }
        for(std::size_t i=0;i<candidate.behaviors_.size();++i)if(replacements.count(candidate.behaviors_[i].script)) {
            auto& b=candidate.behaviors_[i];auto result=candidate.execute(candidate.modules_.at(b.script),b,"on_reload",Json::object());
            check(result.commands.empty(),"NG-E-RELOAD-MIGRATION","on_reload cannot write world");b.state=validate(candidate.modules_.at(b.script).descriptor.at("state"),result.state);
            b.rng=validate({{"type","object"},{"additionalProperties",{{"type","integer"},{"minimum",0},{"maximum",4294967295.0}}}},result.rng,"rng");
        }
        *this=std::move(candidate);return true;
    }catch(const Error&e){note(e.code,e.what());return false;}catch(const std::exception&e){note("NG-E-RELOAD-MIGRATION",e.what());return false;}
}
bool Runtime::pollReload(){
    if(locked_||modules_.empty())return false;
    try{std::map<std::string,Module> changed;for(const auto&[name,m]:modules_){auto next=readModule(name);if(next.hash!=m.hash)changed[name]=std::move(next);}return !changed.empty()&&reload(changed);}
    catch(const Error&e){note(e.code,e.what());return false;}catch(const std::exception&e){note("NG-E-SCRIPT-IO",e.what());return false;}
}
}
